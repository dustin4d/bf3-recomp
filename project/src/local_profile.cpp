// Local soldier profile (see local_profile.h).
//
// The profile is a soldier name (1-15 characters, like a gamertag) in user_data_root/soldier_profile.txt. Until one
// exists, entering MULTIPLAYER, CO-OP or MY SOLDIER (the local online stack's first use, signalled by
// bf3_local_online_profile_needed in bf3_BF_Main.dll) opens the SOLDIER PROFILE prompt over the game, with game input
// held. The game reads the player's name through XamUserGetName and XamUserGetSigninInfo (X_USER_SIGNIN_INFO +0x18
// name[16]); bf3_BF_Main.dll's imports of both are redirected to answer with the soldier name.

#include "local_profile.h"

#if defined(_WIN32)
#include <windows.h>

#include <imgui.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ui/imgui_dialog.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstring>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>

#include "iat_patch.h"
#include "pad_inject.h"
#include "pc_settings.h"

namespace bf3::local_profile {
namespace {

using GuestFunc = void (*)(PPCContext&, uint8_t*);

std::mutex g_mutex;
std::string g_name;  // empty: no profile yet
std::filesystem::path g_file;
GuestFunc g_get_name = nullptr, g_get_signin_info = nullptr;

using NeededFn = int (*)();
using SetFn = void (*)(const char*);
NeededFn g_needed = nullptr;
SetFn g_set = nullptr;

std::string Name() {
  std::lock_guard lock(g_mutex);
  return g_name;
}

bool ValidName(const std::string& name) {
  if (name.empty() || name.size() > 15) return false;
  return std::all_of(name.begin(), name.end(), [](char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == ' ' || c == '_' || c == '-';
  });
}

void Save(const std::string& name) {
  {
    std::lock_guard lock(g_mutex);
    g_name = name;
  }
  std::error_code ec;
  std::filesystem::create_directories(g_file.parent_path(), ec);
  std::ofstream(g_file, std::ios::trunc) << name << '\n';
  REXLOG_INFO("local profile: soldier '{}' saved to {}", name, g_file.string());
  if (g_set) g_set(name.c_str());
}

// u32 XamUserGetName(u32 user_index, char* buffer, u32 size)
void GetName(PPCContext& ctx, uint8_t* base) {
  const uint32_t user = ctx.r3.u32, buffer = ctx.r4.u32, size = ctx.r5.u32;
  g_get_name(ctx, base);
  const std::string name = Name();
  if (user == 0 && buffer && size && !name.empty()) {
    const size_t n = std::min<size_t>(name.size(), size - 1);
    std::memcpy(base + buffer, name.data(), n);
    base[buffer + n] = 0;
    ctx.r3.u64 = 0;
  }
}

// u32 XamUserGetSigninInfo(u32 user_index, u32 flags, X_USER_SIGNIN_INFO* info): +0x18 name[16]
void GetSigninInfo(PPCContext& ctx, uint8_t* base) {
  const uint32_t user = ctx.r3.u32, info = ctx.r5.u32;
  g_get_signin_info(ctx, base);
  const std::string name = Name();
  if (user == 0 && info && ctx.r3.u32 == 0 && !name.empty()) {
    std::memset(base + info + 0x18, 0, 16);
    std::memcpy(base + info + 0x18, name.data(), std::min<size_t>(name.size(), 15));
  }
}

// ---- prompt --------------------------------------------------------------------------------------------------

const ImVec4 kOrange(0.945f, 0.475f, 0.125f, 1.0f);
const ImVec4 kText(0.95f, 0.96f, 0.98f, 1.0f);
const ImVec4 kTextDim(0.55f, 0.62f, 0.70f, 1.0f);
const ImVec4 kPanel(0.02f, 0.035f, 0.055f, 0.97f);
const ImVec4 kSlot(0.09f, 0.13f, 0.18f, 1.0f);
const ImVec4 kSlotHover(0.16f, 0.23f, 0.31f, 1.0f);

class ProfilePrompt final : public rex::ui::ImGuiDialog {
 public:
  explicit ProfilePrompt(rex::ui::ImGuiDrawer* drawer) : ImGuiDialog(drawer) {}

 protected:
  void OnDraw(ImGuiIO& io) override {
    if (!g_needed) {
      if (HMODULE module = GetModuleHandleW(L"bf3_BF_Main.dll")) {
        g_needed = reinterpret_cast<NeededFn>(GetProcAddress(module, "bf3_local_online_profile_needed"));
        g_set = reinterpret_cast<SetFn>(GetProcAddress(module, "bf3_local_online_set_profile"));
        if (g_set && !Name().empty()) g_set(Name().c_str());
      }
      return;
    }
    const bool show = g_needed() && Name().empty();
    if (show != open_) {
      open_ = show;
      if (show) {
        SetGameInputBlocked(true);
        focus_ = true;
      } else {
        release_at_ = GetTickCount64() + 400;  // the Enter that confirmed must not reach the game
      }
    }
    if (release_at_ && GetTickCount64() >= release_at_) {
      release_at_ = 0;
      SetGameInputBlocked(false);
    }
    if (!show) return;
    Draw(io);
  }

