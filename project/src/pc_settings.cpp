// PC SETTINGS screen (see pc_settings.h). Adapted from the Trials HD port's KEY BINDINGS screen.
//
// The KEYBOARD & MOUSE tab edits the key_* flags of the native PC bindings (pc_keys.cpp) by context (common, on foot,
// vehicles, menus), the MOUSE tab our mouse look flags (mouse_look, mouse_sensitivity, mouse_ads_sensitivity,
// mouse_invert_y: pc_mouse.cpp; vehicle_mouse_sensitivity: pc_keys.cpp), the DISPLAY tab fullscreen, present_vsync
// (host vsync) and max_fps (gameplay frame rate limit, read by bf_main/frame_rate.cpp). While it is open the game sees
// an idle controller (pad_inject.h). Flag writes take the input lock exclusively.

#include "pc_settings.h"

#include "pad_inject.h"
#include "pc_icons.h"
#include "pc_keys.h"
#include "pc_mouse.h"
#include "ui_state.h"

#include <imgui.h>
#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/keybinds.h>
#include <rex/ui/windowed_app_context.h>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <atomic>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#endif

REXCVAR_DEFINE_INT32(max_fps, 0, "BF3", "Frame rate limit while playing (0 = unlimited; menus and loading keep 30)");

namespace bf3::pc_settings {
namespace {

// Every flag this screen owns (and saves): the key_* bindings (pc_keys.cpp) plus mouse and display flags.
const std::vector<std::string>& OwnedFlags() {
  static const std::vector<std::string> flags = [] {
    std::vector<std::string> out;
    for (const pc_keys::Action& a : pc_keys::Actions()) out.push_back(a.flag);
    for (const char* f : {"mouse_look", "mouse_sensitivity", "mouse_ads_sensitivity", "mouse_invert_y",
                          "vehicle_mouse_sensitivity", "fullscreen", "present_vsync", "max_fps"}) {
      out.push_back(f);
    }
    return out;
  }();
  return flags;
}

// Keys conflict within one context: common actions apply both on foot and in vehicles; menus are separate.
bool SameContext(pc_keys::Section a, pc_keys::Section b) {
  using S = pc_keys::Section;
  if (a == S::kMenus || b == S::kMenus) return a == b;
  return a == b || a == S::kCommon || b == S::kCommon;
}

constexpr int kSlots = 3;
using Slots = std::array<std::string, kSlots>;

Slots Split(const std::string& value) {
  Slots out;
  std::stringstream ss(value);
  std::string key;
  for (int i = 0; i < kSlots && std::getline(ss, key, ',');) {
    if (!key.empty()) out[i++] = key;
  }
  return out;
}

std::string Join(const Slots& slots) {
  std::string out;
  for (const std::string& k : slots) {
    if (k.empty()) continue;
    if (!out.empty()) out += ',';
    out += k;
  }
  return out;
}

std::string DefaultFor(const std::string& flag) {
  for (const pc_keys::Action& a : pc_keys::Actions()) {
    if (flag == a.flag) return a.defaults;
  }
  for (const auto& entry : rex::cvar::GetRegistry()) {
    if (entry.name == flag) return entry.default_value;
  }
  return {};
}

// BF3 menu palette: near-black navy panels, white condensed text, the orange of the game's dialog headers.
constexpr ImU32 kOrange = IM_COL32(241, 121, 32, 255);
const ImVec4 kOrangeV(0.945f, 0.475f, 0.125f, 1.0f);
const ImVec4 kText(0.95f, 0.96f, 0.98f, 1.0f);
const ImVec4 kTextDim(0.55f, 0.62f, 0.70f, 1.0f);
const ImVec4 kPanel(0.02f, 0.035f, 0.055f, 0.97f);
const ImVec4 kSlot(0.09f, 0.13f, 0.18f, 1.0f);
const ImVec4 kSlotHover(0.16f, 0.23f, 0.31f, 1.0f);
const ImVec4 kSlotActive(0.94f, 0.95f, 0.96f, 1.0f);

enum class Tab { kBindings, kMouse, kDisplay };

int g_test_tab = -1;  // BF3_TEST_PC_SETTINGS=<tab>: open on that tab when an Options screen first shows (screenshots)

class SettingsDialog final : public rex::ui::ImGuiDialog {
 public:
  explicit SettingsDialog(rex::ui::ImGuiDrawer* drawer) : ImGuiDialog(drawer) {
    if (g_test_tab >= 0 && g_test_tab < 3) tab_ = Tab(g_test_tab);
    for (const std::string& flag : OwnedFlags()) {
      if (flag.rfind("key_", 0) == 0) working_[flag] = Split(rex::cvar::GetFlagByName(flag));
    }
    mouse_look_ = rex::cvar::GetFlagByName("mouse_look") == "true";
    sensitivity_ = std::stof(rex::cvar::GetFlagByName("mouse_sensitivity"));
    ads_sensitivity_ = std::stof(rex::cvar::GetFlagByName("mouse_ads_sensitivity"));
    invert_y_ = rex::cvar::GetFlagByName("mouse_invert_y") == "true";
    vehicle_sensitivity_ = std::stof(rex::cvar::GetFlagByName("vehicle_mouse_sensitivity"));
    fullscreen_ = rex::cvar::GetFlagByName("fullscreen") == "true";
    vsync_ = rex::cvar::GetFlagByName("present_vsync") == "true";
    max_fps_ = std::atoi(rex::cvar::GetFlagByName("max_fps").c_str());
    SetGameInputBlocked(true);
  }
  ~SettingsDialog() override { SetGameInputBlocked(false); }

