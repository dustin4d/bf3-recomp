// Debug API server (development builds only: BF3_DEV_TOOLS). Never part of a production build: CMakeLists.txt leaves
// src/dev/ and bf_main/dev_state.cpp out when BF3_DEV_TOOLS is off.
//
// Protocol: TCP on 127.0.0.1:4790 (BF3_DEBUG_API=<port>; 0 = off). Each request is one line of words, e.g.
// "status" or "key W 2000"; each reply is one line of JSON, {"ok": true, ...} or {"ok": false, "error": "..."}.
// A reply with "bytes": N is followed by N raw bytes (shot). WSL reaches the port directly (mirrored networking).
// Client: scripts/bf3dbg.py. "help" lists the commands.
//
// Sources: game state comes from bf3_BF_Main.dll (bf_main/dev_state.cpp, exports in dev_shared.h) and the UI screen
// stack (ui_state.cpp); input goes in through the same paths as the PC keyboard and mouse (pc_keys.cpp, pc_mouse.cpp)
// and the controller hook (pad_inject.cpp), none of which need the game window in front.
//
// Adding a command: one Register() call in RegisterCommands() with its usage line, help text and handler.

#include "debug_api.h"

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <rex/cvar.h>
#include <rex/logging.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "dev_shared.h"
#include "../pad_inject.h"
#include "../pc_keys.h"
#include "../pc_mouse.h"
#include "../ui_state.h"

namespace bf3::dev {
namespace {

constexpr int kDefaultPort = 4790;

// ---- JSON output -----------------------------------------------------------------------------------------------------

std::string Quote(std::string_view s) {
  std::string out = "\"";
  for (unsigned char c : s) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (c < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof(buf), "\\u%04x", c);
          out += buf;
        } else {
          out += char(c);
        }
    }
  }
  return out + "\"";
}

std::string Num(double v) {
  if (!std::isfinite(v)) return "null";
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%.6g", v);
  return buf;
}

std::string Hex(uint32_t v) {
  char buf[16];
  std::snprintf(buf, sizeof(buf), "\"0x%08X\"", v);
  return buf;
}

// A JSON object, members in insertion order.
class Obj {
 public:
  Obj& Raw(std::string_view key, std::string_view json) {
    if (!body_.empty()) body_ += ',';
    body_ += Quote(key);
    body_ += ':';
    body_ += json;
    return *this;
  }
  Obj& Str(std::string_view key, std::string_view v) { return Raw(key, Quote(v)); }
  Obj& Number(std::string_view key, double v) { return Raw(key, Num(v)); }
  Obj& Int(std::string_view key, int64_t v) { return Raw(key, std::to_string(v)); }
  Obj& Bool(std::string_view key, bool v) { return Raw(key, v ? "true" : "false"); }
  Obj& Address(std::string_view key, uint32_t v) { return Raw(key, Hex(v)); }
  Obj& Vec(std::string_view key, const float* v, int n) {
    std::string a = "[";
    for (int i = 0; i < n; ++i) a += (i ? "," : "") + Num(v[i]);
    return Raw(key, a + "]");
  }
  Obj& Strings(std::string_view key, const std::vector<std::string>& v) {
    std::string a = "[";
    for (size_t i = 0; i < v.size(); ++i) a += (i ? "," : "") + Quote(v[i]);
    return Raw(key, a + "]");
  }
  Obj& Child(std::string_view key, const Obj& o) { return Raw(key, o.Json()); }
  std::string Json() const { return "{" + body_ + "}"; }

 private:
  std::string body_;
};

// ---- requests and replies ----------------------------------------------------------------------------------------

struct Reply {
  Obj obj;
  bool ok = true;
  std::string error;
  std::string binary;  // sent after the JSON line ("bytes" member)
};

Reply Fail(std::string error) {
  Reply r;
  r.ok = false;
  r.error = std::move(error);
  return r;
}

using Args = std::vector<std::string>;  // the request's words, [0] = command

struct Command {
  std::string usage, help;
  std::function<Reply(const Args&)> run;
  bool quiet = false;  // not logged (polled queries)
};

std::map<std::string, Command>& Commands() {
  static std::map<std::string, Command> commands;
  return commands;
}

void Register(const std::string& name, std::string usage, std::string help, std::function<Reply(const Args&)> run,
              bool quiet = false) {
  Commands()[name] = Command{std::move(usage), std::move(help), std::move(run), quiet};
}

