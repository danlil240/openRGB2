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
    Q_PROPERTY(QVariantMap frameStatus READ frameStatus
                   NOTIFY frameStatusChanged)
    Q_PROPERTY(QVariantMap deliveryStats READ deliveryStats
                   NOTIFY deliveryStatsChanged)
    Q_PROPERTY(QVariantList presets READ presets
                   NOTIFY presetsChanged)
    Q_PROPERTY(QVariantMap diagnostics READ diagnostics
                   NOTIFY diagnosticsChanged)
    Q_PROPERTY(QVariantMap stateReport READ stateReport
                   NOTIFY stateReportChanged)

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
    Q_INVOKABLE int quitEngine();

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
    QVariantMap frameStatus() const;
    QVariantMap deliveryStats() const;
    QVariantList presets() const;
    QVariantMap diagnostics() const;
    QVariantMap stateReport() const;

    /*---- test hooks (call before start()) ----------------------*/
    using ChannelFactory =
        std::function<std::unique_ptr<ipc::IIpcChannel>(
            std::string& err)>;
    void setChannelFactoryForTesting(ChannelFactory f);
    void setWireOptionsForTesting(const EngineWireClient::Options& o);
    void setBackoffForTesting(const std::vector<int>& delays_ms);
    int  connectAttemptsForTesting() const;

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
    void frameStatusChanged();
    void deliveryStatsChanged();
    void presetsChanged();
    void diagnosticsChanged();
    void stateReportChanged();

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
    void ApplyReply(uint32_t request_id, ipc::MsgType type,
                    const nlohmann::json& body);
    void ApplySessionEnd(EngineWireClient::EndReason reason,
                         const std::string& detail);
    void RecomputeDegraded();

    void setConnectionState(const QString& s);
    void setLastError(const QString& s);
    void setDegraded(bool d);
    void setRetryInMs(int ms);

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
    QVariantMap playback_;
    QVariantMap frame_status_;
    QVariantMap delivery_stats_;
    QVariantList presets_;
    QVariantMap diagnostics_;
    QVariantMap state_report_;
};

} /* namespace ui */
} /* namespace openrgb2 */
