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
#include "dev_internal.h"
#include "mouse_aim.h"
#include "pc_concepts.h"

#include "../dev/dev_shared.h"

#include <windows.h>

#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <condition_variable>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

REX_EXTERN(__imp__sub_8325A1D0);
REX_EXTERN(__imp__sub_832C8108);
REX_EXTERN(sub_833E9F10);
REX_EXTERN(sub_8307DB88);
REX_EXTERN(sub_8307DAF0);

uint64_t bf3_level_entered_ingame_ms();  // level_boot.cpp
uint32_t bf3_level_manager();            // level_boot.cpp
bool bf3_boot_flow_settled(uint8_t* base);  // boot_flow.cpp
uint32_t PracticeServerPlayer();         // practice.cpp
uint32_t PracticePreRoundEntity(uint8_t* base);  // practice.cpp
int PracticeLastKit();                   // practice.cpp
void PracticeApplyTeam(PPCContext& ctx, uint8_t* base, uint32_t team);  // practice.cpp
void PracticeRequestDeploy(int kit);     // practice.cpp
extern "C" void bf3_practice_set_team(int team);       // practice.cpp
extern "C" void bf3_practice_launch(const char* spec);  // level_boot.cpp
extern "C" int bf3_ui_screen_list(char* out, int capacity);  // ui_screens.cpp

namespace bf3_dev {
uint8_t* g_base = nullptr;
}  // namespace bf3_dev

using namespace bf3_dev;

namespace {

using Clock = std::chrono::steady_clock;
using Snapshot = bf3::dev::Snapshot;

constexpr const char* kMetroTdm = "Levels/MP_Subway/MP_Subway:GameMode=TeamDeathMatch0";
constexpr float kPi = 3.14159265f;

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
std::condition_variable g_done_cv;  // RunOnMainThread waits here (with g_mutex)

void LoadMatrix(uint8_t* base, uint32_t a, float* out) {
  for (int i = 0; i < 16; ++i) out[i] = LoadF(base, a + 4 * i);
}

double Seconds(Clock::duration d) { return std::chrono::duration<double>(d).count(); }

}  // namespace

float bf3_dev::LoadF(uint8_t* base, uint32_t a) { return std::bit_cast<float>(REX_LOAD_U32(a)); }

