// Debug API tools, game side (development builds only: BF3_DEV_TOOLS). bf3.exe's server forwards these commands
// through bf3_dev_query (src/dev/dev_shared.h); each reply is JSON members for the reply object.
//
//   call <addr|sub_X> [r3..r10=<v>] [f1..f4=<v>] [scratch=<n>] [scratchhex=<hex>] [out=<n>]
//       Runs a BF.Main guest function on the main thread with those registers and returns r3, r4, f1. Values are
//       decimal, 0x-hex or $scratch[+<n>]: a 16-byte aligned buffer on the guest stack below the caller's frame (up to
//       8 KB, zeroed or filled from scratchhex), whose first `out` bytes come back afterwards.
//   scan new <u32|f32|u16|u8> <value|any> [tol] [<lo>-<hi>]   start a narrowing search (any: a range up to 64 MB)
//   scan next <eq|ne|gt|lt|incby|decby> <v> [tol] | changed | unchanged | inc | dec
//   scan list [n=50]
//   snap <name> <addr> <len>   /   diff <name> [max=200]       what changed in a region since the snapshot
//   ptrpath <root> <target> [depth=3] [span=0x800] [max=20] [vt]   pointer paths from root to target (in-process BFS)
//   whatis <addr>      code (containing function), vtable / type info / object of a class (Frostbite getType),
//                      inside an object (nearest typed object start before it), string, image data
//   messages on [<hash>,...] | off | read [max=500] | stats [n=40]   the message dispatcher (level_boot.cpp)
//   pause | resume | step [n=1] [frames|ticks] | timescale [x]
//
// Class names: a Frostbite object's vtable +0 is getType(), "lis rX,hi; addi r3,rX,lo; blr" returning its type info
// (e.g. sub_837C6930 -> 0x8418953C ServerKillCounterEntity); type info +0 -> member record, whose +0 is the name.
// Time: pause holds the main thread at the frame boundary (the server and client simulation, rendering submission and
// input all stop) and slows the guest clock to 1% meanwhile, so resuming doesn't jump the game time; queued API work
// still runs. step lets n frames (or until n more local simulation ticks) through. timescale sets the guest clock speed
// (rex::chrono::Clock guest time scalar, 0.1-4).

#include "bf3_pch.h"

#include "dev_internal.h"
#include "mouse_aim.h"

#include <windows.h>

#include <rex/chrono/clock.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using namespace bf3_dev;

namespace {

using Clock = std::chrono::steady_clock;
const Clock::time_point g_start = Clock::now();

// ---- JSON members --------------------------------------------------------------------------------------------------

std::string Quote(std::string_view s) {
  std::string out = "\"";
  for (unsigned char c : s) {
    if (c == '"' || c == '\\') {
      out += '\\';
      out += char(c);
    } else if (c < 0x20) {
      out += fmt::format("\\u{:04x}", c);
    } else {
      out += char(c);
    }
  }
  return out + "\"";
}

std::string Num(double v) { return std::isfinite(v) ? fmt::format("{:.7g}", v) : "null"; }
std::string HexJ(uint32_t v) { return fmt::format("\"0x{:08X}\"", v); }

class Members {
 public:
  Members& Raw(std::string_view key, std::string_view json) {
    if (!body_.empty()) body_ += ',';
    body_ += Quote(key);
    body_ += ':';
    body_ += json;
    return *this;
  }
  Members& Str(std::string_view k, std::string_view v) { return Raw(k, Quote(v)); }
  Members& Number(std::string_view k, double v) { return Raw(k, Num(v)); }
  Members& Int(std::string_view k, int64_t v) { return Raw(k, std::to_string(v)); }
  Members& Bool(std::string_view k, bool v) { return Raw(k, v ? "true" : "false"); }
  Members& Addr(std::string_view k, uint32_t v) { return Raw(k, HexJ(v)); }
  std::string Object() const { return "{" + body_ + "}"; }
  const std::string& body() const { return body_; }

