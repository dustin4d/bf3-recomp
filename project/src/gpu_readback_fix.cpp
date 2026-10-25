// Precise memexport readback for the xenos GPU plugin.
//
// ReXGlue reads back shader memexport results by copying the WHOLE declared export range (stream constant base and
// size) from its GPU-side copy of guest memory into guest memory (D3D12CommandProcessor::IssueDraw_MemexportReadback*,
// readback_memexport defaults to true). The GPU-side copy of the bytes the shader didn't export is whatever was
// uploaded before the draw (or, on the default fast path, the previous draw's copy), so CPU writes made in the meantime
// get rolled back. In BF3 the job system's pool (free-list head at guest F1A9F180) lies inside an export range: the
// rollbacks corrupted its tagged free list (double allocations, jobs run twice, fn == 0 calls, then a hang). Found
// with a hardware write watch: the stale write was VCRUNTIME140!memcpy called from the plugin's command processor,
// through the physical view. The console's GPU writes only the exported elements.
//
// Fix, without rebuilding the plugin: hook memcpy in rexgpu-xenos.dll's import table. The plugin copies guest memory
// only on the GPU command thread, in two directions:
//   upload   (src in the physical view): guest -> shadow -> upload buffer, so the shadow holds exactly what the GPU got
//   readback (dst in the physical view): write only the bytes that differ from the shadow (= changed by the GPU), then
//            update the shadow. Pages never uploaded fall back to the plugin's full copy.
// Requires the synchronous readback path (readback_memexport_fast=false, set in Bf3App::OnPreSetup): the fast path
// copies the previous draw's results, which would be compared against a newer shadow.

#include "gpu_readback_fix.h"

#if defined(_WIN32)
#include <windows.h>

#include <rex/logging.h>
#include <rex/system/xmemory.h>

#include <algorithm>
#include <atomic>
#include <cstring>

