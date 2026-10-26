// PC-native button icons (see pc_icons.h).
//
// BF3's menus and HUD draw controller glyphs from one texture atlas: ui/assets/buttons (a small GFX movie in
// Data/Xenon/UI.sb) whose image buttons_i1 is a Frostbite texture, 2048x64, format 9 (32-bit ARGB, big-endian texels,
// Xbox 360 2D-tiled), 0x80000 bytes of pixels in chunk ecb69283-d1a4-57c5-24c7-e09691a421f9. It holds the Xbox and
// PS3 glyphs side by side (boxes below, measured from the atlas's alpha). The legends name glyphs by button
// (IDB_Rdown = A, IDB_Rright = B, ...), so redrawing the Xbox glyphs as keycaps changes every prompt in the game.
//
// The atlas is found in guest physical memory by fingerprint once the UI has loaded it (a u64 at +0x78 of the first
// 4 KB tile, then an FNV-1a hash of that tile), copied as the pristine original, and overwritten in place through the
// guest's 0xA0000000 view (the GPU texture cache's write watches see that and re-upload). Each keycap shows the PC key
// for that button in the current context (pc_keys::GlyphKey: menu keys in menus, the on-foot key while playing), so
// prompts change between menus and gameplay and follow remapping in PC SETTINGS. No game data is stored here: only the fingerprint and glyph boxes. Start gets the PS3 R1
// glyph's place (bf_main/ui_buttons.cpp re-points its shape there; the movie mirrored the Back glyph for it).

#include "pc_icons.h"
#include "pc_keys.h"

#if defined(_WIN32)
#include <windows.h>

#include <rex/cvar.h>
#include <rex/logging.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

