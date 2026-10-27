// Debug menu cheats, game side (development builds only: BF3_DEV_TOOLS). The host menu (src/dev/dev_menu.cpp) and the
// debug API send these as game commands (bf3_dev_request); state goes out in the snapshot (src/dev/dev_shared.h).
//
// God mode: the server soldier's virtual setHealth sub_833E1A48(soldier r3, health f1) (ServerSoldierEntity vtable
//   0x82E325F0 +0xE8) stores the health through sub_83751EB8 and runs the death (sub_8307E138) at <= 0. Every damage
//   path (bullets, falls, explosions) goes through it; for our soldier a lower value is ignored.
// Infinite ammo: sub_8387C5A8(firing + 0x8C) fires one shot for both the client's predicted weapon firing (soldier
//   +0x34C -> +0xCC -> +0x24 -> +0x15C) and the server's (ServerSoldierWeapon +0x2D0) and takes the round from the
//   magazine at firing +0xF0. The magazine is put back afterwards. The client magazine is also restored from the
//   server's state (sub_8387C688), so the server copy is what counts. Applies to every weapon in the local session.
// Fly (noclip): the character physics step sub_830B2628(physics r3, dt f1) of a CharacterPhysicsEntity (client soldier
//   +0x164, server soldier +0x184; vtable 0x82D8DFF0). After each step of ours the physics state is read
//   (sub_830AB9B8(physics, out), 0xE0 bytes: +0x20 position (feet + 0.05), +0x30 and +0x40 velocities), set to the
//   flight position with no velocity and written back (sub_830AEEE0(physics, in), the state restore the client
//   prediction uses; it moves the Havok proxy/phantom properly). The flight position moves on the main thread each
//   frame from the keys (bf3.exe bf3_pc_fly_input) along the render camera. Gravity and collisions never act on it.
// Weapons: the server player keeps the selected loadout inline at +0x4EC + slot * 0x24 {+0 weapon unlock asset,
//   +4..+0xC attachment unlocks, +0x20 attachment count}; slots 0 primary, 1 sidearm, 4 and 5 gadgets, 6 grenade,
//   7 knife. The deploy's NetworkPlayerSelectedWeapon messages fill it and NetworkSpawnOnSelected spawns from it, so
//   the overrides are written when that message reaches the server (dispatcher hook, before its handlers).
//   SoldierWeaponUnlockAsset (type info 0x84174F24): vtable 0x82DBEAA8, name at +8 ("Weapons/AK74M/U_AK74M").
// Network: the local server and client talk through an in-memory socket: sendto sub_8303B078(socket, data r4,
//   length r5) and recvfrom sub_8303A1D0(socket, buffer, capacity, from) -> length r3 (0 = none). The client's socket
//   is used on the main thread, the server's on the server thread. Each queued packet ages at packet +0x6C8; here the
//   latency is measured as send -> receive per direction (the queue is first in, first out).
// Server-side actions (kill, heal, respawn) run on the server thread (ServerTick from the level manager update
//   sub_833AF040, level_boot.cpp): server code called from the main thread faulted (sub_82FE41B8 null read).

#include "bf3_pch.h"

#include "dev_internal.h"
#include "mouse_aim.h"

#include "../dev/dev_shared.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <vector>

REX_EXTERN(__imp__sub_833E1A48);
REX_EXTERN(__imp__sub_8387C5A8);
REX_EXTERN(__imp__sub_830B2628);
REX_EXTERN(__imp__sub_8303B078);
REX_EXTERN(__imp__sub_8303A1D0);
REX_EXTERN(sub_830AB9B8);
REX_EXTERN(sub_830AEEE0);

uint32_t PracticeServerPlayer();        // practice.cpp
void PracticeRequestDeploy(int kit);    // practice.cpp
int PracticeLastKit();                  // practice.cpp
extern "C" bool bf3_dev_snapshot(bf3::dev::Snapshot* out, uint32_t size);  // dev_state.cpp

