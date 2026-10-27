// Fixed-tick animation with pose interpolation for the client (cvar anim_fixed_tick, default on).
//
// Guest side (BF.Main): the client's setup sub_831FC8A8 creates the animation manager (sub_83072F18) and calls
// sub_8306BFF0(manager, on), which stores `on` in manager +0xC1 ("variable tick length") when AntSettings
// AllowVariableTickLength (+0x3C) is set; `on` is 1 unless the timer settings' flags (*(timer + 0x60) +0x60 / +0x62,
// sub_838358B0 / sub_83835888) say otherwise, and it is 1 in this build. The animation manager update sub_83070B98
// interpolates poses (AntSettings InterpolatePoses +0x3B, manager +0xC3) only while +0xC1 is clear; otherwise every
// update, the per-frame pass sub_831E3610 included, advances the animation by the frame's dt.
//
// The console ran at 30 fps, so dt was always a whole tick. Uncapped, the frame's dt is arbitrary, and a pose clip one
// 60 Hz frame long that loops (it bends the local soldier's spine, skeleton bones 6/7) got sampled anywhere along its
// loop: the third-person body flickered by up to 23 degrees, most visibly when looking down. With +0xC1 clear the
// animation advances on simulation ticks only and the per-frame pass blends the last two tick poses by the frame's
// interpolation fraction (info +0x1C): the console's poses, shown smoothly at any frame rate.

#include "bf3_pch.h"

#include <rex/cvar.h>

REXCVAR_DEFINE_BOOL(anim_fixed_tick, true, "BF3",
                    "Animation advances on simulation ticks and interpolates poses between them (applies at spawn)");

REX_EXTERN(__imp__sub_8306BFF0);

extern "C" REX_FUNC(sub_8306BFF0) {
  if (REXCVAR_GET(anim_fixed_tick)) ctx.r4.u64 = 0;
  __imp__sub_8306BFF0(ctx, base);
}
