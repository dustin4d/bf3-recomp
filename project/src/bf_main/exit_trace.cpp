// Log who makes the game exit. sub_838B5860 is BF.Main's CRT doexit(code, quick, retcaller): it runs the
// atexit table and the __xp/__xt terminators, then KeBugCheck(0) unless retcaller, which the runtime logs only
// as "*** STOP: 0x00000000". Before running the original, log the arguments and the guest call chain.

// Compiled into the bf3_BF_Main DLL (see CMakeLists.txt): bf3_pch.h is generated/bf_main/bf3_pch.h.
#include "bf3_pch.h"

REX_EXTERN(__imp__sub_838B5860);

namespace {

// Xbox 360 frames: back chain at 0(r1); a non-leaf function saves LR at -8 from its caller's r1.
void LogGuestCallChain(PPCContext& ctx, uint8_t* base, const char* what) {
  REXLOG_WARN("{}: lr {:08X} r3 {:08X} r4 {:08X} r5 {:08X}", what, ctx.lr, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32);
  uint32_t sp = ctx.r1.u32;
  for (int depth = 0; depth < 24; ++depth) {
    uint32_t caller_sp = REX_LOAD_U32(sp);
    if (caller_sp <= sp || caller_sp - sp > 0x100000) break;
    REXLOG_WARN("  #{:02} return {:08X} (frame {:08X})", depth, REX_LOAD_U32(caller_sp - 8), caller_sp);
    sp = caller_sp;
  }
}

}  // namespace

extern "C" REX_FUNC(sub_838B5860) {
  LogGuestCallChain(ctx, base, "doexit");
  __imp__sub_838B5860(ctx, base);
}
