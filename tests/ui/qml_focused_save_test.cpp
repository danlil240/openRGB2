// SPDX-License-Identifier: GPL-2.0-or-later
//
// Focused-field save regression (Task 4.5 — the calibration
// report's second bug): editing a text field WITHOUT
// committing it (the field still holds focus, editingFinished
// never fired) then activating Save must save the EDITED
// value, never the stale one.
//
// This drives the REAL shell: Main.qml + LightingPage with a
// live EngineBridge + SceneEditController over the in-process
// wire pipe (test_pipe.h) against a scripted engine —
//   1. the name field takes focus + text (uncommitted — the
//      controller's working copy still holds the old name)
//   2. the page's Save path (commitPendingEdits, wired to the
//      Save button's onClicked) runs
//   3. the SET_SCENE body on the wire carries the EDITED name
//
// CHECK-macro main like the other UI suites — no framework.

#include "DiagnosticsController.h"
#include "EngineBridge.h"
#include "LayoutController.h"
#include "SceneEditController.h"

#include "test_pipe.h"

#include <QGuiApplication>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QVariantMap>

#include <chrono>
#include <cstdio>
#include <memory>

using namespace openrgb2;
using namespace openrgb2::ipc;
using namespace openrgb2::ui;
using namespace ui_test;

static int checks   = 0;
static int failures = 0;
#define CHECK(x) do { ++checks; if(!(x)) { ++failures; \
    std::printf("FAIL line %d: %s\n", __LINE__, #x); } } while(0)

static nlohmann::json SceneFixture()
{
    return nlohmann::json::object({
        { "version",    2 },
        { "name",       "desk" },
        { "brightness", 1.0 },
        { "bindings",   nlohmann::json::array() },
        { "objects",    nlohmann::json::array({
            nlohmann::json::object({
                { "id",   "case" },
                { "kind", "decor" } }) }) },
        { "object_colors",  nlohmann::json::object() },
        { "emitter_colors", nlohmann::json::object() },
        { "effect", nlohmann::json::object({
            { "preset", "aurora" }, { "seed", 0 },
            { "speed", 1.0 }, { "intensity", 1.0 },
            { "playing", true },
            { "layers", nlohmann::json::array() } }) },
    });
}

struct Server
{
    explicit Server(ServerPeer peer) : srv(std::move(peer)) {}
    ServerPeer     srv;
    nlohmann::json doc = SceneFixture();
    int            revision    = 1;
    int            scene_saves = 0;
    nlohmann::json last_scene_body;
};

static nlohmann::json SceneEnvelope(const Server& s)
{
    return nlohmann::json::object({
        { "loaded",   true },
        { "revision", s.revision },
        { "name",     s.doc.value("name", std::string("desk")) },
        { "playing",  true },
        { "document", s.doc },
    });
}

static nlohmann::json WelcomeBody(const nlohmann::json& doc)
{
    return nlohmann::json::object({
        { "server_version",     1 },
        { "negotiated_version", 1 },
        { "channel",            "command" },
        { "engine",             "openrgb2-engine" },
        { "running",            true },
        { "state", nlohmann::json::object({
            { "engine", nlohmann::json::object(
                {{ "running", true }, { "scene_clock_hz", 60 }}) },
            { "scene", nlohmann::json::object(
                {{ "loaded", true }, { "revision", 1 },
                 { "name", "desk" }, { "playing", true },
                 { "document", doc }}) },
            { "sdk", nlohmann::json::object(
                {{ "state", "connected" },
                 { "reconnect_attempt", 0 }}) },
        }) },
    });
}

static bool ServeOnce(Server& s, uint32_t wait_ms = 15)
{
    Message m;
    if(!s.srv.RecvMsg(m, wait_ms))
    {
        return false;
    }
    const nlohmann::json body = m.Body();
    switch(m.type)
    {
    case MsgType::Hello:
        s.srv.SendMsg(MsgType::Welcome, 0, WelcomeBody(s.doc));
        break;
    case MsgType::Subscribe:
        s.srv.SendMsg(MsgType::Ok, m.request_id,
            nlohmann::json::object(
                { { "subscribed", nlohmann::json::array({
                    "topology", "scene", "playback",
                    "connection", "frame_status" }) } }));
        break;
    case MsgType::GetScene:
        s.srv.SendMsg(MsgType::Scene, m.request_id,
                      SceneEnvelope(s));
        break;
    case MsgType::GetSnapshots:
        s.srv.SendMsg(MsgType::Snapshots, m.request_id,
                      nlohmann::json::object());
        break;
    case MsgType::GetState:
        s.srv.SendMsg(MsgType::State, m.request_id,
            nlohmann::json::object({
                { "engine", nlohmann::json::object(
                    {{ "running", true }}) },
                { "scene",  SceneEnvelope(s) },
            }));
        break;
    case MsgType::GetPresets:
        s.srv.SendMsg(MsgType::Presets, m.request_id,
                      nlohmann::json::object(
                          { { "presets",
                              nlohmann::json::array() } }));
        break;
    case MsgType::GetPlayback:
        s.srv.SendMsg(MsgType::Playback, m.request_id,
                      nlohmann::json::object());
        break;
    case MsgType::GetDeliveryStats:
        s.srv.SendMsg(MsgType::DeliveryStats, m.request_id,
                      nlohmann::json::object());
        break;
    case MsgType::SetScene:
        s.last_scene_body = body;
        ++s.scene_saves;
        s.doc = body.value("document",
                           nlohmann::json::object());
        ++s.revision;
        s.srv.SendMsg(MsgType::Ok, m.request_id,
            nlohmann::json::object(
                { { "revision", s.revision } }));
        s.srv.SendMsg(MsgType::Event, 0,
            nlohmann::json::object({
                { "kind", "scene" },
                { "data", SceneEnvelope(s) },
            }));
        break;
    default:
        s.srv.SendMsg(MsgType::Error, m.request_id,
            nlohmann::json::object({
                { "code",    (uint32_t)ErrCode::BadRequest },
                { "error",   "bad_request" },
                { "message", "unexpected request" },
            }));
        break;
    }
    return true;
}

template <typename Pred>
static bool PumpBoth(Server& s, Pred pred, int ms = 12000)
{
    const auto deadline = std::chrono::steady_clock::now()
        + std::chrono::milliseconds(ms);
    while(std::chrono::steady_clock::now() < deadline)
    {
        if(pred())
        {
            return true;
        }
        ServeOnce(s, 10);
        QCoreApplication::processEvents(QEventLoop::AllEvents,
                                        10);
    }
    return pred();
}

int main(int argc, char** argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QGuiApplication app(argc, argv);
    QQuickStyle::setStyle(QStringLiteral("Basic"));

    auto         pipe = std::make_shared<TestPipe>();
    Server       s(pipe->Peer());
    EngineBridge bridge;
    bridge.setChannelFactoryForTesting(
        [&](std::string& err)
            -> std::unique_ptr<IIpcChannel>
        {
            (void)err;
            return pipe->ClientChannel();
        });
    EngineWireClient::Options wopts;
    wopts.keepalive_ms = 0;
    bridge.setWireOptionsForTesting(wopts);

    SceneEditController    scene_edit(&bridge);
    LayoutController       layout_ctl(&bridge);
    DiagnosticsController  diag_ctl(&bridge);

    QQmlEngine engine;
    engine.rootContext()->setContextProperty(
        QStringLiteral("bridge"), &bridge);
    engine.rootContext()->setContextProperty(
        QStringLiteral("sceneEdit"), &scene_edit);
    engine.rootContext()->setContextProperty(
        QStringLiteral("layoutCtl"), &layout_ctl);
    engine.rootContext()->setContextProperty(
        QStringLiteral("diagCtl"), &diag_ctl);

    bridge.start();
    CHECK(PumpBoth(s, [&] { return scene_edit.hasScene(); }));
    CHECK(scene_edit.sceneName() == QStringLiteral("desk"));

    /*---- the real shell — Main.qml instantiates the real
           LightingPage (nameField + Save wiring under test) */
    QQmlComponent comp(&engine,
        QUrl(QStringLiteral("qrc:/qt/qml/OpenRGB2/UI/Main.qml")));
    if(comp.isError())
    {
        ++failures;
        std::printf("FAIL load Main.qml:\n%s\n",
                    comp.errorString().toUtf8().constData());
        std::printf("%d checks, %d failures\n", checks, failures);
        return 1;
    }
    std::unique_ptr<QObject> win(
        comp.create(engine.rootContext()));
    CHECK(win != nullptr);
    if(!win)
    {
        std::printf("%d checks, %d failures\n", checks, failures);
        return 1;
    }
    QCoreApplication::processEvents();

    QObject* nameField =
        win->findChild<QObject*>(QStringLiteral("nameField"));
    QObject* saveBtn =
        win->findChild<QObject*>(QStringLiteral("saveButton"));
    CHECK(nameField != nullptr);
    CHECK(saveBtn != nullptr);
    if(!nameField || !saveBtn)
    {
        std::printf("%d checks, %d failures\n", checks, failures);
        return 1;
    }

    /*---- THE REGRESSION: focused, uncommitted text ----------
       Focus the field and type — text changes but
       editingFinished never fires, so the working copy still
       holds "desk". The pre-fix Save sent exactly that. */
    CHECK(PumpBoth(s, [&] {
        return nameField->property("text").toString()
               == QStringLiteral("desk");
    }));
    nameField->setProperty("focus", true);
    nameField->setProperty("text",
                           QStringLiteral("renamed-scene"));
    QCoreApplication::processEvents();
    /* uncommitted: the model never saw it — the field still
       owns it (the bug was Save ignoring exactly this) */
    CHECK(scene_edit.sceneName() == QStringLiteral("desk"));
    CHECK(!scene_edit.dirty());
    CHECK(nameField->property("focus").toBool());
    /* the Save button stays reachable for an uncommitted-only
       edit — dirty alone can't gate it */
    CHECK(PumpBoth(s, [&] {
        return saveBtn->property("enabled").toBool();
    }));

    /* Save through the page's real click path
       (commitPendingEdits via onClicked). */
    QMetaObject::invokeMethod(saveBtn, "clicked",
                              Qt::DirectConnection);
    CHECK(PumpBoth(s, [&] { return s.scene_saves >= 1; }));
    /* the wire body carries the EDITED name — the field's
       uncommitted text was committed, never the stale one */
    CHECK(s.last_scene_body["document"]["name"]
              .get<std::string>() == "renamed-scene");
    CHECK(scene_edit.sceneName() ==
          QStringLiteral("renamed-scene"));
    CHECK(PumpBoth(s, [&] {
        return !scene_edit.saving() && !scene_edit.dirty();
    }));
    CHECK(scene_edit.saveError().isEmpty());
    /* the engine's stored document agrees — the saved value,
       not the stale baseline */
    CHECK(s.doc["name"].get<std::string>() == "renamed-scene");

    bridge.stop();
    std::printf("qml_focused_save_test: %d checks, %d failures\n",
                checks, failures);
    return failures ? 1 : 0;
}
