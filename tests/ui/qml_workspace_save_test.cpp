// SPDX-License-Identifier: GPL-2.0-or-later
//
// Workspace-mode no-op-save regression (studio-import Task 4,
// fix round 2): commitPendingEdits() runs
// setSceneName(nameField.text) BEFORE sceneEdit.save(). When
// the field text already matches the draft's name the ws-mode
// rename is a NO-OP — which must be reported as success
// (SetName parity), never as the refusal that used to abort
// the save path entirely.
//
// Drives the REAL shell — Main.qml + LightingPage + the
// conflict strip's wsKeepDraft — over the in-process pipe
// (test_pipe.h) against a scripted studio_workspace_v1 engine:
//   1. a non-name edit dirties the shared draft
//   2. a remote commit moves the baseline -> the strip's
//      "Keep my draft (Save)" becomes reachable
//   3. clicking it with the name field UNCHANGED must still
//      fire a workspace commit job carrying the draft
//   4. Lighting's own saveButton with an unchanged name has
//      the same contract
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
#include <QQuickStyle>
#include <QQuickWindow>
#include <QTemporaryDir>
#include <QVariantMap>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <map>
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

static std::string Sha256HexStr(const std::string& s)
{
    return QCryptographicHash::hash(
        QByteArray::fromStdString(s),
        QCryptographicHash::Sha256).toHex().toStdString();
}

