// bf3 - ReXGlue Recompiled Project
//
// Customize your app by overriding virtual hooks from rex::ReXApp.

#pragma once

#include <rex/filesystem.h>

#include "kernel_fixes.h"
#include "live_emulation.h"
#include "pc_icons.h"
#include "pc_mouse.h"
#include "pc_settings.h"
#include "pad_inject.h"
#include "pc_input.h"

namespace bf3 {
void InstallDiagnostics();  // crash_trace.cpp
}
#include <rex/rex_app.h>

class Bf3App : public rex::ReXApp {
 public:
  using rex::ReXApp::ReXApp;

  static std::unique_ptr<rex::ui::WindowedApp> Create(
      rex::ui::WindowedAppContext& ctx) {
    return std::unique_ptr<Bf3App>(new Bf3App(ctx, "bf3",
        PPCImageConfig));
  }

  // Default to the xenos GPU plugin (staged next to the exe by GPU_PLUGINS xenos) unless --gpu_plugin is given.
  // Without one the game stalls after VdInitializeRingBuffer ("no GPU emulation loaded").
  // Memexport readback must be precise (patches/rexglue/0001: gpu_plugin/ builds the patched rexgpu-xenos.dll).
  void OnPreSetup(rex::RuntimeConfig& config) override {
    if (config.gpu_plugin.empty()) config.gpu_plugin = "xenos";
  }

  // Without --game_data_root, use game\ next to the exe (C:\Games\BF3-recomp\game), so a double-click works.
  void OnConfigurePaths(rex::PathConfig& paths) override {
    config_path_ = paths.config_path;
    if (!paths.game_data_root.empty()) return;
    auto game_dir = rex::filesystem::GetExecutableFolder() / "game";
    if (std::filesystem::is_directory(game_dir)) paths.game_data_root = game_dir;
  }

  // Override virtual hooks for customization:
  void OnPostInitLogging() override {
    bf3::InstallDiagnostics();
    bf3::ApplyPcInputDefaults();
  }

  void OnPostSetup() override {
    bf3::InstallPadInject();
    bf3::InstallLiveEmulation();
    bf3::InstallKernelFixes();
    bf3::pc_icons::Install();
    bf3::pc_mouse::Install();
  }
  // void OnLoadXexImage(std::string& xex_image) override {}
  // void OnPostLoadXexImage() override {}
  void OnConfigureFonts(ImFontAtlas* atlas) override { bf3::pc_settings::ConfigureFonts(atlas); }

  void OnCreateDialogs(rex::ui::ImGuiDrawer* drawer) override {
    bf3::pc_settings::Init(drawer, &app_context(), config_path_);
  }
  // std::unique_ptr<rex::ui::ImGuiDialog> CreateAchievementsOverlay() override;
  // std::unique_ptr<rex::ui::AchievementNotificationDialog>
  // CreateAchievementNotificationDialog() override;
  // void OnShutdown() override {}

 private:
  std::filesystem::path config_path_;  // where PC SETTINGS saves (bf3.toml)
};
