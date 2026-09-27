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
    /* Bounded exponential backoff — retries are forever but the
       WAIT between them is capped (plan section 6). */
    factory_ = [this](std::string& err)
    {
        return ipc::ConnectNamedPipeChannel(pipe_name_.toStdString(),
                                            2000, err);
    };
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
    if(running_)
    {
        return;
    }
    running_      = true;
    fail_count_   = 0;
    reconnect_req_ = false;
    worker_ = std::thread(&EngineBridge::WorkerMain, this);
}

void EngineBridge::stop()
{
    {
        std::lock_guard<std::mutex> lock(mu_);
        running_ = false;
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
    std::lock_guard<std::mutex> lock(mu_);
    return running_;
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

        {
            std::lock_guard<std::mutex> lock(mu_);
            fail_count_ = 0;
        }

        EngineWireClient::Handlers h;
        h.onWelcome = [this](const nlohmann::json& j)
        {
            Post([this, j] { ApplyWelcome(j); });
        };
        h.onEvent = [this](const nlohmann::json& j)
        {
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
        const EngineWireClient::EndReason r =
            wire_.Run(std::move(ch), h, wire_options_, detail);
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
    server_info_ = JsonToVariant(welcome).toMap();
    emit serverInfoChanged();
    setConnectionState("connected");
    setRetryInMs(0);
    setLastError(QString());

    /* WELCOME carries a GET_STATE body — paint immediately;
       the SUBSCRIBE initial EVENTs refine it right after. */
    const nlohmann::json state = welcome.value("state",
                                               nlohmann::json());
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

void EngineBridge::ApplyEvent(const nlohmann::json& event_body)
{
    const std::string kind =
        event_body.value("kind", std::string());
    const nlohmann::json data = event_body.value("data",
                                                 nlohmann::json());

    if(kind == ipc::event_kind::kTopology)
    {
        snapshots_ =
            JsonToVariant(data.value("snapshots",
                                     nlohmann::json::array()))
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
        scene_ = JsonToVariant(data).toMap();
        emit sceneChanged();
    }
    else if(kind == ipc::event_kind::kPlayback)
    {
        playback_ = JsonToVariant(data).toMap();
        emit playbackChanged();
    }
    else if(kind == ipc::event_kind::kConnection)
    {
        connection_ = JsonToVariant(data).toMap();
        emit connectionChanged();
    }
    else if(kind == ipc::event_kind::kFrameStatus)
    {
        frame_status_ = JsonToVariant(data).toMap();
        emit frameStatusChanged();
    }
    RecomputeDegraded();

    emit eventReceived(QString::fromStdString(kind),
                       JsonToVariant(data).toMap());
}

void EngineBridge::ApplyReply(uint32_t request_id, ipc::MsgType type,
                              const nlohmann::json& body)
{
    switch(type)
    {
    case ipc::MsgType::Snapshots:
        snapshots_ =
            JsonToVariant(body.value("snapshots",
                                     nlohmann::json::array()))
                .toList();
        emit snapshotsChanged();
        if(body.contains("connection") &&
           body["connection"].is_object())
        {
            connection_ = JsonToVariant(body["connection"]).toMap();
            emit connectionChanged();
        }
        break;
    case ipc::MsgType::Scene:
        scene_ = JsonToVariant(body).toMap();
        emit sceneChanged();
        break;
    case ipc::MsgType::Playback:
        playback_ = JsonToVariant(body).toMap();
        emit playbackChanged();
        break;
    case ipc::MsgType::DeliveryStats:
        delivery_stats_ = JsonToVariant(body).toMap();
        emit deliveryStatsChanged();
        break;
    case ipc::MsgType::State:
        state_report_ = JsonToVariant(body).toMap();
        emit stateReportChanged();
        if(body.contains("sdk") && body["sdk"].is_object())
        {
            connection_ = JsonToVariant(body["sdk"]).toMap();
            emit connectionChanged();
        }
        break;
    case ipc::MsgType::Presets:
        presets_ =
            JsonToVariant(body.value("presets",
                                     nlohmann::json::array()))
                .toList();
        emit presetsChanged();
        break;
    case ipc::MsgType::Diagnostics:
        diagnostics_ = JsonToVariant(body).toMap();
        emit diagnosticsChanged();
        break;
    case ipc::MsgType::Error:
        setLastError(QString::fromStdString(
            body.value("message", std::string("engine error"))));
        break;
    default:
        break;
    }
    RecomputeDegraded();

    emit replyReceived((int)request_id,
                       QString::fromLatin1(ipc::MsgTypeName(type)),
                       JsonToVariant(body).toMap());
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
int EngineBridge::quitEngine()     { return Issue(ipc::MsgType::Quit, nlohmann::json::object()); }

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
QVariantMap EngineBridge::frameStatus() const     { return frame_status_; }
QVariantMap EngineBridge::deliveryStats() const   { return delivery_stats_; }
QVariantList EngineBridge::presets() const        { return presets_; }
QVariantMap EngineBridge::diagnostics() const     { return diagnostics_; }
QVariantMap EngineBridge::stateReport() const     { return state_report_; }

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
