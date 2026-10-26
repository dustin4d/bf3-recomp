// Raw mouse aim: the mouse turns the soldier's view directly instead of going through the emulated right stick
// (dead zone, response curve and turn acceleration made mouse look feel muddy).
//
// Guest side (BF.Main):
// - sub_8325D1C0 fills the local player's EntryInputState from the soldier; for infantry it takes yaw/pitch from the
//   aiming state returned by sub_832EC788 (call at 0x8325D1F8): yaw +0xC (wrapped to [0, 2pi)), pitch +0x10.
// - The aiming update sub_83267DF0 integrates input in sub_83264488(aimer = *(state + 8)) (aimer yaw +0xC, pitch
//   +0x10; stick right = yaw up), copies the aimer's angles into the state and adds weapon sway (sub_832652A0).
// Before sub_83264488 runs for the local player's aimer, the mouse turn since the last frame (bf3.exe's raw input,
// bf3_mouse_look_delta in pc_mouse.cpp: radians, sensitivity applied) is added to the aimer's yaw/pitch; the game then
// wraps and clamps them like stick input.
//
// BF3_MOUSE_AIM_TEST="<dyaw>,<dpitch>,<frames>" adds that many radians per frame for <frames> frames once playing;
// BF3_AIM_TRACE=1 logs the aimer's per-frame angle change from the game's own input once a second.

#include "bf3_pch.h"

#include <windows.h>

#include <atomic>
#include <bit>
#include <cstdio>
#include <cstdlib>

REX_EXTERN(__imp__sub_832EC788);
REX_EXTERN(__imp__sub_83264488);

namespace {

std::atomic<uint32_t> g_player_aim_state{0};

using DeltaFn = bool (*)(float*, float*);

DeltaFn MouseLookDelta() {
  static DeltaFn fn = reinterpret_cast<DeltaFn>(GetProcAddress(GetModuleHandleW(nullptr), "bf3_mouse_look_delta"));
  return fn;
}

float LoadF(uint8_t* base, uint32_t a) { return std::bit_cast<float>(REX_LOAD_U32(a)); }
void StoreF(uint8_t* base, uint32_t a, float v) { REX_STORE_U32(a, std::bit_cast<uint32_t>(v)); }

}  // namespace

// The aiming state the local player's input state is filled from (also read by bf3.exe's BF3_WATCH=aim).
extern "C" __declspec(dllexport) uint32_t bf3_aim_state() { return g_player_aim_state.load(); }

extern "C" REX_FUNC(sub_832EC788) {
  const uint32_t lr = uint32_t(ctx.lr);
  __imp__sub_832EC788(ctx, base);
  if (lr == 0x8325D1FC && ctx.r3.u32) g_player_aim_state.store(ctx.r3.u32, std::memory_order_relaxed);
}

extern "C" REX_FUNC(sub_83264488) {
  const uint32_t aimer = ctx.r3.u32;
  const uint32_t state = g_player_aim_state.load(std::memory_order_relaxed);
  const bool local = aimer && state && REX_LOAD_U32(state + 8) == aimer;
  if (!local) {
    __imp__sub_83264488(ctx, base);
    return;
  }

  float dx = 0.0f, dy = 0.0f;  // yaw, pitch radians
  if (DeltaFn delta = MouseLookDelta()) delta(&dx, &dy);
  static int test_frames = -1;
  static float test_dx = 0.0f, test_dy = 0.0f;
  if (test_frames < 0) {
    test_frames = 0;
    if (const char* t = std::getenv("BF3_MOUSE_AIM_TEST")) {
      std::sscanf(t, "%f,%f,%d", &test_dx, &test_dy, &test_frames);
      REXLOG_INFO("mouse aim: test {} x ({}, {}) rad", test_frames, test_dx, test_dy);
    }
  }
  if (test_frames > 0) {
    --test_frames;
    dx += test_dx;
    dy += test_dy;
  }

  if (dx != 0.0f || dy != 0.0f) {
    StoreF(base, aimer + 0xC, LoadF(base, aimer + 0xC) + dx);
    StoreF(base, aimer + 0x10, LoadF(base, aimer + 0x10) + dy);
  }

  static const bool trace = [] {
    const char* v = std::getenv("BF3_AIM_TRACE");
    return v && *v && *v != '0';
  }();
  const float yaw0 = LoadF(base, aimer + 0xC), pitch0 = LoadF(base, aimer + 0x10);
  __imp__sub_83264488(ctx, base);
  if (trace) {
    static ULONGLONG next = 0;
    const ULONGLONG now = GetTickCount64();
    if (now >= next) {
      next = now + 1000;
      REXLOG_INFO("mouse aim: aimer {:08X} yaw {:.4f} pitch {:.4f}, game input moved them {:+.4f} {:+.4f}, mouse {} {}",
                  aimer, LoadF(base, aimer + 0xC), LoadF(base, aimer + 0x10), LoadF(base, aimer + 0xC) - yaw0,
                  LoadF(base, aimer + 0x10) - pitch0, dx, dy);
    }
  }
}
