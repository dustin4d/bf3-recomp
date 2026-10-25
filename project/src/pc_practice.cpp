// PRACTICE launcher (see pc_practice.h).
//
// Opens each time the developer level select (DevScreen) is pushed and covers it. LAUNCH hands the level and game
// mode to bf3_BF_Main.dll (bf3_practice_launch: the in-place level switch of level_boot.cpp) and the team to
// practice.cpp (bf3_practice_set_team); DEVELOPER MENU leaves DICE's screen usable; BACK closes both (Back is pulsed
// to the game). While open, the game sees an idle controller and no mouse (pad_inject.h, pc_mouse.cpp).
// Tests: BF3_PRACTICE_AUTO=<map id>:<mode layer>:<team 1|2> (e.g. MP_001:ConquestLarge0:2) launches that 2 s after
// the launcher opens (screenshots show it first).

#include "pc_practice.h"

#include "pad_inject.h"
#include "pc_settings.h"
#include "ui_state.h"

#include <imgui.h>
#include <rex/logging.h>
#include <rex/ui/imgui_dialog.h>

#include <algorithm>
#include <cstdlib>
#include <iterator>
#include <memory>
#include <string>

#if defined(_WIN32)
#include <windows.h>

namespace bf3::pc_practice {
namespace {

struct Map {
  const char* id;    // Levels/<id>/<id>
  const char* name;
};
constexpr Map kMaps[] = {
    {"MP_Subway", "OPERATION METRO"}, {"MP_001", "GRAND BAZAAR"},      {"MP_003", "TEHRAN HIGHWAY"},
    {"MP_007", "CASPIAN BORDER"},     {"MP_011", "SEINE CROSSING"},    {"MP_012", "OPERATION FIRESTORM"},
    {"MP_013", "DAMAVAND PEAK"},      {"MP_017", "NOSHAHR CANALS"},    {"MP_018", "KHARG ISLAND"},
};

struct Mode {
  const char* layer;  // GameMode layer inclusion
  const char* name;
  const char* about;
};
constexpr Mode kModes[] = {
    {"TeamDeathMatch0", "TEAM DEATHMATCH", "Infantry on a compact part of the map."},
    {"SquadDeathMatch0", "SQUAD DEATHMATCH", "Squads, compact map, one vehicle."},
    {"ConquestLarge0", "CONQUEST", "The full map with flags and vehicles."},
    {"RushLarge0", "RUSH", "Attack or defend M-COM stations."},
    {"SquadRush0", "SQUAD RUSH", "Rush for two squads."},
};

constexpr uint16_t kButtonB = 0x2000;

const ImVec4 kOrange(0.945f, 0.475f, 0.125f, 1.0f);
const ImVec4 kText(0.95f, 0.96f, 0.98f, 1.0f);
const ImVec4 kTextDim(0.55f, 0.62f, 0.70f, 1.0f);
const ImVec4 kInk(0.02f, 0.03f, 0.05f, 1.0f);
const ImVec4 kPanel(0.02f, 0.035f, 0.055f, 0.97f);
const ImVec4 kSlot(0.09f, 0.13f, 0.18f, 1.0f);
const ImVec4 kSlotHover(0.16f, 0.23f, 0.31f, 1.0f);
const ImVec4 kSlotActive(0.94f, 0.95f, 0.96f, 1.0f);

using LaunchFn = void (*)(const char*);
using TeamFn = void (*)(int);

class Launcher final : public rex::ui::ImGuiDialog {
 public:
  explicit Launcher(rex::ui::ImGuiDrawer* drawer) : ImGuiDialog(drawer) {}

 protected:
  void OnDraw(ImGuiIO& io) override {
    if (!launch_) {
      if (HMODULE module = GetModuleHandleW(L"bf3_BF_Main.dll")) {
        launch_ = reinterpret_cast<LaunchFn>(GetProcAddress(module, "bf3_practice_launch"));
        team_fn_ = reinterpret_cast<TeamFn>(GetProcAddress(module, "bf3_practice_set_team"));
      }
      if (!launch_) return;
    }
    // A new DevScreen push opens the launcher.
    const int pushes = ui_state::ScreenCount("DevScreen");
    if (pushes != dev_pushes_) {
      dev_pushes_ = pushes;
      if (ui_state::TopScreen() == "DevScreen") Open();
    }
    if (open_ && ui_state::TopScreen() != "DevScreen") Close();  // left some other way
    if (release_at_ && GetTickCount64() >= release_at_) {
      release_at_ = 0;
      if (!open_) SetGameInputBlocked(false);
    }
    if (open_) {
      Draw(io);
      AutoLaunch();
    }
  }

