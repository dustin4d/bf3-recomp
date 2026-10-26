// Debug API, game side: what dev_state.cpp (snapshot, game commands) and dev_tools.cpp (call, memory tools, whatis,
// messages, time controls) share. Development builds only (BF3_DEV_TOOLS).
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "../dev/dev_shared.h"

namespace bf3_dev {

extern uint8_t* g_base;  // guest memory base, set by the first DevFrame

// Guest memory a host read can't fault on.
bool Readable(uint8_t* base, uint32_t addr, uint32_t length);
// A printable guest C string up to `max` chars ("" if unreadable or not text), read within the string's page.
std::string GuestString(uint8_t* base, uint32_t addr, size_t max);
float LoadF(uint8_t* base, uint32_t addr);
std::vector<std::string> Split(const char* line);

// Main thread work at the next frame boundary (DevFrame). RunOnMainThread waits for it (false on timeout: the game
// isn't running frames, so fn may still run later and must only touch what it owns).
void Queue(std::function<void(PPCContext&, uint8_t*)> fn);
bool RunOnMainThread(std::function<void(PPCContext&, uint8_t*)> fn, int timeout_ms = 3000);
// Runs the queued work now (main thread; also called while paused).
void RunPending(PPCContext& ctx, uint8_t* base);

// Guest memory search (see bf3_dev_find): kind 0 f32 within tol, 1 u32, 2 bytes.
int Find(int kind, const double* values, int count, double tol, uint32_t lo, uint32_t hi, uint32_t* out, int capacity);

// dev_tools.cpp
void FrameControl(PPCContext& ctx, uint8_t* base);  // pause/step: may hold the main thread (end of DevFrame)
bool Paused();
float Timescale();
void RecordHistory(const bf3::dev::Snapshot& s, float frame_ms);
void OnMessage(uint8_t* base, uint32_t msg, uint32_t lr);  // the message dispatcher (level_boot.cpp)

}  // namespace bf3_dev
