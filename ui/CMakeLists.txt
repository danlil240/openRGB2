# OpenRGB2.UI — the Milestone-4 QML module (plan section 5).
# The QML tree lives in this directory (theme design tokens,
# reusable components, the four views, the application shell).
# The module's static backing library also compiles the C++ IPC
# client bridge from src/ui so openrgb2-ui and tests/ui share
# ONE implementation of the wire contract.
#
# Module layout mirrors the source tree: types import
#   OpenRGB2.UI.theme      (singleton Theme)
#   OpenRGB2.UI.components (Card, StatusPill, AppButton, ...)
#   OpenRGB2.UI.pages      (Dashboard/Lighting/Layout/Devices)

find_package(Qt6 6.8 REQUIRED COMPONENTS
    Core Gui Qml Quick QuickControls2)

# Scoped here (not project-wide) so the Qt-free engine/core
# targets never pick up AUTOMOC.
qt_standard_project_setup()

# Theme.qml is the module singleton — property must be set
# BEFORE qt_add_qml_module consumes the file list.
set_source_files_properties(theme/Theme.qml PROPERTIES
    QT_QML_SINGLETON_TYPE TRUE)

qt_add_library(openrgb2_ui STATIC)

# Subdirectories (theme/components/pages) are part of the ONE
# OpenRGB2.UI module — no per-directory qmldir fragments.
if(COMMAND qt_policy)
    qt_policy(SET QTP0001 NEW)
    qt_policy(SET QTP0004 NEW)
endif()

qt_add_qml_module(openrgb2_ui
    URI OpenRGB2.UI
    VERSION 1.0
    OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/OpenRGB2/UI
    QML_FILES
        Main.qml
        theme/Theme.qml
        components/AppButton.qml
        components/AppSlider.qml
        components/Card.qml
        components/ColorWell.qml
        components/EmptyState.qml
        components/NavButton.qml
        components/PageHeader.qml
        components/StatusPill.qml
        pages/DashboardPage.qml
        pages/DevicesPage.qml
        pages/LayoutPage.qml
        pages/LightingPage.qml
    SOURCES
        ${CMAKE_SOURCE_DIR}/src/ui/EngineBridge.h
        ${CMAKE_SOURCE_DIR}/src/ui/EngineBridge.cpp
        ${CMAKE_SOURCE_DIR}/src/ui/EngineWireClient.h
        ${CMAKE_SOURCE_DIR}/src/ui/EngineWireClient.cpp
        ${CMAKE_SOURCE_DIR}/src/ui/JsonVariant.h)

set_target_properties(openrgb2_ui PROPERTIES AUTOMOC ON)

target_include_directories(openrgb2_ui PUBLIC
    ${CMAKE_SOURCE_DIR}/src/ui)

target_link_libraries(openrgb2_ui PUBLIC
    Qt6::Core
    Qt6::Gui
    Qt6::Qml
    Qt6::Quick
    openrgb2_ipc)
