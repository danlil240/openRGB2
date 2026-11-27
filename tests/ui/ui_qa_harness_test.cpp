// SPDX-License-Identifier: GPL-2.0-or-later
//
// QA-harness wire test — the REAL shell (Main.qml) headless,
// driven through QaServer's loopback socket exactly the way
// tools/qa/qa.py drives a live window: JSON lines in, JSON
// replies out. Asserts the op catalogue end-to-end:
//
//   - ping/state/tree/find resolve the live item tree
//   - click synthesizes input that moves the view (nav button)
//   - set/get read and write control properties
//   - type + key reach focused text input
//   - wait polls a property to match
//   - eval exposes the root context
//   - unknown ops and bad selectors fail honestly
//   - hardening: every request needs the per-launch token; a
//     missing/wrong token, a malformed (e.g. HTTP) line or an
//     over-long line gets one error reply and the connection
//     closes; `shot` confines relative paths to the QA output
//     dir and refuses to overwrite files outside it
//
// No engine, no hardware — the bridge stays disconnected and
// the shell renders its honest degraded surface.

#include "DiagnosticsController.h"
#include "EngineBridge.h"
#include "LayoutController.h"
#include "QaServer.h"
#include "SceneEditController.h"
#include "UiSettings.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QEventLoop>
#include <QGuiApplication>
#include <QJsonArray>
#include <QKeyEvent>
#include <QJsonDocument>
#include <QJsonObject>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickItem>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QTcpSocket>
#include <QTimer>

#include <cstdio>

using namespace openrgb2;

