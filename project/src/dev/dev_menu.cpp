// In-game debug menu and stats overlay (development builds only: BF3_DEV_TOOLS; never in win-cross-ship).
//
// Insert opens the menu (the game sees no input while it is open and the cursor is free), F9 shows or hides the stats
// overlay (on by default; BF3_DEV_OVERLAY=0 starts without it), Home toggles flight. Tabs:
//   PLAYER    god mode, infinite ammo, heal, kill, respawn with a kit, team
//   MOVEMENT  flight and its speed, game speed (timescale) and pause
//   TELEPORT  built-in Metro spots, saved spots (<user_data_root>/dev_spots.txt), coordinates
//   WEAPONS   loadout overrides for the next deploy (any weapon loaded in the level), apply by respawning
//   OVERLAY   what the overlay shows: frame rate and frame times, network, position
// Everything goes through the same game commands as the debug API (bf3_BF_Main.dll bf3_dev_request, see
// bf_main/dev_cheats.cpp) and reads the frame snapshot (bf3_dev_snapshot, dev_shared.h).

#include "dev_menu.h"

#include "dev_shared.h"
#include "../pad_inject.h"
#include "../pc_settings.h"

#include <imgui.h>
#include <rex/logging.h>
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/keybinds.h>
#include <rex/ui/windowed_app_context.h>

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace bf3::dev {
namespace {

constexpr ImU32 kOrange = IM_COL32(241, 121, 32, 255);
const ImVec4 kOrangeV(0.945f, 0.475f, 0.125f, 1.0f);
const ImVec4 kText(0.95f, 0.96f, 0.98f, 1.0f);
const ImVec4 kTextDim(0.55f, 0.62f, 0.70f, 1.0f);
const ImVec4 kGood(0.45f, 0.85f, 0.45f, 1.0f);
const ImVec4 kPanel(0.02f, 0.035f, 0.055f, 0.94f);
const ImVec4 kSlot(0.09f, 0.13f, 0.18f, 1.0f);
const ImVec4 kSlotHover(0.16f, 0.23f, 0.31f, 1.0f);

template <typename Fn>
Fn DllExport(const char* name) {
  HMODULE module = GetModuleHandleW(L"bf3_BF_Main.dll");
  return module ? reinterpret_cast<Fn>(GetProcAddress(module, name)) : nullptr;
}

bool GetSnapshot(Snapshot& s) {
  static auto fn = DllExport<bool (*)(Snapshot*, uint32_t)>("bf3_dev_snapshot");
  if (!fn) fn = DllExport<bool (*)(Snapshot*, uint32_t)>("bf3_dev_snapshot");
  return fn && fn(&s, sizeof(s));
}

std::vector<float> FrameTimes() {
  static auto fn = DllExport<int (*)(float*, int)>("bf3_dev_frame_times");
  if (!fn) fn = DllExport<int (*)(float*, int)>("bf3_dev_frame_times");
  std::vector<float> ms(600);
  ms.resize(fn ? size_t(fn(ms.data(), int(ms.size()))) : 0);
  return ms;
}

// A game command (bf3_dev_request); the reply text goes to the menu's status line.
std::string Request(const std::string& line, bool* ok_out = nullptr) {
  static auto fn = DllExport<bool (*)(const char*, char*, int)>("bf3_dev_request");
  if (!fn) fn = DllExport<bool (*)(const char*, char*, int)>("bf3_dev_request");
  char text[512] = {};
  const bool ok = fn && fn(line.c_str(), text, sizeof(text));
  if (ok_out) *ok_out = ok;
  REXLOG_INFO("dev menu: {} -> {}{}", line, ok ? "" : "failed: ", fn ? text : "BF.Main not loaded");
  return fn ? text : "BF.Main is not loaded yet";
}

// Time controls live in the tool commands (bf3_dev_query, bf_main/dev_tools.cpp).
void Query(const std::string& line) {
  static auto fn = DllExport<int (*)(const char*, char*, int)>("bf3_dev_query");
  if (!fn) fn = DllExport<int (*)(const char*, char*, int)>("bf3_dev_query");
  char out[1024];
  if (fn) fn(line.c_str(), out, sizeof(out));
}

struct Spot {
  std::string name;
  float pos[3];
  float heading;
};

// Metro TDM (Frostbite world space, soldier origin; see docs/CLAUDE_SKILL.md and scripts/bf3dbg.py SPOTS).
const std::vector<Spot> kMetroSpots = {
    {"Sunny street", {-229.091f, 64.437f, -543.129f}, -1.534f},
    {"Balcony over the park", {-229.239f, 69.573f, -537.213f}, -1.381f},
};

struct WeaponEntry {
  char kind;  // p primary, s sidearm, g gadget, x grenade/knife
  std::string name;  // Weapons/<dir>/U_<...>
  std::string label;  // U_<...> without the prefix
};

std::atomic<bool> g_menu_open{false};
std::atomic<bool> g_overlay{true};
std::atomic<int> g_select_tab{-1};  // a tab to select on the next draw (SetMenuOpen)
constexpr const char* kTabs[] = {"PLAYER", "MOVEMENT", "TELEPORT", "WEAPONS", "OVERLAY"};

class DevMenu final : public rex::ui::ImGuiDialog {
 public:
  DevMenu(rex::ui::ImGuiDrawer* drawer, std::filesystem::path spots_path)
      : ImGuiDialog(drawer), spots_path_(std::move(spots_path)) {
    LoadSpots();
  }

 protected:
  void OnDraw(ImGuiIO& io) override;

 private:
  void DrawOverlay(ImGuiIO& io, const Snapshot* s, float scale);
  void DrawMenu(ImGuiIO& io, const Snapshot* s, float scale);
  void DrawPlayer(const Snapshot* s);
  void DrawMovement(const Snapshot* s);
  void DrawTeleport(const Snapshot* s);
  void DrawWeapons(const Snapshot* s);
  void DrawOverlayOptions();
  void Send(const std::string& line) { status_ = Request(line); }
  void LoadSpots();
  void SaveSpots();
  void LoadWeapons();

  std::filesystem::path spots_path_;
  std::vector<Spot> spots_;
  char spot_name_[48] = "";
  float coords_[4] = {};
  std::vector<WeaponEntry> weapons_;
  char weapon_filter_[48] = "";
  int kit_ = 0;
  float fly_speed_ = 10.0f;
  float timescale_ = 1.0f;
  bool show_fps_ = true, show_graph_ = true, show_net_ = true, show_pos_ = true;
  bool was_open_ = false;
  std::string status_;
};

void DevMenu::LoadSpots() {
  spots_.clear();
  std::ifstream in(spots_path_);
  for (std::string line; std::getline(in, line);) {
    std::istringstream ls(line);
    Spot s{};
    if (!(ls >> s.pos[0] >> s.pos[1] >> s.pos[2] >> s.heading)) continue;
    std::getline(ls >> std::ws, s.name);
    spots_.push_back(s);
  }
}

void DevMenu::SaveSpots() {
  std::error_code ec;
  std::filesystem::create_directories(spots_path_.parent_path(), ec);
  std::ofstream out(spots_path_, std::ios::trunc);
  for (const Spot& s : spots_) {
    out << s.pos[0] << ' ' << s.pos[1] << ' ' << s.pos[2] << ' ' << s.heading << ' ' << s.name << '\n';
  }
  status_ = out ? "Spots saved." : "Could not write " + spots_path_.string();
}

void DevMenu::LoadWeapons() {
  static auto fn = DllExport<int (*)(char*, int)>("bf3_dev_weapons");
  if (!fn) fn = DllExport<int (*)(char*, int)>("bf3_dev_weapons");
  weapons_.clear();
  if (!fn) return;
  std::string list(32768, '\0');
  list.resize(size_t(std::max(0, fn(list.data(), int(list.size())))));
  std::stringstream ss(list);
  for (std::string e; std::getline(ss, e, ';');) {
    if (e.size() < 3 || e[1] != ':') continue;
    WeaponEntry w{e[0], e.substr(2), e.substr(e.rfind('/') + 1)};
    if (w.label.rfind("U_", 0) == 0) w.label = w.label.substr(2);
    weapons_.push_back(std::move(w));
  }
  std::sort(weapons_.begin(), weapons_.end(), [](const auto& a, const auto& b) { return a.label < b.label; });
}

// Heading of the soldier (as teleport takes it) from the snapshot transform's forward row.
float Heading(const Snapshot& s) { return std::atan2(s.transform[8], s.transform[10]); }

void DevMenu::OnDraw(ImGuiIO& io) {
  Snapshot snap{};
  const Snapshot* s = GetSnapshot(snap) ? &snap : nullptr;
  const float scale = std::max(1.0f, io.DisplaySize.y / 720.0f) * 0.8f;
  const bool open = g_menu_open;
  if (open != was_open_) {
    SetGameInputBlocked(open);
    was_open_ = open;
    if (open && s) {
      fly_speed_ = s->fly_speed;
      timescale_ = s->timescale;
      if (s->kit >= 0) kit_ = s->kit;
    }
  }
  if (g_overlay) DrawOverlay(io, s, scale);
  if (open) DrawMenu(io, s, scale);
}

void DevMenu::DrawOverlay(ImGuiIO& io, const Snapshot* s, float scale) {
  ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x - 12 * scale, 12 * scale), ImGuiCond_Always, ImVec2(1, 0));
  ImGui::SetNextWindowBgAlpha(0.6f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10 * scale, 8 * scale));
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(6 * scale, 2 * scale));
  ImFont* font = pc_settings::BodyFont();
  if (font) ImGui::PushFont(font, 15.0f * scale);
  constexpr ImGuiWindowFlags kFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs |
                                      ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
                                      ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav;
  if (ImGui::Begin("##devoverlay", nullptr, kFlags)) {
    ImVec2 wp = ImGui::GetWindowPos();
    ImGui::GetWindowDrawList()->AddRectFilled(wp, ImVec2(wp.x + 3 * scale, wp.y + ImGui::GetWindowHeight()), kOrange);
    if (show_fps_) {
      std::vector<float> ms = FrameTimes();
      double total = 0;
      size_t n = 0;
      for (auto it = ms.rbegin(); it != ms.rend() && total < 1000.0; ++it, ++n) total += *it;
      std::vector<float> recent(ms.end() - n, ms.end());
      std::sort(recent.begin(), recent.end());
      const float fps = total > 0 ? float(1000.0 * double(n) / total) : 0.0f;
      const float p99 = recent.empty() ? 0.0f : recent[std::min(recent.size() - 1, size_t(0.99 * recent.size()))];
      const float worst = recent.empty() ? 0.0f : recent.back();
      ImGui::TextColored(kOrangeV, "%.0f FPS", fps);
      ImGui::SameLine();
      ImGui::Text("%.2f ms  p99 %.1f  max %.1f  present %.0f", n ? total / double(n) : 0.0, p99, worst,
                  io.Framerate);
      if (show_graph_ && ms.size() > 1) {
        const size_t count = std::min<size_t>(ms.size(), 240);
        float peak = 1.0f;
        for (size_t i = ms.size() - count; i < ms.size(); ++i) peak = std::max(peak, ms[i]);
        ImGui::PushStyleColor(ImGuiCol_PlotLines, kOrangeV);
        ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0, 0, 0, 0.35f));
        char label[32];
        std::snprintf(label, sizeof(label), "%.0f ms", peak);
        ImGui::PlotLines("##frames", ms.data() + (ms.size() - count), int(count), 0, label, 0.0f, peak,
                         ImVec2(300 * scale, 44 * scale));
        ImGui::PopStyleColor(2);
      }
    }
    if (show_net_ && s) {
      if (show_fps_) ImGui::Separator();
      const auto& c = s->net_client;
      const auto& v = s->net_server;
      ImGui::TextColored(kOrangeV, "NET");
      ImGui::SameLine();
      ImGui::TextColored(kTextDim, "local server <-> client (in-memory socket)");
      ImGui::Text("up    %5.1f pkt/s %6.2f KB/s  latency %4.1f ms (max %.1f)", c.tx_packets, c.tx_kbytes,
                  v.latency_ms, v.latency_max_ms);
      ImGui::Text("down  %5.1f pkt/s %6.2f KB/s  latency %4.1f ms (max %.1f)", v.tx_packets, v.tx_kbytes,
                  c.latency_ms, c.latency_max_ms);
      ImGui::Text("msgs  %.0f up / %.0f down per s   tick server %.0f Hz  client %.0f Hz", s->msg_up, s->msg_down,
                  s->server_ticks, s->client_ticks);
    }
    if (show_pos_ && s && s->soldier) {
      ImGui::Separator();
      ImGui::Text("pos %.2f %.2f %.2f  heading %.3f  hp %.0f", s->transform[12], s->transform[13], s->transform[14],
                  Heading(*s), s->server_soldier ? s->server_health : s->health);
    }
    if (s && (s->god || s->infinite_ammo || s->flying)) {
      std::string flags;
      if (s->god) flags += "GOD  ";
      if (s->infinite_ammo) flags += "AMMO  ";
      if (s->flying) flags += "FLY";
      ImGui::TextColored(kGood, "%s", flags.c_str());
    }
  }
  ImGui::End();
  if (font) ImGui::PopFont();
  ImGui::PopStyleVar(4);
}

