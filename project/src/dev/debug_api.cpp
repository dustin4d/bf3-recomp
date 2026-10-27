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
#include <rex/image_info.h>
#include <rex/logging.h>
#include <rex/logging/api.h>
#include <spdlog/details/null_mutex.h>
#include <spdlog/sinks/base_sink.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <deque>
#include <mutex>
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
#include "dev_breakpoints.h"
#include "dev_menu.h"
#include "../crash_trace.h"
#include "../pad_inject.h"
#include "../pc_keys.h"
#include "../pc_mouse.h"
#include "../ui_state.h"

// The launcher's image info and coverage hit bytes, generated into bf3.exe (bf3_init.cpp).
extern const rex::PPCImageInfo PPCImageConfig;
#if defined(REX_COVERAGE)
extern "C" uint8_t rex_coverage_hits[];
#endif

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
  // Appends already formatted members ("k":v,...), e.g. from bf3_dev_query.
  Obj& Members(std::string_view members) {
    if (members.empty()) return *this;
    if (!body_.empty()) body_ += ',';
    body_ += members;
    return *this;
  }
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

// ---- coverage (codegen option `coverage`, patch 0008) -------------------------------------------------------------

// One module's hit bytes: entry i belongs to the guest function starting at code_base + 4 * i.
struct CoverageModule {
  const char* name;
  uint32_t code_base = 0, count = 0;
  uint8_t* hits = nullptr;
};

std::vector<CoverageModule> CoverageModules() {
  std::vector<CoverageModule> out;
#if defined(REX_COVERAGE)
  out.push_back({"Default", PPCImageConfig.code_base, (PPCImageConfig.code_size + 3) / 4, rex_coverage_hits});
#endif
  for (auto [name, dll] : {std::pair{"BF.Main", L"bf3_BF_Main.dll"}, {"Engine.BuildInfo", L"bf3_Engine_BuildInfo.dll"}}) {
    HMODULE module = GetModuleHandleW(dll);
    auto get = module ? reinterpret_cast<uint8_t* (*)(uint32_t*, uint32_t*)>(GetProcAddress(module, "ReXModule_GetCoverage"))
                      : nullptr;
    if (!get) continue;
    CoverageModule m{name};
    m.hits = get(&m.code_base, &m.count);
    out.push_back(m);
  }
  return out;
}

