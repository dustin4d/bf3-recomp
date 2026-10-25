// Booting straight into a level (BF3_LEVEL, see lua_dump.cpp), plus heap diagnostics.
//
// Server backend: sub_8379E870(type r3, ...) creates a server backend through a factory table at 0x840DFBA4 indexed by
// backend type; sub_836BB1E8 then calls the new object's vtable+12. On Xenon only types 2 and 3 are registered
// (sub_8379E858 calls in bf3_recomp.158), and the front end creates type 2. Booting straight into an MP level selects
// type 1 (no factory on Xenon), which called a null pointer; so does the PRACTICE (dev menu) LoadLevel. Unregistered
// types fall back to 2, the backend the front end runs on.
//
// Main heap: the default allocator 0x84041D50 is a fixed 48 MB region (sub_8301AAxx: VirtualAlloc wrapper
// sub_838D1F28(0, 0x3000000) at 0x8301AAC4, then heap constructor sub_8301A610(heap, vt, type 2, base, size)). An MP
// level loaded without the front end's teardown overflows it (level partitions alone hold ~13 MB), the allocator's
// out-of-memory path returns null and sub_83013BA0 faults writing the block header. BF3_MAIN_HEAP_MB=<n> sizes the
// region (default 160; the game's own 48 with BF3_MAIN_HEAP_MB=48); guest virtual memory has the room.
//
// Level check: sub_83845C30(r3, level setup r4) returns false unless the setup's layer inclusion names an allowed value
// for each category (GameMode: RushLarge0/1, ConquestLarge0/1, ConquestSmall0-3, SquadRush0-3, TeamDeathMatch0-3,
// SquadDeathMatch0-3, TutorialMP, TutorialMP_S); false makes the server start (sub_833AFD20) set the quit flag
// 0x84091C34 and the main loop exits.

// Compiled into the bf3_BF_Main DLL (see CMakeLists.txt): bf3_pch.h is generated/bf_main/bf3_pch.h.
#include "bf3_pch.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <cstdlib>
#include <mutex>
#include <cctype>
#include <string>
#include <unordered_map>
#include <vector>

REX_EXTERN(__imp__sub_8379E870);
REX_EXTERN(__imp__sub_838D1F28);
REX_EXTERN(__imp__sub_8301A610);
REX_EXTERN(__imp__sub_8301A290);
REX_EXTERN(__imp__sub_8301A330);
REX_EXTERN(__imp__sub_83013BA0);
REX_EXTERN(__imp__sub_833AFD20);
REX_EXTERN(__imp__sub_82FE8DC8);
REX_EXTERN(__imp__sub_831D43E0);
REX_EXTERN(__imp__sub_831C7240);
REX_EXTERN(__imp__sub_831C7F98);
REX_EXTERN(__imp__sub_83027490);
REX_EXTERN(__imp__sub_83013DA8);
REX_EXTERN(__imp__sub_83034E28);
REX_EXTERN(__imp__sub_833B1500);
REX_EXTERN(__imp__sub_8339DF30);
REX_EXTERN(__imp__sub_833B0750);
REX_EXTERN(__imp__sub_833AF040);

