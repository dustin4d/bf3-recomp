// Forensics for the BF.Main job system (see job_worker.cpp): record every operation on the job lists in a ring
// buffer, so a job that reaches a worker with fn == 0 can be explained by its history. Enabled with
// BF3_JOB_TRACE=1 (environment). Hooks call the original guest functions unchanged.
//   sub_8346BB60(head, linkoff) -> node   tagged free-list pop  (alloc)
//   sub_8346BAE0(head, node, linkoff)     tagged free-list push (free)
//   sub_8346CDE8(head, node)              run-queue push
// The worker (job_worker.cpp) records chain starts and claims through JobTraceRecord.

#include "bf3_pch.h"

#include <intrin.h>

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <unordered_set>

REX_EXTERN(__imp__sub_8346BB60);
REX_EXTERN(__imp__sub_8346BAE0);
REX_EXTERN(__imp__sub_8346CDE8);
REX_EXTERN(__imp__sub_8346DE18);

namespace {

struct Event {
  uint64_t seq;
  uint32_t op, tid, a, b, c, d;
  uint64_t host_base;  // the `base` this call ran with
};
constexpr uint32_t kEvents = 1u << 18;
Event g_events[kEvents];
std::atomic<uint64_t> g_seq{0};

const char* OpName(uint32_t op) {
  switch (op) {
    case 1: return "alloc";
    case 2: return "free";
    case 3: return "qpush";
    case 4: return "chain";
    case 5: return "claim";
    case 6: return "popall";
    default: return "?";
  }
}

// Ownership check: nodes currently allocated from a traced free list. Marked after a pop returns (the popping
// thread owns the node), checked before a push, so the bookkeeping itself can't race.
std::mutex g_owned_mutex;
std::unordered_set<uint32_t> g_owned;
std::atomic<int> g_anomalies{0};

}  // namespace

void JobTraceDump(uint32_t addr, uint64_t upto);

bool JobTraceEnabled() {
  static const bool enabled = [] {
    const char* v = std::getenv("BF3_JOB_TRACE");
    return v && std::strcmp(v, "1") == 0;
  }();
  return enabled;
}

uint64_t JobTraceRecord(uint32_t op, uint32_t a, uint32_t b, uint32_t c, uint32_t d, const uint8_t* base) {
  if (!JobTraceEnabled()) return 0;
  uint64_t s = g_seq.fetch_add(1, std::memory_order_relaxed);
  Event& e = g_events[s & (kEvents - 1)];
  e = {s, op, __readgsdword(0x48), a, b, c, d, reinterpret_cast<uint64_t>(base)};
  return s;
}

// Log the last 48 recorded events up to sequence number `upto` (0 = now) that mention addr, oldest first.
void JobTraceDump(uint32_t addr, uint64_t upto) {
  if (!JobTraceEnabled()) return;
  uint64_t end = upto ? upto + 1 : g_seq.load();
  uint64_t begin = end > kEvents ? end - kEvents : 0;
  uint64_t hits[48];
  int n = 0;
  for (uint64_t s = end; s-- > begin && n < 48;) {
    const Event& e = g_events[s & (kEvents - 1)];
    if (e.seq == s && (e.a == addr || e.b == addr || e.c == addr || e.d == addr)) hits[n++] = s;
  }
  for (int i = n - 1; i >= 0; --i) {
    const Event& e = g_events[hits[i] & (kEvents - 1)];
    REXLOG_WARN("job trace:   #{} [t{}] {:6} {:08X} {:08X} {:08X} {:08X} base {:X}", e.seq, e.tid, OpName(e.op), e.a,
                e.b, e.c, e.d, e.host_base);
  }
  REXLOG_WARN("job trace:   ({} events shown, {} recorded)", n, end);
}

// alloc: a = pool, b = node, c = head pointer after the pop (= the link the pop read), d = head tag after.
extern "C" REX_FUNC(sub_8346BB60) {
  const uint32_t head = ctx.r3.u32;
  __imp__sub_8346BB60(ctx, base);
  const uint32_t node = ctx.r3.u32;
  if (!node || !JobTraceEnabled()) return;
  const uint64_t seq = JobTraceRecord(1, head, node, REX_LOAD_U32(head + 4), REX_LOAD_U32(head), base);
  if (REX_LOAD_U32(head + 16) != 0xAAC0FFEE) return;  // only pools (magic at +16), e.g. the job pool F1A9F180
  bool fresh;
  {
    std::lock_guard<std::mutex> lock(g_owned_mutex);
    fresh = g_owned.insert(node).second;
  }
  if (!fresh && g_anomalies.fetch_add(1) < 4) {
    REXLOG_WARN("job trace: DOUBLE ALLOC of {:08X} from free list {:08X} (seq {})", node, head, seq);
    JobTraceDump(node, seq);
  }
}

// free: a = pool, b = node, c = head pointer before the push (becomes node's link), d = fn at free time.
extern "C" REX_FUNC(sub_8346BAE0) {
  if (JobTraceEnabled()) {
    const uint32_t head = ctx.r3.u32, node = ctx.r4.u32;
    const uint64_t seq = JobTraceRecord(2, head, node, REX_LOAD_U32(head + 4), REX_LOAD_U32(node + 8), base);
    if (REX_LOAD_U32(head + 16) == 0xAAC0FFEE) {
      size_t erased;
      {
        std::lock_guard<std::mutex> lock(g_owned_mutex);
        erased = g_owned.erase(node);
      }
      if (!erased && g_anomalies.fetch_add(1) < 4) {
        REXLOG_WARN("job trace: FREE OF UNOWNED {:08X} to pool {:08X} (seq {})", node, head, seq);
        JobTraceDump(node, seq);
      }
    }
  }
  __imp__sub_8346BAE0(ctx, base);
}

// popall: a = queue head, b = head pointer (the chain), c = head tag, read just before the pop.
extern "C" REX_FUNC(sub_8346DE18) {
  JobTraceRecord(6, ctx.r3.u32, REX_LOAD_U32(ctx.r3.u32 + 4), REX_LOAD_U32(ctx.r3.u32), 0, base);
  __imp__sub_8346DE18(ctx, base);
}

extern "C" REX_FUNC(sub_8346CDE8) {
  JobTraceRecord(3, ctx.r3.u32, ctx.r4.u32, REX_LOAD_U32(ctx.r4.u32 + 8), REX_LOAD_U32(ctx.r3.u32 + 4), base);
  __imp__sub_8346CDE8(ctx, base);
}