namespace bf3::pc_icons {
namespace {

constexpr int kWidth = 2048;
constexpr int kHeight = 64;
constexpr size_t kBytes = size_t(kWidth) * kHeight * 4;
constexpr size_t kProbeOffset = 0x78;
constexpr uint64_t kProbeValue = 0x00001C1C02133B3Bull;  // big-endian u64 at +0x78 of the first tile
constexpr uint64_t kTileHash = 0x4954AEEECE39DDF6ull;    // FNV-1a 64 of the first 4 KB
constexpr uint64_t kGuestBase = 0x100000000ull;
constexpr uint64_t kPhysicalView = 0x200000000ull;
constexpr uint32_t kPhysicalSize = 0x20000000;

enum class Art { kKey, kMouseLeft, kMouseRight, kMouseMove, kArrows };

// Xbox glyphs in the atlas: box (x, y, w, h) and the emulated button it stands for.
struct Glyph {
  int x, y, w, h;
  const char* button;  // controller button whose PC key is shown (pc_keys::GlyphKey)
  Art art;
  const char* name;
  int clear_x = 0, clear_y = 0, clear_w = 0, clear_h = 0;  // area cleared first (a reused glyph's sub-image)
};

const Glyph kGlyphs[] = {
    {2, 2, 48, 48, "D-pad", Art::kArrows, "D-pad"},
    {56, 2, 48, 49, "Stick", Art::kKey, "Stick"},  // bare stick (e.g. SHOW BATTLE FEED)
    {376, 2, 47, 44, "LS", Art::kKey, "LS"},
    {482, 2, 46, 45, "RT", Art::kKey, "RT"},
    {534, 2, 46, 45, "LT", Art::kKey, "LT"},
    {585, 0, 47, 44, "RS", Art::kKey, "RS"},
    {639, 2, 42, 41, "Y", Art::kKey, "Y"},
    {687, 2, 42, 41, "X", Art::kKey, "X"},
    {735, 2, 42, 41, "B", Art::kKey, "B"},
    {783, 2, 42, 41, "A", Art::kKey, "A"},
    {1129, 2, 46, 39, "Back", Art::kKey, "Back"},
    {1181, 2, 50, 37, "LB", Art::kKey, "LB"},
    {1235, 2, 50, 37, "RB", Art::kKey, "RB"},
    // Start: the movie draws it as the mirrored Back glyph; bf_main/ui_buttons.cpp points it at the PS3 R1 glyph's
    // sub-image (1021..1072 x 0..42) instead, drawn like Back's (+2, +2 inside its sub-image).
    {1023, 2, 46, 39, "Start", Art::kKey, "Start", 1021, 0, 51, 42},
};

// Same addressing as the GPU (XGAddress2DTiledOffset), in texels, for 32bpp.
uint32_t TiledOffset(uint32_t x, uint32_t y) {
  constexpr uint32_t log_bpp = 2;
  const uint32_t aligned = (kWidth + 31) & ~31u;
  const uint32_t macro = ((x >> 5) + (y >> 5) * (aligned >> 5)) << (log_bpp + 7);
  const uint32_t micro = ((x & 7) + ((y & 0xE) << 2)) << log_bpp;
  const uint32_t off = macro + ((micro & ~0xFu) << 1) + (micro & 0xF) + ((y & 1) << 4);
  return (((off & ~0x1FFu) << 3) + ((y & 16) << 7) + ((off & 0x1C0) << 2) + (((((y & 8) >> 2) + (x >> 3)) & 3) << 6) +
          (off & 0x3F)) >>
         log_bpp;
}

uint64_t Fnv1a(const uint8_t* p, size_t n) {
  uint64_t h = 0xCBF29CE484222325ull;
  for (size_t i = 0; i < n; ++i) h = (h ^ p[i]) * 0x100000001B3ull;
  return h;
}

uint64_t LoadBe64(const uint8_t* p) {
  uint64_t v = 0;
  for (int i = 0; i < 8; ++i) v = v << 8 | p[i];
  return v;
}

// ---- key labels -----------------------------------------------------------------------------------------------

// The key a prompt shows: the menu binding in menus, the on-foot key while playing (pc_keys.cpp).
std::string DisplayKey(const char* button) { return pc_keys::GlyphKey(button); }

std::string Label(const std::string& key) {
  static const std::pair<const char*, const char*> kNames[] = {
      {"Return", "ENTER"}, {"Escape", "ESC"},      {"Space", "SPACE"},   {"Backspace", "BKSP"}, {"Control", "CTRL"},
      {"Shift", "SHIFT"},  {"Tab", "TAB"},         {"Menu", "ALT"},      {"Alt", "ALT"},        {"Delete", "DEL"},
      {"Insert", "INS"},   {"PageUp", "PGUP"},     {"PageDown", "PGDN"}, {"Home", "HOME"},      {"End", "END"},
      {"CapsLock", "CAPS"}, {"OemTilde", "~"},     {"Backtick", "~"},
      {"WheelUp", "WHEEL"}, {"WheelDown", "WHEEL"}, {"Mouse4", "M4"},    {"Mouse5", "M5"},
  };
  for (auto [name, label] : kNames) {
    if (key == name) return label;
  }
  std::string out = key;
  for (char& c : out) c = char(toupper(static_cast<unsigned char>(c)));
  return out;
}

// ---- drawing ----------------------------------------------------------------------------------------------------

constexpr int kSuper = 4;  // supersampling factor for smooth edges

// A coverage mask drawn with GDI at kSuper x the glyph size (white on black).
class Mask {
 public:
  Mask(int w, int h) : w_(w * kSuper), h_(h * kSuper) {
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = w_;
    bi.bmiHeader.biHeight = -h_;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    dc_ = CreateCompatibleDC(nullptr);
    bmp_ = CreateDIBSection(dc_, &bi, DIB_RGB_COLORS, reinterpret_cast<void**>(&bits_), nullptr, 0);
    old_ = SelectObject(dc_, bmp_);
    std::memset(bits_, 0, size_t(w_) * h_ * 4);
  }
  ~Mask() {
    SelectObject(dc_, old_);
    DeleteObject(bmp_);
    DeleteDC(dc_);
  }
  HDC dc() const { return dc_; }
  int w() const { return w_; }
  int h() const { return h_; }
  // Coverage 0..1 at output pixel (x, y): average of the kSuper x kSuper block.
  float At(int x, int y) const {
    int sum = 0;
    for (int j = 0; j < kSuper; ++j) {
      const uint32_t* row = bits_ + size_t(y * kSuper + j) * w_ + x * kSuper;
      for (int i = 0; i < kSuper; ++i) sum += row[i] & 0xFF;
    }
    return sum / (255.0f * kSuper * kSuper);
  }
  void RoundRect(float x0, float y0, float x1, float y1, float r) {
    HBRUSH brush = CreateSolidBrush(RGB(255, 255, 255));
    HGDIOBJ ob = SelectObject(dc_, brush);
    HGDIOBJ op = SelectObject(dc_, GetStockObject(NULL_PEN));
    ::RoundRect(dc_, int(x0 * kSuper), int(y0 * kSuper), int(x1 * kSuper) + 1, int(y1 * kSuper) + 1,
                int(r * 2 * kSuper), int(r * 2 * kSuper));
    SelectObject(dc_, op);
    SelectObject(dc_, ob);
    DeleteObject(brush);
  }
  void Polygon(std::initializer_list<std::pair<float, float>> pts) {
    std::vector<POINT> p;
    for (auto [x, y] : pts) p.push_back({LONG(x * kSuper), LONG(y * kSuper)});
    HBRUSH brush = CreateSolidBrush(RGB(255, 255, 255));
    HGDIOBJ ob = SelectObject(dc_, brush);
    HGDIOBJ op = SelectObject(dc_, GetStockObject(NULL_PEN));
    ::Polygon(dc_, p.data(), int(p.size()));
    SelectObject(dc_, op);
    SelectObject(dc_, ob);
    DeleteObject(brush);
  }
  // Text centered in the box, shrunk until it fits max_w.
  void Text(const std::string& text, float cx, float cy, float max_w, float max_h) {
    SetBkMode(dc_, TRANSPARENT);
    SetTextColor(dc_, RGB(255, 255, 255));
    for (float size = max_h; size >= 6; size -= 0.5f) {
      HFONT font = CreateFontA(-int(size * kSuper), 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, OUT_TT_PRECIS,
                               CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH, "Bahnschrift SemiBold Condensed");
      HGDIOBJ of = SelectObject(dc_, font);
      SIZE ext{};
      GetTextExtentPoint32A(dc_, text.c_str(), int(text.size()), &ext);
      TEXTMETRICA tm{};
      GetTextMetricsA(dc_, &tm);
      if (ext.cx <= max_w * kSuper || size <= 6) {
        // Center on the cap height rather than the full em box.
        const int cap = tm.tmAscent - tm.tmInternalLeading;
        const int x = int(cx * kSuper) - ext.cx / 2;
        const int y = int(cy * kSuper) - tm.tmInternalLeading - cap / 2 - int(cap * 0.04f);
        TextOutA(dc_, x, y, text.c_str(), int(text.size()));
        SelectObject(dc_, of);
        DeleteObject(font);
        return;
      }
      SelectObject(dc_, of);
      DeleteObject(font);
    }
  }