 private:
  std::string body_;
};

struct Error {
  std::string text;
};

// ---- parsing -------------------------------------------------------------------------------------------------------

// A guest address: hex with or without 0x, or sub_XXXXXXXX.
bool ParseAddr(std::string s, uint32_t& out) {
  if (s.rfind("sub_", 0) == 0) s = s.substr(4);
  if (s.rfind("0x", 0) == 0 || s.rfind("0X", 0) == 0) s = s.substr(2);
  if (s.empty() || s.size() > 8) return false;
  char* end = nullptr;
  const unsigned long long v = std::strtoull(s.c_str(), &end, 16);
  if (!end || *end) return false;
  out = uint32_t(v);
  return true;
}

// A number: 0x-hex or decimal (negative allowed).
bool ParseNum(const std::string& s, int64_t& out) {
  if (s.empty()) return false;
  char* end = nullptr;
  if (s.rfind("0x", 0) == 0 || s.rfind("-0x", 0) == 0) {
    const bool neg = s[0] == '-';
    out = int64_t(std::strtoull(s.c_str() + (neg ? 3 : 2), &end, 16));
    if (neg) out = -out;
  } else {
    out = std::strtoll(s.c_str(), &end, 10);
  }
  return end && *end == 0;
}

bool ParseDouble(const std::string& s, double& out) {
  char* end = nullptr;
  out = std::strtod(s.c_str(), &end);
  return !s.empty() && end && *end == 0 && std::isfinite(out);
}

bool ParseRange(const std::string& s, uint32_t& lo, uint32_t& hi) {
  const size_t dash = s.find('-');
  if (dash == std::string::npos || dash == 0) return false;
  uint32_t l, h;
  if (!ParseAddr(s.substr(0, dash), l) || !ParseAddr(s.substr(dash + 1), h) || l >= h) return false;
  lo = l, hi = h;
  return true;
}

std::string HexBytes(const uint8_t* p, size_t n) {
  std::string s;
  s.reserve(n * 2);
  for (size_t i = 0; i < n; ++i) s += fmt::format("{:02x}", p[i]);
  return s;
}

bool InImage(uint32_t a) { return a >= 0x82000000u && a < 0x84300000u; }
bool InHeap(uint32_t a) { return a >= 0x40000000u && a < 0xA0000000u; }
bool InCode(uint32_t a) { return uint64_t(a - uint32_t(REX_CODE_BASE)) < REX_CODE_SIZE; }

uint32_t U32(uint8_t* base, uint32_t a) { return Readable(base, a, 4) ? REX_LOAD_U32(a) : 0; }

// Visits the committed, readable regions of [lo, hi) as (start, end) guest ranges.
template <typename Fn>
void ForEachRegion(uint8_t* base, uint32_t lo, uint32_t hi, Fn fn) {
  uint64_t addr = lo;
  while (addr < hi) {
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery(base + addr, &mbi, sizeof(mbi))) break;
    const uint64_t end =
        std::min<uint64_t>(hi, uint64_t(static_cast<uint8_t*>(mbi.BaseAddress) - base) + mbi.RegionSize);
    if (mbi.State == MEM_COMMIT && !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) fn(uint32_t(addr), uint32_t(end));
    addr = std::max(addr + 1, end);
  }
}

// ---- functions and classes -----------------------------------------------------------------------------------------

// The launcher (Default.xex: image 82000000+C0000, code 82010000+C650) has its own dispatch table after its image,
// so its functions can be traced too.
constexpr uint32_t kLauncherCode = 0x82010000u, kLauncherCodeSize = 0xC650u, kLauncherTable = 0x820C0000u;

PPCFunc* HostFunc(uint8_t* base, uint32_t addr) {
  if (InCode(addr)) return REX_LOOKUP_FUNC(base, addr);
  if (addr - kLauncherCode < kLauncherCodeSize) {
    return *reinterpret_cast<PPCFunc**>(base + kLauncherTable + uint64_t(addr - kLauncherCode) * 2);
  }
  return nullptr;
}

// The registered function that contains a code address (the dispatch table has an entry at each function start).
uint32_t FunctionStart(uint8_t* base, uint32_t addr) {
  for (uint32_t a = addr & ~3u, n = 0; InCode(a) && n < 0x10000; a -= 4, ++n) {
    if (REX_LOOKUP_FUNC(base, a)) return a;
  }
  return 0;
}

// getType() at vtable +0: "lis rX,hi; addi r3,rX,lo; blr" -> the type info address; 0 if the vtable isn't one.
uint32_t TypeFromVtable(uint8_t* base, uint32_t vtable) {
  if (!InImage(vtable)) return 0;
  const uint32_t fn = U32(base, vtable);
  if (!InCode(fn) || !Readable(base, fn, 12)) return 0;
  const uint32_t i0 = REX_LOAD_U32(fn), i1 = REX_LOAD_U32(fn + 4), i2 = REX_LOAD_U32(fn + 8);
  if ((i0 & 0xFC1F0000u) != 0x3C000000u || i2 != 0x4E800020u) return 0;  // lis rX,hi (addis rX,0,hi) ... blr
  const uint32_t rx = (i0 >> 21) & 31;
  if ((i1 & 0xFFFF0000u) != (0x38600000u | (rx << 16))) return 0;      // addi r3,rX,lo
  return (i0 << 16) + uint32_t(int32_t(int16_t(i1 & 0xFFFF)));
}

// Type info -> class name (type info +0 -> member record, +0 -> name); "" if it doesn't look like one.
std::string TypeName(uint8_t* base, uint32_t type) {
  if (!InImage(type)) return {};
  const uint32_t record = U32(base, type);
  if (!InImage(record)) return {};
  const std::string name = GuestString(base, U32(base, record), 96);
  return name.size() >= 2 ? name : std::string();
}

std::string ClassOf(uint8_t* base, uint32_t object) {
  if (!Readable(base, object, 4)) return {};
  return TypeName(base, TypeFromVtable(base, REX_LOAD_U32(object)));
}

// ---- call ----------------------------------------------------------------------------------------------------------