namespace {
constexpr uint32_t kFactoryTable = 0x840DFBA4;
constexpr uint32_t kLocalBackend = 2;
constexpr uint32_t kMainHeap = 0x84041D50;
constexpr uint32_t kMainHeapSize = 0x3000000;
constexpr uint32_t kMainHeapAllocReturn = 0x8301AAC8;

bool LevelBoot() {
  static const bool on = [] {
    const char* env = std::getenv("BF3_LEVEL");
    return env && *env;
  }();
  return on;
}

bool MsgTrace() {
  static const bool on = [] {
    const char* env = std::getenv("BF3_MSG_TRACE");
    return env && *env == '1';
  }();
  return on;
}

bool HeapTrace() {
  static const bool on = [] {
    const char* env = std::getenv("BF3_HEAP_TRACE");
    return env && *env == '1';
  }();
  return on;
}

uint32_t MainHeapSize() {
  static const uint32_t size = [] {
    const char* env = std::getenv("BF3_MAIN_HEAP_MB");
    uint32_t mb = env && *env ? uint32_t(std::atoi(env)) : 160;
    return std::clamp<uint32_t>(mb, 48, 512) << 20;
  }();
  return size;
}

uint32_t g_main_heap_base = 0;
std::atomic<uint64_t> g_entered_ingame_ms{0};

// A level switch waiting for the level manager to be idle: "<level>[:<category>=<value>]".
std::mutex g_request_mutex;
std::string g_request;

void RequestLevel(const std::string& spec) {
  std::lock_guard lock(g_request_mutex);
  g_request = spec;
}

uint64_t NowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
      .count();
}
}  // namespace

extern "C" REX_FUNC(sub_8379E870) {
  static const int forced = [] {
    const char* env = std::getenv("BF3_BACKEND");
    return env && *env ? std::atoi(env) : -1;
  }();
  if (LevelBoot() && forced >= 0) {
    REXLOG_INFO("server backend: type {} forced to {}", ctx.r3.u32, forced);
    ctx.r3.u64 = uint32_t(forced);
  }
  const uint32_t type = ctx.r3.u32;
  // Always (the local online stack has no Blaze): the dev menu's LoadLevel starts MP levels the same way.
  if (type < 4 && !REX_LOAD_U32(kFactoryTable + type * 4) &&
      REX_LOAD_U32(kFactoryTable + kLocalBackend * 4)) {
    REXLOG_WARN("server backend: type {} has no factory, using {}", type, kLocalBackend);
    ctx.r3.u64 = kLocalBackend;
  }
  __imp__sub_8379E870(ctx, base);
}

// VirtualAlloc wrapper: enlarge the main heap's region.
extern "C" REX_FUNC(sub_838D1F28) {
  const bool main_heap = static_cast<uint32_t>(ctx.lr) == kMainHeapAllocReturn && ctx.r4.u32 == kMainHeapSize;
  if (main_heap) ctx.r4.u64 = MainHeapSize();
  __imp__sub_838D1F28(ctx, base);
  if (main_heap) {
    g_main_heap_base = ctx.r3.u32;
    REXLOG_INFO("main heap: {} MB at {:08X}", MainHeapSize() >> 20, ctx.r3.u32);
  }
}

// Heap constructor (heap r3, vtable r4, type r5, region r6, size r7): the main heap's size follows its region.
extern "C" REX_FUNC(sub_8301A610) {
  if (LevelBoot() && ctx.r7.u32 >= (16u << 20)) {
    REXLOG_INFO("heap ctor: heap {:08X} type {} region {:08X} size {:X}", ctx.r3.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32);
  }
  if (g_main_heap_base && ctx.r6.u32 == g_main_heap_base && ctx.r7.u32 == kMainHeapSize) {
    ctx.r7.u64 = MainHeapSize();
  }
  __imp__sub_8301A610(ctx, base);
}

// Diagnostic (BF3_HEAP_TRACE=1): live-allocation tracker for the default 48 MB heap (0x84041D50, created by
// sub_8301A610 type 2): sub_8301A290(heap r3, size r4, align r5) allocates, sub_8301A330(heap r3, ptr r4) frees. Each
// allocation is keyed by its guest call chain (saved LRs at back-chain - 8); whenever live bytes cross another 4 MB,
// the biggest holders are logged.

