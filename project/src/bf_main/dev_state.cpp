// Debug API, game side (development builds only: BF3_DEV_TOOLS, see CMakeLists.txt). bf3.exe's command server
// (src/dev/debug_api.cpp) reads the state gathered here and hands game commands over; see src/dev/dev_shared.h.
//
// State: once per main loop frame (DevFrame, from the frame boundary in frame_rate.cpp) the snapshot is rebuilt from
// the level manager (level_boot.cpp), the local soldier and its aiming state (mouse_aim.cpp), the server player
// (practice.cpp) and two hooks here:
// - sub_8325A1D0(soldier r3, transform r4, r5): checks the 4x4 transform (rows left, up, forward, position, 16 bytes
//   each) is finite (sub_830123E0 per row), then sets it as the soldier's world transform (sub_838344A0) and, with r5,
//   runs sub_83259BC0. Called from the soldier's camera update (sub_8325C658) every frame and tick.
// - sub_832C8108(this r3, out r4, r5): the render camera (camera vtable +0xFC into a local, then sub_832C3DE0 writes
//   the camera transform to r4).
// Game commands (bf3_dev_request) run on the main thread at the next frame boundary, so they may call guest code.

#include "bf3_pch.h"

#include "dev_state.h"
#include "mouse_aim.h"
#include "pc_concepts.h"

#include "../dev/dev_shared.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstring>
#include <functional>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

REX_EXTERN(__imp__sub_8325A1D0);
REX_EXTERN(__imp__sub_832C8108);

uint64_t bf3_level_entered_ingame_ms();  // level_boot.cpp
uint32_t bf3_level_manager();            // level_boot.cpp
uint32_t PracticeServerPlayer();         // practice.cpp
void PracticeApplyTeam(PPCContext& ctx, uint8_t* base, uint32_t team);  // practice.cpp
void PracticeRequestDeploy(int kit);     // practice.cpp
extern "C" void bf3_practice_set_team(int team);       // practice.cpp
extern "C" void bf3_practice_launch(const char* spec);  // level_boot.cpp

