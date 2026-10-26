// PC input and display setup (see pc_input.cpp).
#pragma once

namespace bf3 {
// Native keyboard/mouse: ReXGlue's controller emulation off (unless given on the command line).
void ApplyPcInputDefaults();
// Moves ReXGlue's overlay hotkeys off F1-F8 (call once they are registered, OnCreateDialogs).
void ApplyPcBindDefaults();
// Guest vblank unthrottled (after setup: the flag is the GPU plugin's).
void ApplyPcDisplayDefaults();
}  // namespace bf3
