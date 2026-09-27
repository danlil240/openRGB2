/*---------------------------------------------------------*\
|| EngineService.h                                           ||
||                                                           ||
||   IIpcService over the live engine: the verb surface    ||
||   the Milestone-4 UI consumes (docs/ipc-protocol.md).   ||
||                                                           ||
||   Covered surfaces (plan sections 4 and 5):             ||
||   - device snapshots        (GET_SNAPSHOTS/topology)     ||
||   - scene get/set           (GET_SCENE/SET_SCENE)        ||
||   - playback mode + status  (GET_/SET_PLAYBACK_MODE)     ||
||   - delivery stats          (GET_DELIVERY_STATS)         ||
||   - presets                 (GET_PRESETS)                ||
||   - layout/calibration      (LAYOUT_COMMAND — Task 4.4: ||
||     identify drives the mapper's bounded blink overlay;||
||     the scene-mutation verbs apply through the shared    ||
||     scene-apply path)                                    ||
||   - engine/connection state (GET_STATE)                  ||
||   - diagnostics             (DIAG_GET — also the only    ||
||     verb on the read-only diagnostics channel)           ||
||   - push events             (SUBSCRIBE kinds: topology,  ||
||     scene, playback, connection, frame_status)           ||
||                                                           ||
||   Scene ownership lives here so the --scene CLI path     ||
||   and SET_SCENE share ONE apply path: parse -> resolve   ||
||   layers (inline stack or named preset through the       ||
||   registry) -> SubmitScene + re-bind the mapper.         ||
||                                                           ||
||   SPDX-License-Identifier: GPL-2.0-or-later               ||
\*---------------------------------------------------------*/

#pragma once

#include "IpcServer.h"
#include "IpcService.h"

#include "DeliveryPipeline.h"
#include "DeviceMapper.h"
#include "Engine.h"
#include "OpenRgbClient.h"
#include "PlaybackController.h"
#include "PowerLifecycle.h"
#include "ReactiveInputs.h"

#include "effects/Presets.h"
#include "scene/SceneTypes.h"

#include <functional>
#include <memory>
#include <mutex>
#include <string>

namespace openrgb2
{
namespace ipc
{

struct EngineServiceOptions
{
    /* Minimum interval between frame_status pushes — the
       push channel is a status feed, not a per-frame fire
       hose. */
    uint32_t frame_status_min_interval_ms = 1000;
};

class EngineService final : public IIpcService
{
public:
    struct Deps
    {
        DeliveryPipeline*   pipeline = nullptr;   /* stats/queues */
        DeviceMapper*       mapper   = nullptr;   /* re-bind      */
        sdk::OpenRgbClient* sdk      = nullptr;   /* topology     */

        /* Topology snapshot source for the layout verbs
           (identify's controller resolution, set_binding's
           snapshot-derived identity). When set it wins over
           deps.sdk->Snapshots() — the same seam
           playback_caps provides, so offline tests drive the
           handlers without an SDK client. */
        std::function<std::vector<DeviceSnapshot>()> snapshots;

        /* Task 3.6 playback controller — when set, it is the
           authoritative playback state machine: SET_PLAYBACK_
           MODE runs the real transition (suppression, SDK
           select, revision acknowledgement), capabilities are
           classified from the applied scene, and scene
           applies restore the persisted selection. */
        PlaybackController* playback = nullptr;

        /* Playback capability source — fallback when no
           controller is wired (tests, bare engine): the
           default is live-effects only, so animation_playback
           requests get an honest Unavailable. */
        std::function<PlaybackCapabilities()> playback_caps;

        /* Task 3.7 power lifecycle — surfaced read-only in
           STATE/DIAGNOSTICS (display/suspend/gate flags). */
        const PowerLifecycle* power = nullptr;

        /* Task 4.6 reactive inputs — the REACTIVE_COMMAND
           verb space (set_enabled / set_params) and the
           read-only reactive surface in STATE/DIAGNOSTICS +
           the "reactive" event. Nullptr -> the surface
           reports available=false honestly rather than
           fabricating toggles. */
        ReactiveInputs*     reactive = nullptr;

        /* IPC-side queue/counter source (IpcServer::Stats) —
           diagnostics exposes queue depths, not guesses. */
        std::function<IpcServerStats()> server_stats;

        /* Task 3.8 offline bindings: before a scene applies,
           the persisted resolved-identity table completes the
           document's bindings. The hook may REPLACE `doc` with
           an augmented copy — a name-only binding inherits its
           recorded serial/location, so a missing device's
           binding is never reassigned to a same-named
           different device after restart. Empty -> no
           augmentation. */
        std::function<void(std::shared_ptr<const studio::SceneDocument>&)>
            prepare_scene;

