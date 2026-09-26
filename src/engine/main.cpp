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

#include "DeliveryPipeline.h"
#include "Engine.h"
#include "SceneWorker.h"

#include "OpenRgbClient.h"
#include "TcpSocket.h"

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

/* Per-controller delivery budgets (plan section 4 — budgets are
   qualified per controller, never a global rate). The wireless
   group's measured ceiling is ~33.5 admitted uploads/s
   (docs/benchmarks/wireless-result.md), so it is paced at 30 ms.
   Other controllers are unpaced until measured — the 60 Hz scene
   clock already bounds how often producers can submit. */
openrgb2::Clock::duration BudgetFor(
    const openrgb2::DeviceSnapshot& snap)
{
    if(snap.key.device.find("Wireless") != std::string::npos)
    {
        return std::chrono::milliseconds(30);
    }
    return openrgb2::Clock::duration::zero();
}

void SyncLanes(openrgb2::DeliveryPipeline& pipeline,
               const openrgb2::sdk::OpenRgbClient& client)
{
    const std::vector<openrgb2::DeviceSnapshot> snaps =
        client.Snapshots();
    pipeline.SyncDevices(snaps);
    for(const openrgb2::DeviceSnapshot& snap : snaps)
    {
        pipeline.SetBudget(snap.key, BudgetFor(snap));
    }
}

void Usage(const char* argv0)
{
    std::fprintf(stderr,
        "usage: %s [--scene <scene.json>] [--effects-dir <dir>]\n"
        "          [--sdk [host:port]]\n"
        "\n"
        "  Runs the lighting engine until SIGINT/SIGTERM. No UI is\n"
        "  required or expected. Without --scene the engine idles at\n"
        "  the 60 Hz scene clock (no frames are produced).\n"
        "  --scene expects an expanded SceneDocument JSON (the core\n"
        "  scene schema); workspace/profile resolution lands with the\n"
        "  profile task. --effects-dir loads effect-look JSON files so\n"
        "  scenes that name a preset resolve through the registry.\n"
        "  --sdk connects submitLatest to the OpenRGB backend SDK\n"
        "  (default 127.0.0.1:6744); without it frames go to a null\n"
        "  sink. The backend is launched externally — this process\n"
        "  never spawns it.\n",
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
    std::string sdk_addr;                 /* empty = SDK disabled */
    bool        sdk_enabled = false;
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
        else if(arg == "--sdk")
        {
            /* Optional host:port; defaults to the pinned backend
               endpoint (plan section 4). */
            sdk_enabled = true;
            sdk_addr    = "127.0.0.1:6744";
            if(i + 1 < argc && argv[i + 1][0] != '-')
            {
                sdk_addr = argv[++i];
            }
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

    /* The delivery pipeline is the engine's submitLatest sink:
       per-controller depth-1 pending lanes with independent
       budgets, unchanged-frame suppression, and the distinct
       reliable convergence path (Task 3.3). It drains into the
       SDK transport below; with no transport configured its
       lanes simply idle (and device mapping — Task 3.4 — is
       what will produce per-controller RgbFrames). */
    std::shared_ptr<openrgb2::DeliveryPipeline> pipeline =
        std::make_shared<openrgb2::DeliveryPipeline>();
    engine.SetFrameSink(pipeline);

    /* Optional SDK output: the client is the pipeline's
       transport — its SubmitLatest sends one
       complete-controller UPDATELEDS per RgbFrame over the
       protocol-4 wire. Reconnects run on the client's bounded
       backoff (polled below); connection state is surfaced
       through its DeviceSnapshots, which are mirrored into
       pipeline lanes. */
    std::shared_ptr<openrgb2::sdk::OpenRgbClient> sdk_client;
    if(sdk_enabled)
    {
        openrgb2::sdk::OpenRgbClientOptions opts;
        const std::string::size_type colon = sdk_addr.rfind(':');
        if(colon != std::string::npos)
        {
            opts.host = sdk_addr.substr(0, colon);
            opts.port = (uint16_t)std::strtoul(
                sdk_addr.substr(colon + 1).c_str(), nullptr, 10);
        }
        else
        {
            opts.host = sdk_addr;
        }
        sdk_client =
            std::make_shared<openrgb2::sdk::OpenRgbClient>(
                opts,
                std::make_unique<openrgb2::sdk::TcpSocket>());
        pipeline->SetTransport(sdk_client);
        if(sdk_client->Connect())
        {
            std::fprintf(stderr,
                "openrgb2-engine: SDK connected to %s:%u "
                "(protocol %u, %zu controllers)\n",
                opts.host.c_str(), (unsigned)opts.port,
                sdk_client->ProtocolVersion(),
                sdk_client->Snapshots().size());
        }
        else
        {
            /* Not fatal: the sink reports Disconnected and the
               bounded backoff keeps retrying via Poll(). */
            std::fprintf(stderr,
                "openrgb2-engine: SDK connect failed: %s "
                "(will retry)\n",
                sdk_client->LastError().c_str());
            sdk_client->ReconnectNow();
        }
        SyncLanes(*pipeline, *sdk_client);
    }

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

    /* Per-lane delivery workers: a lane blocked inside a slow
       transport send cannot stall the others. */
    pipeline->Start();

    const auto started = openrgb2::Clock::now();
    while(!g_stop.load())
    {
        if(sdk_client)
        {
            sdk_client->Poll();
            /* Mirror connection/topology into the lanes so
               validation tracks the live backend state and new
               controllers get lanes without a restart. */
            SyncLanes(*pipeline, *sdk_client);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }

    engine.Stop();
    pipeline->Stop();

    const double elapsed =
        std::chrono::duration<double>(openrgb2::Clock::now() - started)
            .count();
    std::fprintf(stderr,
        "openrgb2-engine: stopped after %.2f s, %llu scene frames\n",
        elapsed, (unsigned long long)frames.load());
    return 0;
}
