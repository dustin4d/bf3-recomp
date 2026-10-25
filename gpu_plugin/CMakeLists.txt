# Builds rexgpu-xenos.dll (ReXGlue's Xenos GPU plugin, D3D12 backend) from the ReXGlue source tree with our patches
# (patches/rexglue/*.patch applied to tools/rexglue-src), against the prebuilt Windows SDK's runtime. The plugin only
# talks to rexruntime.dll through the SDK's public headers, so it can be rebuilt on its own as long as the source tree
# is at the SDK's commit (bd833a2a for nightly-20261002).
#
#   cmake -S gpu_plugin -B gpu_plugin/build -G Ninja -DCMAKE_BUILD_TYPE=Release \
#     -DCMAKE_TOOLCHAIN_FILE=$PWD/tools/cross/win-amd64-xwin.cmake -Drexglue_DIR=$PWD/tools/cross/rexglue
#   cmake --build gpu_plugin/build
cmake_minimum_required(VERSION 3.25)
project(rexgpu_xenos_patched CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(REXGLUE_SRC "${CMAKE_CURRENT_SOURCE_DIR}/../tools/rexglue-src" CACHE PATH "ReXGlue source tree")
find_package(rexglue CONFIG REQUIRED)

set(G "${REXGLUE_SRC}/src/graphics")
add_library(rexgpu-xenos SHARED
    ${G}/plugin_main.cpp
    ${G}/flags.cpp
    ${G}/register_file.cpp
    ${G}/registers.cpp
    ${G}/pipeline/shader/shader.cpp
    ${G}/format/ucode.cpp
    ${G}/sampler_info.cpp
    ${G}/graphics_system.cpp
    ${G}/command_processor.cpp
    ${G}/util/draw_extent_estimator.cpp
    ${G}/util/draw.cpp
    ${G}/packet_disassembler.cpp
    ${G}/primitive_processor.cpp
    ${G}/pipeline/render_target/cache.cpp
    ${G}/shared_memory.cpp
    ${G}/pipeline/texture/cache.cpp
    ${G}/pipeline/shader/interpreter.cpp
    ${G}/pipeline/shader/translator.cpp
    ${G}/pipeline/shader/translator_disasm.cpp
    ${REXGLUE_SRC}/thirdparty/dxbc/DXBCChecksum.cpp
    ${G}/pipeline/shader/dxbc.cpp
    ${G}/pipeline/shader/dxbc_translator.cpp
    ${G}/pipeline/shader/dxbc_translator_alu.cpp
    ${G}/pipeline/shader/dxbc_translator_fetch.cpp
    ${G}/pipeline/shader/dxbc_translator_memexport.cpp
    ${G}/pipeline/shader/dxbc_translator_om.cpp
    ${G}/d3d12/graphics_system.cpp
    ${G}/d3d12/command_processor.cpp
    ${G}/d3d12/primitive_processor.cpp
    ${G}/d3d12/render_target_cache.cpp
    ${G}/d3d12/shader.cpp
    ${G}/d3d12/shared_memory.cpp
    ${G}/d3d12/texture_cache.cpp
    ${G}/d3d12/deferred_command_list.cpp
    ${G}/d3d12/pipeline_cache.cpp
)
# Source-tree headers first (same commit as the SDK's, plus our patches), then the SDK's third-party headers.
target_include_directories(rexgpu-xenos BEFORE PRIVATE
    "${REXGLUE_SRC}/include"
    "${REXGLUE_SRC}/thirdparty/renderdoc"
    "${REXGLUE_SRC}"
)
# SDK targets require SSE4.1 (nightly-20261001 "require SSE4.1 on SDK targets"). include-case: case variants of
# Windows SDK headers that ReXGlue includes with capitals (ObjBase.h); xwin's splat is lowercase.
target_compile_options(rexgpu-xenos PRIVATE -Wno-unused-variable -msse4.1
    "SHELL:-idirafter ${CMAKE_CURRENT_SOURCE_DIR}/../tools/cross/include-case")
target_link_libraries(rexgpu-xenos PRIVATE rex::runtime d3d12 dxgi dxguid)
if(TARGET xxHash::xxhash)
    target_link_libraries(rexgpu-xenos PRIVATE xxHash::xxhash)
else()
    target_link_libraries(rexgpu-xenos PRIVATE "${rexglue_DIR}/../../rexglue-sdk-win/lib/xxhash.lib")
endif()
set_target_properties(rexgpu-xenos PROPERTIES CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN ON)
# Linker map for crash/thread-dump symbolization (scripts/resolve_rva.py).
target_link_options(rexgpu-xenos PRIVATE "LINKER:/MAP:${CMAKE_CURRENT_BINARY_DIR}/rexgpu-xenos.map")
