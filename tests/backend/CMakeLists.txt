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
    ${DEPS}/json
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

# Mode-table regression (QA F05): constructs the real
# RGBController_LianLiWireless over a fake link/clock and asserts the full
# 11-mode table plus admitted multi-frame uploads for playback modes 5-10.
# Pulls in RGBController.cpp (Qt-free; its only external hooks are
# StringUtils + the LOG_* macros, which the test stubs) and the vendor
# root include dir for LogManager.h/StringUtils.h/filesystem.h.
add_executable(mode_table_test
    mode_table_test.cpp
    ${CTRL}/LianLiWirelessController.cpp
    ${CTRL}/RGBController_LianLiWireless.cpp
    ${CMAKE_SOURCE_DIR}/vendor/OpenRGB/RGBController/RGBController.cpp
    ${CMAKE_SOURCE_DIR}/vendor/OpenRGB/StringUtils.cpp
    ${LLW_SOURCES})

target_include_directories(mode_table_test PRIVATE
    ${LLW_INCLUDES}
    ${CMAKE_SOURCE_DIR}/vendor/OpenRGB
    ${CMAKE_SOURCE_DIR}/vendor/OpenRGB/RGBController
    ${DEPS}/json)

target_compile_definitions(mode_table_test PRIVATE ${LLW_DEFINES})

add_test(NAME mode_table_test COMMAND mode_table_test)
set_tests_properties(mode_table_test PROPERTIES
    LABELS backend
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

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

# Gigabyte RGB Fusion 2 Blackwell GPU packet regression (offline —
# captures raw i2c block writes from the real controller; no hardware).
set(BWGPU ${CMAKE_SOURCE_DIR}/vendor/OpenRGB/Controllers/GigabyteRGBFusion2BlackwellGPUController)

add_executable(blackwell_gpu_packet_test
    blackwell_gpu_packet_test.cpp
    ${CMAKE_SOURCE_DIR}/vendor/OpenRGB/RGBController/RGBController.cpp
    ${CMAKE_SOURCE_DIR}/vendor/OpenRGB/i2c_smbus/i2c_smbus.cpp
    ${CMAKE_SOURCE_DIR}/vendor/OpenRGB/StringUtils.cpp
    ${BWGPU}/GigabyteRGBFusion2BlackwellGPUController.cpp
    ${BWGPU}/RGBController_GigabyteRGBFusion2BlackwellGPU.cpp)

target_include_directories(blackwell_gpu_packet_test PRIVATE
    ${CMAKE_SOURCE_DIR}/vendor/OpenRGB
    ${BWGPU}
    ${CMAKE_SOURCE_DIR}/vendor/OpenRGB/RGBController
    ${CMAKE_SOURCE_DIR}/vendor/OpenRGB/i2c_smbus
    ${DEPS}/json)

target_compile_definitions(blackwell_gpu_packet_test PRIVATE NDEBUG)

add_test(NAME blackwell_gpu_packet_test COMMAND blackwell_gpu_packet_test)
set_tests_properties(blackwell_gpu_packet_test PROPERTIES
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

# Audit P-13: the REAL libusb transport (LianLiWirelessTransport.cpp)
# against fake_libusb.cpp, a link-time stub of the libusb entry points
# it calls (scripted read chunks, write results, failure injection,
# a per-device transfer log -- API in fake_libusb.h). No libusb
# library is linked and no USB device is touched. The vendored
# libusb 1.0.27 header is still the one compiled against, so a
# drifted stub prototype fails the build. Home for the P-10 (timeout,
# close, reopen) and P-12 (length-bounded discovery reads) tests.
add_executable(lianli_transport_test
    lianli_transport_test.cpp
    fake_libusb.cpp
    ${CTRL}/LianLiWirelessTransport.cpp
    ${LLW_SOURCES})
target_include_directories(lianli_transport_test PRIVATE
    ${LLW_INCLUDES}
    ${DEPS}/libusb-1.0.27/include
    ${CMAKE_CURRENT_SOURCE_DIR})
# libusb.h pulls in <windows.h>: keep its min/max macros out.
target_compile_definitions(lianli_transport_test PRIVATE
    ${LLW_DEFINES} NOMINMAX WIN32_LEAN_AND_MEAN)
add_test(NAME lianli_transport_test COMMAND lianli_transport_test)
set_tests_properties(lianli_transport_test PROPERTIES
    LABELS backend
    TIMEOUT 60)

# Audit P-7: the Logitech HID++ 0x8080 per-key send planner
# (LogitechPerKey8080Plan, Qt-free and hidapi-free): grouping by
# keyType, chunking at 14 keys, the G410 keyType-0x40 filter,
# brightness scaling, the delta against sent_colors and the forced
# full repaints. Pins the full repaint byte for byte against a copy
# of the pre-extraction send loop.
set(LOGI8080 ${CMAKE_SOURCE_DIR}/vendor/OpenRGB/Controllers/LogitechController/LogitechHIDPP20Controller)
add_executable(logitech_8080_plan_test
    logitech_8080_plan_test.cpp
    ${LOGI8080}/LogitechPerKey8080Plan.cpp)
target_include_directories(logitech_8080_plan_test PRIVATE
    ${LOGI8080}
    ${CMAKE_SOURCE_DIR}/vendor/OpenRGB/RGBController
    ${DEPS}/json)
target_compile_definitions(logitech_8080_plan_test PRIVATE NDEBUG)
add_test(NAME logitech_8080_plan_test COMMAND logitech_8080_plan_test)
set_tests_properties(logitech_8080_plan_test PROPERTIES LABELS backend)
