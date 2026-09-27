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
#include "DeviceMapper.h"
#include "Engine.h"
#include "SceneWorker.h"

#include "EngineService.h"
#include "IpcServer.h"
#include "NamedPipe.h"

#include "OpenRgbClient.h"
#include "TcpSocket.h"

#include "effects/Presets.h"
#include "presets/EffectRegistry.h"
#include "scene/SceneJson.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <fstream>
#include <map>
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
   Qualification is device-aware: IsWirelessGroupController keys
   on the wireless service's own transport identity (the
   "Wireless:" location prefix), not a name heuristic. Other
   controllers are unpaced until measured — the 60 Hz scene
   clock already bounds how often producers can submit. */
openrgb2::Clock::duration BudgetFor(
    const openrgb2::DeviceSnapshot& snap)
{
    if(openrgb2::IsWirelessGroupController(snap))
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
        /* Only re-apply on change — SetBudget notifies the lane
           even when the value is identical. */
        openrgb2::Clock::duration current;
        const openrgb2::Clock::duration want = BudgetFor(snap);
        if(!pipeline.Budget(snap.key, current) || current != want)
        {
            pipeline.SetBudget(snap.key, want);
        }
    }
}

void Usage(const char* argv0)
{
    std::fprintf(stderr,
        "usage: %s [--scene <scene.json>] [--effects-dir <dir>]\n"
        "          [--sdk [host:port]] [--ipc | --no-ipc]\n"
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
        "  never spawns it.\n"
        "  IPC is on by default: the command pipe %s and the\n"
        "  read-only diagnostics pipe %s (restricted to the\n"
        "  creating user; see docs/ipc-protocol.md). --no-ipc\n"
        "  runs headless with no pipes.\n",
        argv0, openrgb2::ipc::kCommandPipeName,
        openrgb2::ipc::kDiagnosticsPipeName);
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
    bool        ipc_enabled = true;       /* --no-ipc opts out    */
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
        else if(arg == "--ipc")
        {
            ipc_enabled = true;
        }
        else if(arg == "--no-ipc")
        {
            ipc_enabled = false;
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
       lanes simply idle. */
    std::shared_ptr<openrgb2::DeliveryPipeline> pipeline =
        std::make_shared<openrgb2::DeliveryPipeline>();
    engine.SetFrameSink(pipeline);

    /* Device mapping (Task 3.4): resolves the scene's persisted
       DeviceBindings against each enumeration — identities
       re-resolve per reconnect, never persisted indices — and
       assembles ONE complete-controller RgbFrame per
       controller (wireless group's three zone edits = one
       frame; unmapped LEDs keep their colors). */
    openrgb2::DeviceMapper mapper;

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

    /* Every evaluated scene frame is split into per-controller
       complete-controller frames and handed to the pipeline via
       the engine's submitLatest sink. */
    std::atomic<uint64_t> frames{ 0 };
    engine.Worker().SetFrameCallback(
        [&engine, &mapper, &frames](const openrgb2::EvaluatedFrame& eval)
        {
            ++frames;
            for(const openrgb2::RgbFrame& frame : mapper.MapFrame(eval))
            {
                engine.SubmitLatest(frame);
            }
        });

    if(!effects_dir.empty())
    {
        /* Preset-look files loaded as the registry's defaults layer;
           DLS_EFFECT_DIR and the source-tree probe still apply. */
        studio::EffectLooks().LoadDefaultsDirectory(effects_dir, nullptr);
    }

    /* The IPC service owns the scene-apply path (Task 3.5): the
       --scene CLI and SET_SCENE both go through
       ApplySceneDocument, so a UI-applied scene and a
       CLI-loaded scene are indistinguishable to the engine. */
    openrgb2::ipc::EngineService::Deps ipc_deps;
    ipc_deps.pipeline = pipeline.get();
    ipc_deps.mapper   = &mapper;
    ipc_deps.sdk      = sdk_client.get();
    openrgb2::ipc::EngineService ipc_service(engine, ipc_deps);

    std::unique_ptr<openrgb2::ipc::IpcServer>         ipc_server;
    std::unique_ptr<openrgb2::ipc::NamedPipeListener> cmd_listener;
    std::unique_ptr<openrgb2::ipc::NamedPipeListener> diag_listener;
    if(ipc_enabled)
    {
        ipc_server =
            std::make_unique<openrgb2::ipc::IpcServer>(ipc_service);
        ipc_service.SetEventSink(
            [&ipc_server](const openrgb2::ipc::IpcEvent& ev)
            { ipc_server->Emit(ev); });
        ipc_service.SetServerStatsProvider(
            [&ipc_server]() { return ipc_server->Stats(); });

        /* Both pipes share the restricted DACL (creating user +
           LocalSystem only). The diagnostics pipe is read-only
           by construction: DiagGet is the only verb it serves. */
        cmd_listener =
            std::make_unique<openrgb2::ipc::NamedPipeListener>(
                *ipc_server, openrgb2::ipc::ChannelKind::Command,
                openrgb2::ipc::kCommandPipeName);
        diag_listener =
            std::make_unique<openrgb2::ipc::NamedPipeListener>(
                *ipc_server, openrgb2::ipc::ChannelKind::Diagnostics,
                openrgb2::ipc::kDiagnosticsPipeName);
        std::string err;
        if(!cmd_listener->Start(err))
        {
            std::fprintf(stderr,
                "openrgb2-engine: command pipe unavailable: %s "
                "(continuing without IPC)\n", err.c_str());
            cmd_listener.reset();
        }
        err.clear();
        if(!diag_listener->Start(err))
        {
            std::fprintf(stderr,
                "openrgb2-engine: diagnostics pipe unavailable: %s "
                "(continuing)\n", err.c_str());
            diag_listener.reset();
        }
    }

    if(!scene_path.empty())
    {
        std::shared_ptr<studio::SceneDocument> doc;
        if(!LoadSceneFile(scene_path, doc))
        {
            return 1;
        }
        std::string err;
        if(!ipc_service.ApplySceneDocument(doc, err))
        {
            std::fprintf(stderr,
                "openrgb2-engine: scene apply failed: %s\n",
                err.c_str());
            return 1;
        }
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

    /* Bound controllers need their direct (custom) mode before
       live UpdateLEDs writes reach the LEDs. Applied once per
       (controller, topology generation) — a reconnect/new
       enumeration re-asserts it. Playback-mode ownership is the
       M3 playback task's concern; nothing selects an uploaded
       loop here yet. */
    std::map<openrgb2::DeviceKey, uint64_t> custom_mode_done;

    const auto started = openrgb2::Clock::now();
    while(!g_stop.load())
    {
        if(ipc_server)
        {
            /* Diff-driven push events: topology, connection,
               playback changes and the bounded-rate
               frame_status feed (Task 3.5). */
            ipc_service.PollEvents();
        }
        if(sdk_client)
        {
            sdk_client->Poll();
            /* Mirror connection/topology into the lanes so
               validation tracks the live backend state and new
               controllers get lanes without a restart. */
            SyncLanes(*pipeline, *sdk_client);

            const std::vector<openrgb2::DeviceSnapshot> snaps =
                sdk_client->Snapshots();
            mapper.SyncDevices(snaps);
            for(const openrgb2::DeviceKey& key :
                mapper.BoundControllers())
            {
                const auto snap_it =
                    std::find_if(snaps.begin(), snaps.end(),
                        [&key](const openrgb2::DeviceSnapshot& s)
                        { return s.key == key; });
                if(snap_it == snaps.end() ||
                   snap_it->connection !=
                       openrgb2::ConnectionState::Connected)
                {
                    continue;
                }
                uint64_t& done = custom_mode_done[key];
                if(done != snap_it->topology_generation)
                {
                    if(sdk_client->SetCustomMode(key))
                    {
                        done = snap_it->topology_generation;
                    }
                }
            }
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
