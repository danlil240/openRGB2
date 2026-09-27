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
#include "OrderlyShutdown.h"
#include "PlaybackController.h"
#include "PowerLifecycle.h"
#include "ProfileStore.h"
#include "SceneLoader.h"
#include "SceneWorker.h"

#include "EngineService.h"
#include "IpcServer.h"
#include "NamedPipe.h"

#include "OpenRgbClient.h"
#include "TcpSocket.h"

#include "WinPowerMonitor.h"

#include "effects/Presets.h"
#include "presets/EffectRegistry.h"
#include "presets/PresetRegistry.h"
#include "scene/SceneJson.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
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
        "usage: %s [--scene <file>] [--effects-dir <dir>]\n"
        "          [--devices-dir <dir>] [--state <file>]\n"
        "          [--sdk [host:port]] [--ipc | --no-ipc]\n"
        "\n"
        "  Runs the lighting engine until SIGINT/SIGTERM. No UI is\n"
        "  required or expected. Without --scene the engine idles at\n"
        "  the 60 Hz scene clock (no frames are produced).\n"
        "  --scene accepts any persisted form the profile layer\n"
        "  resolves: an expanded SceneDocument JSON ('version'),\n"
        "  a compact workspace studio.json ('schema_version'), or a\n"
        "  legacy host-settings blob ('scene'). Workspace docs need\n"
        "  --devices-dir (default: presets/devices or\n"
        "  src/core/presets/devices relative to the working\n"
        "  directory). --effects-dir loads effect-look JSON files so\n"
        "  scenes that name a preset resolve through the registry.\n"
        "  --sdk connects submitLatest to the OpenRGB backend SDK\n"
        "  (default 127.0.0.1:6744); without it frames go to a null\n"
        "  sink. The backend is launched externally — this process\n"
        "  never spawns it.\n"
        "  --state sets the durable engine profile (default\n"
        "  profiles/local/engine-state.json — gitignored user state\n"
        "  holding per-scene playback selections and resolved\n"
        "  binding identities; atomic writes + last-known-good).\n"
        "  IPC is on by default: the command pipe %s and the\n"
        "  read-only diagnostics pipe %s (restricted to the\n"
        "  creating user; see docs/ipc-protocol.md). --no-ipc\n"
        "  runs headless with no pipes.\n",
        argv0, openrgb2::ipc::kCommandPipeName,
        openrgb2::ipc::kDiagnosticsPipeName);
}

/* Device-type registry for workspace resolution: --devices-dir,
   else the conventional probe paths relative to the working
   directory. An empty registry is legal — only compact
   workspace documents actually need it. */
studio::PresetRegistry BuildDeviceRegistry(const std::string& dir)
{
    studio::PresetRegistry reg;
    std::vector<std::string> candidates;
    if(!dir.empty())
    {
        candidates.push_back(dir);
    }
    else
    {
        candidates.push_back("presets/devices");
        candidates.push_back("src/core/presets/devices");
    }
    for(const std::string& c : candidates)
    {
        std::error_code ec;
        if(dir.empty() && !std::filesystem::exists(c, ec))
        {
            continue;   /* probe candidate absent — try the next */
        }
        std::vector<std::string> errors;
        reg.LoadDirectory(c, &errors);
        for(const std::string& e : errors)
        {
            std::fprintf(stderr,
                "openrgb2-engine: device types: %s\n", e.c_str());
        }
        if(dir.empty() && reg.FileCount() == 0)
        {
            continue;
        }
        std::fprintf(stderr,
            "openrgb2-engine: device-type registry loaded %zu "
            "types from %s\n", reg.FileCount(), c.c_str());
        return reg;
    }
    return reg;
}

/* Task 3.8 offline bindings: record the identity each scene
   binding resolved TO (serial/location/zone + LED count) into
   the engine profile. A binding then resolves to the SAME
   device after restart — a missing device keeps its binding
   and a same-named different device can never inherit it.
   Only reports true on change (the owner loop must not
   rewrite the profile every 500 ms tick). */
