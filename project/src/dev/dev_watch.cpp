// Debug API write watch (development builds only): a hardware data breakpoint (DR0) on a guest address, armed at runtime
// in every thread, that records who writes there. Each hit keeps the writing thread, the value after the write and the
// host stack (<module>+0x<RVA>; scripts/bf3dbg.py names the frames from the linker maps: recompiled frames are
// sub_<guest address>). Used to find which guest code owns a field (e.g. what keeps rewriting a position).
// Heap addresses (0x40000000-0x9FFFFFFF) only: the physical views' aliases are not watched.

#include "dev_watch.h"

#if defined(_WIN32)
#include <windows.h>
#include <tlhelp32.h>

#include <rex/logging.h>

#include <atomic>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "../crash_trace.h"

namespace bf3::dev {
namespace {

std::mutex g_mutex;  // arming and the records
std::atomic<uint64_t> g_host{0};  // watched host address (0 = off)
uint32_t g_guest = 0, g_len = 4;
std::atomic<int> g_max_hits{8};
std::atomic<int> g_hits{0};
std::vector<WatchHit> g_records;
std::atomic<uint64_t> g_until{0};  // GetTickCount64 when the watch disarms itself
std::atomic<uint64_t> g_generation{0};
bool g_handler_added = false;

DWORD64 Dr7For(uint32_t len) {
  const DWORD64 len_bits = len == 1 ? 0 : len == 2 ? 1 : len == 8 ? 2 : 3;
  return 1ull | (1ull << 16) | (len_bits << 18);  // L0, RW0 = write, LEN0
}

LONG CALLBACK Handler(EXCEPTION_POINTERS* info) {
  if (info->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP) return EXCEPTION_CONTINUE_SEARCH;
  const uint64_t host = g_host.load();
  CONTEXT* c = info->ContextRecord;
  if (!host || !(c->Dr6 & 1) || c->Dr0 != host) return EXCEPTION_CONTINUE_SEARCH;
  c->Dr6 = 0;
  const int n = g_hits.fetch_add(1);
  if (n >= g_max_hits.load()) {
    c->Dr7 &= ~0xFull;  // this thread has reported enough; the arming loop disarms the rest
    return EXCEPTION_CONTINUE_EXECUTION;
  }
  WatchHit hit;
  hit.tid = GetCurrentThreadId();
  uint8_t bytes[8] = {};
  std::memcpy(bytes, reinterpret_cast<const void*>(host), g_len);
  for (uint32_t i = 0; i < g_len; ++i) hit.value_hex += fmt::format("{:02x}", bytes[i]);
  ThreadStack st;
  WalkStack(*c, st);
  for (int f = 0; f < st.count && hit.frames.size() < 24; ++f) {
    const std::string name = AnyFrameName(st.pcs[f]);
    if (name.rfind("bf3", 0) == 0) hit.frames.push_back(name);  // our modules only (exe + recompiled DLLs)
  }
  std::lock_guard lock(g_mutex);
  g_records.push_back(std::move(hit));
  return EXCEPTION_CONTINUE_EXECUTION;
}

// Sets (or clears, dr7 = 0) DR0 in every thread but the caller. Returns the threads changed.
int ArmAll(uint64_t host, DWORD64 dr7) {
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
        const bool already = dr7 ? (ctx.Dr0 == host && (ctx.Dr7 & 0xF0003ull) == (dr7 & 0xF0003ull))
                                 : !(ctx.Dr7 & 3ull);
        if (!already) {
          ctx.Dr0 = host;
          ctx.Dr7 = (ctx.Dr7 & ~0xF0003ull) | dr7;
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

}  // namespace

bool StartWatch(uint32_t guest_addr, uint32_t len, int max_hits, uint32_t seconds, std::string& error) {
  if (len != 1 && len != 2 && len != 4 && len != 8) return error = "length must be 1, 2, 4 or 8", false;
  if (guest_addr % len) return error = "address must be aligned to the length", false;
  if (guest_addr < 0x40000000u || guest_addr >= 0xA0000000u) return error = "heap addresses only (40000000-9FFFFFFF)", false;
  StopWatch();
  std::lock_guard lock(g_mutex);
  if (!g_handler_added) {
    AddVectoredExceptionHandler(1, Handler);
    g_handler_added = true;
  }
  g_records.clear();
  g_hits = 0;
  g_max_hits = max_hits;
  g_guest = guest_addr;
  g_len = len;
  const uint64_t host = kGuestBase + guest_addr;
  g_host = host;
  g_until = GetTickCount64() + uint64_t(seconds) * 1000;
  const uint64_t gen = ++g_generation;
  const int armed = ArmAll(host, Dr7For(len));
  REXLOG_INFO("debug api: watch {:08X} ({} bytes) armed in {} threads for {} s", guest_addr, len, armed, seconds);
  // Threads keep being created: re-arm until the time is up or enough hits came in, then disarm everywhere.
  std::thread([gen, host, len] {
    while (g_generation.load() == gen && GetTickCount64() < g_until.load() && g_hits.load() < g_max_hits.load()) {
      ArmAll(host, Dr7For(len));
      Sleep(100);
    }
    if (g_generation.load() == gen) {
      ArmAll(host, 0);
      REXLOG_INFO("debug api: watch {:08X} done, {} writes", g_guest, g_hits.load());
    }
  }).detach();
  return true;
}

void StopWatch() {
  if (!g_host.load()) return;
  ++g_generation;
  ArmAll(g_host.load(), 0);
  g_host = 0;
}

WatchState GetWatch() {
  std::lock_guard lock(g_mutex);
  WatchState s;
  s.guest = g_guest;
  s.len = g_len;
  s.active = g_host.load() && GetTickCount64() < g_until.load() && g_hits.load() < g_max_hits.load();
  s.hits = g_hits.load();
  s.records = g_records;
  return s;
}

}  // namespace bf3::dev

#else
namespace bf3::dev {
bool StartWatch(uint32_t, uint32_t, int, uint32_t, std::string& error) { return error = "Windows only", false; }
void StopWatch() {}
WatchState GetWatch() { return {}; }
}  // namespace bf3::dev
#endif