std::optional<Error> Call(const std::vector<std::string>& args, Members& m) {
  uint8_t* base = g_base;
  uint32_t addr;
  if (args.size() < 2 || !ParseAddr(args[1], addr)) return Error{"usage: call <addr|sub_X> [r3=..] [f1=..] ..."};
  PPCFunc* fn = HostFunc(base, addr);
  if (!fn) return Error{fmt::format("no BF.Main function at {:08X}", addr)};
  struct Spec {
    std::array<std::optional<std::string>, 11> r;
    std::array<std::optional<double>, 5> f;
    std::vector<uint8_t> scratch;
    size_t out = 0;
  };
  auto spec = std::make_shared<Spec>();
  for (size_t i = 2; i < args.size(); ++i) {
    const size_t eq = args[i].find('=');
    if (eq == std::string::npos) return Error{"arguments are name=value: '" + args[i] + "'"};
    const std::string k = args[i].substr(0, eq), v = args[i].substr(eq + 1);
    int64_t n;
    double d;
    if (k.size() >= 2 && k[0] == 'r' && std::isdigit(uint8_t(k[1]))) {
      const int idx = std::atoi(k.c_str() + 1);
      if (idx < 3 || idx > 10) return Error{"registers r3-r10"};
      spec->r[idx] = v;
    } else if (k.size() == 2 && k[0] == 'f' && k[1] >= '1' && k[1] <= '4') {
      if (!ParseDouble(v, d)) return Error{"bad float '" + v + "'"};
      spec->f[k[1] - '0'] = d;
    } else if (k == "scratch") {
      if (!ParseNum(v, n) || n < 0 || n > 0x2000) return Error{"scratch=<bytes, up to 8192>"};
      spec->scratch.assign(size_t(n), 0);
    } else if (k == "scratchhex") {
      if (v.size() % 2 || v.size() > 0x4000) return Error{"scratchhex: an even number of hex digits, up to 8 KB"};
      spec->scratch.clear();
      for (size_t j = 0; j < v.size(); j += 2) spec->scratch.push_back(uint8_t(std::strtoul(v.substr(j, 2).c_str(), nullptr, 16)));
    } else if (k == "out") {
      if (!ParseNum(v, n) || n < 0 || n > 0x2000) return Error{"out=<bytes, up to 8192>"};
      spec->out = size_t(n);
    } else {
      return Error{"unknown argument '" + k + "'"};
    }
  }
  if (spec->out > spec->scratch.size()) spec->scratch.resize(spec->out, 0);
  // Register values may name the scratch buffer, whose address is only known on the main thread.
  for (size_t i = 3; i <= 10; ++i) {
    if (!spec->r[i]) continue;
    const std::string& v = *spec->r[i];
    int64_t n;
    if (v.rfind("$scratch", 0) == 0) {
      if (v.size() > 8 && (v[8] != '+' || !ParseNum(v.substr(9), n))) return Error{"$scratch[+<n>]"};
      if (spec->scratch.empty()) spec->scratch.assign(0x100, 0);
    } else if (!ParseNum(v, n)) {
      return Error{"bad value '" + v + "'"};
    }
  }
  struct Result {
    uint64_t r3 = 0, r4 = 0;
    double f1 = 0;
    uint32_t scratch = 0;
    std::vector<uint8_t> out;
  };
  auto result = std::make_shared<Result>();
  const bool ran = RunOnMainThread([spec, result, fn](PPCContext& ctx, uint8_t* base) {
    const PPCContext saved = ctx;
    const uint32_t sp = (saved.r1.u32 - 0x3000) & ~0xFu;
    const uint32_t scratch = sp + 0x800;
    if (!spec->scratch.empty()) std::memcpy(base + scratch, spec->scratch.data(), spec->scratch.size());
    PPCRegister* regs[11] = {nullptr, nullptr, nullptr, &ctx.r3, &ctx.r4, &ctx.r5, &ctx.r6, &ctx.r7, &ctx.r8, &ctx.r9,
                             &ctx.r10};
    for (size_t i = 3; i <= 10; ++i) {
      if (!spec->r[i]) continue;
      const std::string& v = *spec->r[i];
      int64_t n = 0;
      if (v.rfind("$scratch", 0) == 0) {
        if (v.size() > 8) ParseNum(v.substr(9), n);
        regs[i]->u64 = uint64_t(scratch + uint32_t(n));
      } else {
        ParseNum(v, n);
        regs[i]->u64 = uint64_t(n);
      }
    }
    PPCRegister* fregs[5] = {nullptr, &ctx.f1, &ctx.f2, &ctx.f3, &ctx.f4};
    for (size_t i = 1; i <= 4; ++i) {
      if (spec->f[i]) fregs[i]->f64 = *spec->f[i];
    }
    ctx.r1.u64 = sp;
    fn(ctx, base);
    result->r3 = ctx.r3.u64;
    result->r4 = ctx.r4.u64;
    result->f1 = ctx.f1.f64;
    result->scratch = scratch;
    if (spec->out) result->out.assign(base + scratch, base + scratch + spec->out);
    ctx = saved;
  });
  if (!ran) return Error{"the main thread didn't run a frame in time"};
  m.Addr("r3", uint32_t(result->r3)).Int("r3_int", int64_t(int32_t(result->r3))).Addr("r4", uint32_t(result->r4))
      .Number("f1", result->f1);
  if (!spec->scratch.empty()) m.Addr("scratch", result->scratch);
  if (spec->out) m.Str("out", HexBytes(result->out.data(), result->out.size()));
  return std::nullopt;
}