void DevMenu::DrawMenu(ImGuiIO& io, const Snapshot* s, float scale) {
  ImGui::SetNextWindowPos(ImVec2(24 * scale, 24 * scale), ImGuiCond_Always);
  ImGui::SetNextWindowSize(ImVec2(std::min(io.DisplaySize.x - 48 * scale, 620 * scale),
                                  std::min(io.DisplaySize.y - 48 * scale, 560 * scale)),
                           ImGuiCond_Always);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 0.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(18 * scale, 14 * scale));
  ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8 * scale, 4 * scale));
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8 * scale, 6 * scale));
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
  ImGui::PushStyleColor(ImGuiCol_Tab, kSlot);
  ImGui::PushStyleColor(ImGuiCol_TabHovered, kSlotHover);
  ImGui::PushStyleColor(ImGuiCol_TabSelected, ImVec4(0.80f, 0.40f, 0.10f, 1.0f));
  ImGui::PushStyleColor(ImGuiCol_Header, kSlot);
  ImGui::PushStyleColor(ImGuiCol_HeaderHovered, kSlotHover);
  ImFont* title = pc_settings::TitleFont();
  ImFont* body = pc_settings::BodyFont();
  if (ImGui::Begin("##devmenu", nullptr,
                   ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings)) {
    ImVec2 wp = ImGui::GetWindowPos();
    ImGui::GetWindowDrawList()->AddRectFilled(wp, ImVec2(wp.x + ImGui::GetWindowWidth(), wp.y + 5 * scale), kOrange);
    if (title) ImGui::PushFont(title, 34.0f * scale);
    ImGui::TextUnformatted("DEBUG MENU");
    if (title) ImGui::PopFont();
    if (body) ImGui::PushFont(body, 19.0f * scale);
    ImGui::TextColored(kTextDim, "Development build.  Insert / Esc close   F9 overlay   Home fly");
    if (ImGui::BeginTabBar("##devtabs")) {
      const int select = g_select_tab.exchange(-1);
      auto tab = [select](const char* name) {
        const bool chosen = select >= 0 && std::string_view(kTabs[select]) == name;
        return ImGui::BeginTabItem(name, nullptr, chosen ? ImGuiTabItemFlags_SetSelected : 0);
      };
      if (tab("PLAYER")) {
        DrawPlayer(s);
        ImGui::EndTabItem();
      }
      if (tab("MOVEMENT")) {
        DrawMovement(s);
        ImGui::EndTabItem();
      }
      if (tab("TELEPORT")) {
        DrawTeleport(s);
        ImGui::EndTabItem();
      }
      if (tab("WEAPONS")) {
        DrawWeapons(s);
        ImGui::EndTabItem();
      }
      if (tab("OVERLAY")) {
        DrawOverlayOptions();
        ImGui::EndTabItem();
      }
      ImGui::EndTabBar();
    }
    ImGui::SetCursorPosY(ImGui::GetWindowHeight() - ImGui::GetFrameHeightWithSpacing() - 10 * scale);
    ImGui::Separator();
    ImGui::TextColored(kOrangeV, "%s", status_.c_str());
    if (body) ImGui::PopFont();
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) g_menu_open = false;
  }
  ImGui::End();
  ImGui::PopStyleColor(15);
  ImGui::PopStyleVar(6);
}