 protected:
  void OnDraw(ImGuiIO& io) override;

 private:
  void DrawBindings(float scale);
  void DrawMouse(float scale);
  void DrawDisplay(float scale);
  void CaptureKey();
  std::string UsersOf(const std::string& key, const pc_keys::Action& except) const;
  void RestoreDefaults();
  void Save();
  void RequestClose();

  Tab tab_ = Tab::kBindings;
  std::map<std::string, Slots> working_;
  bool mouse_look_ = true;
  float sensitivity_ = 1.0f;
  float ads_sensitivity_ = 0.6f;
  bool invert_y_ = false;
  float vehicle_sensitivity_ = 1.0f;
  bool fullscreen_ = false;
  bool vsync_ = false;
  int max_fps_ = 0;
  std::string listen_flag_;
  int listen_slot_ = -1;
  bool armed_ = false;         // all keys released since listening started
  bool wait_release_ = false;  // swallow the key that just got bound
  std::string status_;
};

std::unique_ptr<SettingsDialog> g_dialog;
ImFont* g_title_font = nullptr;
ImFont* g_body_font = nullptr;
std::atomic<bool> g_open{false};
rex::ui::ImGuiDrawer* g_drawer = nullptr;
rex::ui::WindowedAppContext* g_app_context = nullptr;
std::filesystem::path g_config_path;

void SettingsDialog::RequestClose() {
  listen_slot_ = -1;
  g_app_context->CallInUIThreadDeferred([] {
    g_dialog.reset();
    g_open = false;
  });
}

std::string SettingsDialog::UsersOf(const std::string& key, const pc_keys::Action& except) const {
  std::string out;
  for (const pc_keys::Action& a : pc_keys::Actions()) {
    if (a.flag == except.flag || !SameContext(a.section, except.section)) continue;
    const Slots& slots = working_.at(a.flag);
    if (std::find(slots.begin(), slots.end(), key) == slots.end()) continue;
    out += (out.empty() ? "" : ", ") + std::string(a.label);
  }
  return out;
}

void SettingsDialog::CaptureKey() {
#if defined(_WIN32)
  auto bind = [&](const std::string& name) {
    working_[listen_flag_][listen_slot_] = name;
    for (const pc_keys::Action& a : pc_keys::Actions()) {
      if (listen_flag_ != a.flag) continue;
      std::string others = UsersOf(name, a);
      status_ = others.empty() ? std::string() : name + " is also bound to " + others + ".";
    }
    listen_slot_ = -1;
  };
  if (std::string wheel = pc_keys::TakeRecentWheel(); !wheel.empty() && armed_) {
    bind(wheel);
    return;
  }
  bool any_down = false;
  for (int vk = 0x01; vk <= 0xFE; ++vk) {
    if (vk == VK_SHIFT || vk == VK_CONTROL || vk == VK_MENU) continue;  // use the L/R variants
    if (!(GetAsyncKeyState(vk) & 0x8000)) continue;
    any_down = true;
    if (!armed_) continue;
    armed_ = false;
    wait_release_ = true;
    if (vk == VK_ESCAPE && GetAsyncKeyState(VK_SHIFT) & 0x8000) {  // Shift+Esc cancels; Esc alone binds Esc
      listen_slot_ = -1;
      status_.clear();
      return;
    }
    const std::string name = pc_keys::KeyName(vk);
    if (name.empty()) {
      status_ = "That key can't be bound.";
      return;
    }
    bind(name);
    return;
  }
  if (!any_down) {
    armed_ = true;
    pc_keys::TakeRecentWheel();  // a notch from before listening doesn't count
  }
#endif
}

void SettingsDialog::RestoreDefaults() {
  for (auto& [flag, slots] : working_) slots = Split(DefaultFor(flag));
  mouse_look_ = true;
  sensitivity_ = 1.0f;
  ads_sensitivity_ = 0.6f;
  invert_y_ = false;
  vehicle_sensitivity_ = 1.0f;
  fullscreen_ = false;
  vsync_ = false;
  max_fps_ = 0;
  status_ = "Defaults restored. Save to keep them.";
}

// Writes only this screen's keys to the config, keeping every other line as it was. (rex::cvar::SaveConfig would also
// persist command-line overrides such as log_file or user_data_root.)
bool WriteOwnedSettings(const std::filesystem::path& path, const std::map<std::string, std::string>& values) {
  std::vector<std::string> lines;
  {
    std::ifstream in(path);
    for (std::string line; std::getline(in, line);) lines.push_back(line);
  }
  auto quoted = [](const std::string& v) {
    std::string out = "\"";
    for (char c : v) {
      if (c == '"' || c == '\\') out += '\\';
      out += c;
    }
    return out + "\"";
  };
  auto render = [&](const std::string& flag, const std::string& value) {
    bool literal = value == "true" || value == "false" || (!value.empty() && (isdigit(value[0]) || value[0] == '.'));
    return flag + " = " + (literal ? value : quoted(value));
  };
  std::map<std::string, std::string> pending = values;
  for (std::string& line : lines) {
    size_t start = line.find_first_not_of(" \t");
    if (start == std::string::npos) continue;
    size_t end = line.find_first_of(" \t=", start);
    auto it = pending.find(line.substr(start, end - start));
    if (it == pending.end()) continue;
    line = render(it->first, it->second);
    pending.erase(it);
  }
  if (lines.empty()) {
    lines.push_back("# bf3 settings (PC SETTINGS screen writes key_*, mouse_*, fullscreen, present_vsync, max_fps)");
  }
  for (const auto& [flag, value] : pending) lines.push_back(render(flag, value));
  std::ofstream out(path, std::ios::trunc);
  for (const std::string& line : lines) out << line << '\n';
  return bool(out);
}

void SettingsDialog::Save() {
  std::map<std::string, std::string> values;
  for (const auto& [flag, slots] : working_) values[flag] = Join(slots);
  values["mouse_look"] = mouse_look_ ? "true" : "false";
  values["mouse_sensitivity"] = fmt::format("{:.2f}", sensitivity_);
  values["mouse_ads_sensitivity"] = fmt::format("{:.2f}", ads_sensitivity_);
  values["mouse_invert_y"] = invert_y_ ? "true" : "false";
  values["vehicle_mouse_sensitivity"] = fmt::format("{:.2f}", vehicle_sensitivity_);
  values["fullscreen"] = fullscreen_ ? "true" : "false";
  values["present_vsync"] = vsync_ ? "true" : "false";
  values["max_fps"] = std::to_string(max_fps_);
  // Older versions of this screen saved the guest vblank flag here; it now always runs unthrottled (pc_input.cpp).
  values["vsync"] = "false";
  {
    std::unique_lock lock(InputLock());
    for (const auto& [flag, value] : values) {
      if (!rex::cvar::SetFlagByName(flag, value)) REXLOG_WARN("pc settings: could not set {} = {}", flag, value);
    }
  }
  pc_icons::RequestRefresh();
  if (WriteOwnedSettings(g_config_path, values)) {
    REXLOG_INFO("pc settings: saved to {}", g_config_path.string());
  } else {
    REXLOG_WARN("pc settings: could not write {}", g_config_path.string());
  }
}

void SettingsDialog::DrawBindings(float scale) {
  bool busy = listen_slot_ >= 0 || wait_release_;
  if (!ImGui::BeginTable("##binds", 1 + kSlots, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) return;
  ImGui::TableSetupColumn("Action", ImGuiTableColumnFlags_WidthStretch, 1.6f);
  for (int i = 0; i < kSlots; ++i) ImGui::TableSetupColumn("Key", ImGuiTableColumnFlags_WidthStretch, 0.7f);
  using pc_keys::Section;
  for (Section section : {Section::kCommon, Section::kInfantry, Section::kVehicle, Section::kMenus}) {
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGui::Dummy(ImVec2(0, 6 * scale));
    ImGui::TextColored(kOrangeV, "%s", pc_keys::SectionTitle(section));
    static const char* kSlotNames[kSlots] = {"PRIMARY", "SECONDARY", "THIRD"};
    for (int i = 0; i < kSlots; ++i) {
      ImGui::TableSetColumnIndex(1 + i);
      ImGui::Dummy(ImVec2(0, 6 * scale));
      ImGui::TextColored(kTextDim, "%s", kSlotNames[i]);
    }
    for (const pc_keys::Action& a : pc_keys::Actions()) {
      if (a.section != section) continue;
      ImGui::TableNextRow();
      ImGui::PushID(a.flag);
      ImGui::TableSetColumnIndex(0);
      ImGui::AlignTextToFramePadding();
      ImGui::TextUnformatted(a.label);
      Slots& slots = working_[a.flag];
      for (int i = 0; i < kSlots; ++i) {
        ImGui::TableSetColumnIndex(1 + i);
        ImGui::PushID(i);
        bool listening = listen_slot_ == i && listen_flag_ == a.flag;
        std::string label = listening ? "PRESS A KEY..." : slots[i].empty() ? "-" : slots[i];
        std::string shared = listening || slots[i].empty() ? std::string() : UsersOf(slots[i], a);
        if (listening) {
          ImGui::PushStyleColor(ImGuiCol_Button, kSlotActive);
          ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.02f, 0.03f, 0.05f, 1.0f));
        }
        if (!shared.empty()) ImGui::PushStyleColor(ImGuiCol_Text, kOrangeV);
        ImGui::BeginDisabled(busy && !listening);
        if (ImGui::Button((label + "##slot").c_str(), ImVec2(-FLT_MIN, 0)) && !busy) {
          listen_flag_ = a.flag;
          listen_slot_ = i;
          armed_ = false;
          status_.clear();
        }
        if (!busy && ImGui::IsItemClicked(ImGuiMouseButton_Right)) slots[i].clear();
        ImGui::EndDisabled();
        if (!shared.empty()) {
          ImGui::PopStyleColor();
          ImGui::SetItemTooltip("Also bound to %s", shared.c_str());
        }
        if (listening) ImGui::PopStyleColor(2);
        ImGui::PopID();
      }
      ImGui::PopID();
    }
  }
  ImGui::EndTable();
}

void SettingsDialog::DrawMouse(float scale) {
  ImGui::Dummy(ImVec2(0, 8 * scale));
  ImGui::TextColored(kOrangeV, "MOUSE");
  ImGui::Checkbox("Mouse look (mouse moves the camera; the cursor is captured while playing)", &mouse_look_);
  ImGui::Dummy(ImVec2(0, 6 * scale));
  ImGui::TextUnformatted("Sensitivity");
  ImGui::SetNextItemWidth(420 * scale);
  ImGui::SliderFloat("##sens", &sensitivity_, 0.10f, 5.0f, "%.2f");
  ImGui::Dummy(ImVec2(0, 6 * scale));
  ImGui::TextUnformatted("Aiming down sights (scale while the aim button is held)");
  ImGui::SetNextItemWidth(420 * scale);
  ImGui::SliderFloat("##ads", &ads_sensitivity_, 0.10f, 1.50f, "%.2f");
  ImGui::Dummy(ImVec2(0, 6 * scale));
  ImGui::Checkbox("Invert vertical look", &invert_y_);
  ImGui::Dummy(ImVec2(0, 6 * scale));
  ImGui::TextUnformatted("Vehicles and aircraft (turret aim, pitch and roll)");
  ImGui::SetNextItemWidth(420 * scale);
  ImGui::SliderFloat("##veh", &vehicle_sensitivity_, 0.10f, 5.0f, "%.2f");
  ImGui::Dummy(ImVec2(0, 10 * scale));
  ImGui::TextColored(kTextDim, "Mouse look is raw input with no acceleration, like BF3 on PC. The game's own");
  ImGui::TextColored(kTextDim, "stick sensitivity and invert options only affect a controller.");
}

void SettingsDialog::DrawDisplay(float scale) {
  ImGui::Dummy(ImVec2(0, 8 * scale));
  ImGui::TextColored(kOrangeV, "DISPLAY");
  ImGui::Checkbox("Fullscreen", &fullscreen_);
  ImGui::Checkbox("Vertical sync (no tearing)", &vsync_);
  ImGui::Dummy(ImVec2(0, 6 * scale));
  ImGui::TextUnformatted("Frame rate limit (in game)");
  static constexpr int kLimits[] = {0, 30, 60, 75, 120, 144, 165, 240};
  auto label = [](int fps) { return fps ? std::to_string(fps) + " FPS" : std::string("Unlimited"); };
  ImGui::SetNextItemWidth(260 * scale);
  if (ImGui::BeginCombo("##max_fps", label(max_fps_).c_str())) {
    for (int fps : kLimits) {
      if (ImGui::Selectable(label(fps).c_str(), fps == max_fps_)) max_fps_ = fps;
    }
    ImGui::EndCombo();
  }
  ImGui::Dummy(ImVec2(0, 10 * scale));
  ImGui::TextColored(kTextDim, "Menus and loading screens run at 30 FPS like the console.");
  ImGui::TextColored(kTextDim, "Brightness and HUD placement are in the game's OPTIONS > VIDEO screen.");
}

void SettingsDialog::OnDraw(ImGuiIO& io) {
  if (listen_slot_ >= 0) CaptureKey();
#if defined(_WIN32)
  if (wait_release_ && listen_slot_ < 0) {
    bool any = false;
    for (int vk = 0x01; vk <= 0xFE && !any; ++vk) any = GetAsyncKeyState(vk) & 0x8000;
    wait_release_ = any;
  }
#endif
  bool busy = listen_slot_ >= 0 || wait_release_;

  ImGui::GetBackgroundDrawList()->AddRectFilled(ImVec2(0, 0), io.DisplaySize, IM_COL32(0, 0, 0, 170));

  float scale = std::max(1.0f, io.DisplaySize.y / 720.0f);
  ImVec2 size(std::min(io.DisplaySize.x * 0.92f, 960.0f * scale), std::min(io.DisplaySize.y * 0.92f, 660.0f * scale));
  ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_Always,
                          ImVec2(0.5f, 0.5f));
  ImGui::SetNextWindowSize(size, ImGuiCond_Always);
  ImGui::SetNextWindowFocus();

  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 0.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(28 * scale, 20 * scale));
  ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8 * scale, 4 * scale));
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8 * scale, 5 * scale));
  ImGui::PushStyleColor(ImGuiCol_WindowBg, kPanel);
  ImGui::PushStyleColor(ImGuiCol_Text, kText);
  ImGui::PushStyleColor(ImGuiCol_Button, kSlot);
  ImGui::PushStyleColor(ImGuiCol_ButtonHovered, kSlotHover);
  ImGui::PushStyleColor(ImGuiCol_ButtonActive, kSlotHover);
  ImGui::PushStyleColor(ImGuiCol_FrameBg, kSlot);
  ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, kSlotHover);
  ImGui::PushStyleColor(ImGuiCol_SliderGrab, kOrangeV);
  ImGui::PushStyleColor(ImGuiCol_SliderGrabActive, kOrangeV);
  ImGui::PushStyleColor(ImGuiCol_CheckMark, kOrangeV);
  ImGui::PushStyleColor(ImGuiCol_Separator, ImVec4(0.25f, 0.32f, 0.40f, 1.0f));
  ImGui::PushStyleColor(ImGuiCol_NavHighlight, kOrangeV);
  ImGui::PushStyleColor(ImGuiCol_ScrollbarBg, ImVec4(0, 0, 0, 0.2f));
  ImGui::PushStyleColor(ImGuiCol_ScrollbarGrab, kSlot);
  ImGui::PushStyleColor(ImGuiCol_TableRowBgAlt, ImVec4(1, 1, 1, 0.03f));

  if (ImGui::Begin("##pcsettings", nullptr,
                   ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings)) {
    // Orange header bar like the game's dialogs.
    ImVec2 wp = ImGui::GetWindowPos();
    ImGui::GetWindowDrawList()->AddRectFilled(wp, ImVec2(wp.x + ImGui::GetWindowWidth(), wp.y + 6 * scale), kOrange);
    ImGui::PushFont(g_body_font, 19.0f * scale);
    ImGui::TextColored(kTextDim, "MAIN / OPTIONS /");
    ImGui::PopFont();
    ImGui::PushFont(g_title_font, 44.0f * scale);
    ImGui::TextUnformatted("PC SETTINGS");
    ImGui::PopFont();
    ImGui::PushFont(g_body_font, 21.0f * scale);

    // Tabs (LB / RB like the game's options).
    const char* tabs[] = {"KEYBOARD & MOUSE", "MOUSE", "DISPLAY"};
    for (int i = 0; i < 3; ++i) {
      if (i) ImGui::SameLine();
      bool active = int(tab_) == i;
      ImGui::PushStyleColor(ImGuiCol_Button, active ? kSlotActive : kSlot);
      ImGui::PushStyleColor(ImGuiCol_Text, active ? ImVec4(0.02f, 0.03f, 0.05f, 1.0f) : kText);
      ImGui::BeginDisabled(busy);
      if (ImGui::Button(tabs[i], ImVec2(220 * scale, 0))) tab_ = Tab(i);
      ImGui::EndDisabled();
      ImGui::PopStyleColor(2);
    }
    if (!busy && ImGui::IsKeyPressed(ImGuiKey_PageDown, false)) tab_ = Tab((int(tab_) + 1) % 3);
    if (!busy && ImGui::IsKeyPressed(ImGuiKey_PageUp, false)) tab_ = Tab((int(tab_) + 2) % 3);
    if (tab_ == Tab::kBindings) {
      ImGui::TextColored(kTextDim, "Click a slot, then press a key, mouse button or wheel.  Right-click clears.  Shift+Esc cancels.");
    } else {
      ImGui::TextUnformatted("");
    }
    ImGui::Separator();

    float footer = ImGui::GetFrameHeightWithSpacing() * 2.4f;
    if (ImGui::BeginChild("##page", ImVec2(0, -footer), false)) {
      switch (tab_) {
        case Tab::kBindings: DrawBindings(scale); break;
        case Tab::kMouse: DrawMouse(scale); break;
        case Tab::kDisplay: DrawDisplay(scale); break;
      }
    }
    ImGui::EndChild();

    ImGui::Separator();
    ImGui::TextColored(kOrangeV, "%s", status_.c_str());
    float bw = ImGui::CalcTextSize("RESTORE DEFAULTS").x + ImGui::GetStyle().FramePadding.x * 4;
    ImGui::BeginDisabled(busy);
    if (ImGui::Button("RESTORE DEFAULTS", ImVec2(bw, 0))) RestoreDefaults();
    ImGui::SameLine(ImGui::GetWindowWidth() - 2 * bw - ImGui::GetStyle().WindowPadding.x - 8 * scale);
    bool cancel = ImGui::Button("BACK", ImVec2(bw, 0)) || (!busy && ImGui::IsKeyPressed(ImGuiKey_Escape, false));
    ImGui::SameLine();
    bool save = ImGui::Button("SAVE", ImVec2(bw, 0));
    ImGui::EndDisabled();
    if (save) {
      Save();
      RequestClose();
    } else if (cancel) {
      RequestClose();
    }
    ImGui::PopFont();
  }
  ImGui::End();
  ImGui::PopStyleColor(15);
  ImGui::PopStyleVar(6);
}

