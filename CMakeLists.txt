cmake_minimum_required(VERSION 3.24)

project(OpenRGB2
    VERSION 0.1.0
    DESCRIPTION "Polished Windows RGB application with uploaded animation and live-effects modes"
    LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Qt 6.8.3 / MSVC 2022 is the pinned toolkit for the application (engine and
# UI milestones). The offline core and backend tests are Qt-free, so Qt is not
# required until those targets land.
#
#   cmake -S . -B build -G "Visual Studio 17 2022" -A x64 ^
#       -DCMAKE_PREFIX_PATH=C:/Qt/6.8.3/msvc2022_64
#
find_package(Qt6 6.8 QUIET COMPONENTS Core)
if(NOT Qt6_FOUND)
    message(STATUS
        "Qt6 not found (pass -DCMAKE_PREFIX_PATH=C:/Qt/6.8.3/msvc2022_64). "
        "Required for the engine/UI milestones; the offline tests are Qt-free.")
endif()

enable_testing()

# CTest labels defined by the implementation plan (Milestone 0): core, sdk,
# engine, ui, backend. Targets are added as their milestones land.
add_subdirectory(src/engine)
add_subdirectory(src/backends/openrgb)
add_subdirectory(src/ipc)
add_subdirectory(src/platform/windows)
add_subdirectory(tests/core)
add_subdirectory(tests/backend)
add_subdirectory(tests/engine)
add_subdirectory(tests/sdk)

# Milestone 1 benchmark producer (opt-in physical runs, offline build)
add_subdirectory(tools/bench)

# Task 3.9 verification driver (IPC client + SDK save/restore)
add_subdirectory(tools/engine_ctl)
