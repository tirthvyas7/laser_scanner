cmake_minimum_required(VERSION 3.16)
project(line_scanner_pipeline LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

find_package(Threads REQUIRED)

# yaml-cpp — vendored via FetchContent so the build is self-contained on
# every platform (Linux, Windows, macOS) with no manual install steps.
include(FetchContent)
set(YAML_CPP_BUILD_TESTS   OFF CACHE BOOL "" FORCE)
set(YAML_CPP_BUILD_TOOLS   OFF CACHE BOOL "" FORCE)
set(YAML_CPP_BUILD_CONTRIB OFF CACHE BOOL "" FORCE)
set(YAML_CPP_INSTALL       OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    yaml-cpp
    GIT_REPOSITORY https://github.com/jbeder/yaml-cpp.git
    GIT_TAG        0.8.0
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(yaml-cpp)

add_executable(line_scanner
    src/main.cpp
    src/orchestrator.cpp
    src/block.cpp
    src/block_factory.cpp
    src/metrics_logger.cpp
    blocks/data_generation/csv_source.cpp
    blocks/data_generation/rng_source.cpp
    blocks/data_generation/data_generation_block.cpp
    blocks/filter_threshold/filter_threshold_block.cpp
    blocks/labelling/labelling_block.cpp
    blocks/tracing_computation/tracing_block.cpp
    blocks/output_logger/defect_logger/defect_logger.cpp
)

target_include_directories(line_scanner PRIVATE
    include
    channels
    blocks/data_generation
    blocks/filter_threshold
    blocks/labelling
    blocks/tracing_computation
    blocks/output_logger
)

target_link_libraries(line_scanner PRIVATE Threads::Threads yaml-cpp)

if(MSVC)
    target_compile_options(line_scanner PRIVATE /O2 /W4 /permissive-)
else()
    target_compile_options(line_scanner PRIVATE -O2 -Wall -Wextra)
endif()
