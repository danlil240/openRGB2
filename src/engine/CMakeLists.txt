# Qt-free lighting engine (Milestone 3). The engine runs as its own
# process — the UI (Milestone 4) is separate and the engine keeps
# running with no UI attached. No Qt dependency here by design.

set(CORE_DIR ${CMAKE_SOURCE_DIR}/src/core)
set(JSON_DIR ${CMAKE_SOURCE_DIR}/vendor/OpenRGB/dependencies/json)

find_package(Threads REQUIRED)

# Engine library: the section-4 boundary types, the submitLatest sink
# plumbing, and the scene-evaluation worker. The core scene/effects
# sources it drives are compiled in directly (same pattern as the
# offline test suites) — src/core stays unchanged.
add_library(openrgb2_engine STATIC
    EngineTypes.cpp
    FrameSink.cpp
    DeliveryPipeline.cpp
    SceneWorker.cpp
    Engine.cpp
    ${CORE_DIR}/scene/SceneTypes.cpp
    ${CORE_DIR}/scene/SceneGraph.cpp
    ${CORE_DIR}/effects/EffectTypes.cpp
    ${CORE_DIR}/effects/EffectEngine.cpp)
target_include_directories(openrgb2_engine PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CORE_DIR}
    ${JSON_DIR})
target_link_libraries(openrgb2_engine PUBLIC Threads::Threads)

# Standalone engine process. --scene loads a scene document; preset
# names resolve through the packaged effect registry
# (--effects-dir / DLS_EFFECT_DIR / source-tree probe), falling back
# to the persisted inline layer stack.
add_executable(openrgb2-engine
    main.cpp
    ${CORE_DIR}/scene/SceneJson.cpp
    ${CORE_DIR}/scene/EmitterLayout.cpp
    ${CORE_DIR}/effects/EffectJson.cpp
    ${CORE_DIR}/effects/Presets.cpp
    ${CORE_DIR}/presets/EffectRegistry.cpp
    ${CORE_DIR}/presets/DevicePreset.cpp)
target_link_libraries(openrgb2-engine PRIVATE
    openrgb2_engine
    openrgb2_backend_openrgb)
