// BF3 UI screen tracking. sub_8328B990(manager r3, screen r4) pushes a UI screen into the Scaleform movie (invokes
// "instance1.pushScreen" with the screen's name *(screen+0x18), path string at screen+0x6C, depth, flag at +0x54).
// We record the name of the most recently pushed screen so host features (PC settings) can react to where the player
// is in the menus.

// Compiled into the bf3_BF_Main DLL (see CMakeLists.txt): bf3_pch.h is generated/bf_main/bf3_pch.h.
#include "bf3_pch.h"

#include <cstring>
#include <map>
#include <mutex>
#include <string>

REX_EXTERN(__imp__sub_8328B990);

namespace {

std::mutex g_mutex;
std::string g_top_screen;
std::map<std::string, int> g_push_counts;

std::string GuestString(uint8_t* base, uint32_t addr) {
  if (!addr) return {};
  const char* s = reinterpret_cast<const char*>(REX_RAW_ADDR(addr));
  return std::string(s, strnlen(s, 256));
}

}  // namespace

// Read by bf3.exe (src/ui_state.cpp). Copies the last pushed screen's name into out (no terminator), returns length.
extern "C" __declspec(dllexport) int bf3_ui_top_screen(char* out, int capacity) {
  std::lock_guard<std::mutex> lock(g_mutex);
  int n = static_cast<int>(std::min<size_t>(g_top_screen.size(), size_t(capacity)));
  std::memcpy(out, g_top_screen.data(), n);
  return n;
}

// How many times a screen with this name has been pushed (e.g. StartScreen: once while loading, again when the title
// screen becomes interactive).
extern "C" __declspec(dllexport) int bf3_ui_screen_count(const char* name) {
  std::lock_guard<std::mutex> lock(g_mutex);
  auto it = g_push_counts.find(name);
  return it == g_push_counts.end() ? 0 : it->second;
}

extern "C" REX_FUNC(sub_8328B990) {
  const uint32_t screen = ctx.r4.u32;
  if (screen) {
    std::string name = GuestString(base, REX_LOAD_U32(screen + 0x18));
    REXLOG_INFO("ui: push screen '{}' ({:08X})", name, screen);
    std::lock_guard<std::mutex> lock(g_mutex);
    g_top_screen = name;
    ++g_push_counts[name];
  }
  __imp__sub_8328B990(ctx, base);
}