// ---- scan (narrowing search) ---------------------------------------------------------------------------------------

struct ScanState {
  int width = 4;
  bool is_float = false;
  std::vector<uint32_t> addrs, values;
} g_scan;
std::mutex g_scan_mutex;

uint32_t ReadRaw(uint8_t* base, uint32_t a, int width) {
  switch (width) {
    case 1: return REX_LOAD_U8(a);
    case 2: return REX_LOAD_U16(a);
    default: return REX_LOAD_U32(a);
  }
}

double AsValue(uint32_t raw, int width, bool is_float) {
  if (is_float) return double(std::bit_cast<float>(raw));
  return width == 4 ? double(int64_t(int32_t(raw))) : double(raw);
}

std::string ValueJson(uint32_t raw, int width, bool is_float) { return Num(AsValue(raw, width, is_float)); }

std::optional<Error> ScanCmd(const std::vector<std::string>& args, Members& m) {
  uint8_t* base = g_base;
  std::lock_guard lock(g_scan_mutex);
  ScanState& sc = g_scan;
  const std::string sub = args.size() > 1 ? args[1] : "list";
  if (sub == "new") {
    if (args.size() < 4) return Error{"usage: scan new <u32|f32|u16|u8> <value|any> [tol] [lo-hi]"};
    const std::string& kind = args[2];
    const int width = kind == "u8" ? 1 : kind == "u16" ? 2 : (kind == "u32" || kind == "f32") ? 4 : 0;
    if (!width) return Error{"kind must be u32, f32, u16 or u8"};
    const bool any = args[3] == "any";
    double value = 0, tol = kind == "f32" ? 0.001 : 0;
    if (!any && !ParseDouble(args[3], value)) return Error{"bad value '" + args[3] + "'"};
    uint32_t lo = 0x40000000, hi = 0xA0000000;
    for (size_t i = 4; i < args.size(); ++i) {
      if (!ParseRange(args[i], lo, hi) && !ParseDouble(args[i], tol)) return Error{"bad argument '" + args[i] + "'"};
    }
    if (any && hi - lo > 64u << 20) return Error{"'any' needs a range of at most 64 MB"};
    ScanState next;
    next.width = width;
    next.is_float = kind == "f32";
    ForEachRegion(base, lo, hi, [&](uint32_t start, uint32_t end) {
      for (uint64_t a = (uint64_t(start) + width - 1) & ~uint64_t(width - 1); a + width <= end; a += width) {
        const uint32_t raw = ReadRaw(base, uint32_t(a), width);
        if (any || std::fabs(AsValue(raw, width, next.is_float) - value) <= tol) {
          next.addrs.push_back(uint32_t(a));
          next.values.push_back(raw);
        }
      }
    });
    sc = std::move(next);
  } else if (sub == "next") {
    if (sc.addrs.empty()) return Error{"no scan (scan new first)"};
    if (args.size() < 3) return Error{"usage: scan next <op> [value] [tol]"};
    const std::string& op = args[2];
    double v = 0, tol = sc.is_float ? 0.001 : 0;
    const bool needs_value = op == "eq" || op == "ne" || op == "gt" || op == "lt" || op == "incby" || op == "decby";
    if (needs_value && (args.size() < 4 || !ParseDouble(args[3], v))) return Error{op + " needs a value"};
    if (args.size() > size_t(needs_value ? 4 : 3) && !ParseDouble(args[needs_value ? 4 : 3], tol)) {
      return Error{"bad tolerance"};
    }
    size_t kept = 0;
    for (size_t i = 0; i < sc.addrs.size(); ++i) {
      const uint32_t a = sc.addrs[i];
      if (!Readable(base, a, uint32_t(sc.width))) continue;
      const uint32_t raw = ReadRaw(base, a, sc.width);
      const double now = AsValue(raw, sc.width, sc.is_float), was = AsValue(sc.values[i], sc.width, sc.is_float);
      bool keep;
      if (op == "eq") keep = std::fabs(now - v) <= tol;
      else if (op == "ne") keep = std::fabs(now - v) > tol;
      else if (op == "gt") keep = now > v;
      else if (op == "lt") keep = now < v;
      else if (op == "changed") keep = raw != sc.values[i];
      else if (op == "unchanged") keep = raw == sc.values[i];
      else if (op == "inc") keep = now > was;
      else if (op == "dec") keep = now < was;
      else if (op == "incby") keep = std::fabs(now - was - v) <= tol;
      else if (op == "decby") keep = std::fabs(was - now - v) <= tol;
      else return Error{"op must be eq, ne, gt, lt, changed, unchanged, inc, dec, incby or decby"};
      if (keep) {
        sc.addrs[kept] = a;
        sc.values[kept] = raw;
        ++kept;
      }
    }
    sc.addrs.resize(kept);
    sc.values.resize(kept);
  } else if (sub != "list") {
    return Error{"usage: scan new|next|list"};
  }
  int64_t n = 50;
  if (sub == "list" && args.size() > 2 && !ParseNum(args[2], n)) return Error{"bad count"};
  std::string items = "[";
  for (size_t i = 0; i < sc.addrs.size() && int64_t(i) < n; ++i) {
    const uint32_t raw = Readable(base, sc.addrs[i], uint32_t(sc.width)) ? ReadRaw(base, sc.addrs[i], sc.width) : 0;
    items += (i ? "," : "") + Members().Addr("addr", sc.addrs[i]).Raw("value", ValueJson(raw, sc.width, sc.is_float)).Object();
  }
  m.Int("count", int64_t(sc.addrs.size())).Raw("items", items + "]");
  return std::nullopt;
}