void DevMenu::DrawPlayer(const Snapshot* s) {
  if (!s || !s->level_loaded) {
    ImGui::TextColored(kTextDim, "Not in a level.");
    return;
  }
  ImGui::TextColored(kTextDim, "%s", s->level);
  static const char* kTeams[] = {"neutral", "US", "RU"};
  static const char* kKits[] = {"Assault", "Engineer", "Support", "Recon"};
  if (s->soldier) {
    ImGui::Text("Health %.0f   Team %s   Kit %s", s->server_soldier ? s->server_health : s->health,
                s->team < 3 ? kTeams[s->team] : "?", s->kit >= 0 && s->kit < 4 ? kKits[s->kit] : "-");
    ImGui::Text("Weapon %s  %d / %d", s->weapon, s->ammo_mag, s->ammo_reserve);
  } else {
    ImGui::TextColored(kTextDim, s->respawning ? "Respawning..." : "Not spawned.");
  }
  ImGui::Separator();
  bool god = s->god, ammo = s->infinite_ammo;
  if (ImGui::Checkbox("God mode", &god)) Send(god ? "god on" : "god off");
  ImGui::SameLine(220);
  if (ImGui::Checkbox("Infinite ammo", &ammo)) Send(ammo ? "ammo on" : "ammo off");
  if (ImGui::Button("HEAL")) Send("heal");
  ImGui::SameLine();
  if (ImGui::Button("KILL")) Send("kill");
  ImGui::Separator();
  ImGui::SetNextItemWidth(200);
  ImGui::Combo("##kit", &kit_, kKits, 4);
  ImGui::SameLine();
  if (ImGui::Button("RESPAWN")) Send("respawn " + std::to_string(kit_));
  ImGui::TextColored(kTextDim, "Respawn kills the soldier (if alive) and deploys again with this kit.");
  ImGui::Separator();
  ImGui::TextUnformatted("Team");
  ImGui::SameLine();
  if (ImGui::Button("US MARINES")) Send("team us");
  ImGui::SameLine();
  if (ImGui::Button("RUSSIAN ARMY")) Send("team ru");
}

