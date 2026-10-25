// Guards for runtime kernel calls made by BF.Main (see kernel_fixes.cpp).
#pragma once

namespace bf3 {

// Redirects bf3_BF_Main.dll's KeSetEvent/KeResetEvent/KePulseEvent imports once the module loads.
void InstallKernelFixes();

}  // namespace bf3
