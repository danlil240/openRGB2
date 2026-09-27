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