bool RecordResolvedBindings(
    openrgb2::DeviceMapper& mapper,
    const std::vector<openrgb2::DeviceSnapshot>& snaps,
    openrgb2::EngineState& state)
{
    const std::shared_ptr<const studio::SceneDocument> doc =
        mapper.Scene();
    if(!doc)
    {
        return false;
    }
    bool changed = false;
    for(const studio::DeviceBinding& b : doc->bindings)
    {
        openrgb2::ZonePlacement zp;
        if(!mapper.BoundZone(b.id, zp))
        {
            continue;   /* unresolved/ambiguous — keep the last
                           recorded identity (missing-device
                           profile preservation). */
        }
        const auto sit = std::find_if(snaps.begin(), snaps.end(),
            [&zp](const openrgb2::DeviceSnapshot& s)
            { return s.key == zp.controller; });
        if(sit == snaps.end())
        {
            continue;
        }
        std::string zone_name = b.zone_name;
        if(zone_name.empty() && zp.zone_index < sit->zones.size())
        {
            zone_name = sit->zones[zp.zone_index].name;
        }
        const studio::DeviceBinding id =
            openrgb2::ResolvedBindingIdentity(b, *sit, zone_name,
                                              zp.led_count);
        studio::DeviceBinding& slot = state.bindings[b.id];
        if(slot.id              != id.id              ||
           slot.controller_name != id.controller_name ||
           slot.vendor          != id.vendor          ||
           slot.serial          != id.serial          ||
           slot.location        != id.location        ||
           slot.device_type     != id.device_type     ||
           slot.zone_name       != id.zone_name       ||
           slot.zone_leds       != id.zone_leds)
        {
            slot    = id;
            changed = true;
        }
    }
    return changed;
}

} /* anonymous namespace */

