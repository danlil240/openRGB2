# Milestone 1 benchmark producer. Physical runs are opt-in and separate
# from CTest; this target only builds the tool. The wire layer has its
# own offline test (tests/backend/bench_sdk_wire_test.cpp).

add_executable(lianli_bench
    bench_producer.cpp
    sdk_client.cpp
    sdk_wire.cpp)

target_include_directories(lianli_bench PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

target_compile_definitions(lianli_bench PRIVATE NDEBUG)

# winmm: timeBeginPeriod(1) for the pacing loop (audit P-8).
target_link_libraries(lianli_bench PRIVATE ws2_32 winmm)

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
    ws2_32
    winmm)

# R-11 step 3: offline loop-size measurement (no hardware, not a
# test). Compiles every packaged look for the 120-LED SL Wireless
# group through the shared BuildLoopFromRenderer and prints T, N,
# fps and the tinyuz size, without and with speed snapping. Results:
# docs/benchmarks/loop-size.md.
set(LOOP_SIZE_CTRL ${CMAKE_SOURCE_DIR}/vendor/OpenRGB/Controllers/LianLiWirelessController)
set(LOOP_SIZE_DEPS ${CMAKE_SOURCE_DIR}/vendor/OpenRGB/dependencies)
add_executable(loop_size
    loop_size.cpp
    ${CMAKE_SOURCE_DIR}/src/core/playback/PlaybackLoop.cpp
    ${LOOP_SIZE_CTRL}/LianLiWirelessEffects.cpp
    ${LOOP_SIZE_CTRL}/LianLiWirelessCodec.cpp
    ${LOOP_SIZE_DEPS}/tinyuz/compress/tuz_enc.cpp
    ${LOOP_SIZE_DEPS}/tinyuz/compress/tuz_enc_private/tuz_enc_clip.cpp
    ${LOOP_SIZE_DEPS}/tinyuz/compress/tuz_enc_private/tuz_enc_code.cpp
    ${LOOP_SIZE_DEPS}/tinyuz/compress/tuz_enc_private/tuz_enc_match.cpp
    ${LOOP_SIZE_DEPS}/tinyuz/compress/tuz_enc_private/tuz_sstring.cpp
    ${LOOP_SIZE_DEPS}/tinyuz/decompress/tuz_dec.c
    ${LOOP_SIZE_DEPS}/HDiffPatch/libHDiffPatch/HDiff/private_diff/libdivsufsort/divsufsort.cpp)
target_include_directories(loop_size PRIVATE
    ${LOOP_SIZE_CTRL}
    ${LOOP_SIZE_DEPS}
    ${LOOP_SIZE_DEPS}/tinyuz
    ${LOOP_SIZE_DEPS}/HDiffPatch)
target_link_libraries(loop_size PRIVATE openrgb2_studio_core)
target_compile_definitions(loop_size PRIVATE NDEBUG _IS_USED_MULTITHREAD=0
    OPENRGB2_EFFECT_DIR="${CMAKE_SOURCE_DIR}/src/core/presets/effects")