namespace {
struct LiveAlloc {
  uint32_t size;
  uint64_t key;
};
std::mutex g_live_mutex;
std::unordered_map<uint32_t, LiveAlloc> g_live;
std::unordered_map<uint64_t, uint64_t> g_live_by_key;
uint64_t g_live_bytes = 0, g_live_logged = 0;

uint64_t ChainKey(PPCContext& ctx, uint8_t* base) {
  uint32_t b = 0, c = 0, sp = ctx.r1.u32;
  for (int depth = 0; depth < 3 && sp >= 0x40000000 && sp < 0x80000000; ++depth) {
    uint32_t prev = REX_LOAD_U32(sp);
    if (prev <= sp || prev - sp > 0x10000) break;
    uint32_t ret = REX_LOAD_U32(prev - 8);
    if (depth == 1) b = ret;
    if (depth == 2) c = ret;
    sp = prev;
  }
  return (uint64_t(b) << 32) | c;
}

void LogTop() {
  std::vector<std::pair<uint64_t, uint64_t>> top(g_live_by_key.begin(), g_live_by_key.end());
  std::sort(top.begin(), top.end(), [](auto& x, auto& y) { return x.second > y.second; });
  REXLOG_INFO("heap live: {} KB in {} blocks", g_live_bytes >> 10, g_live.size());
  for (size_t i = 0; i < top.size() && i < 10; ++i) {
    REXLOG_INFO("heap live:   {:6} KB  callers {:08X} {:08X}", top[i].second >> 10, uint32_t(top[i].first >> 32),
                uint32_t(top[i].first));
  }
}
}  // namespace

extern "C" REX_FUNC(sub_8301A290) {
  if (!HeapTrace() || ctx.r3.u32 != kMainHeap) return __imp__sub_8301A290(ctx, base);
  const uint32_t size = ctx.r4.u32;
  const uint64_t key = ChainKey(ctx, base);
  __imp__sub_8301A290(ctx, base);
  const uint32_t ptr = ctx.r3.u32;
  std::lock_guard lock(g_live_mutex);
  if (!ptr) {
    REXLOG_WARN("heap: alloc {:X} returned null", size);
    LogTop();
    return;
  }
  g_live[ptr] = {size, key};
  g_live_by_key[key] += size;
  g_live_bytes += size;
  if (g_live_bytes >= g_live_logged + (4u << 20)) {
    g_live_logged = g_live_bytes;
    LogTop();
  }
}

extern "C" REX_FUNC(sub_8301A330) {
  if (HeapTrace() && ctx.r3.u32 == kMainHeap && ctx.r4.u32) {
    std::lock_guard lock(g_live_mutex);
    if (auto it = g_live.find(ctx.r4.u32); it != g_live.end()) {
      g_live_by_key[it->second.key] -= it->second.size;
      g_live_bytes -= it->second.size;
      g_live.erase(it);
      if (g_live_logged > g_live_bytes + (4u << 20)) g_live_logged = g_live_bytes;
    }
  }
  __imp__sub_8301A330(ctx, base);
}

// Diagnostic (BF3_HEAP_TRACE=1): sub_83013BA0(allocator r3, size r4, align r5) writes a block header unless the
// allocator's flags (+4) bit 0 is set; log each allocator taking that path, and null results.
extern "C" REX_FUNC(sub_83013BA0) {
  if (!HeapTrace()) return __imp__sub_83013BA0(ctx, base);
  const uint32_t alloc = ctx.r3.u32, size = ctx.r4.u32;
  const uint32_t lr = static_cast<uint32_t>(ctx.lr);
  if (!(REX_LOAD_U32(alloc + 4) & 1)) {
    static std::mutex m;
    static std::vector<uint32_t> seen;
    std::lock_guard lock(m);
    if (std::find(seen.begin(), seen.end(), alloc) == seen.end()) {
      seen.push_back(alloc);
      uint32_t vt = REX_LOAD_U32(alloc);
      REXLOG_INFO("header alloc: allocator {:08X} vt {:08X} alloc fn {:08X} (first size {:X} from {:08X})", alloc, vt,
                  REX_LOAD_U32(vt + 4), size, lr);
    }
  }
  __imp__sub_83013BA0(ctx, base);
}