/*---------------------------------------------------------*\
|| Minimal valid compact v3 workspace document + a scripted ||
|| workspace engine (the verbs this flow exercises only).  ||
\*---------------------------------------------------------*/
static nlohmann::json CompactDoc(const std::string& name = "desk")
{
    return nlohmann::json::object({
        { "schema_version", 3 },
        { "name",           name },
        { "devices", nlohmann::json::object({
            { "fan0", nlohmann::json::object({
                { "type", "slw_fan" }, { "x", 0.1 },
                { "y", 0.0 }, { "z", 0.2 } }) },
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

struct WsServer
{
    explicit WsServer(ServerPeer p) : srv(std::move(p)) {}

    ServerPeer    srv;
    std::string   ws_id   = "ws0";
    uint64_t      ws_rev  = 1;
    uint64_t      res_rev = 1;
    uint64_t      next_job = 1;
    nlohmann::json doc    = CompactDoc();

    int           commits = 0;
    nlohmann::json last_commit_doc;

    std::string DocBytesFor() const { return doc.dump(2); }
    std::string DocSha() const
    {
        return Sha256HexStr(DocBytesFor());
    }

    nlohmann::json Snapshot() const
    {
        return nlohmann::json::object({
            { "managed",            true },
            { "id",                 ws_id },
            { "name",               doc.value("name",
                                              std::string("desk")) },
            { "workspace_revision", ws_rev },
            { "resource_revision",  res_rev },
            { "generation",         ws_rev },
            { "content_sha256",     DocSha() },
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
                    { "scene", nlohmann::json::object(
                        {{ "loaded", false }}) },
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
            nlohmann::json::object({{ "loaded", false }}));
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
            }));
        break;
    case MsgType::GetPresets:
        e.srv.SendMsg(MsgType::Presets, m.request_id,
            nlohmann::json::object(
                {{ "presets", nlohmann::json::array() }}));
        break;
    case MsgType::GetPlayback:
        e.srv.SendMsg(MsgType::Playback, m.request_id,
                      nlohmann::json::object());
        break;
    case MsgType::GetDeliveryStats:
        e.srv.SendMsg(MsgType::DeliveryStats, m.request_id,
                      nlohmann::json::object());
        break;
    case MsgType::Ping:
        e.srv.SendMsg(MsgType::Pong, m.request_id,
                      nlohmann::json::object());
        break;
    case MsgType::WorkspaceCommand:
    {
        const std::string verb =
            body.value("verb", std::string());
        if(verb == "get")
        {
            e.srv.SendMsg(MsgType::Ok, m.request_id,
                          e.Snapshot());
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
            const uint64_t off =
                body.value("offset", (uint64_t)0);
            const uint64_t mx =
                body.value("max_bytes", (uint64_t)400);
            const uint64_t n =
                off < bytes.size()
                    ? std::min<uint64_t>(mx, bytes.size() - off)
                    : 0;
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
            const uint64_t job = e.next_job++;
            e.last_commit_doc =
                body.value("document", nlohmann::json());
            e.srv.SendMsg(MsgType::Ok, m.request_id,
                nlohmann::json::object({
                    { "accepted", true },
                    { "job_id",   job },
                }));
            /* The commit lands: the engine stores the submitted
               document verbatim and reports the terminal event. */
            ++e.commits;
            e.doc = e.last_commit_doc;
            ++e.ws_rev;
            e.SendEvent(nlohmann::json::object({
                { "found",              true  },
                { "job_id",             job   },
                { "session_id",         1     },
                { "verb",               "commit" },
                { "state",              "succeeded" },
                { "committed",          true  },
                { "applied",            true  },
                { "generation",         e.ws_rev },
                { "workspace_revision", e.ws_rev },
                { "resource_revision",  e.res_rev },
                { "result", nlohmann::json::object() },
            }));
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
               && scene_edit.hasWorking();
    }));
    CHECK(scene_edit.sceneName() == QStringLiteral("desk"));

    /*---- the real shell ------------------------------------*/
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
    QObject* keepBtn =
        win->findChild<QObject*>(QStringLiteral("wsKeepDraft"));
    QObject* saveBtn =
        win->findChild<QObject*>(QStringLiteral("saveButton"));
    CHECK(nameField != nullptr);
    CHECK(keepBtn != nullptr);
    CHECK(saveBtn != nullptr);
    if(!nameField || !keepBtn || !saveBtn)
    {
        std::printf("%d checks, %d failures\n", checks, failures);
        return 1;
    }
    CHECK(PumpBoth(s, [&] {
        return nameField->property("text").toString()
               == QStringLiteral("desk");
    }));

    /*---- THE REGRESSION ------------------------------------
       A non-name edit dirties the draft; a remote commit moves
       the baseline -> conflict. The strip's "Keep my draft
       (Save)" runs commitPendingEdits() with the name field
       UNCHANGED ("desk" == draft name): pre-fix the no-op
       rename returned false, aborted the function, and no
       commit job ever reached the engine. */
    scene_edit.setBrightness(0.5);
    CHECK(PumpBoth(s, [&] { return ws_ctl.dirty(); }));

    s.doc["name"] = "other-client";
    ++s.ws_rev;
    s.SendEvent(s.Snapshot());
    CHECK(PumpBoth(s, [&] { return ws_ctl.conflict(); }));
    CHECK(PumpBoth(s, [&] {
        return keepBtn->property("visible").toBool();
    }));

    QMetaObject::invokeMethod(keepBtn, "clicked",
                              Qt::DirectConnection);
    CHECK(PumpBoth(s, [&] { return s.commits >= 1; }));
    /* the draft rode the commit — OUR name, not the remote's */
    CHECK(s.last_commit_doc.value("name", std::string())
          == "desk");
    CHECK(s.last_commit_doc["output"]["brightness"]
              .get<double>() == 0.5);
    CHECK(PumpBoth(s, [&] {
        return !ws_ctl.saving() && !ws_ctl.dirty()
               && !ws_ctl.conflict();
    }));
    CHECK(ws_ctl.error().isEmpty());

    /*---- same contract through Lighting's own Save ---------*/
    scene_edit.setBrightness(0.75);
    CHECK(PumpBoth(s, [&] {
        return saveBtn->property("enabled").toBool();
    }));
    QMetaObject::invokeMethod(saveBtn, "clicked",
                              Qt::DirectConnection);
    CHECK(PumpBoth(s, [&] { return s.commits >= 2; }));
    CHECK(s.last_commit_doc.value("name", std::string())
          == "desk");
    CHECK(PumpBoth(s, [&] {
        return !ws_ctl.saving() && !ws_ctl.dirty();
    }));

    bridge.stop();
    std::printf("qml_workspace_save_test: %d checks, "
                "%d failures\n", checks, failures);
    return failures ? 1 : 0;
}
