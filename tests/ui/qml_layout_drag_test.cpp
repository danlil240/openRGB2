// SPDX-License-Identifier: GPL-2.0-or-later
//
// N02 regression — Layout 2D canvas: an UNCOMMITTED draft
// move must repaint the dragged object's rectangle. The
// reported defect (QA 2026-10-01): after a native drag the
// document coordinates, staged status and dirty flag all
// updated, but the rendered rectangle stayed pinned at its
// prior position until the scene was Saved.
//
// Root cause exercised here: the ws-mode items join mixed
// document generations — x/z came from the draft while the
// footprint offset ox/oz was measured against the engine's
// APPLIED resolved scene (which only Save refreshes), so
// x+ox silently cancelled the move.
//
// Drives the REAL LayoutPage.qml over the in-process wire
// pipe (test_pipe.h) against a scripted
// studio_workspace_v1 engine:
//   1. the managed workspace loads a compact draft with the
//      keyboard at (0.1, 0.2)
//   2. the engine's APPLIED resolved scene still carries the
//      keyboard at the origin — the generation skew a draft
//      move creates live
//   3. layoutCtl.moveObject() (the exact call the canvas
//      delegate makes on drag release) moves the draft —
//      the canvasObj_kbd delegate's painted position must
//      follow WITHOUT any workspace commit
//
// CHECK-macro main like the other UI suites — no framework.

#include "DiagnosticsController.h"
#include "EngineBridge.h"
#include "LayoutController.h"
#include "SceneEditController.h"
#include "WorkspaceController.h"

#include "test_pipe.h"

#include <QCryptographicHash>
#include <QGuiApplication>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickStyle>
#include <QTemporaryDir>
#include <QVariantMap>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

using namespace openrgb2;
using namespace openrgb2::ipc;
using namespace openrgb2::ui;
using namespace ui_test;

