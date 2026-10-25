// Lua chunk hooks.
// - Research aid: save every Lua chunk BF3 loads (UI flow, options, ...) when BF3_LUA_DUMP=<host directory> is set.
// - BF3_LEVEL=<level>[:<layer inclusion>] with BF3_LEVEL_DIRECT=1 boots straight into a level instead of the front
//   end, by rewriting the "Game.Level Levels/Frontend/Frontend" setting in Scripts/Game.cfg (a Lua chunk from initfs)
//   in place, e.g. BF3_LEVEL=MP_Subway:GameMode=TeamDeathMatch0. The server loads the level, but the client can't
//   connect while the server is busy loading; without BF3_LEVEL_DIRECT, level_boot.cpp switches levels from the
//   front end instead.
// sub_838F7B38 is Frostbite's luaL_loadbuffer(L r3, buf r4, size r5, chunkname r6) (see docs/CLAUDE_SKILL.md, the
// tableToString caller 0x830501F8). Chunks are written as <index>_<sanitized name>.lua (source or bytecode as given).

// Compiled into the bf3_BF_Main DLL (see CMakeLists.txt): bf3_pch.h is generated/bf_main/bf3_pch.h.
#include "bf3_pch.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>

REX_EXTERN(__imp__sub_838F7B38);

namespace {

const char* DumpDir() {
  static const char* dir = std::getenv("BF3_LUA_DUMP");
  return dir && *dir ? dir : nullptr;
}

std::atomic<int> g_chunks{0};

// The Game.cfg block the level override replaces (setting plus the two commented lines after it, which give room).
// The file has CRLF line endings.
constexpr char kFrontendBlock[] =
    "Game.Level Levels/Frontend/Frontend\r\n#Game.DefaultLayerInclusion GameMode=ConquestLarge\r\n#Level.Start E3_Demo";

// Two commented-out lines in Game.cfg that give room for extra settings on level boots.
constexpr char kServerIpBlock[] =
    "#Client.ServerIp                   10.20.102.99\r\n#Client.SecondaryServerIp          10.20.102.63";

// The commented-out timeout settings in Game.cfg (enabled on level boots: the client gives up on a server that is
// busy loading a large level).
constexpr char kTimeoutBlock[] =
    "//Timeout values\r\n//Client.LoadingTimeOut 99999\r\n//Client.LoadedTimeOut  99999\r\n"
    "//Client.IngameTimeOut  99999\r\n//Server.IngameTimeOut  99999\r\n//Server.LoadingTimeOut 99999";

// Replaces a block of Game.cfg in place with settings lines; padding goes on a trailing comment line.
bool ReplaceBlock(uint8_t* text, std::string_view chunk, std::string_view block, std::string lines) {
  size_t at = chunk.find(block);
  if (at == std::string_view::npos) return false;
  lines += "\r\n#";
  if (lines.size() > block.size()) {
    REXLOG_WARN("cfg override: '{}' too long for its block", lines);
    return false;
  }
  lines.resize(block.size(), ' ');
  std::memcpy(text + at, lines.data(), lines.size());
  return true;
}

void OverrideLevel(uint8_t* text, uint32_t size) {
  static const char* env = std::getenv("BF3_LEVEL");
  static const char* direct = std::getenv("BF3_LEVEL_DIRECT");
  if (!env || !*env || !direct || *direct != '1') return;
  std::string chunk(reinterpret_cast<const char*>(text), size);
  size_t at = chunk.find(kFrontendBlock);
  if (at == std::string::npos) return;
  std::string level = env, layers = "GameMode=TeamDeathMatch0";
  if (size_t colon = level.find(':'); colon != std::string::npos) {
    layers = level.substr(colon + 1);
    level.resize(colon);
  }
  if (level.find('/') == std::string::npos) level = "Levels/" + level + "/" + level;
  if (!ReplaceBlock(text, chunk, kFrontendBlock,
                    "Game.Level " + level + "\r\nGame.DefaultLayerInclusion " + layers)) {
    return;
  }
  // The online backend the front end ends up with (BF3_ONLINE_BACKEND overrides; "-" leaves the data's own).
  const char* backend = std::getenv("BF3_ONLINE_BACKEND");
  std::string b = backend && *backend ? backend : "Backend_Peer";
  // The server can't accept the client while it loads a large level, so give the connection time.
  std::string extra = "Network.ConnectTimeout 180";
  if (b != "-") extra = "Online.Backend " + b + "\r\n" + extra;
  if (ReplaceBlock(text, chunk, kServerIpBlock, extra)) REXLOG_INFO("level override: {}", extra);
  if (ReplaceBlock(text, chunk, kTimeoutBlock,
                   "Client.LoadingTimeOut 99999\r\nClient.LoadedTimeOut 99999\r\nClient.IngameTimeOut 99999\r\n"
                   "Server.IngameTimeOut 99999\r\nServer.LoadingTimeOut 99999")) {
    REXLOG_INFO("level override: loading/ingame timeouts 99999");
  }
  REXLOG_INFO("level override: Game.Level {} ({})", level, layers);
}

// BF3_CFG="Setting value;Setting value": extra Game.cfg settings, written over the commented-out timeout block (not
// with BF3_LEVEL_DIRECT, which uses that block), e.g. BF3_CFG="GameRender.FadeEnable false".
void OverrideCfg(uint8_t* text, uint32_t size) {
  static const char* env = std::getenv("BF3_CFG");
  static const char* direct = std::getenv("BF3_LEVEL_DIRECT");
  if (!env || !*env || (direct && *direct == '1')) return;
  std::string chunk(reinterpret_cast<const char*>(text), size);
  if (chunk.find(kTimeoutBlock) == std::string::npos) return;
  std::string lines = env;
  for (char& c : lines) {
    if (c == ';') c = '\n';
  }
  std::string crlf;
  for (char c : lines) crlf += c == '\n' ? std::string("\r\n") : std::string(1, c);
  if (ReplaceBlock(text, chunk, kTimeoutBlock, crlf)) REXLOG_INFO("cfg override: {}", env);
}

}  // namespace

extern "C" REX_FUNC(sub_838F7B38) {
  if (ctx.r4.u32 && ctx.r5.u32) {
    OverrideLevel(REX_RAW_ADDR(ctx.r4.u32), ctx.r5.u32);
    OverrideCfg(REX_RAW_ADDR(ctx.r4.u32), ctx.r5.u32);
  }
  if (const char* dir = DumpDir()) {
    const uint32_t buf = ctx.r4.u32, size = ctx.r5.u32, name_addr = ctx.r6.u32;
    std::string name = name_addr ? std::string(reinterpret_cast<const char*>(REX_RAW_ADDR(name_addr))) : "noname";
    std::string safe;
    for (char c : name.substr(0, 80)) safe += (isalnum(static_cast<unsigned char>(c)) || c == '.' || c == '-') ? c : '_';
    int index = g_chunks++;
    std::string path = std::string(dir) + "\\" + std::to_string(index) + "_" + safe + ".lua";
    if (FILE* f = std::fopen(path.c_str(), "wb")) {
      std::fwrite(REX_RAW_ADDR(buf), 1, size, f);
      std::fclose(f);
    }
    if (index < 400) REXLOG_INFO("lua dump: #{} '{}' {} bytes", index, name, size);
  }
  __imp__sub_838F7B38(ctx, base);
}
