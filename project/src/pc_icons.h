// PC-native button icons: replaces the Xbox 360 controller glyphs in BF3's UI button atlas with keyboard/mouse
// keycaps showing the keys currently bound to each emulated button (see pc_icons.cpp).
#pragma once

namespace bf3::pc_icons {

// Starts the background thread that finds the atlas in guest memory and patches it (BF3_PC_ICONS=0 disables).
void Install();

// Re-draws the keycaps from the current key bindings (call after they change; context changes redraw by themselves).
void RequestRefresh();

}  // namespace bf3::pc_icons