void DevMenu::DrawMovement(const Snapshot* s) {
  bool fly = s && s->flying;
  if (ImGui::Checkbox("Fly (noclip)", &fly)) Send(fly ? "fly on" : "fly off");
  ImGui::SetNextItemWidth(320);
  if (ImGui::SliderFloat("Speed (m/s)", &fly_speed_, 1.0f, 100.0f, "%.0f", ImGuiSliderFlags_Logarithmic)) {
    Send("flyspeed " + std::to_string(fly_speed_));
  }
  ImGui::TextColored(kTextDim, "Move keys fly along the view, Jump goes up, Crouch down, Sprint x4.");
  ImGui::TextColored(kTextDim, "No collisions or gravity. Falls don't hurt for 8 s after landing a flight.");
  ImGui::Separator();
  ImGui::TextUnformatted("Game speed");
  ImGui::SetNextItemWidth(320);
  if (ImGui::SliderFloat("##timescale", &timescale_, 0.1f, 4.0f, "%.2fx", ImGuiSliderFlags_Logarithmic)) {
    Query("timescale " + std::to_string(timescale_));
  }
  ImGui::SameLine();
  if (ImGui::Button("1x")) {
    timescale_ = 1.0f;
    Query("timescale 1");
  }
  const bool paused = s && s->paused;
  if (ImGui::Button(paused ? "RESUME" : "PAUSE")) Query(paused ? "resume" : "pause");
  ImGui::SameLine();
  ImGui::BeginDisabled(!paused);
  if (ImGui::Button("STEP 1 FRAME")) Query("step 1");
  ImGui::EndDisabled();
}

