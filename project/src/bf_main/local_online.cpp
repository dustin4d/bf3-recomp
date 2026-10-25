// Local online stack: the game's online sections run against this machine instead of Xbox LIVE and EA's servers
// (Blaze, long gone). For now the stack is empty: it reports the player online with a local profile and stats
// available, and every network service behind that (matchmaking, stats, friends) has nothing to offer yet.
//
// The front end decides online-or-not from UI data read through sub_83294100 (bf_main/ui_mouse.cpp hooks it):
// - 0x9E2114F5: online state, published by sub_837F0090 from the online manager (0x840D63D0 +0x20: 0 "Offline",
//   1 "Connecting", 2 "Online"). The flow's CheckOnline (ui/flow/graph/frontend/frontendroot) sends MULTIPLAYER,
//   CO-OP and MY SOLDIER through Xbox LIVE sign-in and an EA Online login (GoOnlineLoadingScreen) unless it is
//   "Online". Answered "Online".
// - 0x938E1FD9: HasStats (ui/flow/graph/frontend/mysoldiergraph: RequestStats, then HasStats picks MySoldierScreen
//   or the "Failed to retrieve stats" dialog). Answered true.
// - The player's name: without an online persona the game names the player after the build's machine name
//   ("xenon-retail", sub_830271A0) in sub_83704EB0 (player name getter); there it gets the soldier name.
// The local profile (soldier name) is set up on first use by bf3.exe (src/local_profile.cpp), which polls
// bf3_local_online_profile_needed and supplies the name through bf3_local_online_set_profile.
// BF3_LOCAL_ONLINE=0 turns the stack off (the console behaviour: "sign in to Xbox LIVE").

// Compiled into the bf3_BF_Main DLL (see CMakeLists.txt): bf3_pch.h is generated/bf_main/bf3_pch.h.
#include "bf3_pch.h"

#include <atomic>
#include <cstring>
#include <mutex>
#include <string>
#include <algorithm>
#include <cstdlib>

REX_EXTERN(__imp__sub_8328D658);  // UI data value: set string (value r3, const char* r4)
REX_EXTERN(__imp__sub_830271A0);  // machine name ("xenon-retail"), the player name without an online persona
REX_EXTERN(sub_83013BA0);         // allocator (allocator r3, size r4, align r5)

namespace {

constexpr uint32_t kOnlineStateKey = 0x9E2114F5;
constexpr uint32_t kHasStatsKey = 0x938E1FD9;
constexpr uint32_t kOnlineString = 0x82DBF408;  // "Online" in BF.Main's data

std::atomic<bool> g_have_profile{false};
std::mutex g_name_mutex;
std::string g_name;          // soldier name from bf3.exe
uint32_t g_name_buffer = 0;  // guest copy of it
std::atomic<bool> g_profile_needed{false};

bool Enabled() {
  static const bool on = [] {
    const char* env = std::getenv("BF3_LOCAL_ONLINE");
    return !(env && *env == '0');
  }();
  return on;
}

}  // namespace

// bf3.exe: true while an online section wants a profile and none is set.
extern "C" __declspec(dllexport) int bf3_local_online_profile_needed() {
  return g_profile_needed.load() && !g_have_profile.load();
}

// bf3.exe: the local profile's soldier name (loaded or just created).
extern "C" __declspec(dllexport) void bf3_local_online_set_profile(const char* name) {
  {
    std::lock_guard lock(g_name_mutex);
    g_name = name ? name : "";
  }
  g_have_profile = true;
  g_profile_needed = false;
  REXLOG_INFO("local online: profile ready");
}

// From the flow data read hook: key r4, value** out (value: +8 payload, +16 type; 6 string, 2 bool).
void bf3_local_online_data_read(PPCContext& ctx, uint8_t* base, uint32_t key, uint32_t out) {
  if (!Enabled() || !out) return;
  const uint32_t value = REX_LOAD_U32(out);
  if (!value) return;
  if (key == kOnlineStateKey) {
    if (!g_have_profile) g_profile_needed = true;
    PPCContext keep = ctx;
    ctx.r3.u64 = value;
    ctx.r4.u64 = kOnlineString;
    __imp__sub_8328D658(ctx, base);
    ctx = keep;
  } else if (key == kHasStatsKey) {
    REX_STORE_U32(value + 16, 2);
    REX_STORE_U32(value + 8, 0x01000000);  // bool true (first byte)
  }
}

extern "C" REX_FUNC(sub_830271A0) {
  const uint32_t lr = static_cast<uint32_t>(ctx.lr);
  __imp__sub_830271A0(ctx, base);
  if (!Enabled() || lr < 0x83704EB0 || lr >= 0x83704F00) return;  // only the player name getter sub_83704EB0
  std::string name;
  {
    std::lock_guard lock(g_name_mutex);
    name = g_name;
  }
  if (name.empty()) return;
  if (!g_name_buffer) {
    PPCContext keep = ctx;
    ctx.r3.u64 = 0x84042270;
    ctx.r4.u64 = 32;
    ctx.r5.u64 = 16;
    sub_83013BA0(ctx, base);
    g_name_buffer = ctx.r3.u32;
    ctx = keep;
    if (!g_name_buffer) return;
  }
  std::memset(REX_RAW_ADDR(g_name_buffer), 0, 32);
  std::memcpy(REX_RAW_ADDR(g_name_buffer), name.data(), std::min<size_t>(name.size(), 31));
  ctx.r3.u64 = g_name_buffer;
}
