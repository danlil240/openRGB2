# Qt-free lighting engine (Milestone 3). The engine runs as its own
# process — the UI (Milestone 4) is separate and the engine keeps
# running with no UI attached. No Qt dependency here by design.

set(CORE_DIR ${CMAKE_SOURCE_DIR}/src/core)
set(JSON_DIR ${CMAKE_SOURCE_DIR}/vendor/OpenRGB/dependencies/json)
set(LLW_CTRL ${CMAKE_SOURCE_DIR}/vendor/OpenRGB/Controllers/LianLiWirelessController)
set(LLW_DEPS ${CMAKE_SOURCE_DIR}/vendor/OpenRGB/dependencies)

find_package(Threads REQUIRED)

# Engine library: the section-4 boundary types, the submitLatest sink
# plumbing, the delivery pipeline, device mapping, the scene-
# evaluation worker, and the playback-mode state machine (Task 3.6).
# The core scene/effects sources it drives are compiled in directly
# (same pattern as the offline test suites) — src/core stays
# unchanged. The scene JSON + preset-registry stack is part of the
# library so both the --scene CLI path and the IPC service
# (src/ipc/EngineService.cpp) share ONE scene-apply path.
#
# Playback geometry reuses the SHARED src/core/playback/PlaybackLoop
# builder plus the pure vendor renderers — identical inputs give the
# same frames the backend uploads. The codec drags in the tinyuz /
# divsufsort compression sources (same set as tests/backend).
add_library(openrgb2_engine STATIC
    EngineTypes.cpp
    FrameSink.cpp
    DeliveryPipeline.cpp
    DeviceMapper.cpp
    SceneWorker.cpp
    PlaybackPresets.cpp
    PlaybackClockSync.cpp
    PlaybackController.cpp
    Engine.cpp
    ${CORE_DIR}/scene/SceneTypes.cpp
    ${CORE_DIR}/scene/SceneGraph.cpp
    ${CORE_DIR}/scene/BindingResolver.cpp
    ${CORE_DIR}/scene/SceneJson.cpp
    ${CORE_DIR}/scene/EmitterLayout.cpp
    ${CORE_DIR}/effects/EffectTypes.cpp
    ${CORE_DIR}/effects/EffectEngine.cpp
    ${CORE_DIR}/effects/EffectJson.cpp
    ${CORE_DIR}/effects/Presets.cpp
    ${CORE_DIR}/presets/EffectRegistry.cpp
    ${CORE_DIR}/presets/DevicePreset.cpp
    ${CORE_DIR}/playback/PlaybackLoop.cpp
    ${LLW_CTRL}/LianLiWirelessEffects.cpp
    ${LLW_CTRL}/LianLiWirelessCodec.cpp
    ${LLW_DEPS}/tinyuz/compress/tuz_enc.cpp
    ${LLW_DEPS}/tinyuz/compress/tuz_enc_private/tuz_enc_clip.cpp
    ${LLW_DEPS}/tinyuz/compress/tuz_enc_private/tuz_enc_code.cpp
    ${LLW_DEPS}/tinyuz/compress/tuz_enc_private/tuz_enc_match.cpp
    ${LLW_DEPS}/tinyuz/compress/tuz_enc_private/tuz_sstring.cpp
    ${LLW_DEPS}/tinyuz/decompress/tuz_dec.c
    ${LLW_DEPS}/HDiffPatch/libHDiffPatch/HDiff/private_diff/libdivsufsort/divsufsort.cpp)
target_include_directories(openrgb2_engine PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CORE_DIR}
    ${JSON_DIR}
    ${LLW_CTRL})
target_include_directories(openrgb2_engine PRIVATE
    ${LLW_DEPS}
    ${LLW_DEPS}/tinyuz
    ${LLW_DEPS}/HDiffPatch)
target_compile_definitions(openrgb2_engine PRIVATE
    _IS_USED_MULTITHREAD=0)
target_link_libraries(openrgb2_engine PUBLIC Threads::Threads)

# Standalone engine process. --scene loads a scene document; preset
# names resolve through the packaged effect registry
# (--effects-dir / DLS_EFFECT_DIR / source-tree probe), falling back
# to the persisted inline layer stack. --ipc (default on) serves the
# versioned command + read-only diagnostics pipes for the UI.
add_executable(openrgb2-engine
    main.cpp)
target_link_libraries(openrgb2-engine PRIVATE
    openrgb2_engine
    openrgb2_backend_openrgb
    openrgb2_ipc)