void DevMenu::DrawTeleport(const Snapshot* s) {
  const bool alive = s && s->server_soldier;
  auto go = [&](const float* pos, float heading) {
    char line[160];
    std::snprintf(line, sizeof(line), "teleport %.3f %.3f %.3f %.4f", pos[0], pos[1], pos[2], heading);
    Send(line);
  };
  if (s && s->soldier) {
    ImGui::Text("Position %.2f %.2f %.2f   heading %.3f", s->transform[12], s->transform[13], s->transform[14],
                Heading(*s));
  }
  ImGui::BeginDisabled(!alive);
  ImGui::TextColored(kOrangeV, "METRO");
  for (const Spot& spot : kMetroSpots) {
    ImGui::PushID(spot.name.c_str());
    if (ImGui::Button("GO")) go(spot.pos, spot.heading);
    ImGui::SameLine();
    ImGui::TextUnformatted(spot.name.c_str());
    ImGui::PopID();
  }
  ImGui::TextColored(kOrangeV, "SAVED");
  int remove = -1;
  for (size_t i = 0; i < spots_.size(); ++i) {
    ImGui::PushID(int(i));
    if (ImGui::Button("GO")) go(spots_[i].pos, spots_[i].heading);
    ImGui::SameLine();
    ImGui::EndDisabled();
    if (ImGui::Button("X")) remove = int(i);
    ImGui::BeginDisabled(!alive);
    ImGui::SameLine();
    ImGui::Text("%s  (%.1f, %.1f, %.1f)", spots_[i].name.c_str(), spots_[i].pos[0], spots_[i].pos[1],
                spots_[i].pos[2]);
    ImGui::PopID();
  }
  ImGui::EndDisabled();
  if (remove >= 0) {
    spots_.erase(spots_.begin() + remove);
    SaveSpots();
  }
  ImGui::SetNextItemWidth(260);
  ImGui::InputTextWithHint("##spotname", "name", spot_name_, sizeof(spot_name_));
  ImGui::SameLine();
  ImGui::BeginDisabled(!(s && s->soldier));
  if (ImGui::Button("SAVE CURRENT")) {
    Spot spot{spot_name_[0] ? spot_name_ : "spot " + std::to_string(spots_.size() + 1),
              {s->transform[12], s->transform[13], s->transform[14]},
              Heading(*s)};
    spots_.push_back(spot);
    spot_name_[0] = 0;
    SaveSpots();
  }
  ImGui::EndDisabled();
  ImGui::Separator();
  ImGui::TextUnformatted("Coordinates (x, y up, z, heading)");
  ImGui::SetNextItemWidth(420);
  ImGui::InputFloat4("##coords", coords_, "%.2f");
  ImGui::SameLine();
  ImGui::BeginDisabled(!alive);
  if (ImGui::Button("GO##coords")) go(coords_, coords_[3]);
  ImGui::EndDisabled();
  if (s && s->soldier && ImGui::Button("USE CURRENT")) {
    std::copy(s->transform + 12, s->transform + 15, coords_);
    coords_[3] = Heading(*s);
  }
}

