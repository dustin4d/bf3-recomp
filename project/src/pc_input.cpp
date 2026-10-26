// PC keyboard/mouse defaults for ReXGlue's controller emulation, modeled on Battlefield 3's PC layout.
//
// The game sees an Xbox 360 controller; each emulated button gets the PC key(s) that do the same thing in BF3 PC:
//   menus     arrows = D-pad, Enter = A (confirm), Esc/Backspace = B (back), P = Start
//   (at most 3 keys per button: the PC SETTINGS screen edits 3 slots)
//   on foot   WASD = left stick, mouse = look (our raw input, pc_mouse.cpp; not the right stick), LMB = RT (fire), RMB = LT (aim), Space = A (jump),
//             C = B (crouch, hold = prone), R/E = X (reload/interact), 1/2 = Y (switch weapon),
//             G = RB (grenade), Q = LB (spot), Shift = left stick press (sprint), F/V = right stick press (melee),
//             3/4 = D-pad left/right (gadgets), Tab = Back (scoreboard)

#include "pc_input.h"

#include <rex/cvar.h>
#include <rex/logging.h>

#include <string_view>
#include <utility>

namespace bf3 {
namespace {

constexpr std::pair<std::string_view, std::string_view> kPcInputDefaults[] = {
    {"mnk_mode", "true"},
    {"keybind_a", "Space,Return"},
    {"keybind_b", "C,Escape,Backspace"},
    {"keybind_x", "R,E"},
    {"keybind_y", "1,2"},
    {"keybind_left_trigger", "RMB"},
    {"keybind_right_trigger", "LMB"},
    {"keybind_left_shoulder", "Q"},
    {"keybind_right_shoulder", "G"},
    {"keybind_lstick_press", "Shift"},
    {"keybind_rstick_press", "F,V"},
    {"keybind_rstick_up", ""},
    {"keybind_rstick_down", ""},
    {"keybind_rstick_left", ""},
    {"keybind_rstick_right", ""},
    {"keybind_dpad_up", "Up"},
    {"keybind_dpad_down", "Down"},
    {"keybind_dpad_left", "Left,3"},
    {"keybind_dpad_right", "Right,4"},
    {"keybind_back", "Tab"},
    {"keybind_start", "P"},
};

}  // namespace

std::string PcInputDefault(std::string_view flag) {
  for (auto [name, value] : kPcInputDefaults) {
    if (name == flag) return std::string(value);
  }
  return {};
}

bool IsPcInputFlag(std::string_view flag) {
  for (auto [name, value] : kPcInputDefaults) {
    if (name == flag) return true;
  }
  return false;
}

void ApplyPcInputDefaults() {
  for (auto [name, value] : kPcInputDefaults) {
    if (rex::cvar::GetFlagSource(name) != rex::cvar::Source::kDefault) continue;
    if (!rex::cvar::SetFlagByName(name, value)) REXLOG_WARN("pc_input: could not set {} = '{}'", name, value);
  }
  REXLOG_INFO("pc_input: mnk_mode={} start='{}' a='{}'", rex::cvar::GetFlagByName("mnk_mode"),
              rex::cvar::GetFlagByName("keybind_start"),
              rex::cvar::GetFlagByName("keybind_a"));
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
