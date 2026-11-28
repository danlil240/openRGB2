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

# Warning level. With cmake_minimum_required >= 3.15 (CMP0092 NEW) CMake no
# longer puts MSVC's default /W3 into CMAKE_<LANG>_FLAGS, so without this the
# whole tree builds at the compiler default /W1. Warnings stay warnings (no
# /WX). C/C++ only -- the RC compiler does not take /W3.
# cmake/OpenRGB2BuildFlags.cmake adds that /W3 plus the opt-in /W4
# (OPENRGB2_WARNINGS_W4) and AddressSanitizer (OPENRGB2_ASAN)
# configurations; CMakePresets.json names them `default`, `w4`, `asan`.
include(${CMAKE_SOURCE_DIR}/cmake/OpenRGB2BuildFlags.cmake)

# Qt 6.8.3 / MSVC 2022 is the pinned toolkit for the UI process
# (openrgb2-ui + the OpenRGB2.UI QML module + the `ui` CTest suites). The
# engine, IPC, SDK client and the core/backend/engine/sdk tests are Qt-free:
# a Qt-free development build passes -DOPENRGB2_BUILD_UI=OFF. With the UI ON
# (the default, and what packaging needs) a missing Qt component is a
# configure ERROR -- it used to silently drop the UI and every `ui` test.
#
#   cmake -S . -B build -G "Visual Studio 17 2022" -A x64 ^
#       -DCMAKE_PREFIX_PATH=C:/Qt/6.8.3/msvc2022_64
#
# Threads::Threads (src/ipc and others) -- Qt's package config used to
# provide it implicitly, which broke every Qt-free configure.
find_package(Threads REQUIRED)

option(OPENRGB2_BUILD_UI
    "Build the Qt UI (openrgb2-ui, OpenRGB2.UI QML module, ui tests)" ON)
if(OPENRGB2_BUILD_UI)
    find_package(Qt6 6.8 REQUIRED COMPONENTS
        Core Gui Qml QmlMeta QmlModels QmlWorkerScript
        Quick QuickControls2 Network OpenGL Widgets)
else()
    message(STATUS
        "OPENRGB2_BUILD_UI=OFF: building the Qt-free engine/tools/tests only "
        "(no openrgb2-ui, no `ui` tests).")
endif()

# studio-import Task 8: Qt Quick3D is provisioned for the Layout
# viewport — an OPTIONAL component. A core/basic-UI development build
# configures without it (the ui/studio/viewport QML subtree is
# excluded and the Layout page keeps its 2D canvas); the packaged
# full-Studio build requires it (packaging asserts the module).
option(OPENRGB2_WITH_QUICK3D
    "Include the Quick3D Layout viewport (full Studio package)" ON)
set(OPENRGB2_HAVE_QUICK3D OFF)
if(OPENRGB2_BUILD_UI)
    find_package(Qt6 6.8 QUIET COMPONENTS Quick3D)
endif()
if(OPENRGB2_BUILD_UI AND OPENRGB2_WITH_QUICK3D AND Qt6Quick3D_FOUND)
    set(OPENRGB2_HAVE_QUICK3D ON)
elseif(OPENRGB2_BUILD_UI AND OPENRGB2_WITH_QUICK3D)
    message(STATUS
        "Qt6Quick3D not found — building the basic UI without the "
        "Layout 3D viewport (set OPENRGB2_WITH_QUICK3D=OFF to silence).")
endif()

enable_testing()

# CTest labels defined by the implementation plan (Milestone 0): core, sdk,
# engine, ui, backend. Targets are added as their milestones land.
# Shared Qt-free Studio model core — must precede every consumer.
add_subdirectory(src/core)
# R-9: crash dumps + restart budget, shared by engine and UI.
add_subdirectory(src/supervise)
add_subdirectory(src/engine)
if(OPENRGB2_BUILD_UI)
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
if(OPENRGB2_BUILD_UI)
    add_subdirectory(tests/ui)
endif()
# Milestone 6 — package lifecycle (managed startup, takeover,
# update/rollback, clean shutdown). Internally WIN32-gated.
add_subdirectory(tests/packaging)

# Milestone 1 benchmark producer (opt-in physical runs, offline build)
add_subdirectory(tools/bench)

# Task 3.9 verification driver (IPC client + SDK save/restore)
add_subdirectory(tools/engine_ctl)

# Every directory above is first-party: opt its compiled targets into
# the /W4 configuration (no-op unless OPENRGB2_WARNINGS_W4), and give
# every test the ASan runtime on PATH (no-op unless OPENRGB2_ASAN).
openrgb2_warnings_for_dirs(${CMAKE_SOURCE_DIR})
openrgb2_asan_test_env(${CMAKE_SOURCE_DIR})
