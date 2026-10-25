// bf3 - ReXGlue Recompiled Project
//
// Customize your app by overriding virtual hooks from rex::ReXApp.

#pragma once

#include <rex/filesystem.h>

#include <rex/cvar.h>
#include <rex/system/xmemory.h>

#include "gpu_readback_fix.h"

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
  // The precise memexport readback (gpu_readback_fix.cpp) needs the synchronous readback path.
  void OnPreSetup(rex::RuntimeConfig& config) override {
    if (config.gpu_plugin.empty()) config.gpu_plugin = "xenos";
    if (!rex::cvar::HasNonDefaultValue("readback_memexport_fast")) {
      rex::cvar::SetFlagByName("readback_memexport_fast", "false");
    }
  }

  void OnPostSetup() override { bf3::InstallGpuReadbackFix(runtime()->memory()); }

  // Without --game_data_root, use game\ next to the exe (C:\Games\BF3-recomp\game), so a double-click works.
  void OnConfigurePaths(rex::PathConfig& paths) override {
    if (!paths.game_data_root.empty()) return;
    auto game_dir = rex::filesystem::GetExecutableFolder() / "game";
    if (std::filesystem::is_directory(game_dir)) paths.game_data_root = game_dir;
  }

  // Override virtual hooks for customization:
  void OnPostInitLogging() override { bf3::InstallDiagnostics(); }
  // void OnLoadXexImage(std::string& xex_image) override {}
  // void OnPostLoadXexImage() override {}
  // void OnCreateDialogs(rex::ui::ImGuiDrawer* drawer) override {}
  // std::unique_ptr<rex::ui::ImGuiDialog> CreateAchievementsOverlay() override;
  // std::unique_ptr<rex::ui::AchievementNotificationDialog>
  // CreateAchievementNotificationDialog() override;
  // void OnShutdown() override {}
};
