// PRACTICE page: DICE's developer level select (ui/flow/screen/devscreen, opened by the main menu's PRACTICE item,
// see main_menu.cpp) reshaped into a native page: Operation Metro, Team Deathmatch, then the team.
//
// - Column titles: the screen's movie (ui/assets/devscreen, CFX in Levels/FrontEnd/FrontEnd.sb) places four static
//   DefineEditText fields; their initial texts are rewritten as the movie comes out of zlib (inflate hook in
//   ui_buttons.cpp): SINGLEPLAYER LEVELS -> TEAM DEATHMATCH, MULTIPLAYER LEVELS -> TEAM, the other two emptied.
// - Map column (List_01, focused when the screen opens): the level data source sub_837E9F80(r3, key r4, out r5)
//   answers the list queries (key 0x84EE6302 SP -> category 3, 0xB142F75C MP -> 2, 0x3CE14372 co-op -> 1) through
//   sub_837E98D0, which lists the levels sub_837E4E38 collects; each level is tested by sub_837DEC28(r3, level r4,
//   category r5) (level +8 name, +0xC index, +0x14 label ID, +0x20 co-op, +0x22 MP). While the page is up, category 3
//   admits only MP_Subway and 1/2 admit nothing.
// - Team column (List_05, the MP list): empty until the map is chosen. Choosing it (the list's Release -> LoadLevel,
//   sub_836EE268 in level_boot.cpp, params[0] = the row's Index) starts the team step: the MP key is answered with two
//   rows (sub_8328F408(out, row, field name, char*) sets Label / Index), the UI manager (sub_83280730) is told the key
//   changed (vtable +0x60(manager, key, 1), as sub_837DEAB0 does), and the host taps Right once (List_01
//   NavigateRight -> List_05 EnterTop; pad_inject.cpp) so US MARINES is highlighted. Choosing a team sets it
//   (practice.cpp) and launches Metro TDM. Left/right are otherwise masked on the page (pad_inject.cpp): they would
//   move the focus into the emptied columns. B leaves the page; reopening starts over at the map.
// - Page header (DEVELOPER MENU -> PRACTICE) and the emptied MENU SHORTCUTS column: main_menu.cpp.

// Compiled into the bf3_BF_Main DLL (see CMakeLists.txt): bf3_pch.h is generated/bf_main/bf3_pch.h.
#include "bf3_pch.h"

#include <atomic>
#include <cstring>
#include <string>
#include <string_view>

REX_EXTERN(__imp__sub_837DEC28);
REX_EXTERN(__imp__sub_837E9F80);
REX_EXTERN(__imp__sub_8328E558);
REX_EXTERN(sub_8328F408);  // list row field (out r3, row r4, field name r5, char* value r6)
REX_EXTERN(sub_83280730);  // UI manager
REX_EXTERN(sub_83013BA0);  // allocator (allocator r3, size r4, align r5)

extern "C" int bf3_ui_top_screen(char* out, int capacity);  // ui_screens.cpp
extern "C" int bf3_ui_screen_count(const char* name);        // ui_screens.cpp
extern "C" void bf3_practice_set_team(int team);              // practice.cpp