namespace bf3 {
namespace {

using MemcpyFn = void*(__cdecl*)(void*, const void*, size_t);

constexpr size_t kPhysSize = 0x20000000;  // 512 MB guest physical memory
constexpr size_t kPageShift = 12;

MemcpyFn g_memcpy = nullptr;
uint8_t* g_phys = nullptr;    // guest physical view (Memory::TranslatePhysical base)
uint8_t* g_shadow = nullptr;  // GPU-copy baseline, same layout as g_phys
uint8_t g_page_shadowed[kPhysSize >> kPageShift];
std::atomic<uint64_t> g_readbacks{0}, g_readback_bytes{0}, g_written_bytes{0}, g_kept_bytes{0};
std::atomic<uint64_t> g_calls{0}, g_uploads{0};

bool InPhys(const void* p, size_t n) {
  auto a = static_cast<const uint8_t*>(p);
  return a >= g_phys && n <= kPhysSize && a + n <= g_phys + kPhysSize;
}

std::atomic<int> g_logged{0};
thread_local int t_logged_this_readback = 0;
thread_local uint64_t t_readback_index = 0;
rex::memory::Memory* g_memory = nullptr;

// Which guest allocations own physical address `phys` in each view of physical memory (A: 64 KB pages, C: 16 MB,
// E: 4 KB with the console's 0x1000 offset)? Two committed owners = two allocations sharing physical memory.
void LogPhysicalOwners(uint32_t phys) {
  const uint32_t views[3] = {0xA0000000u + phys, 0xC0000000u + phys, 0xE0000000u + phys - 0x1000u};
  for (uint32_t v : views) {
    auto* heap = g_memory->LookupHeap(v);
    rex::memory::HeapAllocationInfo info{};
    if (heap && heap->QueryRegionInfo(v, &info)) {
      REXLOG_WARN("gpu readback fix:   view {:08X}: alloc base {:08X} size {:X}, region {:08X}+{:X} state {:X}", v,
                  info.allocation_base, info.allocation_size, info.base_address, info.region_size, info.state);
    }
  }
}

// Readback of one span that lies in pages with a shadow: write only what the GPU changed. Returns bytes written.
size_t DiffWrite(uint8_t* dst, const uint8_t* src, uint8_t* shadow, size_t n) {
  size_t written = 0;
  for (size_t i = 0; i < n; ++i) {
    if (src[i] != shadow[i]) {
      if (t_readback_index < 8 && t_logged_this_readback < 12) {
        ++t_logged_this_readback;
        REXLOG_WARN("gpu readback fix:   #{} phys {:08X}: guest {:02X} shadow {:02X} gpu {:02X}", t_readback_index,
                    static_cast<uint32_t>(dst + i - g_phys), dst[i], shadow[i], src[i]);
      }
      dst[i] = src[i];
      shadow[i] = src[i];
      ++written;
    }
  }
  g_written_bytes += written;
  g_kept_bytes += n - written;
  return written;
}

void* __cdecl HookedMemcpy(void* dst, const void* src, size_t n) {
  ++g_calls;
  if (n == 0) return g_memcpy(dst, src, n);
  if (InPhys(src, n) && !InPhys(dst, n)) {  // upload
    size_t off = static_cast<const uint8_t*>(src) - g_phys;
    ++g_uploads;
    g_memcpy(g_shadow + off, src, n);
    g_memcpy(dst, g_shadow + off, n);
    for (size_t p = off >> kPageShift; p <= (off + n - 1) >> kPageShift; ++p) g_page_shadowed[p] = 1;
    return dst;
  }
  if (InPhys(dst, n) && !InPhys(src, n)) {  // readback (memexport, resolve)
    auto* d = static_cast<uint8_t*>(dst);
    auto* s = static_cast<const uint8_t*>(src);
    size_t off = d - g_phys;
    const uint64_t index = g_readbacks++;
    t_readback_index = index;
    t_logged_this_readback = 0;
    g_readback_bytes += n;
    if (index < 16) {
      REXLOG_WARN("gpu readback fix: readback #{} phys {:08X} size {:X}", index, off, n);
      LogPhysicalOwners(static_cast<uint32_t>(off));
    }
    size_t done = 0;
    while (done < n) {
      size_t page = (off + done) >> kPageShift;
      size_t span = std::min(n - done, ((page + 1) << kPageShift) - (off + done));
      if (g_page_shadowed[page]) {
        DiffWrite(d + done, s + done, g_shadow + off + done, span);
      } else {
        g_memcpy(d + done, s + done, span);
        g_memcpy(g_shadow + off + done, s + done, span);
        g_page_shadowed[page] = 1;
        g_written_bytes += span;
      }
      done += span;
    }
    return dst;
  }
  return g_memcpy(dst, src, n);
}

// Point the module's imported VCRUNTIME140!memcpy at HookedMemcpy.
bool PatchImport(HMODULE module) {
  auto base = reinterpret_cast<uint8_t*>(module);
  auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
  auto nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
  auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
  for (auto imp = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress); imp->Name; ++imp) {
    if (_stricmp(reinterpret_cast<char*>(base + imp->Name), "VCRUNTIME140.dll") != 0) continue;
    auto names = reinterpret_cast<IMAGE_THUNK_DATA*>(base + imp->OriginalFirstThunk);
    auto slots = reinterpret_cast<IMAGE_THUNK_DATA*>(base + imp->FirstThunk);
    for (; names->u1.AddressOfData; ++names, ++slots) {
      if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
      auto by_name = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData);
      if (std::strcmp(reinterpret_cast<char*>(by_name->Name), "memcpy") != 0) continue;
      DWORD old;
      if (!VirtualProtect(&slots->u1.Function, sizeof(slots->u1.Function), PAGE_READWRITE, &old)) return false;
      g_memcpy = reinterpret_cast<MemcpyFn>(slots->u1.Function);
      slots->u1.Function = reinterpret_cast<ULONGLONG>(&HookedMemcpy);
      VirtualProtect(&slots->u1.Function, sizeof(slots->u1.Function), old, &old);
      return true;
    }
  }
  return false;
}

}  // namespace

void InstallGpuReadbackFix(rex::memory::Memory* memory) {
  g_memory = memory;
  uint8_t* physical_membase = memory ? memory->physical_membase() : nullptr;
  HMODULE gpu = GetModuleHandleW(L"rexgpu-xenos.dll");
  if (!gpu || !physical_membase) {
    REXLOG_WARN("gpu readback fix: GPU plugin or physical memory not found, not installed");
    return;
  }
  g_phys = physical_membase;
  g_shadow = static_cast<uint8_t*>(VirtualAlloc(nullptr, kPhysSize, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
  if (!g_shadow || !PatchImport(gpu)) {
    REXLOG_WARN("gpu readback fix: could not hook rexgpu-xenos.dll memcpy, not installed");
    return;
  }
  REXLOG_INFO("gpu readback fix: memexport/resolve readbacks now write only GPU-changed bytes (physical view {})",
              static_cast<void*>(g_phys));
  CreateThread(
      nullptr, 0,
      [](LPVOID) -> DWORD {
        for (;;) {
          Sleep(10000);
          LogGpuReadbackStats();
        }
      },
      nullptr, 0, nullptr);
}

void LogGpuReadbackStats() {
  if (!g_memcpy) return;
  REXLOG_INFO("gpu readback fix: {} memcpy calls, {} uploads, {} readbacks ({} bytes: {} written, {} kept)",
              g_calls.load(), g_uploads.load(), g_readbacks.load(), g_readback_bytes.load(), g_written_bytes.load(),
              g_kept_bytes.load());
}

}  // namespace bf3

#else
namespace bf3 {
void InstallGpuReadbackFix(rex::memory::Memory*) {}
void LogGpuReadbackStats() {}
}  // namespace bf3
#endif
