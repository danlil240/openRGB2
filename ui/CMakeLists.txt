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
    Core Gui Qml Quick QuickControls2 Network)

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

# studio-import Task 8: the Quick3D viewport subtree
# (studio/viewport/) ships only in the full-Studio build —
# OPENRGB2_HAVE_QUICK3D is set by the root CMakeLists when the
# component is provisioned and the opt-out is off. The subtree is
# QML-only (no C++ Quick3D linkage); Layout's 2D canvas stays the
# fallback in basic builds. The PUBLIC definition exposes the gate
# to the openrgb2-ui entry point (main.cpp) and tests.
set(_viewport_qml)
if(OPENRGB2_HAVE_QUICK3D)
    set(_viewport_qml
        studio/viewport/StudioViewport.qml
        studio/viewport/editor/CameraController.qml
        studio/viewport/editor/SelectionController.qml
        studio/viewport/editor/TransformGizmo.qml
        studio/viewport/devices/Case.qml
        studio/viewport/devices/Fan.qml
        studio/viewport/devices/Gpu.qml
        studio/viewport/devices/Keyboard.qml
        studio/viewport/devices/Monitor.qml
        studio/viewport/devices/Mouse.qml
        studio/viewport/devices/Ram.qml
        studio/viewport/devices/SelectionFrame.qml
        studio/viewport/devices/Strip.qml
        studio/viewport/materials/StudioEnvironment.qml)
endif()

qt_add_qml_module(openrgb2_ui
    URI OpenRGB2.UI
    VERSION 1.0
    OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/OpenRGB2/UI
    QML_FILES
        Main.qml
        theme/Theme.qml
        components/AppButton.qml
        components/AppCheckBox.qml
        components/AppComboBox.qml
        components/AppSlider.qml
        components/AppTextField.qml
        components/Card.qml
        components/ColorWell.qml
        components/EmptyState.qml
        components/FrameStrip.qml
        components/GroupChip.qml
        components/NavButton.qml
        components/PageHeader.qml
        components/PaletteSwatch.qml
        components/StatusPill.qml
        pages/DashboardPage.qml
        pages/DevicesPage.qml
        pages/LayoutPage.qml
        pages/LightingPage.qml
        pages/SettingsPage.qml
        studio/DeviceLibrary.qml
        studio/DevicePresetEditor.qml
        studio/LedCalibrator.qml
        studio/DeviceTree.qml
        studio/TransformInspector.qml
        studio/WorkspaceImportDialog.qml
        studio/WorkspaceActionsDialog.qml
        studio/EffectEditor.qml
        studio/EffectInspector.qml
        studio/LayerStack.qml
        studio/LookShelf.qml
        studio/PaletteEditor.qml
        ${_viewport_qml}
    SOURCES
        ${CMAKE_SOURCE_DIR}/src/ui/EngineBridge.h
        ${CMAKE_SOURCE_DIR}/src/ui/EngineBridge.cpp
        ${CMAKE_SOURCE_DIR}/src/ui/FrameFeedModel.h
        ${CMAKE_SOURCE_DIR}/src/ui/FrameFeedModel.cpp
        ${CMAKE_SOURCE_DIR}/src/ui/EngineWireClient.h
        ${CMAKE_SOURCE_DIR}/src/ui/EngineWireClient.cpp
        ${CMAKE_SOURCE_DIR}/src/ui/JsonVariant.h
        ${CMAKE_SOURCE_DIR}/src/ui/SceneEditModel.h
        ${CMAKE_SOURCE_DIR}/src/ui/SceneEditModel.cpp
        ${CMAKE_SOURCE_DIR}/src/ui/SceneEditController.h
        ${CMAKE_SOURCE_DIR}/src/ui/SceneEditController.cpp
        ${CMAKE_SOURCE_DIR}/src/ui/LayoutEditModel.h
        ${CMAKE_SOURCE_DIR}/src/ui/LayoutEditModel.cpp
        ${CMAKE_SOURCE_DIR}/src/ui/LayoutController.h
        ${CMAKE_SOURCE_DIR}/src/ui/LayoutController.cpp
        ${CMAKE_SOURCE_DIR}/src/ui/DiagnosticsController.h
        ${CMAKE_SOURCE_DIR}/src/ui/DiagnosticsController.cpp
        ${CMAKE_SOURCE_DIR}/src/ui/ProfileLibrary.h
        ${CMAKE_SOURCE_DIR}/src/ui/ProfileLibrary.cpp
        ${CMAKE_SOURCE_DIR}/src/ui/UiSettings.h
        ${CMAKE_SOURCE_DIR}/src/ui/UiSettings.cpp
        ${CMAKE_SOURCE_DIR}/src/ui/StartupTaskController.h
        ${CMAKE_SOURCE_DIR}/src/ui/StartupTaskController.cpp
        ${CMAKE_SOURCE_DIR}/src/ui/LaunchHardening.h
        ${CMAKE_SOURCE_DIR}/src/ui/LaunchHardening.cpp
        ${CMAKE_SOURCE_DIR}/src/ui/WorkspaceEditSession.h
        ${CMAKE_SOURCE_DIR}/src/ui/WorkspaceEditSession.cpp
        ${CMAKE_SOURCE_DIR}/src/ui/WorkspaceDraftStore.h
        ${CMAKE_SOURCE_DIR}/src/ui/WorkspaceDraftStore.cpp
        ${CMAKE_SOURCE_DIR}/src/ui/WorkspaceController.h
        ${CMAKE_SOURCE_DIR}/src/ui/WorkspaceController.cpp
        ${CMAKE_SOURCE_DIR}/src/ui/StudioViewportModel.h
        ${CMAKE_SOURCE_DIR}/src/ui/StudioViewportModel.cpp
        ${CMAKE_SOURCE_DIR}/src/ui/studio/SceneObjectModel.h
        ${CMAKE_SOURCE_DIR}/src/ui/studio/SceneObjectModel.cpp
        ${CMAKE_SOURCE_DIR}/src/ui/studio/PresetListModel.h
        ${CMAKE_SOURCE_DIR}/src/ui/studio/PresetListModel.cpp
        ${CMAKE_SOURCE_DIR}/src/ui/studio/ObjectEditorController.h
        ${CMAKE_SOURCE_DIR}/src/ui/studio/ObjectEditorController.cpp
        ${CMAKE_SOURCE_DIR}/src/ui/studio/EffectLayerModel.h
        ${CMAKE_SOURCE_DIR}/src/ui/studio/EffectLayerModel.cpp
        ${CMAKE_SOURCE_DIR}/src/ui/studio/EffectEditorController.h
        ${CMAKE_SOURCE_DIR}/src/ui/studio/EffectEditorController.cpp
        ${CMAKE_SOURCE_DIR}/src/ui/studio/DevicePresetController.h
        ${CMAKE_SOURCE_DIR}/src/ui/studio/DevicePresetController.cpp
        ${CMAKE_SOURCE_DIR}/src/ui/studio/CalibrationController.h
        ${CMAKE_SOURCE_DIR}/src/ui/studio/CalibrationController.cpp
        ${CMAKE_SOURCE_DIR}/src/ui/QaServer.h
        ${CMAKE_SOURCE_DIR}/src/ui/QaServer.cpp)

