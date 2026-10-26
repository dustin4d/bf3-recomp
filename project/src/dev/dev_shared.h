// Debug API (development builds only, BF3_DEV_TOOLS): what bf3.exe's command server (dev/debug_api.cpp) and
// bf3_BF_Main.dll's game-state side (bf_main/dev_state.cpp) exchange. Plain data, same layout on both sides; the
// version guards against a stale DLL next to a newer exe.
#pragma once

#include <cstdint>

namespace bf3::dev {

constexpr uint32_t kSnapshotVersion = 5;

// Game state as of the last main loop frame (filled on the main thread at the frame boundary).
struct Snapshot {
  uint32_t version;   // kSnapshotVersion
  uint64_t frame;     // main loop frames since BF.Main started
  uint64_t ticks;     // local aiming updates (the 30 Hz simulation tick, while a local soldier exists)
  double uptime_s;    // since BF.Main's first frame
  char screens[512];  // the UI manager's live screen stack, ';' separated (read on the main thread, ui_screens.cpp)

  // Level manager (sub_833AF040's r3): +0x1c current LevelSetup (+0 char* name), +0x3d unloading, +0x70 loaded level.
  uint32_t level_manager;
  char level[96];     // current LevelSetup name, "" if none
  uint8_t level_loaded;
  uint8_t level_unloading;
  double ingame_s;    // since the last CoreEnteredIngame (front end or level), -1 before
  uint8_t frontend_ready;  // the boot's profile load and first save are through (boot_flow.cpp)

  // Local soldier (client side): the soldier sub_8325D1C0 fills the local input state from.
  uint32_t soldier;          // 0 unless seen by a tick in the last 200 ms
  uint8_t on_foot;           // its last mapped entry is the soldier (not a vehicle seat)
  float transform[16];       // world transform set by sub_8325A1D0: rows left, up, forward, position (w unused)
  double transform_age_s;    // since that transform was last set
  float velocity[3];         // m/s, from the position over ~0.1 s windows
  uint32_t aim_state;        // aiming state (sub_832EC788 at 0x8325D1F8)
  float aim_yaw, aim_pitch;  // its aimer +0xC/+0x10 (radians; yaw wraps to [0, 2pi))
  float health;              // soldier +0x20 (what the HUD shows; 100 = full)
  // Current weapon: soldier +0x34C (weapon handler, vtable 0x82DD5FE0; sub_832D1BA0's this) +0xCC (weapon switching
  // state, 0x82DCFC3C; sub_83259218 sets +0x24) +0x24 (weapon, 0x82DD8408) +0x15C (firing, 0x82E77408) +0xF0
  // {magazine, reserve} (u32). The weapon's unlock assets (+0x8C, +0x9C, ... every 0x10; name at asset +8,
  // "Weapons/<weapon>/U_<...>") give its name and attachments.
  int32_t ammo_mag, ammo_reserve;  // -1 if unknown
  char weapon[32];
  char attachments[96];      // unlock names without the weapon's own, ';' separated
  int32_t kit;               // kit of the last deploy request (0 assault .. 3 recon), -1 before

  // Render camera: the last output of sub_832C8108 (rows left, up, forward, position).
  float camera[16];
  uint32_t camera_calls;     // sub_832C8108 calls in the last full frame

  // Server side (in-process local server).
  uint32_t server_player;    // ServerPlayer of the last ServerPlayerEnteredLevel
  uint32_t team;             // its team (+0x324): 0 neutral, 1 US, 2 RU
  uint32_t server_soldier;   // its soldier (player +0x3B0), 0 when not spawned
  float server_health;       // server soldier +0x20 (authoritative)
  float server_pos[3];       // server soldier +0x400 (its copy of the physics position)

  // Round (the level's ServerPreRoundEntity, see practice.cpp): +0xA4 players needed to start, +0xAC start countdown,
  // +0xB4 time left, +0xB8 bit 0x80 started.
  uint32_t preround_entity;
  uint32_t round_players_needed;
  float round_countdown;     // seconds left before the round starts (<= 0 once running)
  uint8_t round_started;     // the countdown has begun (enough players)
};

}  // namespace bf3::dev

// Exported by bf3_BF_Main.dll (bf_main/dev_state.cpp), looked up by bf3.exe with GetProcAddress:
//   bool bf3_dev_snapshot(bf3::dev::Snapshot* out, uint32_t size)        copy of the last frame's state
//   int  bf3_dev_frame_times(float* out_ms, int capacity)                recent frame times, oldest first
//   bool bf3_dev_request(const char* line, char* reply, int capacity)    game commands (main thread, next frame)
//   int  bf3_dev_peek(uint32_t guest_addr, uint8_t* out, int length)     guest memory read (bytes copied, 0 if unmapped)
