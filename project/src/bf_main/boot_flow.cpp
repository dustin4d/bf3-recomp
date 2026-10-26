// Boot straight to the main menu (cvar skip_title, default on): no PRESS START title screen, no CHECKING DATA popup.
//
// Front-end flow (ui/flow/graph/frontend/frontendroot), from the UI graph transitions (BF3_FLOW_TRACE=1):
//   ... NeedToPressStart -> HasPressedStart -> ShowExitToMenuReason -> AllowIdleMovie -> StartScreen
// StartScreen's own graph takes START after a ~5 s StartScreenTimer (InputListener -> SetActiveController ->
// PressStart); the profile then loads behind the "CHECKING DATA" popup and the screen outputs StartPressed:
//   StartScreen -> SetHasPressedStart -> IsDisc2Installed -> Platform check -> Has calibrated? ->
//   First save + calibration done -> ShowExitToMenuReason -> ShowNewLicenses -> RequestDynamicPopup -> EnterMainMenu
//   -> ... -> Main menu
// What this does instead:
// - HasPressedStart (the branch the game takes when it comes back to the front end) reads flow data key 0x880850C5
//   (bool, read through sub_83294100, hooked in ui_mouse.cpp); answered true, the root skips StartScreen.
// - StartScreen's SetActiveController action is sub_83749090(UI manager): it takes the controller with the latest
//   input, makes it the input manager's active controller and switches the active user (manager+0x124, vtable +4 =
//   sub_83C87DB8); with that user signed in, sub_83C862C0 broadcasts the change (message 0x005973B9 / 0xE3A3CDFC)
//   and the front end's handler sub_8374EE10 starts the profile load (sub_8374C4C0: SAVEDATA_PROFILE, PROF_SAVE).
//   It runs when the root reaches SetHasPressedStart, so the profile (game options) still loads as after START.
// - "Has calibrated?" (flow data 0x47A90E9B) is read from the profile, which is still loading then: answered true
//   for that one read (on the console a fresh profile gets the TV brightness calibration screen).
// - The load job (RealMemCard worker, sub_83DF1898) posts the CHECKING DATA popup through sub_83E6EE58(result,
//   self, request, &shown_ms), then sleeps until shown_ms + 3000 (the console's minimum display time) before it
//   reports done. For the boot load that post is not made: no popup, shown_ms stays -1, no sleep. The job still
//   closes the popup when it's done (MemoryCardPopupHide, 0x4DF6DB30 / 0xEC2DDCE4): that marks the load done.
// - "First save + calibration done" then saves the profile (sub_832268B8(options manager, r4, r5): serializes the
//   options, sends MemoryCardSave 0x4DF6DB30 / 0xD191BB6F and keeps the request id at manager +0x914 until it is
//   done). On the skipped boot that comes before the load is through; the memory card turns it away and the id stays
//   pending, which blocks every later save. So that save is held back and made once the load is done.
//
// UI graph transitions: sub_832917E0(graph r3, transition r4, r5) logs "[Transition]: [%s] -> [%s]" with the names at
// *(from node +8) / *(to node +8) (transition +8 = from, +0xC = to) and enters the target node through
// sub_832914F8(graph, to, transition +0x10, +0x14, r5).

// Compiled into the bf3_BF_Main DLL (see CMakeLists.txt): bf3_pch.h is generated/bf_main/bf3_pch.h.
#include "bf3_pch.h"

#include <windows.h>

#include <rex/cvar.h>

#include <cstdlib>
#include <cstring>
#include <string>

REX_EXTERN(__imp__sub_832917E0);
REX_EXTERN(__imp__sub_8374EE10);
REX_EXTERN(__imp__sub_83E6EE58);
REX_EXTERN(__imp__sub_832268B8);
REX_EXTERN(sub_83749090);  // UI manager's SetActiveController action (manager r3)
REX_EXTERN(sub_83013BA0);  // allocator (allocator r3, size r4, align r5)