        /* The "Quit All" hook (plan section 4): QUIT invokes this
           AFTER the reply is prepared; production wires it to the
           owner loop's stop flag so owned components shut down in
           order (OrderlyShutdown). Empty -> Unavailable. */
        std::function<void()> request_shutdown;
    };

    explicit EngineService(Engine& engine);
    EngineService(Engine& engine, Deps deps,
                  EngineServiceOptions options = EngineServiceOptions{});

    EngineService(const EngineService&)            = delete;
    EngineService& operator=(const EngineService&) = delete;

    /* Event fanout — wire to IpcServer::Emit. */
    void SetEventSink(std::function<void(const IpcEvent&)> sink);

    /* Post-construction wiring for deps_.server_stats (the
       server exists after the service). */
    void SetServerStatsProvider(
        std::function<IpcServerStats()> fn);

    /* The ONE scene-apply path shared by --scene and
       SET_SCENE: resolves the persisted effect stack (inline
       layers, else the named preset through the registry —
       unknown preset names are rejected), submits to the
       worker at the next revision, and re-binds the mapper.
       Emits the "scene" event. */
    bool     ApplySceneDocument(
            std::shared_ptr<const studio::SceneDocument> doc,
            std::string& err);
    uint64_t SceneRevision() const;
    std::shared_ptr<const studio::SceneDocument> Scene() const;

    /* Diff-driven event emission for the push channel: call
       periodically (~500 ms) from the engine's owner loop.
       Emits topology/connection/playback/reactive events on
       change and frame_status at a bounded rate when delivery
       stats moved. Single-caller: the diffing signatures are
       not mutex-guarded (same contract as SceneWorker::Step). */
    void PollEvents(TimePoint now = Clock::now());

    /* Emits the "reactive" event with the current reactive
       state — the ReactiveInputs manager's changed hook
       calls this (enable/disable, pause transitions). */
    void EmitReactive();

    /*---- IIpcService ---------------------------------------*/
    nlohmann::json OnWelcome() override;
    IpcReply       Dispatch(MsgType type,
                            const nlohmann::json& body) override;
    bool           SnapshotEvent(const std::string& kind,
                                 nlohmann::json& out) override;

    /* Read-only aggregate snapshot — the diagnostics channel's
       single verb and the Diagnostics event payload share
       this builder. `backend_counters` is null until the
       backend's own read-only pipe exists (later integration
       point — plan section 4; vendor/OpenRGB untouched). */
    nlohmann::json DiagnosticsJson(TimePoint now = Clock::now()) const;

private:
    Engine&             engine_;
    Deps                deps_;
    EngineServiceOptions options_;

    mutable std::mutex  mutex_;
    std::shared_ptr<const studio::SceneDocument> scene_;
    uint64_t            scene_revision_  = 0;
    PlaybackMode        requested_mode_  = PlaybackMode::LiveEffects;
    TimePoint           started_;

    std::function<void(const IpcEvent&)> event_sink_;

    /* PollEvents diffing state. */
    std::string         last_connection_sig_;
    std::string         last_topology_sig_;
    std::string         last_playback_sig_;
    std::string         last_reactive_sig_;
    std::string         last_stats_sig_;
    TimePoint           last_frame_status_{};

    void   Emit(const std::string& kind, const nlohmann::json& data);
    /* Task 4.4 LAYOUT_COMMAND surface (docs/ipc-protocol.md
       §5): identify drives the mapper's bounded blink
       overlay; the other verbs mutate the applied scene
       document through the ONE apply path
       (ApplySceneDocument — same validation, revision bump,
       and scene event as SET_SCENE). */
    IpcReply       HandleLayoutCommand(const std::string& verb,
                                       const nlohmann::json& body);
    /* Task 4.6 REACTIVE_COMMAND surface (docs/ipc-protocol.md):
       set_enabled / set_params drive the ReactiveInputs
       manager; every reply carries the full reactive body so
       the client lands on real state, never an assumed one. */
    IpcReply       HandleReactiveCommand(const std::string& verb,
                                         const nlohmann::json& body);
    IpcReply       HandleIdentify(const nlohmann::json& body);
    IpcReply       HandleLayoutMutate(const std::string& verb,
                                      const nlohmann::json& body);
    std::vector<DeviceSnapshot> LayoutSnapshots() const;
    nlohmann::json SceneJsonLocked() const;
    nlohmann::json PlaybackJson() const;
    nlohmann::json PowerJson() const;
    nlohmann::json StateJson() const;
    nlohmann::json ReactiveJson() const;
    nlohmann::json SnapshotsJson() const;
    nlohmann::json DeliveryStatsJson(const nlohmann::json& body) const;
    nlohmann::json ConnectionJson() const;
    nlohmann::json FrameStatusJson() const;
};

} /* namespace ipc */
} /* namespace openrgb2 */
