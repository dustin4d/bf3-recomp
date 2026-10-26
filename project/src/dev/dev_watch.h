// Debug API write watch (see dev_watch.cpp). Development builds only.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace bf3::dev {

struct WatchHit {
  uint32_t tid = 0;
  std::string value_hex;            // the watched bytes after the write (big-endian guest bytes)
  std::vector<std::string> frames;  // host stack, <module>+0x<RVA>, innermost first
};

struct WatchState {
  uint32_t guest = 0, len = 0;
  bool active = false;
  int hits = 0;  // writes seen (the first max_hits are recorded)
  std::vector<WatchHit> records;
};

// Watches `len` bytes at a guest heap address for `seconds` or until `max_hits` writes; replaces any earlier watch.
bool StartWatch(uint32_t guest_addr, uint32_t len, int max_hits, uint32_t seconds, std::string& error);
void StopWatch();
WatchState GetWatch();

}  // namespace bf3::dev
