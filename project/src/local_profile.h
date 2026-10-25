// Local soldier profile for the local online stack (bf_main/local_online.cpp): set up on first use of an online
// section with a SOLDIER PROFILE prompt, stored in the user data folder, and reported as the player's name.
#pragma once

#include <filesystem>

namespace rex::ui {
class ImGuiDrawer;
}  // namespace rex::ui

namespace bf3::local_profile {

// Loads the saved profile (user_data_root/soldier_profile.txt) and redirects the game's player-name queries to it.
void Install(std::filesystem::path user_data_root);

// Creates the prompt; call from ReXApp::OnCreateDialogs.
void CreateDialogs(rex::ui::ImGuiDrawer* drawer);

}  // namespace bf3::local_profile
