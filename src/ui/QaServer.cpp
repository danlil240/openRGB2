/*---------------------------------------------------------*\
||| QaServer.cpp — in-process QA automation socket          ||
|||                                                           ||
|||   See QaServer.h for the op catalogue and the security  ||
|||   posture (loopback only, explicit --qa opt-in).        ||
|||                                                           ||
|||   SPDX-License-Identifier: GPL-2.0-or-later               ||
||\*---------------------------------------------------------*/

#include "QaServer.h"

#include <QElapsedTimer>
#include <QEventLoop>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonValue>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QQmlContext>
#include <QQmlExpression>
#include <QQmlProperty>
#include <QQmlApplicationEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QRegularExpression>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QWheelEvent>

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRandomGenerator>
#include <QStandardPaths>
#include <QMetaProperty>
#include <QMetaType>
#include <QVariant>

namespace openrgb2
{
namespace ui
{

namespace
{

/* Text-bearing properties worth matching a "text:" selector
   against, in priority order. */
const char* kTextProps[] = { "text", "title", "label",
                             "placeholderText", "toolTip" };

QString StripQmlSuffix(const char* class_name)
{
    QString n = QString::fromLatin1(class_name);
    static const QRegularExpression kSuffix(
        QStringLiteral("_QML(TYPE)?_\\d+$"));
    n.remove(kSuffix);
    return n;
}

} /* anonymous namespace */

namespace
{
/* Running time independent of the first mismatch position. */
bool ConstantTimeEquals(const QByteArray& a, const QByteArray& b)
{
    if(a.size() != b.size())
    {
        return false;
    }
    unsigned char diff = 0;
    for(int i = 0; i < a.size(); ++i)
    {
        diff |= (unsigned char)(a[i] ^ b[i]);
    }
    return diff == 0;
}
} /* anonymous namespace */

QaServer::QaServer(QQmlApplicationEngine& engine, QObject* parent)
    : QObject(parent)
    , engine_(engine)
{
    /* 256-bit per-launch secret from the OS CSPRNG. */
    quint32 words[8];
    QRandomGenerator::system()->fillRange(words, 8);
    token_ = QString::fromLatin1(
        QByteArray(reinterpret_cast<const char*>(words),
                   (int)sizeof(words)).toHex());
}

QString QaServer::TokenFilePath(quint16 port)
{
    const QString base = QStandardPaths::writableLocation(
        QStandardPaths::GenericDataLocation);
    return QDir(base).filePath(
        QStringLiteral("OpenRGB2/qa/qa-%1.token").arg(port));
}

bool QaServer::WriteTokenFile(const QString& path, QString& err) const
{
    if(!QDir().mkpath(QFileInfo(path).absolutePath()))
    {
        err = QStringLiteral("cannot create %1")
                  .arg(QFileInfo(path).absolutePath());
        return false;
    }
    QFile f(path);
    if(!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
    {
        err = f.errorString();
        return false;
    }
    /* Owner-only on POSIX; on Windows the per-user profile
       directory's ACL (%LOCALAPPDATA%) is what keeps other
       users out. */
    f.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    f.write(token_.toLatin1());
    f.close();
    return true;
}

QString QaServer::ShotDir()
{
    const QString env = qEnvironmentVariable("OPENRGB2_QA_OUTDIR");
    return QDir::cleanPath(QFileInfo(
        env.isEmpty() ? QDir::temp().filePath(
                            QStringLiteral("openrgb2-qa"))
                      : env).absoluteFilePath());
}

bool QaServer::Start(quint16 port, QString& err)
{
    if(server_)
    {
        err = QStringLiteral("QA server already running");
        return false;
    }
    server_ = new QTcpServer(this);
    /* Loopback only — the QA surface must never be reachable
       off-machine. Binding failure is reported, never widened. */
    if(!server_->listen(QHostAddress::LocalHost, port))
    {
        err = server_->errorString();
        delete server_;
        server_ = nullptr;
        return false;
    }
    connect(server_, &QTcpServer::newConnection,
            this, &QaServer::OnNewConnection);
    return true;
}

quint16 QaServer::Port() const
{
    return server_ ? server_->serverPort() : 0;
}

/*---------------------------------------------------------*\
||| Connection plumbing — one JSON object per line         ||
||\*---------------------------------------------------------*/

void QaServer::OnNewConnection()
{
    while(QTcpSocket* sock = server_->nextPendingConnection())
    {
        clients_.append(sock);
        state_.insert(sock, std::make_shared<ClientState>());
        connect(sock, &QTcpSocket::readyRead,
                this, [this, sock] { OnReadyRead(sock); });
        connect(sock, &QTcpSocket::disconnected,
                this, [this, sock] { OnDisconnected(sock); });
    }
}

void QaServer::OnDisconnected(QTcpSocket* sock)
{
    clients_.removeAll(sock);
    state_.remove(sock);
    sock->deleteLater();
}

void QaServer::CloseAfterReply(QTcpSocket* sock)
{
    if(auto it = state_.find(sock); it != state_.end())
    {
        (*it)->closing = true;
        (*it)->acc.clear();
    }
    /* Graceful: the queued error reply is flushed first. */
    sock->disconnectFromHost();
}

void QaServer::OnReadyRead(QTcpSocket* sock)
{
    /* Per-socket buffered line framing: partial lines stay in
       the socket's ClientState until a '\n' completes a request.
       Dispatch() delivers REAL input events — a click/type op
       can close this very socket mid-line (the peer hung up,
       a quit shortcut, a handler that tears the UI down). The
       raw pointer is unusable past that boundary, so every
       use after Dispatch re-checks the QPointer guard; the
       state is held by shared_ptr so it outlives a disconnect
       inside the dispatch. */
    const QPointer<QTcpSocket> guard(sock);
    const auto sit = state_.constFind(sock);
    if(sit == state_.constEnd())
    {
        return;
    }
    const std::shared_ptr<ClientState> st = *sit;
    if(st->closing)
    {
        sock->readAll();               /* discard: connection ends */
        return;
    }
    st->acc += sock->readAll();
    if(st->dispatching)
    {
        /* Re-entered from a nested event loop inside Dispatch
           (wait/shot/input) — queue only; the outer call drains
           these lines in order once the current op returns. */
        if(st->acc.size() > kMaxLineBytes * 2)
        {
            CloseAfterReply(sock);
        }
        return;
    }

    /* Writes one reply line; false = stop processing (socket
       gone, or closed on purpose after this reply). */
    auto reply_and_maybe_close =
        [&](const QJsonObject& reply, bool close_after) -> bool
    {
        if(guard.isNull())
        {
            return false;
        }
        sock->write(QJsonDocument(reply).toJson(
                        QJsonDocument::Compact));
        sock->write("\n");
        sock->flush();
        if(guard.isNull())
        {
            return false;
        }
        if(close_after)
        {
            CloseAfterReply(sock);
            return false;
        }
        return true;
    };
    const QJsonObject too_long =
        Err(QStringLiteral("line_too_long"),
            QStringLiteral("request line exceeds %1 bytes")
                .arg(kMaxLineBytes));

    for(;;)
    {
        /* Bytes that arrived while an op ran a nested event loop:
           Qt does NOT re-emit readyRead for a socket whose
           readyRead handler is still on the stack, so pull them
           here or they would sit unread until the NEXT request
           (answering every later request one line late). */
        if(guard.isNull())
        {
            return;
        }
        st->acc += sock->readAll();
        const int nl = st->acc.indexOf('\n');
        if(nl < 0)
        {
            if(st->acc.size() > kMaxLineBytes)
            {
                reply_and_maybe_close(too_long, true);
            }
            return;
        }
        if(nl > kMaxLineBytes)
        {
            reply_and_maybe_close(too_long, true);
            return;
        }
        const QByteArray line = st->acc.left(nl).trimmed();
        st->acc.remove(0, nl + 1);
        if(line.isEmpty())
        {
            continue;
        }
        QJsonParseError perr;
        const QJsonDocument doc =
            QJsonDocument::fromJson(line, &perr);
        if(perr.error != QJsonParseError::NoError ||
           !doc.isObject())
        {
            /* Not our protocol (e.g. the request line of a
               browser's cross-protocol HTTP POST) — answer once
               and hang up; never keep parsing the stream. */
            reply_and_maybe_close(
                Err(QStringLiteral("malformed"),
                    QStringLiteral("request must be a JSON "
                                   "object on one line: %1")
                        .arg(perr.errorString())),
                true);
            return;
        }
        const QJsonObject req = doc.object();
        if(!ConstantTimeEquals(
               req.value(QStringLiteral("token")).toString()
                   .toLatin1(),
               token_.toLatin1()))
        {
            reply_and_maybe_close(
                Err(QStringLiteral("unauthorized"),
                    QStringLiteral("missing or wrong \"token\"")),
                true);
            return;
        }
        st->dispatching = true;
        const QJsonObject reply = Dispatch(req, sock);
        st->dispatching = false;
        /* The dispatch may have disconnected+deleted the
           client — writing to a dangling socket is the crash
           this guard exists for. */
        if(st->closing || !reply_and_maybe_close(reply, false))
        {
            return;
        }
    }
}

/*---------------------------------------------------------*\
||| Dispatch                                                ||
||\*---------------------------------------------------------*/

QJsonObject QaServer::Dispatch(const QJsonObject& req,
                               QTcpSocket* client)
{
    const QString op = req.value(QStringLiteral("op")).toString();
    if(op.isEmpty())
    {
        return Err(QStringLiteral("bad_request"),
                   QStringLiteral("missing \"op\""));
    }
    if(op == QLatin1String("ping"))
    {
        return Ok(QStringLiteral("pong"));
    }
    if(op == QLatin1String("state"))   return OpState();
    if(op == QLatin1String("tree"))    return OpTree(req);
    if(op == QLatin1String("find"))    return OpFind(req);
    if(op == QLatin1String("click") ||
       op == QLatin1String("clickxy")) return OpClick(req);
    if(op == QLatin1String("set"))     return OpSet(req);
    if(op == QLatin1String("get"))     return OpGet(req);
    if(op == QLatin1String("geom"))    return OpGeom(req);
    if(op == QLatin1String("type"))    return OpType(req);
    if(op == QLatin1String("key"))     return OpKey(req);
    if(op == QLatin1String("scroll"))  return OpScroll(req);
    if(op == QLatin1String("shot"))    return OpShot(req);
    if(op == QLatin1String("wait"))    return OpWait(req, client);
    if(op == QLatin1String("eval"))    return OpEval(req);
    return Err(QStringLiteral("unknown_op"),
               QStringLiteral("unknown op \"%1\"").arg(op));
}

QJsonObject QaServer::Ok(QJsonValue result)
{
    QJsonObject o;
    o.insert(QStringLiteral("ok"), true);
    if(!result.isUndefined())
    {
        o.insert(QStringLiteral("result"), result);
    }
    return o;
}

QJsonObject QaServer::Err(const QString& code, const QString& message)
{
    QJsonObject o;
    o.insert(QStringLiteral("ok"), false);
    o.insert(QStringLiteral("error"), code);
    o.insert(QStringLiteral("message"), message);
    return o;
}

/*---------------------------------------------------------*\
||| Item tree traversal                                     ||
||\*---------------------------------------------------------*/

QQuickWindow* QaServer::Window() const
{
    const QList<QObject*> roots = engine_.rootObjects();
    for(QObject* o : roots)
    {
        if(QQuickWindow* w = qobject_cast<QQuickWindow*>(o))
        {
            return w;
        }
    }
    return nullptr;
}

QList<QQuickItem*> QaServer::AllItems() const
{
    QList<QQuickItem*> out;
    QQuickWindow* w = Window();
    if(!w || !w->contentItem())
    {
        return out;
    }
    QList<QQuickItem*> stack{ w->contentItem() };
    while(!stack.isEmpty())
    {
        QQuickItem* it = stack.takeLast();
        out.append(it);
        stack.append(it->childItems());
    }
    return out;
}

QString QaServer::TypeName(const QObject* obj)
{
    return obj ? StripQmlSuffix(obj->metaObject()->className())
               : QString();
}

QString QaServer::ItemText(const QQuickItem* item)
{
    for(const char* p : kTextProps)
    {
        const QVariant v = item->property(p);
        if(v.isValid() && !v.toString().isEmpty())
        {
            return v.toString();
        }
    }
    /* Accessible.name is an ATTACHED property — invisible to
       QObject::property() — but many named-only controls use
       it as their label (Layout pickers, icon buttons). */
    const QVariant acc =
        QQmlProperty::read(
            const_cast<QQuickItem*>(item),
            QStringLiteral("Accessible.name"));
    if(acc.isValid())
    {
        return acc.toString();
    }
    return QString();
}

bool QaServer::IsInteractive(const QQuickItem* item)
{
    /* Walk the C++ superclass chain — a QML `Button {}` still
       descends from QQuickAbstractButton. Any ancestor in the
       interactive set counts. */
    static const char* kBase[] = {
        "QQuickAbstractButton", "QQuickButton", "QQuickCheckBox",
        "QQuickSlider", "QQuickComboBox", "QQuickTextInput",
        "QQuickTextEdit", "QQuickSpinBox", "QQuickSwitch",
        "QQuickItemDelegate", "QQuickTabButton",
        "QQuickRadioButton", "QQuickMouseArea", "QQuickFlickable",
        "QQuickScrollView", "QQuickSwipeView", "QQuickDial",
        "QQuickRangeSlider", "QQuickTumbler", "QQuickTextField",
        "QQuickTextArea", "QQuickPane"
    };
    const QMetaObject* mo = item->metaObject();
    while(mo)
    {
        const QString n = StripQmlSuffix(mo->className());
        for(const char* b : kBase)
        {
            if(n == QLatin1String(b))
            {
                return true;
            }
        }
        mo = mo->superClass();
    }
    return false;
}

QJsonObject QaServer::ItemInfo(QQuickItem* item, int idx) const
{
    QJsonObject o;
    o.insert(QStringLiteral("i"), idx);
    o.insert(QStringLiteral("name"), item->objectName());
    o.insert(QStringLiteral("type"), TypeName(item));
    const QString text = ItemText(item);
    if(!text.isEmpty())
    {
        o.insert(QStringLiteral("text"), text);
    }
    QQuickWindow* w = Window();
    if(w)
    {
        /* mapToScene applies the item transform (position,
           scale, rotation) — the axis-aligned scene bbox is
           the window rect the driver should click within. */
        const QRectF r =
            item->mapRectToScene(item->boundingRect());
        o.insert(QStringLiteral("rect"),
                 QJsonArray{ r.x(), r.y(), r.width(), r.height() });
    }
    o.insert(QStringLiteral("enabled"), item->isEnabled());
    o.insert(QStringLiteral("visible"), item->isVisible());
    o.insert(QStringLiteral("focus"), item->hasActiveFocus());
    if(QQuickItem* p = item->parentItem())
    {
        if(!p->objectName().isEmpty())
        {
            o.insert(QStringLiteral("parent"), p->objectName());
        }
    }
    return o;
}

QJsonArray QaServer::MatchList(const QList<QQuickItem*>& items) const
{
    QJsonArray a;
    int i = 0;
    for(QQuickItem* it : items)
    {
        a.append(ItemInfo(it, i++));
    }
    return a;
}

/*---------------------------------------------------------*\
||| Selector resolution                                     ||
||\*---------------------------------------------------------*/

QList<QQuickItem*> QaServer::Resolve(const QJsonValue& sel,
                                     QString& err) const
{
    QList<QQuickItem*> matches;
    QString name, text, type, path;
    bool want_visible = true;

    if(sel.isString())
    {
        const QString s = sel.toString();
        const int colon = s.indexOf(QLatin1Char(':'));
        const QString prefix =
            colon > 0 ? s.left(colon) : QString();
        const QString value =
            colon > 0 ? s.mid(colon + 1) : s;
        if(prefix == QLatin1String("name") || prefix.isEmpty())
        {
            name = value;
        }
        else if(prefix == QLatin1String("text"))  { text = value; }
        else if(prefix == QLatin1String("type"))  { type = value; }
        else if(prefix == QLatin1String("path"))  { path = value; }
        else { name = s; }
    }
    else if(sel.isObject())
    {
        const QJsonObject o = sel.toObject();
        name = o.value(QStringLiteral("name")).toString();
        text = o.value(QStringLiteral("text")).toString();
        type = o.value(QStringLiteral("type")).toString();
        path = o.value(QStringLiteral("path")).toString();
        if(o.contains(QStringLiteral("visible")))
        {
            want_visible =
                o.value(QStringLiteral("visible")).toBool();
        }
    }
    else
    {
        err = QStringLiteral("selector must be a string or object");
        return matches;
    }

    if(!path.isEmpty())
    {
        /* Deterministic child-index path from contentItem. */
        QQuickItem* cur = Window() ? Window()->contentItem()
                                   : nullptr;
        const QStringList hops = path.split(QLatin1Char('.'));
        for(const QString& h : hops)
        {
            bool ok_num = false;
            const int idx = h.toInt(&ok_num);
            if(!cur || !ok_num || idx < 0 ||
               idx >= cur->childItems().size())
            {
                cur = nullptr;
                break;
            }
            cur = cur->childItems().at(idx);
        }
        if(cur)
        {
            matches.append(cur);
        }
        else
        {
            err = QStringLiteral("path %1 does not resolve").arg(path);
        }
        return matches;
    }

    for(QQuickItem* it : AllItems())
    {
        if(want_visible && !it->isVisible())
        {
            continue;
        }
        if(!name.isEmpty() && it->objectName() != name)
        {
            continue;
        }
        if(!type.isEmpty())
        {
            /* Match the QML type or any C++ superclass name,
               with and without the "QQuick" prefix. */
            const QString want =
                type.startsWith(QLatin1String("QQuick"))
                    ? type : QStringLiteral("QQuick") + type;
            bool tmatch = false;
            const QMetaObject* mo = it->metaObject();
            while(mo && !tmatch)
            {
                const QString n = StripQmlSuffix(mo->className());
                if(n.compare(type, Qt::CaseInsensitive) == 0 ||
                   n.compare(want, Qt::CaseInsensitive) == 0)
                {
                    tmatch = true;
                }
                mo = mo->superClass();
            }
            if(!tmatch)
            {
                continue;
            }
        }
        if(!text.isEmpty())
        {
            const QString it_text = ItemText(it);
            if(it_text.isEmpty())
            {
                continue;
            }
            /* Exact match wins; substring only when nothing
               matches exactly (handled after the loop). */
            if(it_text.compare(text, Qt::CaseInsensitive) != 0)
            {
                continue;
            }
        }
        matches.append(it);
    }

    /* Substring fallback for text selectors. */
    if(matches.isEmpty() && !text.isEmpty())
    {
        for(QQuickItem* it : AllItems())
        {
            if(want_visible && !it->isVisible())
            {
                continue;
            }
            if(ItemText(it).contains(text, Qt::CaseInsensitive))
            {
                matches.append(it);
            }
        }
    }

    if(matches.isEmpty() && err.isEmpty())
    {
        err = QStringLiteral("no item matches selector");
    }
    return matches;
}

QQuickItem* QaServer::ResolveOne(const QJsonValue& sel,
                                 QString& err) const
{
    int index = 0;
    if(sel.isObject())
    {
        index = sel.toObject()
                    .value(QStringLiteral("index")).toInt(0);
    }
    const QList<QQuickItem*> m = Resolve(sel, err);
    if(m.isEmpty())
    {
        return nullptr;
    }
    if(index < 0 || index >= m.size())
    {
        err = QStringLiteral("%1 items match; pass index 0..%2")
                  .arg(m.size()).arg(m.size() - 1);
        return nullptr;
    }
    return m.at(index);
}

QPointF QaServer::ItemCenter(QQuickItem* item) const
{
    return item->mapToScene(
        item->boundingRect().center());
}

/*---------------------------------------------------------*\
||| Input synthesis — real events through the window       ||
||\*---------------------------------------------------------*/

bool QaServer::ClickAt(const QPointF& scene_pos, int button,
                       bool dbl)
{
    QPointer<QQuickWindow> w(Window());
    if(w.isNull())
    {
        return false;
    }
    const Qt::MouseButton b =
        static_cast<Qt::MouseButton>(button);
    const QPointF local = scene_pos;
    const QPoint  global =
        w->mapToGlobal(local.toPoint());

    /* Every sendEvent runs QML handlers — a press can close
       the window (quit shortcut, dialog teardown). The
       QPointer re-check keeps each subsequent delivery off a
       dangling window. */
    auto send = [&](QEvent::Type t, Qt::MouseButtons buttons) {
        if(w.isNull())
        {
            return;
        }
        QMouseEvent ev(t, local, scene_pos, global, b, buttons,
                       Qt::NoModifier);
        QCoreApplication::sendEvent(w, &ev);
    };

    QMouseEvent move(QEvent::MouseMove, local, scene_pos, global,
                     Qt::NoButton, Qt::NoButton, Qt::NoModifier);
    QCoreApplication::sendEvent(w, &move);
    send(QEvent::MouseButtonPress, b);
    send(QEvent::MouseButtonRelease, Qt::NoButton);
    if(dbl)
    {
        send(QEvent::MouseButtonDblClick, b);
        send(QEvent::MouseButtonRelease, Qt::NoButton);
    }
    return true;
}

/*---------------------------------------------------------*\
||| Ops                                                     ||
||\*---------------------------------------------------------*/

QJsonObject QaServer::OpTree(const QJsonObject& req)
{
    const bool all =
        req.value(QStringLiteral("all")).toBool(false);
    QList<QQuickItem*> items;
    /* Optional "under" selector scopes the dump to one
       subtree — handy for delegate rows inside a card. */
    if(req.contains(QStringLiteral("under")))
    {
        QString err;
        QQuickItem* root =
            ResolveOne(req.value(QStringLiteral("under")), err);
        if(!root)
        {
            return Err(QStringLiteral("not_found"), err);
        }
        QList<QQuickItem*> stack{ root };
        while(!stack.isEmpty())
        {
            QQuickItem* it = stack.takeLast();
            items.append(it);
            stack.append(it->childItems());
        }
    }
    else
    {
        items = AllItems();
    }
    QJsonArray out;
    int i = 0;
    for(QQuickItem* it : items)
    {
        if(!all && !it->isVisible())
        {
            continue;
        }
        const bool interesting =
            !it->objectName().isEmpty() || IsInteractive(it) ||
            !ItemText(it).isEmpty();
        if(!all && !interesting)
        {
            continue;
        }
        out.append(ItemInfo(it, i++));
    }
    QJsonObject r;
    r.insert(QStringLiteral("count"), out.size());
    r.insert(QStringLiteral("items"), out);
    return Ok(r);
}

QJsonObject QaServer::OpFind(const QJsonObject& req)
{
    QString err;
    const QList<QQuickItem*> m =
        Resolve(req.value(QStringLiteral("sel")), err);
    if(m.isEmpty())
    {
        return Err(QStringLiteral("not_found"), err);
    }
    return Ok(MatchList(m));
}

QJsonObject QaServer::OpClick(const QJsonObject& req)
{
    QString err;
    QPointF pos;
    const bool dbl =
        req.value(QStringLiteral("double")).toBool(false);
    const QString bname =
        req.value(QStringLiteral("button"))
            .toString(QStringLiteral("left"));
    const int button =
        bname == QLatin1String("right")  ? Qt::RightButton :
        bname == QLatin1String("middle") ? Qt::MiddleButton :
                                           Qt::LeftButton;

    if(req.contains(QStringLiteral("x")))
    {
        pos = QPointF(req.value(QStringLiteral("x")).toDouble(),
                      req.value(QStringLiteral("y")).toDouble());
    }
    else
    {
        QQuickItem* it =
            ResolveOne(req.value(QStringLiteral("sel")), err);
        if(!it)
        {
            QJsonObject r =
                Err(QStringLiteral("not_found"), err);
            const QList<QQuickItem*> m =
                Resolve(req.value(QStringLiteral("sel")), err);
            if(!m.isEmpty())
            {
                r.insert(QStringLiteral("matches"), MatchList(m));
            }
            return r;
        }
        pos = ItemCenter(it);
    }
    if(!ClickAt(pos, button, dbl))
    {
        return Err(QStringLiteral("no_window"),
                   QStringLiteral("no QQuickWindow"));
    }
    QJsonObject r;
    r.insert(QStringLiteral("x"), pos.x());
    r.insert(QStringLiteral("y"), pos.y());
    return Ok(r);
}

QJsonObject QaServer::OpSet(const QJsonObject& req)
{
    QString err;
    QQuickItem* it =
        ResolveOne(req.value(QStringLiteral("sel")), err);
    if(!it)
    {
        return Err(QStringLiteral("not_found"), err);
    }
    const QString prop =
        req.value(QStringLiteral("prop")).toString();
    if(prop.isEmpty())
    {
        return Err(QStringLiteral("bad_request"),
                   QStringLiteral("missing \"prop\""));
    }
    const QJsonValue val = req.value(QStringLiteral("value"));
    QVariant v = val.toVariant();
    /* Convert through the meta-property's declared type when
       one exists (JSON numbers arrive as double but `value`
       on a Slider, `checked`, `currentIndex`, `text` need
       their own types). Dynamic QML properties accept the
       QVariant as-is. */
    QPointer<QQuickItem> guard(it);
    const QMetaObject* mo = it->metaObject();
    const int pi = mo->indexOfProperty(prop.toUtf8());
    if(pi >= 0)
    {
        const QMetaProperty mp = mo->property(pi);
        if(v.isValid() && v.userType() != mp.userType() &&
           !v.convert(QMetaType(mp.userType())))
        {
            return Err(QStringLiteral("bad_value"),
                       QStringLiteral("cannot convert value to "
                                      "%1").arg(mp.typeName()));
        }
    }
    if(!it->setProperty(prop.toUtf8(), v))
    {
        return Err(QStringLiteral("set_failed"),
                   QStringLiteral("property \"%1\" rejected "
                                  "(read-only or missing)")
                       .arg(prop));
    }
    /* setProperty runs NOTIFY handlers + bindings — a write
       can legitimately destroy the item it landed on. */
    if(guard.isNull())
    {
        QJsonObject r;
        r.insert(QStringLiteral("prop"), prop);
        r.insert(QStringLiteral("destroyed"), true);
        return Ok(r);
    }
    QJsonObject r;
    r.insert(QStringLiteral("prop"), prop);
    r.insert(QStringLiteral("value"),
             QJsonValue::fromVariant(it->property(prop.toUtf8())));
    return Ok(r);
}

QJsonObject QaServer::OpGet(const QJsonObject& req)
{
    QString err;
    QQuickItem* it =
        ResolveOne(req.value(QStringLiteral("sel")), err);
    if(!it)
    {
        return Err(QStringLiteral("not_found"), err);
    }
    const QString prop =
        req.value(QStringLiteral("prop")).toString();
    const QVariant v =
        prop.isEmpty()
            ? QVariant() : it->property(prop.toUtf8());
    if(!prop.isEmpty() && !v.isValid())
    {
        return Err(QStringLiteral("bad_prop"),
                   QStringLiteral("no property \"%1\"").arg(prop));
    }
    QJsonObject r = ItemInfo(it, 0);
    if(!prop.isEmpty())
    {
        r.insert(QStringLiteral("prop"), prop);
        r.insert(QStringLiteral("value"), QJsonValue::fromVariant(v));
    }
    return Ok(r);
}

QJsonObject QaServer::OpGeom(const QJsonObject& req)
{
    QString err;
    QQuickItem* it =
        ResolveOne(req.value(QStringLiteral("sel")), err);
    if(!it)
    {
        return Err(QStringLiteral("not_found"), err);
    }
    QJsonObject r = ItemInfo(it, 0);
    r.insert(QStringLiteral("implicitWidth"),
             it->implicitWidth());
    r.insert(QStringLiteral("implicitHeight"),
             it->implicitHeight());
    r.insert(QStringLiteral("width"), it->width());
    r.insert(QStringLiteral("height"), it->height());
    r.insert(QStringLiteral("clip"), it->clip());
    QJsonArray chain;
    for(QQuickItem* p = it->parentItem(); p; p = p->parentItem())
    {
        chain.append(QStringLiteral("%1(%2)")
            .arg(p->objectName().isEmpty()
                     ? TypeName(p) : p->objectName(),
                 TypeName(p)));
    }
    r.insert(QStringLiteral("ancestors"), chain);
    return Ok(r);
}

QJsonObject QaServer::OpType(const QJsonObject& req)
{
    QString err;
    QPointer<QQuickItem> it(
        ResolveOne(req.value(QStringLiteral("sel")), err));
    if(it.isNull())
    {
        return Err(QStringLiteral("not_found"), err);
    }
    if(req.value(QStringLiteral("clear")).toBool(false))
    {
        /* Ctrl+A + Delete through the key path, so cleared
           signal handlers fire exactly like user input. The
           delete can take the item (or the window) with it —
           re-check both before the next delivery. */
        it->forceActiveFocus(Qt::MouseFocusReason);
        if(QQuickWindow* w = Window())
        {
            QKeyEvent sel_all(QEvent::KeyPress, Qt::Key_A,
                              Qt::ControlModifier);
            QCoreApplication::sendEvent(w, &sel_all);
            QKeyEvent del(QEvent::KeyPress, Qt::Key_Delete,
                          Qt::NoModifier);
            QCoreApplication::sendEvent(w, &del);
        }
        if(it.isNull())
        {
            QJsonObject r;
            r.insert(QStringLiteral("sent"), 0);
            r.insert(QStringLiteral("destroyed"), true);
            return Ok(r);
        }
    }
    it->forceActiveFocus(Qt::MouseFocusReason);
    const QString text =
        req.value(QStringLiteral("text")).toString();
    int sent = 0;
    for(const QChar ch : text)
    {
        QQuickWindow* w = Window();
        if(w == nullptr || it.isNull())
        {
            break;   /* the previous keystroke's handler tore
                        down the item or its window */
        }
        /* The event's text field carries the literal char —
           that is what TextInput/TextEdit consume; key() only
           needs to be a plausible Qt::Key for the char. */
        const int key = ch.isLetterOrNumber()
                            ? ch.toUpper().unicode()
                            : ch.unicode();
        QKeyEvent press(QEvent::KeyPress, key, Qt::NoModifier,
                        QString(ch));
        QCoreApplication::sendEvent(w, &press);
        if(it.isNull())
        {
            ++sent;
            break;
        }
        if(QQuickWindow* rw = Window())
        {
            QKeyEvent release(QEvent::KeyRelease, key,
                              Qt::NoModifier, QString(ch));
            QCoreApplication::sendEvent(rw, &release);
        }
        ++sent;
        if(it.isNull())
        {
            break;
        }
    }
    QJsonObject r;
    r.insert(QStringLiteral("sent"), sent);
    if(it.isNull())
    {
        /* Honest reply: the read-back below would dereference
           a dead item. The script can see the truncation via
           "sent" + this flag. */
        r.insert(QStringLiteral("destroyed"), true);
    }
    else
    {
        r.insert(QStringLiteral("text"),
                 it->property("text").toString());
    }
    return Ok(r);
}

QJsonObject QaServer::OpKey(const QJsonObject& req)
{
    const QString k =
        req.value(QStringLiteral("key")).toString();
    if(k.isEmpty())
    {
        return Err(QStringLiteral("bad_request"),
                   QStringLiteral("missing \"key\""));
    }
    /* "Ctrl+Shift+Z", "Tab", "F2", or a single char.
       Explicit parsing — deterministic, no QKeySequence
       locale dependence. */
    const QStringList parts =
        k.split(QLatin1Char('+'), Qt::SkipEmptyParts);
    Qt::KeyboardModifiers mods = Qt::NoModifier;
    int key = 0;
    for(const QString& part : parts)
    {
        const QString p = part.trimmed();
        const QString lp = p.toLower();
        if(lp == QLatin1String("ctrl") ||
           lp == QLatin1String("control"))
        {
            mods |= Qt::ControlModifier; continue;
        }
        if(lp == QLatin1String("shift"))
        {
            mods |= Qt::ShiftModifier; continue;
        }
        if(lp == QLatin1String("alt"))
        {
            mods |= Qt::AltModifier; continue;
        }
        if(lp == QLatin1String("meta") || lp == QLatin1String("win"))
        {
            mods |= Qt::MetaModifier; continue;
        }
        static const struct { const char* name; int key; }
        kNamed[] = {
            { "tab",        Qt::Key_Tab },
            { "backtab",    Qt::Key_Backtab },
            { "backspace",  Qt::Key_Backspace },
            { "return",     Qt::Key_Return },
            { "enter",      Qt::Key_Enter },
            { "escape",     Qt::Key_Escape },
            { "esc",        Qt::Key_Escape },
            { "space",      Qt::Key_Space },
            { "delete",     Qt::Key_Delete },
            { "insert",     Qt::Key_Insert },
            { "home",       Qt::Key_Home },
            { "end",        Qt::Key_End },
            { "pageup",     Qt::Key_PageUp },
            { "pagedown",   Qt::Key_PageDown },
            { "left",       Qt::Key_Left },
            { "right",      Qt::Key_Right },
            { "up",         Qt::Key_Up },
            { "down",       Qt::Key_Down },
        };
        bool matched = false;
        for(const auto& nm : kNamed)
        {
            if(lp == QLatin1String(nm.name))
            {
                key = nm.key; matched = true; break;
            }
        }
        if(!matched && lp.size() == 2 && lp.at(0) == 'f')
        {
            const int fn = lp.at(1).digitValue();
            if(fn >= 1 && fn <= 9)
            {
                key = Qt::Key_F1 + fn - 1; matched = true;
            }
        }
        if(!matched && lp.size() == 3 &&
           lp.startsWith(QLatin1String("f1")))
        {
            /* f10/f11/f12 */
            const int fn = lp.mid(1).toInt();
            if(fn >= 10 && fn <= 12)
            {
                key = Qt::Key_F1 + fn - 1; matched = true;
            }
        }
        if(!matched && p.size() == 1)
        {
            key = p.at(0).toUpper().unicode();
            matched = true;
        }
        if(!matched)
        {
            return Err(QStringLiteral("bad_key"),
                       QStringLiteral("unrecognized key part "
                                      "\"%1\"").arg(p));
        }
    }
    if(!key)
    {
        return Err(QStringLiteral("bad_key"),
                   QStringLiteral("unrecognized key \"%1\"")
                       .arg(k));
    }
    QPointer<QQuickWindow> w(Window());
    if(w.isNull())
    {
        return Err(QStringLiteral("no_window"),
                   QStringLiteral("no QQuickWindow"));
    }
    /* Route through the application-level notify, not a bare
       sendEvent to the window: the ShortcutOverride pass and
       the QShortcutMap match live in QGuiApplication::notify —
       sendEvent would skip window Shortcuts entirely (Ctrl+S,
       Ctrl+1..5 are the real keyboard-access surface). The
       press itself can close the window — guard before the
       release. */
    QKeyEvent press(QEvent::KeyPress, key, mods);
    QCoreApplication::instance()->notify(w, &press);
    if(w.isNull())
    {
        return Err(QStringLiteral("no_window"),
                   QStringLiteral("window closed during key "
                                  "delivery"));
    }
    QKeyEvent release(QEvent::KeyRelease, key, mods);
    QCoreApplication::instance()->notify(w, &release);
    QJsonObject r;
    r.insert(QStringLiteral("key"), key);
    r.insert(QStringLiteral("mods"), int(mods));
    return Ok(r);
}

QJsonObject QaServer::OpScroll(const QJsonObject& req)
{
    QString err;
    QPointF pos;
    if(req.contains(QStringLiteral("x")))
    {
        pos = QPointF(req.value(QStringLiteral("x")).toDouble(),
                      req.value(QStringLiteral("y")).toDouble());
    }
    else
    {
        QQuickItem* it =
            ResolveOne(req.value(QStringLiteral("sel")), err);
        if(!it)
        {
            return Err(QStringLiteral("not_found"), err);
        }
        pos = ItemCenter(it);
    }
    QPointer<QQuickWindow> w(Window());
    if(w.isNull())
    {
        return Err(QStringLiteral("no_window"),
                   QStringLiteral("no QQuickWindow"));
    }
    const int steps =
        req.value(QStringLiteral("dy")).toInt(-3);
    const int steps_x =
        req.value(QStringLiteral("dx")).toInt(0);
    QWheelEvent ev(pos, w->mapToGlobal(pos.toPoint()),
                   QPoint(0, 0), QPoint(steps_x * 120,
                                        steps * 120),
                   Qt::NoButton, Qt::NoModifier,
                   Qt::NoScrollPhase, false);
    QCoreApplication::sendEvent(w, &ev);
    QJsonObject r;
    r.insert(QStringLiteral("x"), pos.x());
    r.insert(QStringLiteral("y"), pos.y());
    return Ok(r);
}

QJsonObject QaServer::OpShot(const QJsonObject& req)
{
    QPointer<QQuickWindow> w(Window());
    if(w.isNull())
    {
        return Err(QStringLiteral("no_window"),
                   QStringLiteral("no QQuickWindow"));
    }
    /* Pump one frame so deferred polish/layout settles before
       the grab — the pump itself can tear the window down
       (a queued close), so re-check before touching it. */
    QCoreApplication::processEvents();
    if(w.isNull())
    {
        return Err(QStringLiteral("no_window"),
                   QStringLiteral("window closed during "
                                  "deferred processing"));
    }
    const QImage img = w->grabWindow();
    if(img.isNull())
    {
        return Err(QStringLiteral("grab_failed"),
                   QStringLiteral("grabWindow returned null "
                                  "(window hidden?)"));
    }
    /* Output is confined: relative paths (and the default name)
       land under ShotDir(); an absolute path inside ShotDir()
       may overwrite; anything else must be a NEW .png file —
       the QA socket never clobbers an existing file elsewhere. */
    const QString shot_dir = ShotDir();
    QString path =
        req.value(QStringLiteral("path")).toString();
    if(path.isEmpty())
    {
        path = QStringLiteral("openrgb2-qa-%1.png")
                   .arg(QDateTime::currentMSecsSinceEpoch());
    }
    if(QFileInfo(path).isRelative())
    {
        path = QDir(shot_dir).filePath(path);
    }
    path = QDir::cleanPath(QFileInfo(path).absoluteFilePath());
    const bool inside =
        path.startsWith(shot_dir + QLatin1Char('/'),
                        Qt::CaseInsensitive);
    if(!path.endsWith(QLatin1String(".png"), Qt::CaseInsensitive))
    {
        return Err(QStringLiteral("bad_path"),
                   QStringLiteral("shot path must end in .png"));
    }
    if(!inside && QFileInfo::exists(path))
    {
        return Err(QStringLiteral("bad_path"),
                   QStringLiteral("refusing to overwrite %1 outside "
                                  "the QA output directory %2")
                       .arg(path, shot_dir));
    }
    if(inside)
    {
        QDir().mkpath(QFileInfo(path).absolutePath());
    }
    if(!img.save(path, "PNG"))
    {
        return Err(QStringLiteral("save_failed"),
                   QStringLiteral("cannot write %1").arg(path));
    }
    QJsonObject r;
    r.insert(QStringLiteral("path"), path);
    r.insert(QStringLiteral("width"), img.width());
    r.insert(QStringLiteral("height"), img.height());
    return Ok(r);
}

QJsonObject QaServer::OpWait(const QJsonObject& req,
                             QTcpSocket* client)
{
    /* Nested event loop — the GUI keeps processing while the
       reply is pending. Bounded by timeout_ms (cap 30 s). A
       client that hangs up mid-wait ends the loop too —
       OnReadyRead must never write the reply to a socket it
       no longer owns. */
    const int timeout = qBound(
        0, req.value(QStringLiteral("timeout_ms")).toInt(5000),
        30000);
    const QString prop =
        req.value(QStringLiteral("prop")).toString();
    const QJsonValue want =
        req.value(QStringLiteral("value"));
    QElapsedTimer timer;
    timer.start();
    QEventLoop loop;
    QTimer poll;
    poll.setInterval(25);
    QJsonObject result;
    bool done = false;
    bool client_gone = false;
    QMetaObject::Connection dc;
    if(client != nullptr)
    {
        /* The lambda only flips stack flags — it never
           dereferences `client`, so the connection dying with
           the socket stays safe. */
        dc = QObject::connect(client, &QTcpSocket::disconnected,
                              [&] {
                                  client_gone = true;
                                  done        = true;
                                  loop.quit();
                              });
    }
    QObject::connect(&poll, &QTimer::timeout, [&] {
        QString err;
        QQuickItem* it =
            ResolveOne(req.value(QStringLiteral("sel")), err);
        bool match = false;
        QString detail;
        if(it)
        {
            if(prop.isEmpty())
            {
                match = true;
            }
            else
            {
                const QVariant v = it->property(prop.toUtf8());
                match = v.isValid() &&
                    (QJsonValue::fromVariant(v) == want);
                detail = QStringLiteral("%1=%2")
                    .arg(prop,
                         QJsonValue::fromVariant(v)
                             .toVariant().toString());
            }
        }
        else
        {
            detail = err;
        }
        if(match || timer.elapsed() >= timeout)
        {
            result = match ? Ok(ItemInfo(it, 0))
                           : Err(QStringLiteral("timeout"),
                                 QStringLiteral("wait timed out "
                                                "(%1)").arg(detail));
            done = true;
            loop.quit();
        }
    });
    poll.start();
    if(!done)
    {
        loop.exec();
    }
    if(client_gone)
    {
        /* The reply has nowhere to go — report it honestly so
           OnReadyRead's guard can drop the write. */
        return Err(QStringLiteral("client_disconnected"),
                   QStringLiteral("client closed the socket "
                                  "during wait"));
    }
    if(dc)
    {
        QObject::disconnect(dc);
    }
    return result;
}

QJsonObject QaServer::OpEval(const QJsonObject& req)
{
    const QString code =
        req.value(QStringLiteral("code")).toString();
    if(code.isEmpty())
    {
        return Err(QStringLiteral("bad_request"),
                   QStringLiteral("missing \"code\""));
    }
    QQuickWindow* w = Window();
    if(!w)
    {
        return Err(QStringLiteral("no_window"),
                   QStringLiteral("no QQuickWindow"));
    }
    /* `this` = the root ApplicationWindow by default; when the
       request carries "sel", `this` is the resolved item — the
       QA script can then call item-local test seams like
       StudioViewport.screenPosOf() for real 3D picking.
       Context props (bridge, workspace, viewport, uiPrefs) stay
       visible either way. Escape hatch — trust boundary is the
       local user who passed --qa. */
    QObject* scope = w;
    if(req.contains(QStringLiteral("sel")))
    {
        QString err;
        QQuickItem* it = ResolveOne(req.value("sel"), err);
        if(!it)
        {
            return Err(QStringLiteral("not_found"), err);
        }
        scope = it;
    }
    /* The evaluated JS is arbitrary — `this.destroy()` or a
       parent teardown can delete the scope mid-evaluation.
       QPointer lets the reply path report it rather than
       build on a dangling object. */
    const QPointer<QObject> scope_guard(scope);
    QQmlExpression expr(engine_.rootContext(), scope, code);
    bool undefined = false;
    const QVariant v = expr.evaluate(&undefined);
    if(expr.hasError())
    {
        return Err(QStringLiteral("eval_error"),
                   expr.error().toString());
    }
    if(scope_guard.isNull())
    {
        return Err(QStringLiteral("scope_destroyed"),
                   QStringLiteral("eval destroyed its scope "
                                  "item"));
    }
    return Ok(undefined ? QJsonValue()
                        : QJsonValue::fromVariant(v));
}

QJsonObject QaServer::OpState()
{
    QQuickWindow* w = Window();
    if(!w)
    {
        return Err(QStringLiteral("no_window"),
                   QStringLiteral("no QQuickWindow"));
    }
    QJsonObject r;
    r.insert(QStringLiteral("title"), w->title());
    r.insert(QStringLiteral("width"), w->width());
    r.insert(QStringLiteral("height"), w->height());
    r.insert(QStringLiteral("visible"), w->isVisible());
    /* Root ApplicationWindow state (currentView, connState)
       is what most QA checks poll. */
    if(QObject* root = engine_.rootObjects().value(0))
    {
        const QVariant cv = root->property("currentView");
        if(cv.isValid())
        {
            r.insert(QStringLiteral("currentView"),
                     cv.toInt());
        }
        const QVariant cs = root->property("connState");
        if(cs.isValid())
        {
            r.insert(QStringLiteral("connState"),
                     cs.toString());
        }
    }
    if(QQuickItem* f = w->activeFocusItem())
    {
        QJsonObject fo;
        fo.insert(QStringLiteral("name"), f->objectName());
        fo.insert(QStringLiteral("type"), TypeName(f));
        r.insert(QStringLiteral("focus"), fo);
    }
    r.insert(QStringLiteral("itemCount"), AllItems().size());
    return Ok(r);
}

} /* namespace ui */
} /* namespace openrgb2 */
