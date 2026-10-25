// Diagnostic: log the host call stack when guest code faults on the null page or calls an unregistered function.
//
// Recompiled guest functions are real host functions (sub_XXXXXXXX), so a host stack walk names the guest call
// chain. Guest code lives in bf3.exe (launcher) and the bf3_*.dll modules, so frames are logged as
// <module>+0x<RVA>; scripts/win_run.sh resolves them with scripts/resolve_rva.py and the linker maps.
// - Null faults: only accesses inside the first 64KB of guest memory are logged; the runtime may use access
//   violations on purpose (write watches), and those must stay silent.
// - Unregistered calls: the runtime logs "[FATAL] Call to invalid or unregistered function" and exits; a log
//   sink catches that message and walks the stack of the thread that logged it.
#pragma once

#if defined(_WIN32)
#include <windows.h>
#include <tlhelp32.h>

#include <rex/logging.h>
#include <rex/logging/api.h>
#include <spdlog/details/null_mutex.h>
#include <spdlog/sinks/base_sink.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <string>

namespace bf3 {

inline constexpr uintptr_t kGuestBase = 0x100000000ull;
inline constexpr uintptr_t kNullWindow = 0x10000;

// "bf3_BF_Main.dll+0x1234" for our modules, "" for system/runtime frames.
inline std::string FrameName(DWORD64 pc) {
  HMODULE mod = nullptr;
  if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                          reinterpret_cast<LPCWSTR>(pc), &mod)) {
    return {};
  }
  char path[MAX_PATH];
  DWORD n = GetModuleFileNameA(mod, path, MAX_PATH);
  std::string name(path, n);
  name = name.substr(name.find_last_of("\\/") + 1);
  if (name != "bf3.exe" && name.rfind("bf3_", 0) != 0) return {};
  return fmt::format("{}+0x{:X}", name, pc - reinterpret_cast<uintptr_t>(mod));
}

inline void LogStack(CONTEXT ctx) {
  for (int frame = 0; frame < 64 && ctx.Rip; ++frame) {
    std::string name = FrameName(ctx.Rip);
    if (!name.empty()) REXLOG_ERROR("[crash_trace]   #{:02} {}", frame, name);
    DWORD64 image_base = 0;
    auto* fn = RtlLookupFunctionEntry(ctx.Rip, &image_base, nullptr);
    if (!fn) {  // leaf function: return address is at [rsp]
      ctx.Rip = *reinterpret_cast<DWORD64*>(ctx.Rsp);
      ctx.Rsp += 8;
      continue;
    }
    void* handler_data = nullptr;
    DWORD64 establisher = 0;
    RtlVirtualUnwind(UNW_FLAG_NHANDLER, image_base, ctx.Rip, fn, &ctx, &handler_data, &establisher, nullptr);
  }
}

inline LONG CALLBACK GuestNullFaultHandler(EXCEPTION_POINTERS* info) {
  auto* rec = info->ExceptionRecord;
  if (rec->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || rec->NumberParameters < 2) {
    return EXCEPTION_CONTINUE_SEARCH;
  }
  uintptr_t fault = rec->ExceptionInformation[1];
  if (fault < kGuestBase || fault >= kGuestBase + kNullWindow) return EXCEPTION_CONTINUE_SEARCH;
  REXLOG_ERROR("[crash_trace] guest null fault: {} guest 0x{:08X} at {}",
               rec->ExceptionInformation[0] ? "write" : "read", fault - kGuestBase,
               FrameName(reinterpret_cast<DWORD64>(rec->ExceptionAddress)));
  LogStack(*info->ContextRecord);
  return EXCEPTION_CONTINUE_SEARCH;
}

// null_mutex: the stack walk logs through the same loggers, so a locking sink would deadlock on itself.
class FatalTraceSink : public spdlog::sinks::base_sink<spdlog::details::null_mutex> {
 protected:
  void sink_it_(const spdlog::details::log_msg& msg) override {
    std::string_view text(msg.payload.data(), msg.payload.size());
    if (msg.level < spdlog::level::critical || text.find("[FATAL]") == std::string_view::npos) return;
    if (tracing_) return;  // LogStack logs through the same loggers
    tracing_ = true;
    REXLOG_ERROR("[crash_trace] host stack at FATAL:");
    CONTEXT ctx;
    RtlCaptureContext(&ctx);
    LogStack(ctx);
    tracing_ = false;
  }
  void flush_() override {}

 private:
  static inline thread_local bool tracing_ = false;
};

