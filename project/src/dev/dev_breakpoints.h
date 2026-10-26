// Debug API hardware breakpoints (see dev_breakpoints.cpp). Development builds only.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace bf3::dev {

enum class BpKind { kWrite, kReadWrite, kTrace };

struct BpHit {
  uint32_t tid = 0;
  double t = 0;                     // seconds since the breakpoint was set
  std::string value_hex;            // watches: the watched bytes after the access (big-endian guest bytes)
  std::vector<std::string> frames;  // host stack, <module>+0x<RVA>, innermost first
  // Traces: guest registers at entry and on return.
  uint32_t lr = 0;
  uint64_t r[8] = {};               // r3..r10
  double f[4] = {};                 // f1..f4
  bool returned = false;
  uint64_t ret_r3 = 0;
  double ret_f1 = 0;
  double duration_us = 0;
};

struct BpState {
  int slot = -1;  // DR0-DR3
  BpKind kind = BpKind::kWrite;
  uint32_t guest = 0, len = 0;
  bool active = false;
  int hits = 0, max_hits = 0;
  std::vector<BpHit> records;
};

// Data watch on a guest heap address (writes, or reads and writes) for `seconds` or `max_hits` accesses.
// Returns the slot, -1 (with `error`) if none is free or the address can't be watched.
int AddWatch(uint32_t guest_addr, uint32_t len, bool reads, int max_hits, uint32_t seconds, std::string& error);
// Execution breakpoint on a recompiled guest function (host entry point `host_func`): records the calls.
int AddTrace(uint32_t guest_addr, void* host_func, int max_hits, uint32_t seconds, std::string& error);
// Removes one slot, or all (-1).
void RemoveBreakpoint(int slot);
std::vector<BpState> Breakpoints();

}  // namespace bf3::dev
