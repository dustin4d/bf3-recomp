// PC keyboard & mouse bindings (see pc_keys.h).
//
// Defaults follow BF3 PC: the official PC manual's infantry controls (W/A/S/D, C crouch, X toggle crouch, Z prone,
// Space jump/vault, LMB fire, RMB zoom, R reload, 1-5 weapons, G grenade, F knife, E interact, V fire mode, T
// flashlight, Q spot, F1-F8 vehicle seats, Tab scoreboard, Esc game menu) and PC's vehicle layout (C camera, X
// countermeasures, Shift boost, Space brake, mouse aims, wheel switches weapon). Menus: arrows, Enter/Space select,
// Esc/Backspace back, Q/E (PgUp/PgDn) change tab; Esc is back on every menu screen, and opens the game menu while
// playing, as on PC.
//
// While playing, each action drives BF3 input concepts (enum InputConceptIdentifiers, names from its reflection
// switch FUN_83F10E18): bf_main/pc_concepts.cpp merges them into the entry input state through the entry's own
// concept -> action map (soldier, each vehicle type, ...), the same path a PC keyboard binding takes. Keys held when
// the context changes (e.g. Esc that just opened the game menu) are ignored until released.

#include "pc_keys.h"

#if defined(_WIN32)
#include <windows.h>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ui/keybinds.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <bitset>
#include <chrono>
#include <cmath>
#include <map>
#include <mutex>
#include <shared_mutex>
#include <span>
#include <sstream>

#include "pad_inject.h"
#include "pc_mouse.h"
#include "pc_settings.h"
#include "ui_state.h"

REXCVAR_DEFINE_DOUBLE(vehicle_mouse_sensitivity, 1.0, "BF3",
                      "Mouse aim in vehicles and aircraft (1.0 = full turn rate at 3000 counts per second)");