static int checks   = 0;
static int failures = 0;
#define CHECK(x) do { ++checks; if(!(x)) { ++failures; \
    std::printf("FAIL line %d: %s\n", __LINE__, #x); } } while(0)

static nlohmann::json V3(double x, double y, double z)
{
    return nlohmann::json::object({{"x",x},{"y",y},{"z",z}});
}

/* The compact v3 draft — kbd's draft placement (0.1, 0.2)
   intentionally differs from the applied resolved scene
   below (origin): the same skew an unsaved move creates. */
static nlohmann::json CompactDoc()
{
    return nlohmann::json::object({
        { "schema_version", 3 },
        { "name",           "desk" },
        { "devices", nlohmann::json::object({
            { "kbd", nlohmann::json::object({
                { "type", "kbd" }, { "x", 0.1 },
                { "y", 0.0 },    { "z", 0.2 } }) },
            { "desk", nlohmann::json::object({
                { "type", "desk" }, { "x", -0.5 },
                { "y", 0.0 },     { "z", -0.3 } }) },
        }) },
        { "bindings",        nlohmann::json::object() },
        { "device_settings", nlohmann::json::object() },
        { "colors", nlohmann::json::object({
            { "objects",  nlohmann::json::object() },
            { "emitters", nlohmann::json::object() } }) },
        { "effects", nlohmann::json::object({
            { "preset", "aurora" }, { "seed", 0 },
            { "speed", 1.0 }, { "intensity", 1.0 },
            { "playing", true },
            { "layers", nlohmann::json::array() } }) },
        { "output", nlohmann::json::object({
            { "brightness", 1.0 } }) },
    });
}

/* The engine's APPLIED resolved scene — kbd at the ORIGIN
   with its body offset (+5cm x, -2cm z) from the instance
   origin; Save is what would refresh this. A second body
   anchors the view so a one-object recenter can't mask the
   painted position. */
static nlohmann::json ResolvedDoc()
{
    return nlohmann::json::object({
        { "version", 2 },
        { "name",    "desk" },
        { "objects", nlohmann::json::array({
            nlohmann::json::object({
                { "id",       "kbd" },
                { "kind",     "group" },
                { "position", V3(0.0, 0.0, 0.0) },
                { "rotation", V3(0.0, 0.0, 0.0) },
            }),
            nlohmann::json::object({
                { "id",        "kbd/body" },
                { "kind",      "device" },
                { "parent_id", "kbd" },
                { "position",  V3(0.05, 0.0, -0.02) },
                { "rotation",  V3(0.0, 0.0, 0.0) },
                { "size_m",    V3(0.4, 0.02, 0.1) },
            }),
            nlohmann::json::object({
                { "id",       "desk" },
                { "kind",     "group" },
                { "position", V3(-0.5, 0.0, -0.3) },
                { "rotation", V3(0.0, 0.0, 0.0) },
            }),
            nlohmann::json::object({
                { "id",        "desk/top" },
                { "kind",      "decor" },
                { "parent_id", "desk" },
                { "position",  V3(0.0, 0.0, 0.0) },
                { "rotation",  V3(0.0, 0.0, 0.0) },
                { "size_m",    V3(0.6, 0.03, 0.3) },
            }),
        }) },
    });
}

struct WsServer
{
    explicit WsServer(ServerPeer p) : srv(std::move(p)) {}

    ServerPeer    srv;
    std::string   ws_id   = "ws0";
    uint64_t      ws_rev  = 1;
    uint64_t      res_rev = 1;
    nlohmann::json doc    = CompactDoc();
    int           commits = 0;

    std::string DocBytesFor() const { return doc.dump(2); }
    std::string DocSha() const
    {
        return QCryptographicHash::hash(
            QByteArray::fromStdString(DocBytesFor()),
            QCryptographicHash::Sha256).toHex().toStdString();
    }
    nlohmann::json Snapshot() const
    {
        return nlohmann::json::object({
            { "managed",            true },
            { "id",                 ws_id },
            { "name",               "desk" },
            { "workspace_revision", ws_rev },
            { "resource_revision",  res_rev },
            { "generation",         ws_rev },
            { "content_sha256",     DocSha() },
        });
    }
    nlohmann::json SceneEnvelope() const
    {
        return nlohmann::json::object({
            { "loaded",   true },
            { "revision", 1 },
            { "name",     "desk" },
            { "playing",  true },
            { "document", ResolvedDoc() },
        });
    }
    void SendEvent(const nlohmann::json& data)
    {
        srv.SendMsg(MsgType::Event, 0, nlohmann::json::object({
            { "kind", "workspace" }, { "data", data } }));
    }
};

static bool Serve(WsServer& e, uint32_t wait_ms = 15)
{
    Message m;
    if(!e.srv.RecvMsg(m, wait_ms))
    {
        return false;
    }
    const nlohmann::json body = m.Body();
    switch(m.type)
    {
    case MsgType::Hello:
        e.srv.SendMsg(MsgType::Welcome, 0,
            nlohmann::json::object({
                { "server_version",     1 },
                { "negotiated_version", 1 },
                { "channel",            "command" },
                { "engine",             "openrgb2-engine" },
                { "running",            true },
                { "capabilities", nlohmann::json::array(
                    { "studio_workspace_v1" }) },
                { "state", nlohmann::json::object({
                    { "engine", nlohmann::json::object(
                        {{ "running", true },
                         { "scene_clock_hz", 60 }}) },
                    { "scene", e.SceneEnvelope() },
                    { "sdk", nlohmann::json::object(
                        {{ "state", "connected" },
                         { "reconnect_attempt", 0 }}) },
                }) },
            }));
        break;
    case MsgType::Subscribe:
        e.srv.SendMsg(MsgType::Ok, m.request_id,
            nlohmann::json::object(
                { { "subscribed", nlohmann::json::array({
                      "workspace", "scene" }) } }));
        e.SendEvent(e.Snapshot());
        break;
    case MsgType::GetScene:
        e.srv.SendMsg(MsgType::Scene, m.request_id,
                      e.SceneEnvelope());
        break;
    case MsgType::GetSnapshots:
        e.srv.SendMsg(MsgType::Snapshots, m.request_id,
                      nlohmann::json::object());
        break;
    case MsgType::GetState:
        e.srv.SendMsg(MsgType::State, m.request_id,
            nlohmann::json::object({
                { "engine", nlohmann::json::object(
                    {{ "running", true }}) },
                { "scene",  e.SceneEnvelope() },
            }));
        break;
    case MsgType::GetPresets:
        e.srv.SendMsg(MsgType::Presets, m.request_id,
            nlohmann::json::object(
                { { "presets", nlohmann::json::array() } }));
        break;
    case MsgType::GetPlayback:
        e.srv.SendMsg(MsgType::Playback, m.request_id,
                      nlohmann::json::object());
        break;
    case MsgType::GetDeliveryStats:
        e.srv.SendMsg(MsgType::DeliveryStats, m.request_id,
                      nlohmann::json::object());
        break;
    case MsgType::WorkspaceCommand:
    {
        const std::string verb = body.value("verb", std::string());
        if(verb == "get")
        {
            e.srv.SendMsg(MsgType::Ok, m.request_id, e.Snapshot());
        }
        else if(verb == "transfer_begin")
        {
            const std::string bytes = e.DocBytesFor();
            e.srv.SendMsg(MsgType::Ok, m.request_id,
                nlohmann::json::object({
                    { "transfer_id",     "dl1" },
                    { "length",          (uint64_t)bytes.size() },
                    { "sha256",          e.DocSha() },
                    { "max_chunk_bytes", (uint64_t)400 },
                }));
        }
        else if(verb == "transfer_chunk")
        {
            const std::string bytes = e.DocBytesFor();
            const uint64_t off = body.value("offset", (uint64_t)0);
            const uint64_t mx  = body.value("max_bytes", (uint64_t)400);
            const uint64_t n   = off < bytes.size()
                ? std::min<uint64_t>(mx, bytes.size() - off) : 0;
            const bool eof = off + n >= bytes.size();
            e.srv.SendMsg(MsgType::Ok, m.request_id,
                nlohmann::json::object({
                    { "offset", off },
                    { "data",   QByteArray(bytes.data() + off,
                                           (qsizetype)n)
                                    .toBase64().toStdString() },
                    { "eof",    eof },
                }));
        }
        else if(verb == "commit")
        {
            ++e.commits;
            e.srv.SendMsg(MsgType::Ok, m.request_id,
                nlohmann::json::object(
                    { { "accepted", true }, { "job_id", 1 } }));
        }
        else
        {
            e.srv.SendMsg(MsgType::Error, m.request_id,
                nlohmann::json::object({
                    { "code", (uint32_t)ErrCode::BadRequest },
                    { "error",   "bad_request" },
                    { "message", "unknown verb" },
                    { "detail",  "bad_request" } }));
        }
        break;
    }
    default:
        e.srv.SendMsg(MsgType::Error, m.request_id,
            nlohmann::json::object({
                { "code", (uint32_t)ErrCode::BadRequest },
                { "error",   "bad_request" },
                { "message", "unexpected request" },
            }));
        break;
    }
    return true;
}

template <typename Pred>
static bool PumpBoth(WsServer& e, Pred pred, int ms = 12000)
{
    const auto deadline = std::chrono::steady_clock::now()
        + std::chrono::milliseconds(ms);
    while(std::chrono::steady_clock::now() < deadline)
    {
        if(pred())
        {
            return true;
        }
        Serve(e, 10);
        QCoreApplication::processEvents(QEventLoop::AllEvents,
                                        10);
    }
    return pred();
}

/* Repeater delegates are NOT reachable through
   QObject::findChild — resolve them via the canvas Repeater's
   itemAt + objectName. */
static QQuickItem* CanvasObject(QObject* page,
                                const QString& objectName)
{
    QObject* rep = page->findChild<QObject*>(
        QStringLiteral("canvasItems"));
    if(rep == nullptr)
    {
        return nullptr;
    }
    const int n = rep->property("count").toInt();
    for(int i = 0; i < n; ++i)
    {
        QQuickItem* it = nullptr;
        QMetaObject::invokeMethod(rep, "itemAt",
            Q_RETURN_ARG(QQuickItem*, it), Q_ARG(int, i));
        if(it != nullptr && it->objectName() == objectName)
        {
            return it;
        }
    }
    return nullptr;
}

/* The painted footprint center out of the delegate's bound
   modelData — the o.x + o.ox expression the canvas binds. */
static bool Footprint(QQuickItem* item, double& cx, double& cz)
{
    if(item == nullptr)
    {
        return false;
    }
    const QVariantMap o = item->property("o").toMap();
    if(o.isEmpty())
    {
        return false;
    }
    cx = o.value(QStringLiteral("x")).toDouble()
       + o.value(QStringLiteral("ox")).toDouble();
    cz = o.value(QStringLiteral("z")).toDouble()
       + o.value(QStringLiteral("oz")).toDouble();
    return true;
}

int main(int argc, char** argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QGuiApplication app(argc, argv);
    QQuickStyle::setStyle(QStringLiteral("Basic"));

    auto         pipe = std::make_shared<TestPipe>();
    WsServer     s(pipe->Peer());
    EngineBridge bridge;
    bridge.setChannelFactoryForTesting(
        [&](std::string& err) -> std::unique_ptr<IIpcChannel>
        {
            (void)err;
            return pipe->ClientChannel();
        });
    EngineWireClient::Options wopts;
    wopts.keepalive_ms = 0;
    bridge.setWireOptionsForTesting(wopts);

    WorkspaceController   ws_ctl(&bridge);
    SceneEditController   scene_edit(&bridge);
    LayoutController      layout_ctl(&bridge);
    DiagnosticsController diag_ctl(&bridge);
    scene_edit.setWorkspace(&ws_ctl);
    layout_ctl.setWorkspace(&ws_ctl);
    QTemporaryDir draft_dir;
    ws_ctl.setDraftDirForTesting(draft_dir.path());

    QQmlEngine engine;
    engine.rootContext()->setContextProperty(
        QStringLiteral("bridge"), &bridge);
    engine.rootContext()->setContextProperty(
        QStringLiteral("sceneEdit"), &scene_edit);
    engine.rootContext()->setContextProperty(
        QStringLiteral("layoutCtl"), &layout_ctl);
    engine.rootContext()->setContextProperty(
        QStringLiteral("diagCtl"), &diag_ctl);
    engine.rootContext()->setContextProperty(
        QStringLiteral("workspace"), &ws_ctl);

    bridge.start();
    CHECK(PumpBoth(s, [&] {
        return ws_ctl.managed() && ws_ctl.ready()
               && layout_ctl.hasScene();
    }));

    /*---- the real 2D canvas page ---------------------------*/
    QQmlComponent comp(&engine,
        QUrl(QStringLiteral(
            "qrc:/qt/qml/OpenRGB2/UI/pages/LayoutPage.qml")));
    if(comp.isError())
    {
        ++failures;
        std::printf("FAIL load LayoutPage.qml:\n%s\n",
                    comp.errorString().toUtf8().constData());
        std::printf("%d checks, %d failures\n", checks, failures);
        return 1;
    }
    std::unique_ptr<QObject> page(
        comp.create(engine.rootContext()));
    CHECK(page != nullptr);
    if(!page)
    {
        std::printf("%d checks, %d failures\n", checks, failures);
        return 1;
    }
    page->setProperty("width", 1440);
    page->setProperty("height", 1200);

    QQuickItem* kbd = nullptr;
    CHECK(PumpBoth(s, [&] {
        kbd = CanvasObject(page.get(),
                           QStringLiteral("canvasObj_kbd"));
        return kbd != nullptr;
    }));
    if(!kbd)
    {
        std::printf("%d checks, %d failures\n", checks, failures);
        return 1;
    }

    /* The draft origin is (0.1,0.2); the applied body rides
       +0.05/-0.02 off its instance origin, so the painted
       footprint centers at (0.15,0.18) — NOT the applied
       (0.05,-0.02) the generation-split join used to give. */
    double cx = 0, cz = 0;
    CHECK(Footprint(kbd, cx, cz));
    CHECK(std::fabs(cx - 0.15) < 1e-4);
    CHECK(std::fabs(cz - 0.18) < 1e-4);
    const double oldX = kbd->x();
    const double oldY = kbd->y();

    /* The delegate's drag MouseArea must keep its grab — the
       page-level Flickable otherwise steals the drag past its
       threshold and scrolls the page instead of moving the
       object. */
    {
        bool found = false;
        for(QQuickItem* c : kbd->childItems())
        {
            if(QString::fromLatin1(c->metaObject()->className())
                   .contains(QStringLiteral("MouseArea"))
               && c->property("preventStealing").isValid())
            {
                found = true;
                CHECK(c->property("preventStealing").toBool());
            }
        }
        CHECK(found);
    }

    /*---- THE REGRESSION ------------------------------------
       moveObject is exactly what the delegate's drag-release
       calls. The document/draft updates pre-fix too — what
       must ALSO update is the PAINTED position, without any
       workspace commit (no Save). */
    CHECK(layout_ctl.moveObject(QStringLiteral("kbd"),
                                0.5, 0.45));
    CHECK(ws_ctl.dirty());

    QQuickItem* moved = nullptr;
    double mcx = 0, mcz = 0;
    CHECK(PumpBoth(s, [&] {
        moved = CanvasObject(page.get(),
                             QStringLiteral("canvasObj_kbd"));
        return moved != nullptr
               && Footprint(moved, mcx, mcz)
               && std::fabs(mcx - 0.55) < 1e-4
               && std::fabs(mcz - 0.43) < 1e-4;
    }));
    /* no commit rode the wire — the repaint is draft-true */
    CHECK(s.commits == 0);
    CHECK(ws_ctl.dirty());
    /* the rectangle itself moved on screen */
    CHECK(moved != nullptr);
    if(moved)
    {
        CHECK(std::fabs(moved->x() - oldX) > 1.0
              || std::fabs(moved->y() - oldY) > 1.0);
    }

    /* Undo walks the footprint back — the painted position
       follows the reverted draft, again without Save. */
    layout_ctl.undo();
    CHECK(PumpBoth(s, [&] {
        moved = CanvasObject(page.get(),
                             QStringLiteral("canvasObj_kbd"));
        return moved != nullptr
               && Footprint(moved, mcx, mcz)
               && std::fabs(mcx - 0.15) < 1e-4
               && std::fabs(mcz - 0.18) < 1e-4;
    }));
    CHECK(s.commits == 0);

    bridge.stop();
    std::printf("qml_layout_drag_test: %d checks, "
                "%d failures\n", checks, failures);
    return failures ? 1 : 0;
}