// The guest heaps' page tables (ReXGlue's xmemory) say what is committed and readable. VirtualQuery gives the same
// answer but walks the host pages of the whole region to size it: ~150 us a call on the big guest heaps, which made the
// snapshot cost ~9 ms a frame. It remains the fallback for a range that spans heaps.
bool bf3_dev::Readable(uint8_t* base, uint32_t addr, uint32_t length) {
  if (!base || !length || uint64_t(addr) + length > 0x100000000ull) return false;
  if (rex::memory::Memory* memory = rex::system::kernel_memory()) {
    rex::memory::BaseHeap* heap = memory->LookupHeap(addr);
    if (heap && heap == memory->LookupHeap(addr + length - 1)) {
      if (heap->QueryRangeAccess(addr, addr + length - 1) != rex::memory::PageAccess::kNoAccess) return true;
      // The module images (0x82000000-0x8A000000): the heap's page table reports parts of them (e.g. BF.Main's
      // typeinfo section 0x84155000) as no access although the game reads them; ask the host instead.
      if (addr < 0x82000000u || addr >= 0x8A000000u) return false;
    }
  }
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

namespace {
// Copies a NUL-terminated guest string of printable characters into out; -1 if it isn't one or the read faults.
// SEH-guarded: a pointer the check passed can still go stale before the read (2026-10-27: DevFrame read a freed
// string at guest 0x400B3779 during a Metro session, "Unhandled guest access violation").
int CopyGuestString(uint8_t* base, uint32_t addr, char* out, size_t max) {
  __try {
    for (size_t i = 0; i < max; ++i) {
      const char c = char(REX_LOAD_U8(addr + uint32_t(i)));
      if (!c) return int(i);
      if (c < 0x20 || c > 0x7E) return -1;
      out[i] = c;
    }
    return int(max);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return -1;
  }
}
}  // namespace

std::string bf3_dev::GuestString(uint8_t* base, uint32_t addr, size_t max) {
  if (!addr) return {};
  max = std::min<size_t>(max, 0x1000 - (addr & 0xFFF));
  if (!Readable(base, addr, uint32_t(max))) return {};
  char buf[0x1000];
  const int n = CopyGuestString(base, addr, buf, max);
  return n > 0 ? std::string(buf, size_t(n)) : std::string();
}

void bf3_dev::Queue(std::function<void(PPCContext&, uint8_t*)> fn) {
  std::lock_guard lock(g_mutex);
  g_pending.push_back(std::move(fn));
}

bool bf3_dev::RunOnMainThread(std::function<void(PPCContext&, uint8_t*)> fn, int timeout_ms) {
  auto done = std::make_shared<bool>(false);
  std::unique_lock lock(g_mutex);
  g_pending.push_back([fn = std::move(fn), done](PPCContext& ctx, uint8_t* base) {
    fn(ctx, base);
    std::lock_guard l(g_mutex);
    *done = true;
    g_done_cv.notify_all();
  });
  return g_done_cv.wait_for(lock, std::chrono::milliseconds(timeout_ms), [&] { return *done; });
}

void bf3_dev::RunPending(PPCContext& ctx, uint8_t* base) {
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
}

std::vector<std::string> bf3_dev::Split(const char* line) {
  std::vector<std::string> out;
  std::istringstream in(line ? line : "");
  for (std::string w; in >> w;) out.push_back(w);
  return out;
}

namespace {

// Wraps an angle difference to [-pi, pi).
float WrapDelta(float a) {
  a = std::fmod(a + kPi, 2 * kPi);
  if (a < 0) a += 2 * kPi;
  return a - kPi;
}

// Server entities of a class (type info object, e.g. 0x8418958C ServerPreRoundEntity), iterated like the idle-kick
// check sub_8377FFA0: sub_8307DB88(*(0x840C2C68) entity world, type, iterator, 1), then sub_8307DAF0(iterator) until 0.
std::vector<uint32_t> ServerEntities(PPCContext& ctx, uint8_t* base, uint32_t type, size_t max) {
  std::vector<uint32_t> out;
  PPCContext saved = ctx;
  ctx.r1.u64 = (saved.r1.u32 - 256) & ~0xFull;
  const uint32_t iterator = ctx.r1.u32 + 112;
  ctx.r3.u64 = REX_LOAD_U32(0x840C2C68);
  ctx.r4.u64 = type;
  ctx.r5.u64 = iterator;
  ctx.r6.u64 = 1;
  if (ctx.r3.u32) {
    sub_8307DB88(ctx, base);
    while (out.size() < max) {
      ctx.r3.u64 = iterator;
      sub_8307DAF0(ctx, base);
      if (!ctx.r3.u32) break;
      out.push_back(ctx.r3.u32);
    }
  }
  ctx = saved;
  return out;
}

bool ParseFloat(const std::string& s, float& out) {
  char* end = nullptr;
  out = std::strtof(s.c_str(), &end);
  return end && *end == 0 && std::isfinite(out);
}

constexpr uint32_t kServerSoldierVtable = 0x82E325F0;
constexpr uint32_t kWeaponHandlerVtable = 0x82DD5FE0, kWeaponSwitchingVtable = 0x82DCFC3C;
constexpr uint32_t kWeaponVtable = 0x82DD8408, kFiringVtable = 0x82E77408;

// Follows `offset` from `obj` to an object whose vtable is `vtable`; 0 if anything along the way doesn't match.
uint32_t Follow(uint8_t* base, uint32_t obj, uint32_t offset, uint32_t vtable) {
  if (!obj || !Readable(base, obj + offset, 4)) return 0;
  const uint32_t next = REX_LOAD_U32(obj + offset);
  return next && Readable(base, next, 4) && REX_LOAD_U32(next) == vtable ? next : 0;
}

// The local soldier's current weapon: ammo, name and attachments (see dev_shared.h).
void ReadWeapon(uint8_t* base, uint32_t soldier, Snapshot& s) {
  s.ammo_mag = s.ammo_reserve = -1;
  const uint32_t handler = Follow(base, soldier, 0x34C, kWeaponHandlerVtable);
  const uint32_t switching = Follow(base, handler, 0xCC, kWeaponSwitchingVtable);
  const uint32_t weapon = Follow(base, switching, 0x24, kWeaponVtable);
  if (!weapon) return;
  if (const uint32_t firing = Follow(base, weapon, 0x15C, kFiringVtable); firing && Readable(base, firing + 0xF0, 8)) {
    s.ammo_mag = int32_t(REX_LOAD_U32(firing + 0xF0));
    s.ammo_reserve = int32_t(REX_LOAD_U32(firing + 0xF4));
  }
  std::string name, attachments;
  for (uint32_t off = 0x8C; off < 0x8C + 8 * 0x10; off += 0x10) {
    if (!Readable(base, weapon + off, 4)) break;
    const uint32_t asset = REX_LOAD_U32(weapon + off);
    if (asset < 0x40000000 || asset >= 0xA0000000 || !Readable(base, asset + 8, 4)) break;
    const std::string path = GuestString(base, REX_LOAD_U32(asset + 8), 96);  // Weapons/<weapon>/<unlock>
    if (path.rfind("Weapons/", 0) != 0) break;
    const size_t slash = path.find('/', 8);
    if (slash == std::string::npos) break;
    const std::string dir = path.substr(8, slash - 8), unlock = path.substr(slash + 1);
    if (name.empty()) name = dir;
    if (unlock != "U_" + dir) attachments += (attachments.empty() ? "" : ";") + unlock;
  }
  std::snprintf(s.weapon, sizeof(s.weapon), "%s", name.c_str());
  std::snprintf(s.attachments, sizeof(s.attachments), "%s", attachments.c_str());
}

// Server soldier teleport (main thread): sub_833E9F10(soldier r3, transform r4, r5 = 1) sets the server soldier's
// transform the way a spawn does: unless soldier +747 is set, the entity transform and aim (sub_833E9620 with the aim
// flag), the position at +272 (sub_83875BA8), and the soldier's physics at +388 (sub_830B2498, with r5 also
// sub_830B2198). The transform goes on the guest stack below the caller's frame (16-byte aligned for lvx128).
// Rows: left (cos h, 0, -sin h), up (0, 1, 0), forward (sin h, 0, cos h), position.
bool TeleportServer(PPCContext& ctx, uint8_t* base, uint32_t soldier, const float pos[3], float heading) {
  if (!soldier || REX_LOAD_U32(soldier) != kServerSoldierVtable) return false;
  const float c = std::cos(heading), sn = std::sin(heading);
  const float m[16] = {c, 0, -sn, 0, 0, 1, 0, 0, sn, 0, c, 0, pos[0], pos[1], pos[2], 1};
  PPCContext saved = ctx;
  const uint32_t sp = (saved.r1.u32 - 0x200) & ~0xFu;
  const uint32_t xf = sp + 0x100;
  for (int i = 0; i < 16; ++i) REX_STORE_U32(xf + 4 * i, std::bit_cast<uint32_t>(m[i]));
  ctx.r1.u64 = sp;
  ctx.r3.u64 = soldier;
  ctx.r4.u64 = xf;
  ctx.r5.u64 = 1;
  sub_833E9F10(ctx, base);
  ctx = saved;
  return true;
}

}  // namespace

void bf3_main::DevFrame(PPCContext& ctx, uint8_t* base) {
  const Clock::time_point now = Clock::now();
  static Clock::time_point last;
  if (!g_base) {
    g_base = base;
    g_start = now;
  }

  RunPending(ctx, base);

  Snapshot s{};
  s.version = bf3::dev::kSnapshotVersion;
  s.uptime_s = Seconds(now - g_start);
  s.ticks = LocalAimTicks();
  // The screen stack is read here, on the main thread that changes it, rather than from the API's socket thread.
  const int n = bf3_ui_screen_list(s.screens, int(sizeof(s.screens)) - 1);
  s.screens[std::clamp(n, 0, int(sizeof(s.screens)) - 1)] = 0;

  if (const uint32_t manager = bf3_level_manager(); manager && Readable(base, manager, 0x74)) {
    s.level_manager = manager;
    const std::string name = GuestString(base, REX_LOAD_U32(manager + 0x1c), sizeof(s.level) - 1);
    std::memcpy(s.level, name.c_str(), name.size() + 1);
    s.level_loaded = REX_LOAD_U32(manager + 0x70) != 0;
    s.level_unloading = REX_LOAD_U8(manager + 0x3d);
  }
  s.frontend_ready = bf3_boot_flow_settled(base);
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
  if (s.soldier && Readable(base, s.soldier + 0x20, 4)) s.health = LoadF(base, s.soldier + 0x20);
  if (s.soldier) {
    ReadWeapon(base, s.soldier, s);
  } else {
    s.ammo_mag = s.ammo_reserve = -1;
  }
  s.kit = PracticeLastKit();
  s.server_player = PracticeServerPlayer();
  if (s.server_player && Readable(base, s.server_player, 0x3B4)) {
    s.team = REX_LOAD_U32(s.server_player + 0x324);
    s.server_soldier = REX_LOAD_U32(s.server_player + 0x3B0);
    if (s.server_soldier && Readable(base, s.server_soldier, 0x40C)) {
      s.server_health = LoadF(base, s.server_soldier + 0x20);
      for (int i = 0; i < 3; ++i) s.server_pos[i] = LoadF(base, s.server_soldier + 0x400 + 4 * i);
    } else {
      s.server_soldier = 0;
    }
  }
  if (const uint32_t pre = PracticePreRoundEntity(base); pre && Readable(base, pre, 0xBC)) {
    s.preround_entity = pre;
    s.round_players_needed = REX_LOAD_U32(pre + 0xA4);
    s.round_countdown = LoadF(base, pre + 0xB4);
    s.round_started = (REX_LOAD_U8(pre + 0xB8) & 0x80) != 0;
  }
  s.paused = Paused();
  s.timescale = Timescale();
  FillCheats(base, s);

  float frame_ms = 0.0f;
  {
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
    if (last != Clock::time_point{}) {
      frame_ms = float(Seconds(now - last) * 1000.0);
      g_frame_ms[g_frame_count++ % g_frame_ms.size()] = frame_ms;
    }
  }
  RecordHistory(s, frame_ms);
  CheatFrame(ctx, base, s, frame_ms);
  const Clock::time_point hold = Clock::now();
  FrameControl(ctx, base);  // may hold here while paused (running queued work)
  // Frame time = start to start (this work included), minus a pause hold.
  last = now + (Clock::now() - hold);
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

// Writes raw (big-endian) bytes into guest memory; false unless the range is committed and writable.
extern "C" __declspec(dllexport) bool bf3_dev_poke(uint32_t addr, const uint8_t* bytes, int length) {
  if (length <= 0 || !Readable(g_base, addr, uint32_t(length))) return false;
  MEMORY_BASIC_INFORMATION mbi;
  if (!VirtualQuery(g_base + addr, &mbi, sizeof(mbi)) ||
      !(mbi.Protect & (PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY))) {
    return false;
  }
  std::memcpy(g_base + addr, bytes, size_t(length));
  return true;
}

// Searches committed guest memory in [lo, hi) for a run of `count` values: kind 0 = f32 (each within `tol`), 1 = u32,
// 2 = bytes; f32/u32 4-byte aligned. Writes up to `capacity` addresses, returns how many were found in total.
extern "C" __declspec(dllexport) int bf3_dev_find(int kind, const double* values, int count, double tol, uint32_t lo,
                                                  uint32_t hi, uint32_t* out, int capacity) {
  return bf3_dev::Find(kind, values, count, tol, lo, hi, out, capacity);
}

int bf3_dev::Find(int kind, const double* values, int count, double tol, uint32_t lo, uint32_t hi, uint32_t* out,
                  int capacity) {
  if (!g_base || count <= 0 || count > 64 || kind < 0 || kind > 2 || (kind != 2 && count > 16)) return 0;
  uint8_t* base = g_base;
  const uint32_t step = kind == 2 ? 1 : 4, span = kind == 2 ? uint32_t(count) : uint32_t(count) * 4;
  uint32_t want_u32[16] = {};
  uint8_t want_u8[64] = {};
  for (int i = 0; i < count; ++i) {
    if (kind == 1) want_u32[i] = uint32_t(int64_t(values[i]));
    if (kind == 2) want_u8[i] = uint8_t(values[i]);
  }
  int found = 0;
  uint64_t addr = lo & ~uint64_t(step - 1);
  while (addr < hi) {
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery(base + addr, &mbi, sizeof(mbi))) break;
    const uint64_t region_end =
        std::min<uint64_t>(hi, uint64_t(static_cast<uint8_t*>(mbi.BaseAddress) - base) + mbi.RegionSize);
    const bool readable = mbi.State == MEM_COMMIT && !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD));
    if (readable) {
      for (; addr + span <= region_end; addr += step) {
        const uint32_t a = uint32_t(addr);
        bool match = true;
        for (int i = 0; i < count && match; ++i) {
          if (kind == 0) {
            const float v = LoadF(base, a + 4 * i);
            match = std::fabs(double(v) - values[i]) <= tol;
          } else if (kind == 1) {
            match = REX_LOAD_U32(a + 4 * i) == want_u32[i];
          } else {
            match = base[a + i] == want_u8[i];
          }
        }
        if (match) {
          if (found < capacity) out[found] = a;
          ++found;
        }
      }
    }
    addr = std::max(addr, region_end);
  }
  return found;
}

