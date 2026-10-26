// Frame rate limit. The main loop's frame wait sub_83838CF0(timer r3, ..., f1 = minimum frame time in seconds) sleeps
// (KeDelayExecutionThread via sub_838D14C0) until the time accumulated at timer+0x50 reaches f1; the console build
// passes 1/30 s. While playing (HudScreen on the UI stack) the limit is the max_fps flag (PC SETTINGS > DISPLAY;
// 0 = no limit, 30 = the console behavior), or BF3_MAX_FPS=<n> if set (tests);
// loading and the menus keep 30: without the limit, boot and level loads hit timing races (a CPU-released GPU sync
// slot of sub_82FD6968 never released -> WAIT_REG_MEM forever; texture binds of half-built textures).

#include "bf3_pch.h"

#include <rex/cvar.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <string_view>

REX_EXTERN(__imp__sub_83838CF0);

extern "C" int bf3_ui_screen_list(char* out, int capacity);  // ui_screens.cpp

namespace {
bool Playing() {
  char buf[1024];
  const int n = bf3_ui_screen_list(buf, sizeof(buf));
  const std::string_view list(buf, n > 0 ? n : 0);
  return list.find("HudScreen") != std::string_view::npos && list.find("Spawn") == std::string_view::npos &&
         list.find("Loading") == std::string_view::npos;
}

// Logs the frame rate every 5 s while playing: frames, average and worst frame time (stutter).
void CountFrame(bool playing) {
  using clock = std::chrono::steady_clock;
  static clock::time_point window_start, last;
  static int frames = 0;
  static double worst_ms = 0.0;
  const auto now = clock::now();
  if (!playing) {
    frames = 0;
    last = {};
    return;
  }
  if (last == clock::time_point{}) {
    window_start = last = now;
    worst_ms = 0.0;
    return;
  }
  const double ms = std::chrono::duration<double, std::milli>(now - last).count();
  last = now;
  worst_ms = std::max(worst_ms, ms);
  ++frames;
  const double window_s = std::chrono::duration<double>(now - window_start).count();
  if (window_s >= 5.0) {
    REXLOG_INFO("frame rate: {:.1f} fps, avg {:.2f} ms, worst {:.1f} ms", frames / window_s, 1000.0 * window_s / frames,
                worst_ms);
    window_start = now;
    frames = 0;
    worst_ms = 0.0;
  }
}
}  // namespace

extern "C" REX_FUNC(sub_83838CF0) {
  static std::atomic<int> logs{0};
  if (logs.fetch_add(1) < 3) {
    REXLOG_INFO("frame rate: frame wait min {:.5f} s (lr {:08X})", ctx.f1.f64, static_cast<uint32_t>(ctx.lr));
  }
  static const char* env_max_fps = std::getenv("BF3_MAX_FPS");
  static double max_fps = 0.0;
  static int refresh = 0;
  if (refresh-- <= 0) {  // the flag can change in PC SETTINGS; a string lookup every frame isn't needed
    refresh = 30;
    max_fps = env_max_fps && *env_max_fps ? std::atof(env_max_fps)
                                          : std::atof(rex::cvar::GetFlagByName("max_fps").c_str());
  }
  const bool playing = Playing();
  CountFrame(playing);
  if (max_fps >= 0.0 && playing) {
    ctx.f1.f64 = max_fps == 0.0 ? 0.0 : 1.0 / max_fps;
  }
  __imp__sub_83838CF0(ctx, base);
}

// Texture bind sub_8313C6E0(renderer r3, slot r4, texture r5, srgb r6) calls the texture's vtable +0x38. With the
// frame limit lifted a render job (sub_83D21C50) sometimes passes a texture whose object is gone or not constructed
// yet (vtable 0): bind nothing instead of calling through it.
REX_EXTERN(__imp__sub_8313C6E0);
extern "C" REX_FUNC(sub_8313C6E0) {
  if (const uint32_t texture = ctx.r5.u32) {
    const uint32_t vtable = REX_LOAD_U32(texture);
    if (vtable < 0x82000000 || vtable >= 0x84300000 || !REX_LOAD_U32(vtable + 0x38)) {
      static std::atomic<int> logs{0};
      if (logs.fetch_add(1) < 5) {
        REXLOG_WARN("texture bind: slot {} texture {:08X} has vtable {:08X}, bound as none (lr {:08X})", ctx.r4.u32,
                    texture, vtable, static_cast<uint32_t>(ctx.lr));
      }
      ctx.r5.u64 = 0;
    }
  }
  __imp__sub_8313C6E0(ctx, base);
}
