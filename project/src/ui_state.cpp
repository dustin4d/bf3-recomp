#include "ui_state.h"

#if defined(_WIN32)
#include <windows.h>
#endif

namespace bf3::ui_state {

std::string TopScreen() {
#if defined(_WIN32)
  using Fn = int (*)(char*, int);
  static Fn fn = nullptr;
  if (!fn) {
    HMODULE module = GetModuleHandleW(L"bf3_BF_Main.dll");
    if (!module) return {};
    fn = reinterpret_cast<Fn>(GetProcAddress(module, "bf3_ui_top_screen"));
    if (!fn) return {};
  }
  char buf[256];
  int n = fn(buf, sizeof(buf));
  return std::string(buf, n > 0 ? n : 0);
#else
  return {};
#endif
}

int ScreenCount(const std::string& name) {
#if defined(_WIN32)
  using Fn = int (*)(const char*);
  static Fn fn = nullptr;
  if (!fn) {
    HMODULE module = GetModuleHandleW(L"bf3_BF_Main.dll");
    if (!module) return 0;
    fn = reinterpret_cast<Fn>(GetProcAddress(module, "bf3_ui_screen_count"));
    if (!fn) return 0;
  }
  return fn(name.c_str());
#else
  return 0;
#endif
}

}  // namespace bf3::ui_state
