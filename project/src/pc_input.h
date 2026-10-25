// PC keyboard/mouse defaults for ReXGlue's controller emulation (see pc_input.cpp).
#pragma once

namespace bf3 {
// Turns on keyboard/mouse emulation with BF3-on-PC style binds. Only settings still at their compiled-in default are
// changed, so --keybind_* / mnk_* on the command line or in bf3.toml always win.
void ApplyPcInputDefaults();
}  // namespace bf3
