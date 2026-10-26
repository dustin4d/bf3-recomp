// PC keyboard & mouse bindings modeled on Battlefield 3 PC, applied per context (see pc_keys.cpp):
// - playing (on foot, land vehicles, aircraft): keys drive BF3's own input concepts (ConceptJump, ConceptFire, ...),
//   which each entry's concept -> action map turns into soldier or vehicle actions (bf_main/pc_concepts.cpp);
// - menus (front end, spawn screen, pause menu, popups): keys press controller buttons (Esc = back everywhere).
// ReXGlue's keyboard-to-controller emulation (mnk_mode) is not used.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace bf3::pc_keys {

enum class Section { kCommon, kInfantry, kVehicle, kMenus };

struct ConceptValue {
  int id;  // InputConceptIdentifiers value (see pc_keys.cpp)
  float value;
};

struct Action {
  const char* flag;   // cvar holding the keys, e.g. "key_jump" = "Space"
  const char* label;  // as on BF3 PC's controls screen
  Section section;
  const char* defaults;  // BF3 PC defaults, comma separated
  std::vector<ConceptValue> concepts;  // playing: concepts it drives
  uint16_t pad = 0;                    // menus (or the two gameplay buttons Esc/Tab): X_INPUT_GAMEPAD bits
};

const std::vector<Action>& Actions();
const char* SectionTitle(Section s);

// Splits "A,B" into key names / joins them.
std::vector<std::string> SplitKeys(const std::string& value);

// Key name for a Windows virtual key, as written in the key_* flags ("" if it can't be bound). Covers keyboard keys,
// LMB/RMB/MMB/Mouse4/Mouse5; the wheel is "WheelUp"/"WheelDown" (see TakeRecentWheel).
std::string KeyName(int vk);
// A wheel notch since the last call: "WheelUp", "WheelDown" or "" (PC SETTINGS key capture).
std::string TakeRecentWheel();
// Called by pc_mouse.cpp for each WM_MOUSEWHEEL (notches, + = away from the user).
void OnWheel(int notches);

// Controller buttons the keyboard holds right now for the guest pad poll (user 0); 0 when the game window isn't in
// front or a host screen is open.
uint16_t PadButtons();

// The key shown on a controller glyph ("A", "B", "X", "Y", "LB", "RB", "LT", "RT", "LS", "RS", "Back", "Start",
// "D-pad", "Stick"): in menus the menu binding, while playing the PC key for what that button does on foot.
std::string GlyphKey(const std::string& button);

// Exported to bf3_BF_Main.dll (bf3_pc_concepts in pc_keys.cpp).
void Install();

}  // namespace bf3::pc_keys
