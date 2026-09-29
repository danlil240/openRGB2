/*---------------------------------------------------------*\
|| EngineHost.cpp                                            ||
||                                                           ||
||   The end-to-end engine composition (Task 3.9). Moved    ||
||   verbatim-intent from the former main.cpp wiring — see  ||
||   EngineHost.h for the startup order and the seams.      ||
||                                                           ||
||   SPDX-License-Identifier: GPL-2.0-or-later               ||
\*---------------------------------------------------------*/

#include "EngineHost.h"

#include "SceneLoader.h"

#include "WinReactiveInputs.h"

#include "TcpSocket.h"

#include "effects/Presets.h"
#include "presets/PresetRegistry.h"
#include "scene/SceneJson.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <thread>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace openrgb2
{

namespace
{

/* Per-controller delivery budgets (plan section 4 — budgets are
   qualified per controller, never a global rate). The wireless
   group's measured ceiling is ~33.5 admitted uploads/s
   (docs/benchmarks/wireless-result.md), so it is paced at 30 ms.
   Qualification is device-aware: IsWirelessGroupController keys
   on the wireless service's own transport identity (the
   "Wireless:" location prefix), not a name heuristic. Other
   controllers are unpaced until measured — the 60 Hz scene
   clock already bounds how often producers can submit. */
Clock::duration BudgetFor(const DeviceSnapshot& snap)
{
    if(IsWirelessGroupController(snap))
    {
        return std::chrono::milliseconds(30);
    }
    return Clock::duration::zero();
}

void SyncLanes(DeliveryPipeline& pipeline,
               const sdk::OpenRgbClient& client)
{
    const std::vector<DeviceSnapshot> snaps = client.Snapshots();
    pipeline.SyncDevices(snaps);
    for(const DeviceSnapshot& snap : snaps)
    {
        /* Only re-apply on change — SetBudget notifies the lane
           even when the value is identical. */
        Clock::duration current;
        const Clock::duration want = BudgetFor(snap);
        if(!pipeline.Budget(snap.key, current) || current != want)
        {
            pipeline.SetBudget(snap.key, want);
        }
    }
}

/* Device-type registry for workspace resolution: the explicit
   --devices-dir, else the conventional probe paths relative to
   the working directory. An empty registry is legal — only
   compact workspace documents actually need it. */
studio::PresetRegistry BuildDeviceRegistry(
    const std::string& dir,
    const std::function<void(const std::string&)>& log)
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
            log("device types: " + e);
        }
        if(dir.empty() && reg.FileCount() == 0)
        {
            continue;
        }
        log("device-type registry loaded " +
            std::to_string(reg.FileCount()) + " types from " + c);
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
   rewrite the profile every tick). */
