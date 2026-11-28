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
#include "WorkspaceCommands.h"
#include "WorkspaceTransfer.h"

#include "Automation.h"
#include "CalibrationSession.h"
#include "DeliveryPipeline.h"
#include "DeviceMapper.h"
#include "Engine.h"
#include "OpenRgbClient.h"
#include "OutputControl.h"
#include "OutputPolicy.h"
#include "PlaybackController.h"
#include "PowerLifecycle.h"
#include "ReactiveInputs.h"
#include "StudioScenePreparation.h"
#include "WorkspaceService.h"

#include "effects/Presets.h"
#include "scene/SceneTypes.h"

#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace openrgb2
{
namespace ipc
{

struct EngineServiceOptions
{
    /* Minimum interval between frame_status pushes — the
       push channel is a status feed, not a per-frame fire
       hose, but the UI preview repaints on this feed so it
       must stay fluid: ~30 Hz cap (~25 Hz delivered on the
       owner loop's 20 ms fast tick). */
    uint32_t frame_status_min_interval_ms = 33;

    /* studio-import Task 3: workspace transfer bounds — the
       defaults are the plan-§3.3 limits; the offline suite
       injects shorter expiries/smaller caps. Only consulted
       when Deps::workspace is wired. */
    WorkspaceTransfer::Options transfers;

    /* studio-import Task 7: the session-bound draft preview
       (plan 3.4). One PreviewSession per IPC session up to
       this bound; publishing capped at min_interval_ms and
       chunked below the 1 MiB body cap; above the display
       budget a reduced, clearly-labelled frame goes out. */
    struct PreviewOptions
    {
        size_t   max_sessions        = 2;
        uint32_t min_interval_ms     = 33;      /* ~30 Hz  */
        size_t   chunk_bytes         = 768u * 1024u;
        size_t   display_budget_leds = 4096;
    };
    PreviewOptions preview;

    /* Audit I-4: the serialized scene document rides inline in
       SCENE/STATE/WELCOME/DIAGNOSTICS bodies only up to this
       many bytes. Larger documents are announced by revision,
       name and document_sha256 (document:null +
       document_omitted:true) and served through a `scene`
       download transfer — a big scene can never push WELCOME
       past the 1 MiB body cap again. Only applies when the
       transfer surface exists (Deps::workspace wired); without
       it the document stays inline. 0 = always out of band. */
    size_t scene_inline_max_bytes = 256u * 1024u;
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

        /* studio-import Task 11: the engine-owned physical
           calibration session — the calibration_begin/show/
           end/status WORKSPACE verbs bind client session +
           opaque token to it. Nullptr -> those verbs answer
           Unavailable honestly. */
        CalibrationSession* calibration = nullptr;

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

        /* R-1: the runtime output control — the OUTPUT_COMMAND
           verb space (get / set_master / set_blackout /
           set_hold), the "output" event and the
           "output_control_v1" WELCOME capability. Nullptr ->
           Unavailable and no capability (clients hide the
           controls). */
        OutputControl*      output = nullptr;

        /* R-2: engine-side automation — the AUTOMATION_COMMAND
           verb space (get / set_hotkeys; R-4 set_dimming), the
           "automation" event, STATE "automation" and the
           "automation_v1" WELCOME capability. Nullptr ->
           Unavailable, STATE null, no capability. */
        Automation*         automation = nullptr;

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

        /* Last-scene persistence: invoked AFTER every
           successful apply of a document that carries objects
           — SET_SCENE, layout mutations (incl. add_object), and
           the --scene/restore startup path all funnel through
           ApplySceneDocument, so this is the one place a
           durable "last applied scene" can be captured. A
           document with NO objects never reaches the hook:
           applying an empty scene is a deliberate "new scene"
           staging step, not the thing to restore on the next
           launch. Runs on the service's background snapshot
           writer, coalesced latest-wins (audit I-6) — never on
           the request thread; FlushPersistedScene() and the
           destructor drain it. Empty -> no persistence. */
        std::function<void(const studio::SceneDocument&)>
            persist_scene;

        /* The "Quit All" hook (plan section 4): QUIT invokes this
           AFTER the reply is prepared; production wires it to the
           owner loop's stop flag so owned components shut down in
           order (OrderlyShutdown). `stop_backend` is the request's
           opt-in managed-backend handoff; the returned object is
           merged into the OK body so the hook can report its
           backend decision honestly ("closing" is a decision at
           request time, never a guarantee the process exited).
           Empty -> Unavailable. */
        std::function<nlohmann::json(bool stop_backend)>
            request_shutdown;

        /* Task 4.7: the Task-3.9 ownership report (competing-
           owner scan + managed-config sole-writer check)
           surfaced read-only in STATE/DIAGNOSTICS — the
           Devices & Diagnostics view's conflict list is the
           engine's report, never a UI-side guess. The provider
           returns the serialized OwnershipReport; empty -> the
           field is an honest null (no scan wired), never a
           fabricated "no conflicts". */
        std::function<nlohmann::json()> ownership;

        /* Audit E-12b: the exit/idle output policy, read-only
           in STATE ("output_policy": {on_exit, on_idle,
           per_controller, idle}) and summarized in the QUIT
           reply ("exit_policy": the global on_exit) so a client
           can say "lights will turn off". Empty -> STATE null,
           QUIT omits the field. */
        std::function<nlohmann::json()> output_policy;
        /* E-12b x R-1: OUTPUT_COMMAND set_policy persists through
           this (EngineHost::SetOutputPolicy: false + err when the
           profile save fails, state unchanged). The service
           validates the body with OutputPolicyFromJson first.
           Empty -> set_policy is Unavailable. */
        std::function<bool(const OutputPolicy&, std::string& err)>
            set_output_policy;

        /* studio-import Task 3: the managed-workspace commit
           surface (engine side). When set, the WORKSPACE_COMMAND
           verb space is live, WELCOME advertises
           "studio_workspace_v1", session-bound transfers and
           jobs exist, and — once a workspace is managed —
           lossy legacy scene mutations (SET_SCENE, the layout
           edit verbs) are refused with bad_request +
           "workspace_required". Nullptr -> the verb space
           answers Unavailable honestly and no capability is
           advertised. */
        WorkspaceService* workspace = nullptr;
    };

    explicit EngineService(Engine& engine);
    EngineService(Engine& engine, Deps deps,
                  EngineServiceOptions options = EngineServiceOptions{});
    /* Clears the workspace job-event hook (it captures this)
       before member teardown. */
    ~EngineService() override;

    EngineService(const EngineService&)            = delete;
    EngineService& operator=(const EngineService&) = delete;

    /* Event fanout — wire to IpcServer::Emit. */
    void SetEventSink(std::function<void(const IpcEvent&)> sink);

    /* Optional: "does anyone want kind K?" — wire to
       IpcServer::HasSubscriber. When set, the ~30 Hz
       frame_status body is not even BUILT while no session
       subscribes to it (IpcServer already skips encoding).
       Unset = always build (previous behavior). */
    void SetEventInterest(
        std::function<bool(const std::string& kind)> interest);

    /* Targeted fanout for session-bound events (Task 7's
       "preview" feed): wire to IpcServer::EmitTo. The key is
       the per-kind coalescing slot — "status" or
       "frame/<seq>/<chunk>". */
    void SetSessionEventSink(
        std::function<void(uint64_t session_id,
                           const IpcEvent& ev,
                           const std::string& coalesce_key)> sink);

    /* Post-construction wiring for deps_.server_stats (the
       server exists after the service). */
    void SetServerStatsProvider(
        std::function<IpcServerStats()> fn);

    /* The ONE scene-apply path shared by --scene and
       SET_SCENE: resolves the persisted effect stack (inline
       layers, else the named preset through the effective
       registry — packaged looks plus the managed root's
       library/effects, so user-installed looks resolve even
       before a workspace is adopted; genuinely unknown preset
       names are still rejected), prepares the shared
       runtime scene (hardware-dependent matrix geometry + key
       positions), submits to the worker at the next revision,
       and re-binds the mapper. Emits the "scene" event. */
    bool     ApplySceneDocument(
            std::shared_ptr<const studio::SceneDocument> doc,
            std::string& err);

    /* Task 6 workspace apply: the committed candidate drives
       StudioScenePreparation — runtime layers resolve through
       the CANDIDATE's own effect-registry snapshot and take the
       persisted inputs.decay_pct ripple scaling; matrix_map
       objects expand against the LIVE snapshots. Emits the
       "scene" event. Unknown preset names stay non-fatal here
       (candidate parse already warned). */
    bool     ApplyWorkspaceCandidate(const WorkspaceCandidate& cand,
                                     std::string& err);
    uint64_t SceneRevision() const;
    std::shared_ptr<const studio::SceneDocument> Scene() const;

    /* Blocks until the last-scene snapshot writer has landed
       every document queued so far (audit I-6: persist_scene
       runs on a background writer, coalesced latest-wins, so
       an apply never waits on the durable write). The owner
       calls it at orderly shutdown; the destructor drains the
       same way. No-op without a persist_scene hook. */
    void     FlushPersistedScene();

    /* Diff-driven event emission for the push channel: call
       periodically (~500 ms) from the engine's owner loop.
       Emits topology/connection/playback/reactive events on
       change and frame_status at a bounded rate when delivery
       stats moved. Single-caller: the diffing signatures are
       not mutex-guarded (same contract as SceneWorker::Step). */
    void PollEvents(TimePoint now = Clock::now());

    /* The frame_status feed on its own tick. PollEvents calls
       this too, but the owner loop also calls it on a faster
       cadence so the UI preview — which repaints on
       frame_status — is not gated by the ~500 ms maintenance
       poll. Rate stays bounded by
       frame_status_min_interval_ms; same single-caller
       contract as PollEvents. */
    void PollFrameStatus(TimePoint now = Clock::now());

    /* The draft-preview publish tick (Task 7). PollEvents
       calls this too; the owner loop calls it on the same
       fast cadence as PollFrameStatus but unconditionally, so
       a client subscribed to "preview" and not to
       "frame_status" still gets the fast feed. Same
       single-caller contract as PollEvents. */
    void PollPreviews(TimePoint now = Clock::now());

    /* Emits the "reactive" event with the current reactive
       state — the ReactiveInputs manager's changed hook
       calls this (enable/disable, pause transitions). */
    void EmitReactive();

    /* Task 11: emits the session-targeted "calibration"
       event carrying the session's status — the
       CalibrationSession::Deps::changed hook calls this. */
    void EmitCalibrationStatus();

    /* R-10: emits the "ownership" event with the current
       OWNERSHIP body — the engine's competitor re-scan calls
       this when a writer appeared or cleared, so the UI sees
       a conflict on any page (and in the tray), not only
       while Diagnostics polls. */
    void EmitOwnership();
    /* R-1: emits the "output" event with the current OUTPUT
       body — the engine's OutputControl changed hook calls
       this after applying a change. */
    void EmitOutput();
    /* R-2: emits the "automation" event with the current
       AUTOMATION body — Automation's changed hook calls it. */
    void EmitAutomation();

    /*---- IIpcService ---------------------------------------*/
    nlohmann::json OnWelcome() override;
    IpcReply       Dispatch(MsgType type,
                            const nlohmann::json& body) override;
    /* Session-bound dispatch: WORKSPACE_COMMAND verbs bind
       jobs/transfers to the calling session. */
    IpcReply       DispatchSession(uint64_t session_id,
                                   MsgType type,
                                   const nlohmann::json& body) override;
    /* Session teardown: queued workspace jobs + staged
       transfers of the dead session are cancelled — and its
       draft preview released (plan 3.4). */
    void           OnSessionEnd(uint64_t session_id) override;
    bool           SnapshotEvent(const std::string& kind,
                                 nlohmann::json& out) override;
    /* Session-bound snapshot: the "preview" kind reports THIS
       session's preview state, not a global one. */
    bool           SnapshotEventFor(uint64_t session_id,
                                    const std::string& kind,
                                    nlohmann::json& out) override;

    /* Read-only aggregate snapshot — the diagnostics channel's
       single verb and the Diagnostics event payload share
       this builder. `backend_counters` carries the backend's
       own wireless-status document when the SDK extension
       answered (NET_PACKET_ID_LIANLI_WIRELESS_STATUS, the
       lianli-wireless fork): a wedged group surfaces as
       state "failed"/"foreign_master" + wedged:true instead
       of staying silent. Null when no SDK is attached;
       {"available":false,...} honestly reports a stock
       backend or a not-yet-fetched cache. */
    nlohmann::json DiagnosticsJson(TimePoint now = Clock::now()) const;

private:
    Engine&             engine_;
    Deps                deps_;
    EngineServiceOptions options_;

    /* studio-import Task 3: the WORKSPACE_COMMAND verb
       surface + session-bound bounded transfers — exists iff
       deps_.workspace is wired. */
    std::unique_ptr<WorkspaceCommands> ws_commands_;

    mutable std::mutex  mutex_;
    /* Serializes scene read-copy-modify-apply edits (layout
       verbs) and SET_SCENE replaces across sessions. */
    std::mutex          scene_edit_mutex_;
    std::shared_ptr<const studio::SceneDocument> scene_;
    uint64_t            scene_revision_  = 0;
    /* The applied document's serialized forms, built once per
       scene revision (audit I-4): the JSON inlined into small
       SCENE bodies, the bytes a `scene` download serves, and
       their SHA-256 (document_sha256). Under mutex_; built
       outside it. */
    struct SceneDocForms
    {
        uint64_t revision = 0;
        std::shared_ptr<const studio::SceneDocument> doc;
        std::shared_ptr<const nlohmann::json>        json;
        std::shared_ptr<const std::string>           bytes;
        std::string                                  sha256;
    };
    mutable std::shared_ptr<const SceneDocForms> scene_forms_;
    /* The current scene's forms (null when none is loaded). */
    std::shared_ptr<const SceneDocForms> SceneForms() const;
    PlaybackMode        requested_mode_  = PlaybackMode::LiveEffects;
    TimePoint           started_;

    std::function<void(const IpcEvent&)> event_sink_;
    std::function<bool(const std::string&)> event_interest_;
    /* Task 7 targeted feed — preview chunks/statuses go to
       the OWNING session only. */
    std::function<void(uint64_t, const IpcEvent&,
                       const std::string&)> session_sink_;

    /* Last-scene snapshot writer (audit I-6): a depth-1
       latest-wins slot drained by one background thread —
       a burst of applies lands only the newest document, and
       the request thread never runs the durable write. The
       thread starts with the first queued document. */
    std::mutex              persist_mutex_;
    std::condition_variable persist_cv_;
    std::shared_ptr<const studio::SceneDocument> persist_pending_;
    bool                    persist_busy_ = false;
    bool                    persist_stop_ = false;
    std::thread             persist_thread_;
    void   QueuePersist(std::shared_ptr<const studio::SceneDocument> doc);
    void   PersistMain();

    /* PollEvents diffing state. */
    std::string         last_connection_sig_;
    std::string         last_topology_sig_;
    std::string         last_playback_sig_;
    std::string         last_reactive_sig_;
    /* R-1: the "output" signature is written from IPC threads
       (the host's changed hook) AND the owner loop — guarded,
       and held across the emit so two writers can never push
       an older body after a newer one. */
    std::mutex          output_sig_mutex_;
    std::string         last_output_sig_;
    std::string         last_stats_sig_;
    TimePoint           last_frame_status_{};

    /* Bodies are taken by value and moved into the event —
       callers hand temporaries, so nothing is deep-copied on
       the way to the sink (audit I-7). coalesce_key partitions
       latest-wins inside the kind (audit I-8). */
    void   Emit(const std::string& kind, nlohmann::json data,
                const std::string& coalesce_key = std::string());
    void   EmitEvent(IpcEvent ev);
    /* Task 4.4 LAYOUT_COMMAND surface (docs/ipc-protocol.md
       §5): identify drives the mapper's bounded blink
       overlay; the other verbs mutate the applied scene
       document through the ONE apply path
       (ApplySceneDocument — same validation, revision bump,
       and scene event as SET_SCENE). */
    IpcReply       HandleLayoutCommand(const std::string& verb,
                                       const nlohmann::json& body);
    /* add_object: creates device objects + bindings from LIVE
       topology keys ({controllers:[<DeviceKey>...],
       position?}) — the scene-authoring bootstrap path. */
    IpcReply       HandleAddObjects(const nlohmann::json& body);
    /* Task 4.6 REACTIVE_COMMAND surface (docs/ipc-protocol.md):
       set_enabled / set_params drive the ReactiveInputs
       manager; every reply carries the full reactive body so
       the client lands on real state, never an assumed one. */
    IpcReply       HandleReactiveCommand(const std::string& verb,
                                         const nlohmann::json& body);
    /* R-1 OUTPUT_COMMAND surface: a verb table (OutputVerbs)
       dispatches to one handler per verb; every reply is the
       full OUTPUT body. Add a verb = add a table row. */
    using OutputVerbFn = IpcReply (EngineService::*)(const nlohmann::json&);
    struct OutputVerb
    {
        const char*  name;
        OutputVerbFn fn;
        /* false: the verb has its own dependency (the E-12b
           policy verbs) and works without the runtime output
           control wired. */
        bool         needs_output_control = true;
    };
    static const std::vector<OutputVerb>& OutputVerbs();
    IpcReply       HandleOutputCommand(const std::string& verb,
                                       const nlohmann::json& body);
    IpcReply       OutputGet(const nlohmann::json& body);
    IpcReply       OutputSetMaster(const nlohmann::json& body);
    IpcReply       OutputSetBlackout(const nlohmann::json& body);
    IpcReply       OutputSetHold(const nlohmann::json& body);
    /* E-12c: clears the timing histograms (scene tick + every
       lane's queue/send). Reply OK {reset:true}. */
    IpcReply       OutputResetStats(const nlohmann::json& body);
    /* E-12b: reply OK {output_policy} — not the OUTPUT body, so a
       client's OUTPUT store is never overwritten by a policy. */
    IpcReply       OutputGetPolicy(const nlohmann::json& body);
    IpcReply       OutputSetPolicy(const nlohmann::json& body);

    /* R-2 AUTOMATION_COMMAND surface — same table shape as
       OutputVerbs; every reply is the full AUTOMATION body. */
    using AutomationVerbFn =
        IpcReply (EngineService::*)(const nlohmann::json&);
    struct AutomationVerb
    {
        const char*      name;
        AutomationVerbFn fn;
    };
    static const std::vector<AutomationVerb>& AutomationVerbs();
    IpcReply       HandleAutomationCommand(const std::string& verb,
                                           const nlohmann::json& body);
    IpcReply       AutomationGet(const nlohmann::json& body);
    IpcReply       AutomationSetHotkeys(const nlohmann::json& body);
    IpcReply       AutomationSetDimming(const nlohmann::json& body);  /* R-4 */
    /* Null when no Automation is wired. */
    nlohmann::json AutomationJson() const;
    IpcReply       HandleIdentify(const nlohmann::json& body);
    /* studio-import Task 3 gate: HandleLayoutMutate refuses
       lossy legacy edits while a managed workspace is active
       (detail "workspace_required") and forwards to the impl. */
    IpcReply       HandleLayoutMutate(const std::string& verb,
                                      const nlohmann::json& body);
    IpcReply       HandleLayoutMutateImpl(const std::string& verb,
                                          const nlohmann::json& body);
    /* The shared tail of both apply paths: validation, the
       revision bump, worker submission, mapper re-bind, stored
       scene, playback restore, and the scene event — all keyed
       off the PREPARED runtime state. */
    bool           ApplyPreparedScene(const PreparedStudioScene& prepared,
                                      std::string& err);
    std::vector<DeviceSnapshot> LayoutSnapshots() const;
    /* studio-import Task 3: true once a managed workspace is
       active — the lossy legacy mutation gate (plan 3.3). */
    bool           WorkspaceManaged() const;
    nlohmann::json SceneJsonLocked() const;
    nlohmann::json PlaybackJson() const;
    nlohmann::json PowerJson() const;
    nlohmann::json StateJson() const;
    nlohmann::json ReactiveJson() const;
    /* R-1: the OUTPUT body (null when no OutputControl is
       wired). */
    nlohmann::json OutputJson() const;
    /* R-1: emit "output" when `force` or the body moved since
       the last emit — the host hook and PollEvents share one
       signature, so a change is pushed once, not twice. */
    void           EmitOutputIfChanged(bool force);
    nlohmann::json SnapshotsJson(bool include_colors = true) const;
    /* F12: DIAGNOSTICS and GET_DELIVERY_STATS share ONE
       topology capture per call — `snaps` is the caller's
       LayoutSnapshots() result, never a second SDK read. */
    nlohmann::json DeliveryStatsJson(
        const nlohmann::json& body,
        const std::vector<DeviceSnapshot>& snaps) const;
    nlohmann::json ConnectionJson() const;
    /* The delivered-state feed (`frame_status` event): every
       enumerated controller produces a row — never-delivered
       lanes report delivered:false + zeroed counters so
       "bound but never delivered" is visible (QA F04). */
    nlohmann::json FrameStatusJson(
        const std::vector<DeviceSnapshot>& snaps) const;
    /* The wireless lane's OWN counters (QA F11): the
       pipeline-wide aggregate mixes every wired lane, so a
       "wireless performance" surface must read the wireless
       lane alone. Null when no wireless lane exists — "not
       measured", never a wired number under a wireless
       label. */
    nlohmann::json WirelessLaneJson(
        const std::vector<DeviceSnapshot>& snaps,
        bool full_timing = true) const;
    /* E-12c: the scene clock's tick-interval histogram (STATE
       scene.tick_hist). */
    nlohmann::json TickHistJson() const;
    /* IPC server counters (IpcServer::Stats provider) —
       shared by STATE and DIAGNOSTICS so DIAG never re-runs
       the whole StateJson for one field (QA F12). */
    nlohmann::json IpcStatsJson() const;
    nlohmann::json OwnershipJson() const;
    nlohmann::json OutputPolicyJson() const;     /* E-12b */
    /* The backend's own read-only diagnostics pipe: the
       SDK client's cached LIANLI_WIRELESS_STATUS snapshot
       (state-lock read only — this never queues behind
       wire I/O, same discipline as ConnectionJson). */
    nlohmann::json BackendCountersJson() const;
};

} /* namespace ipc */
} /* namespace openrgb2 */