 private:
  int w_, h_;
  HDC dc_;
  HBITMAP bmp_;
  HGDIOBJ old_;
  uint32_t* bits_ = nullptr;
};

struct Rgb {
  float r, g, b;
};
// Colors of the atlas's own key glyphs (LB/RB): pale cap, dark legend, soft blue glow.
constexpr Rgb kCap{0xE3, 0xF1, 0xF1};
constexpr Rgb kInk{0x22, 0x27, 0x22};
constexpr Rgb kGlow{0x6E, 0xBE, 0xFA};

// Renders one glyph (w x h) as non-premultiplied ARGB.
std::vector<uint32_t> Render(const Glyph& g, const std::string& key) {
  const int w = g.w, h = g.h;
  Mask body(w, h), ink(w, h), glow(w, h);
  const float m = 2.0f;  // margin left for the glow
  std::string label = Label(key);
  Art art = g.art;
  if (key == "LMB") art = Art::kMouseLeft;
  if (key == "RMB") art = Art::kMouseRight;
  if (key == "MMB") art = Art::kMouseMove;
  const bool arrow_key = key == "Up" || key == "Down" || key == "Left" || key == "Right";

  if (art == Art::kArrows) {
    // Arrow-key cluster: up on top, left/down/right below.
    const float s = std::min((w - 2 * m) / 3.0f, (h - 2 * m) / 2.0f) - 1.0f;
    const float x0 = (w - 3 * s - 2) / 2.0f, y0 = (h - 2 * s - 1) / 2.0f;
    auto key_at = [&](float x, float y, int dir) {
      body.RoundRect(x, y, x + s, y + s, s * 0.18f);
      glow.RoundRect(x - 1.5f, y - 1.5f, x + s + 1.5f, y + s + 1.5f, s * 0.25f);
      const float cx = x + s / 2, cy = y + s / 2, a = s * 0.22f;
      switch (dir) {
        case 0: ink.Polygon({{cx, cy - a}, {cx + a, cy + a * 0.6f}, {cx - a, cy + a * 0.6f}}); break;
        case 1: ink.Polygon({{cx, cy + a}, {cx + a, cy - a * 0.6f}, {cx - a, cy - a * 0.6f}}); break;
        case 2: ink.Polygon({{cx - a, cy}, {cx + a * 0.6f, cy - a}, {cx + a * 0.6f, cy + a}}); break;
        case 3: ink.Polygon({{cx + a, cy}, {cx - a * 0.6f, cy - a}, {cx - a * 0.6f, cy + a}}); break;
      }
    };
    key_at(x0 + s + 1, y0, 0);
    key_at(x0, y0 + s + 1, 2);
    key_at(x0 + s + 1, y0 + s + 1, 1);
    key_at(x0 + 2 * s + 2, y0 + s + 1, 3);
  } else if (art == Art::kMouseLeft || art == Art::kMouseRight || art == Art::kMouseMove) {
    // Mouse: rounded body, the pressed button filled with ink.
    const float bh = h - 2 * m, bw = std::min(w - 2 * m, bh * 0.68f);
    const float x0 = (w - bw) / 2, y0 = m, x1 = x0 + bw, y1 = y0 + bh, mid = (x0 + x1) / 2, split = y0 + bh * 0.42f;
    body.RoundRect(x0, y0, x1, y1, bw * 0.48f);
    glow.RoundRect(x0 - 1.5f, y0 - 1.5f, x1 + 1.5f, y1 + 1.5f, bw * 0.5f);
    const float gap = std::max(1.0f, bw * 0.05f);
    ink.Polygon({{mid - gap / 2, y0}, {mid + gap / 2, y0}, {mid + gap / 2, split}, {mid - gap / 2, split}});
    ink.Polygon({{x0, split - gap / 2}, {x1, split - gap / 2}, {x1, split + gap / 2}, {x0, split + gap / 2}});
    if (art == Art::kMouseLeft) ink.RoundRect(x0 + gap, y0 + gap, mid - gap, split - gap, bw * 0.3f);
    if (art == Art::kMouseRight) ink.RoundRect(mid + gap, y0 + gap, x1 - gap, split - gap, bw * 0.3f);
    if (art == Art::kMouseMove) ink.RoundRect(mid - bw * 0.1f, y0 + bh * 0.1f, mid + bw * 0.1f, split - gap, bw * 0.1f);
  } else {
    // Keycap with the key's name.
    body.RoundRect(m, m, w - m, h - m, std::min(w, h) * 0.16f);
    glow.RoundRect(m - 1.5f, m - 1.5f, w - m + 1.5f, h - m + 1.5f, std::min(w, h) * 0.2f);
    if (arrow_key) {
      const float cx = w / 2.0f, cy = h / 2.0f, a = std::min(w, h) * 0.22f;
      if (key == "Up") ink.Polygon({{cx, cy - a}, {cx + a, cy + a * 0.6f}, {cx - a, cy + a * 0.6f}});
      if (key == "Down") ink.Polygon({{cx, cy + a}, {cx + a, cy - a * 0.6f}, {cx - a, cy - a * 0.6f}});
      if (key == "Left") ink.Polygon({{cx - a, cy}, {cx + a * 0.6f, cy - a}, {cx + a * 0.6f, cy + a}});
      if (key == "Right") ink.Polygon({{cx + a, cy}, {cx - a * 0.6f, cy - a}, {cx - a * 0.6f, cy + a}});
    } else if (!label.empty()) {
      const float cap_h = (h - 2 * m) * (label.size() <= 2 ? 0.62f : 0.5f);
      ink.Text(label, w / 2.0f, h / 2.0f, w - 2 * m - 6, cap_h);
    }
  }

  std::vector<uint32_t> out(size_t(w) * h);
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      const float b = body.At(x, y), t = ink.At(x, y) * b, gl = glow.At(x, y);
      // Glow under the cap, cap over it, ink on the cap.
      const float a_glow = gl * 0.45f * (1 - b);
      const float a = std::min(1.0f, b + a_glow);
      Rgb c{0, 0, 0};
      if (a > 0) {
        const float wb = b / a, wg = a_glow / a;
        c.r = (kCap.r * (1 - t) + kInk.r * t) * wb + kGlow.r * wg;
        c.g = (kCap.g * (1 - t) + kInk.g * t) * wb + kGlow.g * wg;
        c.b = (kCap.b * (1 - t) + kInk.b * t) * wb + kGlow.b * wg;
      }
      auto u8 = [](float v) { return uint32_t(std::clamp(v, 0.0f, 255.0f) + 0.5f); };
      out[size_t(y) * w + x] = u8(a * 255) << 24 | u8(c.r) << 16 | u8(c.g) << 8 | u8(c.b);
    }
  }
  return out;
}

