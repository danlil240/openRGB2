/*---------------------------------------------------------*\
||| QaServer.h — openrgb2-ui QA automation socket           ||
|||                                                           ||
|||   Dev-only in-process harness: a loopback-only TCP      ||
|||   listener that exposes the live QML item tree to an    ||
|||   external driver (tools/qa/qa.py). Line-delimited      ||
|||   JSON in, one JSON reply line out. Started ONLY by     ||
|||   `--qa` / `OPENRGB2_QA` — the app never opens a port   ||
|||   on its own, and the listener binds 127.0.0.1 so the   ||
|||   surface cannot leave the machine. main.cpp REFUSES it ||
|||   in an elevated or --managed UI (LaunchHardening.h):   ||
|||   medium-integrity code must never drive an admin UI.   ||
|||                                                           ||
|||   Ops: ping, state, tree, find, click, clickxy, set,    ||
|||   get, geom, type, key, scroll, shot, wait, eval.       ||
|||   Selectors resolve items by objectName ("navLighting"  ||
|||   or "name:navLighting"), visible text ("text:Save"),   ||
|||   QML/C++ type ("type:AppSlider"), or child-index path  ||
|||   ("path:0.2.1"). Clicking synthesizes real mouse/      ||
|||   wheel/key events through the QQuickWindow delivery    ||
|||   path, so MouseAreas, Controls handlers, focus and     ||
|||   shortcuts all behave as they do for a user.           ||
|||                                                           ||
|||   Hardening: loopback alone does not stop other local   ||
|||   processes or a browser's cross-protocol POST, so      ||
|||   every request must carry "token" — a per-launch      ||
|||   random secret (Token(); main.cpp writes it to a      ||
|||   per-user file and stderr). A wrong/missing token, a  ||
|||   malformed line, or an over-long line gets one error  ||
|||   reply and the connection is CLOSED. `shot` writes    ||
|||   relative paths under ShotDir() and never overwrites  ||
|||   an existing file outside it.                          ||
|||                                                           ||
|||   SPDX-License-Identifier: GPL-2.0-or-later               ||
||\*---------------------------------------------------------*/

#pragma once

#include <QByteArray>
#include <QHash>
#include <QJsonObject>
#include <QObject>
#include <QPointer>
#include <QString>

#include <memory>

class QQmlApplicationEngine;
class QQuickItem;
class QQuickWindow;
class QTcpServer;
class QTcpSocket;

namespace openrgb2
{
namespace ui
{

class QaServer : public QObject
{
    Q_OBJECT
public:
    explicit QaServer(QQmlApplicationEngine& engine,
                      QObject* parent = nullptr);

    /* Binds 127.0.0.1:port (0 = ephemeral). Fails rather than
       listen on a non-loopback interface. */
    bool    Start(quint16 port, QString& err);
    quint16 Port() const;

    /* The per-launch secret every request must carry as
       "token" (64 hex chars, generated in the constructor). */
    QString Token() const { return token_; }

    /* Writes Token() to `path` (parent created; owner-only
       permissions where the platform honours them). */
    bool    WriteTokenFile(const QString& path, QString& err) const;
    /* The per-user token file for `port` -
       <GenericDataLocation>/OpenRGB2/qa/qa-<port>.token
       (%LOCALAPPDATA% on Windows). tools/qa/qa.py reads it. */
    static QString TokenFilePath(quint16 port);

    /* Where `shot` writes relative paths (created on demand):
       $OPENRGB2_QA_OUTDIR, else <temp>/openrgb2-qa. */
    static QString ShotDir();

    /* Default listen port (`--qa` without a value). */
    static constexpr quint16 kDefaultPort = 5583;
    /* A request line longer than this (no newline yet) closes
       the connection — the buffer is never unbounded. */
    static constexpr int kMaxLineBytes = 1 << 20;

private:
    void        OnNewConnection();
    void        OnReadyRead(QTcpSocket* sock);
    void        OnDisconnected(QTcpSocket* sock);

    /* Dispatch delivers real input events — an op can tear
       down the very socket that carried the request (e.g. a
       click that quits, a wait whose client hung up). The
       client pointer threads through so OpWait can listen for
       its disconnect. */
    QJsonObject Dispatch(const QJsonObject& req,
                         QTcpSocket* client);

    /*---- Item resolution ------------------------------------*/
    QQuickWindow*       Window() const;
    QList<QQuickItem*>  AllItems() const;
    QList<QQuickItem*>  Resolve(const QJsonValue& sel,
                                QString& err) const;
    QQuickItem*         ResolveOne(const QJsonValue& sel,
                                   QString& err) const;

    static QString TypeName(const QObject* obj);
    static QString ItemText(const QQuickItem* item);
    static bool    IsInteractive(const QQuickItem* item);
    QJsonObject    ItemInfo(QQuickItem* item, int idx) const;
    QJsonArray     MatchList(const QList<QQuickItem*>& items) const;

    /*---- Ops -------------------------------------------------*/
    QJsonObject OpTree(const QJsonObject& req);
    QJsonObject OpFind(const QJsonObject& req);
    QJsonObject OpClick(const QJsonObject& req);
    QJsonObject OpSet(const QJsonObject& req);
    QJsonObject OpGet(const QJsonObject& req);
    QJsonObject OpGeom(const QJsonObject& req);
    QJsonObject OpType(const QJsonObject& req);
    QJsonObject OpKey(const QJsonObject& req);
    QJsonObject OpScroll(const QJsonObject& req);
    QJsonObject OpShot(const QJsonObject& req);
    QJsonObject OpWait(const QJsonObject& req, QTcpSocket* client);
    QJsonObject OpEval(const QJsonObject& req);
    QJsonObject OpState();

    bool        ClickAt(const QPointF& scene_pos, int button,
                        bool dbl);
    QPointF     ItemCenter(QQuickItem* item) const;

    static QJsonObject Ok(QJsonValue result = QJsonValue());
    static QJsonObject Err(const QString& code,
                           const QString& message);

    /* Per-socket framing state. `dispatching` is the re-entry
       guard: an op that spins a nested event loop (wait, shot,
       synthesized input) can fire readyRead for the SAME socket -
       the nested call only appends to `acc`; the outer loop
       drains the queued lines in order after Dispatch returns. */
    struct ClientState
    {
        QByteArray acc;
        bool       dispatching = false;
        bool       closing     = false;
    };
    void        CloseAfterReply(QTcpSocket* sock);

    QQmlApplicationEngine& engine_;
    QTcpServer*            server_ = nullptr;
    QList<QPointer<QTcpSocket>> clients_;
    QHash<QTcpSocket*, std::shared_ptr<ClientState>> state_;
    QString                token_;
};

} /* namespace ui */
} /* namespace openrgb2 */