using namespace bf3_dev;

namespace {

using Clock = std::chrono::steady_clock;

constexpr uint32_t kServerSoldierVtable = 0x82E325F0;
constexpr uint32_t kPhysicsVtable = 0x82D8DFF0;
constexpr uint32_t kWeaponUnlockVtable = 0x82DBEAA8;
constexpr uint32_t kSpawnOnSelectedType = 0xB3DB72C3;  // djb2-xor of NetworkSpawnOnSelectedMessage
constexpr uint32_t kServerDelivery = 0x83C529EC, kClientDelivery = 0x83C51AE0;  // dispatcher lr (level_boot.cpp)
constexpr uint32_t kLoadoutOffset = 0x4EC, kLoadoutStride = 0x24;
constexpr uint32_t kOverrideSlots[4] = {0, 1, 4, 5};
constexpr float kFullHealth = 100.0f;

std::atomic<bool> g_god{false}, g_ammo{false}, g_fly{false};
std::atomic<float> g_fly_speed{10.0f};
std::atomic<uint32_t> g_server_soldier{0}, g_client_phys{0}, g_server_phys{0};
std::atomic<int64_t> g_fall_grace_until_ms{0};  // after a flight ends, falls don't hurt until this time (or landing)
std::atomic<DWORD> g_main_tid{0};

int64_t NowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count();
}

std::mutex g_mutex;  // the flight position, overrides, respawn, server jobs
bool g_fly_ready = false;  // g_fly_pos holds the soldier's position (taken from the first client step in flight)
float g_fly_pos[3] = {};
struct Override {
  uint32_t asset = 0;
  std::string name;
};
std::array<Override, 4> g_override;
struct Respawn {
  bool active = false;
  int kit = 0;
  Clock::time_point started, last_deploy;
} g_respawn;
std::vector<std::function<void(PPCContext&, uint8_t*)>> g_server_jobs;

void ServerJob(std::function<void(PPCContext&, uint8_t*)> fn) {
  std::lock_guard lock(g_mutex);
  g_server_jobs.push_back(std::move(fn));
}

void StoreF(uint8_t* base, uint32_t a, float v) { REX_STORE_U32(a, std::bit_cast<uint32_t>(v)); }

// The physics entity at `offset` of a soldier, 0 unless it is one.
uint32_t Physics(uint8_t* base, uint32_t soldier, uint32_t offset) {
  if (!soldier || !Readable(base, soldier + offset, 4)) return 0;
  const uint32_t phys = REX_LOAD_U32(soldier + offset);
  return phys && Readable(base, phys, 4) && REX_LOAD_U32(phys) == kPhysicsVtable ? phys : 0;
}

// Calls the original setHealth (no god mode) on the server thread.
void SetServerHealth(PPCContext& ctx, uint8_t* base, float health) {
  const uint32_t soldier = g_server_soldier.load();
  if (!soldier || !Readable(base, soldier, 0x30) || REX_LOAD_U32(soldier) != kServerSoldierVtable) return;
  PPCContext saved = ctx;
  ctx.r1.u64 = (saved.r1.u32 - 0x100) & ~0xFu;
  ctx.r3.u64 = soldier;
  ctx.f1.f64 = health;
  __imp__sub_833E1A48(ctx, base);
  ctx = saved;
}

// ---- network ---------------------------------------------------------------------------------------------------------

struct NetCounter {
  uint64_t tx_packets = 0, rx_packets = 0, tx_bytes = 0, rx_bytes = 0;
  double latency_sum_ms = 0, latency_max_ms = 0;
  uint64_t latency_count = 0;
};
std::mutex g_net_mutex;
NetCounter g_net[2];                     // 0 client socket, 1 server socket (this second)
std::deque<Clock::time_point> g_in_flight[2];  // send times of packets on the way to side i
std::atomic<uint64_t> g_msg_up{0}, g_msg_down{0}, g_server_steps{0};
bf3::dev::Snapshot::NetSide g_net_rate[2] = {};
float g_msg_up_rate = 0, g_msg_down_rate = 0, g_server_tick_rate = 0, g_client_tick_rate = 0;