// Server start (see the level check above): log when it raises the quit flag.
extern "C" REX_FUNC(sub_833AFD20) {
  const uint32_t lr = static_cast<uint32_t>(ctx.lr);
  __imp__sub_833AFD20(ctx, base);
  if (LevelBoot()) REXLOG_INFO("server start from {:08X}: quit flag {}", lr, REX_LOAD_U32(0x84091C34));
}

// Main app object (sub_82FE9E18's loop calls its vtable+16 each frame until it returns false).
extern "C" REX_FUNC(sub_82FE8DC8) {
  __imp__sub_82FE8DC8(ctx, base);
  uint32_t app = ctx.r3.u32, vt = app ? REX_LOAD_U32(app) : 0;
  if (LevelBoot()) REXLOG_INFO("app {:08X} vtable {:08X} update {:08X}", app, vt, vt ? REX_LOAD_U32(vt + 16) : 0);
}

extern "C" REX_FUNC(sub_83013DA8) {
  if (LevelBoot()) REXLOG_WARN("memory shutdown (sub_83013DA8) from {:08X}", uint32_t(ctx.lr));
  __imp__sub_83013DA8(ctx, base);
}
extern "C" REX_FUNC(sub_83034E28) {
  if (LevelBoot()) REXLOG_WARN("main loop ended (sub_83034E28) from {:08X}", uint32_t(ctx.lr));
  __imp__sub_83034E28(ctx, base);
}

// Client message handler (client r3, message r4: +4 category hash, +8 type hash). CoreQuitMessage (category "Core"
// 0x7C813E9E, type 0xE913E8AC) sets client+473, which makes the client update return false and the game exit.
extern "C" REX_FUNC(sub_831D43E0) {
  const uint32_t msg = ctx.r4.u32;
  if (MsgTrace()) REXLOG_INFO("client msg {:08X}/{:08X}", REX_LOAD_U32(msg + 4), REX_LOAD_U32(msg + 8));
  if (REX_LOAD_U32(msg + 4) == 0x7C813E9E && REX_LOAD_U32(msg + 8) == 0x68AFE2B5) {  // CoreEnteredIngameMessage
    g_entered_ingame_ms = NowMs();
  }
  if (LevelBoot() && REX_LOAD_U32(msg + 4) == 0x7C813E9E && REX_LOAD_U32(msg + 8) == 0xE913E8AC) {
    REXLOG_WARN("client: CoreQuitMessage ignored (msg {:08X}, lr {:08X})", msg, uint32_t(ctx.lr));
    return;
  }
  __imp__sub_831D43E0(ctx, base);
}

// Frame update pieces (sub_82FE41F0): any false return ends the main loop.
extern "C" REX_FUNC(sub_831C7240) {
  __imp__sub_831C7240(ctx, base);
  static int n = 0;
  if (LevelBoot() && !(ctx.r3.u32 & 0xFF) && n++ < 3) REXLOG_WARN("frame: sub_831C7240 returned false");
}
extern "C" REX_FUNC(sub_831C7F98) {
  const uint32_t client = ctx.r3.u32;
  __imp__sub_831C7F98(ctx, base);
  static int n = 0;
  if (LevelBoot() && !(ctx.r3.u32 & 0xFF) && n++ < 3) REXLOG_WARN("frame: client {:08X} update returned false", client);
}
extern "C" REX_FUNC(sub_83027490) {
  __imp__sub_83027490(ctx, base);
  static bool logged = false;
  if (LevelBoot() && !logged && ctx.r3.u32) {
    logged = true;
    uint32_t vt = REX_LOAD_U32(ctx.r3.u32);
    REXLOG_INFO("frame: platform {:08X} vt {:08X} update {:08X}", ctx.r3.u32, vt, REX_LOAD_U32(vt + 20));
  }
}

