// Mouse in BF3's menus (see pc_mouse.h).
//
// The game window is subclassed so its mouse messages also reach the game's Scaleform menus: positions are mapped
// onto the game image (letterboxed in the client area) and queued in bf3_BF_Main.dll (bf3_ui_mouse_event,
// bf_main/ui_mouse.cpp), which hands them to the UI movie on the UI thread. The messages still go on to ReXGlue.
//
// Menu mode is on while a menu screen is on top: ReXGlue's mouse look is switched off (the cursor is free and the
// mouse doesn't drive the right stick), and the triggers the mouse buttons are bound to are masked from the game
// (pad_inject.cpp), so a click is only a click. Right-click (and the mouse's back button) goes back: some PC screens
// have no Back button. In gameplay (a HUD screen on top) mouse look comes back.

#include "pc_mouse.h"

#if defined(_WIN32)
#include <windows.h>
#include <windowsx.h>

#include <rex/input/mnk/mnk_input_driver.h>
#include <rex/logging.h>

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <string>

#include "pad_inject.h"
#include "pc_settings.h"
#include "ui_state.h"

namespace bf3::pc_mouse {
namespace {

using EventFn = void (*)(uint32_t type, float nx, float ny, uint32_t button, float scroll);

constexpr float kGameAspect = 1280.0f / 720.0f;
constexpr uint16_t kButtonB = 0x2000;  // X_INPUT_GAMEPAD_B

HWND g_window = nullptr;
WNDPROC g_original_proc = nullptr;
EventFn g_event = nullptr;
std::atomic<bool> g_menu_mode{true};

// Gameplay (the mouse aims) when the live screen stack has the HUD and nothing menu-like on it; the stack keeps
// hidden overlays (TabListenerScreen, ChatLogScreen, the Tab scoreboard, CrosshairDefault, DefaultTrackingScreen), so
// the most recently pushed screen alone can't tell (the spawn screen ends with TabListenerScreen, gameplay with
// DefaultTrackingScreen). Falls back to the top screen when the stack can't be read.
bool IsGameplay(const std::string& live, const std::string& top) {
  if (live.empty()) {
    return top.find("Hud") != std::string::npos || top == "CrosshairDefault" || top == "DefaultTrackingScreen";
  }
  if (live.find("HudScreen") == std::string::npos) return false;
  for (const char* menu : {"Menu", "Spawn", "Popup", "Options", "BackScreen", "Customiz", "EndOfRound", "Loading"}) {
    if (live.find(menu) != std::string::npos) return false;
  }
  return true;
}

// Client point -> 0..1 across the game image (the 16:9 image is letterboxed/pillarboxed in the client area).
bool Normalize(HWND hwnd, int x, int y, float& nx, float& ny) {
  RECT rc;
  if (!GetClientRect(hwnd, &rc)) return false;
  const float cw = float(rc.right - rc.left), ch = float(rc.bottom - rc.top);
  if (cw <= 0 || ch <= 0) return false;
  float iw = cw, ih = ch;
  if (cw / ch > kGameAspect) {
    iw = ch * kGameAspect;
  } else {
    ih = cw / kGameAspect;
  }
  nx = std::clamp((x - (cw - iw) / 2) / iw, 0.0f, 1.0f);
  ny = std::clamp((y - (ch - ih) / 2) / ih, 0.0f, 1.0f);
  return true;
}

bool Forwarding() { return g_event && g_menu_mode && !pc_settings::IsOpen() && !GameInputBlocked(); }

LRESULT CALLBACK Proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  if (Forwarding()) {
    float nx, ny;
    switch (msg) {
      case WM_MOUSEMOVE:
        if (Normalize(hwnd, GET_X_LPARAM(lp), GET_Y_LPARAM(lp), nx, ny)) g_event(1, nx, ny, 0, 0);
        break;
      case WM_LBUTTONDOWN:
      case WM_RBUTTONDOWN:
      case WM_MBUTTONDOWN:
      case WM_LBUTTONUP:
      case WM_RBUTTONUP:
      case WM_MBUTTONUP: {
        const bool down = msg == WM_LBUTTONDOWN || msg == WM_RBUTTONDOWN || msg == WM_MBUTTONDOWN;
        const uint32_t button = (msg == WM_LBUTTONDOWN || msg == WM_LBUTTONUP)   ? 0
                                : (msg == WM_RBUTTONDOWN || msg == WM_RBUTTONUP) ? 1
                                                                                 : 2;
        if (Normalize(hwnd, GET_X_LPARAM(lp), GET_Y_LPARAM(lp), nx, ny)) {
          g_event(1, nx, ny, 0, 0);
          g_event(down ? 2 : 3, nx, ny, button, 0);
        }
        // Right-click goes back, as in PC menus (several PC screens have no Back button).
        if (msg == WM_RBUTTONUP) PulseButtons(kButtonB, 120);
        break;
      }
      case WM_XBUTTONUP:
        if (GET_XBUTTON_WPARAM(wp) == XBUTTON1) PulseButtons(kButtonB, 120);  // mouse "back" button
        break;
      case WM_MOUSEWHEEL: {
        POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};  // screen coordinates
        ScreenToClient(hwnd, &pt);
        if (Normalize(hwnd, pt.x, pt.y, nx, ny)) {
          // Scaleform scrolls by lines; one notch = 3 lines, as on Windows.
          g_event(4, nx, ny, 0, 3.0f * GET_WHEEL_DELTA_WPARAM(wp) / WHEEL_DELTA);
        }
        break;
      }
      default:
        break;
    }
  }
  return CallWindowProcW(g_original_proc, hwnd, msg, wp, lp);
}