// Thread dump: suspend each other thread in turn, walk its host stack into a fixed buffer, resume it, and only then
// log (a suspended thread may hold the logger or heap lock). Frames are <module>+0x<RVA> for every module; the
// resolver names bf3 modules via linker maps and rexruntime.dll via its exports. Guest thread names come from the
// runtime's "XThread::Execute ... native=<tid>" lines.
struct ThreadStack {
  DWORD tid = 0;
  int count = 0;
  DWORD64 pcs[48] = {};
};

inline void WalkStack(CONTEXT ctx, ThreadStack& out) {
  out.count = 0;
  while (out.count < 48 && ctx.Rip) {
    out.pcs[out.count++] = ctx.Rip;
    DWORD64 image_base = 0;
    auto* fn = RtlLookupFunctionEntry(ctx.Rip, &image_base, nullptr);
    if (!fn) {
      ctx.Rip = *reinterpret_cast<DWORD64*>(ctx.Rsp);
      ctx.Rsp += 8;
      continue;
    }
    void* handler_data = nullptr;
    DWORD64 establisher = 0;
    RtlVirtualUnwind(UNW_FLAG_NHANDLER, image_base, ctx.Rip, fn, &ctx, &handler_data, &establisher, nullptr);
  }
}

inline std::string AnyFrameName(DWORD64 pc) {
  HMODULE mod = nullptr;
  if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                          reinterpret_cast<LPCWSTR>(pc), &mod)) {
    return fmt::format("0x{:X}", pc);
  }
  char path[MAX_PATH];
  DWORD n = GetModuleFileNameA(mod, path, MAX_PATH);
  std::string name(path, n);
  name = name.substr(name.find_last_of("\\/") + 1);
  return fmt::format("{}+0x{:X}", name, pc - reinterpret_cast<uintptr_t>(mod));
}

inline void DumpAllThreads(const char* why) {
  static ThreadStack stacks[256];
  int n = 0;
  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
  if (snap == INVALID_HANDLE_VALUE) return;
  THREADENTRY32 te{sizeof(te)};
  for (BOOL ok = Thread32First(snap, &te); ok && n < 256; ok = Thread32Next(snap, &te)) {
    if (te.th32OwnerProcessID != GetCurrentProcessId() || te.th32ThreadID == GetCurrentThreadId()) continue;
    HANDLE th = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE,
                           te.th32ThreadID);
    if (!th) continue;
    if (SuspendThread(th) != static_cast<DWORD>(-1)) {
      CONTEXT ctx{};
      ctx.ContextFlags = CONTEXT_FULL;
      if (GetThreadContext(th, &ctx)) {
        stacks[n].tid = te.th32ThreadID;
        WalkStack(ctx, stacks[n]);
        ++n;
      }
      ResumeThread(th);
    }
    CloseHandle(th);
  }
  CloseHandle(snap);
  REXLOG_ERROR("[crash_trace] thread dump ({}): {} threads", why, n);
  for (int i = 0; i < n; ++i) {
    REXLOG_ERROR("[crash_trace] thread {:08X}", stacks[i].tid);
    for (int f = 0; f < stacks[i].count; ++f) {
      REXLOG_ERROR("[crash_trace]   #{:02} {}", f, AnyFrameName(stacks[i].pcs[f]));
    }
  }
  rex::FlushLogging();
}

// --thread_dump_at=<s>: dump all threads <s> seconds after start, then every --thread_dump_every=<s> (0 = once).
inline void StartThreadDumpTimer(int at_seconds, int every_seconds) {
  if (at_seconds <= 0) return;
  CreateThread(
      nullptr, 0,
      [](LPVOID p) -> DWORD {
        auto packed = reinterpret_cast<uintptr_t>(p);
        DWORD at = static_cast<DWORD>(packed >> 16), every = static_cast<DWORD>(packed & 0xFFFF);
        Sleep(at * 1000);
        for (int i = 1;; ++i) {
          DumpAllThreads(fmt::format("t+{}s", at + (i - 1) * every).c_str());
          if (!every) break;
          Sleep(every * 1000);
        }
        return 0;
      },
      reinterpret_cast<LPVOID>((static_cast<uintptr_t>(at_seconds) << 16) | (every_seconds & 0xFFFF)), 0, nullptr);
}

struct ThreadStack;
inline void WalkStack(CONTEXT ctx, ThreadStack& out);
inline std::string AnyFrameName(DWORD64 pc);

