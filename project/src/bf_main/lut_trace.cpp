// Diagnostic for the color grading LUT (BF3_LUT_TRACE=1). sub_83C8EB28 is the job that builds the 32x32x32 LUT
// the tonemap pass samples: for each 32x32 slice it copies the first source LUT, blends the others in with 8-bit
// weights (VMX), tiles the slice and copies it to the texture. Job data (r3):
//   +0 slice counter ptr, +4 source count, +12 + 4*(i-1) weight of source i (0..256), +100 dest base,
//   +108 dest slice stride, +112 source 0 base, +144 source 0 slice stride, +104 + 4*i / +136 + 4*i source i base /
//   stride (i >= 1), +32..+63 tiling parameters.

#include "bf3_pch.h"

#include <atomic>
#include <mutex>
#include <cstdlib>
#include <cstring>

REX_EXTERN(__imp__sub_83C8EB28);
REX_EXTERN(__imp__sub_83D31208);
REX_EXTERN(__imp__sub_83D31650);

namespace {

bool LutTraceEnabled() {
  static const bool enabled = [] {
    const char* v = std::getenv("BF3_LUT_TRACE");
    return v && std::strcmp(v, "1") == 0;
  }();
  return enabled;
}

std::mutex g_lut_mutex;
uint32_t g_lut_seen[64];
int g_lut_seen_count = 0;

// First call for each (destination, first source) pair.
bool FirstLutCall(uint32_t dest, uint32_t src) {
  std::lock_guard<std::mutex> lock(g_lut_mutex);
  const uint32_t key = dest ^ (src * 2654435761u);
  for (int i = 0; i < g_lut_seen_count; ++i) {
    if (g_lut_seen[i] == key) return false;
  }
  if (g_lut_seen_count == 64) return false;
  g_lut_seen[g_lut_seen_count++] = key;
  return true;
}

std::string Texels(uint8_t* base, uint32_t addr) {
  std::string s;
  for (uint32_t i = 0; i < 4; ++i) {
    s += fmt::format("{:08X} ", REX_LOAD_U32(addr + i * 4));
  }
  return s;
}

}  // namespace

extern "C" REX_FUNC(sub_83C8EB28) {
  if (LutTraceEnabled() &&
      FirstLutCall(REX_LOAD_U32(ctx.r3.u32 + 100), REX_LOAD_U32(ctx.r3.u32 + 112))) {
    const uint32_t job = ctx.r3.u32;
    const uint32_t count = REX_LOAD_U32(job + 4);
    REXLOG_INFO("lut: job {:08X} count {} dest {:08X} stride {:X} counter {}", job, count, REX_LOAD_U32(job + 100),
                REX_LOAD_U32(job + 108), REX_LOAD_U32(REX_LOAD_U32(job)));
    std::string matrix;
    for (uint32_t i = 0; i < 16; ++i) {
      uint32_t bits = REX_LOAD_U32(job + 32 + i * 4);
      float f;
      std::memcpy(&f, &bits, 4);
      matrix += fmt::format("{:.3f}{}", f, (i & 3) == 3 ? " | " : " ");
    }
    std::string curves;
    for (uint32_t c = 0; c < 3; ++c) {
      for (uint32_t x : {0u, 16u, 64u, 128u, 192u, 255u}) {
        curves += fmt::format("{:02X} ", REX_LOAD_U8(job + 160 + c * 256 + x));
      }
      curves += "| ";
    }
    REXLOG_INFO("lut:   matrix {} curves {}", matrix, curves);
    for (uint32_t i = 0; i < count && i < 8; ++i) {
      const uint32_t src = i ? REX_LOAD_U32(job + 104 + 4 * i) : REX_LOAD_U32(job + 112);
      const uint32_t stride = i ? REX_LOAD_U32(job + 136 + 4 * i) : REX_LOAD_U32(job + 144);
      const uint32_t weight = i ? REX_LOAD_U32(job + 12 + 4 * (i - 1)) : 0;
      REXLOG_INFO("lut:   src{} {:08X} stride {:X} weight {} slice0 {} slice16 {}", i, src, stride, weight,
                  Texels(base, src), Texels(base, src + 16 * stride + 16 * 32 * 4 + 64));
    }
  }
  __imp__sub_83C8EB28(ctx, base);
}

// Schedules the LUT job: r5 = 64-byte color matrix (4x4 floats), r3 = object holding the three 256-byte curves.
extern "C" REX_FUNC(sub_83D31208) {
  static std::atomic<int> logs{0};
  if (LutTraceEnabled() && logs.fetch_add(1) < 40) {
    std::string matrix;
    for (uint32_t i = 0; i < 16; ++i) {
      uint32_t bits = REX_LOAD_U32(ctx.r5.u32 + i * 4);
      float f;
      std::memcpy(&f, &bits, 4);
      matrix += fmt::format("{:.3f}{}", f, (i & 3) == 3 ? " | " : " ");
    }
    REXLOG_INFO("lut: schedule lr {:08X} this {:08X} r4 {} matrix {:08X}: {}", static_cast<uint32_t>(ctx.lr), ctx.r3.u32,
                ctx.r4.u32, ctx.r5.u32, matrix);
  }
  __imp__sub_83D31208(ctx, base);
}

// Caller of sub_83D31208 (virtual): r5 = the same color matrix.
extern "C" REX_FUNC(sub_83D31650) {
  static std::atomic<int> logs{0};
  if (LutTraceEnabled() && logs.fetch_add(1) < 12) {
    float m0, m5;
    uint32_t b0 = REX_LOAD_U32(ctx.r5.u32), b5 = REX_LOAD_U32(ctx.r5.u32 + 20);
    std::memcpy(&m0, &b0, 4);
    std::memcpy(&m5, &b5, 4);
    REXLOG_INFO("lut: update lr {:08X} this {:08X} r4 {:08X} matrix {:08X} m00 {} m11 {} r6 {:08X}",
                static_cast<uint32_t>(ctx.lr), ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, m0, m5, ctx.r6.u32);
  }
  __imp__sub_83D31650(ctx, base);
}