BOOL CALLBACK FindWindowProc(HWND hwnd, LPARAM out) {
  DWORD pid = 0;
  GetWindowThreadProcessId(hwnd, &pid);
  if (pid != GetCurrentProcessId() || !IsWindowVisible(hwnd) || GetWindow(hwnd, GW_OWNER)) return TRUE;
  RECT rc;
  GetClientRect(hwnd, &rc);
  if (rc.right - rc.left < 320) return TRUE;
  *reinterpret_cast<HWND*>(out) = hwnd;
  return FALSE;
}

void UpdateMenuMode() {
  const std::string top = ui_state::TopScreen();
  const bool menu = top.empty() || !IsGameplay(ui_state::LiveScreens(), top);
  if (menu != g_menu_mode.exchange(menu)) REXLOG_INFO("pc mouse: {} ({})", menu ? "menu mode" : "mouse look", top);
  rex::input::mnk::SetMouseLookActive(!menu);
  SetMenuTriggerMask(menu);
}

// Test input (BF3_MOUSE_SCRIPT): steps separated by ';': "wait:<Screen>", "<delay ms>:move:<x>,<y>" (0..1 across the
// game image), "<delay>:click", "<delay>:down", "<delay>:up", "<delay>:wheel:<lines>". Goes through the same queue as
// the window's messages.
void RunScript(const char* script) {
  std::string all = script;
  float x = 0.5f, y = 0.5f;
  size_t start = 0;
  while (start < all.size()) {
    size_t end = all.find(';', start);
    std::string step = all.substr(start, end == std::string::npos ? std::string::npos : end - start);
    start = end == std::string::npos ? all.size() : end + 1;
    if (step.rfind("wait:", 0) == 0) {
      const std::string screen = step.substr(5);
      for (int i = 0; i < 1200 && ui_state::TopScreen() != screen; ++i) Sleep(100);
      continue;
    }
    const size_t c1 = step.find(':');
    Sleep(DWORD(std::atoi(step.substr(0, c1).c_str())));
    const std::string action = c1 == std::string::npos ? "" : step.substr(c1 + 1);
    if (action.rfind("move:", 0) == 0) {
      const size_t comma = action.find(',');
      x = float(std::atof(action.substr(5, comma - 5).c_str()));
      y = float(std::atof(action.substr(comma + 1).c_str()));
      g_event(1, x, y, 0, 0);
    } else if (action == "click") {
      g_event(2, x, y, 0, 0);
      Sleep(80);
      g_event(3, x, y, 0, 0);
    } else if (action == "rclick") {
      PulseButtons(kButtonB, 120);
    } else if (action == "down" || action == "up") {
      g_event(action == "down" ? 2 : 3, x, y, 0, 0);
    } else if (action.rfind("wheel:", 0) == 0) {
      g_event(4, x, y, 0, float(std::atof(action.substr(6).c_str())));
    }
    REXLOG_INFO("pc mouse: script {}", step);
  }
}

}  // namespace

bool MenuMode() { return g_menu_mode; }

void Install() {
  if (const char* env = std::getenv("BF3_MENU_MOUSE"); env && *env == '0') return;
  CreateThread(
      nullptr, 0,
      [](LPVOID) -> DWORD {
        // The window and BF.Main appear a few seconds after start.
        while (!g_window || !g_event) {
          Sleep(100);
          if (!g_window) EnumWindows(FindWindowProc, reinterpret_cast<LPARAM>(&g_window));
          if (!g_event) {
            if (HMODULE module = GetModuleHandleW(L"bf3_BF_Main.dll")) {
              g_event = reinterpret_cast<EventFn>(GetProcAddress(module, "bf3_ui_mouse_event"));
            }
          }
        }
        g_original_proc = reinterpret_cast<WNDPROC>(
            SetWindowLongPtrW(g_window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&Proc)));
        REXLOG_INFO("pc mouse: menu mouse on window {}", static_cast<void*>(g_window));
        if (const char* script = std::getenv("BF3_MOUSE_SCRIPT"); script && *script) {
          CreateThread(
              nullptr, 0,
              [](LPVOID p) -> DWORD {
                RunScript(static_cast<const char*>(p));
                return 0;
              },
              const_cast<char*>(script), 0, nullptr);
        }
        for (;;) {
          UpdateMenuMode();
          Sleep(50);
        }
      },
      nullptr, 0, nullptr);
}

}  // namespace bf3::pc_mouse

#else
namespace bf3::pc_mouse {
void Install() {}
bool MenuMode() { return false; }
}  // namespace bf3::pc_mouse
#endif
