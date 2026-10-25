// Scripted controller input for automated tests, independent of window focus.
//
// BF3_PAD_SCRIPT="<ms>:<buttons>[:<hold ms>];..." presses buttons on controller 0 at <ms> after the first input poll
// (hold default 150 ms). Buttons: A B X Y START BACK LB RB LS RS UP DOWN LEFT RIGHT LT RT, joined with '+'.
// Example: "25000:START;28000:A;30000:DOWN;30500:DOWN;31000:A".
// Works by redirecting bf3_BF_Main.dll's import of __imp__XamInputGetState (rexruntime.dll): the original runs first,
// then the scripted buttons are OR'd into the guest XINPUT_STATE. If no real controller/keyboard device is connected,
// the state is synthesized (success, zeroed sticks). Injected keystrokes (keybd_event) don't work for this: ReXGlue's
// keyboard emulation only reads keys while its window has focus.

#include "pad_inject.h"

#if defined(_WIN32)
#include <windows.h>

#include <rex/logging.h>
#include <rex/ppc/context.h>

#include <atomic>
#include <cstdlib>
#include <string>
#include <vector>

#include "iat_patch.h"

namespace bf3 {
namespace {

using GuestFunc = void (*)(PPCContext&, uint8_t*);

struct Step {
  uint32_t at_ms, hold_ms;
  uint16_t buttons;
  uint8_t lt, rt;
};

GuestFunc g_original = nullptr;
std::vector<Step> g_steps;
std::atomic<uint64_t> g_start_tick{0};
std::atomic<uint32_t> g_packet{0x40000000};

bool ParseButton(const std::string& name, Step& step) {
  static const struct {
    const char* name;
    uint16_t bit;
  } kButtons[] = {{"UP", 0x0001},    {"DOWN", 0x0002}, {"LEFT", 0x0004}, {"RIGHT", 0x0008}, {"START", 0x0010},
                  {"BACK", 0x0020},  {"LS", 0x0040},   {"RS", 0x0080},   {"LB", 0x0100},    {"RB", 0x0200},
                  {"GUIDE", 0x0400}, {"A", 0x1000},    {"B", 0x2000},    {"X", 0x4000},     {"Y", 0x8000}};
  for (auto& b : kButtons) {
    if (name == b.name) {
      step.buttons |= b.bit;
      return true;
    }
  }
  if (name == "LT") return step.lt = 0xFF, true;
  if (name == "RT") return step.rt = 0xFF, true;
  return false;
}

void ParseScript(const std::string& script) {
  size_t pos = 0;
  while (pos < script.size()) {
    size_t end = script.find(';', pos);
    std::string item = script.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
    pos = end == std::string::npos ? script.size() : end + 1;
    if (item.empty()) continue;
    size_t c1 = item.find(':'), c2 = item.find(':', c1 + 1);
    if (c1 == std::string::npos) continue;
    Step step{static_cast<uint32_t>(std::stoul(item.substr(0, c1))), 150, 0, 0, 0};
    std::string buttons = item.substr(c1 + 1, c2 == std::string::npos ? std::string::npos : c2 - c1 - 1);
    if (c2 != std::string::npos) step.hold_ms = static_cast<uint32_t>(std::stoul(item.substr(c2 + 1)));
    size_t b = 0;
    while (b <= buttons.size()) {
      size_t plus = buttons.find('+', b);
      std::string name = buttons.substr(b, plus == std::string::npos ? std::string::npos : plus - b);
      if (!ParseButton(name, step)) REXLOG_WARN("pad inject: unknown button '{}'", name);
      if (plus == std::string::npos) break;
      b = plus + 1;
    }
    g_steps.push_back(step);
  }
}

// __imp__XamInputGetState(user_index r3, flags r4, X_INPUT_STATE* r5): packet_number u32, buttons u16, lt u8, rt u8,
// thumbs 4 x s16, all big-endian.
void HookedXamInputGetState(PPCContext& ctx, uint8_t* base) {
  const uint32_t user = ctx.r3.u32;
  const uint32_t state = ctx.r5.u32;
  g_original(ctx, base);
  if (user != 0 || !state) return;
  uint64_t now = GetTickCount64();
  uint64_t start = g_start_tick.load();
  if (!start) {
    g_start_tick.compare_exchange_strong(start, now);
    start = g_start_tick.load();
  }
  uint32_t t = static_cast<uint32_t>(now - start);
  uint16_t buttons = 0;
  uint8_t lt = 0, rt = 0;
  for (const Step& s : g_steps) {
    if (t >= s.at_ms && t < s.at_ms + s.hold_ms) {
      buttons |= s.buttons;
      lt |= s.lt;
      rt |= s.rt;
    }
  }
  uint8_t* p = base + state;
  if (ctx.r3.u32 != 0) {  // no device: synthesize a connected, idle controller
    std::memset(p, 0, 16);
    ctx.r3.u64 = 0;
  }
  if (!buttons && !lt && !rt) return;
  uint32_t packet = g_packet.fetch_add(1);
  p[0] = uint8_t(packet >> 24), p[1] = uint8_t(packet >> 16), p[2] = uint8_t(packet >> 8), p[3] = uint8_t(packet);
  uint16_t cur = uint16_t(p[4] << 8 | p[5]) | buttons;
  p[4] = uint8_t(cur >> 8), p[5] = uint8_t(cur);
  p[6] |= lt;
  p[7] |= rt;
}

}  // namespace

void InstallPadInject() {
  const char* script = std::getenv("BF3_PAD_SCRIPT");
  if (!script || !*script) return;
  ParseScript(script);
  // bf3_BF_Main.dll is loaded when the launcher loads BF.Main, after setup: wait for it, then patch its import.
  CreateThread(
      nullptr, 0,
      [](LPVOID) -> DWORD {
        HMODULE module = nullptr;
        for (int i = 0; i < 6000 && !module; ++i) {
          module = GetModuleHandleW(L"bf3_BF_Main.dll");
          if (!module) Sleep(10);
        }
        void* original = PatchImport(module, "rexruntime.dll", "__imp__XamInputGetState",
                                     reinterpret_cast<void*>(&HookedXamInputGetState));
        if (!original) {
          REXLOG_WARN("pad inject: no __imp__XamInputGetState import in bf3_BF_Main.dll ({}), script ignored",
                      static_cast<void*>(module));
          return 0;
        }
        g_original = reinterpret_cast<GuestFunc>(original);
        REXLOG_INFO("pad inject: hooked XamInputGetState, {} scripted steps", g_steps.size());
        return 0;
      },
      nullptr, 0, nullptr);
}

}  // namespace bf3

#else
namespace bf3 {
void InstallPadInject() {}
}  // namespace bf3
#endif
