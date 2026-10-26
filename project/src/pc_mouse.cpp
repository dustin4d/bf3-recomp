// Mouse in BF3's menus (see pc_mouse.h).
//
// The game window is subclassed so its mouse messages also reach the game's Scaleform menus: positions are mapped
// onto the game image (letterboxed in the client area) and queued in bf3_BF_Main.dll (bf3_ui_mouse_event,
// bf_main/ui_mouse.cpp), which hands them to the UI movie on the UI thread. The messages still go on to ReXGlue.
//
// Menu mode is on while a menu screen is on top: the cursor is free and the keyboard drives the menus (pc_keys.cpp);
// mouse buttons only fire and zoom while playing. Right-click (and the mouse's back button) goes back: some PC screens
// have no Back button.
//
// Mouse look (gameplay: a HUD screen on top, mouse_look on) is our own, like a native PC game, and doesn't go through
// ReXGlue's controller emulation (its mnk_mouse is kept off): the window takes Raw Input (WM_INPUT, unaccelerated
// counts), the cursor is hidden and clipped to the window while the game has focus, and bf_main/mouse_aim.cpp turns
// the soldier by the counts each frame through bf3_mouse_look_delta (radians, with mouse_sensitivity, the aim-down-
// sights scale mouse_ads_sensitivity while the Zoom key (pc_keys key_zoom) is held, and mouse_invert_y).

#include "pc_mouse.h"

#if defined(_WIN32)
#include <windows.h>
#include <windowsx.h>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ui/keybinds.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <vector>

#include "pad_inject.h"
#include "pc_keys.h"
#include "pc_settings.h"
#include "ui_state.h"

REXCVAR_DEFINE_BOOL(mouse_look, true, "BF3", "Mouse turns the soldier while playing (raw input, cursor captured)");
REXCVAR_DEFINE_DOUBLE(mouse_sensitivity, 1.0, "BF3", "Mouse look sensitivity (1.0 = 0.046 degrees per count)");
REXCVAR_DEFINE_DOUBLE(mouse_ads_sensitivity, 0.6, "BF3",
                      "Mouse look scale while aiming down sights (the Zoom key held)");
REXCVAR_DEFINE_BOOL(mouse_invert_y, false, "BF3", "Invert mouse look up/down");

