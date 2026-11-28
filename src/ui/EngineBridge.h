/*---------------------------------------------------------*\
||| EngineBridge.h                                            ||
|||                                                           ||
|||   QObject bridge between QML and the engine IPC client  ||
|||   (EngineWireClient — docs/ipc-protocol.md). Owns a     ||
|||   worker thread that connects to the restricted         ||
|||   command pipe, handshakes, subscribes to every event   ||
|||   kind, and pumps replies/EVENT pushes into QVariant    ||
|||   stores exposed as Q_PROPERTYs. Reconnects with a      ||
|||   bounded backoff schedule; every state — unreachable,  ||
|||   connecting, connected, degraded — is surfaced         ||
|||   honestly (plan section 5).                            ||
|||                                                           ||
|||   UI is a CLIENT: nothing here touches hardware, the    ||
|||   SDK, or the backend — pipe traffic only.              ||
|||                                                           ||
|||   SPDX-License-Identifier: GPL-2.0-or-later               ||
|\*---------------------------------------------------------*/

#pragma once

#include "EngineWireClient.h"
#include "FrameFeedModel.h"
#include "NamedPipe.h"

#include <QObject>
#include <QString>
#include <QVariantList>
#include <QVariantMap>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace openrgb2
{
namespace ui
{

class EngineBridge : public QObject
{
    Q_OBJECT

    /* "disconnected" | "connecting" | "unreachable" | "connected" */
    Q_PROPERTY(QString connectionState READ connectionState
                   NOTIFY connectionStateChanged)
    Q_PROPERTY(bool connected READ connected
                   NOTIFY connectionStateChanged)
    /* Connected but the engine reports an unhealthy link
       (e.g. SDK disconnected, engine not running). */
    Q_PROPERTY(bool degraded READ degraded NOTIFY degradedChanged)
    Q_PROPERTY(QString lastError READ lastError NOTIFY lastErrorChanged)
    Q_PROPERTY(QString pipeName READ pipeName WRITE setPipeName)
    /* ms until the next automatic retry while unreachable */
    Q_PROPERTY(int retryInMs READ retryInMs NOTIFY retryInMsChanged)

    Q_PROPERTY(QVariantMap serverInfo READ serverInfo
                   NOTIFY serverInfoChanged)
    Q_PROPERTY(QVariantList snapshots READ snapshots
                   NOTIFY snapshotsChanged)
    Q_PROPERTY(QVariantMap connection READ connection
                   NOTIFY connectionChanged)
    Q_PROPERTY(QVariantMap scene READ scene NOTIFY sceneChanged)
    Q_PROPERTY(QVariantMap playback READ playback
                   NOTIFY playbackChanged)
    /* Derived playback view (Task 4.3) — the honest
       applied-vs-requested state the pages bind to, composed
       once here from the playback + connection stores so
       every view reports the SAME truth:
       - requested_mode is the recorded UI selection
       - applied_mode is status.mode, which only moves on
         backend acknowledgement — "applied" is never
         reported before the wire confirms it
       - pending stays true while an ack is in flight OR the
         two modes disagree
       - phase is the single display token (reconnecting /
         applying / failed / uploading / active / live /
         unknown)
       - cadence/sync/physical fields are pass-throughs of
         what PLAYBACK reports — nothing physical is invented */
    Q_PROPERTY(QVariantMap playbackView READ playbackView
                   NOTIFY playbackViewChanged)
    /* The whole last frame_status body. C++ consumers and tests
       read it; QML should bind to the split views below — every
       QML read of this map converts ALL controller colors to JS,
       and it changes at up to ~30 Hz. */
    Q_PROPERTY(QVariantMap frameStatus READ frameStatus
                   NOTIFY frameStatusChanged)
    /* frame_status.controllers[] as a stable-row list model
       (FrameFeedModel): a push updates rows in place, so preview
       Repeaters keep their delegates. */
    Q_PROPERTY(QObject* frameControllers READ frameControllers
                   CONSTANT)
    /* frame_status.aggregate / .wireless — notify only when the
       sub-object actually changed. */
    Q_PROPERTY(QVariantMap frameAggregate READ frameAggregate
                   NOTIFY frameAggregateChanged)
    Q_PROPERTY(QVariantMap frameWireless READ frameWireless
                   NOTIFY frameWirelessChanged)
    /* false while nothing can show the feed (window hidden to the
       tray or minimized): frame_status pushes are dropped on the
       worker thread before they reach the GUI thread. The stores
       keep their last (stale) values; the next push after
       re-activation refreshes them. Default true. */
    Q_PROPERTY(bool frameFeedActive READ frameFeedActive
                   WRITE setFrameFeedActive
                   NOTIFY frameFeedActiveChanged)
    Q_PROPERTY(QVariantMap deliveryStats READ deliveryStats
                   NOTIFY deliveryStatsChanged)
    Q_PROPERTY(QVariantList presets READ presets
                   NOTIFY presetsChanged)
    Q_PROPERTY(QVariantMap diagnostics READ diagnostics
                   NOTIFY diagnosticsChanged)
    Q_PROPERTY(QVariantMap stateReport READ stateReport
                   NOTIFY stateReportChanged)
    /* Task 4.6 reactive-input surface — the REACTIVE_COMMAND
       body's read view: {available, capture_paused, inputs:
       [{id,available,enabled,running,paused,sensitivity,
       screen_index,status}], displays:[{index,name,width,
       height}]}. available=false carries the engine's honest
       reason — never a fabricated toggle set. */
    Q_PROPERTY(QVariantMap reactive READ reactive
                   NOTIFY reactiveChanged)
    /* R-10: the engine's OWNERSHIP report (competing RGB
       writers), freshest-wins across WELCOME state, the STATE
       and DIAGNOSTICS replies and the pushed "ownership"
       event — so the shell can surface a conflict on any page
       without polling. Empty = no report published yet. */
    Q_PROPERTY(QVariantMap ownership READ ownership
                   NOTIFY ownershipChanged)
    /* R-9: the engine's backend-supervisor status (STATE
       "supervisor" and every "supervisor" event's status) —
       {enabled, backend_restarts, wedge_restarts, last_reason,
       last_at, budget_exhausted, seq, history}. Empty = none
       published. Live actions also arrive as supervisorAction. */
    Q_PROPERTY(QVariantMap supervisor READ supervisor
                   NOTIFY supervisorChanged)
    /* R-1: the engine's runtime output control (OUTPUT body:
       {master, blackout, hold, effective, contributors,
       uploaded_unscaled}) — freshest-wins across WELCOME state,
       STATE replies, OUTPUT replies and the "output" event.
       Engine-side state, never a document field: nothing here
       dirties a draft. Empty = not published / no capability. */
    Q_PROPERTY(QVariantMap output READ output NOTIFY outputChanged)
    /* True exactly when WELCOME advertised "output_control_v1";
       the tray hides its quick controls otherwise. */
    Q_PROPERTY(bool hasOutputControl READ hasOutputControl
                   NOTIFY hasOutputControlChanged)
    /* R-2: engine-side automation (AUTOMATION body: {hotkeys:
       [{action, chord, state, detail?}], actions, proposed,
       cycle, brightness_step, registrar}) — WELCOME state,
       STATE replies, AUTOMATION replies and the "automation"
       event. Empty = not published / no capability. */
    Q_PROPERTY(QVariantMap automation READ automation
                   NOTIFY automationChanged)
    /* True exactly when WELCOME advertised "automation_v1". */
    Q_PROPERTY(bool hasAutomation READ hasAutomation
                   NOTIFY hasAutomationChanged)
    /* studio-import Task 3: true exactly when WELCOME advertised
       "studio_workspace_v1". A false bridge falls back to the
       legacy scene verbs — workspaceCommand still sends (the
       engine answers Unavailable honestly), but pages bind
       their workspace UI to this flag. */
    Q_PROPERTY(bool hasWorkspace READ hasWorkspace
                   NOTIFY hasWorkspaceChanged)
    /* Last "workspace" event payload — the get reply's managed
       metadata for the initial snapshot, the job status record
       for completions. */
    Q_PROPERTY(QVariantMap workspace READ workspace
                   NOTIFY workspaceChanged)

public:
    explicit EngineBridge(QObject* parent = nullptr);
    ~EngineBridge() override;

    /*---- lifecycle ------------------------------------------*/
    Q_INVOKABLE void start();          /* begin auto-connect    */
    Q_INVOKABLE void stop();           /* disconnect; no retry  */
    Q_INVOKABLE void reconnectNow();   /* drop + retry at once  */

    /*---- requests ----------------------------------------------
       Each returns the request correlator (>0) echoed in
       replyReceived, or 0 when nothing was sent (offline,
       over-bound body). Replies also land in the matching store. */
    Q_INVOKABLE int getSnapshots();
    Q_INVOKABLE int getScene();
    Q_INVOKABLE int setScene(const QVariantMap& document);
    Q_INVOKABLE int getPlayback();
    Q_INVOKABLE int setPlaybackMode(const QString& mode,
                                    const QString& control,
                                    const QVariantMap& params);
    Q_INVOKABLE int getDeliveryStats();
    Q_INVOKABLE int getState();
    Q_INVOKABLE int getPresets();
    Q_INVOKABLE int getDiagnostics();   /* DIAG_GET (command pipe) */
    Q_INVOKABLE int ping();
    Q_INVOKABLE int resubscribe();
    Q_INVOKABLE int unsubscribe();
    Q_INVOKABLE int layoutCommand(const QString& verb,
                                  const QVariantMap& args);
    /* REACTIVE_COMMAND {verb,...args} — "set_enabled"
       {input,enabled} / "set_params" {input,sensitivity?|
       screen_index}. The engine replies with the full
       reactive body; the "reactive" event updates the store. */
    Q_INVOKABLE int reactiveCommand(const QString& verb,
                                    const QVariantMap& args);
    /* R-1 OUTPUT_COMMAND {verb,...args} — "get", "set_master"
       {value}, "set_blackout" {on}, "set_hold" {on}. The OUTPUT
       reply lands in the output store. Returns 0 (nothing
       sent) when the engine did not advertise
       output_control_v1. */
    Q_INVOKABLE int outputCommand(const QString& verb,
                                  const QVariantMap& args);
    /* R-2 AUTOMATION_COMMAND {verb,...args} — "get",
       "set_hotkeys" {bindings:[{action, chord}], cycle?,
       brightness_step?}. The AUTOMATION reply lands in the
       automation store. Returns 0 (nothing sent) when the
       engine did not advertise automation_v1. */
    Q_INVOKABLE int automationCommand(const QString& verb,
                                      const QVariantMap& args);
    /* WORKSPACE_COMMAND {verb,...args} — section 3.3 verbs
       (get/list/catalog/validate/commit/save_copy, the
       transfer_* trio, job_status/job_cancel, preview_*). The
       reply lands in replyReceived with the request id; terminal
       job results also arrive as "workspace" EVENTs. */
    Q_INVOKABLE int workspaceCommand(const QString& verb,
                                     const QVariantMap& args);
    /* C++-only twin taking a ready JSON object (non-objects are
       replaced by {}) — skips the QVariant round trip for large
       bodies such as the draft-preview document. */
    int workspaceCommandJson(const QString& verb,
                             nlohmann::json args);
    Q_INVOKABLE int quitEngine(bool stopBackend);

    /*---- state ------------------------------------------------*/
    QString     connectionState() const;
    bool        connected() const;
    bool        degraded() const;
    QString     lastError() const;
    QString     pipeName() const;
    int         retryInMs() const;
    QVariantMap serverInfo() const;
    QVariantList snapshots() const;
    QVariantMap connection() const;
    QVariantMap scene() const;
    QVariantMap playback() const;
    QVariantMap playbackView() const;
    QVariantMap frameStatus() const;
    QObject*    frameControllers() const;
    QVariantMap frameAggregate() const;
    QVariantMap frameWireless() const;
    bool        frameFeedActive() const;
    void        setFrameFeedActive(bool active);
    QVariantMap deliveryStats() const;
    QVariantList presets() const;
    QVariantMap diagnostics() const;
    QVariantMap stateReport() const;
    QVariantMap reactive() const;
    QVariantMap ownership() const;
    QVariantMap supervisor() const;
    QVariantMap output() const;
    bool        hasOutputControl() const;
    QVariantMap automation() const;
    bool        hasAutomation() const;
    bool        hasWorkspace() const;
    QVariantMap workspace() const;

    /*---- test hooks (call before start()) ----------------------*/
    using ChannelFactory =
        std::function<std::unique_ptr<ipc::IIpcChannel>(
            std::string& err)>;
    void setChannelFactoryForTesting(ChannelFactory f);
    void setWireOptionsForTesting(const EngineWireClient::Options& o);
    void setBackoffForTesting(const std::vector<int>& delays_ms);
    int  connectAttemptsForTesting() const;
    /* Feed a reactive-state body the way a "reactive" event
       does (offline tests can't drive the engine's inputs). */
    void setReactiveForTesting(const QVariantMap& m)
        { reactive_ = m; emit reactiveChanged(); }
    /* R-10: feed an OWNERSHIP body the way the "ownership"
       event does. */
    void setOwnershipForTesting(const QVariantMap& m)
        { AdoptOwnership(m); }
    /* R-9: feed a supervisor status the way STATE does. */
    void setSupervisorForTesting(const QVariantMap& m)
        { AdoptSupervisor(m); }
    /* R-1: feed an OUTPUT body the way the "output" event does;
       setOutputControlForTesting fakes the WELCOME capability
       (offline tray tests). */
    void setOutputForTesting(const QVariantMap& m)
        { AdoptOutput(m); }
    void setOutputControlForTesting(bool on)
        { setHasOutputControl(on); }
    /* R-2: same pair for the automation store. */
    void setAutomationForTesting(const QVariantMap& m)
        { automation_ = m; emit automationChanged(); }
    void setAutomationCapabilityForTesting(bool on)
        { setHasAutomation(on); }

    void setPipeName(const QString& name);

signals:
    void connectionStateChanged();
    void degradedChanged();
    void lastErrorChanged();
    void retryInMsChanged();
    void serverInfoChanged();
    void snapshotsChanged();
    void connectionChanged();
    void sceneChanged();
    void playbackChanged();
    void playbackViewChanged();
    void frameStatusChanged();
    void frameAggregateChanged();
    void frameWirelessChanged();
    void frameFeedActiveChanged();
    void deliveryStatsChanged();
    void presetsChanged();
    void diagnosticsChanged();
    void stateReportChanged();
    void reactiveChanged();
    void ownershipChanged();
    void supervisorChanged();
    /* R-9: one live supervisor action ({action, reason, detail,
       at, seq}) — never emitted for a SUBSCRIBE snapshot. */
    void supervisorAction(const QVariantMap& event);
    /* quitEngine() was called: the engine exit that follows is
       intended (the UI's engine supervisor must not restart). */
    void quitRequested();
    void outputChanged();
    void hasOutputControlChanged();
    void automationChanged();
    void hasAutomationChanged();
    void hasWorkspaceChanged();
    void workspaceChanged();

    /* requestId echoes SendRequest's correlator; typeName is the
       wire type (MsgTypeName — e.g. "PONG", "SNAPSHOTS", "ERROR"). */
    void replyReceived(int requestId, const QString& typeName,
                       const QVariantMap& body);
    void eventReceived(const QString& kind, const QVariantMap& data);
    void protocolError(const QString& detail);
    void engineLost(const QString& reason);

private:
    /*---- worker ------------------------------------------------*/
    void WorkerMain();
    bool BackoffWait();   /* false → stop requested */
    bool Running() const;

    template <typename F> void Post(F&& f);
    int  Issue(ipc::MsgType type, const nlohmann::json& body);

    /*---- GUI-thread application of wire callbacks -------------*/
    void ApplyWelcome(const nlohmann::json& welcome);
    void ApplyEvent(const nlohmann::json& event_body);
    /* GUI side of the frame_status slot: take the newest one
       and apply it (audit U-10). */
    void ApplyPendingFrameStatus();
    void ApplyReply(uint32_t request_id, ipc::MsgType type,
                    const nlohmann::json& body);
    void ApplySessionEnd(EngineWireClient::EndReason reason,
                         const std::string& detail);
    void RecomputeDegraded();
    /* R-10: store an OWNERSHIP body when it is an object
       (null = nothing wired — keep what we have). */
    void AdoptOwnership(const QVariantMap& m);
    /* R-9: store a supervisor status (empty = keep). */
    void AdoptSupervisor(const QVariantMap& m);
    /* R-1: store an OUTPUT body (empty = keep what we have). */
    void AdoptOutput(const QVariantMap& m);
    void setHasOutputControl(bool h);
    /* R-2: store an AUTOMATION body (empty = keep). */
    void AdoptAutomation(const QVariantMap& m);
    void setHasAutomation(bool h);

    /* Audit I-4: a SCENE body (WELCOME state, scene EVENT,
       GET_SCENE reply) may announce a large document by
       document_sha256 instead of inlining it
       (document_omitted). The bridge completes it through a
       `scene` download transfer before publishing, so scene()
       keeps its shape for every consumer: a loaded scene
       carries its document. A body whose hash matches the
       held document reuses it without a fetch. */
    void AdoptScene(const nlohmann::json& body);
    void StartSceneFetch();
    bool HandleSceneFetchReply(uint32_t request_id, ipc::MsgType type,
                               const nlohmann::json& body);
    void FinishSceneFetch(const nlohmann::json* doc,
                          const std::string& sha,
                          const std::string& error);

    void setConnectionState(const QString& s);
    void setLastError(const QString& s);
    void setDegraded(bool d);
    void setRetryInMs(int ms);
    void setHasWorkspace(bool h);

    EngineWireClient          wire_;
    EngineWireClient::Options wire_options_;
    ChannelFactory            factory_;
    QString                   pipe_name_;
    std::vector<int>          backoff_ms_;

    std::thread               worker_;
    mutable std::mutex        mu_;
    std::condition_variable   wake_cv_;
    /* Atomic so the worker can pass it to the wire session as the
       external cancellation flag: a stop() that lands between the
       factory returning a channel and wire_.Run() starting still
       ends the run before HELLO — never a session the owner
       already asked to die. */
    std::atomic<bool>         running_{ false };
    bool                      reconnect_req_ = false;
    size_t                    fail_count_    = 0;
    std::atomic<int>          attempts_{ 0 };

    QString     connection_state_ = "disconnected";
    bool        degraded_    = false;
    QString     last_error_;
    int         retry_in_ms_ = 0;
    QVariantMap server_info_;
    QVariantList snapshots_;
    QVariantMap connection_;
    QVariantMap scene_;
    /* Audit I-4 fetch state (GUI thread). */
    QString        scene_doc_sha_;     /* sha of scene_'s document  */
    nlohmann::json scene_pending_;     /* newest body awaiting its
                                          document (null: none)      */
    struct SceneFetch
    {
        int         stage    = 0;      /* 0 idle, 1 begin, 2 chunk  */
        uint32_t    req      = 0;
        std::string transfer_id;
        uint64_t    length   = 0;
        uint64_t    max_chunk = 0;
        std::string sha256;
        std::string bytes;
        int         attempts = 0;
    } scene_fetch_;
    uint32_t       scene_finish_req_ = 0;  /* unawaited finish */
    QVariantMap playback_;
    QVariantMap frame_status_;
    QVariantMap frame_aggregate_;
    QVariantMap frame_wireless_;
    FrameFeedModel* frame_controllers_ = nullptr;   /* child */
    /* Read on the worker thread (event filter), written on the
       GUI thread. */
    std::atomic<bool> frame_feed_active_{ true };
    /* ONE latest-wins frame_status slot (audit U-10). The wire
       thread overwrites it and posts at most one apply while
       none is queued, so a GUI thread that falls behind a ~30 Hz
       feed converts and repaints only the newest frame instead
       of a backlog. While the feed is inactive the slot is just
       parked: the engine only emits on change, so on
       re-activation it IS the current state (static, uploaded
       and power-gated output would otherwise keep showing
       pre-hide data). frame_mu_ guards slot + queued flag
       between the wire thread and the GUI thread. */
    std::mutex                      frame_mu_;
    std::unique_ptr<nlohmann::json> pending_frame_status_;
    bool                            frame_post_queued_ = false;
    QVariantMap delivery_stats_;
    QVariantList presets_;
    QVariantMap diagnostics_;
    QVariantMap state_report_;
    QVariantMap reactive_;
    QVariantMap ownership_;
    QVariantMap supervisor_;
    QVariantMap output_;
    bool        has_output_control_ = false;
    QVariantMap automation_;
    bool        has_automation_ = false;
    bool        has_workspace_ = false;
    QVariantMap workspace_;
};

} /* namespace ui */
} /* namespace openrgb2 */