bool ParseInt(const std::string& s, long long& out, int radix = 10) {
  char* end = nullptr;
  out = std::strtoll(s.c_str(), &end, radix);
  return !s.empty() && end && *end == 0;
}

bool ParseDouble(const std::string& s, double& out) {
  char* end = nullptr;
  out = std::strtod(s.c_str(), &end);
  return !s.empty() && end && *end == 0 && std::isfinite(out);
}

// ---- bf3_BF_Main.dll side ----------------------------------------------------------------------------------------

template <typename Fn>
Fn DllExport(const char* name) {
  HMODULE module = GetModuleHandleW(L"bf3_BF_Main.dll");
  return module ? reinterpret_cast<Fn>(GetProcAddress(module, name)) : nullptr;
}

bool GetSnapshot(Snapshot& s) {
  static auto fn = DllExport<bool (*)(Snapshot*, uint32_t)>("bf3_dev_snapshot");
  if (!fn) fn = DllExport<bool (*)(Snapshot*, uint32_t)>("bf3_dev_snapshot");
  return fn && fn(&s, sizeof(s));
}

std::vector<float> FrameTimes() {
  static auto fn = DllExport<int (*)(float*, int)>("bf3_dev_frame_times");
  if (!fn) fn = DllExport<int (*)(float*, int)>("bf3_dev_frame_times");
  std::vector<float> ms(600);
  ms.resize(fn ? size_t(fn(ms.data(), int(ms.size()))) : 0);
  return ms;
}

Reply GameRequest(const std::string& line) {
  static auto fn = DllExport<bool (*)(const char*, char*, int)>("bf3_dev_request");
  if (!fn) fn = DllExport<bool (*)(const char*, char*, int)>("bf3_dev_request");
  if (!fn) return Fail("BF.Main is not loaded yet");
  char text[512] = {};
  if (!fn(line.c_str(), text, sizeof(text))) return Fail(text);
  Reply r;
  r.obj.Str("result", text);
  return r;
}

// ---- derived state -----------------------------------------------------------------------------------------------

std::vector<std::string> SplitScreens(const std::string& list) {
  std::vector<std::string> out;
  std::stringstream ss(list);
  for (std::string s; std::getline(ss, s, ';');) {
    if (!s.empty()) out.push_back(s);
  }
  return out;
}

// Where the game is, for tests to wait on: boot, main_menu, menu (other front end screens), loading, spawn_screen,
// playing (a local soldier with the HUD up), ingame_menu (a menu over the level).
std::string Phase(const Snapshot* s, const std::string& top, const std::string& live) {
  if (!s || live.empty()) return "boot";
  if (live.find("Loading") != std::string::npos) return "loading";
  if (live.find("SpawnScreen") != std::string::npos) return "spawn_screen";
  const bool hud = live.find("HudScreen") != std::string::npos;
  if (hud && s->soldier) {
    for (const char* menu : {"Menu", "Popup", "Options", "BackScreen", "EndOfRound"}) {
      if (live.find(menu) != std::string::npos) return "ingame_menu";
    }
    return "playing";
  }
  if (top == "MainMenuScreen") return "main_menu";
  return hud ? "ingame_menu" : "menu";
}

// Yaw/pitch of a transform's forward row (+0x20) with sign `dir`: the render camera looks down -forward
// (yaw = atan2(-f.x, -f.z)), the soldier faces +forward. Same convention for both: camera yaw = -aim_yaw (mod 2pi),
// camera pitch = aim_pitch (up +).
void ViewAngles(const float* m, float dir, float& yaw, float& pitch) {
  const float fx = dir * m[8], fy = dir * m[9], fz = dir * m[10];
  yaw = std::atan2(fx, fz);
  pitch = std::asin(std::clamp(fy, -1.0f, 1.0f));
}

const char* TeamName(uint32_t team) { return team == 1 ? "US" : team == 2 ? "RU" : team == 0 ? "neutral" : "?"; }

Obj Perf(double window_s) {
  std::vector<float> ms = FrameTimes();
  Obj o;
  double total = 0.0;
  size_t n = 0;
  for (auto it = ms.rbegin(); it != ms.rend() && total < window_s * 1000.0; ++it, ++n) total += *it;
  if (!n) return o.Int("frames", 0);
  std::vector<float> recent(ms.end() - n, ms.end());
  std::sort(recent.begin(), recent.end());
  auto pct = [&](double p) { return recent[std::min(recent.size() - 1, size_t(p * double(recent.size())))]; };
  o.Int("frames", int64_t(n))
      .Number("window_s", total / 1000.0)
      .Number("fps", total > 0 ? 1000.0 * double(n) / total : 0.0)
      .Number("avg_ms", total / double(n))
      .Number("p50_ms", pct(0.50))
      .Number("p95_ms", pct(0.95))
      .Number("p99_ms", pct(0.99))
      .Number("max_ms", recent.back());
  return o;
}

