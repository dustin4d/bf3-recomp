// Main menu layout: no STORE and no EXTRAS, and an EXIT that closes the game.
//
// The console main menu (ui/flow/screen/frontend/mainmenuscreen, FrontEnd.sb) lists its items in UIListDataBinding
// StaticItems arrays: [CAMPAIGN, MULTIPLAYER, CO-OP] and [MY SOLDIER, OPTIONS, STORE, DEV MENU (dev only), EXTRAS].
// Each StaticListItem (0xA4 bytes in the EBX, 0x28 once loaded) starts with ItemName, a char* into the partition's
// string table; the name is both the label's localization ID and the screen output port the flow graph routes
// (ui/flow/graph/frontend/frontendroot): ID_M_EXTRA leads to ExtraMenuScreen.
//
// Before the menu is first built (from the push hook), the loaded data is edited in guest memory:
// - STORE's element is removed from its array (Frostbite arrays keep their count at -4);
// - every "ID_M_EXTRA" string is renamed "EXIT" (an ID without a localization shows as-is), item and port alike;
// and when ExtraMenuScreen is pushed (EXIT selected), the game's quit flag (0x84091C34, read by the main loop
// sub_82FE9E18) is raised so the game shuts down normally.

// Compiled into the bf3_BF_Main DLL (see CMakeLists.txt): bf3_pch.h is generated/bf_main/bf3_pch.h.
#include "bf3_pch.h"

#include <windows.h>

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

namespace {

constexpr uint64_t kGuestBase = 0x100000000ull;
constexpr uint32_t kHeapBegin = 0x40000000, kHeapEnd = 0x80000000;
constexpr uint32_t kItemSize = 0x28;  // StaticListItem as loaded (0xA4 in the EBX; the data sources are compacted)
constexpr uint32_t kQuitFlag = 0x84091C34;

struct Region {
  uint32_t begin, end;
};

// Committed, readable guest regions in [kHeapBegin, kHeapEnd).
std::vector<Region> Regions() {
  std::vector<Region> out;
  uint64_t addr = kGuestBase + kHeapBegin;
  MEMORY_BASIC_INFORMATION mbi;
  while (addr < kGuestBase + kHeapEnd && VirtualQuery(reinterpret_cast<void*>(addr), &mbi, sizeof(mbi))) {
    const uint64_t end = std::min<uint64_t>(reinterpret_cast<uint64_t>(mbi.BaseAddress) + mbi.RegionSize,
                                            kGuestBase + kHeapEnd);
    const DWORD prot = mbi.Protect & 0xFF;
    if (mbi.State == MEM_COMMIT && prot != PAGE_NOACCESS && !(mbi.Protect & PAGE_GUARD)) {
      out.push_back({uint32_t(addr - kGuestBase), uint32_t(end - kGuestBase)});
    }
    addr = end;
  }
  return out;
}

// Guest addresses of NUL-delimited occurrences of `text` (including its terminator).
std::vector<uint32_t> FindStrings(uint8_t* base, const std::vector<Region>& regions, const char* text) {
  std::vector<uint32_t> out;
  const size_t n = std::strlen(text) + 1;
  for (const Region& r : regions) {
    const uint8_t* begin = base + r.begin;
    const uint8_t* end = base + r.end;
    for (const uint8_t* p = begin; p + n <= end;) {
      p = static_cast<const uint8_t*>(std::memchr(p, text[0], size_t(end - p)));
      if (!p || p + n > end) break;
      if (std::memcmp(p, text, n) == 0 && (p == begin || p[-1] == 0)) out.push_back(uint32_t(p - base));
      ++p;
    }
  }
  return out;
}

// Guest addresses (4-aligned) holding a big-endian pointer to one of `targets`.
std::vector<uint32_t> FindPointers(uint8_t* base, const std::vector<Region>& regions,
                                   const std::vector<uint32_t>& targets) {
  std::vector<uint32_t> out;
  for (const Region& r : regions) {
    for (uint32_t a = (r.begin + 3) & ~3u; a + 4 <= r.end; a += 4) {
      const uint32_t v = REX_LOAD_U32(a);
      if (v >= kHeapBegin && v < kHeapEnd && std::find(targets.begin(), targets.end(), v) != targets.end()) {
        out.push_back(a);
      }
    }
  }
  return out;
}

std::string NameAt(uint8_t* base, uint32_t item) {
  const uint32_t s = REX_LOAD_U32(item);
  if (s < kHeapBegin || s >= kHeapEnd) return {};
  const char* p = reinterpret_cast<const char*>(REX_RAW_ADDR(s));
  return std::string(p, strnlen(p, 64));
}

void PatchMainMenu(uint8_t* base) {
  const std::vector<Region> regions = Regions();

  // STORE: remove its element from the item array.
  const std::vector<uint32_t> store = FindStrings(base, regions, "ID_M_STORE");
  int removed = 0;
  const std::vector<uint32_t> store_refs = FindPointers(base, regions, store);
  for (uint32_t item : store_refs) {
    if (NameAt(base, item - kItemSize) != "ID_M_OPTIONS" || NameAt(base, item - 2 * kItemSize) != "ID_M_MY_SOLDIER") {
      continue;
    }
    const uint32_t first = item - 2 * kItemSize;
    const uint32_t count = REX_LOAD_U32(first - 4);
    if (count < 3 || count > 16) continue;
    const uint32_t index = 2, tail = count - index - 1;
    std::memmove(REX_RAW_ADDR(item), REX_RAW_ADDR(item + kItemSize), size_t(tail) * kItemSize);
    REX_STORE_U32(first - 4, count - 1);
    ++removed;
  }

  // EXTRAS -> EXIT: rename the ID everywhere it was loaded (menu item and flow port).
  int renamed = 0;
  for (uint32_t s : FindStrings(base, regions, "ID_M_EXTRA")) {
    std::memcpy(REX_RAW_ADDR(s), "EXIT\0\0\0\0\0\0\0", 11);  // no Xbox localization for an exit ID: unknown IDs show as-is
    ++renamed;
  }
  REXLOG_INFO("main menu: removed STORE from {} list(s), renamed EXTRAS to EXIT in {} place(s)", removed, renamed);
}

}  // namespace

// From the screen push hook (ui_screens.cpp), before the screen is built.
void bf3_main_menu_before_push(uint8_t* base, const std::string& name) {
  static bool patched = false;
  if (name == "MainMenuScreen" && !patched) {
    patched = true;
    PatchMainMenu(base);
  }
  if (name == "ExtraMenuScreen") {  // EXIT
    REXLOG_INFO("main menu: EXIT selected, closing the game");
    REX_STORE_U32(kQuitFlag, 1);
  }
}