// ---- snap / diff ---------------------------------------------------------------------------------------------------

struct Region {
  uint32_t addr = 0;
  std::vector<uint8_t> bytes;
};
std::map<std::string, Region> g_snaps;
std::mutex g_snaps_mutex;

std::optional<Error> SnapCmd(const std::vector<std::string>& args, Members& m) {
  uint8_t* base = g_base;
  uint32_t addr;
  int64_t len;
  if (args.size() < 4 || !ParseAddr(args[2], addr) || !ParseNum(args[3], len) || len <= 0 || len > (4 << 20)) {
    return Error{"usage: snap <name> <addr> <len up to 4 MB>"};
  }
  if (!Readable(base, addr, uint32_t(len))) return Error{"not readable"};
  Region r{addr, std::vector<uint8_t>(base + addr, base + addr + len)};
  std::lock_guard lock(g_snaps_mutex);
  g_snaps[args[1]] = std::move(r);
  m.Str("name", args[1]).Addr("addr", addr).Int("len", len);
  return std::nullopt;
}

std::optional<Error> DiffCmd(const std::vector<std::string>& args, Members& m) {
  uint8_t* base = g_base;
  if (args.size() < 2) return Error{"usage: diff <name> [max=200]"};
  int64_t max = 200;
  if (args.size() > 2 && !ParseNum(args[2], max)) return Error{"bad max"};
  std::lock_guard lock(g_snaps_mutex);
  auto it = g_snaps.find(args[1]);
  if (it == g_snaps.end()) return Error{"no snapshot '" + args[1] + "'"};
  const Region& r = it->second;
  if (!Readable(base, r.addr, uint32_t(r.bytes.size()))) return Error{"region no longer readable"};
  std::string items = "[";
  int64_t changed = 0;
  for (size_t off = 0; off + 4 <= r.bytes.size(); off += 4) {
    const uint8_t* now = base + r.addr + off;
    if (std::memcmp(now, r.bytes.data() + off, 4) == 0) continue;
    if (changed++ >= max) continue;
    uint32_t was_raw, now_raw;
    std::memcpy(&was_raw, r.bytes.data() + off, 4);
    std::memcpy(&now_raw, now, 4);
    was_raw = _byteswap_ulong(was_raw), now_raw = _byteswap_ulong(now_raw);
    items += (changed > 1 ? "," : "") + Members()
                                            .Raw("off", fmt::format("\"0x{:X}\"", off))
                                            .Raw("was", HexJ(was_raw))
                                            .Raw("now", HexJ(now_raw))
                                            .Number("was_f", std::bit_cast<float>(was_raw))
                                            .Number("now_f", std::bit_cast<float>(now_raw))
                                            .Object();
  }
  m.Str("name", args[1]).Addr("addr", r.addr).Int("changed_words", changed).Raw("items", items + "]");
  return std::nullopt;
}

// ---- ptrpath -------------------------------------------------------------------------------------------------------