int Side() {
  const DWORD main = g_main_tid.load(std::memory_order_relaxed);
  return main ? (GetCurrentThreadId() == main ? 0 : 1) : -1;
}

void RollNetStats(double seconds, uint64_t client_ticks) {
  static uint64_t last_up = 0, last_down = 0, last_steps = 0, last_ticks = 0;
  std::lock_guard lock(g_net_mutex);
  for (int i = 0; i < 2; ++i) {
    NetCounter& c = g_net[i];
    auto& r = g_net_rate[i];
    r.tx_packets = float(c.tx_packets / seconds);
    r.rx_packets = float(c.rx_packets / seconds);
    r.tx_kbytes = float(c.tx_bytes / 1024.0 / seconds);
    r.rx_kbytes = float(c.rx_bytes / 1024.0 / seconds);
    r.latency_ms = c.latency_count ? float(c.latency_sum_ms / double(c.latency_count)) : 0.0f;
    r.latency_max_ms = float(c.latency_max_ms);
    c = {};
  }
  const uint64_t up = g_msg_up, down = g_msg_down, steps = g_server_steps;
  g_msg_up_rate = float((up - last_up) / seconds);
  g_msg_down_rate = float((down - last_down) / seconds);
  g_server_tick_rate = float((steps - last_steps) / seconds);
  g_client_tick_rate = float((client_ticks - last_ticks) / seconds);
  last_up = up, last_down = down, last_steps = steps, last_ticks = client_ticks;
}

// ---- weapons -------------------------------------------------------------------------------------------------------

struct WeaponUnlock {
  uint32_t asset;
  std::string name;  // Weapons/<dir>/U_<...>
};

// Weapon unlocks loaded in guest memory (the level's and the shared MP ones), one per name.
std::vector<WeaponUnlock> ScanWeapons(uint8_t* base) {
  std::vector<uint32_t> found(4096);
  const double vt = kWeaponUnlockVtable;
  const int n = std::min(Find(1, &vt, 1, 0, 0x40000000, 0x60000000, found.data(), int(found.size())), 4096);
  std::map<std::string, uint32_t> by_name;
  for (int i = 0; i < n; ++i) {
    if (!Readable(base, found[i] + 8, 4)) continue;
    const std::string name = GuestString(base, REX_LOAD_U32(found[i] + 8), 96);
    if (name.rfind("Weapons/", 0) == 0) by_name[name] = found[i];  // the last (highest) copy, as the game's selection
  }
  std::vector<WeaponUnlock> out;
  for (const auto& [name, asset] : by_name) out.push_back({asset, name});
  return out;
}

const std::vector<WeaponUnlock>& Weapons(uint8_t* base) {
  static std::mutex m;
  static std::vector<WeaponUnlock> cache;
  static std::string cache_level;
  std::lock_guard lock(m);
  bf3::dev::Snapshot s{};
  const std::string level = bf3_dev_snapshot(&s, sizeof(s)) ? s.level : "";
  if (cache.empty() || level != cache_level) {
    cache = ScanWeapons(base);
    cache_level = level;
  }
  return cache;
}

// Kind of a weapon unlock for the menu: p primary, s sidearm, g gadget, x grenade/knife.
char WeaponKind(const std::string& name) {
  const size_t slash = name.find('/', 8);
  const std::string dir = slash == std::string::npos ? "" : name.substr(8, slash - 8);
  if (dir == "Gadgets") return 'g';
  for (const char* s : {"M9", "M1911", "MP443", "Glock17", "Glock18", "M93R", "MP412Rex", "Taurus44"}) {
    if (dir == s) return 's';
  }
  for (const char* g : {"FGM148", "RPG7", "SMAW", "Sa18IGLA", "FIM92A"}) {
    if (dir == g) return 'g';
  }
  if (dir == "M67" || dir == "Knife") return 'x';
  return 'p';
}

