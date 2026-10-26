// Diagnostics setup (see crash_trace.h): null-fault/FATAL stack traces always, thread dumps on request.

#include "crash_trace.h"

#include <string_view>

#include <rex/cvar.h>

#include <cstdlib>
#include <string>

REXCVAR_DEFINE_INT32(thread_dump_at, 0, "Debug", "Dump all thread stacks this many seconds after start (0 = off)");
REXCVAR_DEFINE_INT32(thread_dump_every, 0, "Debug", "Repeat the thread dump every N seconds (0 = once)");

namespace bf3 {

void InstallDiagnostics() {
  InstallCrashTrace();
#if defined(_WIN32)
  StartThreadDumpTimer(REXCVAR_GET(thread_dump_at), REXCVAR_GET(thread_dump_every));
  if (const char* w = std::getenv("BF3_WATCH")) {
    const char* at = std::getenv("BF3_WATCH_AT");
    const char* any = std::getenv("BF3_WATCH_ANY");
    g_watch_any = any && *any == '1';
    if (std::string_view(w) == "aim") {
      // BF3_WATCH=aim: the local player's aiming yaw (+0xC of the state bf3_BF_Main.dll's aim_trace.cpp exports).
      static int at_s = at ? std::atoi(at) : 6;
      CreateThread(
          nullptr, 0,
          [](LPVOID) -> DWORD {
            const ULONGLONG start = GetTickCount64();
            using Fn = uint32_t (*)();
            Fn fn = nullptr;
            uint32_t state = 0;
            while (!state || GetTickCount64() - start < ULONGLONG(at_s) * 1000) {
              Sleep(250);
              if (!fn) {
                if (HMODULE m = GetModuleHandleW(L"bf3_BF_Main.dll")) {
                  fn = reinterpret_cast<Fn>(GetProcAddress(m, "bf3_aim_state"));
                }
              }
              if (fn) state = fn();
            }
            REXLOG_ERROR("[crash_trace] WATCH aim state {:08X}", state);
            StartWatch(state + 0xC, 0);
            return 0;
          },
          nullptr, 0, nullptr);
    } else {
      StartWatch(static_cast<uint32_t>(std::stoul(w, nullptr, 16)), at ? std::atoi(at) : 6);
    }
  }
#endif
}

}  // namespace bf3