int main(int argc, char** argv)
{
    std::string scene_path;
    std::string effects_dir;
    std::string devices_dir;
    std::string state_path = "profiles/local/engine-state.json";
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
        else if((arg == "--devices-dir") && i + 1 < argc)
        {
            devices_dir = argv[++i];
        }
        else if((arg == "--state") && i + 1 < argc)
        {
            state_path = argv[++i];
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

    /* Playback-mode state machine (Task 3.6): owns the
       LiveEffects <-> AnimationPlayback contract — per-scene
       persisted selection, lane suppression (one producer per
       wireless group), SDK preset select, revision-based
       acknowledgement, pause/stop transport control, and the
       baked wired timeline that follows the validated
       wireless clock (no backend clock channel exists over
       the SDK wire, so clock_sample stays unset and sync is
       honestly reported as unavailable). All backend access
       goes through injected deps. */
    openrgb2::PlaybackController::Deps pdeps;
    pdeps.snapshots =
        [&sdk_client]() -> std::vector<openrgb2::DeviceSnapshot>
        {
            return sdk_client ? sdk_client->Snapshots()
                              : std::vector<openrgb2::DeviceSnapshot>{};
        };
    pdeps.wired_targets =
        [&sdk_client, &mapper]() -> std::vector<openrgb2::DeviceSnapshot>
        {
            std::vector<openrgb2::DeviceSnapshot> out;
            if(!sdk_client)
            {
                return out;
            }
            const std::vector<openrgb2::DeviceKey> bound =
                mapper.BoundControllers();
            for(const openrgb2::DeviceSnapshot& s :
                sdk_client->Snapshots())
            {
                if(!openrgb2::IsWirelessGroupController(s) &&
                   s.connection ==
                       openrgb2::ConnectionState::Connected &&
                   std::find(bound.begin(), bound.end(), s.key) !=
                       bound.end())
                {
                    out.push_back(s);
                }
            }
            return out;
        };
    pdeps.select_mode =
        [&sdk_client](const openrgb2::DeviceKey& k,
                      const std::string& name,
                      const openrgb2::PlaybackModePatch& p,
                      std::string& err)
        {
            if(!sdk_client)
            {
                err = "no SDK transport configured";
                return false;
            }
            openrgb2::sdk::ModePatch mp;
            mp.has_speed     = p.has_speed;     mp.speed     = p.speed;
            mp.has_direction = p.has_direction; mp.direction = p.direction;
            mp.has_colors    = p.has_colors;    mp.colors    = p.colors;
            if(!sdk_client->SelectMode(k, name, mp))
            {
                err = sdk_client->LastError();
                return false;
            }
            return true;
        };
    pdeps.active_mode =
        [&sdk_client](const openrgb2::DeviceKey& k,
                      openrgb2::BackendModeReport& rep)
        {
            if(!sdk_client)
            {
                return false;
            }
            int32_t           idx = -1;
            openrgb2::sdk::SdkMode m;
            if(!sdk_client->ActiveMode(k, idx, &m))
            {
                return false;
            }
            rep.valid     = true;
            rep.name      = m.name;
            rep.speed     = m.speed;
            rep.direction = m.direction;
            rep.colors    = m.colors;
            return true;
        };
    pdeps.refresh_status =
        [&sdk_client](std::string& err)
        {
            if(!sdk_client)
            {
                err = "no SDK transport configured";
                return false;
            }
            if(!sdk_client->RefreshStatus())
            {
                err = sdk_client->LastError();
                return false;
            }
            return true;
        };
    pdeps.set_custom_mode =
        [&sdk_client](const openrgb2::DeviceKey& k, std::string& err)
        {
            if(!sdk_client)
            {
                err = "no SDK transport configured";
                return false;
            }
            if(!sdk_client->SetCustomMode(k))
            {
                err = sdk_client->LastError();
                return false;
            }
            return true;
        };
    pdeps.direct_submit =
        [&sdk_client](const openrgb2::RgbFrame& f)
        {
            /* Pause-hold / stop-black bypass the suppressed
               lane — the controller is the one producer. */
            return sdk_client
                ? sdk_client->SubmitLatest(f)
                : openrgb2::SubmitResult::Reject(
                      "no SDK transport configured");
        };
    pdeps.wired_submit =
        [&pipeline](const openrgb2::RgbFrame& f)
        { return pipeline->SubmitLatest(f); };
    pdeps.set_suppressed =
        [&pipeline](const openrgb2::DeviceKey& k, bool s,
                    const std::string& r)
        { pipeline->SetSuppressed(k, s, r); };
    pdeps.drop_pending =
        [&pipeline](const openrgb2::DeviceKey& k)
        { return pipeline->DropPending(k); };
    pdeps.reset_delivered =
        [&pipeline](const openrgb2::DeviceKey& k)
        { pipeline->ResetDeliveredState(k); };
    pdeps.wait_idle =
        [&pipeline](const openrgb2::DeviceKey& k,
                    openrgb2::Clock::duration timeout)
        { return pipeline->WaitIdle(k, timeout); };
    /* clock_sample intentionally unset: the backend's
       GroupStatus/RgbClockSnapshot is in-process state not
       exposed over the SDK wire, and no backend diagnostics
       pipe exists yet — sync reports honest Unknown/Stale. */

    /*---- Task 3.8 — the durable engine profile -------------*\
    || profiles/local/ is gitignored user state: resolved      ||
    || identities (serials/locations) live here, never in      ||
    || committed files. The store does atomic temp+rename      ||
    || writes, keeps a last-known-good backup, and migrates    ||
    || older schema versions forward. A corrupt primary is     ||
    || reported honestly and recovered from LKG — never a      ||
    || blank or guessed state.                                 ||
    \*---------------------------------------------------------*/
    openrgb2::ProfileStore    profile(state_path);
    openrgb2::EngineState     engine_state;
    std::mutex                state_mutex;
    {
        const openrgb2::ProfileStore::LoadResult lr =
            profile.Load();
        switch(lr.status)
        {
        case openrgb2::ProfileStore::LoadStatus::Empty:
            break;   /* fresh start — not an error */
        case openrgb2::ProfileStore::LoadStatus::Loaded:
            engine_state = lr.state;
            break;
        case openrgb2::ProfileStore::LoadStatus::Migrated:
            engine_state = lr.state;
            std::fprintf(stderr,
                "openrgb2-engine: profile %s migrated to schema "
                "v%d\n", state_path.c_str(),
                openrgb2::kEngineStateSchemaVersion);
            break;
        case openrgb2::ProfileStore::LoadStatus::Recovered:
            engine_state = lr.state;
            std::fprintf(stderr,
                "openrgb2-engine: %s\n", lr.error.c_str());
            break;
        case openrgb2::ProfileStore::LoadStatus::Failed:
            /* Honest report; the engine still runs (the state
               is empty and flagged, never guessed). */
            std::fprintf(stderr,
                "openrgb2-engine: %s — running with no profile "
                "state\n", lr.error.c_str());
            break;
        }
        for(const std::string& w : lr.warnings)
        {
            std::fprintf(stderr,
                "openrgb2-engine: profile: %s\n", w.c_str());
        }
    }

    /* Durable per-scene playback selections (Task 3.8): the
       controller hands its whole LRU store on every change —
       write it atomically through the profile store. */
    pdeps.save_modes =
        [&profile, &engine_state, &state_mutex](
            const std::vector<openrgb2::PersistedSceneMode>& modes,
            std::string& err)
        {
            std::lock_guard<std::mutex> g(state_mutex);
            engine_state.scenes = modes;
            return profile.Save(engine_state, err);
        };

    openrgb2::PlaybackController playback(std::move(pdeps));
    {
        const size_t restored =
            playback.RestoreSceneModes(engine_state.scenes);
        if(restored > 0)
        {
            std::fprintf(stderr,
                "openrgb2-engine: restored %zu per-scene playback "
                "selections from %s\n", restored,
                state_path.c_str());
        }
    }

    /* Power lifecycle (Task 3.7): display-off gates ALL RGB
       frame output + reactive capture through the pipeline's
       output pause and freezes an active uploaded loop ONCE
       (explicit sampled-hold, never a write stream and never
       a maintenance stop); suspend discards pending work and
       parks the controller; wake/reconnect revalidates
       identity before any restore write. All hardware-facing
       operations are the injected deps below — the Windows
       monitor only posts events into this machine. */
    std::atomic<bool> capture_paused{ false };
    openrgb2::PowerLifecycle::Deps ldeps;
    ldeps.set_output_paused =
        [&pipeline](bool p, const std::string& r)
        { pipeline->SetOutputPaused(p, r); };
    ldeps.lane_keys =
        [&pipeline]() { return pipeline->LaneKeys(); };
    ldeps.drop_pending =
        [&pipeline](const openrgb2::DeviceKey& k)
        { return pipeline->DropPending(k); };
    ldeps.reset_delivered =
        [&pipeline](const openrgb2::DeviceKey& k)
        { pipeline->ResetDeliveredState(k); };
    ldeps.last_delivered =
        [&pipeline](const openrgb2::DeviceKey& k,
                    openrgb2::RgbFrame& out)
        { return pipeline->LastDelivered(k, out); };
    ldeps.reliable_submit =
        [&pipeline](const openrgb2::RgbFrame& f)
        { return pipeline->SubmitReliable(f); };
    ldeps.set_capture_paused =
        [&capture_paused](bool p)
        {
            /* The reactive-input pause seam: platform reactive
               samplers (a later milestone) must read this flag —
               GPU-side capture writes must not run while the
               display sleeps. */
            capture_paused.store(p);
        };
    ldeps.snapshots =
        [&sdk_client]() -> std::vector<openrgb2::DeviceSnapshot>
        {
            return sdk_client ? sdk_client->Snapshots()
                              : std::vector<openrgb2::DeviceSnapshot>{};
        };
    ldeps.playback = &playback;
    ldeps.log =
        [](const std::string& line)
        { std::fprintf(stderr, "openrgb2-engine: %s\n", line.c_str()); };
    openrgb2::PowerLifecycle lifecycle(std::move(ldeps));

    /* The Windows event source: a dedicated thread + message-
       only window carrying the display-state and suspend/resume
       registrations (Qt-free adaptation of the preserved
       DisplayPowerMonitor). Failure is non-fatal — the engine
       runs unmonitored (output never power-pauses) and the
       reason is logged. */
    openrgb2::platform::WinPowerMonitor power_monitor;
    {
        std::string perr;
        if(!power_monitor.Start(
            [&lifecycle](openrgb2::PowerLifecycle::Event e)
            { lifecycle.PostEvent(e); }, perr))
        {
            std::fprintf(stderr,
                "openrgb2-engine: power monitoring unavailable: "
                "%s (continuing)\n", perr.c_str());
        }
    }

    /* Every evaluated scene frame is split into per-controller
       complete-controller frames and handed to the pipeline via
       the engine's submitLatest sink. While an uploaded loop
       owns the output, live submissions to OTHER controllers
       are skipped (Tick produces the matching baked frames);
       submissions to the suppressed wireless group still run
       so the lane's rejection — never a silent drop — is
       exercised on every tick. */
    std::atomic<uint64_t> frames{ 0 };
    engine.Worker().SetFrameCallback(
        [&engine, &mapper, &frames, &playback](
            const openrgb2::EvaluatedFrame& eval)
        {
            ++frames;
            const bool owns = playback.PlaybackOwnsWired();
            for(const openrgb2::RgbFrame& frame : mapper.MapFrame(eval))
            {
                if(owns && !playback.OwnsGroup(frame.controller))
                {
                    continue;
                }
                engine.SubmitLatest(frame);
            }
            playback.Tick(eval.sample_time);
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
    ipc_deps.playback = &playback;
    ipc_deps.power    = &lifecycle;
    /* The "Quit All" hook (plan section 4): QUIT flips the
       owner loop's stop flag; the ordered, bounded teardown
       runs below (OrderlyShutdown). */
    ipc_deps.request_shutdown =
        [] { g_stop.store(true); };
    /* Task 3.8 offline bindings: a persisted resolved identity
       completes name-only scene bindings before apply — the
       same binding id keeps pointing at the same physical
       device across restarts, and a same-named different
       device can never inherit the binding. */
    ipc_deps.prepare_scene =
        [&engine_state, &state_mutex](
            std::shared_ptr<const studio::SceneDocument>& doc)
        {
            auto copy = std::make_shared<studio::SceneDocument>(*doc);
            {
                std::lock_guard<std::mutex> g(state_mutex);
                if(openrgb2::AugmentSceneBindings(*copy,
                                                engine_state.bindings))
                {
                    doc = copy;
                }
            }
        };
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
        /* Task 3.8: --scene resolves through the profile/
           workspace layer — expanded scene JSON, a compact
           studio.json workspace, or a legacy host-settings
           blob all land on the same SceneDocument. */
        const studio::PresetRegistry device_registry =
            BuildDeviceRegistry(devices_dir);
        std::shared_ptr<studio::SceneDocument> doc;
        std::string err;
        std::vector<std::string> warnings;
        if(!openrgb2::LoadSceneDocument(scene_path, device_registry,
                                        doc, err, &warnings))
        {
            std::fprintf(stderr, "openrgb2-engine: %s\n",
                         err.c_str());
            return 1;
        }
        for(const std::string& w : warnings)
        {
            std::fprintf(stderr, "openrgb2-engine: %s: %s\n",
                         scene_path.c_str(), w.c_str());
        }
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
        }

        /* Power lifecycle (Task 3.7): applies queued display/
           suspend events, diffs connection state for reconnect
           recovery, and runs the deferred wake restore — always
           BEFORE playback maintenance so the restore ordering
           (revalidate -> write) is never pre-empted by a poll-
           side convergence write. */
        lifecycle.Poll(openrgb2::Clock::now());
        /* Playback maintenance: acknowledgement polling, clock
           sampling, topology re-resolution. */
        playback.Poll(openrgb2::Clock::now());

        if(sdk_client)
        {
            const std::vector<openrgb2::DeviceSnapshot> snaps =
                sdk_client->Snapshots();
            mapper.SyncDevices(snaps);
            /* Task 3.8: persist the identities bindings resolved
               to — the offline binding table survives restarts
               and only writes on change. */
            {
                std::lock_guard<std::mutex> g(state_mutex);
                if(RecordResolvedBindings(mapper, snaps,
                                          engine_state))
                {
                    std::string serr;
                    if(!profile.Save(engine_state, serr))
                    {
                        std::fprintf(stderr,
                            "openrgb2-engine: profile save "
                            "failed: %s\n", serr.c_str());
                    }
                }
            }
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
                /* An uploaded loop owns this group — asserting
                   Direct mode here would stomp it (Task 3.6:
                   one producer per wireless group). */
                if(playback.OwnsGroup(key))
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

    /* Ordered, bounded shutdown (plan section 4 "Quit All shuts
       down owned processes in order" + section 6 bounded
       shutdown): event intake and command intake stop first,
       then frame production, then the delivery lanes (each join
       bounded by the transport's send timeout), then the
       transport itself. Steps run in registration order and
       each is measured against its declared bound — an overrun
       is reported honestly, never silently. */
    openrgb2::OrderlyShutdown shutdown;
    shutdown.Add("power-monitor",
                 std::chrono::milliseconds(2000),
                 [&power_monitor] { power_monitor.Stop(); });
    shutdown.Add("ipc-listeners",
                 std::chrono::milliseconds(2000),
                 [&cmd_listener, &diag_listener]
                 {
                     if(cmd_listener)  cmd_listener->Stop();
                     if(diag_listener) diag_listener->Stop();
                 });
    shutdown.Add("ipc-server",
                 std::chrono::milliseconds(2000),
                 [&ipc_server]
                 { if(ipc_server) ipc_server->Stop(); });
    shutdown.Add("scene-worker",
                 std::chrono::milliseconds(2000),
                 [&engine] { engine.Stop(); });
    /* One in-flight transport send per lane plus join time —
       bounded by the transport's own send timeout (4 s
       default); lanes join sequentially. */
    shutdown.Add("delivery-pipeline",
                 std::chrono::milliseconds(12000),
                 [&pipeline] { pipeline->Stop(); });
    shutdown.Add("sdk-transport",
                 std::chrono::milliseconds(1000),
                 [&sdk_client]
                 { if(sdk_client) sdk_client->Disconnect(); });

    const openrgb2::OrderlyShutdown::Result sd = shutdown.Run();
    for(const openrgb2::OrderlyShutdown::StepResult& s : sd.steps)
    {
        if(!s.within)
        {
            std::fprintf(stderr,
                "openrgb2-engine: shutdown step '%s' overran its "
                "bound (%lld ms > %lld ms)\n",
                s.name.c_str(),
                (long long)std::chrono::duration_cast<
                    std::chrono::milliseconds>(s.elapsed).count(),
                (long long)std::chrono::duration_cast<
                    std::chrono::milliseconds>(s.bound).count());
        }
    }

    const double elapsed =
        std::chrono::duration<double>(openrgb2::Clock::now() - started)
            .count();
    std::fprintf(stderr,
        "openrgb2-engine: stopped after %.2f s, %llu scene frames "
        "(shutdown %lld ms)\n",
        elapsed, (unsigned long long)frames.load(),
        (long long)std::chrono::duration_cast<
            std::chrono::milliseconds>(sd.elapsed).count());
    return 0;
}