namespace bf3::pc_mouse {
namespace {

constexpr double kRadiansPerCount = 0.0008;

// Raw counts gathered while looking (x right, y down), handed out by bf3_mouse_look_delta.
std::mutex g_raw_mutex;
double g_raw_dx = 0.0, g_raw_dy = 0.0;
std::atomic<bool> g_captured{false};
std::atomic<bool> g_feeding{false};  // BF3_MOUSE_FEED test: counts arrive without a captured cursor
std::atomic<uint64_t> g_inject_until{0};  // debug API look counts (InjectLookCounts) count until then

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

void OnRawInput(HRAWINPUT handle) {
  RAWINPUT raw;
  UINT size = sizeof(raw);
  if (GetRawInputData(handle, RID_INPUT, &raw, &size, sizeof(RAWINPUTHEADER)) == UINT(-1)) return;
  if (raw.header.dwType != RIM_TYPEMOUSE || (raw.data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE)) return;
  if (!g_captured.load(std::memory_order_relaxed)) return;
  std::lock_guard lock(g_raw_mutex);
  g_raw_dx += raw.data.mouse.lLastX;
  g_raw_dy += raw.data.mouse.lLastY;
}

LRESULT CALLBACK Proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  if (msg == WM_INPUT) {
    OnRawInput(reinterpret_cast<HRAWINPUT>(lp));
  } else if (msg == WM_MOUSEWHEEL && !g_menu_mode.load(std::memory_order_relaxed)) {
    pc_keys::OnWheel(GET_WHEEL_DELTA_WPARAM(wp) / WHEEL_DELTA);  // weapon switch while playing
  } else if (msg == WM_SETCURSOR && g_captured.load(std::memory_order_relaxed) && LOWORD(lp) == HTCLIENT) {
    SetCursor(nullptr);
    return TRUE;
  }
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

// Raw Input for the mouse, delivered to the game window (re-registered if something else took the registration).
void EnsureRawInput() {
  UINT count = 0;
  GetRegisteredRawInputDevices(nullptr, &count, sizeof(RAWINPUTDEVICE));
  std::vector<RAWINPUTDEVICE> devices(count);
  if (count && GetRegisteredRawInputDevices(devices.data(), &count, sizeof(RAWINPUTDEVICE)) != UINT(-1)) {
    for (const RAWINPUTDEVICE& d : devices) {
      if (d.usUsagePage == 0x01 && d.usUsage == 0x02 && d.hwndTarget == g_window) return;
    }
  }
  RAWINPUTDEVICE mouse{0x01, 0x02, 0, g_window};  // generic desktop, mouse; legacy messages stay on for the menus
  if (RegisterRawInputDevices(&mouse, 1, sizeof(mouse))) {
    REXLOG_INFO("pc mouse: raw input registered");
  } else {
    REXLOG_WARN("pc mouse: RegisterRawInputDevices failed ({})", GetLastError());
  }
}

// Mouse look owns the cursor while playing with the game window in front: hidden and clipped to the client area.
void UpdateCapture(bool look) {
  const bool capture = look && GetForegroundWindow() == g_window && !pc_settings::IsOpen() && !GameInputBlocked();
  if (capture) {
    RECT rc;
    GetClientRect(g_window, &rc);
    POINT tl{rc.left, rc.top}, br{rc.right, rc.bottom};
    ClientToScreen(g_window, &tl);
    ClientToScreen(g_window, &br);
    RECT clip{tl.x, tl.y, br.x, br.y};
    ClipCursor(&clip);  // re-applied every poll: Windows drops the clip on focus changes
  }
  if (capture == g_captured.exchange(capture)) return;
  if (!capture) ClipCursor(nullptr);
  {
    std::lock_guard lock(g_raw_mutex);
    g_raw_dx = g_raw_dy = 0.0;  // nothing gathered before capture or while released reaches the camera
  }
  // WM_SETCURSOR hides it from now on; nudge the window so the change shows without moving the mouse.
  PostMessageW(g_window, WM_SETCURSOR, WPARAM(g_window), MAKELPARAM(HTCLIENT, WM_MOUSEMOVE));
  REXLOG_INFO("pc mouse: cursor {}", capture ? "captured (mouse look)" : "released");
}

void UpdateMenuMode() {
  const std::string top = ui_state::TopScreen();
  const bool menu = top.empty() || !IsGameplay(ui_state::LiveScreens(), top);
  if (menu != g_menu_mode.exchange(menu)) REXLOG_INFO("pc mouse: {} ({})", menu ? "menu mode" : "mouse look", top);
  UpdateCapture(!menu && REXCVAR_GET(mouse_look));
}

// The key(s) bound to Zoom / Aim Down Sights (pc_keys key_zoom), as virtual keys.
bool AimHeld() {
  static std::string cached_bind;
  static std::vector<int> keys;
  const std::string bind = rex::cvar::GetFlagByName("key_zoom");
  if (bind != cached_bind) {
    cached_bind = bind;
    keys.clear();
    for (const std::string& name : pc_keys::SplitKeys(bind)) {
      int vk = name == "Mouse4" ? VK_XBUTTON1 : name == "Mouse5" ? VK_XBUTTON2 : int(rex::ui::ParseVirtualKey(name));
      if (vk) keys.push_back(vk);
    }
  }
  for (int vk : keys) {
    if (GetAsyncKeyState(vk) & 0x8000) return true;
  }
  return false;
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

bool GameFocused() { return g_window && GetForegroundWindow() == g_window; }

bool TakeCounts(double& dx, double& dy) {
  std::lock_guard lock(g_raw_mutex);
  dx = g_raw_dx, dy = g_raw_dy;
  g_raw_dx = g_raw_dy = 0.0;
  return g_captured.load(std::memory_order_relaxed);
}

void InjectLookCounts(double dx, double dy) {
  {
    std::lock_guard lock(g_raw_mutex);
    g_raw_dx += dx;
    g_raw_dy += dy;
  }
  g_inject_until = GetTickCount64() + 500;  // taken by the next frames even without a captured cursor
}

bool InjectMenuEvent(uint32_t type, float x, float y, float scroll) {
  if (!g_event) return false;
  g_event(type, x, y, 0, scroll);
  return true;
}

}  // namespace bf3::pc_mouse

// Read by bf_main/mouse_aim.cpp once per frame: the turn since the last call, in radians (yaw right +, pitch up +),
// false while the mouse isn't looking.
extern "C" __declspec(dllexport) bool bf3_mouse_look_delta(float* yaw, float* pitch) {
  using namespace bf3::pc_mouse;
  double dx, dy;
  {
    std::lock_guard lock(g_raw_mutex);
    dx = g_raw_dx, dy = g_raw_dy;
    g_raw_dx = g_raw_dy = 0.0;
  }
  if (!g_captured.load(std::memory_order_relaxed) && !g_feeding.load(std::memory_order_relaxed) &&
      GetTickCount64() >= g_inject_until.load(std::memory_order_relaxed)) {
    *yaw = *pitch = 0.0f;
    return false;
  }
  double k = kRadiansPerCount * REXCVAR_GET(mouse_sensitivity);
  if (AimHeld()) k *= REXCVAR_GET(mouse_ads_sensitivity);
  *yaw = float(dx * k);
  *pitch = float((REXCVAR_GET(mouse_invert_y) ? dy : -dy) * k);
  return true;
}

namespace bf3::pc_mouse {

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
        // ReXGlue's mouse look (mouse -> emulated right stick, its own capture) stays off: mouse look is ours.
        rex::cvar::SetFlagByName("mnk_mouse", "false");
        REXLOG_INFO("pc mouse: menu mouse on window {}", static_cast<void*>(g_window));
        // BF3_MOUSE_LOOK_TEST="<dx>,<dy>,<steps>[;...]": 4 s after mouse look captures the cursor, sends <steps>
        // relative moves of (dx, dy) counts, 10 ms apart, per group, 3 s between groups (SendInput: real raw input).
        if (const char* test = std::getenv("BF3_MOUSE_LOOK_TEST"); test && *test) {
          CreateThread(
              nullptr, 0,
              [](LPVOID p) -> DWORD {
                while (!g_captured) Sleep(100);
                Sleep(4000);
                std::string all = static_cast<const char*>(p);
                for (size_t start = 0; start < all.size();) {
                  size_t end = all.find(';', start);
                  const std::string group = all.substr(start, end == std::string::npos ? std::string::npos : end - start);
                  start = end == std::string::npos ? all.size() : end + 1;
                  int dx = 0, dy = 0, steps = 0;
                  std::sscanf(group.c_str(), "%d,%d,%d", &dx, &dy, &steps);
                  REXLOG_INFO("pc mouse: look test {} x ({}, {})", steps, dx, dy);
                  for (int i = 0; i < steps; ++i) {
                    INPUT in{};
                    in.type = INPUT_MOUSE;
                    in.mi.dx = dx;
                    in.mi.dy = dy;
                    in.mi.dwFlags = MOUSEEVENTF_MOVE;
                    SendInput(1, &in, sizeof(in));
                    Sleep(10);
                  }
                  Sleep(3000);
                }
                return 0;
              },
              const_cast<char*>(test), 0, nullptr);
        }
        // BF3_MOUSE_FEED="<x counts/s>,<y counts/s>,<ms>[;...]": 6 s into mouse look (no focus or capture needed), adds
        // raw counts at a steady rate straight into the look accumulator, 2 s between groups (smoothness tests).
        if (const char* feed = std::getenv("BF3_MOUSE_FEED"); feed && *feed) {
          CreateThread(
              nullptr, 0,
              [](LPVOID p) -> DWORD {
                while (g_menu_mode) Sleep(100);
                Sleep(6000);
                std::string all = static_cast<const char*>(p);
                for (size_t start = 0; start < all.size();) {
                  size_t end = all.find(';', start);
                  const std::string group = all.substr(start, end == std::string::npos ? std::string::npos : end - start);
                  start = end == std::string::npos ? all.size() : end + 1;
                  double rx = 0, ry = 0;
                  int ms = 0;
                  std::sscanf(group.c_str(), "%lf,%lf,%d", &rx, &ry, &ms);
                  REXLOG_INFO("pc mouse: feed ({}, {}) counts/s for {} ms", rx, ry, ms);
                  g_feeding = true;
                  const ULONGLONG t0 = GetTickCount64();
                  LARGE_INTEGER freq, last, now;
                  QueryPerformanceFrequency(&freq);
                  QueryPerformanceCounter(&last);
                  while (GetTickCount64() - t0 < ULONGLONG(ms)) {
                    Sleep(1);
                    QueryPerformanceCounter(&now);
                    const double dt = double(now.QuadPart - last.QuadPart) / double(freq.QuadPart);
                    last = now;
                    std::lock_guard lock(g_raw_mutex);
                    g_raw_dx += rx * dt;
                    g_raw_dy += ry * dt;
                  }
                  Sleep(2000);
                  g_feeding = false;
                }
                return 0;
              },
              const_cast<char*>(feed), 0, nullptr);
        }
        if (const char* script = std::getenv("BF3_MOUSE_SCRIPT"); script && *script) {
          CreateThread(
              nullptr, 0,
              [](LPVOID p) -> DWORD {
                RunScript(static_cast<const char*>(p));
                return 0;
              },
              const_cast<char*>(script), 0, nullptr);
        }
        for (int i = 0;; ++i) {
          if (i % 20 == 0) EnsureRawInput();  // RegisterRawInputDevices needs no particular thread
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
bool GameFocused() { return false; }
bool TakeCounts(double&, double&) { return false; }
void InjectLookCounts(double, double) {}
bool InjectMenuEvent(uint32_t, float, float, float) { return false; }
}  // namespace bf3::pc_mouse
#endif