// coverage [stats] | dump [reset] | reset. dump sends the hit function addresses (u32 little-endian, module by module
// in the "modules" order) as the reply's bytes; with reset each byte is cleared as it is read.
Reply Coverage(const Args& a) {
  const std::string op = a.size() > 1 ? a[1] : "stats";
  const bool dump = op == "dump", reset = op == "reset" || (dump && a.size() > 2 && a[2] == "reset");
  if (!dump && !reset && op != "stats") return Fail("usage: coverage [stats] | dump [reset] | reset");
  const auto modules = CoverageModules();
  if (modules.empty()) return Fail("no module has coverage (codegen option `coverage`, REX_COVERAGE build)");
  Reply r;
  std::string list = "[";
  for (const auto& m : modules) {
    uint32_t hits = 0;
    for (uint32_t i = 0; i < m.count; ++i) {
      if (!m.hits[i]) continue;
      ++hits;
      if (dump) {
        const uint32_t addr = m.code_base + 4 * i;
        r.binary.append(reinterpret_cast<const char*>(&addr), 4);
      }
      if (reset) m.hits[i] = 0;
    }
    Obj o;
    o.Str("name", m.name).Address("code_base", m.code_base).Int("slots", m.count).Int("hits", hits);
    list += (list.size() > 1 ? "," : "") + o.Json();
  }
  r.obj.Raw("modules", list + "]");
  return r;
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

std::vector<std::string> SplitList(const std::string& list) {  // ';' separated
  std::vector<std::string> out;
  std::stringstream ss(list);
  for (std::string s; std::getline(ss, s, ';');) {
    if (!s.empty()) out.push_back(s);
  }
  return out;
}

// Where the game is, for tests to wait on: boot, main_menu, menu (other front end screens), loading, spawn_screen,
// playing (a local soldier with the HUD up), dead (killed, until the spawn screen), ingame_menu (a menu over the level).
std::string Phase(const Snapshot* s, const std::string& top, const std::string& live) {
  if (!s || live.empty()) return "boot";
  if (live.find("Loading") != std::string::npos) return "loading";
  if (live.find("SpawnScreen") != std::string::npos) return "spawn_screen";
  if (live.find("KillScreen") != std::string::npos) return "dead";
  const bool hud = live.find("HudScreen") != std::string::npos;
  if (hud && !s->soldier && s->level_loaded && std::string_view(s->level).find("FrontEnd") == std::string_view::npos) {
    return "dead";  // between the death and the kill screen
  }
  if (hud && s->soldier) {
    for (const char* menu : {"Menu", "Popup", "Options", "BackScreen", "EndOfRound"}) {
      if (live.find(menu) != std::string::npos) return "ingame_menu";
    }
    return "playing";
  }
  if (top == "MainMenuScreen") return s->frontend_ready ? "main_menu" : "menu";
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
  const std::string top = ui_state::TopScreen(), live = have ? s.screens : "";
  Reply r;
  r.obj.Str("phase", Phase(have ? &s : nullptr, top, live));
  r.obj.Child("ui", Obj().Str("top", top).Strings("screens", SplitList(live)));
  r.obj.Child("input", Obj().Bool("focused", pc_mouse::GameFocused()).Bool("menu_mode", pc_mouse::MenuMode()));
  if (!have) return r;
  r.obj.Int("frame", int64_t(s.frame)).Int("ticks", int64_t(s.ticks)).Number("uptime_s", s.uptime_s);
  r.obj.Child("level", Obj().Str("name", s.level)
                           .Bool("loaded", s.level_loaded)
                           .Bool("unloading", s.level_unloading)
                           .Number("ingame_s", s.ingame_s)
                           .Bool("frontend_ready", s.frontend_ready));
  Obj player;
  player.Address("soldier", s.soldier).Str("team", TeamName(s.team)).Address("server_player", s.server_player)
      .Address("server_soldier", s.server_soldier);
  if (s.server_soldier) player.Number("server_health", s.server_health).Vec("server_pos", s.server_pos, 3);
  if (s.soldier) {
    float yaw, pitch;
    ViewAngles(s.transform, 1.0f, yaw, pitch);
    const float speed = std::sqrt(s.velocity[0] * s.velocity[0] + s.velocity[1] * s.velocity[1] +
                                  s.velocity[2] * s.velocity[2]);
    player.Bool("on_foot", s.on_foot)
        .Vec("pos", s.transform + 12, 3)
        .Number("heading", yaw)
        .Number("health", s.health)
        .Str("weapon", s.weapon)
        .Strings("attachments", SplitList(s.attachments))
        .Int("ammo_mag", s.ammo_mag)
        .Int("ammo_reserve", s.ammo_reserve)
        .Int("kit", s.kit)
        .Number("aim_yaw", s.aim_yaw)
        .Number("aim_pitch", s.aim_pitch)
        .Vec("velocity", s.velocity, 3)
        .Number("speed", speed)
        .Number("transform_age_s", s.transform_age_s);
  }
  r.obj.Child("player", player);
  if (s.preround_entity) {
    const char* state = !s.round_started ? "waiting_for_players" : s.round_countdown > 0 ? "countdown" : "running";
    r.obj.Child("round", Obj().Str("state", state)
                             .Number("countdown_s", std::max(0.0f, s.round_countdown))
                             .Int("players_needed", s.round_players_needed)
                             .Address("preround_entity", s.preround_entity));
  }
  float cyaw, cpitch;
  ViewAngles(s.camera, -1.0f, cyaw, cpitch);
  r.obj.Child("camera", Obj().Vec("pos", s.camera + 12, 3).Number("yaw", cyaw).Number("pitch", cpitch).Int(
                            "calls_per_frame", s.camera_calls));
  r.obj.Child("time", Obj().Bool("paused", s.paused).Number("timescale", s.timescale));
  std::vector<std::string> overrides;
  for (const auto& o : s.weapon_override) overrides.push_back(o);
  r.obj.Child("cheats", Obj().Bool("god", s.god)
                            .Bool("infinite_ammo", s.infinite_ammo)
                            .Bool("flying", s.flying)
                            .Number("fly_speed", s.fly_speed)
                            .Strings("weapon_overrides", overrides)
                            .Bool("respawning", s.respawning));
  auto side = [](const Snapshot::NetSide& n) {
    return Obj().Number("tx_pps", n.tx_packets).Number("rx_pps", n.rx_packets).Number("tx_kbps", n.tx_kbytes)
        .Number("rx_kbps", n.rx_kbytes).Number("latency_ms", n.latency_ms).Number("latency_max_ms", n.latency_max_ms);
  };
  r.obj.Child("net", Obj().Child("client", side(s.net_client))
                         .Child("server", side(s.net_server))
                         .Number("msg_up_ps", s.msg_up)
                         .Number("msg_down_ps", s.msg_down)
                         .Number("server_tick_hz", s.server_ticks)
                         .Number("client_tick_hz", s.client_ticks));
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

// "sub_X" or "0x..." -> bare hex digits.
std::string Hex0(std::string s) {
  if (s.rfind("sub_", 0) == 0) return s.substr(4);
  if (s.rfind("0x", 0) == 0) return s.substr(2);
  return s;
}

Reply BpReply(int) {
  std::string list = "[";
  bool first = true;
  for (const BpState& b : Breakpoints()) {
    std::string records = "[";
    for (size_t i = 0; i < b.records.size(); ++i) {
      const BpHit& h = b.records[i];
      Obj o;
      o.Int("tid", h.tid).Number("t", h.t);
      if (b.kind == BpKind::kTrace) {
        o.Address("lr", h.lr);
        std::string regs = "[";
        for (int k = 0; k < 8; ++k) regs += (k ? "," : "") + Hex(uint32_t(h.r[k]));
        o.Raw("r3_r10", regs + "]");
        o.Raw("f1_f4", "[" + Num(h.f[0]) + "," + Num(h.f[1]) + "," + Num(h.f[2]) + "," + Num(h.f[3]) + "]");
        o.Bool("returned", h.returned);
        if (h.returned) o.Address("ret_r3", uint32_t(h.ret_r3)).Number("ret_f1", h.ret_f1).Number("us", h.duration_us);
      } else {
        o.Str("value", h.value_hex);
      }
      o.Strings("frames", h.frames);
      records += (i ? "," : "") + o.Json();
    }
    const char* kind = b.kind == BpKind::kTrace ? "trace" : b.kind == BpKind::kWrite ? "write" : "readwrite";
    list += (first ? "" : ",") + Obj()
                                     .Int("slot", b.slot)
                                     .Str("kind", kind)
                                     .Address("addr", b.guest)
                                     .Int("len", b.len)
                                     .Bool("active", b.active)
                                     .Int("hits", b.hits)
                                     .Int("max_hits", b.max_hits)
                                     .Raw("records", records + "]")
                                     .Json();
    first = false;
  }
  Reply r;
  r.obj.Raw("breakpoints", list + "]");
  return r;
}

// watch off / trace off [<addr>|all]: removes that kind's breakpoints on the address (all of the kind without one).
Reply RemoveBp(const Args& a, BpKind kind) {
  long long addr = -1;
  if (a.size() > 2 && a[2] != "all" && !ParseInt(Hex0(a[2]), addr, 16)) return Fail("bad address");
  for (const BpState& b : Breakpoints()) {
    const bool same_kind = (kind == BpKind::kTrace) == (b.kind == BpKind::kTrace);
    if (same_kind && (addr < 0 || uint32_t(addr) == b.guest)) RemoveBreakpoint(b.slot);
  }
  return BpReply(-1);
}

// The crash recorder's frames (bf3_dev_history), oldest first.
std::string HistoryJson(int count) {
  static auto fn = DllExport<int (*)(HistoryRecord*, int)>("bf3_dev_history");
  if (!fn) fn = DllExport<int (*)(HistoryRecord*, int)>("bf3_dev_history");
  std::vector<HistoryRecord> h(size_t(std::max(count, 1)));
  h.resize(fn ? size_t(fn(h.data(), int(h.size()))) : 0);
  std::string list = "[";
  for (size_t i = 0; i < h.size(); ++i) {
    const HistoryRecord& r = h[i];
    list += (i ? "," : "") + Obj()
                                 .Number("t", r.uptime_s)
                                 .Int("frame", r.frame)
                                 .Number("ms", r.frame_ms)
                                 .Address("soldier", r.soldier)
                                 .Address("server_soldier", r.server_soldier)
                                 .Vec("pos", r.pos, 3)
                                 .Number("aim_yaw", r.aim_yaw)
                                 .Number("aim_pitch", r.aim_pitch)
                                 .Number("health", r.health)
                                 .Number("server_health", r.server_health)
                                 .Json();
  }
  return list + "]";
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
  for (const char* game : {"level", "team", "deploy", "look", "turn", "teleport", "entities"}) {
    static const std::map<std::string, std::pair<const char*, const char*>> kHelp = {
        {"level", {"level [<level>[:<category>=<value>]]", "switch the local server's level (default Metro TDM)"}},
        {"team", {"team <us|ru>", "team for the next join; moves the player now if already in the level"}},
        {"deploy", {"deploy [kit 0-3]", "deploy from the spawn screen (0 assault, 1 engineer, 2 support, 3 recon)"}},
        {"look", {"look <yaw> <pitch>", "set the soldier's aim (aimer radians, as status player.aim_yaw/aim_pitch)"}},
        {"turn", {"turn <dyaw> <dpitch>", "turn the soldier's aim by radians (yaw right +, pitch up +)"}},
        {"entities", {"entities <ServerClassName|0xTypeInfo> [max=64]", "live server entities of a class "
                                                                         "(address/vtable)"}},
        {"teleport", {"teleport <x> <y> <z> [heading]", "move the soldier (server side, as a spawn does); heading "
                                                         "as status player.heading, default unchanged"}},
    };
    const auto& [usage, help] = kHelp.at(game);
    Register(game, usage, help, [](const Args& a) { return GameRequest(Rest(a, 0)); });
  }

  // Debug menu cheats (bf_main/dev_cheats.cpp), the same commands the in-game menu (dev/dev_menu.cpp) sends.
  static const std::vector<std::array<const char*, 3>> kCheats = {
      {"god", "god [on|off|toggle]", "god mode: the local soldier takes no damage"},
      {"ammo", "ammo [on|off|toggle]", "infinite ammo: shots don't use up the magazine"},
      {"fly", "fly [on|off|toggle] [m/s]", "noclip flight along the camera (move keys, jump up, crouch down, "
                                           "sprint x4); no fall damage for 8 s after it ends"},
      {"flyspeed", "flyspeed <m/s>", "flight speed (default 10)"},
      {"heal", "heal", "full health (server side)"},
      {"kill", "kill", "kill the local soldier"},
      {"respawn", "respawn [kit 0-3]", "kill if alive, then deploy again from the spawn screen (default: last kit)"},
      {"weapon", "weapon <primary|sidearm|gadget1|gadget2> <name|none>",
       "loadout override for the next deploy (name: M416, U_M4, Weapons/M416/U_M416; none = the kit's)"},
  };
  for (const auto& [name, usage, help] : kCheats) {
    Register(name, usage, help, [](const Args& a) { return GameRequest(Rest(a, 0)); });
  }
  Register("menu", "menu [on|off|toggle] [player|movement|teleport|weapons|overlay]",
           "open or close the in-game debug menu (Insert) on a tab", [](const Args& a) {
             const std::string v = a.size() > 1 ? a[1] : "toggle";
             const bool open = v == "on" ? true : v == "off" ? false : !MenuOpen();
             SetMenuOpen(open, a.size() > 2 ? a[2].c_str() : "");
             Reply r;
             r.obj.Bool("open", open);
             return r;
           });
  Register("weapons", "weapons", "weapon unlocks loaded in the level, by kind (p primary, s sidearm, g gadget, x other)",
           [](const Args&) {
             static auto fn = DllExport<int (*)(char*, int)>("bf3_dev_weapons");
             if (!fn) fn = DllExport<int (*)(char*, int)>("bf3_dev_weapons");
             if (!fn) return Fail("BF.Main is not loaded yet");
             std::string list(32768, '\0');
             list.resize(size_t(std::max(0, fn(list.data(), int(list.size())))));
             std::map<std::string, std::vector<std::string>> kinds;
             for (const std::string& e : SplitList(list)) {
               if (e.size() > 2 && e[1] == ':') kinds[std::string(1, e[0])].push_back(e.substr(2));
             }
             Reply r;
             for (const auto& [kind, names] : kinds) r.obj.Strings(kind, names);
             return r;
           });

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
  Register("find", "find f32|u32|bytes <v>[,<v>...] [tol=0.001] [<lo hex>-<hi hex>]",
           "search guest memory for a run of floats (within tol), u32s or a hex byte string; default range "
           "40000000-C0000000 (heaps and physical); returns up to 512 addresses and the total",
           [](const Args& a) {
             if (a.size() < 3) return Fail("usage: find f32|u32|bytes <values> [tol] [lo-hi]");
             const int kind = a[1] == "f32" ? 0 : a[1] == "u32" ? 1 : a[1] == "bytes" ? 2 : -1;
             if (kind < 0) return Fail("kind must be f32, u32 or bytes");
             std::vector<double> values;
             if (kind == 2) {
               if (a[2].size() % 2 || a[2].size() > 128) return Fail("bytes: an even number of hex digits, up to 64 bytes");
               for (size_t i = 0; i < a[2].size(); i += 2) {
                 long long b;
                 if (!ParseInt(a[2].substr(i, 2), b, 16)) return Fail("bad hex '" + a[2] + "'");
                 values.push_back(double(b));
               }
             } else {
               std::stringstream ss(a[2]);
               for (std::string v; std::getline(ss, v, ',');) {
                 double d;
                 long long i;
                 if (kind == 1 && v.rfind("0x", 0) == 0 && ParseInt(v.substr(2), i, 16)) {
                   d = double(i);
                 } else if (!ParseDouble(v, d)) {
                   return Fail("bad value '" + v + "'");
                 }
                 values.push_back(d);
               }
             }
             double tol = 0.001;
             uint32_t lo = 0x40000000, hi = 0xC0000000;
             for (size_t i = 3; i < a.size(); ++i) {
               if (const size_t dash = a[i].find('-'); dash != std::string::npos && dash > 0) {
                 long long l, h;
                 if (!ParseInt(a[i].substr(0, dash), l, 16) || !ParseInt(a[i].substr(dash + 1), h, 16) || l >= h ||
                     h > 0x100000000LL) {
                   return Fail("range must be <lo hex>-<hi hex>");
                 }
                 lo = uint32_t(l), hi = uint32_t(std::min(h, 0xFFFFFFFFLL));
               } else if (!ParseDouble(a[i], tol)) {
                 return Fail("tolerance must be a number");
               }
             }
             static auto fn = DllExport<int (*)(int, const double*, int, double, uint32_t, uint32_t, uint32_t*, int)>(
                 "bf3_dev_find");
             if (!fn) fn = DllExport<int (*)(int, const double*, int, double, uint32_t, uint32_t, uint32_t*, int)>(
                 "bf3_dev_find");
             if (!fn) return Fail("BF.Main is not loaded yet");
             std::vector<uint32_t> hits(512);
             const int total = fn(kind, values.data(), int(values.size()), tol, lo, hi, hits.data(), int(hits.size()));
             std::string list = "[";
             for (int i = 0; i < std::min<int>(total, int(hits.size())); ++i) list += (i ? "," : "") + Hex(hits[i]);
             Reply r;
             r.obj.Int("total", total).Raw("addrs", list + "]");
             return r;
           }, true);
  Register("poke", "poke <guest addr> f32|u32|u16|u8|bytes <v>[,<v>...]",
           "write guest memory (big-endian), e.g. poke 46087100 f32 1.5,2,3", [](const Args& a) {
             long long addr = 0;
             if (a.size() < 4 || !ParseInt(a[1], addr, 16) || addr < 0 || addr > 0xFFFFFFFFLL) {
               return Fail("usage: poke <hex addr> f32|u32|u16|u8|bytes <values>");
             }
             const std::string& kind = a[2];
             std::string bytes;
             if (kind == "bytes") {
               for (size_t i = 0; i + 1 < a[3].size(); i += 2) {
                 long long b;
                 if (!ParseInt(a[3].substr(i, 2), b, 16)) return Fail("bad hex");
                 bytes += char(b);
               }
             } else {
               const int width = kind == "f32" || kind == "u32" ? 4 : kind == "u16" ? 2 : kind == "u8" ? 1 : 0;
               if (!width) return Fail("kind must be f32, u32, u16, u8 or bytes");
               std::stringstream ss(a[3]);
               for (std::string v; std::getline(ss, v, ',');) {
                 uint32_t raw;
                 if (kind == "f32") {
                   double d;
                   if (!ParseDouble(v, d)) return Fail("bad value '" + v + "'");
                   const float f = float(d);
                   std::memcpy(&raw, &f, 4);
                 } else {
                   long long i;
                   if (!(v.rfind("0x", 0) == 0 ? ParseInt(v.substr(2), i, 16) : ParseInt(v, i))) {
                     return Fail("bad value '" + v + "'");
                   }
                   raw = uint32_t(i);
                 }
                 for (int b = width - 1; b >= 0; --b) bytes += char(raw >> (8 * b));
               }
             }
             static auto fn = DllExport<bool (*)(uint32_t, const uint8_t*, int)>("bf3_dev_poke");
             if (!fn) fn = DllExport<bool (*)(uint32_t, const uint8_t*, int)>("bf3_dev_poke");
             if (bytes.empty()) return Fail("nothing to write");
             if (!fn || !fn(uint32_t(addr), reinterpret_cast<const uint8_t*>(bytes.data()), int(bytes.size()))) {
               return Fail("not writable");
             }
             Reply r;
             r.obj.Address("addr", uint32_t(addr)).Int("written", int64_t(bytes.size()));
             return r;
           });
  Register("watch", "watch [<addr> [len=4] [hits=8] [secs=10] [w|rw]] | watch off [<addr>|all]",
           "hardware watch on a heap address: who writes it (w) or reads and writes it (rw); host stacks "
           "(bf3dbg.py names the guest functions); up to 4 watches/traces at once; no address: all breakpoints",
           [](const Args& a) {
             if (a.size() > 1 && a[1] == "off") return RemoveBp(a, BpKind::kWrite);
             if (a.size() > 1) {
               long long addr, len = 4, hits = 8, secs = 10;
               if (!ParseInt(Hex0(a[1]), addr, 16) || addr < 0 || addr > 0xFFFFFFFFLL) return Fail("bad address");
               bool reads = false;
               std::vector<long long*> nums = {&len, &hits, &secs};
               size_t ni = 0;
               for (size_t i = 2; i < a.size(); ++i) {
                 if (a[i] == "rw" || a[i] == "w") {
                   reads = a[i] == "rw";
                 } else if (ni >= nums.size() || !ParseInt(a[i], *nums[ni++])) {
                   return Fail("usage: watch <addr> [len 1|2|4|8] [hits 1-256] [secs 1-3600] [w|rw]");
                 }
               }
               if (hits < 1 || hits > 256 || secs < 1 || secs > 3600) return Fail("hits 1-256, secs 1-3600");
               std::string error;
               if (AddWatch(uint32_t(addr), uint32_t(len), reads, int(hits), uint32_t(secs), error) < 0) return Fail(error);
             }
             return BpReply(-1);
           }, true);
  Register("trace", "trace [<sub_X|addr> [hits=16] [secs=10]] | trace off [<addr>|all]",
           "record calls of a BF.Main guest function: r3-r10, f1-f4 and the guest caller (lr) at entry, r3/f1 on return, "
           "duration, host stack; shares the 4 debug registers with watch",
           [](const Args& a) {
             if (a.size() > 1 && a[1] == "off") return RemoveBp(a, BpKind::kTrace);
             if (a.size() > 1) {
               long long addr, hits = 16, secs = 10;
               if (!ParseInt(Hex0(a[1]), addr, 16) || addr < 0 || addr > 0xFFFFFFFFLL) return Fail("bad address");
               if ((a.size() > 2 && !ParseInt(a[2], hits)) || (a.size() > 3 && !ParseInt(a[3], secs)) || hits < 1 ||
                   hits > 1024 || secs < 1 || secs > 3600) {
                 return Fail("usage: trace <sub_X|addr> [hits 1-1024] [secs 1-3600]");
               }
               static auto fn = DllExport<void* (*)(uint32_t)>("bf3_dev_host_func");
               if (!fn) fn = DllExport<void* (*)(uint32_t)>("bf3_dev_host_func");
               if (!fn) return Fail("BF.Main is not loaded yet");
               std::string error;
               if (AddTrace(uint32_t(addr), fn(uint32_t(addr)), int(hits), uint32_t(secs), error) < 0) return Fail(error);
             }
             return BpReply(-1);
           }, true);
  Register("bp", "bp", "all hardware breakpoints (watches and traces) and their records", [](const Args&) {
    return BpReply(-1);
  }, true);

  // Tools on the game side (bf_main/dev_tools.cpp).
  static const std::vector<std::pair<std::string, std::pair<const char*, const char*>>> kTools = {
      {"call", {"call <sub_X|addr> [r3..r10=<v>] [f1..f4=<v>] [scratch=<n>|scratchhex=<hex>] [out=<n>]",
                "run a guest function on the main thread; values decimal, 0x-hex or $scratch[+n] (a stack buffer)"}},
      {"scan", {"scan new <u32|f32|u16|u8> <value|any> [tol] [lo-hi] | next <op> [v] [tol] | list [n]",
                "narrowing memory search; ops eq ne gt lt incby decby changed unchanged inc dec"}},
      {"snap", {"snap <name> <addr> <len>", "remember a memory region (up to 4 MB) for diff"}},
      {"diff", {"diff <name> [max=200]", "words of a snapped region that changed since"}},
      {"ptrpath", {"ptrpath <root> <target> [depth=3] [span=0x800] [max=20] [vt]",
                   "pointer paths from an object to an address (vt: only through objects with a vtable)"}},
      {"whatis", {"whatis <addr>", "name an address: function, vtable/type info/object of a class, inside an object, "
                                   "string"}},
      {"messages", {"messages on [hash,...] | off | read [max] | stats [n]",
                    "trace the game's message dispatcher (bf3dbg.py names the hashes)"}},
      {"pause", {"pause", "hold the main loop at the frame boundary (guest clock slowed to 1% meanwhile)"}},
      {"resume", {"resume", "release a pause"}},
      {"step", {"step [n=1] [frames|ticks]", "run n frames (or n local simulation ticks), then pause again"}},
      {"timescale", {"timescale [x]", "guest clock speed, 0.1-4 (1 = normal)"}},
  };
  for (const auto& [name, doc] : kTools) {
    Register(name, doc.first, doc.second, [](const Args& a) {
      static auto fn = DllExport<int (*)(const char*, char*, int)>("bf3_dev_query");
      if (!fn) fn = DllExport<int (*)(const char*, char*, int)>("bf3_dev_query");
      if (!fn) return Fail("BF.Main is not loaded yet");
      std::string out(1 << 20, '\0');
      const int n = fn(Rest(a, 0).c_str(), out.data(), int(out.size()));
      if (n < 0) return Fail(out.c_str());
      Reply r;
      r.obj.Members(std::string_view(out.data(), size_t(n)));
      return r;
    }, name == "scan" || name == "diff" || name == "whatis" || name == "messages");
  }
  Register("history", "history [frames=120]",
           "the last frames as the crash recorder keeps them (time, frame ms, soldier, position, aim, health)",
           [](const Args& a) {
             long long n = 120;
             if (a.size() > 1 && !ParseInt(a[1], n)) return Fail("bad count");
             Reply r;
             r.obj.Raw("frames", HistoryJson(int(std::clamp(n, 1LL, 3000LL))));
             return r;
           }, true);
  Register("coverage", "coverage [stats] | dump [reset] | reset",
           "functions run since start or the last reset (one hit byte per function); dump: \"bytes\" of u32 addresses follow",
           Coverage, true);
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

// ---- crash recorder ----------------------------------------------------------------------------------------------
// The last commands, kept for the crash report.
std::mutex g_log_mutex;
std::deque<std::string> g_command_log;
const auto g_api_start = std::chrono::steady_clock::now();

// Logged when a command starts ("..."), then marked ok/FAIL when it returns: a command still running at a crash shows.
uint64_t g_command_seq = 0;

uint64_t LogCommandStart(const std::string& line) {
  const double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - g_api_start).count();
  std::lock_guard lock(g_log_mutex);
  g_command_log.push_back(fmt::format("#{} {:9.3f}s ...  {}", ++g_command_seq, t, line));
  if (g_command_log.size() > 64) g_command_log.pop_front();
  return g_command_seq;
}

void LogCommandEnd(uint64_t seq, bool ok) {
  const std::string tag = fmt::format("#{} ", seq);
  std::lock_guard lock(g_log_mutex);
  for (std::string& c : g_command_log) {
    if (c.rfind(tag, 0) == 0) {
      const size_t dots = c.find(" ...  ");
      if (dots != std::string::npos) c.replace(dots, 6, ok ? " ok   " : " FAIL ");
    }
  }
}

// The crash report: the last API commands, the breakpoints, and the last frames of game state (every frame for the
// last 30, then every 25th; the DLL keeps 3000 frames). scripts/bf3dbg.py crash prints the last report from the log.
std::atomic<int> g_reports{0};

void LogCrashReport(std::string_view reason, const CONTEXT* native) {
  if (g_reports.fetch_add(1) >= 3) return;
  REXLOG_ERROR("[crash_report] begin ({})", reason.substr(0, 160));
  if (native) {
    CONTEXT c = *native;
    for (int frame = 0; frame < 32 && c.Rip; ++frame) {
      REXLOG_ERROR("[crash_report] stack #{:02} {}", frame, AnyFrameName(c.Rip));
      DWORD64 image_base = 0;
      auto* fn = RtlLookupFunctionEntry(c.Rip, &image_base, nullptr);
      if (!fn) break;
      void* handler_data = nullptr;
      DWORD64 establisher = 0;
      RtlVirtualUnwind(UNW_FLAG_NHANDLER, image_base, c.Rip, fn, &c, &handler_data, &establisher, nullptr);
    }
  }
  {
    std::lock_guard lock(g_log_mutex);
    for (const std::string& c : g_command_log) REXLOG_ERROR("[crash_report] command {}", c);
  }
  for (const BpState& b : Breakpoints()) {
    REXLOG_ERROR("[crash_report] breakpoint DR{} {} {:08X} hits {}", b.slot, b.kind == BpKind::kTrace ? "trace" : "watch",
                 b.guest, b.hits);
  }
  static auto fn = DllExport<int (*)(HistoryRecord*, int)>("bf3_dev_history");
  if (!fn) fn = DllExport<int (*)(HistoryRecord*, int)>("bf3_dev_history");
  std::vector<HistoryRecord> h(3000);
  h.resize(fn ? size_t(fn(h.data(), int(h.size()))) : 0);
  for (size_t i = 0; i < h.size(); ++i) {
    if (h.size() - i > 30 && (h.size() - i) % 25) continue;  // every frame for the last 30, then every 25th
    const HistoryRecord& r = h[i];
    REXLOG_ERROR("[crash_report] frame {} t {:.3f} {:.1f} ms soldier {:08X}/{:08X} pos ({:.2f}, {:.2f}, {:.2f}) "
                 "aim {:.3f}/{:.3f} hp {:.1f}/{:.1f}",
                 r.frame, r.uptime_s, r.frame_ms, r.soldier, r.server_soldier, r.pos[0], r.pos[1], r.pos[2], r.aim_yaw,
                 r.aim_pitch, r.health, r.server_health);
  }
  REXLOG_ERROR("[crash_report] end");
  rex::FlushLogging();
}

// Guest faults and FATALs (the same log lines crash_trace.h reacts to). null_mutex: the report logs through the same
// loggers.
class CrashRecorderSink : public spdlog::sinks::base_sink<spdlog::details::null_mutex> {
 protected:
  void sink_it_(const spdlog::details::log_msg& msg) override {
    std::string_view text(msg.payload.data(), msg.payload.size());
    const bool fatal = msg.level >= spdlog::level::critical && text.find("[FATAL]") != std::string_view::npos;
    const bool access = text.find("Unhandled guest access violation") != std::string_view::npos;
    if ((!fatal && !access) || reporting_) return;
    reporting_ = true;
    LogCrashReport(text, nullptr);
    reporting_ = false;
  }
  void flush_() override {}

 private:
  static inline thread_local bool reporting_ = false;
};

// Native crashes nothing else handled (e.g. an unhandled exception in host code): report, then let the previous filter
// (or Windows) end the process.
LPTOP_LEVEL_EXCEPTION_FILTER g_previous_filter = nullptr;

LONG WINAPI CrashFilter(EXCEPTION_POINTERS* info) {
  LogCrashReport(fmt::format("native exception {:08X} at {}", info->ExceptionRecord->ExceptionCode,
                             AnyFrameName(info->ContextRecord->Rip)),
                 info->ContextRecord);
  return g_previous_filter ? g_previous_filter(info) : EXCEPTION_CONTINUE_SEARCH;
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
    const uint64_t seq = LogCommandStart(line);
    try {
      reply = it->second.run(args);
    } catch (const std::exception& e) {
      reply = Fail(std::string("exception: ") + e.what());
    }
    LogCommandEnd(seq, reply.ok);
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
  rex::AddSink(std::make_shared<CrashRecorderSink>());
  g_previous_filter = SetUnhandledExceptionFilter(CrashFilter);
  std::thread(Listen, port).detach();
}

}  // namespace bf3::dev

#else
namespace bf3::dev {
void Install() {}
}  // namespace bf3::dev
#endif