void ApplyOverrides(uint8_t* base) {
  const uint32_t player = PracticeServerPlayer();
  if (!player || !Readable(base, player + kLoadoutOffset, 8 * kLoadoutStride)) return;
  std::lock_guard lock(g_mutex);
  for (int i = 0; i < 4; ++i) {
    const Override& o = g_override[i];
    if (!o.asset) continue;
    const uint32_t slot = player + kLoadoutOffset + kOverrideSlots[i] * kLoadoutStride;
    REX_STORE_U32(slot, o.asset);
    REX_STORE_U32(slot + 0x20, 0);  // no attachments: the kit's belong to its own weapon
    REXLOG_INFO("dev cheats: loadout slot {} = {} ({:08X})", kOverrideSlots[i], o.name, o.asset);
  }
}

bool Toggle(const std::vector<std::string>& args, std::atomic<bool>& flag) {
  const std::string v = args.size() > 1 ? args[1] : "toggle";
  if (v == "on" || v == "1") {
    flag = true;
  } else if (v == "off" || v == "0") {
    flag = false;
  } else {
    flag = !flag;
  }
  return flag;
}

}  // namespace

// ---- hooks -------------------------------------------------------------------------------------------------------------

extern "C" REX_FUNC(sub_833E1A48) {
  const uint32_t soldier = ctx.r3.u32;
  if (soldier && soldier == g_server_soldier.load(std::memory_order_relaxed)) {
    const float current = LoadF(base, soldier + 0x20);
    const bool grace = g_fly || NowMs() < g_fall_grace_until_ms.load();
    if ((g_god || grace) && ctx.f1.f64 < current) ctx.f1.f64 = current;
  }
  __imp__sub_833E1A48(ctx, base);
}

extern "C" REX_FUNC(sub_8387C5A8) {
  const uint32_t mag = ctx.r3.u32 - 0x8C + 0xF0;
  const bool keep = g_ammo.load(std::memory_order_relaxed) && Readable(base, mag, 4);
  const uint32_t before = keep ? REX_LOAD_U32(mag) : 0;
  __imp__sub_8387C5A8(ctx, base);
  if (keep && REX_LOAD_U32(mag) < before) REX_STORE_U32(mag, before);
}

extern "C" REX_FUNC(sub_830B2628) {
  const uint32_t phys = ctx.r3.u32;
  const bool client = phys && phys == g_client_phys.load(std::memory_order_relaxed);
  const bool server = phys && phys == g_server_phys.load(std::memory_order_relaxed);
  if (server) g_server_steps.fetch_add(1, std::memory_order_relaxed);
  PPCContext saved = ctx;
  __imp__sub_830B2628(ctx, base);
  if ((!client && !server) || !g_fly.load(std::memory_order_relaxed)) return;
  PPCContext after = ctx;
  ctx = saved;
  ctx.r1.u64 = (saved.r1.u32 - 0x300) & ~0xFu;
  const uint32_t state = ctx.r1.u32 + 0x180;  // 0xE0 bytes, 16-aligned
  for (uint32_t i = 0; i < 0xE0; i += 4) REX_STORE_U32(state + i, 0);
  ctx.r3.u64 = phys;
  ctx.r4.u64 = state;
  sub_830AB9B8(ctx, base);
  float pos[3];
  {
    std::lock_guard lock(g_mutex);
    if (!g_fly_ready && client) {
      for (int i = 0; i < 3; ++i) g_fly_pos[i] = LoadF(base, state + 0x20 + 4 * i);
      g_fly_ready = true;
    }
    if (!g_fly_ready) {
      ctx = after;
      return;
    }
    std::copy(std::begin(g_fly_pos), std::end(g_fly_pos), pos);
  }
  for (int i = 0; i < 3; ++i) StoreF(base, state + 0x20 + 4 * i, pos[i]);
  for (uint32_t i = 0x30; i < 0x50; i += 4) REX_STORE_U32(state + i, 0);
  ctx.r3.u64 = phys;
  ctx.r4.u64 = state;
  sub_830AEEE0(ctx, base);
  ctx = after;
}