std::optional<Error> PtrPathCmd(const std::vector<std::string>& args, Members& m) {
  uint8_t* base = g_base;
  uint32_t root, target;
  if (args.size() < 3 || !ParseAddr(args[1], root) || !ParseAddr(args[2], target)) {
    return Error{"usage: ptrpath <root> <target> [depth=3] [span=0x800] [max=20] [vt]"};
  }
  int64_t depth = 3, span = 0x800, max = 20;
  bool vt = false;
  std::vector<int64_t*> positional = {&depth, &span, &max};
  size_t pi = 0;
  for (size_t i = 3; i < args.size(); ++i) {
    if (args[i] == "vt") {
      vt = true;
    } else if (pi < positional.size() && ParseNum(args[i], *positional[pi])) {
      ++pi;
    } else {
      return Error{"bad argument '" + args[i] + "'"};
    }
  }
  depth = std::clamp<int64_t>(depth, 0, 6), span = std::clamp<int64_t>(span, 4, 0x4000) & ~3;
  struct Node {
    uint32_t obj;
    int parent;
    uint32_t off;  // offset in the parent that points here
    int depth;
  };
  // Committed heap regions, looked up by binary search (a VirtualQuery per object made big searches take seconds).
  std::vector<std::pair<uint32_t, uint32_t>> regions;
  ForEachRegion(base, 0x40000000, 0xA0000000, [&](uint32_t start, uint32_t end) {
    if (!regions.empty() && regions.back().second == start) {
      regions.back().second = end;
    } else {
      regions.emplace_back(start, end);
    }
  });
  auto readable = [&](uint32_t a, uint32_t len) {
    auto it = std::upper_bound(regions.begin(), regions.end(), std::make_pair(a, 0xFFFFFFFFu));
    if (it == regions.begin()) return false;
    --it;
    return a >= it->first && uint64_t(a) + len <= it->second;
  };
  std::vector<Node> nodes{{root, -1, 0, 0}};
  std::unordered_set<uint32_t> seen{root};
  std::string paths = "[";
  int found = 0;
  constexpr size_t kMaxNodes = 400000;
  for (size_t q = 0; q < nodes.size() && found < max; ++q) {
    const Node n = nodes[q];
    if (!readable(n.obj, uint32_t(span))) continue;
    if (target >= n.obj && target - n.obj < uint32_t(span)) {
      std::vector<uint32_t> offs{target - n.obj};
      for (int p = int(q); nodes[p].parent >= 0; p = nodes[p].parent) offs.push_back(nodes[p].off);
      std::reverse(offs.begin(), offs.end());
      std::string path = "[";
      for (size_t i = 0; i < offs.size(); ++i) path += (i ? "," : "") + fmt::format("\"0x{:X}\"", offs[i]);
      paths += (found++ ? "," : "") + path + "]";
    }
    if (n.depth >= depth) continue;
    for (uint32_t off = 0; off < uint32_t(span) && nodes.size() < kMaxNodes; off += 4) {
      const uint32_t p = REX_LOAD_U32(n.obj + off);
      if (!InHeap(p) || seen.count(p)) continue;
      if (vt && !(readable(p, 4) && InImage(REX_LOAD_U32(p)))) continue;
      seen.insert(p);
      nodes.push_back({p, int(q), off, n.depth + 1});
    }
  }
  m.Addr("root", root).Addr("target", target).Int("visited", int64_t(nodes.size())).Raw("paths", paths + "]");
  return std::nullopt;
}

// ---- whatis --------------------------------------------------------------------------------------------------------

std::optional<Error> WhatIsCmd(const std::vector<std::string>& args, Members& m) {
  uint8_t* base = g_base;
  uint32_t a;
  if (args.size() < 2 || !ParseAddr(args[1], a)) return Error{"usage: whatis <addr>"};
  m.Addr("addr", a);
  if (!Readable(base, a, 1)) {
    m.Str("kind", "unmapped");
    return std::nullopt;
  }
  if (InCode(a)) {
    const uint32_t start = FunctionStart(base, a);
    m.Str("kind", "code");
    if (start) m.Str("function", fmt::format("sub_{:08X}", start)).Int("offset", a - start);
    return std::nullopt;
  }
  if (InImage(a)) {
    if (const uint32_t type = TypeFromVtable(base, a); type) {
      m.Str("kind", "vtable").Str("class", TypeName(base, type)).Addr("type_info", type);
    } else if (const std::string name = TypeName(base, a); !name.empty()) {
      m.Str("kind", "type_info").Str("class", name);
    } else if (const std::string s = GuestString(base, a, 96); s.size() >= 4) {
      m.Str("kind", "string").Str("text", s);
    } else if (InCode(U32(base, a))) {
      m.Str("kind", "image_data").Str("points_to", fmt::format("sub_{:08X}", FunctionStart(base, U32(base, a))));
    } else {
      m.Str("kind", "image_data");
    }
    return std::nullopt;
  }
  // Heap: an object of a class, inside one, or text.
  if (Readable(base, a, 4)) {
    if (const std::string cls = ClassOf(base, a); !cls.empty()) {
      m.Str("kind", "object").Str("class", cls).Addr("vtable", REX_LOAD_U32(a));
      return std::nullopt;
    }
  }
  for (uint32_t off = 4; off <= 0x4000 && a >= off; off += 4) {
    const uint32_t start = (a & ~3u) - off + ((a & 3u) ? 4 : 0);
    if (!Readable(base, start, 4)) break;
    if (const std::string cls = ClassOf(base, start); !cls.empty()) {
      m.Str("kind", "inside_object").Str("class", cls).Addr("object", start).Raw("offset",
                                                                                     fmt::format("\"0x{:X}\"", a - start));
      return std::nullopt;
    }
  }
  if (const std::string s = GuestString(base, a, 96); s.size() >= 4) {
    m.Str("kind", "string").Str("text", s);
  } else {
    m.Str("kind", "heap");
  }
  return std::nullopt;
}

// ---- messages ------------------------------------------------------------------------------------------------------

struct Msg {
  uint64_t seq;
  double t;
  uint32_t category, type, lr, addr;
};
std::atomic<bool> g_msg_on{false};
std::mutex g_msg_mutex;
std::vector<uint32_t> g_msg_filter;  // category or type hashes; empty = all
std::deque<Msg> g_msgs;
uint64_t g_msg_seq = 0, g_msg_read = 0;
std::unordered_map<uint64_t, uint64_t> g_msg_counts;

