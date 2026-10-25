// Audio capture for diagnosing sound (BF3_AUDIO_CAPTURE=<wav path>[:<seconds>], default 60 s): records every frame
// the game submits to the audio driver (XAudioSubmitRenderDriverFrame(driver r3, samples r4): 256 samples x 6
// channels, channel-planar big-endian floats, 48 kHz) into a 6-channel float WAV, and logs per-second stats (frames
// submitted, peak level, silent frames). Works by redirecting bf3_BF_Main.dll's import, like pad_inject.cpp.

#include <cstdint>

namespace bf3 {
void InstallAudioCapture();
}  // namespace bf3

#if defined(_WIN32)
#include <windows.h>

#include <rex/logging.h>
#include <rex/ppc/context.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "iat_patch.h"

namespace bf3 {
namespace {

using GuestFunc = void (*)(PPCContext&, uint8_t*);

constexpr int kChannels = 6;
constexpr int kSamples = 256;

GuestFunc g_original = nullptr;
std::mutex g_mutex;
FILE* g_file = nullptr;
uint64_t g_frames = 0, g_max_frames = 0;
uint64_t g_second_start = 0;
int g_second_frames = 0, g_second_silent = 0;
float g_second_peak = 0;

void WriteHeader(FILE* f, uint64_t frames) {
  const uint32_t data = uint32_t(frames * kSamples * kChannels * 4);
  auto u32 = [&](uint32_t v) { std::fwrite(&v, 4, 1, f); };
  auto u16 = [&](uint16_t v) { std::fwrite(&v, 2, 1, f); };
  std::fseek(f, 0, SEEK_SET);
  std::fwrite("RIFF", 1, 4, f);
  u32(36 + data);
  std::fwrite("WAVEfmt ", 1, 8, f);
  u32(16);
  u16(3);  // IEEE float
  u16(kChannels);
  u32(48000);
  u32(48000 * kChannels * 4);
  u16(kChannels * 4);
  u16(32);
  std::fwrite("data", 1, 4, f);
  u32(data);
}

void HookedSubmit(PPCContext& ctx, uint8_t* base) {
  const uint32_t samples = ctx.r4.u32;
  if (samples) {
    std::lock_guard lock(g_mutex);
    float frame[kSamples * kChannels];
    float peak = 0;
    for (int c = 0; c < kChannels; ++c) {
      for (int s = 0; s < kSamples; ++s) {
        uint32_t bits = _byteswap_ulong(*reinterpret_cast<const uint32_t*>(base + samples + (c * kSamples + s) * 4));
        float v;
        std::memcpy(&v, &bits, 4);
        if (!std::isfinite(v)) v = 0;
        frame[s * kChannels + c] = v;
        peak = std::max(peak, std::fabs(v));
      }
    }
    if (g_file && g_frames < g_max_frames) {
      std::fwrite(frame, sizeof(frame), 1, g_file);
      ++g_frames;
      if (g_frames == g_max_frames) {
        WriteHeader(g_file, g_frames);
        std::fclose(g_file);
        g_file = nullptr;
        REXLOG_INFO("audio capture: done, {} frames", g_frames);
      }
    }
    const uint64_t now = GetTickCount64();
    if (!g_second_start) g_second_start = now;
    ++g_second_frames;
    if (peak < 1e-5f) ++g_second_silent;
    g_second_peak = std::max(g_second_peak, peak);
    if (now - g_second_start >= 1000) {
      REXLOG_INFO("audio: {} frames/s (want 187.5), {} silent, peak {:.3f}", g_second_frames, g_second_silent,
                  g_second_peak);
      g_second_start = now;
      g_second_frames = g_second_silent = 0;
      g_second_peak = 0;
    }
  }
  g_original(ctx, base);
}

}  // namespace

void InstallAudioCapture() {
  const char* env = std::getenv("BF3_AUDIO_CAPTURE");
  if (!env || !*env) return;
  std::string spec = env;
  int seconds = 60;
  // A trailing ":<seconds>" (not the drive colon).
  if (size_t colon = spec.rfind(':'); colon != std::string::npos && colon > 1 &&
                                      spec.find_first_not_of("0123456789", colon + 1) == std::string::npos) {
    seconds = std::atoi(spec.c_str() + colon + 1);
    spec.resize(colon);
  }
  g_max_frames = uint64_t(seconds) * 48000 / kSamples;
  g_file = std::fopen(spec.c_str(), "wb");
  if (!g_file) {
    REXLOG_WARN("audio capture: can't open {}", spec);
    return;
  }
  WriteHeader(g_file, 0);
  CreateThread(
      nullptr, 0,
      [](LPVOID) -> DWORD {
        HMODULE module = nullptr;
        for (int i = 0; i < 6000 && !module; ++i) {
          module = GetModuleHandleW(L"bf3_BF_Main.dll");
          if (!module) Sleep(10);
        }
        g_original = reinterpret_cast<GuestFunc>(PatchImport(module, "rexruntime.dll",
                                                             "__imp__XAudioSubmitRenderDriverFrame",
                                                             reinterpret_cast<void*>(&HookedSubmit)));
        REXLOG_INFO("audio capture: {}", g_original ? "recording" : "no XAudioSubmitRenderDriverFrame import");
        return 0;
      },
      nullptr, 0, nullptr);
}

}  // namespace bf3

#else
namespace bf3 {
void InstallAudioCapture() {}
}  // namespace bf3
#endif
