// Native mouse aim for the local soldier (see mouse_aim.cpp).
#pragma once

#include <cstdint>

namespace bf3_main {
// Takes the mouse turn gathered since the last frame and shows it in the local aiming state; called once per frame at
// the frame boundary (frame_rate.cpp), so everything the frame computes sees the same aim.
void SampleMouseLook(PPCContext& ctx, uint8_t* base);

// The local soldier, 0 unless a tick saw it in the last 200 ms.
uint32_t LocalSoldier();
// Local aiming updates so far (one per simulation tick while a local soldier exists).
uint64_t LocalAimTicks();
// Adds a turn (radians, yaw right +, pitch up +) to the next frame's mouse look, like mouse motion (debug API).
void InjectLook(float dyaw, float dpitch);
}  // namespace bf3_main

// The aiming state the local player's input state is filled from.
extern "C" uint32_t bf3_aim_state();