namespace {

constexpr char kPracticeLevelName[] = "MP_Subway";
constexpr uint32_t kMpListKey = 0xB142F75C;  // List_05, the team column
constexpr uint32_t kLabelField = 0x82E62068;  // "Label"
constexpr uint32_t kIndexField = 0x82D95064;  // "Index"

struct Team {
  int id;           // 1 US, 2 RU (practice.cpp)
  const char* label;
  const char* index;  // the row's Index: what LoadLevel receives when it is chosen
};
constexpr Team kTeams[] = {{1, "US MARINES", "PRACTICE_TEAM_US"}, {2, "RUSSIAN ARMY", "PRACTICE_TEAM_RU"}};

// The team step belongs to one opening of the page: the DevScreen push count when it started (0: not started).
std::atomic<int> g_team_step_push{0};
std::atomic<bool> g_focus_team_column{false};  // read once by bf3.exe (pad_inject.cpp)
int g_highlighted_team = 1;                    // the team row last highlighted (UI thread)
uint32_t g_team_strings = 0;                   // guest copies of the team rows' strings (UI thread)

bool TeamStep() {
  const int push = g_team_step_push.load();
  return push && push == bf3_ui_screen_count("DevScreen");
}

uint32_t GuestStrings(PPCContext& ctx, uint8_t* base) {
  if (g_team_strings) return g_team_strings;
  PPCContext saved = ctx;
  ctx.r3.u64 = 0x84042270;  // the allocator the UI uses for small objects
  ctx.r4.u64 = 0x100;
  ctx.r5.u64 = 16;
  sub_83013BA0(ctx, base);
  const uint32_t block = ctx.r3.u32;
  ctx = saved;
  if (!block) return 0;
  uint32_t at = block;
  for (const Team& team : kTeams) {
    for (const char* text : {team.label, team.index}) {
      std::memcpy(REX_RAW_ADDR(at), text, std::strlen(text) + 1);
      at += 0x40;
    }
  }
  return g_team_strings = block;
}

void SetRowField(PPCContext& ctx, uint8_t* base, uint32_t out, uint32_t row, uint32_t field, uint32_t value) {
  PPCContext saved = ctx;
  ctx.r3.u64 = out;
  ctx.r4.u64 = row;
  ctx.r5.u64 = field;
  ctx.r6.u64 = value;
  sub_8328F408(ctx, base);
  ctx = saved;
}

struct TextPatch {
  const char* from;
  const char* to;
};
constexpr TextPatch kMoviePatches[] = {
    {"SINGLEPLAYER LEVELS", "TEAM DEATHMATCH"},
    {"MULTIPLAYER LEVELS", "TEAM"},
    {"CO-OP LEVELS", ""},
    {"MENU SHORTCUTS", ""},
};

uint8_t* Find(uint8_t* begin, uint8_t* end, const char* text, size_t n) {
  for (uint8_t* p = begin; p + n <= end; ++p) {
    p = static_cast<uint8_t*>(std::memchr(p, text[0], size_t(end - p)));
    if (!p || p + n > end) break;
    if (std::memcmp(p, text, n) == 0) return p;
  }
  return nullptr;
}

}  // namespace

// The PRACTICE page (DevScreen) is the screen pushed last.
bool PracticePageUp() {
  char name[32];
  const int n = bf3_ui_top_screen(name, sizeof(name));
  return std::string_view(name, size_t(n)) == "DevScreen";
}

// From the inflate hook (ui_buttons.cpp): [begin, end) is what one inflate call on z_stream `stream` just wrote, `done`
// when that call ended the stream (Z_STREAM_END). The movie comes out in several pieces, so the stream whose
// output contained DICE's first title is patched until it ends.
void PracticeMenuPatchMovie(uint32_t stream, uint8_t* begin, uint8_t* end, bool done) {
  static uint32_t devscreen_stream = 0;  // UI thread only
  if (Find(begin, end, kMoviePatches[0].from, std::strlen(kMoviePatches[0].from) + 1)) devscreen_stream = stream;
  if (stream != devscreen_stream) return;
  for (const TextPatch& patch : kMoviePatches) {
    const size_t n = std::strlen(patch.from) + 1;
    if (uint8_t* p = Find(begin, end, patch.from, n)) {
      std::memset(p, 0, n);
      std::memcpy(p, patch.to, std::strlen(patch.to));
      REXLOG_INFO("practice page: movie '{}' -> '{}'", patch.from, patch.to);
    }
  }
  if (done) devscreen_stream = 0;
}

// Read by bf3.exe (pad_inject.cpp): true once after the team step started, to move the focus to the team column.
extern "C" __declspec(dllexport) int bf3_practice_take_focus_team() { return g_focus_team_column.exchange(false); }