std::optional<Error> MessagesCmd(const std::vector<std::string>& args, Members& m) {
  const std::string sub = args.size() > 1 ? args[1] : "read";
  std::lock_guard lock(g_msg_mutex);
  if (sub == "on") {
    g_msg_filter.clear();
    if (args.size() > 2) {
      size_t start = 0;
      const std::string& list = args[2];
      while (start <= list.size()) {
        const size_t comma = list.find(',', start);
        uint32_t h;
        if (!ParseAddr(list.substr(start, comma - start), h)) return Error{"filter: comma-separated hex hashes"};
        g_msg_filter.push_back(h);
        if (comma == std::string::npos) break;
        start = comma + 1;
      }
    }
    g_msgs.clear();
    g_msg_counts.clear();
    g_msg_read = g_msg_seq;
    g_msg_on = true;
    m.Bool("on", true).Int("filters", int64_t(g_msg_filter.size()));
  } else if (sub == "off") {
    g_msg_on = false;
    m.Bool("on", false);
  } else if (sub == "read") {
    int64_t max = 500;
    if (args.size() > 2 && !ParseNum(args[2], max)) return Error{"bad max"};
    std::string items = "[";
    int64_t n = 0;
    for (const Msg& e : g_msgs) {
      if (e.seq <= g_msg_read || n >= max) continue;
      items += (n++ ? "," : "") + Members()
                                      .Int("seq", int64_t(e.seq))
                                      .Number("t", e.t)
                                      .Addr("category", e.category)
                                      .Addr("type", e.type)
                                      .Addr("lr", e.lr)
                                      .Addr("msg", e.addr)
                                      .Object();
      g_msg_read = e.seq;
    }
    m.Bool("on", g_msg_on.load()).Int("count", n).Raw("items", items + "]");
  } else if (sub == "stats") {
    int64_t top = 40;
    if (args.size() > 2 && !ParseNum(args[2], top)) return Error{"bad count"};
    std::vector<std::pair<uint64_t, uint64_t>> v(g_msg_counts.begin(), g_msg_counts.end());
    std::sort(v.begin(), v.end(), [](auto& x, auto& y) { return x.second > y.second; });
    std::string items = "[";
    for (size_t i = 0; i < v.size() && int64_t(i) < top; ++i) {
      items += (i ? "," : "") + Members()
                                    .Addr("category", uint32_t(v[i].first >> 32))
                                    .Addr("type", uint32_t(v[i].first))
                                    .Int("count", int64_t(v[i].second))
                                    .Object();
    }
    m.Bool("on", g_msg_on.load()).Int("kinds", int64_t(v.size())).Raw("items", items + "]");
  } else {
    return Error{"usage: messages on [hash,...] | off | read [max] | stats [n]"};
  }
  return std::nullopt;
}

// ---- time controls -------------------------------------------------------------------------------------------------

constexpr double kFrozenScale = 0.01;  // guest clock speed while paused (0 would divide by zero in the runtime clock)
std::atomic<bool> g_paused{false};
std::atomic<int> g_step_frames{0};
std::atomic<uint64_t> g_step_until_tick{0};
std::atomic<float> g_timescale{1.0f};
std::atomic<bool> g_holding{false};  // the main thread is held right now
int g_tick_step_frames = 0;           // frames run so far for a tick step (main thread)

std::optional<Error> TimeCmd(const std::vector<std::string>& args, Members& m) {
  const std::string& cmd = args[0];
  if (cmd == "pause") {
    g_paused = true;
  } else if (cmd == "resume") {
    g_step_frames = 0;
    g_step_until_tick = 0;
    g_paused = false;
  } else if (cmd == "step") {
    int64_t n = 1;
    if (args.size() > 1 && (!ParseNum(args[1], n) || n < 1 || n > 100000)) return Error{"usage: step [n] [frames|ticks]"};
    const bool ticks = args.size() > 2 && args[2] == "ticks";
    if (args.size() > 2 && !ticks && args[2] != "frames") return Error{"unit must be frames or ticks"};
    if (ticks && !bf3_main::LocalAimTicks()) return Error{"ticks are counted by the local soldier's aiming update: spawn first"};
    g_paused = true;
    if (ticks) {
      g_step_until_tick = bf3_main::LocalAimTicks() + uint64_t(n);
    } else {
      g_step_frames = int(n);
    }
  } else if (cmd == "timescale") {
    if (args.size() > 1) {
      double x;
      if (!ParseDouble(args[1], x) || x < 0.1 || x > 4.0) return Error{"timescale 0.1-4"};
      g_timescale = float(x);
      if (!g_holding) rex::chrono::Clock::set_guest_time_scalar(x);
    }
  } else {
    return Error{"unknown time command"};
  }
  m.Bool("paused", g_paused.load())
      .Bool("holding", g_holding.load())
      .Number("timescale", g_timescale.load())
      .Number("clock_scalar", rex::chrono::Clock::guest_time_scalar())
      .Int("ticks", int64_t(bf3_main::LocalAimTicks()));
  return std::nullopt;
}

// ---- history (crash recorder) --------------------------------------------------------------------------------------

std::mutex g_hist_mutex;
std::array<bf3::dev::HistoryRecord, 3000> g_hist;  // ~10 s at 300 fps
size_t g_hist_count = 0;

}  // namespace