if(OPENRGB2_HAVE_QUICK3D)
    # QML-only usage — no C++ Quick3D linkage needed; the definition
    # exposes the gate to main.cpp (quick3dAvailable) and tests.
    target_compile_definitions(openrgb2_ui PUBLIC OPENRGB2_WITH_QUICK3D)
endif()

set_target_properties(openrgb2_ui PROPERTIES AUTOMOC ON)

# Public headers: src/ui for the bridge, src/ipc + the vendored
# single-header json for EngineWireClient.h's IpcProtocol/json
# includes. The LINK dependency is PRIVATE — the codec/transport-only
# openrgb2_ipc_wire target (no IpcServer/EngineService objects, no
# engine/backend libraries transitively): the UI process must never
# carry the engine/SDK boundary (plan section 4).
target_include_directories(openrgb2_ui PUBLIC
    ${CMAKE_SOURCE_DIR}/src/ui
    ${CMAKE_SOURCE_DIR}/src/ipc
    ${CMAKE_SOURCE_DIR}/vendor/OpenRGB/dependencies/json)

target_link_libraries(openrgb2_ui PUBLIC
    Qt6::Core
    Qt6::Gui
    Qt6::Qml
    Qt6::Quick
    # studio-import Task 4: the shared workspace draft edits
    # the Qt-free compact model (StudioDocument +
    # EditorController). PUBLIC so the entry-point target sees
    # WorkspaceEditSession.h's includes. Still no engine/SDK/
    # controller boundary — the core target carries neither.
    openrgb2_studio_core
PRIVATE
    Qt6::Network
    openrgb2_ipc_wire)
if(WIN32)
    # LaunchHardening: token elevation probe (advapi32) and the
    # Program Files known-folder lookup (shell32 + ole32).
    target_link_libraries(openrgb2_ui PRIVATE advapi32 shell32 ole32)
endif()
