// Diagnostic (development builds): the animation manager update sub_83070B98(manager, info) and its two client
// callers, sub_831E3610 (per frame, update pass 3 = FrameInterpolation; returns to 0x831E3700) and sub_831EC2F8
// (simulation tick frames; returns to 0x831EC4B0). Info: +0 dt, +4 tick time, +8 tick count, +0xC tick length,
// +0x18 1 on the per-frame pass, +0x1C interpolation fraction, +0x60/+0x64 tick pass only.
// Runtime switches (cvar command of the debug API):
//   anim_log N        log the next N updates (caller, local tick, info fields)
//   anim_dt_mode M    0 game; 1 dt in whole 1/60 s steps (carry kept); 2 like 1 and the per-frame pass at most once
//                     per local tick; 3 per-frame pass dt = 0; 4 per-frame pass skipped
//   anim_alpha X      X >= 0: override the per-frame pass's interpolation fraction (+0x1C)
//   anim_rec N        log the next N frames (at the client pass, before it runs: the previous frame's final pose):
//                     the shown aim (aiming state +0xC/+0x10), the fraction and the positions of 4 bones of a pose at
//                     anim_rec_addr (bone k's x at addr + k * 32: the world-space pose blocks found near the eye)

#include "bf3_pch.h"

#include "mouse_aim.h"

#include <rex/cvar.h>

#include <bit>
#include <cmath>

REXCVAR_DEFINE_INT32(anim_log, 0, "BF3 dev", "Log the next N animation manager updates");
REXCVAR_DEFINE_INT32(anim_dt_mode, 0, "BF3 dev", "Animation update dt experiment (anim_probe.cpp)");
REXCVAR_DEFINE_INT32(anim_rec, 0, "BF3 dev", "Log bone positions and the shown aim for the next N frames");
REXCVAR_DEFINE_INT32(anim_rec_addr, 0, "BF3 dev", "Guest address of bone 0's x for anim_rec (decimal)");
REXCVAR_DEFINE_DOUBLE(anim_alpha, -1.0, "BF3 dev", "Override the per-frame animation interpolation fraction (< 0 off)");

REX_EXTERN(__imp__sub_83070B98);

namespace {
constexpr uint32_t kFramePass = 0x831E3700;  // return address in sub_831E3610
constexpr uint32_t kTickPass = 0x831EC4B0;   // return address in sub_831EC2F8

float F(uint8_t* base, uint32_t a) { return std::bit_cast<float>(REX_LOAD_U32(a)); }
void SetF(uint8_t* base, uint32_t a, float v) { REX_STORE_U32(a, std::bit_cast<uint32_t>(v)); }
}  // namespace

extern "C" REX_FUNC(sub_83070B98) {
  const uint32_t lr = static_cast<uint32_t>(ctx.lr);
  const uint32_t info = ctx.r4.u32;
  const bool client = lr == kFramePass || lr == kTickPass;
  const uint64_t tick = bf3_main::LocalAimTicks();

  if (client) {
    const int mode = REXCVAR_GET(anim_dt_mode);
    if (mode == 1 || mode == 2) {
      static double carry = 0.0;
      constexpr double kStep = 1.0 / 60.0;
      carry += F(base, info);
      const double steps = std::floor(carry / kStep + 1e-6);
      carry -= steps * kStep;
      SetF(base, info, static_cast<float>(steps * kStep));
    }
    if (mode == 2 && lr == kFramePass) {
      static uint64_t last_tick = 0;
      if (tick == last_tick) return;
      last_tick = tick;
    }
    if (mode == 3 && lr == kFramePass) SetF(base, info, 0.0f);
    if (mode == 4 && lr == kFramePass) return;
    const double alpha = REXCVAR_GET(anim_alpha);
    if (alpha >= 0.0 && lr == kFramePass) SetF(base, info + 0x1C, static_cast<float>(alpha));
  }

  if (client && REXCVAR_GET(anim_rec) > 0 && REXCVAR_GET(anim_rec_addr)) {
    REXCVAR_GET(anim_rec)--;
    const uint32_t a = static_cast<uint32_t>(REXCVAR_GET(anim_rec_addr)), aim = bf3_aim_state();
    auto P = [&](int k) { return a + 32u * k; };
    REXLOG_INFO("anim rec: tick {} pass {} alpha {:.4f} aim {:.6f} {:.6f} b0 {:.5f} {:.5f} {:.5f} b4 {:.5f} {:.5f} {:.5f} "
                "b8 {:.5f} {:.5f} {:.5f} b12 {:.5f} {:.5f} {:.5f}",
                tick, lr == kFramePass ? 'f' : 't', F(base, info + 0x1C), aim ? F(base, aim + 0xC) : 0.0f,
                aim ? F(base, aim + 0x10) : 0.0f, F(base, P(0)), F(base, P(0) + 4), F(base, P(0) + 8), F(base, P(4)),
                F(base, P(4) + 4), F(base, P(4) + 8), F(base, P(8)), F(base, P(8) + 4), F(base, P(8) + 8),
                F(base, P(12)), F(base, P(12) + 4), F(base, P(12) + 8));
  }
  if (REXCVAR_GET(anim_log) > 0) {
    REXCVAR_GET(anim_log)--;
    REXLOG_INFO("anim probe: lr {:08X} tick {} dt {:.5f} +4 {:.5f} +8 {} +C {:.5f} +10 {:.5f} +18 {} alpha {:.5f} "
                "+60 {:.5f} +64 {:.5f} +6C {:.5f}",
                lr, tick, F(base, info), F(base, info + 4), REX_LOAD_U32(info + 8), F(base, info + 0xC),
                F(base, info + 0x10), REX_LOAD_U8(info + 0x18), F(base, info + 0x1C), F(base, info + 0x60),
                F(base, info + 0x64), F(base, info + 0x6C));
  }
  __imp__sub_83070B98(ctx, base);
}