// From LoadLevel (level_boot.cpp) while the page is up, with the chosen row's value. Returns the team to launch with,
// or 0 when the map was chosen (the team step starts). The team column (List_05) keeps its binding's
// SendIndexWithEvent (List_01's widget turns it off), and its Release then hands LoadLevel a dangling string ('')
// instead of the row's Index; the row's own RowHighlighted -> SelectLevel event does carry the Index (see
// sub_8328E558 below), so the team is the one last highlighted.
int PracticeMenuChoose(PPCContext& ctx, uint8_t* base, const std::string& index) {
  int chosen = 0;
  for (const Team& team : kTeams) {
    if (index == team.index) chosen = team.id;
  }
  if (!chosen && TeamStep() && index.find(kPracticeLevelName) == std::string::npos) chosen = g_highlighted_team;
  if (chosen) {
    REXLOG_INFO("practice page: team {} chosen", kTeams[chosen - 1].label);
    bf3_practice_set_team(chosen);
    return chosen;
  }
  REXLOG_INFO("practice page: map chosen ('{}'), team step", index);
  g_team_step_push = bf3_ui_screen_count("DevScreen");
  g_highlighted_team = 1;
  PPCContext saved = ctx;
  sub_83280730(ctx, base);
  const uint32_t manager = ctx.r3.u32;
  if (manager) {
    ctx = saved;
    ctx.r3.u64 = manager;
    ctx.r4.u64 = kMpListKey;
    ctx.r5.u64 = 1;
    const uint32_t fn = REX_LOAD_U32(REX_LOAD_U32(manager) + 0x60);
    REX_CALL_INDIRECT_FUNC(fn);
  }
  ctx = saved;
  g_focus_team_column = true;
  return 0;
}

// Flow output node sub_8328E558(r3, node r4, event r5): event +0x70 value type (6: string), +0x68 char*. A team row's
// highlight (SelectLevel) carries its Index.
extern "C" REX_FUNC(sub_8328E558) {
  const uint32_t event = ctx.r5.u32;
  if (event && REX_LOAD_U32(event + 0x70) == 6 && PracticePageUp()) {
    if (const uint32_t value = REX_LOAD_U32(event + 0x68)) {
      const char* text = reinterpret_cast<const char*>(REX_RAW_ADDR(value));
      const std::string_view v(text, strnlen(text, 32));
      for (const Team& team : kTeams) {
        if (v == team.index) g_highlighted_team = team.id;
      }
    }
  }
  __imp__sub_8328E558(ctx, base);
}

// Level data source: the team column's rows during the team step.
extern "C" REX_FUNC(sub_837E9F80) {
  if (ctx.r4.u32 == kMpListKey && PracticePageUp() && TeamStep()) {
    const uint32_t out = ctx.r5.u32;
    if (const uint32_t strings = GuestStrings(ctx, base)) {
      for (uint32_t row = 0; row < 2; ++row) {
        SetRowField(ctx, base, out, row, kLabelField, strings + row * 0x80);
        SetRowField(ctx, base, out, row, kIndexField, strings + row * 0x80 + 0x40);
      }
    }
    ctx.r3.u64 = 1;
    return;
  }
  __imp__sub_837E9F80(ctx, base);
}

// Level category filter (see the top of the file).
extern "C" REX_FUNC(sub_837DEC28) {
  const uint32_t level = ctx.r4.u32, category = ctx.r5.u32;
  if (level && category >= 1 && category <= 3 && PracticePageUp()) {
    bool admit = false;
    if (category == 3) {
      if (const uint32_t name = REX_LOAD_U32(level + 8)) {
        const char* text = reinterpret_cast<const char*>(REX_RAW_ADDR(name));
        admit = std::string_view(text, strnlen(text, 128)).find(kPracticeLevelName) != std::string_view::npos;
      }
    }
    ctx.r3.u64 = admit ? 1 : 0;
    return;
  }
  __imp__sub_837DEC28(ctx, base);
}