namespace bf3::pc_keys {
namespace {

// InputConceptIdentifiers (BF.Main reflection; ConceptSize = 123).
enum Concept : int {
  kMoveFB = 0, kMoveLR = 1, kMoveForward = 2, kMoveBackward = 3, kMoveLeft = 4, kMoveRight = 5, kYaw = 6,
  kPitch = 7, kFire = 10, kFireCountermeasure = 12, kReload = 13, kZoom = 14, kToggleCamera = 15, kSprint = 16,
  kCrawl = 17, kToggleWeaponLight = 18, kJump = 19, kCrouch = 20, kCrouchOnHold = 21, kProne = 22, kInteract = 23,
  kBreathControl = 26, kParachute = 27, kSwitchInventoryItem = 28, kSelectInventoryItem1 = 29, kMeleeAttack = 43,
  kThrowGrenade = 44, kCycleFireMode = 45, kChangeVehicle = 46, kHandBrake = 48, kSelectPosition1 = 54,
  kMapZoom = 64, kCommMenu1 = 72, kConceptCount = 123,
};

constexpr uint16_t kUp = 0x0001, kDown = 0x0002, kLeft = 0x0004, kRight = 0x0008, kStart = 0x0010, kBack = 0x0020,
                   kRS = 0x0080, kLB = 0x0100, kRB = 0x0200, kA = 0x1000, kB = 0x2000, kX = 0x4000, kY = 0x8000;

// Pseudo virtual keys for the wheel.
constexpr int kWheelUp = 0x100, kWheelDown = 0x101, kKeyCount = 0x102;

std::vector<Action> BuildActions() {
  using S = Section;
  std::vector<Action> a = {
      // Common: on foot and in vehicles.
      {"key_move_forward", "Move Forward / Accelerate", S::kCommon, "W", {{kMoveFB, 1}, {kMoveForward, 1}}},
      {"key_move_backward", "Move Backward / Reverse", S::kCommon, "S", {{kMoveFB, -1}, {kMoveBackward, 1}}},
      {"key_move_left", "Strafe Left / Steer Left", S::kCommon, "A", {{kMoveLR, -1}, {kMoveLeft, 1}}},
      {"key_move_right", "Strafe Right / Steer Right", S::kCommon, "D", {{kMoveLR, 1}, {kMoveRight, 1}}},
      {"key_fire", "Fire", S::kCommon, "LMB", {{kFire, 1}}},
      {"key_zoom", "Zoom / Aim Down Sights", S::kCommon, "RMB", {{kZoom, 1}}},
      {"key_reload", "Reload / Pick Up (hold)", S::kCommon, "R", {{kReload, 1}}},
      {"key_interact", "Interact / Enter / Exit Vehicle", S::kCommon, "E", {{kInteract, 1}, {kChangeVehicle, 1}}},
      {"key_spot", "Spot / Commo Rose", S::kCommon, "Q", {{kCommMenu1, 1}}},
      {"key_next_weapon", "Next Weapon", S::kCommon, "WheelDown,WheelUp", {{kSwitchInventoryItem, 1}}},
      {"key_minimap_zoom", "Zoom Minimap", S::kCommon, "N", {{kMapZoom, 1}}},
      {"key_scoreboard", "Scoreboard", S::kCommon, "Tab", {}, kBack},
      {"key_game_menu", "Game Menu", S::kCommon, "Escape", {}, kStart},
  };
  for (int i = 0; i < 8; ++i) {
    static const char* kFlags[] = {"key_seat_1", "key_seat_2", "key_seat_3", "key_seat_4",
                                   "key_seat_5", "key_seat_6", "key_seat_7", "key_seat_8"};
    static const char* kLabels[] = {"Vehicle Seat 1", "Vehicle Seat 2", "Vehicle Seat 3", "Vehicle Seat 4",
                                    "Vehicle Seat 5", "Vehicle Seat 6", "Vehicle Seat 7", "Vehicle Seat 8"};
    static const char* kKeys[] = {"F1", "F2", "F3", "F4", "F5", "F6", "F7", "F8"};
    a.push_back({kFlags[i], kLabels[i], S::kCommon, kKeys[i], {{kSelectPosition1 + i, 1}}});
  }
  for (int i = 0; i < 6; ++i) {
    static const char* kFlags[] = {"key_weapon_1", "key_weapon_2", "key_weapon_3",
                                   "key_weapon_4", "key_weapon_5", "key_weapon_6"};
    static const char* kLabels[] = {"Primary Weapon", "Secondary Weapon", "Gadget 1",
                                    "Gadget 2",       "Grenade",          "Knife"};
    static const char* kKeys[] = {"1", "2", "3", "4", "5", "6"};
    a.push_back({kFlags[i], kLabels[i], S::kCommon, kKeys[i], {{kSelectInventoryItem1 + i, 1}}});
  }
  std::vector<Action> rest = {
      // On foot.
      {"key_jump", "Jump / Vault / Parachute", S::kInfantry, "Space", {{kJump, 1}, {kParachute, 1}}},
      {"key_sprint", "Sprint / Hold Breath", S::kInfantry, "Shift", {{kSprint, 1}, {kBreathControl, 1}}},
      {"key_crouch", "Crouch", S::kInfantry, "C", {{kCrouchOnHold, 1}, {kCrouch, 1}}},
      {"key_crouch_toggle", "Toggle Crouch", S::kInfantry, "X", {{kCrawl, 1}}},
      {"key_prone", "Prone", S::kInfantry, "Z", {{kProne, 1}}},
      {"key_melee", "Knife (Melee)", S::kInfantry, "F", {{kMeleeAttack, 1}}},
      {"key_grenade", "Throw Grenade", S::kInfantry, "G", {{kThrowGrenade, 1}}},
      {"key_fire_mode", "Fire Mode", S::kInfantry, "V", {{kCycleFireMode, 1}}},
      {"key_flashlight", "Flashlight / Laser", S::kInfantry, "T", {{kToggleWeaponLight, 1}}},
      // Vehicles and aircraft (mouse aims / pitches and rolls; A/D steer or yaw).
      {"key_veh_camera", "Toggle Camera", S::kVehicle, "C", {{kToggleCamera, 1}}},
      {"key_veh_countermeasure", "Countermeasures", S::kVehicle, "X", {{kFireCountermeasure, 1}}},
      {"key_veh_boost", "Boost / Afterburner", S::kVehicle, "Shift", {{kSprint, 1}}},
      {"key_veh_brake", "Brake / Handbrake", S::kVehicle, "Space", {{kHandBrake, 1}}},
      // Menus (front end, deploy screen, game menu, popups).
      {"key_menu_up", "Navigate Up", S::kMenus, "Up", {}, kUp},
      {"key_menu_down", "Navigate Down", S::kMenus, "Down", {}, kDown},
      {"key_menu_left", "Navigate Left", S::kMenus, "Left", {}, kLeft},
      {"key_menu_right", "Navigate Right", S::kMenus, "Right", {}, kRight},
      {"key_menu_select", "Select", S::kMenus, "Return,Space", {}, kA},
      {"key_menu_back", "Back", S::kMenus, "Escape,Backspace", {}, kB},
      {"key_menu_x", "Menu Action (X)", S::kMenus, "X", {}, kX},
      {"key_menu_y", "Menu Action (Y)", S::kMenus, "Y", {}, kY},
      {"key_menu_prev_tab", "Previous Tab", S::kMenus, "Q,PageUp", {}, kLB},
      {"key_menu_next_tab", "Next Tab", S::kMenus, "E,PageDown", {}, kRB},
      {"key_menu_feed", "Battle Feed / Details", S::kMenus, "Tab", {}, kRS},
  };
  a.insert(a.end(), rest.begin(), rest.end());
  return a;
}

// ---- key names --------------------------------------------------------------------------------------------------

int ParseKey(const std::string& name) {
  static const std::pair<const char*, int> kExtra[] = {
      {"Mouse4", VK_XBUTTON1}, {"Mouse5", VK_XBUTTON2}, {"WheelUp", kWheelUp}, {"WheelDown", kWheelDown},
  };
  for (auto [n, vk] : kExtra) {
    if (name == n) return vk;
  }
  return int(rex::ui::ParseVirtualKey(name));
}

// ---- bound keys (written by the cvar setters, read from guest threads) ----------------------------------------------

std::shared_mutex g_binds_mutex;
std::map<std::string, std::string>& Values() {
  static std::map<std::string, std::string> values;
  return values;
}
std::map<std::string, std::vector<int>>& Parsed() {
  static std::map<std::string, std::vector<int>> parsed;
  return parsed;
}

void SetKeys(const std::string& flag, std::string_view value) {
  std::vector<int> keys;
  for (const std::string& k : SplitKeys(std::string(value))) {
    if (int vk = ParseKey(k)) keys.push_back(vk);
  }
  std::unique_lock lock(g_binds_mutex);
  Values()[flag] = std::string(value);
  Parsed()[flag] = std::move(keys);
}

// Registered before the config is read, like REXCVAR_DEFINE flags.
const bool g_registered = [] {
  for (const Action& a : Actions()) {
    const std::string flag = a.flag;
    SetKeys(flag, a.defaults);
    rex::cvar::RegisterFlag({
        .name = flag,
        .type = rex::cvar::FlagType::String,
        .category = "BF3/Keys",
        .description = std::string("PC key(s) for ") + a.label,
        .setter = [flag](std::string_view v) -> bool {
          SetKeys(flag, v);
          return true;
        },
        .getter = [flag]() -> std::string {
          std::shared_lock lock(g_binds_mutex);
          return Values()[flag];
        },
        .lifecycle = rex::cvar::Lifecycle::kHotReload,
        .default_value = a.defaults,
    });
  }
  return true;
}();

// ---- key state ----------------------------------------------------------------------------------------------------

std::atomic<uint64_t> g_wheel_up_until{0}, g_wheel_down_until{0};
std::atomic<int> g_recent_wheel{0};

uint64_t NowMs() { return GetTickCount64(); }

bool RawDown(int vk) {
  if (vk == kWheelUp) return NowMs() < g_wheel_up_until.load(std::memory_order_relaxed);
  if (vk == kWheelDown) return NowMs() < g_wheel_down_until.load(std::memory_order_relaxed);
  return (GetAsyncKeyState(vk) & 0x8000) != 0;
}

// Keys held by the debug API (InjectKey): vk -> release time (NowMs). They count as down without focus and are never
// stale, and while any is held the game plays as if focused.
std::mutex g_inject_mutex;
std::map<int, uint64_t> g_injected;

std::bitset<kKeyCount> InjectedDown() {
  std::bitset<kKeyCount> down;
  std::lock_guard lock(g_inject_mutex);
  const uint64_t now = NowMs();
  for (auto it = g_injected.begin(); it != g_injected.end();) {
    if (now >= it->second) {
      it = g_injected.erase(it);
      continue;
    }
    if (it->first > 0 && it->first < kKeyCount) down.set(it->first);
    ++it;
  }
  return down;
}

enum class Context { kNone, kMenu, kPlay };

// Keys held when the context changed stay ignored until released.
std::mutex g_state_mutex;
Context g_context = Context::kNone;
std::bitset<kKeyCount> g_stale;

Context CurrentContext(bool injected = false) {
  if ((!pc_mouse::GameFocused() && !injected) || pc_settings::IsOpen() || GameInputBlocked()) return Context::kNone;
  return pc_mouse::MenuMode() ? Context::kMenu : Context::kPlay;
}

// Snapshot of usable keys for this poll.
struct Keys {
  Context context = Context::kNone;
  std::bitset<kKeyCount> down;
};

Keys Poll() {
  Keys k;
  const std::bitset<kKeyCount> injected = InjectedDown();
  k.context = CurrentContext(injected.any());
  std::lock_guard lock(g_state_mutex);
  const bool changed = k.context != g_context;
  if (changed) REXLOG_INFO("pc keys: context {}", k.context == Context::kPlay ? "playing" : k.context == Context::kMenu ? "menu" : "none");
  g_context = k.context;
  for (int vk = 1; vk < kKeyCount; ++vk) {
    if (vk > 0xFE && vk < kWheelUp) continue;
    if (!RawDown(vk)) {
      g_stale.reset(vk);
      continue;
    }
    if (changed) g_stale.set(vk);
    if (!g_stale.test(vk)) k.down.set(vk);
  }
  if (k.context != Context::kNone) k.down |= injected;
  return k;
}

bool AnyDown(const Keys& k, const char* flag) {
  std::shared_lock lock(g_binds_mutex);
  auto it = Parsed().find(flag);
  if (it == Parsed().end()) return false;
  for (int vk : it->second) {
    if (vk > 0 && vk < kKeyCount && k.down.test(vk)) return true;
  }
  return false;
}

std::string FirstKey(const char* flag) {
  std::shared_lock lock(g_binds_mutex);
  std::vector<std::string> keys = SplitKeys(Values()[flag]);
  return keys.empty() ? std::string() : keys.front();
}

}  // namespace

const std::vector<Action>& Actions() {
  static const std::vector<Action> actions = BuildActions();
  return actions;
}

const char* SectionTitle(Section s) {
  switch (s) {
    case Section::kCommon: return "COMMON (ON FOOT AND IN VEHICLES)";
    case Section::kInfantry: return "ON FOOT";
    case Section::kVehicle: return "VEHICLES AND AIRCRAFT";
    case Section::kMenus: return "MENUS";
  }
  return "";
}

std::vector<std::string> SplitKeys(const std::string& value) {
  std::vector<std::string> out;
  std::stringstream ss(value);
  for (std::string key; std::getline(ss, key, ',');) {
    key.erase(0, key.find_first_not_of(' '));
    key.erase(key.find_last_not_of(' ') + 1);
    if (!key.empty()) out.push_back(key);
  }
  return out;
}

std::string KeyName(int vk) {
  if (vk == VK_XBUTTON1) return "Mouse4";
  if (vk == VK_XBUTTON2) return "Mouse5";
  if (vk >= VK_LSHIFT && vk <= VK_RMENU) {
    static constexpr int kGeneric[] = {VK_SHIFT, VK_SHIFT, VK_CONTROL, VK_CONTROL, VK_MENU, VK_MENU};
    vk = kGeneric[vk - VK_LSHIFT];
  }
  return rex::ui::VirtualKeyToString(rex::ui::VirtualKey(vk));
}

std::string TakeRecentWheel() {
  const int w = g_recent_wheel.exchange(0);
  return w > 0 ? "WheelUp" : w < 0 ? "WheelDown" : "";
}

void OnWheel(int notches) {
  if (!notches) return;
  g_recent_wheel = notches;
  (notches > 0 ? g_wheel_up_until : g_wheel_down_until) = NowMs() + 90;  // a few frames: one weapon switch
}

int ParseKeyName(const std::string& name) { return ParseKey(name); }

void InjectKey(int vk, uint32_t hold_ms) {
  if (vk <= 0 || vk >= kKeyCount) return;
  std::lock_guard lock(g_inject_mutex);
  if (hold_ms) {
    g_injected[vk] = NowMs() + hold_ms;
  } else {
    g_injected.erase(vk);
  }
}

uint16_t PadButtons() {
  const Keys k = Poll();
  if (k.context == Context::kNone) return 0;
  uint16_t buttons = 0;
  for (const Action& a : Actions()) {
    if (!a.pad) continue;
    const bool menu_action = a.section == Section::kMenus;
    if (menu_action != (k.context == Context::kMenu)) continue;
    if (AnyDown(k, a.flag)) buttons |= a.pad;
  }
  return buttons;
}

std::string GlyphKey(const std::string& button) {
  // Menus: the menu binding of that button. Playing: the PC key for what the button does on foot.
  static const std::pair<const char*, const char*> kMenu[] = {
      {"A", "key_menu_select"}, {"B", "key_menu_back"},     {"X", "key_menu_x"},   {"Y", "key_menu_y"},
      {"LB", "key_menu_prev_tab"}, {"RB", "key_menu_next_tab"}, {"RS", "key_menu_feed"}, {"Stick", "key_menu_feed"},
      {"Back", "key_scoreboard"}, {"Start", "key_game_menu"}, {"LT", "key_zoom"}, {"RT", "key_fire"},
      {"LS", "key_sprint"},
  };
  static const std::pair<const char*, const char*> kPlay[] = {
      {"A", "key_jump"},   {"B", "key_crouch"},     {"X", "key_reload"},        {"Y", "key_interact"},
      {"LB", "key_spot"},  {"RB", "key_grenade"},   {"LT", "key_zoom"},         {"RT", "key_fire"},
      {"LS", "key_sprint"}, {"RS", "key_melee"},    {"Stick", "key_melee"},     {"Back", "key_scoreboard"},
      {"Start", "key_game_menu"},
  };
  const bool menu = pc_mouse::MenuMode();
  for (auto [name, flag] : menu ? std::span(kMenu) : std::span(kPlay)) {
    if (button == name) return FirstKey(flag);
  }
  return {};
}

namespace {

// Test input (BF3_KEY_SCRIPT="<step>;..."): "wait:<Screen>" (top UI screen), "wait:play" / "wait:menu" (context),
// "<delay ms>:<Key>[+<Key>...][:<hold ms>]" presses keys through SendInput (real input to the focused game window;
// hold default 120 ms). Mouse buttons and the wheel aren't scripted here (BF3_MOUSE_SCRIPT covers menu clicks).
void RunScript(std::string all) {
  for (size_t start = 0; start < all.size();) {
    size_t end = all.find(';', start);
    const std::string step = all.substr(start, end == std::string::npos ? std::string::npos : end - start);
    start = end == std::string::npos ? all.size() : end + 1;
    if (step.rfind("wait:", 0) == 0) {
      const std::string what = step.substr(5);
      for (int i = 0; i < 3000; ++i) {
        if (what == "play" ? CurrentContext() == Context::kPlay
            : what == "menu" ? CurrentContext() == Context::kMenu
                             : ui_state::TopScreen() == what) {
          break;
        }
        Sleep(100);
      }
      REXLOG_INFO("pc keys: script {}", step);
      continue;
    }
    const size_t c1 = step.find(':'), c2 = step.find(':', c1 + 1);
    if (c1 == std::string::npos) continue;
    Sleep(DWORD(std::atoi(step.substr(0, c1).c_str())));
    const DWORD hold = c2 == std::string::npos ? 120 : DWORD(std::atoi(step.substr(c2 + 1).c_str()));
    std::vector<WORD> vks;
    std::stringstream names(step.substr(c1 + 1, c2 == std::string::npos ? std::string::npos : c2 - c1 - 1));
    for (std::string n; std::getline(names, n, '+');) {
      if (int vk = ParseKey(n); vk > 0 && vk < 0xFF) vks.push_back(WORD(vk));
    }
    auto send = [&](bool up) {
      for (WORD vk : vks) {
        INPUT in{};
        in.type = INPUT_KEYBOARD;
        in.ki.wVk = vk;
        in.ki.wScan = WORD(MapVirtualKeyW(vk, MAPVK_VK_TO_VSC));
        in.ki.dwFlags = up ? KEYEVENTF_KEYUP : 0;
        if (vk == VK_UP || vk == VK_DOWN || vk == VK_LEFT || vk == VK_RIGHT) in.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
        SendInput(1, &in, sizeof(in));
      }
    };
    REXLOG_INFO("pc keys: script {} (context {})", step, int(CurrentContext()));
    send(false);
    Sleep(hold);
    send(true);
  }
}

}  // namespace

void Install() {
  (void)g_registered;
  if (const char* script = std::getenv("BF3_KEY_SCRIPT"); script && *script) {
    CreateThread(
        nullptr, 0,
        [](LPVOID p) -> DWORD {
          RunScript(static_cast<const char*>(p));
          return 0;
        },
        const_cast<char*>(script), 0, nullptr);
  }
}

}  // namespace bf3::pc_keys

