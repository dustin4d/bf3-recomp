// Debug API hardware breakpoints (development builds only): the four x64 debug registers (DR0-DR3), armed at runtime in
// every thread, shared between data watches and function traces.
// - Watch: who writes (or reads and writes) a guest heap address. Each hit keeps the thread, the bytes after the access
//   and the host stack (<module>+0x<RVA>; scripts/bf3dbg.py names the frames from the linker maps: recompiled frames
//   are sub_<guest address>).
// - Trace: an execution breakpoint on a recompiled function's host entry (sub_X(PPCContext& ctx, uint8_t* base); the
//   dispatch table gives the address, see bf_main/dev_tools.cpp bf3_dev_host_func). Each call keeps the guest
//   registers at entry (r3-r10, f1-f4, lr = the guest caller) and the host stack; the return address on the host stack
//   is swapped for ReturnThunk, which records r3/f1 on return and goes back to the caller. Recompiled functions are
//   noinline weak aliases, so every call reaches the entry. Recursion nests per thread.
// A background thread re-arms the registers every 100 ms (threads keep being created) and clears them when a slot runs
// out of time or hits. Heap addresses only for watches (the physical views' aliases are not watched).

#include "dev_breakpoints.h"

#if defined(_WIN32)
#include <windows.h>
#include <tlhelp32.h>

#include <rex/logging.h>
#include <rex/ppc/context.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "../crash_trace.h"

extern "C" void bf3_trace_return_thunk();
extern "C" uint64_t bf3_trace_on_return();

