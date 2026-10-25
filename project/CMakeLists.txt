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
)

if(WIN32)
    add_executable(bf3 WIN32 ${BF3_SOURCES})
else()
    add_executable(bf3 ${BF3_SOURCES})
endif()

rexglue_setup_target(bf3)
