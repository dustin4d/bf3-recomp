// Mouse input for BF3's Scaleform menus. The exe (src/pc_mouse.cpp) queues mouse events from the game window; they
// are handed to the main UI movie on the UI thread, from its per-frame Advance.
//
// GFxMovieRoot (vtable 0x82E90A88): +0x40/+0x44 viewport buffer size (1280x720), +0x48/+0x4C left/top, +0x9B4 mouse
// cursor count (0 on Xenon, which makes HandleEvent drop every mouse event), +0x2468 flags (0x40000 = focused).
// vtable +0x94 Advance (sub_83A434A0), +0xB4 HandleEvent (sub_83A348F0(movie, GFxEvent*)): event {+0 u32 type
// (1 move, 2 down, 3 up, 4 wheel), +4 f32 x, +8 f32 y (viewport pixels), +0xC f32 scroll delta, +0x10 u32 button
// (0 left, 1 right, 2 middle), +0x14 u32 mouse index}. The BF3 widgets react to Scaleform's mouse events (item
// over/out/pressed/released), as on PC.

// Compiled into the bf3_BF_Main DLL (see CMakeLists.txt): bf3_pch.h is generated/bf_main/bf3_pch.h.
#include "bf3_pch.h"

#include <atomic>
#include <string>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <vector>

REX_EXTERN(__imp__sub_83A434A0);
REX_EXTERN(sub_83013BA0);  // allocator (allocator r3, size r4, align r5)
REX_EXTERN(sub_83A348F0);  // GFxMovieRoot::HandleEvent

uint32_t bf3_ui_main_movie();  // ui_screens.cpp

// Depth of Scaleform calls on this thread: ActionScript only runs inside them, the engine's flow decisions outside.
thread_local int t_gfx_depth = 0;
struct GfxScope {
  GfxScope() { ++t_gfx_depth; }
  ~GfxScope() { --t_gfx_depth; }
};

namespace {

struct QueuedEvent {
  uint32_t type;
  float nx, ny;  // 0..1 across the game image
  float scroll;
  uint32_t button;
};

constexpr uint32_t kMouseCount = 0x9B4;
constexpr uint32_t kViewportWidth = 0x50, kViewportHeight = 0x54, kViewportLeft = 0x48, kViewportTop = 0x4C;

std::mutex g_mutex;
std::vector<QueuedEvent> g_queue;
uint32_t g_event_buffer = 0;  // guest memory for one GFxEvent

void StoreF32(uint8_t* base, uint32_t addr, float v) {
  uint32_t bits;
  std::memcpy(&bits, &v, 4);
  REX_STORE_U32(addr, bits);
}

}  // namespace

// Called by bf3.exe (any thread). type: 1 move, 2 down, 3 up, 4 wheel.
extern "C" __declspec(dllexport) void bf3_ui_mouse_event(uint32_t type, float nx, float ny, uint32_t button,
                                                        float scroll) {
  std::lock_guard lock(g_mutex);
  if (g_queue.size() > 256) return;  // the UI isn't advancing (loading): drop
  if (type == 1 && !g_queue.empty() && g_queue.back().type == 1) {
    g_queue.back().nx = nx, g_queue.back().ny = ny;  // coalesce moves
    return;
  }
  g_queue.push_back({type, nx, ny, scroll, button});
}

