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

# Milestone 5 skew-measurement harness (task 5.1). Physical runs are
# opt-in like lianli_bench. Unlike the M1 producer it drives the REAL
# delivery path — the engine's DeliveryPipeline + OpenRgbClient through
# an instrumented transport sink — so per-send timing and per-tick
# inter-controller skew come from the production objects, not a
# reimplementation. The bench client side (sdk_client/sdk_wire) is
# reused for enumeration, state capture/restore, and the
# request/reply RTT probe.
add_executable(skew_bench
    skew_bench.cpp
    sdk_client.cpp
    sdk_wire.cpp)

target_include_directories(skew_bench PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

target_compile_definitions(skew_bench PRIVATE NDEBUG)

target_link_libraries(skew_bench PRIVATE
    openrgb2_engine
    openrgb2_backend_openrgb
    ws2_32)