 private:
  void Draw(ImGuiIO& io) {
    ImGui::GetBackgroundDrawList()->AddRectFilled(ImVec2(0, 0), io.DisplaySize, IM_COL32(0, 0, 0, 170));
    const float scale = std::max(1.0f, io.DisplaySize.y / 720.0f);
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_Always,
                            ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(640 * scale, 310 * scale), ImGuiCond_Always);
    ImGui::SetNextWindowFocus();
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(28 * scale, 20 * scale));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(10 * scale, 6 * scale));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, kPanel);
    ImGui::PushStyleColor(ImGuiCol_Text, kText);
    ImGui::PushStyleColor(ImGuiCol_FrameBg, kSlot);
    ImGui::PushStyleColor(ImGuiCol_Button, kSlot);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, kSlotHover);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, kSlotHover);
    if (ImGui::Begin("##soldierprofile", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings)) {
      ImVec2 wp = ImGui::GetWindowPos();
      ImGui::GetWindowDrawList()->AddRectFilled(wp, ImVec2(wp.x + ImGui::GetWindowWidth(), wp.y + 6 * scale),
                                                ImGui::GetColorU32(kOrange));
      ImGui::PushFont(pc_settings::TitleFont(), 40.0f * scale);
      ImGui::TextUnformatted("SOLDIER PROFILE");
      ImGui::PopFont();
      ImGui::PushFont(pc_settings::BodyFont(), 21.0f * scale);
      ImGui::TextColored(kTextDim, "Create your soldier to play online. The name is shown");
      ImGui::TextColored(kTextDim, "on your stats, dog tags and to other players.");
      ImGui::Dummy(ImVec2(0, 10 * scale));
      ImGui::TextUnformatted("SOLDIER NAME");
      ImGui::SetNextItemWidth(-FLT_MIN);
      if (focus_) {
        ImGui::SetKeyboardFocusHere();
        focus_ = false;
      }
      const bool enter = ImGui::InputText("##name", buffer_, sizeof(buffer_), ImGuiInputTextFlags_EnterReturnsTrue);
      const std::string name = buffer_;
      const bool valid = ValidName(name);
      ImGui::TextColored(kOrange, "%s", name.empty() || valid ? "" : "Use up to 15 letters, digits, spaces, - or _.");
      ImGui::Dummy(ImVec2(0, 6 * scale));
      const float bw = 200 * scale;
      ImGui::SetCursorPosX(ImGui::GetWindowWidth() - bw - ImGui::GetStyle().WindowPadding.x);
      ImGui::BeginDisabled(!valid);
      const bool create = ImGui::Button("CREATE SOLDIER", ImVec2(bw, 0));
      ImGui::EndDisabled();
      if ((create || enter) && valid) Save(name);
      ImGui::PopFont();
    }
    ImGui::End();
    ImGui::PopStyleColor(6);
    ImGui::PopStyleVar(5);
  }

  bool open_ = false;
  bool focus_ = false;
  uint64_t release_at_ = 0;
  char buffer_[32] = {};
};

std::unique_ptr<ProfilePrompt> g_prompt;

}  // namespace

void Install(std::filesystem::path user_data_root) {
  g_file = user_data_root / "soldier_profile.txt";
  {
    std::ifstream in(g_file);
    std::string line;
    if (std::getline(in, line) && ValidName(line)) {
      std::lock_guard lock(g_mutex);
      g_name = line;
      REXLOG_INFO("local profile: soldier '{}'", line);
    }
  }
  CreateThread(
      nullptr, 0,
      [](LPVOID) -> DWORD {
        HMODULE module = nullptr;
        for (int i = 0; i < 6000 && !module; ++i) {
          module = GetModuleHandleW(L"bf3_BF_Main.dll");
          if (!module) Sleep(10);
        }
        if (!module) return 0;
        g_get_name = reinterpret_cast<GuestFunc>(
            PatchImport(module, "rexruntime.dll", "__imp__XamUserGetName", reinterpret_cast<void*>(&GetName)));
        g_get_signin_info = reinterpret_cast<GuestFunc>(PatchImport(
            module, "rexruntime.dll", "__imp__XamUserGetSigninInfo", reinterpret_cast<void*>(&GetSigninInfo)));
        REXLOG_INFO("local profile: player name queries redirected ({} {})", g_get_name != nullptr,
                    g_get_signin_info != nullptr);
        return 0;
      },
      nullptr, 0, nullptr);
}

void CreateDialogs(rex::ui::ImGuiDrawer* drawer) { g_prompt = std::make_unique<ProfilePrompt>(drawer); }

}  // namespace bf3::local_profile

#else
namespace bf3::local_profile {
void Install(std::filesystem::path) {}
void CreateDialogs(rex::ui::ImGuiDrawer*) {}
}  // namespace bf3::local_profile
#endif
