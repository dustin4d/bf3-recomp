// Native mouse aim for the local soldier (see mouse_aim.cpp).
#pragma once

namespace bf3_main {
// Takes the mouse turn gathered since the last frame and shows it in the local aiming state; called once per frame at
// the frame boundary (frame_rate.cpp), so everything the frame computes sees the same aim.
void SampleMouseLook(PPCContext& ctx, uint8_t* base);
}  // namespace bf3_main