namespace bf3_dev {

bool Paused() { return g_paused.load(); }
float Timescale() { return g_timescale.load(); }

void FrameControl(PPCContext& ctx, uint8_t* base) {
  if (!g_paused.load()) return;
  if (g_step_frames.load() > 0) {
    --g_step_frames;
    return;
  }
  if (const uint64_t until = g_step_until_tick.load(); until) {
    // Run frames until the local aiming update has ticked enough (at most 1000 frames, e.g. if the soldier died).
    if (bf3_main::LocalAimTicks() < until && ++g_tick_step_frames < 1000) return;
    g_step_until_tick = 0;
  }
  g_tick_step_frames = 0;
  rex::chrono::Clock::set_guest_time_scalar(kFrozenScale);
  g_holding = true;
  REXLOG_INFO("debug api: paused at the frame boundary");
  while (g_paused.load() && g_step_frames.load() == 0 && g_step_until_tick.load() == 0) {
    RunPending(ctx, base);
    Sleep(1);
  }
  g_holding = false;
  rex::chrono::Clock::set_guest_time_scalar(g_timescale.load());
  if (g_step_frames.load() > 0) --g_step_frames;  // the held frame is the first stepped one
}

void RecordHistory(const bf3::dev::Snapshot& s, float frame_ms) {
  bf3::dev::HistoryRecord r{};
  r.uptime_s = s.uptime_s;
  r.frame = uint32_t(s.frame);
  r.frame_ms = frame_ms;
  r.soldier = s.soldier;
  r.server_soldier = s.server_soldier;
  std::memcpy(r.pos, s.transform + 12, sizeof(r.pos));
  r.aim_yaw = s.aim_yaw;
  r.aim_pitch = s.aim_pitch;
  r.health = s.health;
  r.server_health = s.server_health;
  r.level_loaded = s.level_loaded;
  r.round_started = s.round_started;
  std::lock_guard lock(g_hist_mutex);
  g_hist[g_hist_count++ % g_hist.size()] = r;
}

void OnMessage(uint8_t* base, uint32_t msg, uint32_t lr) {
  if (!g_msg_on.load(std::memory_order_relaxed) || !Readable(base, msg, 12)) return;
  const uint32_t category = REX_LOAD_U32(msg + 4), type = REX_LOAD_U32(msg + 8);
  std::lock_guard lock(g_msg_mutex);
  if (!g_msg_filter.empty() &&
      std::find_if(g_msg_filter.begin(), g_msg_filter.end(), [&](uint32_t h) { return h == category || h == type; }) ==
          g_msg_filter.end()) {
    return;
  }
  ++g_msg_counts[uint64_t(category) << 32 | type];
  g_msgs.push_back({++g_msg_seq, std::chrono::duration<double>(Clock::now() - g_start).count(), category, type, lr, msg});
  if (g_msgs.size() > 8192) g_msgs.pop_front();
}

}  // namespace bf3_dev

extern "C" __declspec(dllexport) void* bf3_dev_host_func(uint32_t guest_addr) {
  return g_base ? reinterpret_cast<void*>(HostFunc(g_base, guest_addr)) : nullptr;
}

extern "C" __declspec(dllexport) int bf3_dev_history(bf3::dev::HistoryRecord* out, int capacity) {
  std::lock_guard lock(g_hist_mutex);
  const size_t n = std::min({g_hist_count, g_hist.size(), size_t(std::max(capacity, 0))});
  for (size_t i = 0; i < n; ++i) out[i] = g_hist[(g_hist_count - n + i) % g_hist.size()];
  return int(n);
}

extern "C" __declspec(dllexport) int bf3_dev_query(const char* line, char* out, int capacity) {
  auto write = [&](const std::string& text, bool ok) {
    if (capacity <= 0) return ok ? 0 : -1;
    const size_t n = std::min(text.size(), size_t(capacity - 1));
    std::memcpy(out, text.data(), n);
    out[n] = 0;
    return ok ? int(n) : -1;
  };
  if (!g_base) return write("BF.Main has not run a frame yet", false);
  const std::vector<std::string> args = Split(line);
  if (args.empty()) return write("empty command", false);
  const std::string& cmd = args[0];
  Members m;
  std::optional<Error> err;
  if (cmd == "call") err = Call(args, m);
  else if (cmd == "scan") err = ScanCmd(args, m);
  else if (cmd == "snap") err = SnapCmd(args, m);
  else if (cmd == "diff") err = DiffCmd(args, m);
  else if (cmd == "ptrpath") err = PtrPathCmd(args, m);
  else if (cmd == "whatis") err = WhatIsCmd(args, m);
  else if (cmd == "messages") err = MessagesCmd(args, m);
  else if (cmd == "pause" || cmd == "resume" || cmd == "step" || cmd == "timescale") err = TimeCmd(args, m);
  else err = Error{"unknown tool command '" + cmd + "'"};
  if (err) return write(err->text, false);
  if (m.body().size() >= size_t(capacity)) return write("reply too large", false);
  return write(m.body(), true);
}