bool RecordResolvedBindings(
    DeviceMapper& mapper,
    const std::vector<DeviceSnapshot>& snaps,
    EngineState& state)
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
        ZonePlacement zp;
        if(!mapper.BoundZone(b.id, zp))
        {
            continue;   /* unresolved/ambiguous — keep the last
                           recorded identity (missing-device
                           profile preservation). */
        }
        const auto sit = std::find_if(snaps.begin(), snaps.end(),
            [&zp](const DeviceSnapshot& s)
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
            ResolvedBindingIdentity(b, *sit, zone_name,
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

EngineHost::EngineHost(EngineHostOptions options)
    : opts_(std::move(options))
{
    if(!opts_.log)
    {
        opts_.log = [](const std::string& line)
        { std::fprintf(stderr, "openrgb2-engine: %s\n", line.c_str()); };
    }
}

EngineHost::~EngineHost()
{
    Stop();
}

void EngineHost::Log(const std::string& line) const
{
    opts_.log(line);
}

uint64_t EngineHost::FramesProduced() const
{
    return frames_.load();
}

void EngineHost::RequestStop()
{
    stop_.store(true);
}

bool EngineHost::StopRequested() const
{
    return stop_.load();
}

bool EngineHost::Started() const
{
    return started_;
}

/*---------------------------------------------------------*\
|| Ownership scan (Task 3.9): report-only.                   ||
\*---------------------------------------------------------*/
void EngineHost::ScanOwners()
{
    ownership_.scanned = true;

    if(opts_.sdk_enabled)
    {
        ownership_.backend_pid =
            FindTcpListenerPid(opts_.sdk_port);
        ownership_.backend_listening = ownership_.backend_pid != 0;
    }

    uint32_t self_pid = 0;
#ifdef _WIN32
    self_pid = GetCurrentProcessId();
#endif
    ownership_.findings = ClassifyOwners(EnumerateProcesses(),
                                         self_pid,
                                         ownership_.backend_pid);
    if(ownership_.findings.empty())
    {
        Log("ownership scan: no competing RGB owners detected");
    }
    for(const OwnerFinding& f : ownership_.findings)
    {
        Log(std::string("ownership ") +
            (f.conflict ? "CONFLICT" : "notice") + ": " +
            f.owner + " (pid " + std::to_string(f.pid) + ", " +
            f.exe + ") — " + f.detail);
    }

    /* The managed config's sole-writer check covers BOTH
       plugin-load paths (plan section 4): <config>/plugins +
       OpenRGB.json and the Windows system-plugin dir beside
       the backend exe — resolved from the listener PID's image
       path. An unreadable image (e.g. the backend runs
       elevated and we do not) is reported honestly, and the
       check still runs on whichever scope is known. */
    std::string backend_exe_dir;
    if(ownership_.backend_pid != 0)
    {
        const std::string img =
            ProcessImagePath(ownership_.backend_pid);
        if(!img.empty())
        {
            const std::string::size_type p =
                img.find_last_of("/\\");
            backend_exe_dir =
                p == std::string::npos ? img : img.substr(0, p);
        }
        else
        {
            Log("ownership: backend image path unreadable (pid " +
                std::to_string(ownership_.backend_pid) +
                ") — exe-dir plugin scan skipped");
        }
    }
    if(!opts_.backend_config_dir.empty() || !backend_exe_dir.empty())
    {
        ownership_.config_checked = true;
        ownership_.config =
            CheckManagedBackendConfig(opts_.backend_config_dir,
                                      backend_exe_dir);
        for(const std::string& f : ownership_.config.findings)
        {
            Log("managed-config: " + f);
        }
    }
}

nlohmann::json EngineHost::OwnershipJson() const
{
    /* Findings verbatim — the report-only contract (plan
       section 4) means the UI shows the scan's conflicts,
       never a kill button or a UI-side re-classification. */
    nlohmann::json conflicts = nlohmann::json::array();
    nlohmann::json notices   = nlohmann::json::array();
    for(const OwnerFinding& f : ownership_.findings)
    {
        nlohmann::json e = nlohmann::json::object({
            { "owner",  f.owner  },
            { "exe",    f.exe    },
            { "pid",    f.pid    },
            { "detail", f.detail },
        });
        (f.conflict ? conflicts : notices).push_back(
            std::move(e));
    }
    return nlohmann::json::object({
        { "scanned", ownership_.scanned },
        { "backend", nlohmann::json::object({
            { "listening", ownership_.backend_listening },
            { "attached",  ownership_.backend_attached  },
        })},
        { "conflicts", std::move(conflicts) },
        { "notices",   std::move(notices)   },
        { "config", nlohmann::json::object({
            { "checked",           ownership_.config_checked },
            { "findings",          ownership_.config.findings },
            { "enabled_plugins",
              ownership_.config.enabled_plugins },
            { "legacy_producers",
              ownership_.config.legacy_producers },
        })},
    });
}

/*---------------------------------------------------------*\
|| Injected deps (Tasks 3.6/3.7)                            ||
\*---------------------------------------------------------*/

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
PlaybackController::Deps EngineHost::BuildPlaybackDeps()
{
    PlaybackController::Deps pdeps;
    pdeps.snapshots =
        [this]() -> std::vector<DeviceSnapshot>
        {
            return sdk_ ? sdk_->Snapshots()
                        : std::vector<DeviceSnapshot>{};
        };
    pdeps.wired_targets =
        [this]() -> std::vector<DeviceSnapshot>
        {
            std::vector<DeviceSnapshot> out;
            if(!sdk_)
            {
                return out;
            }
            const std::vector<DeviceKey> bound =
                mapper_.BoundControllers();
            for(const DeviceSnapshot& s : sdk_->Snapshots())
            {
                if(!IsWirelessGroupController(s) &&
                   s.connection == ConnectionState::Connected &&
                   std::find(bound.begin(), bound.end(), s.key) !=
                       bound.end())
                {
                    out.push_back(s);
                }
            }
            return out;
        };
    pdeps.select_mode =
        [this](const DeviceKey& k, const std::string& name,
               const PlaybackModePatch& p, std::string& derr)
        {
            if(!sdk_)
            {
                derr = "no SDK transport configured";
                return false;
            }
            sdk::ModePatch mp;
            mp.has_speed     = p.has_speed;     mp.speed     = p.speed;
            mp.has_direction = p.has_direction; mp.direction = p.direction;
            mp.has_colors    = p.has_colors;    mp.colors    = p.colors;
            if(!sdk_->SelectMode(k, name, mp))
            {
                derr = sdk_->LastError();
                return false;
            }
            return true;
        };
    pdeps.active_mode =
        [this](const DeviceKey& k, BackendModeReport& rep)
        {
            if(!sdk_)
            {
                return false;
            }
            int32_t        idx = -1;
            sdk::SdkMode   m;
            if(!sdk_->ActiveMode(k, idx, &m))
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
        [this](std::string& derr)
        {
            if(!sdk_)
            {
                derr = "no SDK transport configured";
                return false;
            }
            if(!sdk_->RefreshStatus())
            {
                derr = sdk_->LastError();
                return false;
            }
            return true;
        };
    pdeps.set_custom_mode =
        [this](const DeviceKey& k, std::string& derr)
        {
            if(!sdk_)
            {
                derr = "no SDK transport configured";
                return false;
            }
            if(!sdk_->SetCustomMode(k))
            {
                derr = sdk_->LastError();
                return false;
            }
            return true;
        };
    pdeps.direct_submit =
        [this](const RgbFrame& f)
        {
            /* Pause-hold / stop-black bypass the suppressed
               lane — the controller is the one producer. */
            return sdk_
                ? sdk_->SubmitLatest(f)
                : SubmitResult::Reject(
                      "no SDK transport configured");
        };
    pdeps.wired_submit =
        [this](const RgbFrame& f)
        { return pipeline_->SubmitLatest(f); };
    pdeps.set_suppressed =
        [this](const DeviceKey& k, bool s, const std::string& r)
        { pipeline_->SetSuppressed(k, s, r); };
    pdeps.drop_pending =
        [this](const DeviceKey& k)
        { return pipeline_->DropPending(k); };
    pdeps.reset_delivered =
        [this](const DeviceKey& k)
        { pipeline_->ResetDeliveredState(k); };
    pdeps.wait_idle =
        [this](const DeviceKey& k, Clock::duration timeout)
        { return pipeline_->WaitIdle(k, timeout); };
    /* clock_sample intentionally unset: the backend's
       GroupStatus/RgbClockSnapshot is in-process state not
       exposed over the SDK wire, and no backend diagnostics
       pipe exists yet — sync reports honest Unknown/Stale. */
    return pdeps;
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
PowerLifecycle::Deps EngineHost::BuildLifecycleDeps()
{
    PowerLifecycle::Deps ldeps;
    ldeps.set_output_paused =
        [this](bool p, const std::string& r)
        { pipeline_->SetOutputPaused(p, r); };
    ldeps.lane_keys =
        [this]() { return pipeline_->LaneKeys(); };
    ldeps.drop_pending =
        [this](const DeviceKey& k)
        { return pipeline_->DropPending(k); };
    ldeps.reset_delivered =
        [this](const DeviceKey& k)
        { pipeline_->ResetDeliveredState(k); };
    ldeps.last_delivered =
        [this](const DeviceKey& k, RgbFrame& out)
        { return pipeline_->LastDelivered(k, out); };
    ldeps.reliable_submit =
        [this](const RgbFrame& f)
        { return pipeline_->SubmitReliable(f); };
    ldeps.set_capture_paused =
        [this](bool p)
        {
            /* The reactive-input pause seam (Task 4.6): the
               ReactiveInputs manager reads this — paused
               sources skip capture work (the screen sampler
               does NO GPU reads while displays sleep) and the
               scene worker sees an empty input state on the
               next tick. */
            capture_paused_.store(p);
            if(reactive_)
            {
                reactive_->SetPaused(p);
            }
        };
    ldeps.snapshots =
        [this]() -> std::vector<DeviceSnapshot>
        {
            return sdk_ ? sdk_->Snapshots()
                        : std::vector<DeviceSnapshot>{};
        };
    ldeps.playback = playback_.get();
    ldeps.log =
        [this](const std::string& line) { Log(line); };
    return ldeps;
}

/*---------------------------------------------------------*\
|| Startup                                                  ||
\*---------------------------------------------------------*/
bool EngineHost::Start(std::string& err)
{
    if(started_)
    {
        err = "already started";
        return false;
    }

    /* 1. Single instance (plan section 4): exactly one engine
       owns the managed configuration. Refusing to start is the
       honest answer to a double-own attempt — the holder is
       reported, never taken over or killed. */
    if(opts_.single_instance)
    {
        instance_ = std::make_unique<SingleInstanceGuard>(
            opts_.instance_name);
        if(!instance_->Acquire(err))
        {
            return false;
        }
    }

    /* 2. Competing-owner scan + managed-config sole-writer
       check — report only, both non-fatal. */
    if(opts_.scan_owners)
    {
        ScanOwners();
    }

    /* The delivery pipeline is the engine's submitLatest sink:
       per-controller depth-1 pending lanes with independent
       budgets, unchanged-frame suppression, and the distinct
       reliable convergence path (Task 3.3). It drains into the
       SDK transport below; with no transport configured its
       lanes simply idle. */
    pipeline_ = std::make_shared<DeliveryPipeline>();
    engine_.SetFrameSink(pipeline_);

    /* 4. Backend attach: the backend is launched EXTERNALLY
       (plan section 4 — `--server --server-host 127.0.0.1
       --server-port 6744 --noautoconnect --localconfig` with a
       private configuration directory as its working dir) and
       remains the sole hardware owner; this process never
       spawns it. The scan above already resolved whether the
       port is listening; Connect() failing while the port is
       held means a foreign service — reported as a conflict. */
    if(opts_.sdk_enabled)
    {
        sdk::OpenRgbClientOptions copts;
        copts.host    = opts_.sdk_host;
        copts.port    = opts_.sdk_port;
        copts.backoff = opts_.sdk_backoff;
        std::unique_ptr<sdk::IStreamTransport> transport =
            opts_.transport_factory
                ? opts_.transport_factory()
                : std::make_unique<sdk::TcpSocket>();
        sdk_ = std::make_shared<sdk::OpenRgbClient>(
            copts, std::move(transport));
        pipeline_->SetTransport(sdk_);

        if(ownership_.backend_listening)
        {
            Log("backend listener detected on port " +
                std::to_string(opts_.sdk_port) + " (pid " +
                std::to_string(ownership_.backend_pid) +
                ") — attaching via SDK; the engine does not "
                "own the backend process");
        }
        if(sdk_->Connect())
        {
            ownership_.backend_attached = true;
            Log("SDK connected to " + copts.host + ":" +
                std::to_string(copts.port) + " (protocol " +
                std::to_string(sdk_->ProtocolVersion()) + ", " +
                std::to_string(sdk_->Snapshots().size()) +
                " controllers)");
        }
        else if(ownership_.backend_listening)
        {
            Log("ownership CONFLICT: port " +
                std::to_string(opts_.sdk_port) +
                " is held by a process that did not complete "
                "the OpenRGB SDK handshake (" +
                sdk_->LastError() + ") — another owner may be "
                "squatting on the managed endpoint; will keep "
                "retrying");
            sdk_->ReconnectNow();
        }
        else
        {
            /* Not fatal: the sink reports Disconnected and the
               bounded backoff keeps retrying via Poll(). */
            Log("no backend detected at " + copts.host + ":" +
                std::to_string(copts.port) + " (" +
                sdk_->LastError() + ") — launch the pinned "
                "backend with --server --server-host 127.0.0.1 "
                "--server-port 6744 --noautoconnect "
                "--localconfig; will retry");
            sdk_->ReconnectNow();
        }
        SyncLanes(*pipeline_, *sdk_);
    }

    /* Playback-mode state machine (Task 3.6): the deps are
       built first, then the durable-profile block below adds
       save_modes before the controller is constructed. */
    PlaybackController::Deps pdeps = BuildPlaybackDeps();

    /*---- Task 3.8 — the durable engine profile -------------*\
    || profiles/local/ is gitignored user state: resolved      ||
    || identities (serials/locations) live here, never in      ||
    || committed files. The store does atomic temp+rename      ||
    || writes, keeps a last-known-good backup, and migrates    ||
    || older schema versions forward. A corrupt primary is     ||
    || reported honestly and recovered from LKG — never a      ||
    || blank or guessed state.                                 ||
    \*---------------------------------------------------------*/
    profile_ = std::make_unique<ProfileStore>(opts_.state_path);
    {
        const ProfileStore::LoadResult lr = profile_->Load();
        switch(lr.status)
        {
        case ProfileStore::LoadStatus::Empty:
            break;   /* fresh start — not an error */
        case ProfileStore::LoadStatus::Loaded:
            engine_state_ = lr.state;
            break;
        case ProfileStore::LoadStatus::Migrated:
            engine_state_ = lr.state;
            Log("profile " + opts_.state_path +
                " migrated to schema v" +
                std::to_string(kEngineStateSchemaVersion));
            break;
        case ProfileStore::LoadStatus::Recovered:
            engine_state_ = lr.state;
            Log(lr.error);
            break;
        case ProfileStore::LoadStatus::Failed:
            /* Honest report; the engine still runs (the state
               is empty and flagged, never guessed). */
            Log(lr.error + " — running with no profile state");
            break;
        }
        for(const std::string& w : lr.warnings)
        {
            Log("profile: " + w);
        }
    }

    /* Durable per-scene playback selections (Task 3.8): the
       controller hands its whole LRU store on every change —
       write it atomically through the profile store. */
    pdeps.save_modes =
        [this](const std::vector<PersistedSceneMode>& modes,
               std::string& serr)
        {
            std::lock_guard<std::mutex> g(state_mutex_);
            engine_state_.scenes = modes;
            return profile_->Save(engine_state_, serr);
        };

    playback_ = std::make_unique<PlaybackController>(
        std::move(pdeps));
    {
        const size_t restored =
            playback_->RestoreSceneModes(engine_state_.scenes);
        if(restored > 0)
        {
            Log("restored " + std::to_string(restored) +
                " per-scene playback selections from " +
                opts_.state_path);
        }
    }

    /*---- Task 4.6 reactive inputs ----------------------------*\
    || Audio / screen / key capture, Qt-free ports of the        ||
    || preserved plugin's inputs. The manager itself is cheap:   ||
    || nothing captures until an input is enabled through IPC.   ||
    || Three honest wirings:                                     ||
    ||   - the bus stamps events on the scene clock so ages      ||
    ||     evaluate correctly against the tick's scene_time_s    ||
    ||   - the worker PULLS a Snapshot() each tick (the          ||
    ||     InputProvider hook), so pause/disable stops feeding   ||
    ||     the scene on the very next frame                      ||
    ||   - the persisted selection re-arms on start; a source    ||
    ||     that fails to start stays disabled and reports its    ||
    ||     own reason — never a fabricated toggle                ||
    \*---------------------------------------------------------*/
    if(opts_.reactive_inputs)
    {
        ReactiveInputs::Deps rdeps;
        rdeps.source_factory =
            opts_.reactive_factory
                ? opts_.reactive_factory
                : [](const std::string& id)
                  { return platform::MakeReactiveSource(id); };
        rdeps.enumerate_displays =
            [] { return platform::EnumerateDisplays(); };
        rdeps.now =
            [this]
            {
                /* Scene-clock seconds. The worker's epoch is
                   only set once engine_.Start() runs (Restore
                   above races ahead of it) — until then stamp
                   t = 0 so early events age against the same
                   base the first ticks evaluate at. */
                if(!engine_.Worker().Running())
                {
                    return 0.0;
                }
                return std::chrono::duration<double>(
                    Clock::now() - engine_.Worker().Epoch())
                    .count();
            };
        rdeps.changed =
            [this] { OnReactiveChanged(); };
        reactive_ = std::make_unique<ReactiveInputs>(
            std::move(rdeps));
        engine_.Worker().SetInputProvider(
            [this]() { return reactive_->Snapshot(); });
        reactive_->Restore(engine_state_.reactive);
        {
            const auto persisted = engine_state_.reactive;
            for(const auto& p : persisted)
            {
                if(p.enabled)
                {
                    Log("reactive input '" + p.id +
                        "' re-armed from " + opts_.state_path);
                }
            }
        }
    }

    lifecycle_ = std::make_unique<PowerLifecycle>(
        BuildLifecycleDeps());

    /* The Windows event source: a dedicated thread + message-
       only window carrying the display-state and suspend/resume
       registrations (Qt-free adaptation of the preserved
       DisplayPowerMonitor). Failure is non-fatal — the engine
       runs unmonitored (output never power-pauses) and the
       reason is logged. */
    if(opts_.power_monitor)
    {
        std::string perr;
        if(!power_monitor_.Start(
            [this](PowerLifecycle::Event e)
            { lifecycle_->PostEvent(e); }, perr))
        {
            Log("power monitoring unavailable: " + perr +
                " (continuing)");
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
    engine_.Worker().SetFrameCallback(
        [this](const EvaluatedFrame& eval)
        {
            ++frames_;
            const bool owns = playback_->PlaybackOwnsWired();
            for(const RgbFrame& frame : mapper_.MapFrame(eval))
            {
                if(owns && !playback_->OwnsGroup(frame.controller))
                {
                    continue;
                }
                engine_.SubmitLatest(frame);
            }
            playback_->Tick(eval.sample_time);
        });

    if(!opts_.effects_dir.empty())
    {
        /* Preset-look files loaded as the registry's defaults
           layer; DLS_EFFECT_DIR and the source-tree probe still
           apply. */
        studio::EffectLooks().LoadDefaultsDirectory(
            opts_.effects_dir, nullptr);
    }

    /* The IPC service owns the scene-apply path (Task 3.5): the
       --scene CLI and SET_SCENE both go through
       ApplySceneDocument, so a UI-applied scene and a
       CLI-loaded scene are indistinguishable to the engine. */
    {
        ipc::EngineService::Deps ipc_deps;
        ipc_deps.pipeline = pipeline_.get();
        ipc_deps.mapper   = &mapper_;
        ipc_deps.sdk      = sdk_.get();
        ipc_deps.playback = playback_.get();
        ipc_deps.power    = lifecycle_.get();
        /* Task 4.6: the REACTIVE_COMMAND surface — nullptr is
           legal; the service then reports available=false
           honestly rather than fabricating toggles. */
        ipc_deps.reactive = reactive_.get();
        /* Task 4.4 layout verbs: identify and the set_binding
           create path resolve against live topology — the
           same source the mapper syncs from. */
        ipc_deps.snapshots =
            [this]() -> std::vector<DeviceSnapshot>
            {
                return sdk_ ? sdk_->Snapshots()
                            : std::vector<DeviceSnapshot>{};
            };
        /* The "Quit All" hook (plan section 4): QUIT flips the
           owner loop's stop flag; the ordered, bounded teardown
           runs in Stop() (OrderlyShutdown). */
        ipc_deps.request_shutdown =
            [this] { stop_.store(true); };
        /* Task 4.7: the startup ownership scan surfaced
           read-only in STATE/DIAGNOSTICS. ownership_ is
           written during Start() before the IPC surface
           exists — the publish order (scan -> service ->
           listeners) makes the provider race-free. */
        ipc_deps.ownership =
            [this] { return OwnershipJson(); };
        /* Task 3.8 offline bindings: a persisted resolved
           identity completes name-only scene bindings before
           apply — the same binding id keeps pointing at the
           same physical device across restarts, and a
           same-named different device can never inherit the
           binding. */
        ipc_deps.prepare_scene =
            [this](std::shared_ptr<const studio::SceneDocument>& doc)
            {
                auto copy =
                    std::make_shared<studio::SceneDocument>(*doc);
                {
                    std::lock_guard<std::mutex> g(state_mutex_);
                    if(AugmentSceneBindings(*copy,
                                            engine_state_.bindings))
                    {
                        doc = copy;
                    }
                }
            };
        service_ = std::make_unique<ipc::EngineService>(
            engine_, std::move(ipc_deps));
    }

    /* Task 2 (studio-import): the managed workspace store +
       commit service. Its publish seam is the same scene-apply
       path --scene and SET_SCENE use; its libraries are the
       packaged types/looks plus the managed root's own
       library layer (types the workspace actually uses are
       snapshotted per generation regardless). The store is
       purely file state — constructing it never touches
       output. */
    {
        WorkspaceService::Deps wdeps;
        wdeps.devices = BuildDeviceRegistry(
            opts_.devices_dir,
            [this](const std::string& l) { Log(l); });
        wdeps.effects = studio::EffectLooks();
        wdeps.apply_scene =
            [this](std::shared_ptr<const studio::SceneDocument> doc,
                   std::string& aerr) -> bool
            {
                return service_ &&
                       service_->ApplySceneDocument(std::move(doc),
                                                  aerr);
            };
        workspaces_ = std::make_unique<WorkspaceService>(
            opts_.workspace_root.empty()
                ? WorkspaceStore::DefaultRoot()
                : opts_.workspace_root,
            std::move(wdeps));
    }

    if(opts_.ipc_enabled && !StartIpc())
    {
        /* StartIpc logs the honest reason and continues —
           headless operation is valid (plan section 4: no UI
           required). */
    }

    if(!ApplyStartupScene(err))
    {
        return false;
    }

    if(!engine_.Start())
    {
        err = "failed to start scene worker";
        return false;
    }
    Log("running (60 Hz scene clock, no UI required)");

    /* Per-lane delivery workers: a lane blocked inside a slow
       transport send cannot stall the others. */
    pipeline_->Start();
    started_ = true;
    return true;
}

bool EngineHost::StartIpc()
{
    bool any = false;
    ipc_server_ = std::make_unique<ipc::IpcServer>(*service_);
    service_->SetEventSink(
        [this](const ipc::IpcEvent& ev) { ipc_server_->Emit(ev); });
    service_->SetServerStatsProvider(
        [this]() { return ipc_server_->Stats(); });

    /* Both pipes share the restricted DACL (creating user +
       LocalSystem only). The diagnostics pipe is read-only
       by construction: DiagGet is the only verb it serves. */
    cmd_listener_ = std::make_unique<ipc::NamedPipeListener>(
        *ipc_server_, ipc::ChannelKind::Command,
        opts_.command_pipe_name);
    diag_listener_ = std::make_unique<ipc::NamedPipeListener>(
        *ipc_server_, ipc::ChannelKind::Diagnostics,
        opts_.diagnostics_pipe_name);
    std::string err;
    if(!cmd_listener_->Start(err))
    {
        Log("command pipe unavailable: " + err +
            " (continuing without command IPC)");
        cmd_listener_.reset();
    }
    else
    {
        any = true;
    }
    err.clear();
    if(!diag_listener_->Start(err))
    {
        Log("diagnostics pipe unavailable: " + err +
            " (continuing)");
        diag_listener_.reset();
    }
    else
    {
        any = true;
    }
    return any;
}

bool EngineHost::ApplyStartupScene(std::string& err)
{
    if(opts_.scene_path.empty())
    {
        return true;
    }
    /* Task 2: --scene loads through the canonical workspace
       candidate — expanded scene JSON, a compact studio.json
       workspace, a v1/v2 expanded workspace, or a legacy
       host-settings blob all land on the same resolved
       SceneDocument while the compact authoring form survives
       for the workspace layer. */
    const studio::PresetRegistry device_registry =
        BuildDeviceRegistry(opts_.devices_dir,
                            [this](const std::string& l) { Log(l); });
    WorkspaceCandidate cand;
    std::vector<std::string> warnings;
    if(!LoadWorkspaceCandidate(opts_.scene_path, device_registry,
                               studio::EffectLooks(),
                               cand, err, &warnings))
    {
        return false;
    }
    for(const std::string& w : warnings)
    {
        Log(opts_.scene_path + ": " + w);
    }
    Log(opts_.scene_path + ": loaded " +
        WorkspaceSourceFormName(cand.source_form) +
        (cand.migrated ? " (migrated)" : ""));
    if(!service_->ApplySceneDocument(cand.resolved, err))
    {
        err = "scene apply failed: " + err;
        return false;
    }
    return true;
}

/*---------------------------------------------------------*\
|| Owner loop                                               ||
\*---------------------------------------------------------*/
/* Task 4.6 reactive-input change hook: the manager's
   `changed` dep — emits the IPC event and persists the
   selection. Runs on whichever thread applied the change
   (IPC session threads for set_enabled/set_params, the
   owner loop for the pause gate); the profile save is
   bounded like every other state_mutex_ holder. */
void EngineHost::OnReactiveChanged()
{
    if(service_)
    {
        service_->EmitReactive();
    }
    if(!reactive_ || !profile_)
    {
        return;
    }
    /* Persist ONLY when the recorded selection actually
       moved — pause transitions and unchanged-value param
       writes (slider debounce ticks) must not rewrite the
       profile: PersistedState doesn't record pause. */
    const std::vector<ReactiveInputs::PersistedInput>
        persisted = reactive_->PersistedState();
    std::string serr;
    std::lock_guard<std::mutex> g(state_mutex_);
    if(persisted == engine_state_.reactive)
    {
        return;
    }
    engine_state_.reactive = persisted;
    if(!profile_->Save(engine_state_, serr))
    {
        Log("profile save failed: " + serr);
    }
}

void EngineHost::PollOnce()
{
    if(ipc_server_)
    {
        /* Diff-driven push events: topology, connection,
           playback changes and the bounded-rate
           frame_status feed (Task 3.5). */
        service_->PollEvents();
    }
    if(sdk_)
    {
        sdk_->Poll();
        ReprobeSdk(Clock::now());
        /* Task 5.3 topology revalidation (option doc above):
           a bounded RefreshStatus on a slow cadence so a
           mid-session unplug/replug is detected within the
           interval instead of misrouting frames through a
           stale positional index until the socket happens
           to break. Failures mark the connection broken —
           the bounded reconnect path takes over. */
        if(sdk_->State() == ConnectionState::Connected)
        {
            const TimePoint now = Clock::now();
            if(now - last_topology_poll_ >= opts_.sdk_topology_poll)
            {
                last_topology_poll_ = now;
                sdk_->RefreshStatus();
            }
        }
        /* Mirror connection/topology into the lanes so
           validation tracks the live backend state and new
           controllers get lanes without a restart. */
        SyncLanes(*pipeline_, *sdk_);
    }

    /* Power lifecycle (Task 3.7): applies queued display/
       suspend events, diffs connection state for reconnect
       recovery, and runs the deferred wake restore — always
       BEFORE playback maintenance so the restore ordering
       (revalidate -> write) is never pre-empted by a poll-
       side convergence write. */
    lifecycle_->Poll(Clock::now());
    /* Playback maintenance: acknowledgement polling, clock
       sampling, topology re-resolution. */
    playback_->Poll(Clock::now());

    if(sdk_)
    {
        const std::vector<DeviceSnapshot> snaps = sdk_->Snapshots();
        mapper_.SyncDevices(snaps);
        /* Task 3.8: persist the identities bindings resolved
           to — the offline binding table survives restarts
           and only writes on change. */
        {
            std::lock_guard<std::mutex> g(state_mutex_);
            if(RecordResolvedBindings(mapper_, snaps,
                                      engine_state_))
            {
                std::string serr;
                if(!profile_->Save(engine_state_, serr))
                {
                    Log("profile save failed: " + serr);
                }
            }
        }
        for(const DeviceKey& key : mapper_.BoundControllers())
        {
            const auto snap_it =
                std::find_if(snaps.begin(), snaps.end(),
                    [&key](const DeviceSnapshot& s)
                    { return s.key == key; });
            if(snap_it == snaps.end() ||
               snap_it->connection != ConnectionState::Connected)
            {
                continue;
            }
            /* An uploaded loop owns this group — asserting
               Direct mode here would stomp it (Task 3.6:
               one producer per wireless group). */
            if(playback_->OwnsGroup(key))
            {
                continue;
            }
            /* Task 5.3: no upkeep writes while the power gate
               is asserted — SETCUSTOMMODE is a hardware write
               and must not cross a suspend/display-off
               boundary (it bypasses the lane gate the same
               way the controller's own freeze does — but the
               freeze is the only allowed exception, and it
               goes through the playback controller). The
               generation bookkeeping stays stale, so the
               upkeep re-applies once the gate releases. */
            if(lifecycle_ && lifecycle_->OutputPaused())
            {
                continue;
            }
            uint64_t& done = custom_mode_done_[key];
            if(done != snap_it->topology_generation)
            {
                if(sdk_->SetCustomMode(key))
                {
                    done = snap_it->topology_generation;
                }
            }
        }
    }
}

/* Task 5.3: the bounded backoff is deliberate, but latching
   Disconnected FOREVER after max_attempts is not — a backend
   outage longer than the backoff window (a machine boot,
   a slow manual restart) left the engine permanently
   detached: nothing called ReconnectNow() after Start().
   Poll-side re-arm at a bounded rate recovers honestly. */
void EngineHost::ReprobeSdk(TimePoint now)
{
    if(sdk_->State() != ConnectionState::Disconnected)
    {
        return;
    }
    if(now - last_reprobe_ < opts_.sdk_reprobe)
    {
        return;
    }
    last_reprobe_ = now;
    Log("backend absent — re-arming the bounded SDK reconnect "
        "(client latched Disconnected after exhausting "
        "backoff)");
    sdk_->ReconnectNow();
}

void EngineHost::Run()
{
    /* frame_status rides a fast tick beside the ~500 ms
       maintenance loop — the UI preview repaints on that
       feed, so it cannot wait for PollOnce's cadence. The
       push rate stays bounded by frame_status_min_interval_ms;
       the tick only decides how soon a due push is noticed. */
    TimePoint next_maint = Clock::now();
    while(!stop_.load())
    {
        const TimePoint now = Clock::now();
        if(now >= next_maint)
        {
            PollOnce();
            next_maint = now + std::chrono::milliseconds(500);
        }
        if(service_)
        {
            service_->PollFrameStatus();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
}

/*---------------------------------------------------------*\
|| Ordered, bounded shutdown                                ||
\*---------------------------------------------------------*/
const OrderlyShutdown::Result& EngineHost::Stop()
{
    if(stopped_)
    {
        return shutdown_result_;
    }
    stopped_ = true;
    if(!started_)
    {
        return shutdown_result_;
    }

    /* Event intake and command intake stop first, then frame
       production, then the delivery lanes (each join bounded
       by the transport's send timeout), then the transport
       itself. Steps run in registration order and each is
       measured against its declared bound — an overrun is
       reported honestly, never silently. */
    OrderlyShutdown shutdown;
    shutdown.Add("power-monitor",
                 std::chrono::milliseconds(2000),
                 [this] { power_monitor_.Stop(); });
    shutdown.Add("ipc-listeners",
                 std::chrono::milliseconds(2000),
                 [this]
                 {
                     if(cmd_listener_)  cmd_listener_->Stop();
                     if(diag_listener_) diag_listener_->Stop();
                 });
    shutdown.Add("ipc-server",
                 std::chrono::milliseconds(2000),
                 [this]
                 { if(ipc_server_) ipc_server_->Stop(); });
    shutdown.Add("scene-worker",
                 std::chrono::milliseconds(2000),
                 [this] { engine_.Stop(); });
    /* Reactive capture sources (Task 4.6): the worker stopped
       first, so sources only feed a dead bus — each source's
       join is bounded by its own thread's wake latency
       (audio ~5 ms chunks, screen ~40-50 ms, key hook's
       WM_QUIT wait bounded by its publish loop). */
    shutdown.Add("reactive-inputs",
                 std::chrono::milliseconds(3000),
                 [this] { if(reactive_) reactive_->Shutdown(); });
    /* One in-flight transport send per lane plus join time —
       bounded by the transport's own send timeout (4 s
       default); lanes join sequentially. */
    shutdown.Add("delivery-pipeline",
                 std::chrono::milliseconds(12000),
                 [this] { pipeline_->Stop(); });
    shutdown.Add("sdk-transport",
                 std::chrono::milliseconds(1000),
                 [this] { if(sdk_) sdk_->Disconnect(); });

    shutdown_result_ = shutdown.Run();
    return shutdown_result_;
}

} /* namespace openrgb2 */
