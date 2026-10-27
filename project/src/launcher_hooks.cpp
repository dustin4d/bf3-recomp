// Hooks in the launcher (Default.xex, BF.Launcher_Xenon_Retail.exe).
//
// sub_820112A0(state, content) loads one game module: it formats "GAME:\BF.%s%s.xex" with "Splash" while the
// byte at state+8 is 0 and with "Main" afterwards, XexLoadImage()s it, calls export ordinal 1 with the state,
// XexUnloadImage()s it and sets state+8 = 1. On a failed load it relaunches default.xex (sub_82017D78).
//
// BF.Splash (0x83480000-0x83E10000) overlaps BF.Main, so the two are never resident together, and ReXGlue's
// codegen can't recompile both (it maps every module into one address space). Splash is therefore not
// recompiled: mark the Splash pass as done so the first call loads BF.Main directly.
//
// Splash also picks the storage device (launcher vtable 0x82000638 slot 8, sub_820107D8: state+0x88 = device,
// state+0xE0 bit 0x80 = device valid). BF.Main's init (sub_82FE8DC8) then calls slot 16 (sub_820126B8) with device
// -1 until it returns true: that scans the stored device for marketplace content (sub_82012148: XContent type 2,
// mounted as DLC%08X, package.mft "Flag hdcontent"), and sub_8302F928 later mounts each registered package into the
// game's file system. Without a device the scan is skipped and the HD Content pack is never loaded, so the HDD
// (device 1) is set for the scan. It is cleared again once the scan is done: the front end (sub_836F9B08) takes a
// device from the launcher as "Arrive from launcher?" and then runs the console's post-Splash flow (device
// selection, TV calibration) instead of booting straight to the main menu.

#include "generated/default/bf3_pch.h"

#include <rex/cvar.h>

REXCVAR_DEFINE_BOOL(skip_splash, true, "BF3", "Skip BF.Splash (not recompiled) and load BF.Main first");
REXCVAR_DEFINE_BOOL(hd_content, true, "BF3",
                    "Scan the HDD for the HD Content pack (marketplace content 4541095000000000) like BF.Splash would");
REXCVAR_DEFINE_BOOL(skip_title, true, "BF3",
                    "Boot straight to the main menu: no PRESS START title screen or CHECKING DATA popup (bf_main/boot_flow.cpp)");

namespace {
// Launcher state whose device was set for the content scan, and its device fields from before.
uint32_t g_scan_state = 0;
uint32_t g_saved_device = 0;
uint8_t g_saved_flags = 0;
}  // namespace

REX_EXTERN(__imp__sub_820112A0);
REX_EXTERN(__imp__sub_820126B8);

extern "C" REX_FUNC(sub_820112A0) {
  uint32_t state = ctx.r3.u32;
  if (REXCVAR_GET(skip_splash) && REX_LOAD_U8(state + 8) == 0) {
    REXLOG_INFO("launcher: skipping BF.Splash, loading BF.Main (state {:08X})", state);
    REX_STORE_U8(state + 8, 1);
    if (REXCVAR_GET(hd_content) && (REX_LOAD_U8(state + 0xE0) & 0xC0) == 0) {
      g_scan_state = state;
      g_saved_device = REX_LOAD_U32(state + 0x88);
      g_saved_flags = REX_LOAD_U8(state + 0xE0);
      REX_STORE_U32(state + 0x88, 1);  // what sub_820107D8(state, 1) does
      REX_STORE_U8(state + 0xE0, g_saved_flags | 0x80);
      REXLOG_INFO("launcher: storage device = HDD for the content scan (HD pack)");
    }
  }
  __imp__sub_820112A0(ctx, base);
}

// sub_820126B8(state, device, rescan): starts or polls the content scan; true once it is done.
extern "C" REX_FUNC(sub_820126B8) {
  const uint32_t state = ctx.r3.u32;
  __imp__sub_820126B8(ctx, base);
  if (state == g_scan_state && (ctx.r3.u32 & 0xFF) != 0) {
    g_scan_state = 0;
    REX_STORE_U32(state + 0x88, g_saved_device);
    REX_STORE_U8(state + 0xE0, g_saved_flags);
    REXLOG_INFO("launcher: content scan done, storage device cleared");
  }
}
