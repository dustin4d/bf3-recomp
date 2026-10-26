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
//
// PRACTICE opens DICE's DevScreen, shaped into the PRACTICE page (practice_menu.cpp). On each DevScreen push its page
// header's StaticSubHeader "DEVELOPER MENU" is renamed "PRACTICE", and its MENU SHORTCUTS list (List_04: StaticItems
// MAIN MENU PC, MAIN MENU CONSOLE, SID SCREEN, ...) is emptied (count at -4 set to 0).

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

// Occurrences of text (n bytes incl. its NUL, NUL-delimited) in [begin, end), up to `cap` offsets. Other threads
// may decommit or protect pages after Regions() looked, so a fault ends the scan of this region (returns -1).
int ScanRegionForString(const uint8_t* begin, const uint8_t* end, const char* text, size_t n, uint32_t* out,
                        int cap) {
  int count = 0;
  __try {
    for (const uint8_t* p = begin; p + n <= end;) {
      p = static_cast<const uint8_t*>(std::memchr(p, text[0], size_t(end - p)));
      if (!p || p + n > end) break;
      if (std::memcmp(p, text, n) == 0 && (p == begin || p[-1] == 0) && count < cap) {
        out[count++] = uint32_t(p - begin);
      }
      ++p;
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return -1;
  }
  return count;
}

// Guest addresses of NUL-delimited occurrences of `text` (including its terminator).
std::vector<uint32_t> FindStrings(uint8_t* base, const std::vector<Region>& regions, const char* text) {
  std::vector<uint32_t> out;
  const size_t n = std::strlen(text) + 1;
  uint32_t hits[64];
  for (const Region& r : regions) {
    const int count = ScanRegionForString(base + r.begin, base + r.end, text, n, hits, 64);
    for (int i = 0; i < count; ++i) out.push_back(r.begin + hits[i]);
  }
  return out;
}

// Guest offsets (4-aligned, relative to begin) of big-endian words equal to one of targets[0..ntargets), up to cap.
// Faults end the scan like ScanRegionForString.
int ScanRegionForPointers(const uint8_t* begin, const uint8_t* end, const uint32_t* targets, size_t ntargets,
                          uint32_t* out, int cap) {
  int count = 0;
  __try {
    for (const uint8_t* p = begin; p + 4 <= end && count < cap; p += 4) {
      const uint32_t v = _byteswap_ulong(*reinterpret_cast<const uint32_t*>(p));
      if (v < kHeapBegin || v >= kHeapEnd) continue;
      for (size_t i = 0; i < ntargets; ++i) {
        if (targets[i] == v) {
          out[count++] = uint32_t(p - begin);
          break;
        }
      }
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return -1;
  }
  return count;
}

// Guest addresses (4-aligned) holding a big-endian pointer to one of `targets`.
std::vector<uint32_t> FindPointers(uint8_t* base, const std::vector<Region>& regions,
                                   const std::vector<uint32_t>& targets) {
  std::vector<uint32_t> out;
  if (targets.empty()) return out;
  uint32_t hits[64];
  for (const Region& r : regions) {
    const uint32_t begin = (r.begin + 3) & ~3u;
    const int count = ScanRegionForPointers(base + begin, base + r.end, targets.data(), targets.size(), hits, 64);
    for (int i = 0; i < count; ++i) out.push_back(begin + hits[i]);
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

  // DEV MENU (hidden in retail: +0x25 ExcludeInRetail) becomes PRACTICE: the dev screen's level loader.
  int unhidden = 0;
  for (uint32_t item : FindPointers(base, regions, FindStrings(base, regions, "DEV MENU"))) {
    if (NameAt(base, item - kItemSize) != "ID_M_STORE" && NameAt(base, item - kItemSize) != "ID_M_OPTIONS") continue;
    base[item + 0x25] = 0;
    ++unhidden;
  }
  for (uint32_t s : FindStrings(base, regions, "DEV MENU")) std::memcpy(REX_RAW_ADDR(s), "PRACTICE", 8);

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
  REXLOG_INFO("main menu: removed STORE from {} list(s), renamed EXTRAS to EXIT in {} place(s), PRACTICE shown in {}",
              removed, renamed, unhidden);
}

void PatchPracticePage(uint8_t* base) {
  const std::vector<Region> regions = Regions();
  int renamed = 0, emptied = 0;
  for (uint32_t s : FindStrings(base, regions, "DEVELOPER MENU")) {
    std::memcpy(REX_RAW_ADDR(s), "PRACTICE\0\0\0\0\0\0", 15);
    ++renamed;
  }
  for (uint32_t item : FindPointers(base, regions, FindStrings(base, regions, "MAIN MENU PC"))) {
    if (NameAt(base, item + kItemSize) != "MAIN MENU CONSOLE") continue;  // the list, not the flow port
    if (REX_LOAD_U32(item - 4) == 0 || REX_LOAD_U32(item - 4) > 16) continue;
    REX_STORE_U32(item - 4, 0);
    ++emptied;
  }
  REXLOG_INFO("practice page: header renamed in {} place(s), shortcut list emptied in {}", renamed, emptied);
}

}  // namespace

// From the screen push hook (ui_screens.cpp), before the screen is built.
void bf3_main_menu_before_push(uint8_t* base, const std::string& name) {
  static bool patched = false;
  if (name == "MainMenuScreen" && !patched) {
    patched = true;
    PatchMainMenu(base);
  }
  if (name == "DevScreen") PatchPracticePage(base);
  if (name == "ExtraMenuScreen") {  // EXIT
    REXLOG_INFO("main menu: EXIT selected, closing the game");
    REX_STORE_U32(kQuitFlag, 1);
  }
}
