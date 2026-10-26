// PC keyboard & mouse into BF3's own input concepts, the way the PC version's keyboard bindings feed them.
//
// Guest side (BF.Main):
// - The client player input update sub_832303C0 has the entry's input mapper fill the EntryInputState:
//   sub_8324E938(mapper r3, state r4, extra state r5, suppress r6). For each entry action (EIA, enum
//   EntryInputActionEnum, 0..99) the mapper's hash map at +0x10 (buckets +0x14, count +0x18; node {u32 eia,
//   u32 concept, next}) names the input concept (enum InputConceptIdentifiers), and the concept source at +4 (the
//   input cache, vtable 0x82DCF628: analog getter sub_8324B728, digital sub_8324C130) evaluates the concept's device
//   bindings. Each entry has its own map (soldier, each vehicle seat type), from the game data.
// - EntryInputState: analog EIAs 0..9 as floats at +0x10 + 4 * eia (signed -1..1 for the bits of 0x37B, else 0..1:
//   sub_83855600), digital EIAs as bits (eia - 10) of the u64 at +0x1D8 (sub_83853A50), and an "action has a binding"
//   mask at +0x1C8 (u64[2], sub_83855578), cleared for concepts without a pad binding in the Xenon data.
// The Xenon data only binds pad inputs, so after the mapper runs, the concepts the PC keys hold (bf3.exe
// bf3_pc_concepts, pc_keys.cpp) are merged into every action the entry maps them to: the stronger analog value wins,
// digital actions are OR'd, and the action is marked bound. Only for the local player's input (calls from
// sub_832303C0) and not while the mapper is told to suppress input.
//
// BF3_INPUT_TRACE=1 logs each mapper's EIA:concept table once (new vehicle types show their map that way).

#include "bf3_pch.h"

#include <windows.h>

#include <algorithm>
#include <bit>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <mutex>
#include <string>
#include <vector>

#include "pc_concepts.h"

REX_EXTERN(__imp__sub_8324E938);

namespace {

constexpr int kConceptCount = 123;
constexpr uint32_t kEiaProne = 14;
constexpr uint32_t kSignedAnalog = 0x37B;  // EIAs 0..9 that take -1..1 (Throttle, Strafe, Yaw, Pitch, Roll, ...)

using ConceptsFn = bool (*)(float*, int, bool);

ConceptsFn PcConcepts() {
  static ConceptsFn fn = reinterpret_cast<ConceptsFn>(GetProcAddress(GetModuleHandleW(nullptr), "bf3_pc_concepts"));
  return fn;
}

std::atomic<bool> g_on_foot{true};

bool Trace() {
  static const bool on = [] {
    const char* v = std::getenv("BF3_INPUT_TRACE");
    return v && *v && *v != '0';
  }();
  return on;
}

struct MapEntry {
  uint32_t eia, id;  // entry action, input concept
};

std::vector<MapEntry> ReadMap(uint8_t* base, uint32_t mapper) {
  std::vector<MapEntry> out;
  const uint32_t buckets = REX_LOAD_U32(mapper + 0x14), count = REX_LOAD_U32(mapper + 0x18);
  if (!buckets || count > 1024) return out;
  for (uint32_t b = 0; b < count; ++b) {
    for (uint32_t node = REX_LOAD_U32(buckets + b * 4), guard = 0; node && guard < 64;
         node = REX_LOAD_U32(node + 8), ++guard) {
      out.push_back({REX_LOAD_U32(node), REX_LOAD_U32(node + 4)});
    }
  }
  return out;
}

void TraceMapper(uint8_t* base, uint32_t mapper, const std::vector<MapEntry>& map, bool on_foot) {
  static std::mutex m;
  static std::vector<uint32_t> seen;
  std::lock_guard lock(m);
  if (std::find(seen.begin(), seen.end(), mapper) != seen.end() || seen.size() >= 64) return;
  seen.push_back(mapper);
  std::string line;
  for (const MapEntry& e : map) line += fmt::format(" {}:{}", e.eia, e.id);
  REXLOG_INFO("pc concepts: mapper {:08X} ({}) eia:concept{}", mapper, on_foot ? "on foot" : "vehicle", line);
}

void Merge(uint8_t* base, uint32_t state, uint32_t eia, float v) {
  const uint32_t avail = state + 0x1C8 + (eia / 64) * 8;
  REX_STORE_U64(avail, REX_LOAD_U64(avail) | (1ull << (eia % 64)));
  if (eia < 10) {
    if (!(kSignedAnalog >> eia & 1)) v = std::fabs(v);
    v = std::clamp(v, -1.0f, 1.0f);
    const uint32_t a = state + (eia + 4) * 4;
    const float cur = std::bit_cast<float>(REX_LOAD_U32(a));
    if (std::fabs(v) > std::fabs(cur)) REX_STORE_U32(a, std::bit_cast<uint32_t>(v));
  } else if (v >= 0.5f) {
    const uint32_t bits = state + 0x1D8;
    REX_STORE_U64(bits, REX_LOAD_U64(bits) | (1ull << (eia - 10)));
  }
}

}  // namespace

namespace bf3_main {
bool LocalOnFoot() { return g_on_foot.load(std::memory_order_relaxed); }
}  // namespace bf3_main

extern "C" REX_FUNC(sub_8324E938) {
  const uint32_t mapper = ctx.r3.u32, state = ctx.r4.u32, lr = uint32_t(ctx.lr);
  const bool suppress = (ctx.r6.u32 & 0xFF) != 0;
  __imp__sub_8324E938(ctx, base);
  if (lr < 0x832303C0 || lr >= 0x83230B44 || !mapper || !state) return;  // local player input only

  const std::vector<MapEntry> map = ReadMap(base, mapper);
  const bool on_foot = std::any_of(map.begin(), map.end(), [](const MapEntry& e) { return e.eia == kEiaProne; });
  g_on_foot.store(on_foot, std::memory_order_relaxed);
  if (Trace()) TraceMapper(base, mapper, map, on_foot);
  if (suppress) return;

  ConceptsFn concepts = PcConcepts();
  float values[kConceptCount];
  if (!concepts || !concepts(values, kConceptCount, on_foot)) return;
  for (const MapEntry& e : map) {
    if (e.eia >= 100 || e.id >= uint32_t(kConceptCount) || values[e.id] == 0.0f) continue;
    Merge(base, state, e.eia, values[e.id]);
  }
}