// The pristine atlas (tiled, as in memory) with the Xbox glyphs redrawn for the current bindings.
std::vector<uint8_t> Patched(const std::vector<uint8_t>& pristine) {
  std::vector<uint8_t> out = pristine;
  for (const Glyph& g : kGlyphs) {
    for (int y = 0; y < g.clear_h; ++y) {
      for (int x = 0; x < g.clear_w; ++x) {
        std::memset(out.data() + size_t(TiledOffset(g.clear_x + x, g.clear_y + y)) * 4, 0, 4);
      }
    }
    std::string key = DisplayKey(g.button);
    std::vector<uint32_t> px = g.art == Art::kArrows ? Render(g, "") : Render(g, key);
    for (int y = 0; y < g.h; ++y) {
      for (int x = 0; x < g.w; ++x) {
        const uint32_t v = px[size_t(y) * g.w + x];
        uint8_t* t = out.data() + size_t(TiledOffset(g.x + x, g.y + y)) * 4;
        t[0] = uint8_t(v >> 24), t[1] = uint8_t(v >> 16), t[2] = uint8_t(v >> 8), t[3] = uint8_t(v);
      }
    }
  }
  return out;
}

std::string BindingsKey() {
  std::string out;
  for (const Glyph& g : kGlyphs) out += DisplayKey(g.button) + ";";
  return out;
}

