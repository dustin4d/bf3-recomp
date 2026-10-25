// Research aid: save every Lua chunk BF3 loads (UI flow, options, ...) when BF3_LUA_DUMP=<host directory> is set.
// sub_838F7B38 is Frostbite's luaL_loadbuffer(L r3, buf r4, size r5, chunkname r6) (see docs/CLAUDE_SKILL.md, the
// tableToString caller 0x830501F8). Chunks are written as <index>_<sanitized name>.lua (source or bytecode as given).

// Compiled into the bf3_BF_Main DLL (see CMakeLists.txt): bf3_pch.h is generated/bf_main/bf3_pch.h.
#include "bf3_pch.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <string>

REX_EXTERN(__imp__sub_838F7B38);

namespace {

const char* DumpDir() {
  static const char* dir = std::getenv("BF3_LUA_DUMP");
  return dir && *dir ? dir : nullptr;
}

std::atomic<int> g_chunks{0};

}  // namespace

extern "C" REX_FUNC(sub_838F7B38) {
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
