// Debug API (development builds only, BF3_DEV_TOOLS): what bf3.exe's command server (dev/debug_api.cpp) and
// bf3_BF_Main.dll's game-state side (bf_main/dev_state.cpp) exchange. Plain data, same layout on both sides; the
// version guards against a stale DLL next to a newer exe.
#pragma once

#include <cstdint>

namespace bf3::dev {

constexpr uint32_t kSnapshotVersion = 1;

// Game state as of the last main loop frame (filled on the main thread at the frame boundary).
struct Snapshot {
  uint32_t version;   // kSnapshotVersion
  uint64_t frame;     // main loop frames since BF.Main started
  uint64_t ticks;     // local aiming updates (the 30 Hz simulation tick, while a local soldier exists)
  double uptime_s;    // since BF.Main's first frame

  // Level manager (sub_833AF040's r3): +0x1c current LevelSetup (+0 char* name), +0x3d unloading, +0x70 loaded level.
  uint32_t level_manager;
  char level[96];     // current LevelSetup name, "" if none
  uint8_t level_loaded;
  uint8_t level_unloading;
  double ingame_s;    // since the last CoreEnteredIngame (front end or level), -1 before

  // Local soldier (client side): the soldier sub_8325D1C0 fills the local input state from.
  uint32_t soldier;          // 0 unless seen by a tick in the last 200 ms
  uint8_t on_foot;           // its last mapped entry is the soldier (not a vehicle seat)
  float transform[16];       // world transform set by sub_8325A1D0: rows left, up, forward, position (w unused)
  double transform_age_s;    // since that transform was last set
  float velocity[3];         // m/s, from the position over ~0.1 s windows
  uint32_t aim_state;        // aiming state (sub_832EC788 at 0x8325D1F8)
  float aim_yaw, aim_pitch;  // its aimer +0xC/+0x10 (radians; yaw wraps to [0, 2pi))

  // Render camera: the last output of sub_832C8108 (rows left, up, forward, position).
  float camera[16];
  uint32_t camera_calls;     // sub_832C8108 calls in the last full frame

  // Server side (in-process local server).
  uint32_t server_player;    // ServerPlayer of the last ServerPlayerEnteredLevel
  uint32_t team;             // its team (+0x324): 0 neutral, 1 US, 2 RU
};

}  // namespace bf3::dev

// Exported by bf3_BF_Main.dll (bf_main/dev_state.cpp), looked up by bf3.exe with GetProcAddress:
//   bool bf3_dev_snapshot(bf3::dev::Snapshot* out, uint32_t size)        copy of the last frame's state
//   int  bf3_dev_frame_times(float* out_ms, int capacity)                recent frame times, oldest first
//   bool bf3_dev_request(const char* line, char* reply, int capacity)    game commands (main thread, next frame)
//   int  bf3_dev_peek(uint32_t guest_addr, uint8_t* out, int length)     guest memory read (bytes copied, 0 if unmapped)
