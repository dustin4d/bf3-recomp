// PC SETTINGS screen: keyboard & mouse bindings (BF3 action names), mouse look and sensitivity, display options.
// Opens with F1, and a prompt offers it while one of the game's Options screens is showing. Saves only its own keys
// to the app config (bf3.toml).
#pragma once

#include <filesystem>

struct ImFont;
struct ImFontAtlas;

namespace rex::ui {
class ImGuiDrawer;
class WindowedAppContext;
}  // namespace rex::ui

namespace bf3::pc_settings {

// Called once from the app (UI thread) when the ImGui drawer exists.
void Init(rex::ui::ImGuiDrawer* drawer, rex::ui::WindowedAppContext* app_context, std::filesystem::path config_path);

// Opens the screen; safe from any thread.
void RequestOpen();
bool IsOpen();

// Adds the screen's fonts (Bahnschrift from %WINDIR%\Fonts); call from ReXApp::OnConfigureFonts.
void ConfigureFonts(ImFontAtlas* atlas);

// The fonts, for other host screens in the same style (nullptr if Bahnschrift is missing).
ImFont* TitleFont();
ImFont* BodyFont();

}  // namespace bf3::pc_settings
