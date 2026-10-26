// Mouse in BF3's menus: a free cursor that hovers, clicks and scrolls the game's own menu items (see pc_mouse.cpp).
#pragma once

#include <cstdint>

namespace bf3::pc_mouse {

// Hooks the game window's mouse messages once the window and BF.Main exist (BF3_MENU_MOUSE=0 disables).
void Install();

// True while the mouse drives the menus (cursor free, no mouse look).
bool MenuMode();

// The game window is the foreground window.
bool GameFocused();

// Raw mouse counts gathered since the last take (x right, y down) while the cursor is captured; false if not
// captured. Vehicles take them through pc_keys (bf3_pc_concepts); on foot bf3_mouse_look_delta does.
bool TakeCounts(double& dx, double& dy);

// Debug API: raw look counts (x right, y down) as if the mouse moved, taken even without a captured cursor.
void InjectLookCounts(double dx, double dy);
// Debug API: a menu mouse event (1 move, 2 button down, 3 button up, 4 wheel), x/y 0..1 across the game image;
// false before BF.Main's menu mouse is up.
bool InjectMenuEvent(uint32_t type, float x, float y, float scroll);

}  // namespace bf3::pc_mouse