Reply Status() {
  Snapshot s{};
  const bool have = GetSnapshot(s);
  const std::string top = ui_state::TopScreen(), live = ui_state::LiveScreens();
  Reply r;
  r.obj.Str("phase", Phase(have ? &s : nullptr, top, live));
  r.obj.Child("ui", Obj().Str("top", top).Strings("screens", SplitScreens(live)));
  r.obj.Child("input", Obj().Bool("focused", pc_mouse::GameFocused()).Bool("menu_mode", pc_mouse::MenuMode()));
  if (!have) return r;
  r.obj.Int("frame", int64_t(s.frame)).Int("ticks", int64_t(s.ticks)).Number("uptime_s", s.uptime_s);
  r.obj.Child("level", Obj().Str("name", s.level)
                           .Bool("loaded", s.level_loaded)
                           .Bool("unloading", s.level_unloading)
                           .Number("ingame_s", s.ingame_s));
  Obj player;
  player.Address("soldier", s.soldier).Str("team", TeamName(s.team)).Address("server_player", s.server_player);
  if (s.soldier) {
    float yaw, pitch;
    ViewAngles(s.transform, 1.0f, yaw, pitch);
    const float speed = std::sqrt(s.velocity[0] * s.velocity[0] + s.velocity[1] * s.velocity[1] +
                                  s.velocity[2] * s.velocity[2]);
    player.Bool("on_foot", s.on_foot)
        .Vec("pos", s.transform + 12, 3)
        .Number("heading", yaw)
        .Number("aim_yaw", s.aim_yaw)
        .Number("aim_pitch", s.aim_pitch)
        .Vec("velocity", s.velocity, 3)
        .Number("speed", speed)
        .Number("transform_age_s", s.transform_age_s);
  }
  r.obj.Child("player", player);
  float cyaw, cpitch;
  ViewAngles(s.camera, -1.0f, cyaw, cpitch);
  r.obj.Child("camera", Obj().Vec("pos", s.camera + 12, 3).Number("yaw", cyaw).Number("pitch", cpitch).Int(
                            "calls_per_frame", s.camera_calls));
  r.obj.Child("perf", Perf(1.0));
  return r;
}

// ---- screenshot ------------------------------------------------------------------------------------------------

BOOL CALLBACK FindGameWindow(HWND hwnd, LPARAM out) {
  DWORD pid = 0;
  GetWindowThreadProcessId(hwnd, &pid);
  if (pid != GetCurrentProcessId() || !IsWindowVisible(hwnd)) return TRUE;
  RECT rc;
  if (!GetClientRect(hwnd, &rc) || rc.right - rc.left < 64) return TRUE;
  auto* best = reinterpret_cast<HWND*>(out);
  RECT best_rc{};
  if (*best) GetClientRect(*best, &best_rc);
  if (!*best || (rc.right - rc.left) * (rc.bottom - rc.top) > (best_rc.right - best_rc.left) * (best_rc.bottom - best_rc.top)) {
    *best = hwnd;
  }
  return TRUE;
}

// The game window's client area (PrintWindow: works while covered or unfocused), as top-down BGRA rows.
Reply Shot() {
  HWND hwnd = nullptr;
  EnumWindows(FindGameWindow, reinterpret_cast<LPARAM>(&hwnd));
  if (!hwnd) return Fail("no game window");
  RECT rc;
  GetClientRect(hwnd, &rc);
  const int w = rc.right - rc.left, h = rc.bottom - rc.top;
  HDC screen = GetDC(nullptr);
  HDC dc = CreateCompatibleDC(screen);
  BITMAPINFO bi{};
  bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
  bi.bmiHeader.biWidth = w;
  bi.bmiHeader.biHeight = -h;  // top-down
  bi.bmiHeader.biPlanes = 1;
  bi.bmiHeader.biBitCount = 32;
  bi.bmiHeader.biCompression = BI_RGB;
  void* bits = nullptr;
  HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
  Reply r;
  if (!bmp || !bits) {
    r = Fail("CreateDIBSection failed");
  } else {
    HGDIOBJ old = SelectObject(dc, bmp);
    const bool printed = PrintWindow(hwnd, dc, 3);  // PW_CLIENTONLY | PW_RENDERFULLCONTENT
    GdiFlush();
    if (printed) {
      r.binary.assign(static_cast<const char*>(bits), size_t(w) * h * 4);
      r.obj.Int("width", w).Int("height", h).Str("format", "bgra");
    } else {
      r = Fail("PrintWindow failed");
    }
    SelectObject(dc, old);
    DeleteObject(bmp);
  }
  DeleteDC(dc);
  ReleaseDC(nullptr, screen);
  return r;
}