void DevMenu::DrawWeapons(const Snapshot* s) {
  if (weapons_.empty() && s && s->level_loaded) LoadWeapons();
  static const char* kSlotNames[] = {"Primary", "Sidearm", "Gadget 1", "Gadget 2"};
  static const char* kSlotCmds[] = {"primary", "sidearm", "gadget1", "gadget2"};
  static const char kSlotKinds[] = {'p', 's', 'g', 'g'};
  if (s && s->soldier) ImGui::Text("Current: %s  %d / %d", s->weapon, s->ammo_mag, s->ammo_reserve);
  bool ammo = s && s->infinite_ammo;
  if (ImGui::Checkbox("Infinite ammo", &ammo)) Send(ammo ? "ammo on" : "ammo off");
  ImGui::SetNextItemWidth(260);
  ImGui::InputTextWithHint("##wfilter", "filter (e.g. M4, SV98)", weapon_filter_, sizeof(weapon_filter_));
  ImGui::SameLine();
  if (ImGui::Button("RELOAD LIST")) LoadWeapons();
  std::string filter = weapon_filter_;
  std::transform(filter.begin(), filter.end(), filter.begin(), ::tolower);
  for (int slot = 0; slot < 4; ++slot) {
    const std::string current = s ? s->weapon_override[slot] : "";
    const std::string preview = current.empty() ? "(kit default)" : current.substr(current.rfind('/') + 1);
    ImGui::SetNextItemWidth(360);
    if (ImGui::BeginCombo(kSlotNames[slot], preview.c_str(), ImGuiComboFlags_HeightLarge)) {
      if (ImGui::Selectable("(kit default)", current.empty())) Send(std::string("weapon ") + kSlotCmds[slot] + " none");
      // The slot's own kind first, then everything else (any unlock can go in any slot).
      for (int pass = 0; pass < 2; ++pass) {
        if (pass == 1) ImGui::Separator();
        for (const WeaponEntry& w : weapons_) {
          if ((w.kind == kSlotKinds[slot]) != (pass == 0)) continue;
          std::string lower = w.name;
          std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
          if (!filter.empty() && lower.find(filter) == std::string::npos) continue;
          if (ImGui::Selectable(w.label.c_str(), w.name == current)) {
            Send(std::string("weapon ") + kSlotCmds[slot] + " " + w.name);
          }
        }
      }
      ImGui::EndCombo();
    }
  }
  ImGui::TextColored(kTextDim, "Overrides apply at the next deploy, without attachments.");
  if (ImGui::Button("APPLY NOW (RESPAWN)")) Send("respawn " + std::to_string(s && s->kit >= 0 ? s->kit : kit_));
  ImGui::SameLine();
  if (ImGui::Button("CLEAR ALL")) {
    for (const char* slot : kSlotCmds) Send(std::string("weapon ") + slot + " none");
  }
  ImGui::TextColored(kTextDim, "%zu weapon unlocks loaded in this level.", weapons_.size());
}