// Called by bf_main/pc_concepts.cpp after each entry input mapping: out[concept] = value from the keys and (in
// vehicles) the mouse. Returns false when the keyboard isn't playing (menus, unfocused, host screen open).
// `on_foot`: the entry is the soldier (the mouse aims through bf_main/mouse_aim.cpp then, not through concepts).
extern "C" __declspec(dllexport) bool bf3_pc_concepts(float* out, int count, bool on_foot) {
  using namespace bf3::pc_keys;
  std::fill(out, out + count, 0.0f);
  static std::mutex m;
  static std::array<float, kConceptCount> cached{};
  static uint64_t cached_at = 0;
  static bool cached_on_foot = false, cached_ok = false;
  std::lock_guard lock(m);
  // Several mappers run per frame (one per entry state); they all get the same values.
  const uint64_t now = GetTickCount64();
  if (now - cached_at > 2 || cached_on_foot != on_foot) {
    cached.fill(0.0f);
    const Keys k = Poll();
    cached_ok = k.context == Context::kPlay;
    if (cached_ok) {
      for (const Action& a : Actions()) {
        if (a.concepts.empty() || a.section == Section::kMenus) continue;
        if (a.section == (on_foot ? Section::kVehicle : Section::kInfantry)) continue;
        if (!AnyDown(k, a.flag)) continue;
        for (const ConceptValue& c : a.concepts) {
          if (c.id < kConceptCount) cached[c.id] = std::clamp(cached[c.id] + c.value, -1.0f, 1.0f);
        }
      }
      if (!on_foot) {
        // Vehicles: mouse speed -> look/aim concepts, as a rate (time-normalized, so frame rate doesn't matter).
        static auto last = std::chrono::steady_clock::now();
        const auto t = std::chrono::steady_clock::now();
        const double dt = std::max(1e-3, std::chrono::duration<double>(t - last).count());
        last = t;
        double dx = 0, dy = 0;
        if (bf3::pc_mouse::TakeCounts(dx, dy)) {
          const double k_rate = REXCVAR_GET(vehicle_mouse_sensitivity) / 3000.0;
          const double invert = rex::cvar::GetFlagByName("mouse_invert_y") == "true" ? 1.0 : -1.0;
          cached[kYaw] = std::clamp(float(cached[kYaw] + dx / dt * k_rate), -1.0f, 1.0f);
          cached[kPitch] = std::clamp(float(cached[kPitch] + invert * dy / dt * k_rate), -1.0f, 1.0f);
        }
      }
    }
    cached_at = now;
    cached_on_foot = on_foot;
    // Log changes of the held concepts (not mouse rates) for tests.
    static std::string last;
    std::string held;
    for (int c = 0; c < kConceptCount; ++c) {
      if (c != kYaw && c != kPitch && cached[c] != 0.0f) held += fmt::format(" {}={:+.0f}", c, cached[c]);
    }
    if (held != last) {
      REXLOG_INFO("pc keys: concepts ({}){}", on_foot ? "on foot" : "vehicle", held.empty() ? " none" : held);
      last = held;
    }
  }
  if (!cached_ok) return false;
  std::copy(cached.begin(), cached.begin() + std::min(count, int(kConceptCount)), out);
  return true;
}

#else
namespace bf3::pc_keys {
const std::vector<Action>& Actions() {
  static const std::vector<Action> actions;
  return actions;
}
const char* SectionTitle(Section) { return ""; }
std::vector<std::string> SplitKeys(const std::string&) { return {}; }
std::string KeyName(int) { return {}; }
std::string TakeRecentWheel() { return {}; }
void OnWheel(int) {}
int ParseKeyName(const std::string&) { return 0; }
void InjectKey(int, uint32_t) {}
uint16_t PadButtons() { return 0; }
std::string GlyphKey(const std::string&) { return {}; }
void Install() {}
}  // namespace bf3::pc_keys
#endif