// ---- commands ----------------------------------------------------------------------------------------------------

std::string Rest(const Args& a, size_t from) {
  std::string s;
  for (size_t i = from; i < a.size(); ++i) s += (i > from ? " " : "") + a[i];
  return s;
}

void RegisterCommands() {
  Register("help", "help", "lists the commands", [](const Args&) {
    Reply r;
    Obj list;
    for (const auto& [name, c] : Commands()) list.Str(c.usage, c.help);
    r.obj.Child("commands", list);
    return r;
  }, true);
  Register("ping", "ping", "liveness check", [](const Args&) {
    Reply r;
    r.obj.Bool("pong", true).Int("snapshot_version", kSnapshotVersion).Int("pid", GetCurrentProcessId());
    return r;
  }, true);
  Register("status", "status", "phase, UI screens, level, player, camera, frame rate (last second)",
           [](const Args&) { return Status(); }, true);
  Register("perf", "perf [seconds=5]", "frame time statistics over the last frames (up to 600)", [](const Args& a) {
    double window = 5.0;
    if (a.size() > 1 && !ParseDouble(a[1], window)) return Fail("seconds must be a number");
    Reply r;
    r.obj.Child("perf", Perf(window));
    return r;
  }, true);
  Register("frames", "frames", "the recent frame times in ms, oldest first (up to 600)", [](const Args&) {
    std::vector<float> ms = FrameTimes();
    Reply r;
    r.obj.Vec("ms", ms.data(), int(ms.size()));
    return r;
  }, true);

  // Game commands (bf_main/dev_state.cpp, run on the main thread at the next frame).
  for (const char* game : {"level", "team", "deploy", "look", "turn"}) {
    static const std::map<std::string, std::pair<const char*, const char*>> kHelp = {
        {"level", {"level [<level>[:<category>=<value>]]", "switch the local server's level (default Metro TDM)"}},
        {"team", {"team <us|ru>", "team for the next join; moves the player now if already in the level"}},
        {"deploy", {"deploy [kit 0-3]", "deploy from the spawn screen (0 assault, 1 engineer, 2 support, 3 recon)"}},
        {"look", {"look <yaw> <pitch>", "set the soldier's aim (aimer radians, as status player.aim_yaw/aim_pitch)"}},
        {"turn", {"turn <dyaw> <dpitch>", "turn the soldier's aim by radians (yaw right +, pitch up +)"}},
    };
    const auto& [usage, help] = kHelp.at(game);
    Register(game, usage, help, [](const Args& a) { return GameRequest(Rest(a, 0)); });
  }

  Register("key", "key <Key>[+<Key>...] [ms=120|down|up]",
           "hold keyboard/mouse-button keys by their key_* names (W, Space, LShift, LMB, RMB...), no focus needed",
           [](const Args& a) {
             if (a.size() < 2) return Fail("usage: key <Key>[+<Key>...] [ms|down|up]");
             long long ms = 120;
             if (a.size() > 2) {
               if (a[2] == "down") {
                 ms = 24LL * 3600 * 1000;
               } else if (a[2] == "up") {
                 ms = 0;
               } else if (!ParseInt(a[2], ms) || ms < 0) {
                 return Fail("hold must be ms, down or up");
               }
             }
             std::vector<int> vks;
             std::stringstream names(a[1]);
             for (std::string n; std::getline(names, n, '+');) {
               const int vk = pc_keys::ParseKeyName(n);
               if (!vk) return Fail("unknown key '" + n + "'");
               vks.push_back(vk);
             }
             for (int vk : vks) pc_keys::InjectKey(vk, uint32_t(ms));
             Reply r;
             r.obj.Int("hold_ms", ms);
             return r;
           });
  Register("pad", "pad <A+LB+LY=32767+RT...|release> [ms=150|down]",
           "hold controller 0 buttons, triggers, sticks (BF3_PAD_SCRIPT names); replaces the previous pad state",
           [](const Args& a) {
             if (a.size() < 2) return Fail("usage: pad <buttons|release> [ms|down]");
             uint16_t buttons = 0;
             uint8_t lt = 0, rt = 0;
             int16_t thumbs[4] = {};
             long long ms = 150;
             if (a[1] == "release") {
               ms = 0;
             } else {
               if (!ParsePadState(a[1], buttons, lt, rt, thumbs)) return Fail("unknown pad input in '" + a[1] + "'");
               if (a.size() > 2) {
                 if (a[2] == "down") {
                   ms = 24LL * 3600 * 1000;
                 } else if (!ParseInt(a[2], ms) || ms < 0) {
                   return Fail("hold must be ms or down");
                 }
               }
             }
             InjectPad(buttons, lt, rt, thumbs, uint32_t(ms));
             Reply r;
             r.obj.Int("hold_ms", ms);
             return r;
           });
  Register("mouse", "mouse move <x> <y> | click [<x> <y>] | down | up | wheel <lines> | look <dx> <dy>",
           "menu mouse (x, y 0..1 across the game image) or raw look counts (x right, y down)", [](const Args& a) {
             static float x = 0.5f, y = 0.5f;
             if (a.size() < 2) return Fail("usage: mouse move|click|down|up|wheel|look ...");
             const std::string& what = a[1];
             double v1 = 0, v2 = 0;
             const bool two = a.size() >= 4 && ParseDouble(a[2], v1) && ParseDouble(a[3], v2);
             if (what == "look") {
               if (!two) return Fail("usage: mouse look <dx> <dy>");
               pc_mouse::InjectLookCounts(v1, v2);
               return Reply{};
             }
             if (what == "move" || (what == "click" && a.size() > 2)) {
               if (!two) return Fail("x and y must be numbers (0..1)");
               x = float(std::clamp(v1, 0.0, 1.0)), y = float(std::clamp(v2, 0.0, 1.0));
             }
             bool sent = false;
             if (what == "move") {
               sent = pc_mouse::InjectMenuEvent(1, x, y, 0);
             } else if (what == "click") {
               sent = pc_mouse::InjectMenuEvent(1, x, y, 0) && pc_mouse::InjectMenuEvent(2, x, y, 0);
               Sleep(80);
               sent = sent && pc_mouse::InjectMenuEvent(3, x, y, 0);
             } else if (what == "down" || what == "up") {
               sent = pc_mouse::InjectMenuEvent(what == "down" ? 2 : 3, x, y, 0);
             } else if (what == "wheel") {
               if (a.size() < 3 || !ParseDouble(a[2], v1)) return Fail("usage: mouse wheel <lines>");
               sent = pc_mouse::InjectMenuEvent(4, x, y, float(v1));
             } else {
               return Fail("unknown mouse action '" + what + "'");
             }
             if (!sent) return Fail("menu mouse not ready");
             Reply r;
             r.obj.Number("x", x).Number("y", y);
             return r;
           });

  Register("cvar", "cvar <name> [value]", "read or set a flag (PC SETTINGS flags, ReXGlue flags)", [](const Args& a) {
    if (a.size() < 2) return Fail("usage: cvar <name> [value]");
    if (a.size() > 2 && !rex::cvar::SetFlagByName(a[1], Rest(a, 2))) return Fail("can't set '" + a[1] + "'");
    Reply r;
    r.obj.Str("name", a[1]).Str("value", rex::cvar::GetFlagByName(a[1]));
    return r;
  });
  Register("peek", "peek <guest addr> [bytes=64, max 4096]", "raw guest memory (big-endian bytes, hex)",
           [](const Args& a) {
             long long addr = 0, len = 64;
             if (a.size() < 2 || !ParseInt(a[1], addr, 16) || addr < 0 || addr > 0xFFFFFFFFLL) {
               return Fail("usage: peek <hex addr> [bytes]");
             }
             if (a.size() > 2 && (!ParseInt(a[2], len) || len <= 0 || len > 4096)) return Fail("bytes must be 1-4096");
             static auto fn = DllExport<int (*)(uint32_t, uint8_t*, int)>("bf3_dev_peek");
             if (!fn) fn = DllExport<int (*)(uint32_t, uint8_t*, int)>("bf3_dev_peek");
             std::vector<uint8_t> buf(static_cast<size_t>(len));
             if (!fn || fn(uint32_t(addr), buf.data(), int(len)) != int(len)) return Fail("not readable");
             std::string hex;
             for (uint8_t b : buf) {
               char h[3];
               std::snprintf(h, sizeof(h), "%02x", b);
               hex += h;
             }
             Reply r;
             r.obj.Address("addr", uint32_t(addr)).Str("hex", hex);
             return r;
           }, true);
  Register("shot", "shot", "capture the game window: \"bytes\" raw top-down BGRA follow the JSON line",
           [](const Args&) { return Shot(); }, true);
  Register("quit", "quit", "end the game process now", [](const Args&) -> Reply {
    REXLOG_INFO("debug api: quit");
    TerminateProcess(GetCurrentProcess(), 0);
    return {};
  });
}

