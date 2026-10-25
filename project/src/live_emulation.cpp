// Xbox LIVE sign-in emulation for BF.Main (experimental, BF3_LIVE=1).
//
// ReXGlue reports profile 0 as signed in locally only (UserProfile::signin_state() == 1) and denies every privilege,
// so Multiplayer, Co-op and My Soldier stop at "You need to be signed into Xbox LIVE". These hooks redirect
// bf3_BF_Main.dll's imports so the profile looks signed in to LIVE with multiplayer privileges and a connected network
// link. They run the runtime's implementation first and only adjust the results.

#include "live_emulation.h"

#if defined(_WIN32)
#include <windows.h>

#include <rex/logging.h>
#include <rex/ppc/context.h>

#include <cstdlib>
#include <cstring>

#include "iat_patch.h"

namespace bf3 {
namespace {

using GuestFunc = void (*)(PPCContext&, uint8_t*);

GuestFunc g_signin_state, g_signin_info, g_check_privilege, g_link_status;

void StoreBe32(uint8_t* p, uint32_t v) {
  p[0] = uint8_t(v >> 24), p[1] = uint8_t(v >> 16), p[2] = uint8_t(v >> 8), p[3] = uint8_t(v);
}

// u32 XamUserGetSigninState(u32 user_index): 0 none, 1 local, 2 LIVE.
void SigninState(PPCContext& ctx, uint8_t* base) {
  uint32_t user = ctx.r3.u32;
  g_signin_state(ctx, base);
  if (user == 0 && ctx.r3.u32 != 0) ctx.r3.u64 = 2;
}

// u32 XamUserGetSigninInfo(u32 user_index, u32 flags, X_USER_SIGNIN_INFO* info): info+0xC = signin_state.
void SigninInfo(PPCContext& ctx, uint8_t* base) {
  uint32_t user = ctx.r3.u32, info = ctx.r5.u32;
  g_signin_info(ctx, base);
  if (user == 0 && info && ctx.r3.u32 == 0) StoreBe32(base + info + 0xC, 2);
}

// u32 XamUserCheckPrivilege(u32 user_index, u32 mask, u32* out): grant everything.
void CheckPrivilege(PPCContext& ctx, uint8_t* base) {
  uint32_t out = ctx.r5.u32;
  g_check_privilege(ctx, base);
  if (out && ctx.r3.u32 == 0) StoreBe32(base + out, 1);
}

// u32 NetDll_XNetGetEthernetLinkStatus(u32 caller): XNET_ETHERNET_LINK_ACTIVE | 100 Mbps | full duplex.
void LinkStatus(PPCContext& ctx, uint8_t* base) {
  g_link_status(ctx, base);
  ctx.r3.u64 = 0x01 | 0x04 | 0x08;
}

void Patch(HMODULE module, const char* symbol, GuestFunc hook, GuestFunc& original) {
  original = reinterpret_cast<GuestFunc>(PatchImport(module, "rexruntime.dll", symbol, reinterpret_cast<void*>(hook)));
  if (!original) REXLOG_WARN("live emulation: no {} import in bf3_BF_Main.dll", symbol);
}

}  // namespace

void InstallLiveEmulation() {
  const char* env = std::getenv("BF3_LIVE");
  if (!env || std::strcmp(env, "1") != 0) return;
  CreateThread(
      nullptr, 0,
      [](LPVOID) -> DWORD {
        HMODULE module = nullptr;
        for (int i = 0; i < 6000 && !module; ++i) {
          module = GetModuleHandleW(L"bf3_BF_Main.dll");
          if (!module) Sleep(10);
        }
        if (!module) return 0;
        Patch(module, "__imp__XamUserGetSigninState", &SigninState, g_signin_state);
        Patch(module, "__imp__XamUserGetSigninInfo", &SigninInfo, g_signin_info);
        Patch(module, "__imp__XamUserCheckPrivilege", &CheckPrivilege, g_check_privilege);
        Patch(module, "__imp__NetDll_XNetGetEthernetLinkStatus", &LinkStatus, g_link_status);
        REXLOG_INFO("live emulation: profile 0 reports Xbox LIVE sign-in");
        return 0;
      },
      nullptr, 0, nullptr);
}

}  // namespace bf3

#else
namespace bf3 {
void InstallLiveEmulation() {}
}  // namespace bf3
#endif
