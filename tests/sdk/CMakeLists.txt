# Qt-free SDK-client tests (Milestone 3.2, CTest label `sdk`).
# Plain executables like the other suites — CHECK-macro mains,
# no test framework. The in-process fake server
# (fake_sdk_server.h) speaks the protocol-4 wire shape through an
# injected in-memory transport: deterministic, offline, no real
# sockets, no backend, no hardware.

function(sdk_suite NAME)
    add_executable(${NAME} ${NAME}.cpp)
    target_link_libraries(${NAME} PRIVATE
        openrgb2_backend_openrgb
        openrgb2_engine
        ws2_32)
    target_include_directories(${NAME} PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_definitions(${NAME} PRIVATE NDEBUG)
    add_test(NAME ${NAME} COMMAND ${NAME})
    set_tests_properties(${NAME} PROPERTIES
        LABELS sdk
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
endfunction()

sdk_suite(sdk_wire_test)
sdk_suite(sdk_client_test)
sdk_suite(sdk_reconnect_test)
