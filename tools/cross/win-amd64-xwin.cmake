# Cross-compile toolchain: Linux host -> Windows x86_64 (MSVC ABI) using clang + lld-link
# against the MSVC CRT / Windows SDK splatted by xwin into tools/xwin.
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR AMD64)

get_filename_component(_TOOLS_DIR "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
set(XWIN_DIR "${_TOOLS_DIR}/xwin" CACHE PATH "xwin splat output")

set(_target x86_64-pc-windows-msvc)
set(CMAKE_C_COMPILER clang)
set(CMAKE_CXX_COMPILER clang++)
set(CMAKE_C_COMPILER_TARGET ${_target})
set(CMAKE_CXX_COMPILER_TARGET ${_target})
set(CMAKE_RC_COMPILER llvm-rc)
set(CMAKE_MT llvm-mt)
set(CMAKE_AR llvm-lib)
set(CMAKE_LINKER lld-link)

set(_sysinc
  "-idirafter ${XWIN_DIR}/crt/include"
  "-idirafter ${XWIN_DIR}/sdk/include/ucrt"
  "-idirafter ${XWIN_DIR}/sdk/include/um"
  "-idirafter ${XWIN_DIR}/sdk/include/shared"
  "-idirafter ${XWIN_DIR}/sdk/include/winrt")
list(JOIN _sysinc " " _sysinc)
set(CMAKE_C_FLAGS_INIT "${_sysinc}")
set(CMAKE_CXX_FLAGS_INIT "${_sysinc}")

set(_libdirs
  "-fuse-ld=lld"
  "-L${XWIN_DIR}/crt/lib/x86_64"
  "-L${XWIN_DIR}/sdk/lib/ucrt/x86_64"
  "-L${XWIN_DIR}/sdk/lib/um/x86_64")
list(JOIN _libdirs " " _libdirs)
set(CMAKE_EXE_LINKER_FLAGS_INIT "${_libdirs}")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "${_libdirs}")
set(CMAKE_MODULE_LINKER_FLAGS_INIT "${_libdirs}")

# llvm-rc needs the SDK headers too.
set(CMAKE_RC_FLAGS_INIT "/I ${XWIN_DIR}/sdk/include/um /I ${XWIN_DIR}/sdk/include/shared")

# xwin ships only the release CRT (debug CRT isn't redistributable); match the SDK's /MD.
set(CMAKE_MSVC_RUNTIME_LIBRARY MultiThreadedDLL)
set(CMAKE_TRY_COMPILE_CONFIGURATION Release)
