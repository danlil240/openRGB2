/*---------------------------------------------------------*\
||| EngineBridge.cpp                                          ||
|||                                                           ||
|||   SPDX-License-Identifier: GPL-2.0-or-later               ||
|\*---------------------------------------------------------*/

#include "EngineBridge.h"

#include "JsonVariant.h"

#include <QMetaObject>

namespace openrgb2
{
namespace ui
{

EngineBridge::EngineBridge(QObject* parent)
    : QObject(parent)
    , pipe_name_(QString::fromStdString(ipc::kCommandPipeName))
    , backoff_ms_({ 1000, 2000, 4000, 8000, 15000, 30000 })
{
    frame_controllers_ = new FrameFeedModel(this);

    /* Bounded exponential backoff — retries are forever but the
       WAIT between them is capped (plan section 6). */
    factory_ = [this](std::string& err)
    {
        return ipc::ConnectNamedPipeChannel(pipe_name_.toStdString(),
                                            2000, err);
    };

    /* playbackView is derived from the playback + connection
       stores — any update to either recomputes the honest
       applied/requested phase for every bound view. */
    connect(this, &EngineBridge::playbackChanged,
            this, &EngineBridge::playbackViewChanged);
    connect(this, &EngineBridge::connectionChanged,
            this, &EngineBridge::playbackViewChanged);
}

EngineBridge::~EngineBridge()
{
    stop();
}

/*---------------------------------------------------------*\
||| Lifecycle                                                ||
\*---------------------------------------------------------*/

void EngineBridge::start()
{
    std::lock_guard<std::mutex> lock(mu_);
    if(running_.load())
    {
        return;
    }
    running_.store(true);
    fail_count_   = 0;
    reconnect_req_ = false;
    worker_ = std::thread(&EngineBridge::WorkerMain, this);
}

void EngineBridge::stop()
{
    {
        std::lock_guard<std::mutex> lock(mu_);
        running_.store(false);
    }
    wake_cv_.notify_all();
    wire_.Stop();
    if(worker_.joinable())
    {
        worker_.join();
    }
}

void EngineBridge::reconnectNow()
{
    if(!Running())
    {
        start();       /* manual reconnect also begins a stopped loop */
        return;
    }
    {
        std::lock_guard<std::mutex> lock(mu_);
        reconnect_req_ = true;
        fail_count_    = 0;
    }
    wake_cv_.notify_all();
    wire_.Stop();      /* drops a live session; loop retries */
}

bool EngineBridge::Running() const
{
    return running_.load();
}

template <typename F> void EngineBridge::Post(F&& f)
{
    /* Handlers arrive on the worker thread; queued delivery runs
       the store updates on the thread that owns this QObject
       (the GUI thread). Posted events targeting a destroyed
       receiver are dropped by Qt. */
    QMetaObject::invokeMethod(this, std::forward<F>(f),
                              Qt::QueuedConnection);
}

void EngineBridge::WorkerMain()
{
    while(Running())
    {
        Post([this] { setConnectionState("connecting"); });
        attempts_.fetch_add(1);

        std::string err;
        std::unique_ptr<ipc::IIpcChannel> ch =
            factory_ ? factory_(err) : nullptr;
        if(!ch)
        {
            const QString e =
                QString::fromStdString(
                    err.empty() ? "engine pipe not available" : err);
            Post([this, e] { setLastError(e); });
            if(!BackoffWait())
            {
                break;
            }
            continue;
        }

        /* fail_count_ is NOT reset here: a pipe that accepts the
           connect but then fails the handshake (version mismatch,
           an impostor/hung server, immediate close) must keep
           climbing the backoff schedule — only a WELCOME proves a
           usable session (see onWelcome below). */

        /* stop() may have landed while the (blocking, up to the
           connect deadline) factory was running — never start a
           session on a bridge that was asked to die. The channel
           closes on destruction. */
        if(!Running())
        {
            break;
        }

        EngineWireClient::Handlers h;
        h.onWelcome = [this](const nlohmann::json& j)
        {
            /* Handshake completed — the session is usable, so the
               next drop restarts the backoff schedule at its
               first step. */
            {
                std::lock_guard<std::mutex> lock(mu_);
                fail_count_ = 0;
            }
            Post([this, j] { ApplyWelcome(j); });
        };
        h.onEvent = [this](const nlohmann::json& j)
        {
            /* frame_status arrives at up to ~30 Hz; while no
               window can show it, park only the newest one here
               so neither the JSON->QVariant conversion nor any
               QML binding work happens. setFrameFeedActive(true)
               applies the parked one — the engine emits only on
               change, so nothing else would repaint it. */
            if(j.is_object())
            {
                const auto k = j.find("kind");
                if(k != j.end() && k->is_string()
                   && k->get_ref<const std::string&>()
                          == ipc::event_kind::kFrameStatus)
                {
                    /* Latest-wins slot (audit U-10): overwrite the
                       newest frame and post ONE apply while none
                       is queued — a GUI thread behind the feed
                       never works through a backlog. While the
                       feed is inactive the frame only parks. */
                    std::lock_guard<std::mutex> lock(frame_mu_);
                    if(pending_frame_status_)
                        *pending_frame_status_ = j;
                    else
                        pending_frame_status_ =
                            std::make_unique<nlohmann::json>(j);
                    if(frame_feed_active_.load() &&
                       !frame_post_queued_)
                    {
                        frame_post_queued_ = true;
                        Post([this] { ApplyPendingFrameStatus(); });
                    }
                    return;
                }
            }
            Post([this, j] { ApplyEvent(j); });
        };
        h.onReply = [this](uint32_t id, ipc::MsgType t,
                           const nlohmann::json& b)
        {
            Post([this, id, t, b] { ApplyReply(id, t, b); });
        };
        h.onProtocolError = [this](const std::string& s)
        {
            Post([this, s] {
                emit protocolError(QString::fromStdString(s));
            });
        };

        std::string detail;
        /* &running_ is the session's external cancellation flag:
           even if stop() raced past every check above, the wire
           loop honours it before HELLO — no session can outlive
           the owner's request to stop. */
        const EngineWireClient::EndReason r =
            wire_.Run(std::move(ch), h, wire_options_, detail,
                      &running_);
        Post([this, r, detail] { ApplySessionEnd(r, detail); });

        if(!Running())
        {
            break;
        }
        if(!BackoffWait())
        {
            break;
        }
    }
    Post([this] { setConnectionState("disconnected"); });
}

bool EngineBridge::BackoffWait()
{
    size_t idx;
    {
        std::lock_guard<std::mutex> lock(mu_);
        idx = fail_count_ < backoff_ms_.size()
                  ? fail_count_
                  : backoff_ms_.size() - 1;
        ++fail_count_;
    }
    const int ms = backoff_ms_.empty() ? 1000 : backoff_ms_[idx];
    Post([this, ms] {
        setConnectionState("unreachable");
        setRetryInMs(ms);
    });

    std::unique_lock<std::mutex> lock(mu_);
    wake_cv_.wait_for(lock, std::chrono::milliseconds(ms),
                      [this] { return !running_ || reconnect_req_; });
    if(!running_)
    {
        return false;
    }
    reconnect_req_ = false;
    return true;
}

/*---------------------------------------------------------*\
||| Wire callbacks -> stores (GUI thread)                    ||
\*---------------------------------------------------------*/

void EngineBridge::ApplyWelcome(const nlohmann::json& welcome)
{
    /* A hostile/buggy body with wrong-typed fields could throw
       out of json accessors — a malformed push must surface as
       protocolError, never take down the UI. */
    try
    {
    server_info_ = JsonToVariant(welcome).toMap();
    emit serverInfoChanged();
    /* The wire captured the WELCOME capability set — a UI bound
       to hasWorkspace falls back to the legacy scene verbs when
       "studio_workspace_v1" is absent. */
    setHasWorkspace(wire_.HasCapability("studio_workspace_v1"));
    setConnectionState("connected");
    setRetryInMs(0);
    setLastError(QString());

    /* WELCOME carries a GET_STATE body — paint immediately;
       the SUBSCRIBE initial EVENTs refine it right after.
       Guarded: a non-object body (array/scalar/discarded parse)
       must never reach .value() — it throws type_error. */
    const nlohmann::json state =
        welcome.is_object()
            ? welcome.value("state", nlohmann::json())
            : nlohmann::json();
    if(state.is_object())
    {
        if(state.contains("scene") && state["scene"].is_object())
        {
            scene_ = JsonToVariant(state["scene"]).toMap();
            emit sceneChanged();
        }
        if(state.contains("sdk") && state["sdk"].is_object())
        {
            connection_ = JsonToVariant(state["sdk"]).toMap();
            emit connectionChanged();
        }
        if(state.contains("playback") &&
           state["playback"].is_object())
        {
            playback_ = JsonToVariant(state["playback"]).toMap();
            emit playbackChanged();
        }
        if(state.contains("reactive") &&
           state["reactive"].is_object())
        {
            reactive_ = JsonToVariant(state["reactive"]).toMap();
            emit reactiveChanged();
        }
        state_report_ = JsonToVariant(state).toMap();
        emit stateReportChanged();
    }
    RecomputeDegraded();

    /* Pull what pushes don't cover (preset cards, delivery
       counters, the full state report). */
    getPresets();
    getDeliveryStats();
    getState();
    }
    catch(const std::exception& e)
    {
        emit protocolError(QString::fromStdString(
            std::string("malformed WELCOME body: ") + e.what()));
    }
}

void EngineBridge::ApplyEvent(const nlohmann::json& event_body)
{
    /* A non-object EVENT body (array, scalar, discarded parse)
       is a protocol violation — surface it, never .value() it. */
    if(!event_body.is_object())
    {
        emit protocolError(
            QStringLiteral("EVENT body is not a JSON object"));
        return;
    }
    try
    {
    const std::string kind =
        event_body.value("kind", std::string());
    const nlohmann::json data = event_body.value("data",
                                                 nlohmann::json());
    /* Converted ONCE: the store update and eventReceived share
       the same QVariantMap (implicitly shared, no deep copy). */
    const QVariantMap data_map = JsonToVariant(data).toMap();

    if(kind == ipc::event_kind::kTopology)
    {
        snapshots_ =
            JsonToVariant(data.is_object()
                              ? data.value("snapshots",
                                           nlohmann::json::array())
                              : nlohmann::json::array())
                .toList();
        emit snapshotsChanged();
        if(data.contains("connection") &&
           data["connection"].is_object())
        {
            connection_ = JsonToVariant(data["connection"]).toMap();
            emit connectionChanged();
        }
    }
    else if(kind == ipc::event_kind::kScene)
    {
        scene_ = data_map;
        emit sceneChanged();
    }
    else if(kind == ipc::event_kind::kPlayback)
    {
        playback_ = data_map;
        emit playbackChanged();
    }
    else if(kind == ipc::event_kind::kConnection)
    {
        connection_ = data_map;
        emit connectionChanged();
    }
    else if(kind == ipc::event_kind::kFrameStatus)
    {
        frame_status_ = data_map;
        frame_controllers_->apply(
            data_map.value(QStringLiteral("controllers")).toList());
        const QVariantMap ag =
            data_map.value(QStringLiteral("aggregate")).toMap();
        if(ag != frame_aggregate_)
        {
            frame_aggregate_ = ag;
            emit frameAggregateChanged();
        }
        const QVariantMap wl =
            data_map.value(QStringLiteral("wireless")).toMap();
        if(wl != frame_wireless_)
        {
            frame_wireless_ = wl;
            emit frameWirelessChanged();
        }
        emit frameStatusChanged();
    }
    else if(kind == ipc::event_kind::kReactive)
    {
        reactive_ = data_map;
        emit reactiveChanged();
    }
    else if(kind == ipc::event_kind::kWorkspace)
    {
        /* Initial snapshot = `get` metadata; terminal job
           results = the job status record — both stored
           verbatim; WorkspaceController correlates by
           job_id/revision. */
        workspace_ = data_map;
        emit workspaceChanged();
    }
    RecomputeDegraded();

    emit eventReceived(QString::fromStdString(kind), data_map);
    }
    catch(const std::exception& e)
    {
        emit protocolError(QString::fromStdString(
            std::string("malformed EVENT body: ") + e.what()));
    }
}

/* Assign + notify only when the value moved (audit U-10): the
   Devices page polls snapshots/state/delivery/diagnostics every
   couple of seconds, and an identical reply re-running every
   binding and LayoutController's per-snapshot rebuild is pure
   GUI-thread waste. */
template <typename T, typename Sig>
static void SetIfChanged(EngineBridge* self, T& store, T&& value,
                         Sig signal)
{
    if(store != value)
    {
        store = std::move(value);
        emit (self->*signal)();
    }
}

void EngineBridge::ApplyReply(uint32_t request_id, ipc::MsgType type,
                              const nlohmann::json& body)
{
    /* Converted ONCE: whole-body stores and replyReceived share
       the same implicitly shared QVariantMap. */
    const QVariantMap body_map = JsonToVariant(body).toMap();
    /* Reply bodies are objects by contract; anything else skips
       the store update (still emitted to replyReceived below —
       the correlator is what callers pair on). The try/catch
       covers wrong-typed fields inside an object body. */
    try
    {
    if(body.is_object())
    {
    switch(type)
    {
    case ipc::MsgType::Snapshots:
        SetIfChanged(this, snapshots_,
            JsonToVariant(body.value("snapshots",
                                     nlohmann::json::array()))
                .toList(),
            &EngineBridge::snapshotsChanged);
        if(body.contains("connection") &&
           body["connection"].is_object())
        {
            SetIfChanged(this, connection_,
                JsonToVariant(body["connection"]).toMap(),
                &EngineBridge::connectionChanged);
        }
        break;
    case ipc::MsgType::Scene:
        scene_ = body_map;
        emit sceneChanged();
        break;
    case ipc::MsgType::Playback:
        playback_ = body_map;
        emit playbackChanged();
        break;
    case ipc::MsgType::DeliveryStats:
        SetIfChanged(this, delivery_stats_, QVariantMap(body_map),
                     &EngineBridge::deliveryStatsChanged);
        break;
    case ipc::MsgType::State:
        SetIfChanged(this, state_report_, QVariantMap(body_map),
                     &EngineBridge::stateReportChanged);
        if(body.contains("sdk") && body["sdk"].is_object())
        {
            SetIfChanged(this, connection_,
                body_map.value(QStringLiteral("sdk")).toMap(),
                &EngineBridge::connectionChanged);
        }
        if(body.contains("reactive") &&
           body["reactive"].is_object())
        {
            SetIfChanged(this, reactive_,
                body_map.value(QStringLiteral("reactive")).toMap(),
                &EngineBridge::reactiveChanged);
        }
        break;
    case ipc::MsgType::Presets:
        SetIfChanged(this, presets_,
            JsonToVariant(body.value("presets",
                                     nlohmann::json::array()))
                .toList(),
            &EngineBridge::presetsChanged);
        break;
    case ipc::MsgType::Diagnostics:
        SetIfChanged(this, diagnostics_, QVariantMap(body_map),
                     &EngineBridge::diagnosticsChanged);
        break;
    case ipc::MsgType::Error:
        setLastError(QString::fromStdString(
            body.value("message", std::string("engine error"))));
        break;
    default:
        break;
    }
    }
    }
    catch(const std::exception& e)
    {
        emit protocolError(QString::fromStdString(
            std::string("malformed reply body: ") + e.what()));
    }
    RecomputeDegraded();

    emit replyReceived((int)request_id,
                       QString::fromLatin1(ipc::MsgTypeName(type)),
                       body_map);
}

void EngineBridge::ApplySessionEnd(EngineWireClient::EndReason reason,
                                   const std::string& detail)
{
    QString why;
    switch(reason)
    {
    case EngineWireClient::EndReason::Stopped:
        why = "session stopped";
        break;
    case EngineWireClient::EndReason::PeerClosed:
        why = "engine closed the connection";
        break;
    case EngineWireClient::EndReason::HandshakeFailed:
        why = "handshake failed";
        break;
    case EngineWireClient::EndReason::TransportError:
        why = "transport error";
        break;
    case EngineWireClient::EndReason::ResyncExceeded:
        why = "unrecoverable stream garbage";
        break;
    }
    if(!detail.empty())
    {
        why += ": " + QString::fromStdString(detail);
    }
    setLastError(why);
    /* The wire cleared its capability set — the flag follows so
       a reconnecting UI never briefly believes an old engine. */
    setHasWorkspace(false);
    emit engineLost(why);
    /* While running_ the worker retries with backoff; the
       stores stay populated (stale-but-labelled, never erased
       into a fake "all good"). */
}

void EngineBridge::RecomputeDegraded()
{
    bool deg = false;
    if(connection_state_ == "connected")
    {
        const QString sdk =
            connection_.value("state").toString();
        if(!sdk.isEmpty() && sdk != "connected")
        {
            deg = true;   /* engine up, backend link down */
        }
        if(server_info_.contains("running") &&
           !server_info_.value("running").toBool())
        {
            deg = true;
        }
    }
    setDegraded(deg);
}

/*---------------------------------------------------------*\
||| Property setters (GUI thread)                            ||
\*---------------------------------------------------------*/

void EngineBridge::setConnectionState(const QString& s)
{
    if(connection_state_ == s)
    {
        return;
    }
    connection_state_ = s;
    emit connectionStateChanged();
    RecomputeDegraded();
}

void EngineBridge::setLastError(const QString& s)
{
    if(last_error_ == s)
    {
        return;
    }
    last_error_ = s;
    emit lastErrorChanged();
}

void EngineBridge::setDegraded(bool d)
{
    if(degraded_ == d)
    {
        return;
    }
    degraded_ = d;
    emit degradedChanged();
}

void EngineBridge::setRetryInMs(int ms)
{
    if(retry_in_ms_ == ms)
    {
        return;
    }
    retry_in_ms_ = ms;
    emit retryInMsChanged();
}

void EngineBridge::setHasWorkspace(bool h)
{
    if(has_workspace_ == h)
    {
        return;
    }
    has_workspace_ = h;
    emit hasWorkspaceChanged();
}

/*---------------------------------------------------------*\
||| Requests                                                 ||
\*---------------------------------------------------------*/

int EngineBridge::Issue(ipc::MsgType type, const nlohmann::json& body)
{
    uint32_t    id = 0;
    std::string err;
    if(!wire_.SendRequest(type, body, id, err))
    {
        setLastError(QString::fromStdString(err));
        return 0;
    }
    return (int)id;
}

int EngineBridge::getSnapshots()   { return Issue(ipc::MsgType::GetSnapshots, nlohmann::json::object()); }
int EngineBridge::getScene()       { return Issue(ipc::MsgType::GetScene, nlohmann::json::object()); }
int EngineBridge::getPlayback()    { return Issue(ipc::MsgType::GetPlayback, nlohmann::json::object()); }
int EngineBridge::getDeliveryStats(){ return Issue(ipc::MsgType::GetDeliveryStats, nlohmann::json::object()); }
int EngineBridge::getState()       { return Issue(ipc::MsgType::GetState, nlohmann::json::object()); }
int EngineBridge::getPresets()     { return Issue(ipc::MsgType::GetPresets, nlohmann::json::object()); }
int EngineBridge::getDiagnostics() { return Issue(ipc::MsgType::DiagGet, nlohmann::json::object()); }
int EngineBridge::ping()           { return Issue(ipc::MsgType::Ping, nlohmann::json::object()); }
int EngineBridge::unsubscribe()    { return Issue(ipc::MsgType::Unsubscribe, nlohmann::json::object()); }
int EngineBridge::quitEngine(bool stopBackend)
{
    return Issue(ipc::MsgType::Quit, nlohmann::json::object({
        { "stop_backend", stopBackend } }));
}

int EngineBridge::resubscribe()
{
    return Issue(ipc::MsgType::Subscribe,
                 nlohmann::json::object({ { "events", "all" } }));
}

int EngineBridge::setScene(const QVariantMap& document)
{
    return Issue(ipc::MsgType::SetScene,
                 nlohmann::json::object(
                     { { "document", VariantToJson(document) } }));
}

int EngineBridge::setPlaybackMode(const QString& mode,
                                  const QString& control,
                                  const QVariantMap& params)
{
    nlohmann::json b = nlohmann::json::object();
    b["mode"] = mode.toStdString();
    if(!control.isEmpty())
    {
        b["control"] = control.toStdString();
    }
    if(!params.isEmpty())
    {
        b["params"] = VariantToJson(params);
    }
    return Issue(ipc::MsgType::SetPlaybackMode, b);
}

int EngineBridge::layoutCommand(const QString& verb,
                                const QVariantMap& args)
{
    nlohmann::json b = VariantToJson(args);
    if(!b.is_object())
    {
        b = nlohmann::json::object();
    }
    b["verb"] = verb.toStdString();
    return Issue(ipc::MsgType::LayoutCommand, b);
}

int EngineBridge::reactiveCommand(const QString& verb,
                                  const QVariantMap& args)
{
    nlohmann::json b = VariantToJson(args);
    if(!b.is_object())
    {
        b = nlohmann::json::object();
    }
    b["verb"] = verb.toStdString();
    return Issue(ipc::MsgType::ReactiveCommand, b);
}

int EngineBridge::workspaceCommand(const QString& verb,
                                   const QVariantMap& args)
{
    return workspaceCommandJson(verb, VariantToJson(args));
}

int EngineBridge::workspaceCommandJson(const QString& verb,
                                       nlohmann::json args)
{
    if(!args.is_object())
    {
        args = nlohmann::json::object();
    }
    args["verb"] = verb.toStdString();
    return Issue(ipc::MsgType::WorkspaceCommand, args);
}

/*---------------------------------------------------------*\
||| Accessors + test hooks                                    ||
\*---------------------------------------------------------*/

QString     EngineBridge::connectionState() const { return connection_state_; }
bool        EngineBridge::connected() const
            { return connection_state_ == "connected"; }
bool        EngineBridge::degraded() const        { return degraded_; }
QString     EngineBridge::lastError() const       { return last_error_; }
QString     EngineBridge::pipeName() const        { return pipe_name_; }
int         EngineBridge::retryInMs() const       { return retry_in_ms_; }
QVariantMap EngineBridge::serverInfo() const      { return server_info_; }
QVariantList EngineBridge::snapshots() const      { return snapshots_; }
QVariantMap EngineBridge::connection() const      { return connection_; }
QVariantMap EngineBridge::scene() const           { return scene_; }
QVariantMap EngineBridge::playback() const        { return playback_; }

/*---------------------------------------------------------*\
|||| playbackView — the honest applied-vs-requested view    ||
|||| of the PLAYBACK body (docs/ipc-protocol.md §5):        ||
|||| requested_mode is the recorded selection, applied_mode ||
|||| is status.mode (moves only on backend ack), pending    ||
|||| covers wire-pending OR a requested/applied mismatch,   ||
|||| and phase is the single display token. Every cadence/  ||
|||| sync/physical field passes through what the wire       ||
|||| reports — unmeasured stays unmeasured, never a         ||
|||| fabricated FPS number (plan section 5).                ||
\*---------------------------------------------------------*/
QVariantMap EngineBridge::playbackView() const
{
    const QVariantMap st =
        playback_.value(QStringLiteral("status")).toMap();
    const QVariantMap caps =
        playback_.value(QStringLiteral("capabilities")).toMap();
    const QVariantMap phys =
        st.value(QStringLiteral("physical")).toMap();

    const QString requested =
        playback_.value(QStringLiteral("requested_mode")).toString();
    const QString applied =
        st.value(QStringLiteral("mode")).toString();
    const QString upload =
        st.value(QStringLiteral("upload")).toString();
    const QString control =
        st.value(QStringLiteral("control")).toString();
    const QString detail =
        playback_.value(QStringLiteral("detail")).toString();
    const QString reason =
        playback_.value(QStringLiteral("unavailable_reason")).toString();
    const QString sdk =
        connection_.value(QStringLiteral("state")).toString();

    const bool wire_pending =
        playback_.value(QStringLiteral("pending")).toBool();
    const bool mismatch = !requested.isEmpty() && !applied.isEmpty()
                          && requested != applied;
    const bool pending = wire_pending || mismatch;

    /* Phase precedence — the most honest headline first:
       a dead/reconnecting backend link outranks everything
       (a pending ack cannot land across it), an in-flight
       transition outranks the last FAILED state (a new
       request supersedes it), and a requested/applied
       mismatch is still "applying" — never "applied" before
       the revision confirms. */
    QString phase;
    if(!sdk.isEmpty() && sdk != QLatin1String("connected"))
    {
        phase = QStringLiteral("reconnecting");
    }
    else if(wire_pending)
    {
        phase = QStringLiteral("applying");
    }
    else if(upload == QLatin1String("failed"))
    {
        phase = QStringLiteral("failed");
    }
    else if(applied.isEmpty())
    {
        phase = QStringLiteral("unknown");
    }
    else if(mismatch)
    {
        phase = QStringLiteral("applying");
    }
    else if(applied == QLatin1String("animation_playback"))
    {
        phase = upload == QLatin1String("uploading")
                    ? QStringLiteral("uploading")
                    : QStringLiteral("active");
    }
    else
    {
        phase = QStringLiteral("live");
    }

    /* Configured (NOT measured) loop cadence — the shared
       loop builder's effective geometry the engine reports. */
    const int    frame_count =
        st.value(QStringLiteral("effective_frame_count")).toInt();
    const double frame_interval =
        st.value(QStringLiteral("effective_frame_interval_s"))
            .toDouble();
    const double cycle =
        frame_count > 0 && frame_interval > 0.0
            ? frame_count * frame_interval : 0.0;
    const double configured_hz =
        frame_interval > 0.0 ? 1.0 / frame_interval : 0.0;

    /* Physical measurement — only what the wire reports as a
       physical measurement: the provenance contract is that
       only "physically_measured" claims one ("configured" and
       "transport_measured" describe other channels). */
    const double  pint =
        phys.value(QStringLiteral("frame_interval_s")).toDouble();
    const QString prov =
        phys.value(QStringLiteral("provenance")).toString();
    const bool measured = pint > 0.0 &&
                          prov == QLatin1String("physically_measured");

    QVariantMap out;
    out.insert(QStringLiteral("requested_mode"),  requested);
    out.insert(QStringLiteral("applied_mode"),    applied);
    out.insert(QStringLiteral("wire_pending"),    wire_pending);
    out.insert(QStringLiteral("pending"),         pending);
    out.insert(QStringLiteral("control"),         control);
    out.insert(QStringLiteral("upload"),          upload);
    out.insert(QStringLiteral("phase"),           phase);
    /* Detail belongs to the state it described — a failure
       reason must not linger beside a fresh "applying" the
       next request already started (Task 4.8 minor). */
    out.insert(QStringLiteral("phase_detail"),
               phase == QLatin1String("failed")
                   ? detail : QString());
    out.insert(QStringLiteral("unavailable_reason"), reason);
    out.insert(QStringLiteral("animation_available"),
               caps.value(QStringLiteral("animation_playback"))
                   .toBool());
    out.insert(QStringLiteral("live_available"),
               caps.value(QStringLiteral("live_effects"))
                   .toBool());
    out.insert(QStringLiteral("sdk_state"),       sdk);
    out.insert(QStringLiteral("requested_revision"),
               playback_.value(QStringLiteral("requested_revision")));
    out.insert(QStringLiteral("preset_revision"),
               st.value(QStringLiteral("preset_revision")));
    out.insert(QStringLiteral("parameter_revision"),
               st.value(QStringLiteral("parameter_revision")));
    out.insert(QStringLiteral("frame_count"),      frame_count);
    out.insert(QStringLiteral("frame_interval_s"), frame_interval);
    out.insert(QStringLiteral("cycle_s"),          cycle);
    out.insert(QStringLiteral("configured_hz"),    configured_hz);
    out.insert(QStringLiteral("sync"),
               st.value(QStringLiteral("sync")).toString());
    out.insert(QStringLiteral("clock_sample_s"),
               st.value(QStringLiteral("clock_sample_s")));
    out.insert(QStringLiteral("clock_uncertainty_s"),
               st.value(QStringLiteral("clock_uncertainty_s")));
    out.insert(QStringLiteral("physical_measured"), measured);
    out.insert(QStringLiteral("physical_frame_interval_s"), pint);
    out.insert(QStringLiteral("physical_cycle_s"),
               phys.value(QStringLiteral("cycle_s")));
    out.insert(QStringLiteral("physical_provenance"), prov);
    return out;
}

QVariantMap EngineBridge::frameStatus() const     { return frame_status_; }
QObject*    EngineBridge::frameControllers() const
            { return frame_controllers_; }
QVariantMap EngineBridge::frameAggregate() const  { return frame_aggregate_; }
QVariantMap EngineBridge::frameWireless() const   { return frame_wireless_; }
bool        EngineBridge::frameFeedActive() const
            { return frame_feed_active_.load(); }

void EngineBridge::setFrameFeedActive(bool active)
{
    {
        std::lock_guard<std::mutex> lock(frame_mu_);
        if(frame_feed_active_.exchange(active) == active)
        {
            return;
        }
        /* false -> true: repaint the parked frame_status (the
           newest; the slot is latest-wins). */
        if(active && pending_frame_status_ && !frame_post_queued_)
        {
            frame_post_queued_ = true;
            Post([this] { ApplyPendingFrameStatus(); });
        }
    }
    emit frameFeedActiveChanged();
}

void EngineBridge::ApplyPendingFrameStatus()
{
    std::unique_ptr<nlohmann::json> f;
    {
        std::lock_guard<std::mutex> lock(frame_mu_);
        frame_post_queued_ = false;
        if(!frame_feed_active_.load())
        {
            return;     /* hidden since the post: stays parked */
        }
        f = std::move(pending_frame_status_);
    }
    if(f)
    {
        ApplyEvent(*f);
    }
}
QVariantMap EngineBridge::deliveryStats() const   { return delivery_stats_; }
QVariantList EngineBridge::presets() const        { return presets_; }
QVariantMap EngineBridge::diagnostics() const     { return diagnostics_; }
QVariantMap EngineBridge::stateReport() const     { return state_report_; }
QVariantMap EngineBridge::reactive() const        { return reactive_; }
bool        EngineBridge::hasWorkspace() const    { return has_workspace_; }
QVariantMap EngineBridge::workspace() const       { return workspace_; }

void EngineBridge::setPipeName(const QString& name)
{
    pipe_name_ = name;
}

void EngineBridge::setChannelFactoryForTesting(ChannelFactory f)
{
    factory_ = std::move(f);
}

void EngineBridge::setWireOptionsForTesting(
    const EngineWireClient::Options& o)
{
    wire_options_ = o;
}

void EngineBridge::setBackoffForTesting(
    const std::vector<int>& delays_ms)
{
    backoff_ms_ = delays_ms;
}

int EngineBridge::connectAttemptsForTesting() const
{
    return attempts_.load();
}

} /* namespace ui */
} /* namespace openrgb2 */
