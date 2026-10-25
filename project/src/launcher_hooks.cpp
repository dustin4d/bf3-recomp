// Hooks in the launcher (Default.xex, BF.Launcher_Xenon_Retail.exe).
//
// sub_820112A0(state, content) loads one game module: it formats "GAME:\BF.%s%s.xex" with "Splash" while the
// byte at state+8 is 0 and with "Main" afterwards, XexLoadImage()s it, calls export ordinal 1 with the state,
// XexUnloadImage()s it and sets state+8 = 1. On a failed load it relaunches default.xex (sub_82017D78).
//
// BF.Splash (0x83480000-0x83E10000) overlaps BF.Main, so the two are never resident together, and ReXGlue's
// codegen can't recompile both (it maps every module into one address space). Splash is therefore not
// recompiled: mark the Splash pass as done so the first call loads BF.Main directly.

#include "generated/default/bf3_pch.h"

#include <rex/cvar.h>

REXCVAR_DEFINE_BOOL(skip_splash, true, "BF3", "Skip BF.Splash (not recompiled) and load BF.Main first");

REX_EXTERN(__imp__sub_820112A0);

extern "C" REX_FUNC(sub_820112A0) {
  uint32_t state = ctx.r3.u32;
  if (REXCVAR_GET(skip_splash) && REX_LOAD_U8(state + 8) == 0) {
    REXLOG_INFO("launcher: skipping BF.Splash, loading BF.Main (state {:08X})", state);
    REX_STORE_U8(state + 8, 1);
  }
  __imp__sub_820112A0(ctx, base);
}
