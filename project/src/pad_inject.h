// Guest controller input hook (see pad_inject.cpp): input lock for settings changes, blocking game input while a host
// screen is open, and scripted test input (BF3_PAD_SCRIPT).
#pragma once

#include <cstdint>
#include <shared_mutex>

namespace bf3 {

// Redirects bf3_BF_Main.dll's XamInputGetState import once the module loads.
void InstallPadInject();

// Held shared around every guest input poll. ReXGlue's keyboard driver copies the keybind_* strings on each poll, so
// changing them needs this exclusively (unguarded writes corrupted the heap in the Trials HD port).
std::shared_mutex& InputLock();

// While set, the game sees an idle controller (a host screen such as PC Settings is open).
void SetGameInputBlocked(bool blocked);
bool GameInputBlocked();

// While set, the game sees no triggers (menu mouse mode: the mouse buttons are bound to the triggers, but in menus a
// click goes to the menu as a click; see pc_mouse.cpp).
void SetMenuTriggerMask(bool masked);

// While active, controller 0's right stick reads (rx, ry) (aim_scan.cpp drives it).
void SetRightStickOverride(bool active, int16_t rx = 0, int16_t ry = 0);

// Holds the given X_INPUT_GAMEPAD button bits down for `ms` milliseconds (menu mouse: right-click = Back).
void PulseButtons(uint16_t buttons, uint32_t ms);

}  // namespace bf3
