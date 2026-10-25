// The BF3 UI screen the player is on, for host features (PC settings). The BF.Main DLL hook (bf_main/ui_screens.cpp)
// records the name of each pushed screen and exports it as bf3_ui_top_screen; this reads it from bf3.exe.
#pragma once

#include <string>

namespace bf3::ui_state {

// Name of the most recently pushed screen, "" before the first one or before BF.Main is loaded.
std::string TopScreen();
// How many times a screen with this name has been pushed so far.
int ScreenCount(const std::string& name);

}  // namespace bf3::ui_state
