/*---------------------------------------------------------*\
|| EngineHost.h                                              ||
||                                                           ||
||   The engine process composition (Task 3.9 — the       ||
||   end-to-end wiring the standalone openrgb2-engine      ||
||   executable runs). Everything main() used to own lives  ||
||   here so the SAME startup/shutdown path runs in the    ||
||   offline end-to-end test (fake backend) and in the     ||
||   shipped binary — no divergent second wiring.          ||
||                                                           ||
||   Startup order (plan section 4 process ownership):    ||
||     1. single-instance guard — one engine owns the      ||
||        managed configuration; a second refuses to        ||
||        double-own (never takes over, never kills)        ||
||     2. competing-owner scan — other OpenRGB instances,  ||
||        L-Connect, GCC/RGB Fusion, Synapse, G Hub, iCUE, ||
||        SignalRGB are REPORTED, never killed; FanControl  ||
||        is an expected notice                             ||
||     3. managed-config sole-writer check — the backend's ||
||        private config dir must not enable the legacy    ||
||        Studio/Effects/HardwareSync scene producers      ||
||     4. backend attach — SDK connect to the externally    ||
||        launched backend on 127.0.0.1:6744; attach vs     ||
||        nothing-listening vs foreign-port-owner are      ||
||        reported honestly                                 ||
||     5. playback / profile / power / scene / IPC wiring  ||
||        (Tasks 3.3-3.8), scene apply, worker + lanes     ||
||                                                           ||
||   Shutdown is the OrderlyShutdown sequence: power        ||
||   monitor -> IPC intake -> IPC server -> scene worker ->||
||   delivery lanes -> SDK transport, each bounded and     ||
||   measured.                                              ||
||                                                           ||
||   Seams for the offline suite: transport_factory        ||
||   injects the SDK byte stream (FakeStream), pipe names  ||
||   and the instance name are overridable, the power      ||
||   monitor can be disabled, and PollOnce() drives the    ||
||   owner loop deterministically.                          ||
||                                                           ||
||   SPDX-License-Identifier: GPL-2.0-or-later               ||
\*---------------------------------------------------------*/

#pragma once

#include "DeliveryPipeline.h"
#include "DeviceMapper.h"
#include "Engine.h"
#include "OrderlyShutdown.h"
#include "OwnershipScan.h"
#include "PlaybackController.h"
#include "PowerLifecycle.h"
#include "ProfileStore.h"
#include "ReactiveInputs.h"
#include "SingleInstance.h"

#include "EngineService.h"
#include "IpcServer.h"
#include "NamedPipe.h"

#include "OpenRgbClient.h"
#include "Transport.h"

#include "WinPowerMonitor.h"

