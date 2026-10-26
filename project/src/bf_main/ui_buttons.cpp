// Gives the Start button prompt its own atlas glyph, so PC keycaps can be drawn for it (see src/pc_icons.cpp).
//
// ui/assets/buttons (CFX movie in Data/Xenon/UI.sb) draws Start as shape 64: the Back glyph (sub-image 62, atlas
// 1127..1177 x 0..42) with a mirrored bitmap fill (scale x -2, tx +50 px). A mirrored keycap reads backwards
// ("TAB" -> "BAT"), so the fill is re-pointed at sub-image 25 (atlas 1021..1072 x 0..42, the PS3 R1 glyph, never
// shown on Xbox) without the mirror, and pc_icons draws the Start key there.
//
// The movie is zlib-compressed and parsed right after Scaleform inflates it, so the bytes are patched as they come
// out of zlib's inflate (sub_8318B500, the only inflate in BF.Main; z_stream: +12 next_out). The tag is found by its
// header (DefineShape, long form, 51 bytes, id 64) and the fill's matrix is re-encoded with the same bit widths, so
// nothing else in the stream moves. The same hook gives the PRACTICE page its column titles (practice_menu.cpp).

// Compiled into the bf3_BF_Main DLL (see CMakeLists.txt): bf3_pch.h is generated/bf_main/bf3_pch.h.
#include "bf3_pch.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>

REX_EXTERN(__imp__sub_8318B500);

namespace {

constexpr uint8_t kStartShapeTag[] = {0xBF, 0x00, 0x33, 0x00, 0x00, 0x00, 0x40, 0x00};
constexpr size_t kTagLength = 6 + 51;
constexpr uint16_t kBackSubImage = 62;
constexpr uint16_t kStartSubImage = 25;

bool Enabled() {
  static const bool on = [] {
    const char* env = std::getenv("BF3_PC_ICONS");
    return !(env && *env == '0');
  }();
  return on;
}

class BitReader {
 public:
  BitReader(const uint8_t* p) : p_(p) {}
  uint32_t U(int n) {
    uint32_t v = 0;
    for (int i = 0; i < n; ++i, ++bit_) v = v << 1 | ((p_[bit_ >> 3] >> (7 - (bit_ & 7))) & 1);
    return v;
  }
  int32_t S(int n) {
    uint32_t v = U(n);
    return n && (v >> (n - 1)) ? int32_t(v | (~0u << n)) : int32_t(v);
  }
  void Align() { bit_ = (bit_ + 7) & ~size_t(7); }
  size_t bit() const { return bit_; }

 private:
  const uint8_t* p_;
  size_t bit_ = 0;
};

class BitWriter {
 public:
  BitWriter(uint8_t* p, size_t bit) : p_(p), bit_(bit) {}
  void U(uint32_t v, int n) {
    for (int i = n - 1; i >= 0; --i, ++bit_) {
      uint8_t& byte = p_[bit_ >> 3];
      const uint8_t mask = uint8_t(0x80 >> (bit_ & 7));
      byte = (v >> i) & 1 ? (byte | mask) : (byte & ~mask);
    }
  }

 private:
  uint8_t* p_;
  size_t bit_;
};

// Rewrites the Back-glyph fill of the Start shape. Returns true if it patched.
bool PatchStartShape(uint8_t* tag) {
  uint8_t* body = tag + 6;
  BitReader r(body + 2);  // after the shape id
  const int rect_bits = int(r.U(5));
  for (int i = 0; i < 4; ++i) r.S(rect_bits);
  r.Align();
  size_t pos = 2 + r.bit() / 8;
  const int fills = body[pos++];
  for (int f = 0; f < fills; ++f) {
    const uint8_t type = body[pos++];
    if (type < 0x40 || type > 0x43) return false;
    const uint16_t id = uint16_t(body[pos] | body[pos + 1] << 8);
    pos += 2;
    BitReader m(body + pos);
    if (!m.U(1)) return false;  // HasScale
    const int scale_bits = int(m.U(5));
    const size_t scale_x_bit = m.bit();
    const int32_t sx = m.S(scale_bits), sy = m.S(scale_bits);
    if (m.U(1)) return false;  // HasRotate: not expected here
    const int translate_bits = int(m.U(5));
    const size_t tx_bit = m.bit();
    const int32_t tx = m.S(translate_bits);
    m.S(translate_bits);
    m.Align();
    if (id == kBackSubImage && sx < 0) {
      body[pos - 2] = uint8_t(kStartSubImage & 0xFF);
      body[pos - 1] = uint8_t(kStartSubImage >> 8);
      BitWriter w(body + pos, scale_x_bit);
      w.U(uint32_t(-sx), scale_bits);
      BitWriter wt(body + pos, tx_bit);
      wt.U(uint32_t(-tx), translate_bits);
      REXLOG_INFO("ui buttons: Start prompt uses its own glyph (scale {} {}, tx {} twips)", -sx / 65536.0, sy / 65536.0,
                  -tx);
      return true;
    }
    pos += m.bit() / 8;
  }
  return false;
}

}  // namespace

void PracticeMenuPatchMovie(uint32_t stream, uint8_t* begin, uint8_t* end, bool done);  // practice_menu.cpp

// inflate(z_stream* r3, int flush r4).
extern "C" REX_FUNC(sub_8318B500) {
  const uint32_t strm = ctx.r3.u32;
  const uint32_t out_before = REX_LOAD_U32(strm + 12);
  __imp__sub_8318B500(ctx, base);
  if (!out_before) return;
  const uint32_t out_after = std::max(REX_LOAD_U32(strm + 12), out_before);
  uint8_t* begin = REX_RAW_ADDR(out_before);
  uint8_t* end = begin + (out_after - out_before);
  const int32_t ret = ctx.r3.s32;  // Z_STREAM_END (1) or an error other than Z_BUF_ERROR (-5) ends the stream
  PracticeMenuPatchMovie(strm, begin, end, ret == 1 || (ret < 0 && ret != -5));
  if (!Enabled() || out_after - out_before < kTagLength) return;
  for (uint8_t* p = begin; p + kTagLength <= end;) {
    p = static_cast<uint8_t*>(std::memchr(p, kStartShapeTag[0], size_t(end - p - kTagLength + 1)));
    if (!p) break;
    if (std::memcmp(p, kStartShapeTag, sizeof(kStartShapeTag)) == 0 && PatchStartShape(p)) break;
    ++p;
  }
}
