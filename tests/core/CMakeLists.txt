# Qt-free Desktop Lighting Studio scene/effects/resolver regression tests,
# adapted from the openrgb-unified offline suites (tests/build-studio-tests.*).
#
# The suites run with WORKING_DIRECTORY set to this source directory so the
# committed fixtures under tests/core/fixtures resolve, and packaged preset
# documents resolve through the DLS_PRESET_DIR candidate added to
# device_preset_test.cpp (../../src/core/presets/devices from here).

set(CORE_DIR ${CMAKE_SOURCE_DIR}/src/core)
set(JSON_DIR ${CMAKE_SOURCE_DIR}/vendor/OpenRGB/dependencies/json)

# Scene core + effects + shared input core.
set(SCENE_SRC
    ${CORE_DIR}/scene/SceneTypes.cpp
    ${CORE_DIR}/scene/SceneGraph.cpp
    ${CORE_DIR}/scene/EmitterLayout.cpp
    ${CORE_DIR}/scene/SceneJson.cpp
    ${CORE_DIR}/scene/BindingResolver.cpp
    ${CORE_DIR}/scene/DefaultDesk.cpp
    ${CORE_DIR}/effects/EffectTypes.cpp
    ${CORE_DIR}/effects/EffectEngine.cpp
    ${CORE_DIR}/effects/EffectJson.cpp
    ${CORE_DIR}/effects/Presets.cpp
    ${CORE_DIR}/inputs/InputBus.cpp
    ${CORE_DIR}/inputs/OnsetDetect.cpp
    ${CORE_DIR}/inputs/KeyMap.cpp)

# v3 type stack: presets, resolver, config migration.
set(V3_SRC
    ${CORE_DIR}/presets/DevicePreset.cpp
    ${CORE_DIR}/presets/PresetRegistry.cpp
    ${CORE_DIR}/presets/EffectRegistry.cpp
    ${CORE_DIR}/presets/PresetBundle.cpp
    ${CORE_DIR}/scene/SceneResolver.cpp
    ${CORE_DIR}/config/StudioConfig.cpp
    ${CORE_DIR}/config/ConfigMigration.cpp)

# Qt-free editor core.
set(EDITOR_SRC
    ${CORE_DIR}/editor/EditorController.cpp
    ${CORE_DIR}/editor/TransformCommands.cpp)

function(dls_suite NAME)
    add_executable(${NAME} ${NAME}.cpp ${ARGN})
    target_include_directories(${NAME} PRIVATE ${CORE_DIR} ${JSON_DIR})
    target_compile_definitions(${NAME} PRIVATE NDEBUG)
    add_test(NAME ${NAME} COMMAND ${NAME})
    set_tests_properties(${NAME} PROPERTIES
        LABELS core
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        ENVIRONMENT
            "DLS_EFFECT_DIR=${CORE_DIR}/presets/effects;DLS_PRESET_DIR=${CORE_DIR}/presets/devices")
endfunction()

dls_suite(studio_scene_test  ${SCENE_SRC} ${V3_SRC})
dls_suite(studio_config_test ${SCENE_SRC} ${V3_SRC})
dls_suite(device_preset_test ${SCENE_SRC} ${V3_SRC})
dls_suite(studio_editor_test ${SCENE_SRC} ${V3_SRC} ${EDITOR_SRC})
dls_suite(effect_json_test  ${SCENE_SRC} ${V3_SRC})

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
