# Offline Lian Li wireless protocol harness, adapted from the openrgb-unified
# tests/build-and-run.ps1 recipe (MSVC cl, no Qt, no hardware). Compiles the
# wireless controller's Qt-free core plus the tinyuz/HDiffPatch compression
# dependencies it needs for uploaded-frame payloads.

set(CTRL ${CMAKE_SOURCE_DIR}/vendor/OpenRGB/Controllers/LianLiWirelessController)
set(DEPS ${CMAKE_SOURCE_DIR}/vendor/OpenRGB/dependencies)

add_executable(lianli_protocol_test
    lianli_protocol_test.cpp
    ${CTRL}/LianLiWirelessProtocol.cpp
    ${CTRL}/LianLiWirelessCodec.cpp
    ${CTRL}/LianLiWirelessRuntime.cpp
    ${CTRL}/LianLiWirelessService.cpp
    ${CTRL}/LianLiWirelessEffects.cpp
    ${DEPS}/tinyuz/compress/tuz_enc.cpp
    ${DEPS}/tinyuz/compress/tuz_enc_private/tuz_enc_clip.cpp
    ${DEPS}/tinyuz/compress/tuz_enc_private/tuz_enc_code.cpp
    ${DEPS}/tinyuz/compress/tuz_enc_private/tuz_enc_match.cpp
    ${DEPS}/tinyuz/compress/tuz_enc_private/tuz_sstring.cpp
    ${DEPS}/tinyuz/decompress/tuz_dec.c
    ${DEPS}/HDiffPatch/libHDiffPatch/HDiff/private_diff/libdivsufsort/divsufsort.cpp)

target_include_directories(lianli_protocol_test PRIVATE
    ${CTRL}
    ${DEPS}
    ${DEPS}/tinyuz
    ${DEPS}/HDiffPatch)

target_compile_definitions(lianli_protocol_test PRIVATE
    NDEBUG
    _IS_USED_MULTITHREAD=0)

add_test(NAME lianli_protocol_test COMMAND lianli_protocol_test)
set_tests_properties(lianli_protocol_test PROPERTIES
    LABELS backend
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
