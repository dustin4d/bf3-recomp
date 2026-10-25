// Precise memexport readback for the xenos GPU plugin (see gpu_readback_fix.cpp).
#pragma once

#include <cstdint>

namespace rex::memory {
class Memory;
}

namespace bf3 {

// Hook rexgpu-xenos.dll's memcpy so GPU readbacks only write bytes the GPU changed. Call after the GPU plugin is
// loaded (Bf3App::OnPostSetup).
void InstallGpuReadbackFix(rex::memory::Memory* memory);
void LogGpuReadbackStats();

}  // namespace bf3
