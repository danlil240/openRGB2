/*---------------------------------------------------------*\
||| QaServer.h — openrgb2-ui QA automation socket           ||
|||                                                           ||
|||   Dev-only in-process harness: a loopback-only TCP      ||
|||   listener that exposes the live QML item tree to an    ||
|||   external driver (tools/qa/qa.py). Line-delimited      ||
|||   JSON in, one JSON reply line out. Started ONLY by     ||
|||   `--qa` / `OPENRGB2_QA` — the app never opens a port   ||
|||   on its own, and the listener binds 127.0.0.1 so the   ||
|||   surface cannot leave the machine.                     ||
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
|||   SPDX-License-Identifier: GPL-2.0-or-later               ||
||\*---------------------------------------------------------*/

#pragma once

#include <QJsonObject>
#include <QObject>
#include <QPointer>

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

    /* Default listen port (`--qa` without a value). */
    static constexpr quint16 kDefaultPort = 5583;

private:
    void        OnNewConnection();
    void        OnReadyRead(QTcpSocket* sock);
    void        OnDisconnected(QTcpSocket* sock);

    QJsonObject Dispatch(const QJsonObject& req);

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
    QJsonObject OpWait(const QJsonObject& req);
    QJsonObject OpEval(const QJsonObject& req);
    QJsonObject OpState();

    bool        ClickAt(const QPointF& scene_pos, int button,
                        bool dbl);
    QPointF     ItemCenter(QQuickItem* item) const;

    static QJsonObject Ok(QJsonValue result = QJsonValue());
    static QJsonObject Err(const QString& code,
                           const QString& message);

    QQmlApplicationEngine& engine_;
    QTcpServer*            server_ = nullptr;
    QList<QPointer<QTcpSocket>> clients_;
};

} /* namespace ui */
} /* namespace openrgb2 */
