// Raw mouse aim: the mouse turns the soldier's view directly instead of going through the emulated right stick
// (dead zone, response curve and turn acceleration made mouse look feel muddy).
//
// Guest side (BF.Main):
// - sub_8325D1C0 fills the local player's EntryInputState from the soldier; for infantry it takes yaw/pitch from the
//   aiming state returned by sub_832EC788 (call at 0x8325D1F8): yaw +0xC (wrapped to [0, 2pi)), pitch +0x10.
// - The aiming update sub_83267DF0 integrates input in sub_83264488(aimer = *(state + 8)) (aimer yaw +0xC, pitch
//   +0x10; stick right = yaw up), copies the aimer's angles into the state and adds weapon sway (sub_832652A0).
// Mouse input is taken once per frame, at the frame boundary (SampleMouseLook after the frame wait, bf3_mouse_look_delta
// in pc_mouse.cpp: radians, sensitivity applied), like a PC game polling input. Before sub_83264488 runs for the local
// player's aimer, the turn sampled so far is added to the aimer's yaw/pitch; the game then wraps and clamps them like
// stick input.
//
// Render aim: the aiming update runs on the 30 Hz simulation tick, but frames run uncapped, and this console build does
// no frame interpolation of the soldier's aim (the per-frame pass sub_83262160 rebuilds the first-person camera,
// sub_8325C658 -> sub_832C3C70 -> sub_832C0360, from the same tick values; the frame loop's interpolation fraction is
// always 0), so the view and the weapon stepped at 30 Hz: jitter. At the frame boundary (then the soldier's camera
// update runs, so its transform, the first-person arms' root, follows too), after each local aiming update
// (sub_83267DF0) and before each camera update (sub_8325C658, from the tick's soldier update sub_83261EC8 or the
// per-frame one), the aiming state's yaw/pitch (+0xC/+0x10, read by the camera and the weapon every frame) are set to the tick's values
// plus the sampled turn no tick has applied yet: what the next tick will apply, shown now, so the view turns every
// frame by that frame's mouse motion with no added latency. Pitch goes through the game's own clamp
// sub_83258A60(aimer, pitch, input = state + 0x30); the sway the tick added (+0x1C/+0x20) is kept. The next aiming
// update rebuilds the state from the aimer, so the simulation never integrates the shown values. If anything else
// changed the state since our write (respawn, vehicle), it is left alone.
//
// BF3_MOUSE_AIM_TEST="<dyaw>,<dpitch>,<frames>" adds that many radians per tick for <frames> ticks once playing;
// BF3_AIM_TRACE=1 logs the aimer's per-tick angle change from the game's own input once a second.

#include "bf3_pch.h"

#include "mouse_aim.h"
#include "pc_concepts.h"

#include <windows.h>

#include <atomic>
#include <bit>
#include <cstdio>
#include <cstdlib>

REX_EXTERN(__imp__sub_832EC788);
REX_EXTERN(__imp__sub_83264488);
REX_EXTERN(__imp__sub_83267DF0);
REX_EXTERN(__imp__sub_8325C658);
REX_EXTERN(sub_83258A60);

namespace {

std::atomic<uint32_t> g_player_aim_state{0};
std::atomic<uint32_t> g_player_soldier{0};      // the soldier sub_8325D1C0 filled the input state from
std::atomic<ULONGLONG> g_player_soldier_ms{0};  // when (GetTickCount64): only a soldier seen by recent ticks is used

using DeltaFn = bool (*)(float*, float*);

DeltaFn MouseLookDelta() {
  static DeltaFn fn = reinterpret_cast<DeltaFn>(GetProcAddress(GetModuleHandleW(nullptr), "bf3_mouse_look_delta"));
  return fn;
}

// Mouse turn sampled at frame starts and not yet applied by a tick (radians; main thread only).
float g_frame_dx = 0.0f, g_frame_dy = 0.0f;
// Turn added by the debug API (InjectLook), taken at the next frame start like mouse motion (main thread only).
float g_inject_dx = 0.0f, g_inject_dy = 0.0f;
std::atomic<uint64_t> g_aim_ticks{0};

float LoadF(uint8_t* base, uint32_t a) { return std::bit_cast<float>(REX_LOAD_U32(a)); }
void StoreF(uint8_t* base, uint32_t a, float v) { REX_STORE_U32(a, std::bit_cast<uint32_t>(v)); }

constexpr float kTwoPi = 6.2831855f;  // the aiming update's yaw wrap (0x82D81004)

// The local aiming state as the last tick left it (main thread only: aiming update and camera updates).
struct TickAim {
  uint32_t state = 0;
  float yaw = 0.0f, pitch = 0.0f;              // state +0xC/+0x10 (aimer + sway)
  float aimer_pitch = 0.0f;                    // aimer +0x10, clamped
  float shown_yaw = 0.0f, shown_pitch = 0.0f;  // what we last wrote into the state
};
TickAim g_tick;

// Writes the tick's aim plus the pending mouse turn into the aiming state.
void ShowRenderAim(PPCContext& ctx, uint8_t* base) {
  const uint32_t state = g_tick.state;
  if (!state || state != g_player_aim_state.load(std::memory_order_relaxed) || !bf3_main::LocalOnFoot()) return;
  const float yaw = LoadF(base, state + 0xC), pitch = LoadF(base, state + 0x10);
  if (yaw != g_tick.shown_yaw || pitch != g_tick.shown_pitch) {  // the game moved it since: not ours any more
    g_tick.state = 0;
    return;
  }
  const float dx = g_frame_dx, dy = g_frame_dy;
  float shown_yaw = g_tick.yaw + dx;
  if (shown_yaw >= kTwoPi) shown_yaw -= kTwoPi;
  if (shown_yaw < 0.0f) shown_yaw += kTwoPi;
  float shown_pitch = g_tick.pitch;
  if (dy != 0.0f) {
    PPCContext saved = ctx;
    ctx.r3.u64 = REX_LOAD_U32(state + 8);
    ctx.r5.u64 = state + 0x30;
    ctx.f1.f64 = double(g_tick.aimer_pitch + dy);
    sub_83258A60(ctx, base);
    shown_pitch = float(ctx.f1.f64) + (g_tick.pitch - g_tick.aimer_pitch);
    ctx = saved;
  }
  StoreF(base, state + 0xC, shown_yaw);
  StoreF(base, state + 0x10, shown_pitch);
  g_tick.shown_yaw = shown_yaw;
  g_tick.shown_pitch = shown_pitch;
}

}  // namespace

