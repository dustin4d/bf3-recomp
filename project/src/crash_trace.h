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

#include <rex/logging.h>
#include <rex/logging/api.h>
#include <spdlog/details/null_mutex.h>
#include <spdlog/sinks/base_sink.h>

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
