# Toolchain for building the ReXGlue SDK itself (tools/rexglue-src) for Windows: the project toolchain plus
# llvm-ar (the SDK's C libraries are archived with GNU-style `ar qc`, which llvm-lib doesn't accept) and the
# SDK presets' -march=x86-64-v2.
include("${CMAKE_CURRENT_LIST_DIR}/win-amd64-xwin.cmake")
set(CMAKE_AR llvm-ar)
string(APPEND CMAKE_C_FLAGS_INIT " -march=x86-64-v2")
string(APPEND CMAKE_CXX_FLAGS_INIT " -march=x86-64-v2 -idirafter ${CMAKE_CURRENT_LIST_DIR}/include-case")
# CMake preprocesses .rc files with clang (cmake_llvm_rc), which takes -I but not /I.
set(CMAKE_RC_FLAGS_INIT "-I ${XWIN_DIR}/sdk/include/um -I ${XWIN_DIR}/sdk/include/shared")
# Linker maps (<dll>.map next to each DLL) for scripts/resolve_rva.py.
string(APPEND CMAKE_SHARED_LINKER_FLAGS_INIT " -Wl,/MAP")
