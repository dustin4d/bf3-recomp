# bf3 - ReXGlue Recompiled Project
#
# SDK boilerplate lives in generated/rexglue.cmake.

cmake_minimum_required(VERSION 3.25)
project(bf3 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(generated/rexglue.cmake)

# Development tools: the debug API (src/dev/, bf_main/dev_state.cpp) that tests and scripts/bf3dbg.py drive the game
# through. Production builds turn it off (preset win-cross-ship) so none of it is compiled in.
option(BF3_DEV_TOOLS "Build the debug API (development only; off for production builds)" ON)

# Sources
set(BF3_SOURCES
    src/main.cpp
    src/launcher_hooks.cpp
    src/crash_trace.cpp
    src/aim_scan.cpp
    src/pad_inject.cpp
    src/pc_input.cpp
    src/ui_state.cpp
    src/kernel_fixes.cpp
    src/pc_icons.cpp
    src/pc_mouse.cpp
    src/pc_keys.cpp
    src/local_profile.cpp
    src/pc_settings.cpp
    src/audio_capture.cpp
)

if(WIN32)
    add_executable(bf3 WIN32 ${BF3_SOURCES})
else()
    add_executable(bf3 ${BF3_SOURCES})
endif()

rexglue_setup_target(bf3 GPU_PLUGINS xenos)

if(BF3_DEV_TOOLS)
    target_sources(bf3 PRIVATE src/dev/debug_api.cpp src/dev/dev_breakpoints.cpp)
    target_link_libraries(bf3 PRIVATE ws2_32)
endif()

# Hooks for guest functions that live in a DLL module must be linked into that module's library: a strong
# definition in bf3.exe can't override a weak function inside bf3_BF_Main.dll.
if(TARGET bf3_BF_Main)
    target_sources(bf3_BF_Main PRIVATE
        src/bf_main/exit_trace.cpp
        src/bf_main/job_worker.cpp
        src/bf_main/job_trace.cpp
        src/bf_main/lut_trace.cpp
        src/bf_main/fade_fix.cpp
        src/bf_main/practice.cpp
        src/bf_main/practice_menu.cpp
        src/bf_main/frame_rate.cpp
        src/bf_main/mouse_aim.cpp
        src/bf_main/lua_dump.cpp
        src/bf_main/ui_screens.cpp
        src/bf_main/level_boot.cpp
        src/bf_main/ui_buttons.cpp
        src/bf_main/ui_mouse.cpp
        src/bf_main/main_menu.cpp
        src/bf_main/boot_flow.cpp
        src/bf_main/local_online.cpp
        src/bf_main/pc_concepts.cpp
    )
    if(BF3_DEV_TOOLS)
        target_sources(bf3_BF_Main PRIVATE src/bf_main/dev_state.cpp src/bf_main/dev_tools.cpp)
    endif()
endif()

foreach(_t bf3 bf3_BF_Main)
    if(TARGET ${_t})
        target_compile_definitions(${_t} PRIVATE BF3_DEV_TOOLS=$<BOOL:${BF3_DEV_TOOLS}>)
    endif()
endforeach()

# Linker maps for scripts/resolve_rva.py (crash_trace frames are <module>+0x<RVA>).
if(WIN32)
    foreach(_t bf3 bf3_BF_Main bf3_Engine_BuildInfo)
        if(TARGET ${_t})
            target_link_options(${_t} PRIVATE "LINKER:/MAP:$<TARGET_FILE_DIR:${_t}>/${_t}.map")
        endif()
    endforeach()
endif()