extern "C" REX_FUNC(sub_8303B078) {
  const int side = Side();
  const uint32_t length = ctx.r5.u32;
  __imp__sub_8303B078(ctx, base);
  if (side < 0 || !ctx.r3.u32) return;
  std::lock_guard lock(g_net_mutex);
  ++g_net[side].tx_packets;
  g_net[side].tx_bytes += length;
  auto& queue = g_in_flight[1 - side];
  if (queue.size() > 512) queue.clear();  // the other side isn't reading (e.g. a level switch)
  queue.push_back(Clock::now());
}

extern "C" REX_FUNC(sub_8303A1D0) {
  const int side = Side();
  __imp__sub_8303A1D0(ctx, base);
  const uint32_t length = ctx.r3.u32;
  if (side < 0 || !length || length > 0x10000) return;
  std::lock_guard lock(g_net_mutex);
  NetCounter& c = g_net[side];
  ++c.rx_packets;
  c.rx_bytes += length;
  auto& queue = g_in_flight[side];
  if (!queue.empty()) {
    const double ms = std::chrono::duration<double, std::milli>(Clock::now() - queue.front()).count();
    queue.pop_front();
    c.latency_sum_ms += ms;
    c.latency_max_ms = std::max(c.latency_max_ms, ms);
    ++c.latency_count;
  }
}

// ---- called from the other dev files ----------------------------------------------------------------------------------

void bf3_dev::CheatOnMessage(uint8_t* base, uint32_t msg, uint32_t lr) {
  if (lr == kServerDelivery) {
    g_msg_up.fetch_add(1, std::memory_order_relaxed);
    if (REX_LOAD_U32(msg + 8) == kSpawnOnSelectedType) ApplyOverrides(base);
  } else if (lr == kClientDelivery) {
    g_msg_down.fetch_add(1, std::memory_order_relaxed);
  }
}

void bf3_dev::ServerTick(PPCContext& ctx, uint8_t* base) {
  std::vector<std::function<void(PPCContext&, uint8_t*)>> jobs;
  {
    std::lock_guard lock(g_mutex);
    jobs.swap(g_server_jobs);
  }
  for (auto& job : jobs) {
    PPCContext saved = ctx;
    job(ctx, base);
    ctx = saved;
  }
}

void bf3_dev::FillCheats(uint8_t* base, bf3::dev::Snapshot& s) {
  if (!g_main_tid.load()) g_main_tid = GetCurrentThreadId();
  g_server_soldier = s.server_soldier;
  g_client_phys = Physics(base, s.soldier, 0x164);
  g_server_phys = Physics(base, s.server_soldier, 0x184);
  s.god = g_god;
  s.infinite_ammo = g_ammo;
  s.flying = g_fly;
  s.fly_speed = g_fly_speed;
  {
    std::lock_guard lock(g_mutex);
    for (int i = 0; i < 4; ++i) {
      std::snprintf(s.weapon_override[i], sizeof(s.weapon_override[i]), "%s", g_override[i].name.c_str());
    }
    s.respawning = g_respawn.active;
  }
  std::lock_guard lock(g_net_mutex);
  s.net_client = g_net_rate[0];
  s.net_server = g_net_rate[1];
  s.msg_up = g_msg_up_rate;
  s.msg_down = g_msg_down_rate;
  s.server_ticks = g_server_tick_rate;
  s.client_ticks = g_client_tick_rate;
}