// A prompt in the corner while the game's Options screens are showing, so PC SETTINGS is found where players look.
class OptionsPrompt final : public rex::ui::ImGuiDialog {
 public:
  explicit OptionsPrompt(rex::ui::ImGuiDrawer* drawer) : ImGuiDialog(drawer) {}

 private:
  bool test_opened_ = false;
  int key_binding_pushes_ = 0;

 protected:
  void OnDraw(ImGuiIO& io) override {
    // The game's PC KEY BINDINGS tab (shown with PC menus, see bf_main/ui_mouse.cpp) edits a PC key map this build
    // doesn't use, and its keyboard/mouse columns are empty: each time it is opened, open these bindings instead.
    const int pushes = ui_state::ScreenCount("OptionsKeyBindingScreen");
    if (pushes != key_binding_pushes_) {
      key_binding_pushes_ = pushes;
      if (!g_open && ui_state::TopScreen() == "OptionsKeyBindingScreen") {
        RequestOpen();
        return;
      }
    }
    if (g_open) return;
    std::string screen = ui_state::TopScreen();
    if (screen.rfind("Options", 0) != 0) return;
    if (g_test_tab >= 0 && !test_opened_) {
      test_opened_ = true;
      RequestOpen();
      return;
    }
    float scale = std::max(1.0f, io.DisplaySize.y / 720.0f);
    const char* text = "F1   PC SETTINGS  (KEYBOARD, MOUSE, DISPLAY)";
    ImGui::PushFont(g_body_font, 20.0f * scale);
    ImVec2 ts = ImGui::CalcTextSize(text);
    ImVec2 pad(14 * scale, 8 * scale);
    // Bottom right, clear of the game's button legend at the bottom left.
    ImVec2 max(io.DisplaySize.x - 60 * scale, io.DisplaySize.y - 70 * scale);
    ImVec2 min(max.x - ts.x - pad.x * 2, max.y - ts.y - pad.y * 2);
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    dl->AddRectFilled(min, max, IM_COL32(4, 9, 14, 220));
    dl->AddRectFilled(min, ImVec2(min.x + 4 * scale, max.y), kOrange);
    dl->AddText(ImVec2(min.x + pad.x, min.y + pad.y), IM_COL32(242, 245, 250, 255), text);
    ImGui::PopFont();
    // Clickable, like the game's PC buttons.
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && ImGui::IsMouseHoveringRect(min, max, false)) RequestOpen();
  }
};

