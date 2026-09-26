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
