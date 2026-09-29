cmake_minimum_required(VERSION 3.24)

project(OpenRGB2
    VERSION 0.1.0
    DESCRIPTION "Polished Windows RGB application with uploaded animation and live-effects modes"
    LANGUAGES C CXX)

if(WIN32)
    enable_language(RC)
endif()

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
find_package(Qt6 6.8 QUIET COMPONENTS
    Core Gui Qml QmlMeta QmlModels QmlWorkerScript
    Quick QuickControls2 Network OpenGL Widgets)
if(NOT Qt6_FOUND)
    message(STATUS
        "Qt6 not found (pass -DCMAKE_PREFIX_PATH=C:/Qt/6.8.3/msvc2022_64). "
        "Required for the engine/UI milestones; the offline tests are Qt-free.")
endif()

enable_testing()

# CTest labels defined by the implementation plan (Milestone 0): core, sdk,
# engine, ui, backend. Targets are added as their milestones land.
# Shared Qt-free Studio model core — must precede every consumer.
add_subdirectory(src/core)
add_subdirectory(src/engine)
if(Qt6_FOUND)
    # Milestone 4 — the UI process (OpenRGB2.UI QML module + the
    # openrgb2-ui executable) and its CTest label.
    add_subdirectory(ui)
    add_subdirectory(src/ui)
endif()
add_subdirectory(src/backends/openrgb)
add_subdirectory(src/ipc)
add_subdirectory(src/platform/windows)
add_subdirectory(tests/core)
add_subdirectory(tests/backend)
add_subdirectory(tests/engine)
add_subdirectory(tests/sdk)
if(Qt6_FOUND)
    add_subdirectory(tests/ui)
endif()
# Milestone 6 — package lifecycle (managed startup, takeover,
# update/rollback, clean shutdown). Internally WIN32-gated.
add_subdirectory(tests/packaging)

# Milestone 1 benchmark producer (opt-in physical runs, offline build)
add_subdirectory(tools/bench)

# Task 3.9 verification driver (IPC client + SDK save/restore)
add_subdirectory(tools/engine_ctl)
