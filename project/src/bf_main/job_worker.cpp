// Frostbite job worker loop, BF.Main sub_8346D590(job chain r3, param r4). Faithful port of the guest code with one
// change: a job whose function pointer is already 0 is not called.
//
// Guest code (see docs/CLAUDE_SKILL.md, "BF.Main job system"): for each job in the chain it atomically replaces the
// 64-bit word at job+0 ({hi: flags/claim count, lo: next}) with {(hi & ~1) + 2, 0} (ldarx/stdcx), and if the old
// bit 0 was clear calls fn(arg, job, param) with fn at +8, arg at +12. Then it clears fn, atomically decrements the
// counter at parent+48 (parent at +16) and calls sub_8346BAE0(parent, job, parent->+40).
// Several workers walk the same chain (sub_8346E020 -> sub_8346DE18 -> here), and each clears fn after its call. A
// worker that claims a job right after another one finished it calls fn == 0: crash_trace showed
// "[FATAL] Call to invalid or unregistered function at guest address 0x00000000" from sub_8346D590 within ~12 s of
// boot, on every SDK. The console has the same window, much narrower (6 hardware threads, claim done with
// interrupts off via mtmsrd r13,1). Skipping the call is what that worker would do after fn returned.

// Compiled into the bf3_BF_Main DLL (see CMakeLists.txt): bf3_pch.h is generated/bf_main/bf3_pch.h.
#include "bf3_pch.h"

#include <atomic>

REX_EXTERN(sub_8346BAE0);

namespace {

uint64_t LoadBe64(uint8_t* base, uint32_t addr) {
  auto* p = reinterpret_cast<uint64_t*>(REX_RAW_ADDR(addr));
  return __builtin_bswap64(__atomic_load_n(p, __ATOMIC_SEQ_CST));
}

bool CasBe64(uint8_t* base, uint32_t addr, uint64_t expected, uint64_t desired) {
  auto* p = reinterpret_cast<uint64_t*>(REX_RAW_ADDR(addr));
  return __sync_bool_compare_and_swap(p, __builtin_bswap64(expected), __builtin_bswap64(desired));
}

void AddBe32(uint8_t* base, uint32_t addr, int32_t delta) {
  auto* p = reinterpret_cast<uint32_t*>(REX_RAW_ADDR(addr));
  for (;;) {
    uint32_t raw = __atomic_load_n(p, __ATOMIC_SEQ_CST);
    uint32_t next = __builtin_bswap32(__builtin_bswap32(raw) + delta);
    if (__sync_bool_compare_and_swap(p, raw, next)) return;
  }
}

std::atomic<uint32_t> g_skipped{0};

}  // namespace

extern "C" REX_FUNC(sub_8346D590) {
  uint32_t job = ctx.r3.u32;
  const uint64_t param = ctx.r4.u64;  // r27
  while (job != 0) {
    const uint32_t next = static_cast<uint32_t>(LoadBe64(base, job));  // r30: read once, before the claim
    uint64_t old;
    for (;;) {  // loc_8346D5BC: claim, retry until the 64-bit word didn't change underneath
      old = LoadBe64(base, job);
      const uint32_t claimed = (static_cast<uint32_t>(old >> 32) & ~1u) + 2;
      if (CasBe64(base, job, old, static_cast<uint64_t>(claimed) << 32)) break;
    }
    if (((old >> 32) & 1) == 0) {
      const uint32_t fn = REX_LOAD_U32(job + 8);
      if (fn != 0) {
        ctx.r3.u64 = REX_LOAD_U32(job + 12);
        ctx.r4.u64 = job;
        ctx.r5.u64 = param;
        ctx.lr = 0x8346D650;
        REX_CALL_INDIRECT_FUNC(fn);
      } else {
        uint32_t n = ++g_skipped;
        if ((n & (n - 1)) == 0) {  // 1, 2, 4, 8, ...
          REXLOG_WARN("job worker: job {:08X} already finished by another worker, skipped (x{})", job, n);
        }
      }
    }
    const uint32_t parent = REX_LOAD_U32(job + 16);  // loc_8346D650
    REX_STORE_U32(job + 8, 0);
    if (parent != 0) {
      AddBe32(base, parent + 48, -1);
      ctx.r3.u64 = parent;
      ctx.r4.u64 = job;
      ctx.r5.u64 = REX_LOAD_U32(parent + 40);
      ctx.lr = 0x8346D68C;
      sub_8346BAE0(ctx, base);
    }
    job = next;
  }
}