void bf3_main::SampleMouseLook(PPCContext& ctx, uint8_t* base) {
  float dx = 0.0f, dy = 0.0f;
  DeltaFn delta = MouseLookDelta();
  // In a vehicle the counts are left for the vehicle's look concepts (pc_concepts.cpp).
  const bool aiming = g_player_aim_state.load(std::memory_order_relaxed) && LocalOnFoot();
  const bool mouse = aiming && delta && delta(&dx, &dy);
  const bool injected = aiming && (g_inject_dx != 0.0f || g_inject_dy != 0.0f);
  if (injected) {
    dx += g_inject_dx;
    dy += g_inject_dy;
  }
  g_inject_dx = g_inject_dy = 0.0f;
  if (!mouse && !injected) {
    g_frame_dx = g_frame_dy = 0.0f;
    return;
  }
  g_frame_dx += dx;
  g_frame_dy += dy;
  ShowRenderAim(ctx, base);
  // The soldier's transform follows the aim only in its camera update (sub_8325C658 -> sub_8325A1D0), which on frames
  // without a tick runs after the animation jobs have read it: the first-person arms' root then lagged a frame behind
  // the view. Run it now, as the per-frame pass would, so the whole frame sees the new aim.
  const uint32_t soldier = g_player_soldier.load(std::memory_order_relaxed);
  if (soldier && g_tick.state && GetTickCount64() - g_player_soldier_ms.load(std::memory_order_relaxed) < 200) {
    PPCContext saved = ctx;
    ctx.r3.u64 = soldier;
    __imp__sub_8325C658(ctx, base);
    ctx = saved;
  }
}

// The aiming state the local player's input state is filled from (also read by bf3.exe's BF3_WATCH=aim).
extern "C" __declspec(dllexport) uint32_t bf3_aim_state() { return g_player_aim_state.load(); }

uint32_t bf3_main::LocalSoldier() {
  const uint32_t soldier = g_player_soldier.load(std::memory_order_relaxed);
  return soldier && GetTickCount64() - g_player_soldier_ms.load(std::memory_order_relaxed) < 200 ? soldier : 0;
}

uint64_t bf3_main::LocalAimTicks() { return g_aim_ticks.load(std::memory_order_relaxed); }

void bf3_main::InjectLook(float dyaw, float dpitch) {
  g_inject_dx += dyaw;
  g_inject_dy += dpitch;
}

extern "C" REX_FUNC(sub_832EC788) {
  const uint32_t lr = uint32_t(ctx.lr);
  __imp__sub_832EC788(ctx, base);
  if (lr == 0x8325D1FC && ctx.r3.u32) {
    g_player_aim_state.store(ctx.r3.u32, std::memory_order_relaxed);
    g_player_soldier.store(ctx.r30.u32, std::memory_order_relaxed);  // sub_8325D1C0 keeps the soldier in r30
    g_player_soldier_ms.store(GetTickCount64(), std::memory_order_relaxed);
  }
}

extern "C" REX_FUNC(sub_83264488) {
  const uint32_t aimer = ctx.r3.u32;
  const uint32_t state = g_player_aim_state.load(std::memory_order_relaxed);
  // In a vehicle the mouse goes through the vehicle's look concepts instead (pc_concepts.cpp).
  const bool local = aimer && state && REX_LOAD_U32(state + 8) == aimer && bf3_main::LocalOnFoot();
  if (!local) {
    __imp__sub_83264488(ctx, base);
    return;
  }

  float dx = g_frame_dx, dy = g_frame_dy;  // yaw, pitch radians
  g_frame_dx = g_frame_dy = 0.0f;
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

extern "C" REX_FUNC(sub_83267DF0) {
  const uint32_t state = ctx.r3.u32;
  __imp__sub_83267DF0(ctx, base);
  if (!state || state != g_player_aim_state.load(std::memory_order_relaxed)) return;
  const uint32_t aimer = REX_LOAD_U32(state + 8);
  if (!aimer) return;
  g_aim_ticks.fetch_add(1, std::memory_order_relaxed);
  g_tick.state = state;
  g_tick.yaw = g_tick.shown_yaw = LoadF(base, state + 0xC);
  g_tick.pitch = g_tick.shown_pitch = LoadF(base, state + 0x10);
  g_tick.aimer_pitch = LoadF(base, aimer + 0x10);
  ShowRenderAim(ctx, base);
}

extern "C" REX_FUNC(sub_8325C658) {
  ShowRenderAim(ctx, base);
  __imp__sub_8325C658(ctx, base);
}
