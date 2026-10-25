// BF3 UI screen tracking. sub_8328B990(manager r3, screen r4) pushes a UI screen into the Scaleform movie (invokes
// "instance1.pushScreen" with the screen's name *(screen+0x18), path string at screen+0x6C, depth, flag at +0x54).
// We record the name of the most recently pushed screen so host features (PC settings) can react to where the player
// is in the menus.

// Compiled into the bf3_BF_Main DLL (see CMakeLists.txt): bf3_pch.h is generated/bf_main/bf3_pch.h.
#include "bf3_pch.h"

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>

REX_EXTERN(__imp__sub_8328B990);

namespace {

std::mutex g_mutex;
std::string g_top_screen;
std::map<std::string, int> g_push_counts;
std::atomic<uint32_t> g_main_movie{0};
std::atomic<uint32_t> g_screen_manager{0};
uint8_t* g_base = nullptr;

std::string GuestString(uint8_t* base, uint32_t addr) {
  if (!addr) return {};
  const char* s = reinterpret_cast<const char*>(REX_RAW_ADDR(addr));
  return std::string(s, strnlen(s, 256));
}

// Heap or image address (the list is read from other threads while the UI thread changes it: never trust a pointer).
bool Plausible(uint32_t a) {
  return (a >= 0x40000000 && a < 0x7FFF0000) || (a >= 0x82000000 && a < 0x84300000);
}

// The manager's live screens: vector at +0x0C..+0x10 of screen pointers (name at +0x18), in push order.
std::string ScreenList(uint8_t* base, uint32_t manager) {
  std::string list;
  if (!base || !Plausible(manager)) return list;
  const uint32_t begin = REX_LOAD_U32(manager + 0x0C), end = REX_LOAD_U32(manager + 0x10);
  if (!Plausible(begin) || end < begin || end - begin > 64 * 4) return list;
  for (uint32_t p = begin; p < end; p += 4) {
    const uint32_t screen = REX_LOAD_U32(p);
    if (!Plausible(screen)) continue;
    const uint32_t name = REX_LOAD_U32(screen + 0x18);
    if (!Plausible(name)) continue;
    const char* text = reinterpret_cast<const char*>(REX_RAW_ADDR(name));
    size_t n = 0;
    while (n < 64 && text[n] >= 0x20 && text[n] < 0x7F) ++n;
    if (!list.empty()) list += ';';
    list.append(text, n);
  }
  return list;
}

}  // namespace

// Read by bf3.exe (pc_mouse.cpp): the live screen stack, names joined by ';' (bottom first). Returns length.
extern "C" __declspec(dllexport) int bf3_ui_screen_list(char* out, int capacity) {
  const std::string list = ScreenList(g_base, g_screen_manager.load());
  int n = static_cast<int>(std::min<size_t>(list.size(), size_t(capacity)));
  std::memcpy(out, list.data(), n);
  return n;
}

void bf3_ui_screen_pushed(const std::string& name);                     // ui_mouse.cpp (before the push)
void bf3_main_menu_before_push(uint8_t* base, const std::string& name);  // main_menu.cpp

uint32_t bf3_ui_main_movie() { return g_main_movie.load(std::memory_order_relaxed); }

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

void PracticeScreenPushed(const std::string& name);  // practice.cpp

extern "C" REX_FUNC(sub_8328B990) {
  const uint32_t screen = ctx.r4.u32;
  g_base = base;
  g_screen_manager = ctx.r3.u32;
  if (screen) {
    std::string name = GuestString(base, REX_LOAD_U32(screen + 0x18));
    REXLOG_INFO("ui: push screen '{}' ({:08X})", name, screen);
    bf3_ui_screen_pushed(name);
    bf3_main_menu_before_push(base, name);
    PracticeScreenPushed(name);
    // The movie the screens live in: manager+48 -> holder, holder+12 -> GFxMovieRoot (ui_mouse.cpp feeds it).
    if (const uint32_t holder = REX_LOAD_U32(ctx.r3.u32 + 48)) g_main_movie = REX_LOAD_U32(holder + 12);
    std::lock_guard<std::mutex> lock(g_mutex);
    g_top_screen = name;
    ++g_push_counts[name];
  }
  __imp__sub_8328B990(ctx, base);
  static const bool trace = [] {
    const char* v = std::getenv("BF3_UI_TRACE");
    return v && *v == '1';
  }();
  if (trace) REXLOG_INFO("ui: screens [{}]", ScreenList(base, g_screen_manager.load()));
}
