// Aim scanner (diagnostic, BF3_AIM_SCAN=1): finds the guest floats that hold the soldier's yaw and pitch.
//
// Once in gameplay (HudScreen up, no spawn screen) it drives controller 0's right stick through short holds
// (right, rest, left, rest, then up, rest, down, rest) and snapshots guest memory at each rest. A float that moves
// one way on the first hold, stays put at rest and moves back on the opposite hold follows that axis; candidates are
// logged with their values at every snapshot ("aim scan:" lines).

#if defined(_WIN32)
#include <windows.h>

#include <rex/logging.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "pad_inject.h"
#include "ui_state.h"

namespace bf3 {
namespace {

constexpr uint64_t kGuestBase = 0x100000000ull;
constexpr uint32_t kRanges[][2] = {{0x40000000, 0x80000000}, {0xA0000000, 0xC0000000}};
constexpr float kPi = 3.14159265f;

struct Region {
  uint32_t begin, end;
};

std::vector<Region> Regions() {
  std::vector<Region> out;
  for (const auto& range : kRanges) {
    uint64_t addr = kGuestBase + range[0];
    MEMORY_BASIC_INFORMATION mbi;
    while (addr < kGuestBase + range[1] && VirtualQuery(reinterpret_cast<void*>(addr), &mbi, sizeof(mbi))) {
      const uint64_t end =
          std::min<uint64_t>(reinterpret_cast<uint64_t>(mbi.BaseAddress) + mbi.RegionSize, kGuestBase + range[1]);
      const DWORD prot = mbi.Protect & 0xFF;
      if (mbi.State == MEM_COMMIT && prot != PAGE_NOACCESS && !(mbi.Protect & PAGE_GUARD)) {
        out.push_back({uint32_t(addr - kGuestBase), uint32_t(end - kGuestBase)});
      }
      addr = end;
    }
  }
  return out;
}

bool SafeCopy(void* dst, const void* src, size_t n) {
  __try {
    std::memcpy(dst, src, n);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
  return true;
}

float LoadFloat(uint32_t guest) {
  uint32_t v = 0;
  if (!SafeCopy(&v, reinterpret_cast<const void*>(kGuestBase + guest), 4)) return NAN;
  v = _byteswap_ulong(v);
  float f;
  std::memcpy(&f, &v, 4);
  return f;
}

// Angle-aware difference (wraps across +-pi).
float Delta(float from, float to) {
  float d = to - from;
  if (d > kPi) d -= 2 * kPi;
  if (d < -kPi) d += 2 * kPi;
  return d;
}

bool Gameplay() {
  const std::string live = ui_state::LiveScreens();
  return live.find("HudScreen") != std::string::npos && live.find("Spawn") == std::string::npos &&
         live.find("Loading") == std::string::npos;
}

void Hold(int16_t rx, int16_t ry, DWORD hold_ms, DWORD rest_ms) {
  SetRightStickOverride(true, rx, ry);
  Sleep(hold_ms);
  SetRightStickOverride(true, 0, 0);
  Sleep(rest_ms);
}

// One axis. Snapshots: 0, hold +, 1, rest, 2, hold +, 3, rest, 4, hold -, 5, rest, 6, hold -, 7. The angle moves by
// about the same step a on each + hold and -a on each - hold, never at rest, and is never exactly 0 (scratch memory
// reused between snapshots tends to go 0 -> x -> 0).
void ScanAxis(const char* axis, int16_t rx, int16_t ry) {
  constexpr DWORD kHold = 300, kRest = 700;
  std::vector<Region> regions = Regions();
  size_t total = 0;
  for (const Region& r : regions) total += r.end - r.begin;
  REXLOG_INFO("aim scan {}: {} regions, {} MB", axis, regions.size(), total >> 20);

  std::vector<std::vector<uint32_t>> s0(regions.size());
  for (size_t i = 0; i < regions.size(); ++i) {
    s0[i].resize((regions[i].end - regions[i].begin) / 4);
    if (!SafeCopy(s0[i].data(), reinterpret_cast<const void*>(kGuestBase + regions[i].begin), s0[i].size() * 4)) {
      s0[i].clear();
    }
  }
  Hold(rx, ry, kHold, kRest);

  struct Cand {
    uint32_t addr;
    float v[8];
  };
  std::vector<Cand> cands;
  for (size_t i = 0; i < regions.size(); ++i) {
    for (size_t w = 0; w < s0[i].size(); ++w) {
      const uint32_t addr = regions[i].begin + uint32_t(w * 4);
      uint32_t raw0 = _byteswap_ulong(s0[i][w]);
      float v0;
      std::memcpy(&v0, &raw0, 4);
      if (!std::isfinite(v0) || v0 == 0.0f || std::fabs(v0) > 100.0f) continue;
      const float v1 = LoadFloat(addr);
      if (!std::isfinite(v1) || v1 == 0.0f) continue;
      const float d = std::fabs(Delta(v0, v1));
      if (d < 0.01f || d > 3.0f) continue;
      cands.push_back({addr, {v0, v1}});
    }
    std::vector<uint32_t>().swap(s0[i]);
  }
  REXLOG_INFO("aim scan {}: {} candidates after the first hold", axis, cands.size());

  auto snap = [&](int k) {
    for (Cand& c : cands) c.v[k] = LoadFloat(c.addr);
  };
  snap(2);
  Hold(rx, ry, kHold, kRest);
  snap(3);
  Sleep(kRest);
  snap(4);
  Hold(int16_t(-rx), int16_t(-ry), kHold, kRest);
  snap(5);
  Sleep(kRest);
  snap(6);
  Hold(int16_t(-rx), int16_t(-ry), kHold, kRest);
  snap(7);

  std::vector<Cand> hits;
  for (const Cand& c : cands) {
    bool ok = true;
    for (float v : c.v) ok = ok && std::isfinite(v) && v != 0.0f;
    if (!ok) continue;
    const float a = Delta(c.v[0], c.v[1]);
    auto close_to = [&](float d, float want) { return std::fabs(d - want) <= std::fabs(a) * 0.35f; };
    auto still = [&](float d) { return std::fabs(d) <= std::fabs(a) * 0.03f; };
    if (!still(Delta(c.v[1], c.v[2])) || !close_to(Delta(c.v[2], c.v[3]), a) || !still(Delta(c.v[3], c.v[4])) ||
        !close_to(Delta(c.v[4], c.v[5]), -a) || !still(Delta(c.v[5], c.v[6])) || !close_to(Delta(c.v[6], c.v[7]), -a)) {
      continue;
    }
    hits.push_back(c);
  }
  std::sort(hits.begin(), hits.end(), [](const Cand& x, const Cand& y) { return x.addr < y.addr; });
  REXLOG_INFO("aim scan {}: {} hits", axis, hits.size());
  for (size_t i = 0; i < hits.size() && i < 80; ++i) {
    const Cand& c = hits[i];
    REXLOG_INFO("aim scan {}: {:08X}  {:.4f} {:.4f} {:.4f} {:.4f} {:.4f} {:.4f} {:.4f} {:.4f}", axis, c.addr, c.v[0],
                c.v[1], c.v[2], c.v[3], c.v[4], c.v[5], c.v[6], c.v[7]);
  }
}

}  // namespace

void InstallAimScan() {
  const char* v = std::getenv("BF3_AIM_SCAN");
  if (!v || !*v || *v == '0') return;
  CreateThread(
      nullptr, 0,
      [](LPVOID) -> DWORD {
        while (!Gameplay()) Sleep(500);
        Sleep(8000);
        REXLOG_INFO("aim scan: start");
        ScanAxis("yaw", 20000, 0);
        ScanAxis("pitch", 0, 16000);
        SetRightStickOverride(false);
        REXLOG_INFO("aim scan: done");
        return 0;
      },
      nullptr, 0, nullptr);
}

}  // namespace bf3

#else
namespace bf3 {
void InstallAimScan() {}
}  // namespace bf3
#endif