// Write watch on one guest 64-bit word holding {tag, pointer} big-endian (a tagged lock-free list head): a hardware
// data breakpoint (DR0, 8 bytes, write) in every thread that exists when it is armed. Tags only grow on legitimate
// updates, so a write that lowers the tag is logged with the writer's host stack. Diagnostic only (traps on every
// write to the word). Env: BF3_WATCH=<guest hex address>, BF3_WATCH_AT=<seconds after start>.
inline uint64_t g_watch_host = 0;
inline uint64_t g_watch_alias[3] = {};  // DR1-DR3: other views of the same physical memory
inline std::atomic<uint32_t> g_watch_max_tag{0};
inline std::atomic<uint32_t> g_watch_hits{0};
inline std::atomic<int> g_watch_reports{0};
struct WatchRecord {
  uint64_t seq, value;
  DWORD64 rip;
  DWORD tid;
};
inline WatchRecord g_watch_ring[256];
inline std::atomic<uint64_t> g_watch_seq{0};

inline LONG CALLBACK WatchHandler(EXCEPTION_POINTERS* info) {
  if (info->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP || !g_watch_host) {
    return EXCEPTION_CONTINUE_SEARCH;
  }
  const DWORD64 dr6 = info->ContextRecord->Dr6;
  if (!(dr6 & 0xF)) return EXCEPTION_CONTINUE_SEARCH;
  info->ContextRecord->Dr6 = 0;
  if (dr6 & 0xE) {  // a write through an alias view: always interesting
    if (g_watch_reports.fetch_add(1) < 6) {
      REXLOG_ERROR("[crash_trace] WATCH: write through alias DR{} at {}", (dr6 & 2) ? 1 : (dr6 & 4) ? 2 : 3,
                   AnyFrameName(info->ContextRecord->Rip));
      ThreadStack st;
      WalkStack(*info->ContextRecord, st);
      for (int f = 0; f < st.count && f < 14; ++f) {
        REXLOG_ERROR("[crash_trace]   #{:02} {}", f, AnyFrameName(st.pcs[f]));
      }
      rex::FlushLogging();
    }
    return EXCEPTION_CONTINUE_EXECUTION;
  }
  g_watch_hits.fetch_add(1, std::memory_order_relaxed);
  uint64_t raw = *reinterpret_cast<volatile uint64_t*>(g_watch_host);
  uint64_t value = _byteswap_uint64(raw);
  {
    uint64_t s = g_watch_seq.fetch_add(1);
    g_watch_ring[s & 255] = {s, value, info->ContextRecord->Rip, GetCurrentThreadId()};
  }
  uint32_t tag = static_cast<uint32_t>(value >> 32);
  uint32_t prev = g_watch_max_tag.load(std::memory_order_relaxed);
  while (tag > prev && !g_watch_max_tag.compare_exchange_weak(prev, tag)) {
  }
  if (tag + 0x10 < prev && g_watch_reports.fetch_add(1) < 6) {  // went back by more than a few in-flight updates
    REXLOG_ERROR("[crash_trace] WATCH: tag went back {:08X} -> {:08X} (ptr {:08X}) at {}", prev, tag,
                 static_cast<uint32_t>(value), FrameName(info->ContextRecord->Rip));
    if (g_watch_reports.load() == 1) {  // first report: the trapped writes leading up to it
      uint64_t end = g_watch_seq.load();
      for (uint64_t s = end > 32 ? end - 32 : 0; s < end; ++s) {
        const WatchRecord& r = g_watch_ring[s & 255];
        if (r.seq != s) continue;
        REXLOG_ERROR("[crash_trace] WATCH   #{} t{:5} {:016X} by {}", r.seq, r.tid, r.value, FrameName(r.rip));
      }
    }
    LogStack(*info->ContextRecord);
    rex::FlushLogging();
  }
  return EXCEPTION_CONTINUE_EXECUTION;
}

// Arms threads not armed yet (call repeatedly: debug registers are per thread, and threads keep being created).
inline int ArmWatchAllThreads() {
  static DWORD armed_tids[1024];
  static int armed_count = 0;
  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
  if (snap == INVALID_HANDLE_VALUE) return 0;
  THREADENTRY32 te{sizeof(te)};
  int armed = 0;
  for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
    if (te.th32OwnerProcessID != GetCurrentProcessId() || te.th32ThreadID == GetCurrentThreadId()) continue;
    bool known = false;
    for (int i = 0; i < armed_count && !known; ++i) known = armed_tids[i] == te.th32ThreadID;
    HANDLE th = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_SET_CONTEXT, FALSE, te.th32ThreadID);
    if (!th) continue;
    if (SuspendThread(th) != static_cast<DWORD>(-1)) {
      CONTEXT ctx{};
      ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
      if (GetThreadContext(th, &ctx)) {
        // Re-arm every pass, and report threads whose registers were reset since (SetThreadContext elsewhere).
        if (known && ctx.Dr0 != g_watch_host) {
          REXLOG_ERROR("[crash_trace] WATCH: thread {:X} lost its debug registers (Dr0 {:X} Dr7 {:X})",
                       te.th32ThreadID, ctx.Dr0, ctx.Dr7);
        }
        ctx.Dr0 = g_watch_host;
        ctx.Dr1 = g_watch_alias[0];
        ctx.Dr2 = g_watch_alias[1];
        ctx.Dr3 = g_watch_alias[2];
        // Ln enable, RWn = 01 (write), LENn = 10 (8 bytes) for DR0..DR3
        ctx.Dr7 = 0x55ull | (0x9999ull << 16);
        if (SetThreadContext(th, &ctx) && !known) {
          ++armed;
          if (armed_count < 1024) armed_tids[armed_count++] = te.th32ThreadID;
        }
      }
      ResumeThread(th);
    }
    CloseHandle(th);
  }
  CloseHandle(snap);
  return armed;
}

