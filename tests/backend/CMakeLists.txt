# Offline Lian Li wireless protocol harness, adapted from the openrgb-unified
# tests/build-and-run.ps1 recipe (MSVC cl, no Qt, no hardware). Compiles the
# wireless controller's Qt-free core plus the tinyuz/HDiffPatch compression
# dependencies it needs for uploaded-frame payloads.

set(CTRL ${CMAKE_SOURCE_DIR}/vendor/OpenRGB/Controllers/LianLiWirelessController)
set(DEPS ${CMAKE_SOURCE_DIR}/vendor/OpenRGB/dependencies)
set(LLW_SOURCES
    ${CMAKE_SOURCE_DIR}/src/core/playback/PlaybackLoop.cpp
    ${CTRL}/LianLiWirelessProtocol.cpp
    ${CTRL}/LianLiWirelessCodec.cpp
    ${CTRL}/LianLiWirelessRuntime.cpp
    ${CTRL}/LianLiWirelessService.cpp
    ${CTRL}/LianLiWirelessEffects.cpp
    ${CTRL}/LianLiWirelessDiagnostics.cpp
    ${DEPS}/tinyuz/compress/tuz_enc.cpp
    ${DEPS}/tinyuz/compress/tuz_enc_private/tuz_enc_clip.cpp
    ${DEPS}/tinyuz/compress/tuz_enc_private/tuz_enc_code.cpp
    ${DEPS}/tinyuz/compress/tuz_enc_private/tuz_enc_match.cpp
    ${DEPS}/tinyuz/compress/tuz_enc_private/tuz_sstring.cpp
    ${DEPS}/tinyuz/decompress/tuz_dec.c
    ${DEPS}/HDiffPatch/libHDiffPatch/HDiff/private_diff/libdivsufsort/divsufsort.cpp)
set(LLW_INCLUDES
    ${CMAKE_SOURCE_DIR}/src/core
    ${CTRL}
    ${DEPS}
    ${DEPS}/tinyuz
    ${DEPS}/HDiffPatch)
set(LLW_DEFINES NDEBUG _IS_USED_MULTITHREAD=0)

add_executable(lianli_protocol_test
    lianli_protocol_test.cpp
    ${LLW_SOURCES})

target_include_directories(lianli_protocol_test PRIVATE ${LLW_INCLUDES})

target_compile_definitions(lianli_protocol_test PRIVATE ${LLW_DEFINES})

add_test(NAME lianli_protocol_test COMMAND lianli_protocol_test)
set_tests_properties(lianli_protocol_test PROPERTIES
    LABELS backend
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

add_executable(lianli_playback_test playback_test.cpp ${LLW_SOURCES})
target_include_directories(lianli_playback_test PRIVATE ${LLW_INCLUDES})
target_compile_definitions(lianli_playback_test PRIVATE ${LLW_DEFINES})
add_test(NAME lianli_playback_test COMMAND lianli_playback_test)
set_tests_properties(lianli_playback_test PROPERTIES LABELS backend)

add_executable(service_pending_test service_pending_test.cpp ${LLW_SOURCES})
target_include_directories(service_pending_test PRIVATE ${LLW_INCLUDES})
target_compile_definitions(service_pending_test PRIVATE ${LLW_DEFINES})
add_test(NAME service_pending_test COMMAND service_pending_test)
set_tests_properties(service_pending_test PROPERTIES LABELS backend)

# Bounded-diagnostics harness: also compiles the device-side controller so
# the compression call sites are exercised (RGBControllerInterface.h is
# Qt-free; nlohmann/json is vendored under dependencies/json).
add_executable(lianli_diagnostics_test
    lianli_diagnostics_test.cpp
    ${CTRL}/LianLiWirelessController.cpp
    ${LLW_SOURCES})

target_include_directories(lianli_diagnostics_test PRIVATE
    ${LLW_INCLUDES}
    ${CMAKE_SOURCE_DIR}/vendor/OpenRGB/RGBController
    ${DEPS}/json)

target_compile_definitions(lianli_diagnostics_test PRIVATE ${LLW_DEFINES})

add_test(NAME lianli_diagnostics_test COMMAND lianli_diagnostics_test)
set_tests_properties(lianli_diagnostics_test PROPERTIES
    LABELS backend
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

# Benchmark producer wire layer (pure parsing/packet building, no sockets).
add_executable(bench_sdk_wire_test
    bench_sdk_wire_test.cpp
    ${CMAKE_SOURCE_DIR}/tools/bench/sdk_wire.cpp)

target_include_directories(bench_sdk_wire_test PRIVATE
    ${CMAKE_SOURCE_DIR}/tools/bench)

target_compile_definitions(bench_sdk_wire_test PRIVATE NDEBUG)

add_test(NAME bench_sdk_wire_test COMMAND bench_sdk_wire_test)
set_tests_properties(bench_sdk_wire_test PROPERTIES
    LABELS backend
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

# Skew-harness aggregation layer (pure math, header-only).
add_executable(skew_stats_test
    skew_stats_test.cpp)

target_include_directories(skew_stats_test PRIVATE
    ${CMAKE_SOURCE_DIR}/tools/bench)

target_compile_definitions(skew_stats_test PRIVATE NDEBUG)

add_test(NAME skew_stats_test COMMAND skew_stats_test)
set_tests_properties(skew_stats_test PROPERTIES
    LABELS backend
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
