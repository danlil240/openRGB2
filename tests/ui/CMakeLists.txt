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