void bf3_dev::CheatFrame(PPCContext&, uint8_t*, const bf3::dev::Snapshot& s, float) {
  static Clock::time_point last_roll = Clock::now();
  const Clock::time_point now = Clock::now();
  if (const double secs = std::chrono::duration<double>(now - last_roll).count(); secs >= 1.0) {
    RollNetStats(secs, s.ticks);
    last_roll = now;
  }

  // Flight: move along the camera (forward = -row 2) with the keys; up/down along world Y.
  static auto fly_input = reinterpret_cast<bool (*)(float*, bool)>(
      GetProcAddress(GetModuleHandleW(nullptr), "bf3_pc_fly_input"));
  const bool flying = g_fly && s.soldier && s.on_foot;
  float in[4] = {};
  const bool keys = fly_input && fly_input(in, flying);
  static Clock::time_point last_move = now;  // the main loop's own frame time leaves out the dev work (DevFrame)
  const float dt = std::clamp(std::chrono::duration<float>(now - last_move).count(), 0.0f, 0.1f);
  last_move = now;
  if (keys && flying) {
    const float* m = s.camera;
    float f[3] = {-m[8], -m[9], -m[10]};
    const float flen = std::sqrt(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
    if (flen > 1e-4f) {
      for (float& v : f) v /= flen;
      float r[3] = {-f[2], 0.0f, f[0]};  // forward x world up
      const float rlen = std::sqrt(r[0] * r[0] + r[2] * r[2]);
      if (rlen > 1e-4f) r[0] /= rlen, r[2] /= rlen;
      const float speed = g_fly_speed * (in[3] > 0 ? 4.0f : 1.0f) * dt;
      std::lock_guard lock(g_mutex);
      if (g_fly_ready) {
        for (int i = 0; i < 3; ++i) g_fly_pos[i] += (f[i] * in[0] + r[i] * in[1]) * speed;
        g_fly_pos[1] += in[2] * speed;
      }
    }
  }
  if (!g_fly) {
    std::lock_guard lock(g_mutex);
    g_fly_ready = false;
  }

  // Respawn: once the spawn screen is up, deploy (again every 2 s until the soldier is back; 30 s at most).
  std::lock_guard lock(g_mutex);
  if (g_respawn.active) {
    const bool spawn_screen = std::strstr(s.screens, "SpawnScreen") != nullptr;
    if (s.soldier && s.server_soldier && now - g_respawn.started > std::chrono::seconds(2)) {
      g_respawn.active = false;
    } else if (now - g_respawn.started > std::chrono::seconds(30)) {
      REXLOG_WARN("dev cheats: respawn gave up");
      g_respawn.active = false;
    } else if (spawn_screen && !s.soldier && now - g_respawn.last_deploy > std::chrono::seconds(2)) {
      g_respawn.last_deploy = now;
      PracticeRequestDeploy(g_respawn.kit);
    }
  }
}

void bf3_dev::FlyTeleport(const float pos[3]) {
  std::lock_guard lock(g_mutex);
  if (!g_fly_ready) return;
  g_fly_pos[0] = pos[0];
  g_fly_pos[1] = pos[1] + 0.05f;
  g_fly_pos[2] = pos[2];
}

// Game commands for the menu and the debug API. Returns false if `args` isn't one of these.
//   god [on|off|toggle]            no damage for the local soldier
//   ammo [on|off|toggle]           shots don't use up magazines
//   fly [on|off|toggle] [speed]    noclip flight (keys: move, jump up, crouch down, sprint x4)
//   flyspeed <m/s>
//   heal | kill | respawn [kit 0-3]
//   weapon <primary|sidearm|gadget1|gadget2> <unlock name or dir, e.g. M416 | none>   for the next deploy
//   weapons                        the weapon unlocks loaded (kind:name;...)
bool bf3_dev::CheatCommand(uint8_t* base, const std::vector<std::string>& args, bool& ok, std::string& reply) {
  const std::string& cmd = args[0];
  ok = true;
  if (cmd == "god") {
    reply = Toggle(args, g_god) ? "god mode on" : "god mode off";
  } else if (cmd == "ammo") {
    reply = Toggle(args, g_ammo) ? "infinite ammo on" : "infinite ammo off";
  } else if (cmd == "fly") {
    const bool was = g_fly;
    const bool on = Toggle(args, g_fly);
    if (args.size() > 2) g_fly_speed = std::clamp(float(std::atof(args[2].c_str())), 0.5f, 200.0f);
    if (was && !on) g_fall_grace_until_ms = NowMs() + 8000;  // land without fall damage
    reply = on ? fmt::format("flying ({:.0f} m/s)", g_fly_speed.load()) : "flight off";
  } else if (cmd == "flyspeed") {
    if (args.size() < 2) return ok = false, reply = "usage: flyspeed <m/s>", true;
    g_fly_speed = std::clamp(float(std::atof(args[1].c_str())), 0.5f, 200.0f);
    reply = fmt::format("fly speed {:.1f} m/s", g_fly_speed.load());
  } else if (cmd == "heal") {
    if (!g_server_soldier) return ok = false, reply = "no server soldier (not spawned)", true;
    ServerJob([](PPCContext& ctx, uint8_t* b) { SetServerHealth(ctx, b, kFullHealth); });
    reply = "heal queued";
  } else if (cmd == "kill" || cmd == "respawn") {
    const int kit = args.size() > 1 ? std::atoi(args[1].c_str()) : std::max(PracticeLastKit(), 0);
    if (kit < 0 || kit > 3) return ok = false, reply = "kit must be 0-3", true;
    if (g_server_soldier) {
      g_fly = false;
      ServerJob([](PPCContext& ctx, uint8_t* b) { SetServerHealth(ctx, b, 0.0f); });
    }
    if (cmd == "respawn") {
      std::lock_guard lock(g_mutex);
      g_respawn = {true, kit, Clock::now(), {}};
    }
    reply = cmd == "kill" ? "kill queued" : fmt::format("respawn as kit {} queued", kit);
  } else if (cmd == "weapon") {
    static const char* kSlots[] = {"primary", "sidearm", "gadget1", "gadget2"};
    int slot = -1;
    for (int i = 0; i < 4 && args.size() > 1; ++i) {
      if (args[1] == kSlots[i]) slot = i;
    }
    if (slot < 0 || args.size() < 3) {
      return ok = false, reply = "usage: weapon <primary|sidearm|gadget1|gadget2> <name|none>", true;
    }
    Override o;
    if (args[2] != "none") {
      for (const WeaponUnlock& w : Weapons(base)) {
        const size_t slash = w.name.find('/', 8);
        const std::string dir = slash == std::string::npos ? "" : w.name.substr(8, slash - 8);
        if (w.name == args[2] || "U_" + args[2] == w.name.substr(w.name.rfind('/') + 1) ||
            (dir == args[2] && w.name == "Weapons/" + dir + "/U_" + dir)) {
          o = {w.asset, w.name};
          break;
        }
      }
      if (!o.asset) return ok = false, reply = "no weapon unlock '" + args[2] + "' loaded (see: weapons)", true;
    }
    {
      std::lock_guard lock(g_mutex);
      g_override[slot] = o;
    }
    reply = fmt::format("{} = {} (next deploy)", kSlots[slot], o.asset ? o.name : "kit default");
  } else if (cmd == "weapons") {
    std::string list;
    for (const WeaponUnlock& w : Weapons(base)) list += fmt::format("{}:{};", WeaponKind(w.name), w.name);
    reply = list;
  } else {
    return false;
  }
  REXLOG_INFO("dev cheats: {} -> {}", cmd, reply.size() > 120 ? reply.substr(0, 120) + "..." : reply);
  return true;
}

extern "C" __declspec(dllexport) int bf3_dev_weapons(char* out, int capacity) {
  if (!g_base || capacity <= 0) return 0;
  std::string list;
  for (const WeaponUnlock& w : Weapons(g_base)) list += fmt::format("{}:{};", WeaponKind(w.name), w.name);
  const int n = std::min(int(list.size()), capacity - 1);
  std::memcpy(out, list.data(), size_t(n));
  out[n] = 0;
  return n;
}