void DevMenu::DrawOverlayOptions() {
  bool overlay = g_overlay;
  if (ImGui::Checkbox("Show overlay (F9)", &overlay)) g_overlay = overlay;
  ImGui::Checkbox("Frame rate and frame times", &show_fps_);
  ImGui::Checkbox("Frame time graph", &show_graph_);
  ImGui::Checkbox("Network", &show_net_);
  ImGui::Checkbox("Position and health", &show_pos_);
  ImGui::Separator();
  ImGui::TextColored(kTextDim, "FPS: main loop frames over the last second (p99 and worst frame time); present is the");
  ImGui::TextColored(kTextDim, "host's draw rate. NET: the local server and client exchange packets through an");
  ImGui::TextColored(kTextDim, "in-memory socket; latency is send to receive. Tick: simulation steps per second.");
}

std::unique_ptr<DevMenu> g_menu;

}  // namespace

void SetMenuOpen(bool open, const char* tab) {
  for (int i = 0; tab && *tab && i < int(std::size(kTabs)); ++i) {
    if (_stricmp(tab, kTabs[i]) == 0) g_select_tab = i;
  }
  g_menu_open = open;
}

bool MenuOpen() { return g_menu_open; }

void InstallMenu(rex::ui::ImGuiDrawer* drawer, rex::ui::WindowedAppContext* app_context,
                 std::filesystem::path user_data_root) {
  (void)app_context;
  if (const char* v = std::getenv("BF3_DEV_OVERLAY"); v && *v == '0') g_overlay = false;  // e.g. clean screenshots
  g_menu = std::make_unique<DevMenu>(drawer, user_data_root / "dev_spots.txt");
  rex::ui::RegisterBind("bind_dev_menu", "Insert", "Debug menu (development builds)",
                        [] { g_menu_open = !g_menu_open; });
  rex::ui::RegisterBind("bind_dev_overlay", "F9", "Debug stats overlay (development builds)",
                        [] { g_overlay = !g_overlay; });
  rex::ui::RegisterBind("bind_dev_fly", "Home", "Toggle flight (development builds)", [] { Request("fly toggle"); });
}

}  // namespace bf3::dev
