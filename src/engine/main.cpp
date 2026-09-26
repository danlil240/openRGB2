/*---------------------------------------------------------*\
|| main.cpp — openrgb2-engine                                |
||                                                           |
||   Standalone Qt-free lighting-engine process (Milestone  |
||   3 skeleton). The UI is a separate process (Milestone   |
||   4); the engine must keep running when no UI is         |
||   attached, so this entry point never depends on a        |
||   client connection.                                     |
||                                                           |
||   Later M3 tasks add the SDK client, device mapping,     |
||   IPC, power handling, and profiles. For now the engine  |
||   runs the 60 Hz scene clock, evaluates an optional       |
||   --scene document, and counts produced frames.          |
||                                                           |
||   SPDX-License-Identifier: GPL-2.0-or-later               |
\*---------------------------------------------------------*/

#include "Engine.h"
#include "SceneWorker.h"

#include "effects/Presets.h"
#include "presets/EffectRegistry.h"
#include "scene/SceneJson.h"

#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <thread>

namespace
{

std::atomic<bool> g_stop{ false };

void OnSignal(int)
{
    g_stop.store(true);
}

void Usage(const char* argv0)
{
    std::fprintf(stderr,
        "usage: %s [--scene <scene.json>] [--effects-dir <dir>]\n"
        "\n"
        "  Runs the lighting engine until SIGINT/SIGTERM. No UI is\n"
        "  required or expected. Without --scene the engine idles at\n"
        "  the 60 Hz scene clock (no frames are produced).\n"
        "  --scene expects an expanded SceneDocument JSON (the core\n"
        "  scene schema); workspace/profile resolution lands with the\n"
        "  profile task. --effects-dir loads effect-look JSON files so\n"
        "  scenes that name a preset resolve through the registry.\n",
        argv0);
}

bool LoadSceneFile(const std::string& path,
                   std::shared_ptr<studio::SceneDocument>& out)
{
    std::ifstream in(path);
    if(!in)
    {
        std::fprintf(stderr, "openrgb2-engine: cannot open %s\n",
                     path.c_str());
        return false;
    }
    std::stringstream ss;
    ss << in.rdbuf();

    nlohmann::json j = nlohmann::json::parse(ss.str(), nullptr, false);
    if(j.is_discarded())
    {
        std::fprintf(stderr, "openrgb2-engine: %s is not valid JSON\n",
                     path.c_str());
        return false;
    }

    auto doc = std::make_shared<studio::SceneDocument>();
    std::vector<std::string> errors;
    if(!studio::FromJson(j, *doc, &errors))
    {
        std::fprintf(stderr, "openrgb2-engine: %s failed to load:\n",
                     path.c_str());
        for(const std::string& e : errors)
        {
            std::fprintf(stderr, "  %s\n", e.c_str());
        }
        return false;
    }
    out = std::move(doc);
    return true;
}

} /* anonymous namespace */

int main(int argc, char** argv)
{
    std::string scene_path;
    std::string effects_dir;
    for(int i = 1; i < argc; ++i)
    {
        const std::string arg = argv[i];
        if((arg == "--scene") && i + 1 < argc)
        {
            scene_path = argv[++i];
        }
        else if((arg == "--effects-dir") && i + 1 < argc)
        {
            effects_dir = argv[++i];
        }
        else if(arg == "--help" || arg == "-h")
        {
            Usage(argv[0]);
            return 0;
        }
        else
        {
            std::fprintf(stderr, "openrgb2-engine: unknown argument %s\n",
                         arg.c_str());
            Usage(argv[0]);
            return 2;
        }
    }

    openrgb2::Engine engine;

    /* Frames are counted for the skeleton's heartbeat log. Device
       mapping (EvaluatedFrame -> per-controller RgbFrame -> the
       submitLatest sink) is a later M3 task. */
    std::atomic<uint64_t> frames{ 0 };
    engine.Worker().SetFrameCallback(
        [&frames](const openrgb2::EvaluatedFrame&) { ++frames; });

    if(!effects_dir.empty())
    {
        /* Preset-look files loaded as the registry's defaults layer;
           DLS_EFFECT_DIR and the source-tree probe still apply. */
        studio::EffectLooks().LoadDefaultsDirectory(effects_dir, nullptr);
    }

    if(!scene_path.empty())
    {
        std::shared_ptr<studio::SceneDocument> doc;
        if(!LoadSceneFile(scene_path, doc))
        {
            return 1;
        }
        /* Resolve the scene's effect stack the way the persisted
           document intends: the inline layer stack when present,
           otherwise the named preset through the effect-look
           registry (EffectState contract in SceneTypes.h). */
        std::vector<studio::EffectLayer> layers = doc->effect.layers;
        if(layers.empty() && !doc->effect.preset.empty())
        {
            layers = studio::BuildPreset(doc->effect.preset,
                                         doc->effect.seed);
            studio::ApplyGlobalParams(layers, doc->effect.speed,
                                      doc->effect.intensity);
        }
        engine.Worker().SubmitScene(doc, std::move(layers),
                                    /*revision*/ 1);
    }

    std::signal(SIGINT, OnSignal);
    std::signal(SIGTERM, OnSignal);
#ifdef SIGBREAK
    std::signal(SIGBREAK, OnSignal);
#endif

    if(!engine.Start())
    {
        std::fprintf(stderr, "openrgb2-engine: failed to start\n");
        return 1;
    }
    std::fprintf(stderr,
        "openrgb2-engine: running (%.0f Hz scene clock, no UI "
        "required; Ctrl+C to stop)\n",
        openrgb2::SceneWorker::kTickHz);

    const auto started = openrgb2::Clock::now();
    while(!g_stop.load())
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }

    engine.Stop();

    const double elapsed =
        std::chrono::duration<double>(openrgb2::Clock::now() - started)
            .count();
    std::fprintf(stderr,
        "openrgb2-engine: stopped after %.2f s, %llu scene frames\n",
        elapsed, (unsigned long long)frames.load());
    return 0;
}
