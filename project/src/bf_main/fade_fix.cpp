// Screen fades (FadeManager, pointer at 0x840BAE54):
//   sub_83247D38(f1 duration, f2, r3 manager, r4 debug name, r5..r9 flags, [r1+92] listener)  fade in
//   sub_83247E10(f1 duration, r3 manager, r4 debug name, r5..r8 flags, [r1+84] listener)      fade out
// The listener (vtable +4 called when the fade completes) may be 0.
// Callers: ClientFadeEntity (its FadeIn/FadeOut events; StartFaded fades out when the entity is created),
// ClientNetSwitchboard::onScreenFadeMessage, ClientLevel, UISystem movies.
//
// Fix: a level's FadeEntity starts faded (screen, UI, audio) and waits for a FadeIn event from the level's logic. In
// the front end it arrives ~2 s later; in multiplayer levels started from PRACTICE it never does, so the level runs
// behind a black screen (the tonemap LUT is built with an all-zero color matrix, and the UI is faded too). If no fade
// in follows a StartFaded within kFadeInAfterMs, FadeWatchdogTick (called from the level manager update, see
// level_boot.cpp) fades in itself, the way the FadeEntity's FadeIn event does. BF3_FADE_FIX=0 disables it;
// BF3_FADE_TRACE=1 logs every fade request.

#include "bf3_pch.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <string>

REX_EXTERN(__imp__sub_83247D38);
REX_EXTERN(__imp__sub_83247E10);

namespace {

constexpr uint32_t kFadeManagerPtr = 0x840BAE54;
constexpr uint32_t kFadeInName = 0x82DCF540;  // "FadeEntity - FadeIn"
constexpr uint32_t kStartFadedName = 0x82DCF6C0;  // "FadeEntity - StartFaded"
constexpr uint64_t kFadeInAfterMs = 5000;

bool EnvFlag(const char* name, bool fallback) {
  const char* v = std::getenv(name);
  return v && *v ? *v == '1' : fallback;
}

bool FadeTrace() {
  static const bool enabled = EnvFlag("BF3_FADE_TRACE", false);
  return enabled;
}

bool FadeFix() {
  static const bool enabled = EnvFlag("BF3_FADE_FIX", true);
  return enabled;
}

uint64_t NowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

// Time of the last StartFaded not followed by a fade in (0 = none pending), and its flags.
std::atomic<uint64_t> g_start_faded_ms{0};
std::atomic<uint32_t> g_start_faded_flags{0};

std::string GuestString(uint8_t* base, uint32_t addr) {
  if (!addr) return "(null)";
  return std::string(reinterpret_cast<const char*>(REX_RAW_ADDR(addr)), 0, 48).c_str();
}

}  // namespace

extern "C" REX_FUNC(sub_83247D38) {
  if (FadeTrace()) {
    REXLOG_INFO("fade: IN '{}' duration {} f2 {} lr {:08X} flags {} {} {} {} {}", GuestString(base, ctx.r4.u32),
                ctx.f1.f64, ctx.f2.f64, static_cast<uint32_t>(ctx.lr), ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, ctx.r8.u32,
                ctx.r9.u32);
  }
  g_start_faded_ms = 0;
  __imp__sub_83247D38(ctx, base);
}

extern "C" REX_FUNC(sub_83247E10) {
  if (FadeTrace()) {
    REXLOG_INFO("fade: OUT '{}' duration {} lr {:08X} flags {} {} {} {}", GuestString(base, ctx.r4.u32), ctx.f1.f64,
                static_cast<uint32_t>(ctx.lr), ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, ctx.r8.u32);
  }
  if (ctx.r4.u32 == kStartFadedName) {
    g_start_faded_flags = (ctx.r5.u32 & 0xFF) | (ctx.r6.u32 & 0xFF) << 8 | (ctx.r7.u32 & 0xFF) << 16 |
                          (ctx.r8.u32 & 0xFF) << 24;
    g_start_faded_ms = NowMs();
  }
  __imp__sub_83247E10(ctx, base);
}

// Called on the main thread every frame (level manager update).
void FadeWatchdogTick(PPCContext& ctx, uint8_t* base) {
  const uint64_t since = g_start_faded_ms.load();
  if (!FadeFix() || !since || NowMs() - since < kFadeInAfterMs) return;
  g_start_faded_ms = 0;
  const uint32_t manager = REX_LOAD_U32(kFadeManagerPtr);
  if (!manager) return;
  const uint32_t flags = g_start_faded_flags.load();
  REXLOG_INFO("fade fix: no FadeIn {} ms after the level's StartFaded, fading in", kFadeInAfterMs);
  PPCContext saved = ctx;
  ctx.f1.f64 = 1.0;  // duration (s)
  ctx.f2.f64 = 0.0;
  ctx.r3.u64 = manager;
  ctx.r4.u64 = kFadeInName;
  ctx.r5.u64 = flags & 0xFF;
  ctx.r6.u64 = (flags >> 8) & 0xFF;
  ctx.r7.u64 = (flags >> 16) & 0xFF;
  ctx.r8.u64 = (flags >> 24) & 0xFF;
  ctx.r9.u64 = 0;
  // Stack argument at the caller's r1+92: the listener told when the fade completes (the FadeEntity passes itself
  // +0x14). None here: call from a scratch frame with it cleared.
  ctx.r1.u64 = (saved.r1.u32 - 256) & ~0xFull;
  REX_STORE_U32(ctx.r1.u32 + 92, 0);
  sub_83247D38(ctx, base);
  ctx = saved;
}