 private:
  void AutoLaunch() {
    static const std::string spec = [] {
      const char* v = std::getenv("BF3_PRACTICE_AUTO");
      return std::string(v ? v : "");
    }();
    if (spec.empty() || !open_ || GetTickCount64() - opened_at_ < 2000) return;
    const size_t c1 = spec.find(':'), c2 = spec.find(':', c1 + 1);
    const std::string id = spec.substr(0, c1);
    const std::string layer = c1 == std::string::npos ? "" : spec.substr(c1 + 1, c2 == std::string::npos ? c2 : c2 - c1 - 1);
    for (int i = 0; i < int(std::size(kMaps)); ++i) {
      if (id == kMaps[i].id) map_ = i;
    }
    for (int i = 0; i < int(std::size(kModes)); ++i) {
      if (layer == kModes[i].layer) mode_ = i;
    }
    if (c2 != std::string::npos) team_ = spec[c2 + 1] == '2' ? 2 : 1;
    Launch();
  }

  void Open() {
    open_ = true;
    opened_at_ = GetTickCount64();
    release_at_ = 0;
    SetGameInputBlocked(true);
    REXLOG_INFO("practice launcher: open");
  }

  // The click or key that closed the launcher must not reach the game.
  void Close() {
    open_ = false;
    release_at_ = GetTickCount64() + 300;
  }

  void Launch() {
    const Map& map = kMaps[map_];
    const Mode& mode = kModes[mode_];
    const std::string spec =
        std::string("Levels/") + map.id + "/" + map.id + ":GameMode=" + mode.layer;
    REXLOG_INFO("practice launcher: {} ({}), {}, team {}", map.name, spec, mode.name, team_ == 1 ? "US" : "RU");
    if (team_fn_) team_fn_(team_);
    launch_(spec.c_str());
    Close();
  }

  // A list of choices drawn as full-width buttons; the selected one is white like the game's lists.
  template <typename T, size_t N>
  void Choices(const char* id, const T (&items)[N], int& selected, float scale) {
    ImGui::PushID(id);
    for (int i = 0; i < int(N); ++i) {
      const bool active = i == selected;
      ImGui::PushStyleColor(ImGuiCol_Button, active ? kSlotActive : kSlot);
      ImGui::PushStyleColor(ImGuiCol_ButtonHovered, active ? kSlotActive : kSlotHover);
      ImGui::PushStyleColor(ImGuiCol_Text, active ? kInk : kText);
      ImGui::PushStyleVar(ImGuiStyleVar_ButtonTextAlign, ImVec2(0.0f, 0.5f));
      ImGui::PushID(i);
      if (ImGui::Button(items[i].name, ImVec2(-FLT_MIN, 34 * scale))) selected = i;
      if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
        selected = i;
        double_clicked_ = true;
      }
      ImGui::PopID();
      ImGui::PopStyleVar();
      ImGui::PopStyleColor(3);
    }
    ImGui::PopID();
  }

