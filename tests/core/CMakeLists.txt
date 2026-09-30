# Qt-free Desktop Lighting Studio scene/effects/resolver regression tests,
# adapted from the openrgb-unified offline suites (tests/build-studio-tests.*).
#
# The suites run with WORKING_DIRECTORY set to this source directory so the
# committed fixtures under tests/core/fixtures resolve, and packaged preset
# documents resolve through the DLS_PRESET_DIR candidate added to
# device_preset_test.cpp (../../src/core/presets/devices from here).

set(CORE_DIR ${CMAKE_SOURCE_DIR}/src/core)

# Scene core + effects + v3 type stack + Qt-free editor core now
# live in the shared openrgb2_studio_core library (src/core); the
# suites link it instead of recompiling the source list so the same
# studio::* objects are exercised everywhere. Its PUBLIC interface
# supplies the src/core + nlohmann-json include dirs.

function(dls_suite NAME)
    add_executable(${NAME} ${NAME}.cpp ${ARGN})
    target_link_libraries(${NAME} PRIVATE openrgb2_studio_core)
    target_compile_definitions(${NAME} PRIVATE NDEBUG)
    add_test(NAME ${NAME} COMMAND ${NAME})
    set_tests_properties(${NAME} PROPERTIES
        LABELS core
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        ENVIRONMENT
            "DLS_EFFECT_DIR=${CORE_DIR}/presets/effects;DLS_PRESET_DIR=${CORE_DIR}/presets/devices")
endfunction()

dls_suite(studio_scene_test)
dls_suite(studio_config_test)
dls_suite(device_preset_test)
dls_suite(studio_editor_test)
dls_suite(effect_json_test)

# Studio-import Task 2 — the canonical workspace candidate
# (parse/migrate/validate/resolve for every persisted form).
dls_suite(workspace_candidate_test)

add_executable(playback_test
    playback_test.cpp
    ${CMAKE_SOURCE_DIR}/src/core/playback/PlaybackLoop.cpp
    ${CMAKE_SOURCE_DIR}/vendor/OpenRGB/Controllers/LianLiWirelessController/LianLiWirelessEffects.cpp
    ${CMAKE_SOURCE_DIR}/vendor/OpenRGB/Controllers/LianLiWirelessController/LianLiWirelessCodec.cpp
    ${CMAKE_SOURCE_DIR}/vendor/OpenRGB/dependencies/tinyuz/compress/tuz_enc.cpp
    ${CMAKE_SOURCE_DIR}/vendor/OpenRGB/dependencies/tinyuz/compress/tuz_enc_private/tuz_enc_clip.cpp
    ${CMAKE_SOURCE_DIR}/vendor/OpenRGB/dependencies/tinyuz/compress/tuz_enc_private/tuz_enc_code.cpp
    ${CMAKE_SOURCE_DIR}/vendor/OpenRGB/dependencies/tinyuz/compress/tuz_enc_private/tuz_enc_match.cpp
    ${CMAKE_SOURCE_DIR}/vendor/OpenRGB/dependencies/tinyuz/compress/tuz_enc_private/tuz_sstring.cpp
    ${CMAKE_SOURCE_DIR}/vendor/OpenRGB/dependencies/tinyuz/decompress/tuz_dec.c
    ${CMAKE_SOURCE_DIR}/vendor/OpenRGB/dependencies/HDiffPatch/libHDiffPatch/HDiff/private_diff/libdivsufsort/divsufsort.cpp)
target_include_directories(playback_test PRIVATE
    ${CORE_DIR}
    ${CMAKE_SOURCE_DIR}/vendor/OpenRGB/Controllers/LianLiWirelessController
    ${CMAKE_SOURCE_DIR}/vendor/OpenRGB/dependencies
    ${CMAKE_SOURCE_DIR}/vendor/OpenRGB/dependencies/tinyuz
    ${CMAKE_SOURCE_DIR}/vendor/OpenRGB/dependencies/HDiffPatch)
target_compile_definitions(playback_test PRIVATE NDEBUG _IS_USED_MULTITHREAD=0)
add_test(NAME playback_test COMMAND playback_test)
set_tests_properties(playback_test PROPERTIES LABELS core)

# Key translation is self-contained.
add_executable(key_translate_test key_translate_test.cpp
    ${CORE_DIR}/inputs/KeyTranslate.cpp)
target_include_directories(key_translate_test PRIVATE ${CORE_DIR})
target_compile_definitions(key_translate_test PRIVATE NDEBUG)
add_test(NAME key_translate_test COMMAND key_translate_test)
set_tests_properties(key_translate_test PROPERTIES
    LABELS core
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
