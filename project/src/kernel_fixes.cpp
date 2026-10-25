// Stale event handles in guest dispatcher headers.
//
// ReXGlue creates the host object for a guest KEVENT on first use and caches its handle in the guest struct
// (wait_list_flink = 'REX\0', wait_list_blink = handle; XObject::GetNativeObject). When the game frees that memory and
// later puts a new event there without KeInitializeEvent (the XDK inlines it), the cached handle still names the old
// object, which may be gone or of another type, and KeSetEvent then calls XEvent::Set on it. That crashed in
// rexruntime (XEvent::Set+0x8) right after the client loaded a level switched to from the front end. Before
// KeSetEvent/KeResetEvent/KePulseEvent, drop a cached handle that no longer names an event so the runtime creates a
// fresh one for the struct.
//
// Critical sections embed the same dispatcher header, and RtlInitializeCriticalSection(AndSpinCount) resets the
// header without the cached handle, so a section initialized in reused memory wakes waiters through the old object
// (RtlLeaveCriticalSection -> xeKeSetEvent inside the runtime, out of reach of the import hooks above). After those
// two calls the cached handle is cleared, and RtlEnter/LeaveCriticalSection validate it like the event calls (not
// every section goes through the initializers, e.g. ones copied or set up by inlined code).

#include "kernel_fixes.h"

#if defined(_WIN32)
#include <windows.h>

#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xobject.h>

#include <atomic>
#include <cstring>

#include "iat_patch.h"

namespace bf3 {
namespace {

using GuestFunc = void (*)(PPCContext&, uint8_t*);
GuestFunc g_set_event, g_reset_event, g_pulse_event, g_init_cs, g_init_cs_spin, g_enter_cs, g_leave_cs;
std::atomic<int> g_reports{0};

uint8_t* Header(uint32_t guest, uint8_t* base);

uint32_t LoadBe32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }

void ValidateEvent(uint32_t guest, uint8_t* base) {
  if (!guest) return;
  uint8_t* header = Header(guest, base);
  if (LoadBe32(header + 8) != rex::system::kXObjSignature) return;
  const uint32_t handle = LoadBe32(header + 12);
  auto object = rex::system::kernel_state()->object_table()->LookupObject<rex::system::XObject>(handle);
  if (object && object->type() == rex::system::XObject::Type::Event) return;
  if (g_reports.fetch_add(1) < 20) {
    REXLOG_WARN("kernel fix: event {:08X} had stale handle {:08X} ({}), recreating", guest, handle,
                object ? "wrong type" : "gone");
  }
  std::memset(header + 8, 0, 8);  // forget the cached handle: GetNativeObject creates a new event
}

void SetEvent(PPCContext& ctx, uint8_t* base) {
  ValidateEvent(ctx.r3.u32, base);
  g_set_event(ctx, base);
}
void ResetEvent(PPCContext& ctx, uint8_t* base) {
  ValidateEvent(ctx.r3.u32, base);
  g_reset_event(ctx, base);
}
void PulseEvent(PPCContext& ctx, uint8_t* base) {
  ValidateEvent(ctx.r3.u32, base);
  g_pulse_event(ctx, base);
}

uint8_t* Header(uint32_t guest, uint8_t* base) { return base + guest + (guest >= 0xE0000000u ? 0x1000u : 0u); }

void InitCriticalSection(PPCContext& ctx, uint8_t* base) {
  const uint32_t cs = ctx.r3.u32;
  g_init_cs(ctx, base);
  if (cs) std::memset(Header(cs, base) + 8, 0, 8);
}
void InitCriticalSectionSpin(PPCContext& ctx, uint8_t* base) {
  const uint32_t cs = ctx.r3.u32;
  g_init_cs_spin(ctx, base);
  if (cs) std::memset(Header(cs, base) + 8, 0, 8);
}

// Enter waits on, and Leave sets, the section's embedded event once it was contended.
void EnterCriticalSection(PPCContext& ctx, uint8_t* base) {
  ValidateEvent(ctx.r3.u32, base);
  g_enter_cs(ctx, base);
}
void LeaveCriticalSection(PPCContext& ctx, uint8_t* base) {
  ValidateEvent(ctx.r3.u32, base);
  g_leave_cs(ctx, base);
}

void Patch(HMODULE module, const char* symbol, GuestFunc hook, GuestFunc& original) {
  original = reinterpret_cast<GuestFunc>(PatchImport(module, "rexruntime.dll", symbol, reinterpret_cast<void*>(hook)));
  if (!original) REXLOG_WARN("kernel fix: no {} import in bf3_BF_Main.dll", symbol);
}

}  // namespace

void InstallKernelFixes() {
  CreateThread(
      nullptr, 0,
      [](LPVOID) -> DWORD {
        HMODULE module = nullptr;
        for (int i = 0; i < 6000 && !module; ++i) {
          module = GetModuleHandleW(L"bf3_BF_Main.dll");
          if (!module) Sleep(10);
        }
        if (!module) return 0;
        Patch(module, "__imp__KeSetEvent", &SetEvent, g_set_event);
        Patch(module, "__imp__KeResetEvent", &ResetEvent, g_reset_event);
        Patch(module, "__imp__KePulseEvent", &PulseEvent, g_pulse_event);
        Patch(module, "__imp__RtlInitializeCriticalSection", &InitCriticalSection, g_init_cs);
        Patch(module, "__imp__RtlInitializeCriticalSectionAndSpinCount", &InitCriticalSectionSpin, g_init_cs_spin);
        Patch(module, "__imp__RtlEnterCriticalSection", &EnterCriticalSection, g_enter_cs);
        Patch(module, "__imp__RtlLeaveCriticalSection", &LeaveCriticalSection, g_leave_cs);
        REXLOG_INFO("kernel fix: event handle validation installed");
        return 0;
      },
      nullptr, 0, nullptr);
}

}  // namespace bf3

#else
namespace bf3 {
void InstallKernelFixes() {}
}  // namespace bf3
#endif
