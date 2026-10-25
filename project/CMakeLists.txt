# bf3 - ReXGlue Recompiled Project
#
# SDK boilerplate lives in generated/rexglue.cmake.

cmake_minimum_required(VERSION 3.25)
project(bf3 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(generated/rexglue.cmake)

# Sources
set(BF3_SOURCES
    src/main.cpp
    src/launcher_hooks.cpp
    src/crash_trace.cpp
    src/pad_inject.cpp
    src/pc_input.cpp
    src/ui_state.cpp
    src/kernel_fixes.cpp
    src/pc_icons.cpp
    src/pc_mouse.cpp
    src/local_profile.cpp
    src/pc_settings.cpp
)

if(WIN32)
    add_executable(bf3 WIN32 ${BF3_SOURCES})
else()
    add_executable(bf3 ${BF3_SOURCES})
endif()

rexglue_setup_target(bf3 GPU_PLUGINS xenos)

# Hooks for guest functions that live in a DLL module must be linked into that module's library: a strong
# definition in bf3.exe can't override a weak function inside bf3_BF_Main.dll.
if(TARGET bf3_BF_Main)
    target_sources(bf3_BF_Main PRIVATE
        src/bf_main/exit_trace.cpp
        src/bf_main/job_worker.cpp
        src/bf_main/job_trace.cpp
        src/bf_main/lua_dump.cpp
        src/bf_main/ui_screens.cpp
        src/bf_main/level_boot.cpp
        src/bf_main/ui_buttons.cpp
        src/bf_main/ui_mouse.cpp
        src/bf_main/main_menu.cpp
        src/bf_main/local_online.cpp
    )
endif()

# Linker maps for scripts/resolve_rva.py (crash_trace frames are <module>+0x<RVA>).
if(WIN32)
    foreach(_t bf3 bf3_BF_Main bf3_Engine_BuildInfo)
        if(TARGET ${_t})
            target_link_options(${_t} PRIVATE "LINKER:/MAP:$<TARGET_FILE_DIR:${_t}>/${_t}.map")
        endif()
    endforeach()
endif()
