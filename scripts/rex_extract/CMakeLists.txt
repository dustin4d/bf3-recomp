cmake_minimum_required(VERSION 3.25)
project(rex_extract CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Configure with -DCMAKE_PREFIX_PATH=<workdir>/tools/rexglue-sdk
find_package(rexglue CONFIG REQUIRED)

add_executable(rex_extract main.cpp)
target_link_libraries(rex_extract PRIVATE rex::runtime)
set_target_properties(rex_extract PROPERTIES BUILD_RPATH "${rexglue_DIR}/../..")