// ---- finding and patching -------------------------------------------------------------------------------------

std::mutex g_mutex;
std::vector<uint8_t> g_pristine;
std::vector<uint32_t> g_locations;  // physical addresses of patched copies
std::string g_patched_for;          // bindings the copies were drawn for
uint64_t g_patched_hash = 0;        // FNV of the first tile once patched
std::atomic<bool> g_refresh{false};

bool Readable(const MEMORY_BASIC_INFORMATION& mbi) {
  if (mbi.State != MEM_COMMIT) return false;
  const DWORD p = mbi.Protect & 0xFF;
  return p != PAGE_NOACCESS && !(mbi.Protect & PAGE_GUARD);
}

// Physical addresses (4 KB aligned) whose first tile has the given hash.
std::vector<uint32_t> Find(uint64_t hash, bool probe) {
  std::vector<uint32_t> out;
  uint64_t addr = kPhysicalView;
  const uint64_t end = kPhysicalView + kPhysicalSize;
  MEMORY_BASIC_INFORMATION mbi;
  while (addr < end && VirtualQuery(reinterpret_cast<void*>(addr), &mbi, sizeof(mbi)) == sizeof(mbi)) {
    const uint64_t region_end = std::min<uint64_t>(reinterpret_cast<uint64_t>(mbi.BaseAddress) + mbi.RegionSize, end);
    if (Readable(mbi)) {
      for (uint64_t page = (addr + 0xFFF) & ~0xFFFull; page + kBytes <= region_end; page += 0x1000) {
        const uint8_t* p = reinterpret_cast<const uint8_t*>(page);
        if (probe && LoadBe64(p + kProbeOffset) != kProbeValue) continue;
        if (Fnv1a(p, 0x1000) == hash) out.push_back(uint32_t(page - kPhysicalView));
      }
    }
    addr = region_end;
  }
  return out;
}

