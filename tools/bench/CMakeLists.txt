# Milestone 1 benchmark producer. Physical runs are opt-in and separate
# from CTest; this target only builds the tool. The wire layer has its
# own offline test (tests/backend/bench_sdk_wire_test.cpp).

add_executable(lianli_bench
    bench_producer.cpp
    sdk_client.cpp
    sdk_wire.cpp)

target_include_directories(lianli_bench PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

target_compile_definitions(lianli_bench PRIVATE NDEBUG)

target_link_libraries(lianli_bench PRIVATE ws2_32)