// Server level-load state machine message handler (server r3, message r4: +8 type hash; load-level name at +0x28).
// Server flags: +0x3c abort, +0x3d unloading, +0x65 unload requested, +0x66 unload after load, +0x70 current level.
extern "C" REX_FUNC(sub_833B1500) {
  const uint32_t server = ctx.r3.u32, msg = ctx.r4.u32, type = REX_LOAD_U32(msg + 8);
  auto flags = [&] {
    return fmt::format("3c={} 3d={} 65={} 66={} level={:08X}", REX_LOAD_U8(server + 0x3c), REX_LOAD_U8(server + 0x3d),
                       REX_LOAD_U8(server + 0x65), REX_LOAD_U8(server + 0x66), REX_LOAD_U32(server + 0x70));
  };
  std::string before = flags(), name;
  if (type == 0xF8429A0E) {
    uint32_t p = REX_LOAD_U32(msg + 0x28);
    if (p) name = reinterpret_cast<const char*>(REX_RAW_ADDR(p));
  }
  __imp__sub_833B1500(ctx, base);
  // ServerInternalLoadLevelMessage (the startup Game.Level load) marks the load internal (+0x3c); the client's join
  // normally clears it, otherwise the server releases and unloads the level once loaded (sub_833AE488).
  // BF3_NORMAL_LOAD=1 clears it right away (keeps the level without a client).
  static const bool normal_load = [] {
    const char* env = std::getenv("BF3_NORMAL_LOAD");
    return env && *env == '1';
  }();
  if (LevelBoot() && normal_load && type == 0x11FF6EFC && REX_LOAD_U8(server + 0x3c)) {
    REX_STORE_U8(server + 0x3c, 0);
    REXLOG_INFO("server: startup level load runs as a normal load");
  }
  if (LevelBoot() || MsgTrace()) REXLOG_INFO("server msg {:08X} '{}': {} -> {}", type, name, before, flags());
}

// Server stop ("Server shutdown"): marks the level for unloading after it loads (+0x66). Reached from the app's
// stopServer (sub_82FE4350, app vtable+0x34 and a script binding at 0x82F466D8), which the UI scripts call during
// startup as if coming from the front end. A level boot keeps its server.
extern "C" REX_FUNC(sub_8339DF30) {
  if (LevelBoot()) {
    REXLOG_WARN("server stop from {:08X} ignored (level boot)", uint32_t(ctx.lr));
    return;
  }
  __imp__sub_8339DF30(ctx, base);
}

// Server construction (sub_833B0750(r3, spawn info r4, r5)). Spawn info bytes: +0x10/+0x11 select the Local backend
// (type 3), +0x14 a networked multiplayer server (backend OnlineSettings+0x10, type 1: no factory on Xenon), otherwise
// OnlineSettings+0xC (type 2) as the front end uses. BF3_LOCAL_SERVER=1 (default for level boots) clears +0x14 so the
// level runs on the same in-process server as the front end.
extern "C" REX_FUNC(sub_833B0750) {
  static const bool local = [] {
    const char* env = std::getenv("BF3_LOCAL_SERVER");
    return env && *env ? *env == '1' : LevelBoot();
  }();
  const uint32_t info = ctx.r4.u32;
  if (LevelBoot()) {
    REXLOG_INFO("server create: info +10 {} +11 {} +12 {} +14 {}", REX_LOAD_U8(info + 0x10), REX_LOAD_U8(info + 0x11),
                REX_LOAD_U8(info + 0x12), REX_LOAD_U8(info + 0x14));
    if (local) REX_STORE_U8(info + 0x14, 0);
  }
  __imp__sub_833B0750(ctx, base);
}

