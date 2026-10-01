# UI tests (Milestone 4) — CTest label `ui`.
#
#   ui_wire_client_test  wire-level client protocol against a
#                        scripted in-process server channel
#                        (fragmentation, malformed, resync,
#                        version mismatch, disconnect)
#   ui_bridge_test       the QObject bridge: connect/drop/
#                        reconnect + bounded backoff over a
#                        QCoreApplication loop
#   qml_smoke_test       headless QML load of the shell and all
#                        four pages with no engine — the honest
#                        degraded state must render

find_package(Qt6 6.8 REQUIRED COMPONENTS
    Core Gui Qml Quick QuickControls2 Test)

# Qt's DLLs live outside the build tree; prepend the Qt bin dir
# to PATH for the test processes only.
get_target_property(_qtcore_loc Qt6::Core LOCATION)
get_filename_component(QT_BIN_DIR "${_qtcore_loc}" DIRECTORY)

function(ui_suite NAME)
    add_executable(${NAME} ${NAME}.cpp)
    target_link_libraries(${NAME} PRIVATE
        openrgb2_ui
        openrgb2_ipc_wire)
    target_include_directories(${NAME} PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}/src/ui)
    target_compile_definitions(${NAME} PRIVATE NDEBUG)
    set_target_properties(${NAME} PROPERTIES AUTOMOC ON)
    add_test(NAME ${NAME} COMMAND ${NAME})
    set_tests_properties(${NAME} PROPERTIES
        LABELS ui
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:${QT_BIN_DIR}")
endfunction()

ui_suite(ui_wire_client_test)
ui_suite(ui_bridge_test)
ui_suite(ui_startup_task_test)
# Task 4.2: the scene-edit model builds the SET_SCENE body and
# owns the named-scene library file; the bridge suite drives a
# real EngineBridge + SceneEditController over an in-process
# wire pipe (edit -> SET_SCENE -> refresh, save-failure
# retention, supported-param filtering).
ui_suite(ui_scene_edit_test)
ui_suite(ui_scene_bridge_test)
# Task 4.3: playback selector + mode behavior — request ->
# pending -> acked state machine, capability/reason mirror,
# failed/reconnecting surfaces, new-instance animation
# default vs. migrated-profile Live, scene-scoped param
# latches.
ui_suite(ui_playback_test)
# Task 4.4: the Qt-free layout edit model (items view, op
# builders' calibration/mirror boundary validation, bounded
# undo/redo history) + a real EngineBridge + LayoutController
# over the in-process pipe (exact LAYOUT_COMMAND bodies,
# scene-event repaint, SET_SCENE undo/redo reverts, refused
# ops leaving no ghost history).
ui_suite(ui_layout_test)

# Task 4.6: the reactive-input surface — the bridge's reactive
# store lands from STATE + pushed events, reactiveCommand issues
# the exact REACTIVE_COMMAND bodies, engine ERRORs surface
# honestly, and unavailable stays unavailable (no fabricated
# toggles). A real EngineBridge over the in-process pipe.
ui_suite(ui_reactive_test)

# Task 4.7: the Devices & Diagnostics surface — the four
# labeled rates (configured / physical / transport-admitted /
# preview) with honest provenance, engine-reported ownership
# conflicts verbatim, controller+binding join, bounded event
# journal, and the redacted export bundle. A real EngineBridge
# + DiagnosticsController over the in-process pipe.
ui_suite(ui_diagnostics_test)

# Task 4.8: the hardening suite — Quit-All issues IPC QUIT and
# reads the ordered reply; an engine restart mid-edit keeps
# the working copy while the restored session reports ENGINE
# truth and emits no ghost mutations; rejected/lost saves keep
# edits with honest errors; a 400-event flood measures bounded
# control-request latency; UiSettings round-trip + bound;
# ProfileLibrary failed-write rollback; stale phase_detail.
ui_suite(ui_hardening_test)

# studio-import Task 4: the shared workspace draft — the
# Qt-free WorkspaceEditSession (one draft for every editing
# page, one undo record per committed gesture, bounded
# history), the debounced WorkspaceDraftStore, and a real
# EngineBridge + WorkspaceController over the in-process pipe
# (capability gate, transfer-based document fetch, commit-job
# correlation, save rejection keeping the draft, edits during
# save, stale echoes + reconnect reconcile by content hash,
# explicit crash recovery, legacy-engine fallback). The suite
# compiles the compact-document fixtures itself, so it links
# the Qt-free studio core directly.
add_executable(ui_workspace_test ui_workspace_test.cpp)
target_link_libraries(ui_workspace_test PRIVATE
    openrgb2_ui
    openrgb2_ipc_wire
    openrgb2_studio_core)
target_include_directories(ui_workspace_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/src/ui)
target_compile_definitions(ui_workspace_test PRIVATE NDEBUG)
set_target_properties(ui_workspace_test PROPERTIES AUTOMOC ON)
add_test(NAME ui_workspace_test COMMAND ui_workspace_test)
set_tests_properties(ui_workspace_test PROPERTIES
    LABELS ui
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    ENVIRONMENT_MODIFICATION
        "PATH=path_list_prepend:${QT_BIN_DIR}")

# studio-import Task 7: the bounded draft preview surface — a
# real EngineBridge + WorkspaceController + StudioViewportModel
# over the in-process pipe against a scripted engine
# (begin/update/end verbs, chunked sequence reassembly, stale
# drops, reduced labeling, status pauses, refusal honesty,
# reconnect re-begin, and the hard separation from the
# transport-accepted frame_status feed).
add_executable(ui_studio_preview_test ui_studio_preview_test.cpp)
target_link_libraries(ui_studio_preview_test PRIVATE
    openrgb2_ui
    openrgb2_ipc_wire
    openrgb2_studio_core)
target_include_directories(ui_studio_preview_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/src/ui)
target_compile_definitions(ui_studio_preview_test PRIVATE NDEBUG)
set_target_properties(ui_studio_preview_test PROPERTIES AUTOMOC ON)
add_test(NAME ui_studio_preview_test COMMAND ui_studio_preview_test)
set_tests_properties(ui_studio_preview_test PROPERTIES
    LABELS ui
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    ENVIRONMENT_MODIFICATION
        "PATH=path_list_prepend:${QT_BIN_DIR}")

# The QML smoke test additionally needs the module's compiled
# plugin + Qt Quick runtime on its link line.
add_executable(qml_smoke_test qml_smoke_test.cpp)
target_link_libraries(qml_smoke_test PRIVATE
    openrgb2_ui
    openrgb2_uiplugin
    Qt6::Gui Qt6::Qml Qt6::Quick Qt6::QuickControls2)
target_include_directories(qml_smoke_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/src/ui)
target_compile_definitions(qml_smoke_test PRIVATE NDEBUG)
set_target_properties(qml_smoke_test PROPERTIES AUTOMOC ON)
qt_import_qml_plugins(qml_smoke_test)
add_test(NAME qml_smoke_test COMMAND qml_smoke_test)
set_tests_properties(qml_smoke_test PROPERTIES
    LABELS ui
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    ENVIRONMENT_MODIFICATION
        "PATH=path_list_prepend:${QT_BIN_DIR}")

# Task 4.5: the focused-save regression — the REAL shell page
# (nameField focus + uncommitted text, then the Save button's
# commitPendingEdits path) against a scripted engine over the
# in-process pipe; the SET_SCENE body must carry the EDITED
# value, never the stale one.
add_executable(qml_focused_save_test qml_focused_save_test.cpp)
target_link_libraries(qml_focused_save_test PRIVATE
    openrgb2_ui
    openrgb2_uiplugin
    openrgb2_ipc_wire
    Qt6::Gui Qt6::Qml Qt6::Quick Qt6::QuickControls2)
target_include_directories(qml_focused_save_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/src/ui)
target_compile_definitions(qml_focused_save_test PRIVATE NDEBUG)
set_target_properties(qml_focused_save_test PROPERTIES AUTOMOC ON)
qt_import_qml_plugins(qml_focused_save_test)
add_test(NAME qml_focused_save_test COMMAND qml_focused_save_test)
set_tests_properties(qml_focused_save_test PROPERTIES
    LABELS ui
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    ENVIRONMENT_MODIFICATION
        "PATH=path_list_prepend:${QT_BIN_DIR}")

# studio-import Task 4 fix round 2: the real shell headless
# against a scripted studio_workspace_v1 engine — the conflict
# strip's "Keep my draft (Save)" and Lighting's Save button must
# reach a commit job even when the name field already matches
# the draft (a no-op rename is success, not refusal).
add_executable(qml_workspace_save_test qml_workspace_save_test.cpp)
target_link_libraries(qml_workspace_save_test PRIVATE
    openrgb2_ui
    openrgb2_uiplugin
    openrgb2_ipc_wire
    Qt6::Gui Qt6::Qml Qt6::Quick Qt6::QuickControls2)
target_include_directories(qml_workspace_save_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/src/ui)
target_compile_definitions(qml_workspace_save_test PRIVATE NDEBUG)
set_target_properties(qml_workspace_save_test PROPERTIES AUTOMOC ON)
qt_import_qml_plugins(qml_workspace_save_test)
add_test(NAME qml_workspace_save_test COMMAND qml_workspace_save_test)
set_tests_properties(qml_workspace_save_test PROPERTIES
    LABELS ui
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    ENVIRONMENT_MODIFICATION
        "PATH=path_list_prepend:${QT_BIN_DIR}")

# N02 regression: the real LayoutPage.qml 2D canvas over a
# scripted studio_workspace_v1 engine — a draft move via
# layoutCtl.moveObject must repaint the canvasObj_<id>
# delegate immediately (the footprint offset is draft-true,
# never pinned to the applied resolved scene until Save).
add_executable(qml_layout_drag_test qml_layout_drag_test.cpp)
target_link_libraries(qml_layout_drag_test PRIVATE
    openrgb2_ui
    openrgb2_uiplugin
    openrgb2_ipc_wire
    Qt6::Gui Qt6::Qml Qt6::Quick Qt6::QuickControls2)
target_include_directories(qml_layout_drag_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/src/ui)
target_compile_definitions(qml_layout_drag_test PRIVATE NDEBUG)
set_target_properties(qml_layout_drag_test PROPERTIES AUTOMOC ON)
qt_import_qml_plugins(qml_layout_drag_test)
add_test(NAME qml_layout_drag_test COMMAND qml_layout_drag_test)
set_tests_properties(qml_layout_drag_test PROPERTIES
    LABELS ui
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    ENVIRONMENT_MODIFICATION
        "PATH=path_list_prepend:${QT_BIN_DIR}")

# Task 4.7: the real DevicesPage.qml headless — the four rate
# fields render with honest labels, unmeasured stays
# unmeasured (never zeros), ownership absence is honest, and
# export is gated on a live engine connection.
add_executable(qml_diagnostics_test qml_diagnostics_test.cpp)
target_link_libraries(qml_diagnostics_test PRIVATE
    openrgb2_ui
    openrgb2_uiplugin
    Qt6::Gui Qt6::Qml Qt6::Quick Qt6::QuickControls2)
target_include_directories(qml_diagnostics_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/src/ui)
target_compile_definitions(qml_diagnostics_test PRIVATE NDEBUG)
set_target_properties(qml_diagnostics_test PROPERTIES AUTOMOC ON)
qt_import_qml_plugins(qml_diagnostics_test)
add_test(NAME qml_diagnostics_test COMMAND qml_diagnostics_test)
set_tests_properties(qml_diagnostics_test PROPERTIES
    LABELS ui
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    ENVIRONMENT_MODIFICATION
        "PATH=path_list_prepend:${QT_BIN_DIR}")

# Task 4.8: the real shell headless — Tab/Space focus chain,
# Ctrl+1..5 view switching, Ctrl+S focused-commit save,
# accessible-name resolution, theme contrast (WCAG ratios),
# nameField remote-sync, and Quit-All driving an actual IPC
# QUIT to window close. Registered at 1.00 plus the three
# scale factors under QT_SCALE_FACTOR 1.25 / 1.5 / 2.0 —
# the same binary asserts the min-size contract under each
# scale pass.
add_executable(qml_hardening_test qml_hardening_test.cpp)
target_link_libraries(qml_hardening_test PRIVATE
    openrgb2_ui
    openrgb2_uiplugin
    openrgb2_ipc_wire
    Qt6::Gui Qt6::Qml Qt6::Quick Qt6::QuickControls2 Qt6::Test)
target_include_directories(qml_hardening_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/src/ui)
target_compile_definitions(qml_hardening_test PRIVATE NDEBUG)
set_target_properties(qml_hardening_test PROPERTIES AUTOMOC ON)
qt_import_qml_plugins(qml_hardening_test)
foreach(scale 1.00 1.25 1.50 2.00)
    string(REPLACE "." "_" scale_tag "${scale}")
    add_test(NAME qml_hardening_test_scale${scale_tag}
             COMMAND qml_hardening_test)
    set_tests_properties(qml_hardening_test_scale${scale_tag}
        PROPERTIES
        LABELS ui
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:${QT_BIN_DIR};QT_SCALE_FACTOR=set:${scale}")
endforeach()

# studio-import Task 5: the scene/object editing surface —
# the real ObjectEditorController + SceneObjectModel +
# PresetListModel over the shared WorkspaceEditSession (one
# undo record per op/gesture, preview never records, rename
# re-keying, mirrored copies never mint bindings, library
# floor lift, favorites as non-undo prefs), plus the three
# adapted QML panels headless against stub bridges.
# DLS_PRESET_DIR feeds the controller's registry probe, the
# same convention tests/core uses.
add_executable(ui_studio_objects_test ui_studio_objects_test.cpp)
target_link_libraries(ui_studio_objects_test PRIVATE
    openrgb2_ui
    openrgb2_uiplugin
    openrgb2_studio_core
    Qt6::Gui Qt6::Qml Qt6::Quick Qt6::QuickControls2 Qt6::Test)
target_include_directories(ui_studio_objects_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/src/ui)
target_compile_definitions(ui_studio_objects_test PRIVATE NDEBUG)
set_target_properties(ui_studio_objects_test PROPERTIES AUTOMOC ON)
qt_import_qml_plugins(ui_studio_objects_test)
add_test(NAME ui_studio_objects_test COMMAND ui_studio_objects_test)
set_tests_properties(ui_studio_objects_test PROPERTIES
    LABELS ui
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    ENVIRONMENT_MODIFICATION
        "PATH=path_list_prepend:${QT_BIN_DIR};DLS_PRESET_DIR=set:${CMAKE_SOURCE_DIR}/src/core/presets/devices")

# studio-import Task 6: the effect-layer editing surface — the
# real EffectEditorController + EffectLayerModel over the shared
# WorkspaceEditSession (first-edit materialization + undo back
# to preset resolution, one record per gesture, stop/path ops,
# reset/remix/saveLookAs, persisted input settings), plus the
# five adapted QML panels headless. DLS_EFFECT_DIR feeds the
# look registry probe, the same convention tests/core uses.
add_executable(ui_studio_effects_test ui_studio_effects_test.cpp)
target_link_libraries(ui_studio_effects_test PRIVATE
    openrgb2_ui
    openrgb2_uiplugin
    openrgb2_studio_core
    Qt6::Gui Qt6::Qml Qt6::Quick Qt6::QuickControls2 Qt6::Test)
target_include_directories(ui_studio_effects_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/src/ui)
target_compile_definitions(ui_studio_effects_test PRIVATE NDEBUG)
set_target_properties(ui_studio_effects_test PROPERTIES AUTOMOC ON)
qt_import_qml_plugins(ui_studio_effects_test)
add_test(NAME ui_studio_effects_test COMMAND ui_studio_effects_test)
set_tests_properties(ui_studio_effects_test PROPERTIES
    LABELS ui
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    ENVIRONMENT_MODIFICATION
        "PATH=path_list_prepend:${QT_BIN_DIR};DLS_EFFECT_DIR=set:${CMAKE_SOURCE_DIR}/src/core/presets/effects")

# studio-import Task 9: device-TYPE authoring + the portable
# workspace dialogs — the real DevicePresetController offline
# (candidate validation/private preview, offline job refusals,
# variant/assembly guards, fed resource_get + job events), plus
# DevicePresetEditor + WorkspaceImportDialog headless against
# stub controllers. DLS_PRESET_DIR feeds the registry probe.
add_executable(ui_studio_presets_test ui_studio_presets_test.cpp)
target_link_libraries(ui_studio_presets_test PRIVATE
    openrgb2_ui
    openrgb2_uiplugin
    openrgb2_studio_core
    Qt6::Gui Qt6::Qml Qt6::Quick Qt6::QuickControls2 Qt6::Test)
target_include_directories(ui_studio_presets_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/src/ui)
target_compile_definitions(ui_studio_presets_test PRIVATE NDEBUG)
set_target_properties(ui_studio_presets_test PROPERTIES AUTOMOC ON)
qt_import_qml_plugins(ui_studio_presets_test)
add_test(NAME ui_studio_presets_test COMMAND ui_studio_presets_test)
set_tests_properties(ui_studio_presets_test PROPERTIES
    LABELS ui
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    ENVIRONMENT_MODIFICATION
        "PATH=path_list_prepend:${QT_BIN_DIR};DLS_PRESET_DIR=set:${CMAKE_SOURCE_DIR}/src/core/presets/devices")

# studio-import Task 9 fix round 1: the REAL end-to-end
# portable-bundle flow — the actual WorkspaceImportDialog.qml
# drives the actual WorkspaceController over the in-process pipe
# against the actual IpcServer + EngineService + WorkspaceService
# on a temp managed root (base64 decode, report/conflict keys,
# choices, importFinished signature, open+list, packed export,
# preset_import for types-only bundles). Stubs cannot catch these
# contract mismatches, so every boundary is the production one.
add_executable(ui_workspace_import_test
    ui_workspace_import_test.cpp)
target_link_libraries(ui_workspace_import_test PRIVATE
    openrgb2_ui
    openrgb2_uiplugin
    openrgb2_ipc_wire
    openrgb2_ipc
    openrgb2_engine
    openrgb2_studio_core
    Qt6::Gui Qt6::Qml Qt6::Quick Qt6::QuickControls2 Qt6::Test)
target_include_directories(ui_workspace_import_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/src/ui
    ${CMAKE_SOURCE_DIR}/src/ipc
    ${CMAKE_SOURCE_DIR}/src/engine)
target_compile_definitions(ui_workspace_import_test PRIVATE NDEBUG)
set_target_properties(ui_workspace_import_test PROPERTIES
    AUTOMOC ON)
qt_import_qml_plugins(ui_workspace_import_test)
add_test(NAME ui_workspace_import_test
         COMMAND ui_workspace_import_test)
set_tests_properties(ui_workspace_import_test PROPERTIES
    LABELS ui
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    ENVIRONMENT_MODIFICATION
        "PATH=path_list_prepend:${QT_BIN_DIR}")

# studio-import Task 11: the physical LED-calibration
# wizard — the real CalibrationController offline (one
# status shape for replies/events, FeedStatusForTesting
# session transitions, owner vs. non-owner views, honest
# offline refusals) plus the adapted source
# tst_calibrator.qml cases against the REAL
# DevicePresetEditor + LedCalibrator headless with plain-JS
# stubs (assign fast path, steal, verify, place, apply to
# the candidate draft, out-of-range rejection, cancel-
# while-starting, draft-zone follow, shapes, safe close).
add_executable(ui_studio_calibration_test
    ui_studio_calibration_test.cpp)
target_link_libraries(ui_studio_calibration_test PRIVATE
    openrgb2_ui
    openrgb2_uiplugin
    Qt6::Gui Qt6::Qml Qt6::Quick Qt6::QuickControls2 Qt6::Test)
target_include_directories(ui_studio_calibration_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/src/ui)
target_compile_definitions(ui_studio_calibration_test PRIVATE NDEBUG)
set_target_properties(ui_studio_calibration_test PROPERTIES AUTOMOC ON)
qt_import_qml_plugins(ui_studio_calibration_test)
add_test(NAME ui_studio_calibration_test COMMAND ui_studio_calibration_test)
set_tests_properties(ui_studio_calibration_test PROPERTIES
    LABELS ui
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    ENVIRONMENT_MODIFICATION
        "PATH=path_list_prepend:${QT_BIN_DIR}")

# studio-import Task 10: file actions + external-change
# recovery — the real EngineBridge + WorkspaceController over
# the in-process pipe against the real IpcServer +
# EngineService + WorkspaceService + WorkspaceStore on a temp
# managed root. Covers the ported ConfigStore cases (external
# replacement/deletion reported once and NEVER applied,
# same-content suppression, last-valid backup restore
# committed forward, dirty-draft replace latching, durable
# draft survival across workspace switches and sessions).
add_executable(ui_workspace_recovery_test
    ui_workspace_recovery_test.cpp)
target_link_libraries(ui_workspace_recovery_test PRIVATE
    openrgb2_ui
    openrgb2_ipc_wire
    openrgb2_ipc
    openrgb2_engine
    openrgb2_studio_core
    Qt6::Gui)
target_include_directories(ui_workspace_recovery_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/src/ui
    ${CMAKE_SOURCE_DIR}/src/ipc
    ${CMAKE_SOURCE_DIR}/src/engine)
target_compile_definitions(ui_workspace_recovery_test
    PRIVATE NDEBUG)
set_target_properties(ui_workspace_recovery_test PROPERTIES
    AUTOMOC ON)
add_test(NAME ui_workspace_recovery_test
         COMMAND ui_workspace_recovery_test)
set_tests_properties(ui_workspace_recovery_test PROPERTIES
    LABELS ui
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    ENVIRONMENT_MODIFICATION
        "PATH=path_list_prepend:${QT_BIN_DIR}")

# studio-import Task 8: the Quick3D Layout viewport — the real
# WorkspaceController/ObjectEditorController offline drive the
# adapted SelectionController/CameraController (gesture routing,
# one undo record, Escape restore, deferred pose persistence,
# paint exclusivity) plus the real StudioViewport.qml headless
# (delegates from the shared model, meta.camera persistence,
# quality-tier writes, the pick seam, the undo gate). Only in the
# Quick3D build — the subtree isn't in the module otherwise.
if(OPENRGB2_HAVE_QUICK3D)
    add_executable(qml_studio_viewport_test
        qml_studio_viewport_test.cpp)
    target_link_libraries(qml_studio_viewport_test PRIVATE
        openrgb2_ui
        openrgb2_uiplugin
        openrgb2_studio_core
        Qt6::Gui Qt6::Qml Qt6::Quick Qt6::QuickControls2 Qt6::Test)
    target_include_directories(qml_studio_viewport_test PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}/src/ui)
    target_compile_definitions(qml_studio_viewport_test PRIVATE NDEBUG)
    set_target_properties(qml_studio_viewport_test PROPERTIES
        AUTOMOC ON)
    qt_import_qml_plugins(qml_studio_viewport_test)
    add_test(NAME qml_studio_viewport_test
             COMMAND qml_studio_viewport_test)
    set_tests_properties(qml_studio_viewport_test PROPERTIES
        LABELS ui
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:${QT_BIN_DIR};DLS_PRESET_DIR=set:${CMAKE_SOURCE_DIR}/src/core/presets/devices")

    # The pure-QML halves of the adapted source suite run under
    # Qt's signed qmltestrunner (tst_controllers, tst_devices,
    # tst_viewport). Resolved against the source tree — the same
    # files the module packs; the path is relative inside the test
    # files themselves. Skipped silently when the tool is absent.
    find_program(QMLTESTRUNNER_EXE
        NAMES qmltestrunner qmltestrunner.exe
        HINTS "${QT_BIN_DIR}")
    if(QMLTESTRUNNER_EXE)
        add_test(NAME qml_studio_qml
            COMMAND "${QMLTESTRUNNER_EXE}"
                    -input "${CMAKE_CURRENT_SOURCE_DIR}/studio_qml")
        set_tests_properties(qml_studio_qml PROPERTIES
            LABELS ui
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
            ENVIRONMENT_MODIFICATION
                "PATH=path_list_prepend:${QT_BIN_DIR}")
    endif()
endif()

# QA harness (QaServer) — the real shell headless, driven over
# the loopback QA socket like tools/qa/qa.py drives a live
# window: selector resolution, synthesized click/type/key,
# property get/set, wait, eval, honest error replies.
add_executable(ui_qa_harness_test ui_qa_harness_test.cpp)
target_link_libraries(ui_qa_harness_test PRIVATE
    openrgb2_ui
    openrgb2_uiplugin
    Qt6::Gui Qt6::Qml Qt6::Quick Qt6::QuickControls2 Qt6::Network)
target_include_directories(ui_qa_harness_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/src/ui)
target_compile_definitions(ui_qa_harness_test PRIVATE NDEBUG)
set_target_properties(ui_qa_harness_test PROPERTIES AUTOMOC ON)
qt_import_qml_plugins(ui_qa_harness_test)
add_test(NAME ui_qa_harness_test COMMAND ui_qa_harness_test)
set_tests_properties(ui_qa_harness_test PROPERTIES
    LABELS ui
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    ENVIRONMENT_MODIFICATION
        "PATH=path_list_prepend:${QT_BIN_DIR}")