namespace {

using Clock = std::chrono::steady_clock;
using Snapshot = bf3::dev::Snapshot;

constexpr const char* kMetroTdm = "Levels/MP_Subway/MP_Subway:GameMode=TeamDeathMatch0";
constexpr float kPi = 3.14159265f;

uint8_t* g_base = nullptr;
Clock::time_point g_start;

std::mutex g_mutex;  // everything below
Snapshot g_snap{};
std::array<float, 600> g_frame_ms{};
size_t g_frame_count = 0;  // frames recorded in g_frame_ms (total; index = count % size)

// Local soldier transform, set by the sub_8325A1D0 hook.
uint32_t g_xf_soldier = 0;
float g_xf[16] = {};
Clock::time_point g_xf_time;
float g_velocity[3] = {};
float g_vel_pos[3] = {};
Clock::time_point g_vel_time;
uint32_t g_vel_soldier = 0;

float g_camera[16] = {};
uint32_t g_camera_calls = 0;

// Game commands waiting for the main thread.
std::vector<std::function<void(PPCContext&, uint8_t*)>> g_pending;

float LoadF(uint8_t* base, uint32_t a) { return std::bit_cast<float>(REX_LOAD_U32(a)); }

void LoadMatrix(uint8_t* base, uint32_t a, float* out) {
  for (int i = 0; i < 16; ++i) out[i] = LoadF(base, a + 4 * i);
}

double Seconds(Clock::duration d) { return std::chrono::duration<double>(d).count(); }

// Guest memory a host read can't fault on.
bool Readable(uint8_t* base, uint32_t addr, uint32_t length) {
  if (!base || !length || uint64_t(addr) + length > 0x100000000ull) return false;
  uint8_t* p = base + addr;
  uint8_t* end = p + length;
  while (p < end) {
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery(p, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT ||
        (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) {
      return false;
    }
    p = static_cast<uint8_t*>(mbi.BaseAddress) + mbi.RegionSize;
  }
  return true;
}

// A printable guest C string up to `max` chars ("" if unreadable or not text), read within the string's page.
std::string GuestString(uint8_t* base, uint32_t addr, size_t max) {
  if (!addr) return {};
  max = std::min<size_t>(max, 0x1000 - (addr & 0xFFF));
  if (!Readable(base, addr, uint32_t(max))) return {};
  std::string s;
  for (size_t i = 0; i < max; ++i) {
    const char c = char(REX_LOAD_U8(addr + uint32_t(i)));
    if (!c) break;
    if (c < 0x20 || c > 0x7E) return {};
    s += c;
  }
  return s;
}

// Wraps an angle difference to [-pi, pi).
float WrapDelta(float a) {
  a = std::fmod(a + kPi, 2 * kPi);
  if (a < 0) a += 2 * kPi;
  return a - kPi;
}

void Queue(std::function<void(PPCContext&, uint8_t*)> fn) {
  std::lock_guard lock(g_mutex);
  g_pending.push_back(std::move(fn));
}

std::vector<std::string> Split(const char* line) {
  std::vector<std::string> out;
  std::istringstream in(line ? line : "");
  for (std::string w; in >> w;) out.push_back(w);
  return out;
}

bool ParseFloat(const std::string& s, float& out) {
  char* end = nullptr;
  out = std::strtof(s.c_str(), &end);
  return end && *end == 0 && std::isfinite(out);
}

}  // namespace

void bf3_main::DevFrame(PPCContext& ctx, uint8_t* base) {
  const Clock::time_point now = Clock::now();
  static Clock::time_point last;
  if (!g_base) {
    g_base = base;
    g_start = now;
  }

  std::vector<std::function<void(PPCContext&, uint8_t*)>> pending;
  {
    std::lock_guard lock(g_mutex);
    pending.swap(g_pending);
  }
  for (auto& fn : pending) {
    PPCContext saved = ctx;
    fn(ctx, base);
    ctx = saved;
  }

  Snapshot s{};
  s.version = bf3::dev::kSnapshotVersion;
  s.uptime_s = Seconds(now - g_start);
  s.ticks = LocalAimTicks();

  if (const uint32_t manager = bf3_level_manager(); manager && Readable(base, manager, 0x74)) {
    s.level_manager = manager;
    const std::string name = GuestString(base, REX_LOAD_U32(manager + 0x1c), sizeof(s.level) - 1);
    std::memcpy(s.level, name.c_str(), name.size() + 1);
    s.level_loaded = REX_LOAD_U32(manager + 0x70) != 0;
    s.level_unloading = REX_LOAD_U8(manager + 0x3d);
  }
  const uint64_t entered = bf3_level_entered_ingame_ms();
  s.ingame_s = entered ? (std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() -
                          int64_t(entered)) / 1000.0
                       : -1.0;

  s.soldier = LocalSoldier();
  s.on_foot = LocalOnFoot();
  s.aim_state = s.soldier ? bf3_aim_state() : 0;
  if (s.aim_state && Readable(base, s.aim_state, 0x14)) {
    if (const uint32_t aimer = REX_LOAD_U32(s.aim_state + 8); aimer && Readable(base, aimer, 0x14)) {
      s.aim_yaw = LoadF(base, aimer + 0xC);
      s.aim_pitch = LoadF(base, aimer + 0x10);
    }
  }
  s.server_player = PracticeServerPlayer();
  if (s.server_player && Readable(base, s.server_player + 0x324, 4)) s.team = REX_LOAD_U32(s.server_player + 0x324);

  std::lock_guard lock(g_mutex);
  if (s.soldier && s.soldier == g_xf_soldier) {
    std::memcpy(s.transform, g_xf, sizeof(g_xf));
    s.transform_age_s = Seconds(now - g_xf_time);
    std::memcpy(s.velocity, g_velocity, sizeof(g_velocity));
  }
  std::memcpy(s.camera, g_camera, sizeof(g_camera));
  s.camera_calls = g_camera_calls;
  g_camera_calls = 0;
  s.frame = g_snap.frame + 1;
  g_snap = s;
  if (last != Clock::time_point{}) g_frame_ms[g_frame_count++ % g_frame_ms.size()] = float(Seconds(now - last) * 1000.0);
  last = now;
}

extern "C" REX_FUNC(sub_8325A1D0) {
  const uint32_t soldier = ctx.r3.u32, xf = ctx.r4.u32;
  __imp__sub_8325A1D0(ctx, base);
  if (!soldier || !xf || soldier != bf3_main::LocalSoldier()) return;
  float m[16];
  LoadMatrix(base, xf, m);
  const Clock::time_point now = Clock::now();
  std::lock_guard lock(g_mutex);
  g_xf_soldier = soldier;
  std::memcpy(g_xf, m, sizeof(m));
  g_xf_time = now;
  if (g_vel_soldier != soldier) {
    g_vel_soldier = soldier;
    g_vel_time = now;
    std::memcpy(g_vel_pos, m + 12, sizeof(g_vel_pos));
    std::fill(std::begin(g_velocity), std::end(g_velocity), 0.0f);
  } else if (const double dt = Seconds(now - g_vel_time); dt >= 0.1) {
    for (int i = 0; i < 3; ++i) {
      g_velocity[i] = float((m[12 + i] - g_vel_pos[i]) / dt);
      g_vel_pos[i] = m[12 + i];
    }
    g_vel_time = now;
  }
}

extern "C" REX_FUNC(sub_832C8108) {
  const uint32_t out = ctx.r4.u32;
  __imp__sub_832C8108(ctx, base);
  if (!out) return;
  float m[16];
  LoadMatrix(base, out, m);
  std::lock_guard lock(g_mutex);
  std::memcpy(g_camera, m, sizeof(m));
  ++g_camera_calls;
}

extern "C" __declspec(dllexport) bool bf3_dev_snapshot(Snapshot* out, uint32_t size) {
  if (!out || size != sizeof(Snapshot)) return false;
  std::lock_guard lock(g_mutex);
  *out = g_snap;
  return g_snap.version == bf3::dev::kSnapshotVersion;
}

extern "C" __declspec(dllexport) int bf3_dev_frame_times(float* out_ms, int capacity) {
  std::lock_guard lock(g_mutex);
  const size_t n = std::min({g_frame_count, g_frame_ms.size(), size_t(std::max(capacity, 0))});
  for (size_t i = 0; i < n; ++i) out_ms[i] = g_frame_ms[(g_frame_count - n + i) % g_frame_ms.size()];
  return int(n);
}

extern "C" __declspec(dllexport) int bf3_dev_peek(uint32_t addr, uint8_t* out, int length) {
  if (length <= 0 || !Readable(g_base, addr, uint32_t(length))) return 0;
  std::memcpy(out, g_base + addr, size_t(length));  // raw guest bytes (big-endian)
  return length;
}

// Game commands. Returns false with the reason in `reply` if the command can't be taken; true once it is queued
// (or done), `reply` then says what will happen.
//   level [<level>[:<category>=<value>]]   switch the local server to a level (default Metro TDM)
//   team <us|ru|1|2>                        team for the next join; also moves the player now if already in a level
//   deploy [kit 0-3]                        deploy from the spawn screen (default 0, assault)
//   look <yaw> <pitch>                      set the local soldier's aim (aimer radians)
//   turn <dyaw> <dpitch>                    turn the local soldier's aim by radians
extern "C" __declspec(dllexport) bool bf3_dev_request(const char* line, char* reply, int capacity) {
  auto answer = [&](bool ok, const std::string& text) {
    if (capacity > 0) {
      const size_t n = std::min(text.size(), size_t(capacity - 1));
      std::memcpy(reply, text.data(), n);
      reply[n] = 0;
    }
    return ok;
  };
  const std::vector<std::string> args = Split(line);
  if (args.empty()) return answer(false, "empty command");
  const std::string& cmd = args[0];
  if (!g_base) return answer(false, "BF.Main has not run a frame yet");
  uint8_t* base = g_base;  // for REX_LOAD_*

  if (cmd == "level") {
    const uint32_t manager = bf3_level_manager();
    if (!manager || !REX_LOAD_U32(manager + 0x70)) return answer(false, "level manager not ready (no level loaded)");
    const std::string spec = args.size() > 1 ? args[1] : kMetroTdm;
    bf3_practice_launch(spec.c_str());
    return answer(true, "level switch requested: " + spec);
  }
  if (cmd == "team") {
    if (args.size() < 2) return answer(false, "usage: team <us|ru|1|2>");
    const std::string& t = args[1];
    const int team = (t == "us" || t == "US" || t == "1") ? 1 : (t == "ru" || t == "RU" || t == "2") ? 2 : 0;
    if (!team) return answer(false, "team must be us, ru, 1 or 2");
    bf3_practice_set_team(team);
    Queue([team](PPCContext& ctx, uint8_t* base) { PracticeApplyTeam(ctx, base, uint32_t(team)); });
    return answer(true, team == 1 ? "team US" : "team RU");
  }
  if (cmd == "deploy") {
    const int kit = args.size() > 1 ? std::atoi(args[1].c_str()) : 0;
    if (kit < 0 || kit > 3) return answer(false, "kit must be 0-3");
    PracticeRequestDeploy(kit);
    return answer(true, "deploy kit " + std::to_string(kit) + " requested");
  }
  if (cmd == "look" || cmd == "turn") {
    float a = 0, b = 0;
    if (args.size() < 3 || !ParseFloat(args[1], a) || !ParseFloat(args[2], b)) {
      return answer(false, "usage: " + cmd + " <yaw> <pitch> (radians)");
    }
    if (!bf3_main::LocalSoldier()) return answer(false, "no local soldier");
    const bool absolute = cmd == "look";
    Queue([absolute, a, b](PPCContext&, uint8_t* base) {
      float dyaw = a, dpitch = b;
      if (absolute) {
        const uint32_t state = bf3_aim_state();
        const uint32_t aimer = state ? REX_LOAD_U32(state + 8) : 0;
        if (!aimer) return;
        dyaw = WrapDelta(a - LoadF(base, aimer + 0xC));
        dpitch = b - LoadF(base, aimer + 0x10);
      }
      bf3_main::InjectLook(dyaw, dpitch);
    });
    return answer(true, absolute ? "aim set" : "aim turned");
  }
  return answer(false, "unknown game command '" + cmd + "'");
}