namespace {

constexpr uint32_t kHasPressedStartKey = 0x880850C5;
constexpr uint32_t kFrontendListenerOffset = 0xC;  // sub_8374EE10's this = UI manager + 0xC
constexpr uint32_t kLoadJobBegin = 0x83DF1898, kLoadJobEnd = 0x83DF1E00;  // sub_83DF1898
constexpr uint32_t kHandleVtable = 0x82F3BD04;                           // sub_83E6EE58's result handle
constexpr uint32_t kEmptyHandleOwner = 0x841527F8;                       // *(this) + 4: the shared empty handle
constexpr uint32_t kMemCardCategory = 0x4DF6DB30, kMemCardPopupHide = 0xEC2DDCE4;

bool g_skipped = false;           // HasPressedStart answered true at boot
uint32_t g_frontend = 0;          // sub_8374EE10's this
bool g_profile_load = false;      // SetActiveController run for the boot
bool g_calibration_read = false;  // the next flow data read is "Has calibrated?"
bool g_hide_popup = false;        // the boot load's CHECKING DATA popup is still to come
bool g_load_done = false;         // the boot load closed its popup
bool g_save_held = false;         // the first profile save, held until the load is done
uint32_t g_save_r3 = 0, g_save_r4 = 0, g_save_r5 = 0;

bool Enabled() {
  static const bool on = rex::cvar::GetFlagByName("skip_title") != "false";
  return on;
}

bool FlowTrace() {
  static const bool on = [] {
    const char* env = std::getenv("BF3_FLOW_TRACE");
    return env && *env == '1';
  }();
  return on;
}

bool Plausible(uint32_t a) {
  return (a >= 0x40000000 && a < 0x7FFF0000) || (a >= 0x82000000 && a < 0x84300000);
}

std::string NodeName(uint8_t* base, uint32_t node) {
  if (!Plausible(node)) return "?";
  const uint32_t name = REX_LOAD_U32(node + 8);
  if (!Plausible(name)) return "?";
  const char* s = reinterpret_cast<const char*>(REX_RAW_ADDR(name));
  return std::string(s, strnlen(s, 128));
}

void SetBool(uint8_t* base, uint32_t value) {
  REX_STORE_U32(value + 16, 2);
  REX_STORE_U32(value + 8, 0x01000000);  // bool true (first byte)
}

void StartProfileLoad(PPCContext& ctx, uint8_t* base) {
  if (g_profile_load || !g_frontend) return;
  g_profile_load = true;
  g_hide_popup = true;
  PPCContext keep = ctx;
  ctx.r3.u64 = g_frontend - kFrontendListenerOffset;
  sub_83749090(ctx, base);
  ctx = keep;
  REXLOG_INFO("boot flow: active controller set, loading the profile");
}

// A guest heap copy of `bytes` bytes at guest address src (the held save's strings: r4 a char string, r5 a UTF-16
// string in the caller's stack frame, gone by the time the save is made).
uint32_t GuestCopy(PPCContext& ctx, uint8_t* base, uint32_t src, uint32_t bytes) {
  PPCContext keep = ctx;
  ctx.r3.u64 = 0x84042270;  // the allocator the UI uses for small objects
  ctx.r4.u64 = bytes;
  ctx.r5.u64 = 16;
  sub_83013BA0(ctx, base);
  const uint32_t copy = ctx.r3.u32;
  ctx = keep;
  if (copy) std::memcpy(REX_RAW_ADDR(copy), REX_RAW_ADDR(src), bytes);
  return copy;
}

uint32_t StringBytes(uint8_t* base, uint32_t s) {
  uint32_t n = 0;
  while (n < 1024 && REX_LOAD_U8(s + n)) ++n;
  return n + 1;
}

uint32_t WideStringBytes(uint8_t* base, uint32_t s) {
  uint32_t n = 0;
  while (n < 1024 && REX_LOAD_U16(s + 2 * n)) ++n;
  return 2 * (n + 1);
}

void LoadDone(PPCContext& ctx, uint8_t* base) {
  g_load_done = true;
  REXLOG_INFO("boot flow: profile loaded");
  if (!g_save_held) return;
  g_save_held = false;
  PPCContext keep = ctx;
  ctx.r3.u64 = g_save_r3;
  ctx.r4.u64 = g_save_r4;
  ctx.r5.u64 = g_save_r5;
  __imp__sub_832268B8(ctx, base);
  ctx = keep;
  REXLOG_INFO("boot flow: first profile save sent");
}

}  // namespace

// The skipped boot's profile load and first save are through (or skip_title is off): the front end can start other
// work, such as a level switch, without racing them (a switch 0.5 s after the main menu appeared hung the load once).
// The save is through when the options manager's pending request id (+0x914) is clear again.
bool bf3_boot_flow_settled(uint8_t* base) {
  if (!Enabled() || !g_skipped) return !Enabled();
  if (!g_load_done || g_save_held) return false;
  return !g_save_r3 || REX_LOAD_U32(g_save_r3 + 0x914) == 0;
}