// GFxMovieRoot::Advance(movie r3, float delta f1, ...).
extern "C" REX_FUNC(sub_83A434A0) {
  GfxScope scope;
  const uint32_t movie = ctx.r3.u32;
  std::vector<QueuedEvent> events;
  if (movie && movie == bf3_ui_main_movie()) {
    std::lock_guard lock(g_mutex);
    events.swap(g_queue);
  }
  if (!events.empty()) {
    PPCContext saved = ctx;
    if (!g_event_buffer) {
      ctx.r3.u64 = 0x84042270;  // the allocator the UI uses for small objects
      ctx.r4.u64 = 64;
      ctx.r5.u64 = 16;
      sub_83013BA0(ctx, base);
      g_event_buffer = ctx.r3.u32;
      ctx = saved;
    }
    if (g_event_buffer) {
      if (REX_LOAD_U32(movie + kMouseCount) == 0) {
        REX_STORE_U32(movie + kMouseCount, 1);
        REXLOG_INFO("ui mouse: enabled the mouse cursor on movie {:08X}", movie);
      }
      const float w = float(REX_LOAD_U32(movie + kViewportWidth)), h = float(REX_LOAD_U32(movie + kViewportHeight));
      const float left = float(REX_LOAD_U32(movie + kViewportLeft)), top = float(REX_LOAD_U32(movie + kViewportTop));
      for (const QueuedEvent& e : events) {
        const float x = left + e.nx * w, y = top + e.ny * h;
        std::memset(REX_RAW_ADDR(g_event_buffer), 0, 0x18);
        REX_STORE_U32(g_event_buffer + 0x00, e.type);
        StoreF32(base, g_event_buffer + 0x04, x);
        StoreF32(base, g_event_buffer + 0x08, y);
        StoreF32(base, g_event_buffer + 0x0C, e.scroll);
        REX_STORE_U32(g_event_buffer + 0x10, e.button);
        REX_STORE_U32(g_event_buffer + 0x14, 0);
        ctx.r3.u64 = movie;
        ctx.r4.u64 = g_event_buffer;
        sub_83A348F0(ctx, base);
        ctx = saved;
      }
    }
  }
  __imp__sub_83A434A0(ctx, base);
}

// The UI's platform. The ActionScript asks the engine for it (HelperFunctions.getPlatform ->
// UIDataInterfaceComp.getData(0xD822972E), a native getter), and the engine's data handler sub_837F3CC0 answers each
// query with the literal "xenon" (0x82D83914) through sub_8328D658(value, const char*). The movies carry their PC
// behaviour too (mouse listeners, clickable button bars and tabs, scrollbars, HideOnConsole/HideOnPC clips),
// switched on "win32" (0x82D8391C). The answer also steers the flow, though: "win32" from the start gives the PC
// game's offline front end (MainMenuScreenPC: Campaign, Options, Quit; PC options screens with KEY BINDINGS).
// BF3_UI_PLATFORM: "hybrid" (default) keeps the console main menu (its decision always gets xenon), but once it is up
// the screens pushed from it get their PC variants where they have one (Options: PC tabs incl. KEY BINDINGS,
// clickable BACK/RESET buttons); "win32" everywhere from the start; "xenon" never.
REX_EXTERN(__imp__sub_8328D658);
REX_EXTERN(__imp__sub_83207790);
REX_EXTERN(__imp__sub_83294100);

namespace {
constexpr uint32_t kXenonString = 0x82D83914;
constexpr uint32_t kWin32String = 0x82D8391C;
constexpr uint32_t kDataHandlerBegin = 0x837F3CC0, kDataHandlerEnd = 0x837F5600;
// The flow reads the platform (through sub_83294100 -> provider sub_83207790 -> handler) at two places: choosing a
// pushed screen's variant (returns to 0x836E3D60) and choosing the main menu's (0x832884E0).
constexpr uint32_t kScreenVariantRead = 0x836E3D60;

enum class UiPlatform { kXenon, kWin32, kHybrid };

UiPlatform Mode() {
  static const UiPlatform mode = [] {
    const char* env = std::getenv("BF3_UI_PLATFORM");
    if (env && std::strcmp(env, "xenon") == 0) return UiPlatform::kXenon;
    if (env && std::strcmp(env, "win32") == 0) return UiPlatform::kWin32;
    return UiPlatform::kHybrid;
  }();
  return mode;
}

std::atomic<bool> g_pc_widgets{false};
}  // namespace



// Called by ui_screens.cpp before a screen is pushed.
void bf3_ui_screen_pushed(const std::string& name) {
  if (Mode() == UiPlatform::kHybrid && name == "MainMenuScreen" && !g_pc_widgets.exchange(true)) {
    REXLOG_INFO("ui: menus get PC widgets from here on (platform answered as win32)");
  }
}

thread_local uint32_t t_provider_caller = 0;
thread_local uint32_t t_read_caller = 0;

// Flow data read (sub_83294100(node r3, key r4, value* r5, refresh r6)): who asked.
extern "C" REX_FUNC(sub_83294100) {
  const uint32_t saved = t_read_caller;
  t_read_caller = static_cast<uint32_t>(ctx.lr);
  __imp__sub_83294100(ctx, base);
  t_read_caller = saved;
}

// The data provider (calls the handler at 0x832077F8): who asked.
extern "C" REX_FUNC(sub_83207790) {
  const uint32_t saved = t_provider_caller;
  t_provider_caller = static_cast<uint32_t>(ctx.lr);
  __imp__sub_83207790(ctx, base);
  t_provider_caller = saved;
}

