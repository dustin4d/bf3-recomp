// Lua chunk hooks.
// - Research aid: save every Lua chunk BF3 loads (UI flow, options, ...) when BF3_LUA_DUMP=<host directory> is set.
// - BF3_LEVEL=<level>[:<layer inclusion>] boots straight into a level instead of the front end, by rewriting the
//   "Game.Level Levels/Frontend/Frontend" setting in Scripts/Game.cfg (a Lua chunk from initfs) in place, e.g.
//   BF3_LEVEL=MP_Subway:GameMode=TeamDeathMatch0. Default layer inclusion: GameMode=TeamDeathMatch0.
// sub_838F7B38 is Frostbite's luaL_loadbuffer(L r3, buf r4, size r5, chunkname r6) (see docs/CLAUDE_SKILL.md, the
// tableToString caller 0x830501F8). Chunks are written as <index>_<sanitized name>.lua (source or bytecode as given).

// Compiled into the bf3_BF_Main DLL (see CMakeLists.txt): bf3_pch.h is generated/bf_main/bf3_pch.h.
#include "bf3_pch.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

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

void OverrideLevel(uint8_t* text, uint32_t size) {
  static const char* env = std::getenv("BF3_LEVEL");
  if (!env || !*env) return;
  std::string chunk(reinterpret_cast<const char*>(text), size);
  size_t at = chunk.find(kFrontendBlock);
  if (at == std::string::npos) return;
  std::string level = env, layers = "GameMode=TeamDeathMatch0";
  if (size_t colon = level.find(':'); colon != std::string::npos) {
    layers = level.substr(colon + 1);
    level.resize(colon);
  }
  if (level.find('/') == std::string::npos) level = "Levels/" + level + "/" + level;
  std::string block = "Game.Level " + level + "\r\nGame.DefaultLayerInclusion " + layers;
  const size_t room = sizeof(kFrontendBlock) - 1;
  block += "\r\n#";  // the padding goes on a comment line; trailing spaces would become part of the value
  if (block.size() > room) {
    REXLOG_WARN("level override: '{}' too long for Game.cfg", block);
    return;
  }
  block.resize(room, ' ');
  std::memcpy(text + at, block.data(), room);
  REXLOG_INFO("level override: Game.Level {} ({})", level, layers);
}

}  // namespace

extern "C" REX_FUNC(sub_838F7B38) {
  if (ctx.r4.u32 && ctx.r5.u32) OverrideLevel(REX_RAW_ADDR(ctx.r4.u32), ctx.r5.u32);
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
