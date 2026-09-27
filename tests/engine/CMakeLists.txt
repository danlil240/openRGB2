# Qt-free engine tests (Milestone 3). Plain executables like the core
# suites — CHECK-macro mains, no test framework.

function(engine_suite NAME)
    add_executable(${NAME} ${NAME}.cpp)
    target_link_libraries(${NAME} PRIVATE openrgb2_engine)
    target_compile_definitions(${NAME} PRIVATE NDEBUG)
    add_test(NAME ${NAME} COMMAND ${NAME})
    set_tests_properties(${NAME} PROPERTIES
        LABELS engine
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
endfunction()

engine_suite(engine_types_test)
engine_suite(frame_sink_test)
engine_suite(scene_worker_test)
engine_suite(delivery_pipeline_test)
engine_suite(device_mapping_test)

# Task 3.6 — playback mode integration + cross-device sync.
engine_suite(playback_mode_test)
set_tests_properties(playback_mode_test PROPERTIES
    ENVIRONMENT "DLS_EFFECT_DIR=${CMAKE_SOURCE_DIR}/src/core/presets/effects")

# Task 3.7 — power lifecycle + ordered bounded shutdown.
engine_suite(power_lifecycle_test)
engine_suite(shutdown_test)

# Task 3.8 — profile store: atomicity, migration, offline
# bindings, last-known-good, durable PlaybackMode, scene loader.
engine_suite(profile_store_test)

# Task 3.9 — ownership: competing-owner classifier, managed-config
# sole-writer check, single-instance guard, listener-PID lookup.
engine_suite(ownership_scan_test)

# Task 4.6 — reactive inputs: bounded queue/flood, enable/disable
# lifecycle against injected fakes, the capture_paused gate end to
# end at the LED level, params/display bounds, persist/restore,
# and the honest live-only playback reason for reactive scenes.
engine_suite(reactive_inputs_test)

# Task 4.9-parallel IPC surface for the same task: REACTIVE_COMMAND
# verb bounds, honest Unavailable paths, STATE/DIAGNOSTICS surface,
# the reactive event, and the reactive-scene playback reason.
engine_suite(ipc_reactive_test)
target_link_libraries(ipc_reactive_test PRIVATE openrgb2_ipc)
target_include_directories(ipc_reactive_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR})

# Task 3.9 — offline end-to-end: the REAL EngineHost composition
# against the scripted in-process SDK backend (startup -> attach ->
# enumerate -> bind -> frames -> ordered shutdown).
add_executable(e2e_fake_backend_test e2e_fake_backend_test.cpp)
target_link_libraries(e2e_fake_backend_test PRIVATE
    openrgb2_host)
target_include_directories(e2e_fake_backend_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(e2e_fake_backend_test PRIVATE NDEBUG)
add_test(NAME e2e_fake_backend_test COMMAND e2e_fake_backend_test)
set_tests_properties(e2e_fake_backend_test PROPERTIES
    LABELS engine
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

# Task 3.5 — IPC layer. ipc_server_test additionally needs the
# named-pipe transport + ACL APIs.
engine_suite(ipc_protocol_test)
engine_suite(ipc_engine_service_test)

add_executable(ipc_server_test ipc_server_test.cpp)
target_link_libraries(ipc_server_test PRIVATE
    openrgb2_engine
    openrgb2_ipc
    advapi32)
target_include_directories(ipc_server_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(ipc_server_test PRIVATE NDEBUG)
add_test(NAME ipc_server_test COMMAND ipc_server_test)
set_tests_properties(ipc_server_test PROPERTIES
    LABELS engine
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(ipc_protocol_test PRIVATE openrgb2_ipc)
target_include_directories(ipc_protocol_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(ipc_engine_service_test PRIVATE openrgb2_ipc)
# Task 4.7 review: the ownership wire surface drives the real
# EngineHost::OwnershipJson serializer as the provider — a
# scripted body would ship a field-name typo green.
target_link_libraries(ipc_engine_service_test PRIVATE openrgb2_host)