  void Draw(ImGuiIO& io) {
    ImGui::GetBackgroundDrawList()->AddRectFilled(ImVec2(0, 0), io.DisplaySize, IM_COL32(0, 0, 0, 170));
    const float scale = std::max(1.0f, io.DisplaySize.y / 720.0f);
    ImVec2 size(std::min(io.DisplaySize.x * 0.92f, 1000.0f * scale), std::min(io.DisplaySize.y * 0.9f, 600.0f * scale));
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_Always,
                            ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(size, ImGuiCond_Always);
    ImGui::SetNextWindowFocus();
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(28 * scale, 20 * scale));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(12 * scale, 5 * scale));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8 * scale, 4 * scale));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, kPanel);
    ImGui::PushStyleColor(ImGuiCol_Text, kText);
    ImGui::PushStyleColor(ImGuiCol_Button, kSlot);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, kSlotHover);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, kSlotHover);
    ImGui::PushStyleColor(ImGuiCol_Separator, ImVec4(0.25f, 0.32f, 0.40f, 1.0f));
    double_clicked_ = false;
    bool launch = false, back = false, dev = false;
    if (ImGui::Begin("##practice", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings)) {
      ImVec2 wp = ImGui::GetWindowPos();
      ImGui::GetWindowDrawList()->AddRectFilled(wp, ImVec2(wp.x + ImGui::GetWindowWidth(), wp.y + 6 * scale),
                                                ImGui::GetColorU32(kOrange));
      ImGui::PushFont(pc_settings::BodyFont(), 19.0f * scale);
      ImGui::TextColored(kTextDim, "MAIN MENU /");
      ImGui::PopFont();
      ImGui::PushFont(pc_settings::TitleFont(), 44.0f * scale);
      ImGui::TextUnformatted("PRACTICE");
      ImGui::PopFont();
      ImGui::PushFont(pc_settings::BodyFont(), 21.0f * scale);
      ImGui::TextColored(kTextDim, "Play a multiplayer map alone on a local server. Choose your kit and weapons on the");
      ImGui::TextColored(kTextDim, "deploy screen, then DEPLOY.");
      ImGui::Separator();

      const float footer = ImGui::GetFrameHeightWithSpacing() * 1.8f;
      if (ImGui::BeginTable("##cols", 3, ImGuiTableFlags_SizingStretchProp, ImVec2(0, -footer))) {
        ImGui::TableSetupColumn("map", ImGuiTableColumnFlags_WidthStretch, 1.15f);
        ImGui::TableSetupColumn("mode", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn("team", ImGuiTableColumnFlags_WidthStretch, 0.8f);
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::TextColored(kOrange, "MAP");
        Choices("maps", kMaps, map_, scale);
        ImGui::TableSetColumnIndex(1);
        ImGui::TextColored(kOrange, "GAME MODE");
        Choices("modes", kModes, mode_, scale);
        ImGui::Dummy(ImVec2(0, 6 * scale));
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextColored(kTextDim, "%s", kModes[mode_].about);
        ImGui::PopTextWrapPos();
        ImGui::TableSetColumnIndex(2);
        ImGui::TextColored(kOrange, "TEAM");
        struct Team {
          const char* name;
        };
        static constexpr Team kTeams[] = {{"US MARINES"}, {"RUSSIAN ARMY"}};
        int team_index = team_ - 1;
        Choices("teams", kTeams, team_index, scale);
        team_ = team_index + 1;
        ImGui::Dummy(ImVec2(0, 6 * scale));
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextColored(kTextDim, "The round starts as soon as you are in; no other players are needed.");
        ImGui::PopTextWrapPos();
        ImGui::EndTable();
      }
      ImGui::Separator();
      const float bw = ImGui::CalcTextSize("DEVELOPER MENU").x + ImGui::GetStyle().FramePadding.x * 4;
      back = ImGui::Button("BACK", ImVec2(bw * 0.7f, 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape, false);
      ImGui::SameLine();
      dev = ImGui::Button("DEVELOPER MENU", ImVec2(bw, 0));
      ImGui::SameLine(ImGui::GetWindowWidth() - bw - ImGui::GetStyle().WindowPadding.x);
      ImGui::PushStyleColor(ImGuiCol_Button, kOrange);
      ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1.0f, 0.58f, 0.25f, 1.0f));
      ImGui::PushStyleColor(ImGuiCol_Text, kInk);
      launch = ImGui::Button("LAUNCH", ImVec2(bw, 0)) || ImGui::IsKeyPressed(ImGuiKey_Enter, false) ||
               ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false) || double_clicked_;
      ImGui::PopStyleColor(3);
      ImGui::PopFont();
    }
    ImGui::End();
    ImGui::PopStyleColor(6);
    ImGui::PopStyleVar(6);
    if (launch) {
      Launch();
    } else if (dev) {
      REXLOG_INFO("practice launcher: developer menu");
      Close();
    } else if (back) {
      REXLOG_INFO("practice launcher: back");
      Close();
      PulseButtons(kButtonB, 150);
    }
  }

  LaunchFn launch_ = nullptr;
  TeamFn team_fn_ = nullptr;
  int dev_pushes_ = 0;
  bool open_ = false;
  bool double_clicked_ = false;
  uint64_t release_at_ = 0;
  uint64_t opened_at_ = 0;
  int map_ = 0;
  int mode_ = 0;
  int team_ = 1;
};

std::unique_ptr<Launcher> g_launcher;

}  // namespace

void CreateDialogs(rex::ui::ImGuiDrawer* drawer) { g_launcher = std::make_unique<Launcher>(drawer); }

}  // namespace bf3::pc_practice

#else
namespace bf3::pc_practice {
void CreateDialogs(rex::ui::ImGuiDrawer*) {}
}  // namespace bf3::pc_practice
#endif
