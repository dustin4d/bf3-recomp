// PRACTICE mode helpers (multiplayer levels on the local server).
//
// Deploy: the spawn screen's DEPLOY button runs the UI action sub_836EE788(action, params: char*[] {kit index,
// optional "auto team" flag}), which gets the spawn system from the UI manager (sub_83280730() -> vtable +0xD0
// (manager, 0x84197820, 1)) and calls sub_837E0C90(system r3, kit r4, flag r5). With flag 0 and no team yet it
// first sends NetworkSelectTeamMessage (team from *(0x840B6658)+0x2C), then the kit customization and the spawn
// request. After the in-place level switch the spawn screen's ActionScript never fires the action (team neutral,
// kit "undefined"), so PracticeDeploy does the same from the host.
//
// Team: the server's network message handler sub_836C7AA0 ignores NetworkSelectTeamMessage (teams come from the
// Blaze game on retail consoles), so a player who joins without Blaze stays on TeamNeutral and can't spawn. The hook
// applies the requested team (message +0x38) with ServerPlayer::setTeam sub_833CACD0(player, team; team at +0x324).
// The player comes from the message the way the handler gets it: sub_8342C678(sub_8358A788(message)).
//
// BF3_AUTO_DEPLOY=<kit 0-3> deploys automatically 8 s after the spawn screen appears (tests).

#include "bf3_pch.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <string>

REX_EXTERN(sub_83280730);
REX_EXTERN(sub_837E0C90);
REX_EXTERN(sub_8358A788);
REX_EXTERN(sub_8342C678);
REX_EXTERN(sub_833CACD0);
REX_EXTERN(__imp__sub_836C7AA0);

namespace {

constexpr uint32_t kSpawnSystemType = 0x84197820;
constexpr uint32_t kTeamPreference = 0x840B6658;  // pointer; +0x2C team the deploy action selects

uint64_t NowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

std::atomic<int> g_deploy_kit{-1};       // pending host deploy request (kit index), -1 = none
std::atomic<uint64_t> g_spawn_screen_ms{0};
std::atomic<uint32_t> g_entered_player{0};  // ServerPlayer that last entered a level
std::atomic<bool> g_team_pending{false};

uint32_t PreferredTeam() {
  static const uint32_t team = [] {
    const char* v = std::getenv("BF3_TEAM");
    return v && *v == '2' ? 2u : 1u;
  }();
  return team;
}

void SetTeam(PPCContext& ctx, uint8_t* base, uint32_t player, uint32_t team) {
  REXLOG_INFO("practice: player {:08X} team {} -> {}", player, REX_LOAD_U32(player + 0x324), team);
  PPCContext saved = ctx;
  ctx.r3.u64 = player;
  ctx.r4.u64 = team;
  sub_833CACD0(ctx, base);
  ctx = saved;
}

}  // namespace

// Called by ui_screens.cpp when a screen is pushed.
void PracticeScreenPushed(const std::string& name) {
  if (name == "SpawnScreen") {
    g_spawn_screen_ms = NowMs();
    g_team_pending = true;
  }
}

// Called by the message dispatcher hook (level_boot.cpp) for every message.
void PracticeOnMessage(uint8_t* base, uint32_t msg) {
  if (REX_LOAD_U32(msg + 4) == 0xF3C93DD3 && REX_LOAD_U32(msg + 8) == 0x09B7EA47) {  // ServerPlayerEnteredLevel
    g_entered_player = REX_LOAD_U32(msg + 0x28);
  }
}

void PracticeRequestDeploy(int kit) { g_deploy_kit = kit; }

// Main thread, every frame (level manager update, see level_boot.cpp).
void PracticeTick(PPCContext& ctx, uint8_t* base) {
  static const int auto_kit = [] {
    const char* v = std::getenv("BF3_AUTO_DEPLOY");
    return v && *v ? std::atoi(v) : -1;
  }();
  if (auto_kit >= 0) {
    const uint64_t shown = g_spawn_screen_ms.load();
    if (shown && NowMs() - shown > 8000) {
      g_spawn_screen_ms = 0;
      g_deploy_kit = auto_kit;
    }
  }
  // A player entering a multiplayer level without a team (no Blaze game to assign one) gets the preferred team once
  // the spawn screen is up, so the spawn screen's own DEPLOY works.
  if (g_team_pending.load()) {
    const uint32_t player = g_entered_player.load();
    if (player) {
      g_team_pending = false;
      if (REX_LOAD_U32(player + 0x324) == 0) SetTeam(ctx, base, player, PreferredTeam());
    }
  }
  const int kit = g_deploy_kit.exchange(-1);
  if (kit < 0) return;

  PPCContext saved = ctx;
  sub_83280730(ctx, base);
  const uint32_t manager = ctx.r3.u32;
  uint32_t system = 0;
  if (manager) {
    ctx = saved;
    ctx.r3.u64 = manager;
    ctx.r4.u64 = kSpawnSystemType;
    ctx.r5.u64 = 1;
    const uint32_t fn = REX_LOAD_U32(REX_LOAD_U32(manager) + 0xD0);
    REX_CALL_INDIRECT_FUNC(fn);
    system = ctx.r3.u32;
  }
  const uint32_t pref = REX_LOAD_U32(kTeamPreference);
  REXLOG_INFO("practice: deploy kit {} (ui manager {:08X}, spawn system {:08X}, team preference {})", kit, manager,
              system, pref ? REX_LOAD_U32(pref + 0x2C) : 0xFFFFFFFF);
  if (system) {
    ctx = saved;
    ctx.r3.u64 = system;
    ctx.r4.u64 = static_cast<uint32_t>(kit);
    ctx.r5.u64 = 0;  // select a team first if the player has none
    sub_837E0C90(ctx, base);
    REXLOG_INFO("practice: deploy request returned {}", ctx.r3.u32);
  }
  ctx = saved;
}

// Server: network messages from clients (category 0xC67D277B "Network").
extern "C" REX_FUNC(sub_836C7AA0) {
  const uint32_t msg = ctx.r4.u32;
  if (msg && REX_LOAD_U32(msg + 4) == 0xC67D277B && REX_LOAD_U32(msg + 8) == 0x94F923A5) {
    const uint32_t team = REX_LOAD_U32(msg + 0x38);
    PPCContext saved = ctx;
    ctx.r3.u64 = msg;
    sub_8358A788(ctx, base);
    sub_8342C678(ctx, base);
    const uint32_t player = ctx.r3.u32;
    ctx = saved;
    if (player && team >= 1 && team <= 2 && REX_LOAD_U32(player + 0x324) != team) SetTeam(ctx, base, player, team);
  }
  __imp__sub_836C7AA0(ctx, base);
}