void Write(uint32_t phys, const std::vector<uint8_t>& data) {
  // Through the guest's 0xA0000000 view so the GPU texture cache's write watches see it.
  std::memcpy(reinterpret_cast<void*>(kGuestBase + 0xA0000000ull + phys), data.data(), data.size());
}

void Scan() {
  std::lock_guard lock(g_mutex);
  std::vector<uint32_t> fresh = Find(kTileHash, true);
  const std::string bindings = BindingsKey();
  const bool rebind = bindings != g_patched_for || g_refresh.exchange(false);
  if (fresh.empty() && !(rebind && !g_locations.empty())) return;
  if (g_pristine.empty() && !fresh.empty()) {
    const uint8_t* src = reinterpret_cast<const uint8_t*>(kPhysicalView + fresh.front());
    g_pristine.assign(src, src + kBytes);
  }
  if (g_pristine.empty()) return;
  std::vector<uint8_t> patched = Patched(g_pristine);
  const uint64_t new_hash = Fnv1a(patched.data(), 0x1000);
  // Copies patched earlier stay ours only while they still hold our pixels.
  std::vector<uint32_t> keep;
  if (rebind) {
    for (uint32_t phys : g_locations) {
      if (Fnv1a(reinterpret_cast<const uint8_t*>(kPhysicalView + phys), 0x1000) == g_patched_hash) {
        keep.push_back(phys);
      }
    }
  }
  for (uint32_t phys : fresh) keep.push_back(phys);
  for (uint32_t phys : keep) Write(phys, patched);
  g_locations = keep;
  g_patched_for = bindings;
  g_patched_hash = new_hash;
  REXLOG_INFO("pc icons: {} button atlas cop{} drawn for [{}] ({} new)", keep.size(), keep.size() == 1 ? "y" : "ies",
              bindings, fresh.size());
}

}  // namespace

void RequestRefresh() { g_refresh = true; }

void Install() {
  if (const char* env = std::getenv("BF3_PC_ICONS"); env && *env == '0') return;
  CreateThread(
      nullptr, 0,
      [](LPVOID) -> DWORD {
        // The UI loads (and reloads with levels) the atlas at unpredictable times; a scan reads one u64 per page.
        for (;;) {
          Sleep(1500);
          Scan();
        }
      },
      nullptr, 0, nullptr);
}

}  // namespace bf3::pc_icons

#else
namespace bf3::pc_icons {
void Install() {}
void RequestRefresh() {}
}  // namespace bf3::pc_icons
#endif
