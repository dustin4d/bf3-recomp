// Patch one imported function in a loaded module's import address table.
//
// Recompiled modules import guest kernel functions (__imp__XamInputGetState, ...) from rexruntime.dll, and the GPU
// plugin imports CRT functions; a hook in bf3.exe can't override those by symbol, but it can redirect the import.
#pragma once

#if defined(_WIN32)
#include <windows.h>

#include <cstring>

namespace bf3 {

// Replaces `module`'s import of `symbol` from `dll` (case-insensitive DLL name) with `replacement` and returns the
// original function, or nullptr if the import isn't there.
inline void* PatchImport(HMODULE module, const char* dll, const char* symbol, void* replacement) {
  if (!module) return nullptr;
  auto base = reinterpret_cast<uint8_t*>(module);
  auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
  auto nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
  auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
  for (auto imp = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress); imp->Name; ++imp) {
    if (_stricmp(reinterpret_cast<char*>(base + imp->Name), dll) != 0) continue;
    auto names = reinterpret_cast<IMAGE_THUNK_DATA*>(base + imp->OriginalFirstThunk);
    auto slots = reinterpret_cast<IMAGE_THUNK_DATA*>(base + imp->FirstThunk);
    for (; names->u1.AddressOfData; ++names, ++slots) {
      if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
      auto by_name = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData);
      if (std::strcmp(reinterpret_cast<char*>(by_name->Name), symbol) != 0) continue;
      DWORD old;
      if (!VirtualProtect(&slots->u1.Function, sizeof(slots->u1.Function), PAGE_READWRITE, &old)) return nullptr;
      void* original = reinterpret_cast<void*>(slots->u1.Function);
      slots->u1.Function = reinterpret_cast<ULONGLONG>(replacement);
      VirtualProtect(&slots->u1.Function, sizeof(slots->u1.Function), old, &old);
      return original;
    }
  }
  return nullptr;
}

}  // namespace bf3
#endif