namespace bf3::dev {
namespace {

struct Slot {
  bool used = false;
  BpKind kind = BpKind::kWrite;
  uint32_t guest = 0, len = 0;
  uint64_t host = 0;
  int max_hits = 0;
  uint64_t until_ms = 0;  // GetTickCount64
  uint64_t generation = 0;
  LARGE_INTEGER t0{};
  std::vector<BpHit> records;
};

std::mutex g_mutex;  // g_slots (records included)
std::array<Slot, 4> g_slots;
std::array<std::atomic<int>, 4> g_hits{};
std::array<std::atomic<uint64_t>, 4> g_host{};   // armed host address per slot (0 = off), read by the handler
std::array<std::atomic<bool>, 4> g_trace{};      // slot is an execution breakpoint
std::atomic<uint64_t> g_generation{0};
std::atomic<bool> g_arming{false};
bool g_handler_added = false;
LARGE_INTEGER g_freq{};

double SecondsSince(const LARGE_INTEGER& t0) {
  LARGE_INTEGER now;
  QueryPerformanceCounter(&now);
  return double(now.QuadPart - t0.QuadPart) / double(g_freq.QuadPart);
}

// DR7 bits for slot i: L<i>, R/W<i> (00 execute, 01 write, 11 read/write), LEN<i> (00 1, 01 2, 11 4, 10 8 bytes).
DWORD64 Dr7Bits(int i, BpKind kind, uint32_t len) {
  const DWORD64 rw = kind == BpKind::kTrace ? 0 : kind == BpKind::kWrite ? 1 : 3;
  const DWORD64 ln = kind == BpKind::kTrace ? 0 : len == 1 ? 0 : len == 2 ? 1 : len == 8 ? 2 : 3;
  return (1ull << (2 * i)) | (rw << (16 + 4 * i)) | (ln << (18 + 4 * i));
}
constexpr DWORD64 kOurDr7Mask = 0xFFull | (0xFFFFull << 16);

bool Active(const Slot& s, int i) {
  return s.used && GetTickCount64() < s.until_ms && g_hits[i].load() < s.max_hits;
}

// Host stack of the trapping thread: our modules' frames only, stopping at a swapped return address.
void WalkFrames(CONTEXT c, std::vector<std::string>& out, size_t max) {
  ULONG_PTR lo = 0, hi = 0;
  GetCurrentThreadStackLimits(&lo, &hi);
  for (int n = 0; n < 48 && c.Rip && out.size() < max; ++n) {
    if (c.Rip == reinterpret_cast<DWORD64>(&bf3_trace_return_thunk)) break;
    std::string name = AnyFrameName(c.Rip);
    if (name.rfind("bf3", 0) == 0) out.push_back(std::move(name));
    DWORD64 image_base = 0;
    auto* fn = RtlLookupFunctionEntry(c.Rip, &image_base, nullptr);
    if (!fn) {
      if (c.Rsp < lo || c.Rsp + 8 > hi) break;
      c.Rip = *reinterpret_cast<DWORD64*>(c.Rsp);
      c.Rsp += 8;
      continue;
    }
    void* handler_data = nullptr;
    DWORD64 establisher = 0;
    RtlVirtualUnwind(UNW_FLAG_NHANDLER, image_base, c.Rip, fn, &c, &handler_data, &establisher, nullptr);
    if (c.Rsp < lo || c.Rsp >= hi) break;
  }
}

struct PendingReturn {
  uint64_t ret;
  PPCContext* ctx;
  int slot;
  size_t index;
  uint64_t generation;
  LARGE_INTEGER t0;
};
thread_local std::vector<PendingReturn> t_returns;
thread_local bool t_in_handler = false;

LONG CALLBACK Handler(EXCEPTION_POINTERS* info) {
  if (info->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP) return EXCEPTION_CONTINUE_SEARCH;
  CONTEXT* c = info->ContextRecord;
  if (t_in_handler) {  // a trap inside the handler (it holds g_mutex): consume it, record nothing
    if ((c->Dr6 & 0xF) == 0) return EXCEPTION_CONTINUE_SEARCH;
    for (int i = 0; i < 4; ++i) {
      if ((c->Dr6 >> i) & 1 && ((c->Dr7 >> (16 + 4 * i)) & 3) == 0) c->EFlags |= 0x10000;
    }
    c->Dr6 = 0;
    return EXCEPTION_CONTINUE_EXECUTION;
  }
  t_in_handler = true;
  struct Leave {
    ~Leave() { t_in_handler = false; }
  } leave;
  const DWORD64 dr6 = c->Dr6;
  const DWORD64 drs[4] = {c->Dr0, c->Dr1, c->Dr2, c->Dr3};
  bool handled = false;
  for (int i = 0; i < 4; ++i) {
    // A trap from one of our registers must always be consumed, even if its slot was just cleared (the arming loop
    // reaches this thread later): an unhandled single step ends the process (exit code 0x80000004).
    if (!(dr6 & (1ull << i)) || !(c->Dr7 & (1ull << (2 * i)))) continue;
    if (i == 0 && g_watch_host) continue;  // crash_trace.h's BF3_WATCH owns DR0
    handled = true;
    const bool execute = ((c->Dr7 >> (16 + 4 * i)) & 3) == 0;
    if (execute) c->EFlags |= 0x10000;  // RF: execute the instruction this time
    const uint64_t host = g_host[i].load();
    if (!host || drs[i] != host) {
      c->Dr7 &= ~(3ull << (2 * i));  // a stale breakpoint in this thread
      continue;
    }
    const bool trace = g_trace[i].load();
    const int n = g_hits[i].fetch_add(1);
    std::unique_lock lock(g_mutex);
    Slot& s = g_slots[i];
    if (n >= s.max_hits || !s.used) {
      c->Dr7 &= ~(3ull << (2 * i));  // enough from this thread; the arming loop clears the rest
      continue;
    }
    BpHit hit;
    hit.tid = GetCurrentThreadId();
    hit.t = SecondsSince(s.t0);
    WalkFrames(*c, hit.frames, trace ? 10 : 16);
    if (trace) {
      auto* ctx = reinterpret_cast<PPCContext*>(c->Rcx);  // sub_X(PPCContext& ctx, uint8_t* base)
      hit.lr = uint32_t(ctx->lr);
      const PPCRegister* r[8] = {&ctx->r3, &ctx->r4, &ctx->r5, &ctx->r6, &ctx->r7, &ctx->r8, &ctx->r9, &ctx->r10};
      for (int k = 0; k < 8; ++k) hit.r[k] = r[k]->u64;
      const PPCRegister* f[4] = {&ctx->f1, &ctx->f2, &ctx->f3, &ctx->f4};
      for (int k = 0; k < 4; ++k) hit.f[k] = f[k]->f64;
      const size_t index = s.records.size();
      s.records.push_back(std::move(hit));
      auto* ret = reinterpret_cast<uint64_t*>(c->Rsp);  // at the entry: the return address
      PendingReturn p{*ret, ctx, i, index, s.generation, {}};
      QueryPerformanceCounter(&p.t0);
      t_returns.push_back(p);
      *ret = reinterpret_cast<uint64_t>(&bf3_trace_return_thunk);
    } else {
      // Read through the kernel: a plain read would trap again on a read/write watch.
      uint8_t bytes[8] = {};
      SIZE_T got = 0;
      ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(host), bytes, s.len, &got);
      for (uint32_t k = 0; k < s.len; ++k) hit.value_hex += fmt::format("{:02x}", bytes[k]);
      s.records.push_back(std::move(hit));
    }
  }
  if (!handled) return EXCEPTION_CONTINUE_SEARCH;
  c->Dr6 = 0;
  return EXCEPTION_CONTINUE_EXECUTION;
}

// Sets the debug registers of every thread but the caller to the active slots. Returns the threads changed.
int ArmAll() {
  DWORD64 addr[4] = {}, dr7 = 0;
  {
    std::lock_guard lock(g_mutex);
    for (int i = 0; i < 4; ++i) {
      const Slot& s = g_slots[i];
      const bool on = Active(s, i);
      g_host[i] = on ? s.host : 0;
      if (!on) continue;
      addr[i] = s.host;
      dr7 |= Dr7Bits(i, s.kind, s.len);
    }
  }
  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
  if (snap == INVALID_HANDLE_VALUE) return 0;
  THREADENTRY32 te{sizeof(te)};
  int changed = 0;
  const DWORD self = GetCurrentThreadId(), pid = GetCurrentProcessId();
  for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
    if (te.th32OwnerProcessID != pid || te.th32ThreadID == self) continue;
    HANDLE th = OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_SUSPEND_RESUME, FALSE, te.th32ThreadID);
    if (!th) continue;
    if (SuspendThread(th) != DWORD(-1)) {
      CONTEXT ctx{};
      ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
      if (GetThreadContext(th, &ctx)) {
        const DWORD64 want7 = (ctx.Dr7 & ~kOurDr7Mask) | dr7;
        if (ctx.Dr7 != want7 || ctx.Dr0 != addr[0] || ctx.Dr1 != addr[1] || ctx.Dr2 != addr[2] || ctx.Dr3 != addr[3]) {
          ctx.Dr0 = addr[0], ctx.Dr1 = addr[1], ctx.Dr2 = addr[2], ctx.Dr3 = addr[3];
          ctx.Dr7 = want7;
          if (SetThreadContext(th, &ctx)) ++changed;
        }
      }
      ResumeThread(th);
    }
    CloseHandle(th);
  }
  CloseHandle(snap);
  return changed;
}

