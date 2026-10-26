// PC keyboard/mouse defaults for ReXGlue's controller emulation (see pc_input.cpp).
#pragma once

#include <string>
#include <string_view>

namespace bf3 {
// Turns on keyboard/mouse emulation with BF3-on-PC style binds. Only settings still at their compiled-in default are
// changed, so --keybind_* / mnk_* on the command line or in bf3.toml always win.
void ApplyPcInputDefaults();
// Guest vblank unthrottled (after setup: the flag is the GPU plugin's).
void ApplyPcDisplayDefaults();

// The PC default for a flag this module sets ("" if none), and whether it sets the flag at all.
std::string PcInputDefault(std::string_view flag);
bool IsPcInputFlag(std::string_view flag);
}  // namespace bf3
