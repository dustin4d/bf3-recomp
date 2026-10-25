// Diagnostics setup (see crash_trace.h): null-fault/FATAL stack traces always, thread dumps on request.

#include "crash_trace.h"

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
    StartWatch(static_cast<uint32_t>(std::stoul(w, nullptr, 16)), at ? std::atoi(at) : 6);
  }
#endif
}

}  // namespace bf3