void StartArming() {
  if (g_arming.exchange(true)) return;
  std::thread([] {
    for (;;) {
      ArmAll();
      bool any = false;
      {
        std::lock_guard lock(g_mutex);
        for (int i = 0; i < 4; ++i) any |= Active(g_slots[i], i);
      }
      if (!any) {
        ArmAll();  // cleared everywhere
        g_arming = false;
        return;
      }
      Sleep(100);
    }
  }).detach();
}

// A free slot: unused, or the oldest inactive one. -1 if all four are active.
int TakeSlot() {
  int pick = -1;
  for (int i = 0; i < 4; ++i) {
    if (!g_slots[i].used) return i;
    if (!Active(g_slots[i], i) && (pick < 0 || g_slots[i].generation < g_slots[pick].generation)) pick = i;
  }
  return pick;
}

int Add(BpKind kind, uint32_t guest, uint64_t host, uint32_t len, int max_hits, uint32_t seconds, std::string& error) {
  std::lock_guard lock(g_mutex);
  if (!g_handler_added) {
    QueryPerformanceFrequency(&g_freq);
    AddVectoredExceptionHandler(1, Handler);
    g_handler_added = true;
  }
  // The same address and kind again replaces that breakpoint (its old records go).
  int i = -1;
  for (int k = 0; k < 4; ++k) {
    const Slot& o = g_slots[k];
    if (o.used && o.guest == guest && (o.kind == BpKind::kTrace) == (kind == BpKind::kTrace)) i = k;
  }
  if (i < 0) i = TakeSlot();
  if (i < 0) {
    error = "all four debug registers are in use (watch off / trace off first)";
    return -1;
  }
  Slot& s = g_slots[i];
  s = Slot{};
  s.used = true;
  s.kind = kind;
  s.guest = guest;
  s.host = host;
  s.len = len;
  s.max_hits = max_hits;
  s.until_ms = GetTickCount64() + uint64_t(seconds) * 1000;
  s.generation = ++g_generation;
  QueryPerformanceCounter(&s.t0);
  g_hits[i] = 0;
  g_trace[i] = kind == BpKind::kTrace;
  REXLOG_INFO("debug api: DR{} {} {:08X} for {} s / {} hits", i,
              kind == BpKind::kTrace ? "trace" : kind == BpKind::kWrite ? "write watch" : "read/write watch", guest,
              seconds, max_hits);
  return i;
}

}  // namespace

