// PRACTICE launcher: a host screen over DICE's developer level select (the main menu's PRACTICE item, see
// bf_main/main_menu.cpp) to start a multiplayer map alone on the local server: map, game mode, team. Kits and weapons
// are chosen on the game's own deploy screen once the level is in.
#pragma once

namespace rex::ui {
class ImGuiDrawer;
}  // namespace rex::ui

namespace bf3::pc_practice {

// Called once from the app (UI thread) when the ImGui drawer exists.
void CreateDialogs(rex::ui::ImGuiDrawer* drawer);

}  // namespace bf3::pc_practice