// Level switch from the front end (BF3_LEVEL without BF3_LEVEL_DIRECT): once the client has entered the front end
// level (CoreEnteredIngameMessage) and the server is idle, hand the server's level manager a ServerLoadLevelMessage
// (type F8429A0E) naming the level, as the campaign menu's level change does. The manager then unloads the front end
// and loads the level, and the connected client follows.
//
// Message: +8 type hash, +0x28 LevelSetup {+0 char* name, +4 inclusion options (array, count at -4) of {char*
// category, char* value}, +8 u32, +12 u32 array}, +0x3d bool. Strings are plain char*; the manager copies all of it.
// Level manager (sub_833AF040's r3, sub_833B1500's r3): +0x1c current LevelSetup, +0x3d unloading, +0x65 unload
// requested, +0x70 loaded level.
void FadeWatchdogTick(PPCContext& ctx, uint8_t* base);  // fade_fix.cpp

extern "C" REX_FUNC(sub_833AF040) {
  FadeWatchdogTick(ctx, base);
  static bool boot_requested = false;
  static const bool direct = [] {
    const char* env = std::getenv("BF3_LEVEL_DIRECT");
    return env && *env == '1';
  }();
  const uint32_t manager = ctx.r3.u32;
  const uint64_t entered = g_entered_ingame_ms.load();
  if (LevelBoot() && !direct && !boot_requested && entered && NowMs() - entered > 3000) {
    boot_requested = true;
    RequestLevel(std::getenv("BF3_LEVEL"));
  }
  std::string request;
  if (REX_LOAD_U32(manager + 0x70) && !REX_LOAD_U8(manager + 0x3d) && !REX_LOAD_U8(manager + 0x65)) {
    std::lock_guard lock(g_request_mutex);
    request.swap(g_request);
  }
  if (!request.empty()) {
    std::string level = request, layers = "GameMode=TeamDeathMatch0";
    if (size_t colon = level.find(':'); colon != std::string::npos) {
      layers = level.substr(colon + 1);
      level.resize(colon);
    }
    if (level.find('/') == std::string::npos) level = "Levels/" + level + "/" + level;
    std::string category = "GameMode", value = layers;
    if (size_t eq = layers.find('='); eq != std::string::npos) {
      category = layers.substr(0, eq);
      value = layers.substr(eq + 1);
    }
    // Guest memory for the message, from the allocator the server uses for messages.
    PPCContext saved = ctx;
    ctx.r3.u64 = 0x84042270;
    ctx.r4.u64 = 0x200;
    ctx.r5.u64 = 16;
    sub_83013BA0(ctx, base);
    const uint32_t msg = ctx.r3.u32;
    ctx = saved;
    if (!msg) {
      REXLOG_WARN("level switch: no memory for the message");
    } else {
      std::memset(REX_RAW_ADDR(msg), 0, 0x200);
      auto put = [&](uint32_t at, const std::string& text) {
        std::memcpy(REX_RAW_ADDR(msg + at), text.c_str(), std::min<size_t>(text.size() + 1, 0x7F));
        return msg + at;
      };
      REX_STORE_U32(msg + 0x08, 0xF8429A0E);
      REX_STORE_U32(msg + 0x28, put(0x100, level));
      REX_STORE_U32(msg + 0x50, 1);  // option count
      REX_STORE_U32(msg + 0x2C, msg + 0x54);
      REX_STORE_U32(msg + 0x54, put(0x180, category));
      REX_STORE_U32(msg + 0x58, put(0x1A0, value));
      REX_STORE_U32(msg + 0x30, REX_LOAD_U32(manager + 0x1c + 8));
      REX_STORE_U32(msg + 0x60, 0);  // u32 array count
      REX_STORE_U32(msg + 0x34, msg + 0x64);
      REXLOG_INFO("level switch: loading {} ({}={}) from the front end", level, category, value);
      ctx.r3.u64 = manager;
      ctx.r4.u64 = msg;
      sub_833B1500(ctx, base);
      ctx = saved;
    }
  }
  __imp__sub_833AF040(ctx, base);
}