int AddWatch(uint32_t guest_addr, uint32_t len, bool reads, int max_hits, uint32_t seconds, std::string& error) {
  if (len != 1 && len != 2 && len != 4 && len != 8) return error = "length must be 1, 2, 4 or 8", -1;
  if (guest_addr % len) return error = "address must be aligned to the length", -1;
  if (guest_addr < 0x40000000u || guest_addr >= 0xA0000000u) return error = "heap addresses only (40000000-9FFFFFFF)", -1;
  const int slot = Add(reads ? BpKind::kReadWrite : BpKind::kWrite, guest_addr, kGuestBase + guest_addr, len, max_hits,
                       seconds, error);
  if (slot >= 0) StartArming();
  return slot;
}

int AddTrace(uint32_t guest_addr, void* host_func, int max_hits, uint32_t seconds, std::string& error) {
  if (!host_func) return error = "no recompiled function at that address", -1;
  const int slot = Add(BpKind::kTrace, guest_addr, reinterpret_cast<uint64_t>(host_func), 1, max_hits, seconds, error);
  if (slot >= 0) StartArming();
  return slot;
}

void RemoveBreakpoint(int slot) {
  {
    std::lock_guard lock(g_mutex);
    for (int i = 0; i < 4; ++i) {
      if (slot < 0 || slot == i) g_slots[i] = Slot{};
    }
  }
  ArmAll();
}

std::vector<BpState> Breakpoints() {
  std::lock_guard lock(g_mutex);
  std::vector<BpState> out;
  for (int i = 0; i < 4; ++i) {
    const Slot& s = g_slots[i];
    if (!s.used) continue;
    BpState b;
    b.slot = i;
    b.kind = s.kind;
    b.guest = s.guest;
    b.len = s.len;
    b.active = Active(s, i);
    b.hits = std::min(g_hits[i].load(), s.max_hits);  // a trap or two can land before every thread is disarmed
    b.max_hits = s.max_hits;
    b.records = s.records;
    out.push_back(std::move(b));
  }
  return out;
}

}  // namespace bf3::dev

// Return side of a trace: the traced function's return address was swapped for this thunk. Entered by its `ret` (the
// stack is then 16-byte aligned), it records the return values, then jumps to the real return address.
extern "C" uint64_t bf3_trace_on_return() {
  using namespace bf3::dev;
  auto& stack = t_returns;
  const PendingReturn p = stack.back();
  stack.pop_back();
  std::lock_guard lock(g_mutex);
  Slot& s = g_slots[p.slot];
  if (s.used && s.generation == p.generation && p.index < s.records.size()) {
    BpHit& hit = s.records[p.index];
    hit.returned = true;
    hit.ret_r3 = p.ctx->r3.u64;
    hit.ret_f1 = p.ctx->f1.f64;
    hit.duration_us = SecondsSince(p.t0) * 1e6;
  }
  return p.ret;
}

extern "C" __attribute__((naked)) void bf3_trace_return_thunk() {
  __asm__ volatile(
      "subq $32, %rsp\n\t"  // shadow space; rsp stays 16-byte aligned for the call
      "callq bf3_trace_on_return\n\t"
      "addq $32, %rsp\n\t"
      "jmpq *%rax\n\t");
}

#else
namespace bf3::dev {
int AddWatch(uint32_t, uint32_t, bool, int, uint32_t, std::string& error) { return error = "Windows only", -1; }
int AddTrace(uint32_t, void*, int, uint32_t, std::string& error) { return error = "Windows only", -1; }
void RemoveBreakpoint(int) {}
std::vector<BpState> Breakpoints() { return {}; }
}  // namespace bf3::dev
#endif