// A class's type info by name: its name string in the image (preceded by a NUL), the member record in the type
// registry (0x84000000-0x84300000) that points at it, and the class info whose +0 points at that record. 0 if not found.
static uint32_t FindTypeInfo(uint8_t* base, const std::string& name) {
  std::vector<double> bytes;
  for (char c : name) bytes.push_back(double(uint8_t(c)));
  bytes.push_back(0);
  uint32_t strings[16], records[16], infos[16];
  const int ns = Find(2, bytes.data(), int(bytes.size()), 0, 0x82000000, 0x84300000, strings, 16);
  for (int i = 0; i < std::min(ns, 16); ++i) {
    if (REX_LOAD_U8(strings[i] - 1) != 0) continue;  // the tail of a longer name
    const double str = strings[i];
    const int nr = Find(1, &str, 1, 0, 0x84000000, 0x84300000, records, 16);
    for (int r = 0; r < std::min(nr, 16); ++r) {
      const double rec = records[r];
      if (Find(1, &rec, 1, 0, 0x84000000, 0x84300000, infos, 16) > 0) return infos[0];
    }
  }
  return 0;
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
    if (!bf3_boot_flow_settled(base)) return answer(false, "front end still booting (profile load/save)");
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
  if (cmd == "teleport") {
    float p[3], heading = 0;
    if (args.size() < 4 || !ParseFloat(args[1], p[0]) || !ParseFloat(args[2], p[1]) || !ParseFloat(args[3], p[2]) ||
        (args.size() > 4 && !ParseFloat(args[4], heading))) {
      return answer(false, "usage: teleport <x> <y> <z> [heading radians]");
    }
    const uint32_t player = PracticeServerPlayer();
    const uint32_t soldier = player ? REX_LOAD_U32(player + 0x3B0) : 0;
    if (!soldier) return answer(false, "no server soldier (not spawned)");
    if (args.size() <= 4) {  // keep the current heading: forward row of the client transform
      std::lock_guard lock(g_mutex);
      heading = std::atan2(g_xf[8], g_xf[10]);
    }
    const std::array<float, 3> pos{p[0], p[1], p[2]};
    FlyTeleport(pos.data());
    Queue([soldier, pos, heading](PPCContext& ctx, uint8_t* base) {
      const bool ok = TeleportServer(ctx, base, soldier, pos.data(), heading);
      REXLOG_INFO("debug api: teleport server soldier {:08X} to ({:.2f}, {:.2f}, {:.2f}) heading {:.3f}: {}", soldier,
                  pos[0], pos[1], pos[2], heading, ok ? "done" : "not a server soldier");
    });
    return answer(true, "teleport queued");
  }
  if (cmd == "entities") {
    if (args.size() < 2) return answer(false, "usage: entities <ServerClassName|type info hex> [max=64]");
    uint32_t type = 0;
    if (args[1].rfind("0x", 0) == 0) {
      type = uint32_t(std::strtoul(args[1].c_str() + 2, nullptr, 16));
    } else {
      type = FindTypeInfo(base, args[1]);
      if (!type) return answer(false, "no type info for '" + args[1] + "'");
    }
    const size_t max = args.size() > 2 ? size_t(std::max(1, std::atoi(args[2].c_str()))) : 64;
    auto found = std::make_shared<std::vector<uint32_t>>();
    if (!RunOnMainThread([type, max, found](PPCContext& ctx, uint8_t* b) { *found = ServerEntities(ctx, b, type, max); })) {
      return answer(false, "the main thread didn't run a frame in time");
    }
    std::string list = fmt::format("type 0x{:08X}:", type);
    for (uint32_t e : *found) list += fmt::format(" 0x{:08X}/vt{:08X}", e, REX_LOAD_U32(e));
    return answer(true, list);
  }
  bool ok = false;
  std::string text;
  if (CheatCommand(base, args, ok, text)) return answer(ok, text);
  return answer(false, "unknown game command '" + cmd + "'");
}