static int checks   = 0;
static int failures = 0;
#define CHECK(x) do { ++checks; if(!(x)) { ++failures; \
    std::printf("FAIL line %d: %s\n", __LINE__, #x); } } while(0)

/* The server's per-launch token — Req() adds it to every
   request that does not carry its own "token". */
static QString g_token;

/* One request line -> one reply line. The test never enters
   app.exec(), so the reply loop must pump the shared event
   loop — that is also what fires the server's readyRead. */
static QJsonObject Req(QTcpSocket& sock, const QJsonObject& req_in)
{
    QJsonObject req = req_in;
    if(!req.contains(QStringLiteral("token")))
    {
        req.insert(QStringLiteral("token"), g_token);
    }
    sock.write(QJsonDocument(req).toJson(QJsonDocument::Compact));
    sock.write("\n");
    sock.flush();
    QByteArray acc;
    const qint64 deadline =
        QDateTime::currentMSecsSinceEpoch() + 10000;
    while(!acc.contains('\n'))
    {
        QCoreApplication::processEvents(
            QEventLoop::AllEvents, 50);
        acc += sock.readAll();
        if(QDateTime::currentMSecsSinceEpoch() > deadline)
        {
            std::printf("FAIL: reply timeout for %s\n",
                        req.value("op").toString()
                            .toUtf8().constData());
            ++checks; ++failures;
            return {};
        }
    }
    return QJsonDocument::fromJson(acc.left(acc.indexOf('\n')))
        .object();
}

static bool OkField(const QJsonObject& r)
{
    return r.value(QStringLiteral("ok")).toBool(false);
}

/* Send raw bytes, read one reply line, then report whether the
   server closed the connection afterwards. */
static QJsonObject RawThenExpectClose(QTcpSocket& sock,
                                      const QByteArray& bytes,
                                      bool& closed)
{
    sock.write(bytes);
    sock.flush();
    QByteArray acc;
    const qint64 deadline =
        QDateTime::currentMSecsSinceEpoch() + 5000;
    while(QDateTime::currentMSecsSinceEpoch() < deadline)
    {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        acc += sock.readAll();
        if(acc.contains('\n') &&
           sock.state() == QAbstractSocket::UnconnectedState)
        {
            break;
        }
    }
    closed = sock.state() == QAbstractSocket::UnconnectedState;
    return QJsonDocument::fromJson(acc.left(acc.indexOf('\n')))
        .object();
}

int main(int argc, char** argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QGuiApplication app(argc, argv);
    QQuickStyle::setStyle(QStringLiteral("Basic"));

    QQmlApplicationEngine engine;
    ui::EngineBridge bridge;   /* never start()ed — offline */
    ui::SceneEditController scene_edit(&bridge);
    ui::LayoutController layout_ctl(&bridge);
    ui::DiagnosticsController diag_ctl(&bridge);
    /* uiPrefs intentionally NOT exposed: QSettings persistence
       from real app runs would make the starting view
       nondeterministic — Main.qml renders defaults without it. */
    engine.rootContext()->setContextProperty(
        QStringLiteral("bridge"), &bridge);
    engine.rootContext()->setContextProperty(
        QStringLiteral("sceneEdit"), &scene_edit);
    engine.rootContext()->setContextProperty(
        QStringLiteral("layoutCtl"), &layout_ctl);
    engine.rootContext()->setContextProperty(
        QStringLiteral("diagCtl"), &diag_ctl);
    engine.loadFromModule(QStringLiteral("OpenRGB2.UI"),
                          QStringLiteral("Main"));
    CHECK(!engine.rootObjects().isEmpty());
    if(engine.rootObjects().isEmpty())
    {
        std::printf("FAIL: Main.qml did not load\n");
        return 1;
    }

    ui::QaServer qa(engine);
    QString err;
    CHECK(qa.Start(0, err));
    if(!err.isEmpty())
    {
        std::printf("start err: %s\n",
                    err.toUtf8().constData());
    }
    const quint16 port = qa.Port();
    CHECK(port != 0);
    g_token = qa.Token();
    CHECK(g_token.size() == 64);

    /* Shots go to a scratch QA output dir, never the source tree. */
    QTemporaryDir shot_dir;
    CHECK(shot_dir.isValid());
    qputenv("OPENRGB2_QA_OUTDIR", shot_dir.path().toUtf8());

    /* token file round-trip */
    {
        QTemporaryDir td;
        const QString f = QDir(td.path()).filePath(
            QStringLiteral("sub/qa.token"));
        QString terr;
        CHECK(qa.WriteTokenFile(f, terr));
        QFile tf(f);
        CHECK(tf.open(QIODevice::ReadOnly));
        CHECK(QString::fromLatin1(tf.readAll()) == g_token);
        CHECK(ui::QaServer::TokenFilePath(5583)
                  .endsWith(QStringLiteral("qa-5583.token")));
    }

    /* Missing / wrong token -> unauthorized + closed. */
    {
        QTcpSocket bad;
        bad.connectToHost(QStringLiteral("127.0.0.1"), port);
        CHECK(bad.waitForConnected(3000));
        bool closed = false;
        const QJsonObject r = RawThenExpectClose(
            bad, "{\"op\":\"ping\"}\n", closed);
        CHECK(!OkField(r));
        CHECK(r.value(QStringLiteral("error")).toString()
              == QLatin1String("unauthorized"));
        CHECK(closed);
    }
    {
        QTcpSocket bad;
        bad.connectToHost(QStringLiteral("127.0.0.1"), port);
        CHECK(bad.waitForConnected(3000));
        bool closed = false;
        const QJsonObject r = RawThenExpectClose(
            bad, "{\"op\":\"eval\",\"code\":\"1\","
                 "\"token\":\"nope\"}\n", closed);
        CHECK(r.value(QStringLiteral("error")).toString()
              == QLatin1String("unauthorized"));
        CHECK(closed);
    }
    /* Cross-protocol HTTP POST: the first (non-JSON) line gets
       one malformed reply and the connection closes — the JSON
       body that follows is never parsed. */
    {
        QTcpSocket bad;
        bad.connectToHost(QStringLiteral("127.0.0.1"), port);
        CHECK(bad.waitForConnected(3000));
        bool closed = false;
        const QByteArray http =
            "POST / HTTP/1.1\r\nHost: 127.0.0.1\r\n"
            "Content-Type: text/plain\r\n\r\n"
            "{\"op\":\"eval\",\"code\":\"Qt.quit()\"}\n";
        const QJsonObject r = RawThenExpectClose(bad, http, closed);
        CHECK(r.value(QStringLiteral("error")).toString()
              == QLatin1String("malformed"));
        CHECK(closed);
    }
    /* Over-long line without a newline -> line_too_long + closed. */
    {
        QTcpSocket bad;
        bad.connectToHost(QStringLiteral("127.0.0.1"), port);
        CHECK(bad.waitForConnected(3000));
        bool closed = false;
        const QJsonObject r = RawThenExpectClose(
            bad, QByteArray(ui::QaServer::kMaxLineBytes + 4096, 'x'),
            closed);
        CHECK(r.value(QStringLiteral("error")).toString()
              == QLatin1String("line_too_long"));
        CHECK(closed);
    }

    QTcpSocket sock;
    sock.connectToHost(QStringLiteral("127.0.0.1"), port);
    CHECK(sock.waitForConnected(3000));

    /* ping */
    {
        const QJsonObject r =
            Req(sock, {{ QStringLiteral("op"),
                         QStringLiteral("ping") }});
        CHECK(OkField(r));
    }

    /* state — root window summary */
    {
        const QJsonObject r =
            Req(sock, {{ QStringLiteral("op"),
                         QStringLiteral("state") }});
        CHECK(OkField(r));
        const QJsonObject res =
            r.value(QStringLiteral("result")).toObject();
        CHECK(res.value(QStringLiteral("currentView")).toInt(-1)
              == 0);
        CHECK(res.value(QStringLiteral("connState")).toString()
              == QLatin1String("disconnected"));
    }

    /* tree — the interactive inventory must see the nav */
    {
        const QJsonObject r =
            Req(sock, {{ QStringLiteral("op"),
                         QStringLiteral("tree") }});
        CHECK(OkField(r));
        const QJsonArray items = r.value(QStringLiteral("result"))
                                     .toObject()
                                     .value(QStringLiteral("items"))
                                     .toArray();
        CHECK(items.size() > 10);
        bool saw_nav = false;
        for(const QJsonValue& v : items)
        {
            if(v.toObject().value(QStringLiteral("name"))
                   .toString() == QLatin1String("navLighting"))
            {
                saw_nav = true;
            }
        }
        CHECK(saw_nav);
    }

    /* find by objectName + by type */
    CHECK(OkField(Req(sock, {{ QStringLiteral("op"),
                              QStringLiteral("find") },
                            { QStringLiteral("sel"),
                              QStringLiteral("navLighting") }})));
    {
        const QJsonObject r =
            Req(sock, {{ QStringLiteral("op"),
                         QStringLiteral("find") },
                       { QStringLiteral("sel"),
                         QStringLiteral("type:NavButton") }});
        CHECK(OkField(r));
        CHECK(r.value(QStringLiteral("result")).toArray().size()
              >= 5);
    }

    /* click — synthesized press/release moves the view */
    CHECK(OkField(Req(sock, {{ QStringLiteral("op"),
                              QStringLiteral("click") },
                            { QStringLiteral("sel"),
                              QStringLiteral("navLighting") }})));
    {
        const QJsonObject r =
            Req(sock, {{ QStringLiteral("op"),
                         QStringLiteral("state") }});
        CHECK(r.value(QStringLiteral("result"))
                  .toObject()
                  .value(QStringLiteral("currentView")).toInt()
              == 1);
    }

    /* eval — root-context escape hatch */
    {
        const QJsonObject r =
            Req(sock, {{ QStringLiteral("op"),
                         QStringLiteral("eval") },
                       { QStringLiteral("code"),
                         QStringLiteral("this.currentView") }});
        CHECK(OkField(r));
        CHECK(r.value(QStringLiteral("result")).toInt() == 1);
        CHECK(OkField(Req(sock, {{ QStringLiteral("op"),
                                  QStringLiteral("eval") },
                                { QStringLiteral("code"),
                                  QStringLiteral(
                                      "typeof bridge !== "
                                      "'undefined'") }})));
    }

    /* wait — property poll (already-true resolves fast) */
    CHECK(OkField(Req(sock,
        {{ QStringLiteral("op"), QStringLiteral("wait") },
         { QStringLiteral("sel"), QStringLiteral("navLighting") },
         { QStringLiteral("prop"), QStringLiteral("visible") },
         { QStringLiteral("value"), true },
         { QStringLiteral("timeout_ms"), 2000 }})));

    /* type — key events reach the scene-name field */
    {
        CHECK(OkField(Req(sock, {{ QStringLiteral("op"),
                                  QStringLiteral("type") },
                                { QStringLiteral("sel"),
                                  QStringLiteral("nameField") },
                                { QStringLiteral("text"),
                                  QStringLiteral("qa-scene") },
                                { QStringLiteral("clear"), true }})));
        const QJsonObject r =
            Req(sock, {{ QStringLiteral("op"),
                         QStringLiteral("get") },
                       { QStringLiteral("sel"),
                         QStringLiteral("nameField") },
                       { QStringLiteral("prop"),
                         QStringLiteral("text") }});
        CHECK(OkField(r));
        CHECK(r.value(QStringLiteral("result"))
                  .toObject()
                  .value(QStringLiteral("value")).toString()
              == QLatin1String("qa-scene"));
    }

    /* Hidden pages report invisible — the selector must reach
       Settings by navigating there first, like a user. */
    {
        const QJsonObject hidden =
            Req(sock, {{ QStringLiteral("op"),
                         QStringLiteral("find") },
                       { QStringLiteral("sel"),
                         QStringLiteral("restoreViewCheck") }});
        CHECK(!OkField(hidden));
    }
    CHECK(OkField(Req(sock, {{ QStringLiteral("op"),
                              QStringLiteral("click") },
                            { QStringLiteral("sel"),
                              QStringLiteral("navSettings") }})));

    /* set — direct property write on a checkbox */
    {
        CHECK(OkField(Req(sock, {{ QStringLiteral("op"),
                                  QStringLiteral("set") },
                                { QStringLiteral("sel"),
                                  QStringLiteral(
                                      "restoreViewCheck") },
                                { QStringLiteral("prop"),
                                  QStringLiteral("checked") },
                                { QStringLiteral("value"), false }})));
        const QJsonObject r =
            Req(sock, {{ QStringLiteral("op"),
                         QStringLiteral("get") },
                       { QStringLiteral("sel"),
                         QStringLiteral("restoreViewCheck") },
                       { QStringLiteral("prop"),
                         QStringLiteral("checked") }});
        CHECK(r.value(QStringLiteral("result"))
                  .toObject()
                  .value(QStringLiteral("value")).toBool(true)
              == false);
    }

    /* geom — detailed geometry for layout debugging */
    {
        const QJsonObject r =
            Req(sock, {{ QStringLiteral("op"),
                         QStringLiteral("geom") },
                       { QStringLiteral("sel"),
                         QStringLiteral("navLighting") }});
        CHECK(OkField(r));
        CHECK(r.value(QStringLiteral("result"))
                  .toObject()
                  .contains(QStringLiteral("implicitWidth")));
    }

    /* honest failures — no match, unknown op, bad JSON */
    {
        const QJsonObject r =
            Req(sock, {{ QStringLiteral("op"),
                         QStringLiteral("click") },
                       { QStringLiteral("sel"),
                         QStringLiteral("doesNotExist") }});
        CHECK(!OkField(r));
        CHECK(r.value(QStringLiteral("error")).toString()
              == QLatin1String("not_found"));
    }
    {
        const QJsonObject r =
            Req(sock, {{ QStringLiteral("op"),
                         QStringLiteral("bogus") }});
        CHECK(!OkField(r));
        CHECK(r.value(QStringLiteral("error")).toString()
              == QLatin1String("unknown_op"));
    }
    {
        /* Bad JSON closes the connection — use a throwaway one. */
        QTcpSocket bad;
        bad.connectToHost(QStringLiteral("127.0.0.1"), port);
        CHECK(bad.waitForConnected(3000));
        bool closed = false;
        const QJsonObject r =
            RawThenExpectClose(bad, "this is not json\n", closed);
        CHECK(!OkField(r));
        CHECK(r.value(QStringLiteral("error")).toString()
              == QLatin1String("malformed"));
        CHECK(closed);
    }

    /* shot — offscreen grab may legitimately fail on some
       platforms; the contract is an honest reply either way.
       A relative path lands under the QA output dir. */
    {
        const QJsonObject r =
            Req(sock, {{ QStringLiteral("op"),
                         QStringLiteral("shot") },
                       { QStringLiteral("path"),
                         QStringLiteral("qa-test.png") }});
        CHECK(r.contains(QStringLiteral("ok")));
        if(OkField(r))
        {
            const QJsonObject res =
                r.value(QStringLiteral("result")).toObject();
            CHECK(res.value(QStringLiteral("width")).toInt() > 0);
            CHECK(QFileInfo(res.value(QStringLiteral("path"))
                                .toString()).absolutePath()
                  == QFileInfo(shot_dir.path()).absoluteFilePath());
        }
    }
    /* ...and never overwrites an existing file outside it, nor
       writes a non-.png name. */
    {
        QTemporaryDir outside;
        const QString victim = QDir(outside.path()).filePath(
            QStringLiteral("keep.png"));
        {
            QFile f(victim);
            CHECK(f.open(QIODevice::WriteOnly));
            f.write("precious");
        }
        const QJsonObject r =
            Req(sock, {{ QStringLiteral("op"),
                         QStringLiteral("shot") },
                       { QStringLiteral("path"), victim }});
        CHECK(!OkField(r));
        CHECK(r.value(QStringLiteral("error")).toString()
              == QLatin1String("bad_path"));
        QFile f(victim);
        CHECK(f.open(QIODevice::ReadOnly));
        CHECK(f.readAll() == "precious");
        const QJsonObject r2 =
            Req(sock, {{ QStringLiteral("op"),
                         QStringLiteral("shot") },
                       { QStringLiteral("path"),
                         QStringLiteral("evil.dll") }});
        CHECK(r2.value(QStringLiteral("error")).toString()
              == QLatin1String("bad_path"));
    }

    /* Re-entry: a second request pipelined behind a `wait` is
       queued while the wait's nested loop runs and answered
       AFTER it, in order (not dispatched re-entrantly). */
    {
        QJsonObject w{
            { QStringLiteral("op"),   QStringLiteral("wait") },
            { QStringLiteral("sel"),  QStringLiteral("navLighting") },
            { QStringLiteral("prop"), QStringLiteral("visible") },
            { QStringLiteral("value"), false },
            { QStringLiteral("timeout_ms"), 300 },
            { QStringLiteral("token"), g_token }};
        QJsonObject p{
            { QStringLiteral("op"),   QStringLiteral("ping") },
            { QStringLiteral("token"), g_token }};
        sock.write(QJsonDocument(w).toJson(QJsonDocument::Compact)
                   + "\n");
        sock.flush();
        /* This thread is inside the server's nested wait loop
           once it pumps — a timer is the only way to put the 2nd
           line on the wire WHILE the wait is running. Without the
           re-entry guard the ping would be dispatched inside the
           wait and its reply would arrive FIRST. */
        const QByteArray ping_line =
            QJsonDocument(p).toJson(QJsonDocument::Compact) + "\n";
        QTimer::singleShot(100, &sock, [&sock, ping_line] {
            sock.write(ping_line);
            sock.flush();
        });
        QByteArray acc;
        const qint64 deadline =
            QDateTime::currentMSecsSinceEpoch() + 5000;
        while(acc.count('\n') < 2 &&
              QDateTime::currentMSecsSinceEpoch() < deadline)
        {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
            acc += sock.readAll();
        }
        const QList<QByteArray> lines = acc.split('\n');
        CHECK(lines.size() >= 2);
        if(lines.size() >= 2)
        {
            const QJsonObject first =
                QJsonDocument::fromJson(lines[0]).object();
            const QJsonObject second =
                QJsonDocument::fromJson(lines[1]).object();
            CHECK(first.value(QStringLiteral("error")).toString()
                  == QLatin1String("timeout"));
            CHECK(OkField(second));
            CHECK(second.value(QStringLiteral("result")).toString()
                  == QLatin1String("pong"));
        }
    }

    /* F02 hardening — a type whose FIRST keystroke destroys
       the target item SYNCHRONOUSLY. QML destroy()/Repeater
       and Loader teardown are all deferred, so they cannot
       reproduce the crash; the real F02 teardown came from
       C++ (a model reset) inside delivery. The event filter
       deletes the victim while the KeyPress is in flight —
       'a' kills it, 'b' must never be delivered to a
       dangling item and the reply stays honest. */
    {
        class VictimFilter final : public QObject
        {
        public:
            explicit VictimFilter(QQuickItem* v) : victim_(v) {}
            bool eventFilter(QObject*, QEvent* ev) override
            {
                if(ev->type() == QEvent::KeyPress)
                {
                    /* Synchronous delete inside the delivery
                       stack — returning true filters the
                       event so Qt never calls event() on the
                       freed object. */
                    delete victim_;
                    return true;
                }
                return false;
            }
        private:
            QQuickItem* victim_;
        };
        QQuickWindow* win = qobject_cast<QQuickWindow*>(
            engine.rootObjects().value(0));
        CHECK(win != nullptr);
        QQuickItem* victim = new QQuickItem(win->contentItem());
        victim->setObjectName(QStringLiteral("qaTypeVictim"));
        victim->setWidth(60);
        victim->setHeight(24);
        victim->setFocus(true);
        auto* vf = new VictimFilter(victim);
        vf->setParent(victim);
        victim->installEventFilter(vf);
        const QJsonObject r =
            Req(sock, {{ QStringLiteral("op"),
                         QStringLiteral("type") },
                       { QStringLiteral("sel"),
                         QStringLiteral("qaTypeVictim") },
                       { QStringLiteral("text"),
                         QStringLiteral("ab") }});
        CHECK(OkField(r));
        const QJsonObject res =
            r.value(QStringLiteral("result")).toObject();
        /* one char delivered, the second never sent — and the
           reply says the item died under the op honestly */
        CHECK(res.value(QStringLiteral("sent")).toInt() == 1);
        CHECK(res.value(QStringLiteral("destroyed"))
                  .toBool(false) == true);
        /* the item is really gone — resolution fails
           honestly afterwards */
        const QJsonObject gone =
            Req(sock, {{ QStringLiteral("op"),
                         QStringLiteral("find") },
                       { QStringLiteral("sel"),
                         QStringLiteral("qaTypeVictim") }});
        CHECK(!OkField(gone));
    }

    /* F02 hardening — the client abandons a pending `wait`.
       The op's nested event loop must exit on disconnect and
       the reply must never be written to a dead socket (the
       raw-pointer dispatch crash). A fresh connection then
       proves the server itself survived. */
    {
        sock.write(QJsonDocument(QJsonObject{
            { QStringLiteral("op"),   QStringLiteral("wait") },
            { QStringLiteral("sel"),  QStringLiteral("navLighting") },
            { QStringLiteral("prop"), QStringLiteral("visible") },
            { QStringLiteral("value"), false },
            { QStringLiteral("timeout_ms"), 30000 },
            { QStringLiteral("token"), g_token }})
                .toJson(QJsonDocument::Compact));
        sock.write("\n");
        sock.flush();
        /* let readyRead pick the line up and enter OpWait's
           nested loop */
        for(int i = 0; i < 10; ++i)
        {
            QCoreApplication::processEvents(
                QEventLoop::AllEvents, 50);
        }
        /* hard disconnect — RST, no graceful close */
        sock.abort();
        for(int i = 0; i < 10; ++i)
        {
            QCoreApplication::processEvents(
                QEventLoop::AllEvents, 50);
        }
        QTcpSocket sock2;
        sock2.connectToHost(QStringLiteral("127.0.0.1"), port);
        CHECK(sock2.waitForConnected(3000));
        CHECK(OkField(Req(sock2, {{ QStringLiteral("op"),
                                  QStringLiteral("ping") }})));
        sock2.disconnectFromHost();
    }

    std::printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