#include <atomic>
#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace openrgb2
{

struct EngineHostOptions
{
    /* Inputs. */
    std::string scene_path;
    std::string effects_dir;
    std::string devices_dir;
    std::string state_path = "profiles/local/engine-state.json";

    /* Backend SDK endpoint (empty sdk_enabled = null sink).
       transport_factory builds the byte stream — production
       uses TcpSocket; tests inject an in-process fake. */
    bool        sdk_enabled = false;
    std::string sdk_host    = "127.0.0.1";
    uint16_t    sdk_port    = 6744;
    std::function<std::unique_ptr<sdk::IStreamTransport>()>
        transport_factory;

    /* Task 5.3 reconnect policy. The client's bounded backoff
       latches to Disconnected after max_attempts (a longer
       backend outage would otherwise leave the engine
       permanently detached — nothing else re-arms it). The
       owner loop re-arms with a ReconnectNow() no more often
       than sdk_reprobe — retries stay bounded while a
       backend that returns late still re-attaches. Both are
       injectable so the offline suite can exhaust and
       re-probe in milliseconds. */
    sdk::ReconnectBackoff   sdk_backoff;
    std::chrono::milliseconds sdk_reprobe{ 30000 };

    /* Task 5.3: periodic topology revalidation. SDK <= v4 has
       no async device events, so a controller vanishing
       mid-session is only detected by re-fetching — without
       this, stale positional dev-index mappings misroute
       frames to a shifted controller until the transport
       next breaks. RefreshStatus keeps the generation on
       data-only change and bumps it on structural change
       (unplug/replug), which obsoletes stale in-flight
       frames and resyncs the lanes. */
    std::chrono::milliseconds sdk_topology_poll{ 2000 };

    /* IPC surface (docs/ipc-protocol.md). Pipe names are
       overridable so a test never touches the well-known
       names a real engine may own. */
    bool        ipc_enabled           = true;
    std::string command_pipe_name     = ipc::kCommandPipeName;
    std::string diagnostics_pipe_name = ipc::kDiagnosticsPipeName;

    /* Task 3.9 ownership (plan section 4). */
    bool        single_instance = true;
    std::string instance_name   = kEngineInstanceName;
    bool        scan_owners     = true;
    std::string backend_config_dir;      /* managed backend
                                            private config dir;
                                            empty = skip the
                                            sole-writer check  */
    bool        power_monitor   = true;

    /* Task 4.6 reactive inputs (audio/screen/key). True wires
       the ReactiveInputs manager against the platform
       sources; the manager itself is cheap — capture only
       starts when an input is enabled. reactive_factory
       overrides the platform sources for the offline suite
       (tests never capture a real desktop). */
    bool        reactive_inputs = true;
    std::function<std::unique_ptr<IReactiveSource>(
        const std::string& id)> reactive_factory;

    /* Log sink (default: stderr with the engine prefix). */
    std::function<void(const std::string&)> log;
};

/* What the startup ownership checks found — kept for the
   report and the offline suite. */
struct OwnershipReport
{
    bool                     scanned           = false;
    std::vector<OwnerFinding> findings;
    bool                     backend_listening = false;
    uint32_t                 backend_pid       = 0;
    bool                     backend_attached  = false;
    bool                     config_checked    = false;
    ManagedConfigReport      config;
};

class EngineHost
{
public:
    explicit EngineHost(EngineHostOptions options);
    ~EngineHost();                              /* Stop() if started */

    EngineHost(const EngineHost&)            = delete;
    EngineHost& operator=(const EngineHost&) = delete;

    /* Full startup sequence above. False + err on the fatal
       cases: instance already owned, scene load/apply failure,
       worker start failure. Ownership findings and a missing
       backend are reported through the log, never fatal. */
    bool Start(std::string& err);

    /* One owner-loop iteration (the former ~500 ms tick):
       IPC event diffing, SDK poll + lane sync, power-lifecycle
       poll, playback maintenance, mapper sync, offline-binding
       persistence, per-controller custom-mode upkeep. */
    void PollOnce();

    /* The owner loop: PollOnce every ~500 ms until
       RequestStop() (SIGINT/SIGTERM/QUIT). Returns when the
       flag is set — call Stop() for the ordered teardown. */
    void Run();

    /* Ordered, bounded teardown (idempotent). The result is
       kept for inspection (offline suite, exit report). */
    const OrderlyShutdown::Result& Stop();

    void RequestStop();
    bool StopRequested() const;
    bool Started() const;

    /*---- Introspection (diagnostics/tests) ------------------*/
    Engine&                  GetEngine()     { return engine_; }
    PlaybackController*      GetPlayback()   { return playback_.get(); }
    DeviceMapper&            GetMapper()     { return mapper_; }
    DeliveryPipeline*        GetPipeline()   { return pipeline_.get(); }
    ipc::EngineService*      GetService()    { return service_.get(); }
    sdk::OpenRgbClient*      GetSdk()        { return sdk_.get(); }
    PowerLifecycle*          GetLifecycle()  { return lifecycle_.get(); }
    ReactiveInputs*          GetReactive()   { return reactive_.get(); }
    const OwnershipReport&   Ownership() const { return ownership_; }
    /* Task 4.7: the wire form of the ownership report —
       serialized for STATE/DIAGNOSTICS (docs/ipc-protocol.md
       §5). Conflicts and notices are the scan's findings
       verbatim; "scanned:false" is the honest surface when
       the scan never ran. */
    nlohmann::json           OwnershipJson() const;
    uint64_t                 FramesProduced() const;

    void Log(const std::string& line) const;

private:
    PlaybackController::Deps   BuildPlaybackDeps();
    PowerLifecycle::Deps       BuildLifecycleDeps();
    bool StartIpc();
    bool ApplyStartupScene(std::string& err);
    void ScanOwners();
    /* ReactiveInputs changed hook: emit the IPC event +
       persist the selection through the profile store. */
    void OnReactiveChanged();

    /* Task 5.3: re-arms the SDK reconnect schedule after the
       client's bounded backoff latches to Disconnected. */
    void ReprobeSdk(TimePoint now);

    EngineHostOptions        opts_;
    /* Built in Start() with opts_.instance_name — the option is
       a real seam: tests take a test-scoped guard name so a
       production engine holding the well-known name never makes
       the offline suite collide. */
    std::unique_ptr<SingleInstanceGuard> instance_;
    OwnershipReport          ownership_;
    std::atomic<bool>        stop_{ false };

    Engine                                   engine_;
    std::shared_ptr<DeliveryPipeline>        pipeline_;
    DeviceMapper                             mapper_;
    std::shared_ptr<sdk::OpenRgbClient>      sdk_;
    std::unique_ptr<PlaybackController>      playback_;
    std::unique_ptr<ProfileStore>            profile_;
    EngineState                              engine_state_;
    std::mutex                               state_mutex_;
    std::unique_ptr<PowerLifecycle>          lifecycle_;
    std::unique_ptr<ReactiveInputs>          reactive_;
    platform::WinPowerMonitor                power_monitor_;
    std::unique_ptr<ipc::EngineService>      service_;
    std::unique_ptr<ipc::IpcServer>          ipc_server_;
    std::unique_ptr<ipc::NamedPipeListener>  cmd_listener_;
    std::unique_ptr<ipc::NamedPipeListener>  diag_listener_;

    std::atomic<uint64_t>                    frames_{ 0 };
    std::atomic<bool>                        capture_paused_{ false };
    TimePoint                                last_reprobe_{};
    TimePoint                                last_topology_poll_{};
    std::map<DeviceKey, uint64_t>            custom_mode_done_;
    bool                                     started_ = false;
    OrderlyShutdown::Result                  shutdown_result_;
    bool                                     stopped_ = false;
};

} /* namespace openrgb2 */