extern "C" REX_FUNC(sub_8328D658) {
  const uint32_t lr = static_cast<uint32_t>(ctx.lr);
  if (ctx.r4.u32 == kXenonString && lr >= kDataHandlerBegin && lr < kDataHandlerEnd &&
      (Mode() == UiPlatform::kWin32 || (g_pc_widgets && t_read_caller == kScreenVariantRead))) {
    ctx.r4.u64 = kWin32String;
  }
  if (ctx.r4.u32 == kXenonString || ctx.r4.u32 == kWin32String) {
    if (lr >= kDataHandlerBegin && lr < kDataHandlerEnd) {
      REXLOG_INFO("ui: platform query from {:08X} < {:08X} (gfx depth {}) -> {}", t_provider_caller, t_read_caller,
                  t_gfx_depth,
                  ctx.r4.u32 == kWin32String ? "win32" : "xenon");
    }
  }
  __imp__sub_8328D658(ctx, base);
}

// Scaleform entry points that run ActionScript, tracked for the platform answer above.
REX_EXTERN(__imp__sub_83A348F0);  // GFxMovieRoot::HandleEvent
REX_EXTERN(__imp__sub_83A3F0E8);  // GFxMovieRoot invoke (vtable +0x5C forwards here)
REX_EXTERN(__imp__sub_83A3F358);  // vtable +0x60 (invoke variant)
REX_EXTERN(__imp__sub_83A3DE08);  // vtable +0x64 (invoke variant)

extern "C" REX_FUNC(sub_83A348F0) {
  GfxScope scope;
  __imp__sub_83A348F0(ctx, base);
}
namespace {
std::string GuestText(uint8_t* base, uint32_t a) {
  if (a < 0x40000000 || a >= 0xA0000000) return {};
  const char* p = reinterpret_cast<const char*>(REX_RAW_ADDR(a));
  std::string s(p, strnlen(p, 120));
  for (char c : s) if (static_cast<unsigned char>(c) < 0x20 && c != '\n') return {};
  return s;
}
}  // namespace

// sub_83A3F0E8(movie root r3, method r4, result r5, args r6, count r7). BF3_INVOKE_TRACE=1 logs them.
extern "C" REX_FUNC(sub_83A3F0E8) {
  GfxScope scope;
  static const bool trace = [] {
    const char* env = std::getenv("BF3_INVOKE_TRACE");
    return env && *env == '1';
  }();
  if (trace) {
    std::string line = "invoke " + GuestText(base, ctx.r4.u32) + "(";
    const uint32_t args = ctx.r6.u32, n = ctx.r7.u32;
    for (uint32_t i = 0; i < n && i < 8 && args; ++i) {
      const uint32_t a = args + i * 16;
      std::string words;
      for (int w = 0; w < 4; ++w) words += fmt::format("{:08X} ", REX_LOAD_U32(a + 4 * w));
      std::string text;
      for (int w = 0; w < 4 && text.empty(); ++w) text = GuestText(base, REX_LOAD_U32(a + 4 * w));
      line += words + (text.empty() ? "" : "'" + text + "'") + "; ";
    }
    REXLOG_INFO("{})", line);
  }
  __imp__sub_83A3F0E8(ctx, base);
}
extern "C" REX_FUNC(sub_83A3F358) {
  GfxScope scope;
  __imp__sub_83A3F358(ctx, base);
}
extern "C" REX_FUNC(sub_83A3DE08) {
  GfxScope scope;
  __imp__sub_83A3DE08(ctx, base);
}

// BF3_CMP_TRACE=<string>: log case-insensitive compares (sub_830078D0(string* r3, const char* r4)) against it.
REX_EXTERN(__imp__sub_830078D0);
extern "C" REX_FUNC(sub_830078D0) {
  static const char* want = [] {
    const char* env = std::getenv("BF3_CMP_TRACE");
    return env && *env ? env : nullptr;
  }();
  if (want) {
    const std::string a = GuestText(base, REX_LOAD_U32(ctx.r3.u32)), b = GuestText(base, ctx.r4.u32);
    if (a == want || b == want) REXLOG_INFO("cmp '{}' '{}' from {:08X}", a, b, uint32_t(ctx.lr));
  }
  __imp__sub_830078D0(ctx, base);
}