std::unique_ptr<OptionsPrompt> g_prompt;

}  // namespace

void Init(rex::ui::ImGuiDrawer* drawer, rex::ui::WindowedAppContext* app_context, std::filesystem::path config_path) {
  g_drawer = drawer;
  g_app_context = app_context;
  g_config_path = std::move(config_path);
  if (const char* t = std::getenv("BF3_TEST_PC_SETTINGS")) g_test_tab = std::atoi(t);
  g_prompt = std::make_unique<OptionsPrompt>(drawer);
  rex::ui::RegisterBind("bind_pc_settings", "F1", "Open PC SETTINGS (key bindings, mouse, display)",
                        [] {
                          if (pc_mouse::MenuMode()) RequestOpen();  // F1 is vehicle seat 1 while playing
                        });
}

void RequestOpen() {
  if (!g_drawer || g_open.exchange(true)) return;
  g_app_context->CallInUIThread([] {
    if (!g_dialog) g_dialog = std::make_unique<SettingsDialog>(g_drawer);
  });
}

bool IsOpen() { return g_open; }

ImFont* TitleFont() { return g_title_font; }
ImFont* BodyFont() { return g_body_font; }

void ConfigureFonts(ImFontAtlas* atlas) {
#if defined(_WIN32)
  char windir[MAX_PATH] = "C:\\Windows";
  GetEnvironmentVariableA("WINDIR", windir, MAX_PATH);
  std::filesystem::path path = std::filesystem::path(windir) / "Fonts" / "bahnschrift.ttf";
  if (std::filesystem::exists(path)) {
    // Baked at 1.5x the 720p layout sizes so 1080p stays sharp.
    g_title_font = atlas->AddFontFromFileTTF(path.string().c_str(), 66.0f);
    g_body_font = atlas->AddFontFromFileTTF(path.string().c_str(), 32.0f);
  }
  REXLOG_INFO("pc settings: fonts {}", g_body_font != nullptr);
#else
  (void)atlas;
#endif
}

}  // namespace bf3::pc_settings