// ---- server ------------------------------------------------------------------------------------------------------

bool SendAll(SOCKET s, const char* data, size_t n) {
  while (n) {
    const int sent = send(s, data, int(std::min<size_t>(n, 1 << 20)), 0);
    if (sent <= 0) return false;
    data += sent;
    n -= size_t(sent);
  }
  return true;
}

std::string Handle(const std::string& line, std::string& binary) {
  Args args;
  std::istringstream in(line);
  for (std::string w; in >> w;) args.push_back(w);
  Reply reply;
  if (args.empty()) {
    reply = Fail("empty request");
  } else if (auto it = Commands().find(args[0]); it == Commands().end()) {
    reply = Fail("unknown command '" + args[0] + "' (try help)");
  } else {
    if (!it->second.quiet) REXLOG_INFO("debug api: {}", line);
    try {
      reply = it->second.run(args);
    } catch (const std::exception& e) {
      reply = Fail(std::string("exception: ") + e.what());
    }
  }
  Obj head;
  head.Bool("ok", reply.ok);
  if (!reply.ok) return head.Str("error", reply.error).Json();
  if (!reply.binary.empty()) head.Int("bytes", int64_t(reply.binary.size()));
  binary = std::move(reply.binary);
  std::string json = head.Json(), body = reply.obj.Json();
  if (body.size() > 2) json = json.substr(0, json.size() - 1) + "," + body.substr(1);
  return json;
}

