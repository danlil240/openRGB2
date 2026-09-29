# Shared Qt-free Desktop Lighting Studio model core (studio-import
# Task 1). One STATIC library so the engine, the offline core
# suites, and later UI-facing code all link the SAME objects instead
# of recompiling this list per target. All studio::* signatures are
# preserved — the sources are byte-identical to the plugin working
# tree (see vendor/provenance/dls-ui-import-hashes.txt).
#
# Scope: scene/config/effects/presets/editor/input-model code only —
# no Qt, no SDK, no vendor-controller dependency. Deliberately NOT
# here: playback/PlaybackLoop.cpp (wireless-specific frame builder;
# stays compiled into openrgb2_engine, playback_test, and the
# backend harness alongside the LianLi codec/tinyuz sources) and the
# platform capture backends (src/platform/windows).

add_library(openrgb2_studio_core STATIC
    scene/SceneTypes.cpp
    scene/SceneGraph.cpp
    scene/EmitterLayout.cpp
    scene/SceneJson.cpp
    scene/BindingResolver.cpp
    scene/SceneResolver.cpp
    scene/DefaultDesk.cpp
    config/StudioConfig.cpp
    config/ConfigMigration.cpp
    effects/EffectTypes.cpp
    effects/EffectEngine.cpp
    effects/EffectJson.cpp
    effects/Presets.cpp
    presets/DevicePreset.cpp
    presets/PresetRegistry.cpp
    presets/EffectRegistry.cpp
    presets/PresetBundle.cpp
    editor/EditorController.cpp
    editor/TransformCommands.cpp
    inputs/InputBus.cpp
    inputs/OnsetDetect.cpp
    inputs/KeyMap.cpp
    inputs/KeyTranslate.cpp)

target_include_directories(openrgb2_studio_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/vendor/OpenRGB/dependencies/json)

# InputBus guards its subscriber list with std::mutex.
find_package(Threads REQUIRED)
target_link_libraries(openrgb2_studio_core PUBLIC Threads::Threads)
