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
#include <QEventLoop>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>
#include <QTcpSocket>

#include <cstdio>

using namespace openrgb2;

static int checks   = 0;
static int failures = 0;
#define CHECK(x) do { ++checks; if(!(x)) { ++failures; \
    std::printf("FAIL line %d: %s\n", __LINE__, #x); } } while(0)

/* One request line -> one reply line. The test never enters
   app.exec(), so the reply loop must pump the shared event
   loop — that is also what fires the server's readyRead. */
static QJsonObject Req(QTcpSocket& sock, const QJsonObject& req)
{
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
        sock.write("this is not json\n");
        sock.flush();
        QByteArray acc;
        const qint64 deadline =
            QDateTime::currentMSecsSinceEpoch() + 5000;
        while(!acc.contains('\n') &&
              QDateTime::currentMSecsSinceEpoch() < deadline)
        {
            QCoreApplication::processEvents(
                QEventLoop::AllEvents, 50);
            acc += sock.readAll();
        }
        const QJsonObject r =
            QJsonDocument::fromJson(acc.left(acc.indexOf('\n')))
                .object();
        CHECK(!OkField(r));
        CHECK(r.value(QStringLiteral("error")).toString()
              == QLatin1String("malformed"));
    }

    /* shot — offscreen grab may legitimately fail on some
       platforms; the contract is an honest reply either way. */
    {
        const QJsonObject r =
            Req(sock, {{ QStringLiteral("op"),
                         QStringLiteral("shot") },
                       { QStringLiteral("path"),
                         QStringLiteral("qa-test.png") }});
        CHECK(r.contains(QStringLiteral("ok")));
        if(OkField(r))
        {
            CHECK(r.value(QStringLiteral("result"))
                      .toObject()
                      .value(QStringLiteral("width")).toInt() > 0);
        }
    }

    std::printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
