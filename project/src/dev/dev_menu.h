// In-game debug menu and stats overlay (development builds only: BF3_DEV_TOOLS). See dev_menu.cpp.
#pragma once

#include <filesystem>

namespace rex::ui {
class ImGuiDrawer;
class WindowedAppContext;
}  // namespace rex::ui

namespace bf3::dev {

// Called once from the app (UI thread) when the ImGui drawer exists. Saved teleport spots go to
// <user_data_root>/dev_spots.txt.
void InstallMenu(rex::ui::ImGuiDrawer* drawer, rex::ui::WindowedAppContext* app_context,
                 std::filesystem::path user_data_root);

// Opens or closes the menu (debug API `menu`); tab: player, movement, teleport, weapons, overlay ("" keeps it).
void SetMenuOpen(bool open, const char* tab = "");
bool MenuOpen();

}  // namespace bf3::dev