void Serve(SOCKET client) {
  std::string pending;
  char buf[4096];
  for (;;) {
    const int n = recv(client, buf, sizeof(buf), 0);
    if (n <= 0) break;
    pending.append(buf, size_t(n));
    for (size_t eol; (eol = pending.find('\n')) != std::string::npos;) {
      std::string line = pending.substr(0, eol);
      pending.erase(0, eol + 1);
      if (!line.empty() && line.back() == '\r') line.pop_back();
      std::string binary;
      const std::string json = Handle(line, binary) + "\n";
      if (!SendAll(client, json.data(), json.size()) || !SendAll(client, binary.data(), binary.size())) {
        closesocket(client);
        return;
      }
    }
  }
  closesocket(client);
}

void Listen(int port) {
  WSADATA wsa;
  if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
    REXLOG_WARN("debug api: WSAStartup failed");
    return;
  }
  SOCKET server = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(uint16_t(port));
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);  // this machine only (WSL reaches it through mirrored networking)
  BOOL exclusive = TRUE;
  setsockopt(server, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&exclusive), sizeof(exclusive));
  if (server == INVALID_SOCKET || bind(server, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 ||
      listen(server, 4) != 0) {
    REXLOG_WARN("debug api: can't listen on 127.0.0.1:{} ({})", port, WSAGetLastError());
    return;
  }
  REXLOG_INFO("debug api: listening on 127.0.0.1:{}", port);
  for (;;) {
    SOCKET client = accept(server, nullptr, nullptr);
    if (client == INVALID_SOCKET) continue;
    BOOL nodelay = TRUE;
    setsockopt(client, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&nodelay), sizeof(nodelay));
    std::thread(Serve, client).detach();
  }
}

}  // namespace

void Install() {
  int port = kDefaultPort;
  if (const char* env = std::getenv("BF3_DEBUG_API"); env && *env) port = std::atoi(env);
  if (port <= 0 || port > 65535) {
    REXLOG_INFO("debug api: off");
    return;
  }
  RegisterCommands();
  std::thread(Listen, port).detach();
}

}  // namespace bf3::dev

#else
namespace bf3::dev {
void Install() {}
}  // namespace bf3::dev
#endif