// Log the process's mapped (section-backed) regions >= 1 MB, merged by allocation base: candidate views of guest
// memory. A write through a view nobody watches is invisible to the debug registers.
inline void LogMappedViews() {
  MEMORY_BASIC_INFORMATION mbi;
  uintptr_t addr = 0, alloc_base = 0, alloc_size = 0;
  DWORD alloc_type = 0;
  auto flush = [&] {
    if (alloc_size >= (1u << 20) && alloc_type == MEM_MAPPED) {
      REXLOG_ERROR("[crash_trace] VIEW {:X} size {:X}", alloc_base, alloc_size);
    }
  };
  while (VirtualQuery(reinterpret_cast<void*>(addr), &mbi, sizeof(mbi)) == sizeof(mbi)) {
    uintptr_t base = reinterpret_cast<uintptr_t>(mbi.AllocationBase);
    if (mbi.State != MEM_FREE) {
      if (base != alloc_base) {
        flush();
        alloc_base = base;
        alloc_size = 0;
        alloc_type = mbi.Type;
      }
      alloc_size += mbi.RegionSize;
    }
    addr = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
    if (addr < reinterpret_cast<uintptr_t>(mbi.BaseAddress)) break;
  }
  flush();
}

inline void StartWatch(uint32_t guest_addr, int at_seconds) {
  if (!guest_addr) return;
  // Same translation as the generated code: base 0x100000000, +0x1000 for 0xE0000000 and up.
  g_watch_host = 0x100000000ull + guest_addr + (guest_addr >= 0xE0000000u ? 0x1000u : 0u);
  if (guest_addr >= 0xA0000000u) {  // physical alias views (A/C/E ranges, and the raw physical view)
    // Console mapping: 0xA0000000 and 0xC0000000 map physical 0; 0xE0000000 maps physical 0x1000.
    const uint32_t phys = (guest_addr & 0x1FFFFFFFu) + (guest_addr >= 0xE0000000u ? 0x1000u : 0u);
    int n = 0;
    for (uint32_t v : {0xA0000000u, 0xC0000000u}) {
      if ((guest_addr & 0xE0000000u) != v) g_watch_alias[n++] = 0x100000000ull + v + phys;
    }
    if (guest_addr < 0xE0000000u) g_watch_alias[n++] = 0x100000000ull + 0xE0000000u + phys;  // E view, host +0x1000
    g_watch_alias[n] = 0x200000000ull + phys;
    REXLOG_ERROR("[crash_trace] WATCH aliases {:X} {:X} {:X}", g_watch_alias[0], g_watch_alias[1], g_watch_alias[2]);
  }
  AddVectoredExceptionHandler(1, WatchHandler);
  CreateThread(
      nullptr, 0,
      [](LPVOID p) -> DWORD {
        Sleep(static_cast<DWORD>(reinterpret_cast<uintptr_t>(p)) * 1000);
        LogMappedViews();
        REXLOG_ERROR("[crash_trace] WATCH armed on host {:X} in {} threads", g_watch_host, ArmWatchAllThreads());
        for (int i = 1; i <= 240; ++i) {
          Sleep(250);
          if (int n = ArmWatchAllThreads()) REXLOG_ERROR("[crash_trace] WATCH armed {} new thread(s)", n);
          if (i % 20 == 0) {
            REXLOG_ERROR("[crash_trace] WATCH: {} writes, max tag {:08X}", g_watch_hits.load(), g_watch_max_tag.load());
          }
        }
        return 0;
      },
      reinterpret_cast<LPVOID>(static_cast<uintptr_t>(at_seconds)), 0, nullptr);
}

inline void InstallCrashTrace() {
  AddVectoredExceptionHandler(1, GuestNullFaultHandler);
  rex::AddSink(std::make_shared<FatalTraceSink>());
}

}  // namespace bf3

#else
namespace bf3 {
inline void InstallCrashTrace() {}
}  // namespace bf3
#endif
