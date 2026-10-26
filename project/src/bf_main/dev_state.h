// Debug API, game side (see dev_state.cpp). Development builds only (BF3_DEV_TOOLS).
#pragma once

namespace bf3_main {
// Runs pending game commands and rebuilds the snapshot; called once per frame at the frame boundary (frame_rate.cpp).
void DevFrame(PPCContext& ctx, uint8_t* base);
}  // namespace bf3_main
