// Mouse in BF3's menus: a free cursor that hovers, clicks and scrolls the game's own menu items (see pc_mouse.cpp).
#pragma once

namespace bf3::pc_mouse {

// Hooks the game window's mouse messages once the window and BF.Main exist (BF3_MENU_MOUSE=0 disables).
void Install();

// True while the mouse drives the menus (cursor free, no mouse look).
bool MenuMode();

}  // namespace bf3::pc_mouse
