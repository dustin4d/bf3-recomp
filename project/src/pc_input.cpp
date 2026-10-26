// PC input setup. Keyboard and mouse are native (pc_keys.cpp, pc_mouse.cpp): ReXGlue's keyboard-to-controller
// emulation (mnk_mode) stays off, and its overlay hotkeys move off F1-F8 (BF3 PC's vehicle seat keys).

#include "pc_input.h"

#include <rex/cvar.h>
#include <rex/logging.h>

#include <string_view>
#include <utility>

namespace bf3 {

void ApplyPcInputDefaults() {
  for (const char* flag : {"mnk_mode", "mnk_mouse"}) {
    if (rex::cvar::GetFlagSource(flag) != rex::cvar::Source::kCommandLine) rex::cvar::SetFlagByName(flag, "false");
  }
  REXLOG_INFO("pc_input: native keyboard/mouse (mnk_mode={})", rex::cvar::GetFlagByName("mnk_mode"));
}

void ApplyPcBindDefaults() {
  constexpr std::pair<std::string_view, std::string_view> kBinds[] = {
      {"bind_debug_overlay", "F10"}, {"bind_settings", "F12"}, {"bind_achievements", "F11"}};
  for (auto [name, key] : kBinds) {
    if (rex::cvar::GetFlagSource(name) == rex::cvar::Source::kDefault) rex::cvar::SetFlagByName(name, key);
  }
}

// The emulated guest vblank (vsync) runs at 1 kHz: BF3 presents every 2nd vblank, so a 60 Hz vblank holds it at
// 30 FPS. The game's own frame wait (bf_main/frame_rate.cpp, max_fps) paces it instead, and present_vsync syncs the
// host display. Only a command-line --vsync=true brings back the console pacing. The flag belongs to the GPU plugin,
// so this runs after setup.
void ApplyPcDisplayDefaults() {
  if (rex::cvar::GetFlagSource("vsync") != rex::cvar::Source::kCommandLine &&
      !rex::cvar::SetFlagByName("vsync", "false")) {
    REXLOG_WARN("pc_input: could not set vsync = false");
  }
  REXLOG_INFO("pc_input: guest vsync={} present_vsync={} max_fps={}", rex::cvar::GetFlagByName("vsync"),
              rex::cvar::GetFlagByName("present_vsync"), rex::cvar::GetFlagByName("max_fps"));
}

}  // namespace bf3