// From the flow data read hook (ui_mouse.cpp): key r4, value** out (value: +8 payload, +16 type; 2 bool).
void bf3_boot_flow_data_read(uint8_t* base, uint32_t key, uint32_t out) {
  if (!Enabled() || !out) return;
  const uint32_t value = REX_LOAD_U32(out);
  if (!value) return;
  if (g_calibration_read) {
    g_calibration_read = false;
    SetBool(base, value);
    REXLOG_INFO("boot flow: Has calibrated? ({:08X}) answered true", key);
  } else if (key == kHasPressedStartKey && !g_skipped) {
    g_skipped = true;
    SetBool(base, value);
    REXLOG_INFO("boot flow: HasPressedStart answered true (skip_title): straight to the main menu");
  }
}

extern "C" REX_FUNC(sub_832917E0) {
  const uint32_t t = ctx.r4.u32;
  if (Plausible(t)) {
    const uint32_t from = REX_LOAD_U32(t + 8), to = REX_LOAD_U32(t + 0xC);
    if (FlowTrace()) {
      REXLOG_INFO("ui flow: [{}] -> [{}]", NodeName(base, from), NodeName(base, to));
    }
    if (g_skipped && !g_profile_load && NodeName(base, to) == "SetHasPressedStart") StartProfileLoad(ctx, base);
    static bool calibration_seen = false;
    if (g_profile_load && !calibration_seen && NodeName(base, to) == "Has calibrated?") {
      calibration_seen = g_calibration_read = true;
    }
  }
  __imp__sub_832917E0(ctx, base);
}

// Front-end engine message handler (this r3, message r4: +4 category, +8 type).
extern "C" REX_FUNC(sub_8374EE10) {
  const uint32_t m = ctx.r4.u32;
  if (FlowTrace() && Plausible(m)) {
    REXLOG_INFO("ui message: category {:08X} type {:08X} +28 {:08X} +2C {:08X}", REX_LOAD_U32(m + 4),
                REX_LOAD_U32(m + 8), REX_LOAD_U32(m + 0x28), REX_LOAD_U32(m + 0x2C));
  }
  if (!g_frontend) g_frontend = ctx.r3.u32;
  const bool load_done = g_profile_load && !g_load_done && Plausible(m) && REX_LOAD_U32(m + 4) == kMemCardCategory &&
                         REX_LOAD_U32(m + 8) == kMemCardPopupHide;
  __imp__sub_8374EE10(ctx, base);
  if (load_done) LoadDone(ctx, base);
}

// RealMemCard: post a request to the main thread (result r3, self r4, request r5, shown_ms* r6).
extern "C" REX_FUNC(sub_83E6EE58) {
  const uint32_t lr = static_cast<uint32_t>(ctx.lr), shown = ctx.r6.u32;
  if (g_hide_popup && shown && lr >= kLoadJobBegin && lr < kLoadJobEnd) {
    g_hide_popup = false;
    // What the post returns when nobody takes the request: the shared empty handle, one more reference on it.
    const uint32_t result = ctx.r3.u32, empty = REX_LOAD_U32(REX_LOAD_U32(kEmptyHandleOwner) + 4);
    auto* refs = reinterpret_cast<volatile LONG*>(REX_RAW_ADDR(empty + 4));  // big-endian, atomic like the guest's
    LONG seen, want;
    do {
      seen = *refs;
      want = static_cast<LONG>(_byteswap_ulong(_byteswap_ulong(static_cast<ULONG>(seen)) + 1));
    } while (InterlockedCompareExchange(refs, want, seen) != seen);
    REX_STORE_U32(result, kHandleVtable);
    REX_STORE_U32(result + 4, empty);
    REX_STORE_U32(shown, 0xFFFFFFFF);
    REXLOG_INFO("boot flow: CHECKING DATA popup skipped");
    return;
  }
  __imp__sub_83E6EE58(ctx, base);
}

// Profile options save (options manager r3, r4, r5); returns whether the save was sent.
extern "C" REX_FUNC(sub_832268B8) {
  if (g_profile_load && !g_load_done && !g_save_held) {
    g_save_held = true;
    g_save_r3 = ctx.r3.u32;
    g_save_r4 = Plausible(ctx.r4.u32) ? GuestCopy(ctx, base, ctx.r4.u32, StringBytes(base, ctx.r4.u32)) : ctx.r4.u32;
    g_save_r5 = Plausible(ctx.r5.u32) ? GuestCopy(ctx, base, ctx.r5.u32, WideStringBytes(base, ctx.r5.u32)) : ctx.r5.u32;
    REXLOG_INFO("boot flow: first profile save held until the profile is loaded");
    ctx.r3.u64 = 0;
    return;
  }
  __imp__sub_832268B8(ctx, base);
}