// UI action LoadLevel: the action table at 0x84016150 maps its key (0xE9F0E7F5) to a factory (sub_836DE440) for an
// action object whose execute is sub_836EE268(action r3, params r4: **params = char*[] {level, optional mode}); it
// hands them to sub_837E29A0, which stops the server and starts a new one on the level. The PRACTICE menu (the dev
// screen, see main_menu.cpp) uses it. The client can't connect while that new server loads (its connection drops
// after ~7 s and the game returns to the menu), so multiplayer levels are switched in place on the running server
// instead, as BF3_LEVEL does, in Team Deathmatch; other levels take the engine's path.
REX_EXTERN(__imp__sub_836EE268);

extern "C" REX_FUNC(sub_836EE268) {
  std::string level;
  if (const uint32_t params = ctx.r4.u32) {
    const uint32_t array = REX_LOAD_U32(REX_LOAD_U32(params));
    if (array && REX_LOAD_U32(array - 4) > 0) {
      if (const uint32_t name = REX_LOAD_U32(array)) level = reinterpret_cast<const char*>(REX_RAW_ADDR(name));
    }
  }
  std::string lower = level;
  for (char& c : lower) c = char(std::tolower(static_cast<unsigned char>(c)));
  if (lower.find("levels/mp_") != std::string::npos || lower.rfind("mp_", 0) == 0) {
    REXLOG_INFO("practice: {} (switched in place on the local server)", level);
    RequestLevel(level);
    return;
  }
  REXLOG_INFO("practice: '{}' (engine load)", level);
  __imp__sub_836EE268(ctx, base);
}

// Diagnostic (BF3_MSG_TRACE=1): UI actions reaching the dispatcher sub_834131A0(handler r3, action r4: +4 key).
REX_EXTERN(__imp__sub_834131A0);
extern "C" REX_FUNC(sub_834131A0) {
  if (MsgTrace() && ctx.r4.u32) {
    REXLOG_INFO("ui action {:08X} flags {:04X}", REX_LOAD_U32(ctx.r4.u32 + 4), REX_LOAD_U16(ctx.r3.u32 + 8));
  }
  __imp__sub_834131A0(ctx, base);
}

// Diagnostic: GPU fence wait (sub_82FCBE68(device r3, fence r4, ...) loops on sub_82FCC8D8 until the completed fence
// **(device+0x2B10) passes the target; device+0x2B1C is the last fence issued). Logs every 2 s while waiting.
REX_EXTERN(__imp__sub_82FCC8D8);
extern "C" REX_FUNC(sub_82FCC8D8) {
  static std::atomic<uint64_t> last{0};
  const uint64_t now = NowMs();
  if (now - last.load() > 2000) {
    last = now;
    const uint32_t state = ctx.r3.u32, device = REX_LOAD_U32(state);
    const uint32_t fence_ptr = REX_LOAD_U32(device + 0x2B10);
    REXLOG_INFO("gpu fence wait: device {:08X} issued {:08X} completed {:08X} (at {:08X}) state[1..3] {:08X} {:08X} {:08X}",
                device, REX_LOAD_U32(device + 0x2B1C), fence_ptr ? REX_LOAD_U32(fence_ptr) : 0, fence_ptr,
                REX_LOAD_U32(state + 4), REX_LOAD_U32(state + 8), REX_LOAD_U32(state + 12));
  }
  __imp__sub_82FCC8D8(ctx, base);
}

// Diagnostic (BF3_MSG_TRACE=1): main loop rate (app update sub_82FE7898, once per frame).
REX_EXTERN(__imp__sub_82FE7898);
extern "C" REX_FUNC(sub_82FE7898) {
  static uint64_t window_start = 0;
  static int frames = 0;
  if (MsgTrace()) {
    const uint64_t now = NowMs();
    if (!window_start) window_start = now;
    ++frames;
    if (now - window_start >= 2000) {
      REXLOG_INFO("main loop: {:.1f} frames/s", frames * 1000.0 / double(now - window_start));
      frames = 0;
      window_start = now;
    }
  }
  __imp__sub_82FE7898(ctx, base);
}
