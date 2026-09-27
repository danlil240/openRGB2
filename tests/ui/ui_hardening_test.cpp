// SPDX-License-Identifier: GPL-2.0-or-later
//
// Task 4.8 hardening tests — the failure/restart/responsiveness
// contracts the UI owes, driven end-to-end over the in-process
// pipe fixture:
//
//   - Quit All: the user-facing action issues IPC QUIT and
//     reads the ordered-shutdown reply (not a UI close)
//   - engine restart mid-edit: the working copy survives, the
//     restored session reports ENGINE truth (not a local
//     cache), and the reconnect issues no ghost mutations
//   - rejected SET_SCENE keeps the edits and surfaces the
//     refusal; a mid-save engine loss reports not-sent and
//     keeps the edits too
//   - responsiveness: control-request latency stays bounded
//     while the event queue is flooded (measurement method
//     documented at the probe)
//   - UiSettings: UI-pref round-trip, per-scene param seeds,
//     the bounded index, and clear()
//   - ProfileLibrary: a failed write never diverges memory
//     from the file
//   - playbackView: phase_detail clears when a new request
//     supersedes the failed state

#include "EngineBridge.h"
#include "ProfileLibrary.h"
#include "SceneEditController.h"
#include "UiSettings.h"

#include "test_pipe.h"

#include <QCoreApplication>
#include <QFile>
#include <QTemporaryDir>
#include <QThread>
#include <QVariantMap>

#include <chrono>
#include <cstdio>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>

using namespace openrgb2;
using namespace openrgb2::ipc;
using namespace ui_test;

static int checks   = 0;
static int failures = 0;
#define CHECK(x) do { ++checks; if(!(x)) { ++failures; \
    std::printf("FAIL line %d: %s\n", __LINE__, #x); } } while(0)

/*---------------------------------------------------------*\
|||| Scripted engine — records every client message so a   ||
|||| reconnect can be audited for ghost mutations.         ||
\*---------------------------------------------------------*/

static nlohmann::json DocFixture(const char* name)
{
    return nlohmann::json::object({
        { "version",    2    },
        { "name",       name },
        { "brightness", 1.0  },
        { "bindings",   nlohmann::json::array() },
        { "objects",    nlohmann::json::array() },
        { "object_colors",  nlohmann::json::object() },
        { "emitter_colors", nlohmann::json::object() },
        { "effect", nlohmann::json::object({
            { "preset",    "rainbow" },
            { "seed",      0    },
            { "speed",     1.0  },
            { "intensity", 1.0  },
            { "playing",   true },
            { "layers",    nlohmann::json::array() },
        }) },
    });
}

struct Engine
{
    explicit Engine(ServerPeer p) : srv(std::move(p)) {}
    ServerPeer     srv;
    nlohmann::json doc              = DocFixture("desk");
    int            revision         = 3;
    int            scene_saves      = 0;
    int            quits            = 0;
    bool           fail_set_scene   = false;
    bool           swallow_set_scene = false;
    std::vector<MsgType> seen;
};

static nlohmann::json SceneEnvelope(const Engine& e,
                                  const char* name = "desk")
{
    return nlohmann::json::object({
        { "loaded",   true },
        { "revision", e.revision },
        /* The envelope name is engine truth — it follows the
           document the scripted engine reports (a restarted
           engine may name a different applied scene). */
        { "name",     e.doc.value("name", name) },
        { "playing",  true },
        { "document", e.doc },
    });
}

static nlohmann::json WelcomeBody(const Engine& e,
                                  const char* name = "desk")
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
            { "scene",  SceneEnvelope(e, name) },
            { "sdk", nlohmann::json::object(
                {{ "state", "connected" },
                 { "reconnect_attempt", 0 }}) },
            { "playback", nlohmann::json::object({
                { "requested_mode", "live_effects" },
                { "pending", false },
                { "status", nlohmann::json::object({
                    { "control", "playing" },
                    { "mode",    "live_effects" } }) } }) },
        }) },
    });
}

/* One message: record the type, answer per the script. */
static bool ServeOnce(Engine& e, uint32_t wait_ms = 15)
{
    Message m;
    if(!e.srv.RecvMsg(m, wait_ms))
    {
        return false;
    }
    e.seen.push_back(m.type);
    const nlohmann::json body = m.Body();
    switch(m.type)
    {
    case MsgType::Hello:
        e.srv.SendMsg(MsgType::Welcome, 0, WelcomeBody(e));
        break;
    case MsgType::Subscribe:
        e.srv.SendMsg(MsgType::Ok, m.request_id,
            nlohmann::json::object(
                { { "subscribed", nlohmann::json::array({
                    "topology", "scene", "playback",
                    "connection", "frame_status" }) } }));
        e.srv.SendMsg(MsgType::Event, 0,
            nlohmann::json::object({
                { "kind", "scene" },
                { "data", SceneEnvelope(e) },
            }));
        break;
    case MsgType::GetScene:
        e.srv.SendMsg(MsgType::Scene, m.request_id,
                      SceneEnvelope(e));
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
                { "scene",  SceneEnvelope(e) },
            }));
        break;
    case MsgType::GetPresets:
        e.srv.SendMsg(MsgType::Presets, m.request_id,
            nlohmann::json::object(
                { { "presets", nlohmann::json::array() } }));
        break;
    case MsgType::GetDeliveryStats:
        e.srv.SendMsg(MsgType::DeliveryStats, m.request_id,
                      nlohmann::json::object());
        break;
    case MsgType::GetPlayback:
        e.srv.SendMsg(MsgType::Playback, m.request_id,
                      nlohmann::json::object());
        break;
    case MsgType::SetScene:
        ++e.scene_saves;
        if(e.swallow_set_scene)
        {
            break;         /* the reply never comes */
        }
        if(e.fail_set_scene)
        {
            e.fail_set_scene = false;
            e.srv.SendMsg(MsgType::Error, m.request_id,
                nlohmann::json::object({
                    { "code",    (uint32_t)ErrCode::BadRequest },
                    { "error",   "bad_request" },
                    { "message", "scene refused by test" },
                }));
            break;
        }
        e.doc = body.value("document", nlohmann::json::object());
        ++e.revision;
        e.srv.SendMsg(MsgType::Ok, m.request_id,
            nlohmann::json::object({ { "revision", e.revision } }));
        e.srv.SendMsg(MsgType::Event, 0,
            nlohmann::json::object({
                { "kind", "scene" },
                { "data", SceneEnvelope(e) },
            }));
        break;
    case MsgType::SetPlaybackMode:
        e.srv.SendMsg(MsgType::Ok, m.request_id,
                      nlohmann::json::object());
        break;
    case MsgType::LayoutCommand:
        e.srv.SendMsg(MsgType::Ok, m.request_id,
                      nlohmann::json::object());
        break;
    case MsgType::ReactiveCommand:
        e.srv.SendMsg(MsgType::Ok, m.request_id,
                      nlohmann::json::object());
        break;
    case MsgType::Quit:
        ++e.quits;
        e.srv.SendMsg(MsgType::Ok, m.request_id,
            nlohmann::json::object({
                { "shutting_down", true },
                { "ordered",       true },
            }));
        break;
    case MsgType::Ping:
        e.srv.SendMsg(MsgType::Pong, m.request_id,
                      nlohmann::json::object());
        break;
    default:
        break;
    }
    return true;
}

/* Pump the GUI event loop while serving the engine — every
   client message is recorded/answered until cond() or the
   deadline. */
static bool PumpUntil(Engine& e, std::function<bool()> cond,
                    int ms = 6000)
{
    const auto deadline = std::chrono::steady_clock::now()
        + std::chrono::milliseconds(ms);
    while(std::chrono::steady_clock::now() < deadline)
    {
        QCoreApplication::processEvents(QEventLoop::AllEvents,
                                        2);
        ServeOnce(e, 1);
        if(cond())
        {
            return true;
        }
        QThread::msleep(1);
    }
    for(int i = 0; i < 8; ++i)
    {
        QCoreApplication::processEvents(QEventLoop::AllEvents,
                                        5);
        ServeOnce(e, 1);
    }
    return cond();
}

/* A bridge wired to the fixture: factory pipes + no
   keepalives + a short backoff list. */
static void WireBridge(ui::EngineBridge& bridge,
                       std::vector<std::shared_ptr<TestPipe>>& pipes,
                       std::mutex& pm)
{
    bridge.setChannelFactoryForTesting(
        [&](std::string& err) -> std::unique_ptr<IIpcChannel>
        {
            (void)err;
            auto p = std::make_shared<TestPipe>();
            {
                std::lock_guard<std::mutex> lock(pm);
                pipes.push_back(p);
            }
            return p->ClientChannel();
        });
    ui::EngineWireClient::Options wopts;
    wopts.keepalive_ms = 0;
    bridge.setWireOptionsForTesting(wopts);
    bridge.setBackoffForTesting({ 20, 40, 80 });
}

static bool Counted(const Engine& e, MsgType t)
{
    for(const MsgType s : e.seen)
    {
        if(s == t)
        {
            return true;
        }
    }
    return false;
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    /*---- Quit All: IPC QUIT + ordered reply ------------------*/
    std::printf("-- quit-all issues QUIT, reads ordered reply\n");
    {
        ui::EngineBridge bridge;
        std::mutex pm;
        std::vector<std::shared_ptr<TestPipe>> pipes;
        WireBridge(bridge, pipes, pm);
        bridge.start();
        CHECK(WaitFor([&] {
            std::lock_guard<std::mutex> l(pm);
            return pipes.size() >= 1;
        }));
        Engine e(pipes[0]->Peer());
        CHECK(PumpUntil(e, [&] {
            return bridge.connectionState() == "connected";
        }));

        int         reply_id   = -1;
        QString     reply_type;
        QVariantMap reply_body;
        QObject::connect(&bridge, &ui::EngineBridge::replyReceived,
            [&](int id, const QString& type,
                const QVariantMap& body) {
                if(id == reply_id)
                {
                    reply_type = type;
                    reply_body = body;
                }
            });
        reply_id = bridge.quitEngine();
        CHECK(reply_id > 0);
        CHECK(PumpUntil(e, [&] { return e.quits == 1; }));
        CHECK(Counted(e, MsgType::Quit));
        CHECK(PumpUntil(e, [&] {
            return reply_type == "OK";
        }));
        CHECK(reply_body.value("ordered").toBool());
        CHECK(reply_body.value("shutting_down").toBool());

        /* A QUIT that the engine refuses still surfaces
           through the same correlator as an ERROR. */
        reply_id   = bridge.quitEngine();
        reply_type.clear();
        reply_body = QVariantMap();
        /* ServeOnce auto-OKs QUIT — force the error by
           answering this one manually. Consume until the
           second Quit arrives, then send ERROR instead of the
           scripted OK. */
        {
            const auto dl = std::chrono::steady_clock::now()
                + std::chrono::milliseconds(4000);
            bool got = false;
            while(!got && std::chrono::steady_clock::now() < dl)
            {
                QCoreApplication::processEvents(
                    QEventLoop::AllEvents, 2);
                Message m;
                if(e.srv.RecvMsg(m, 10))
                {
                    e.seen.push_back(m.type);
                    if(m.type == MsgType::Quit)
                    {
                        ++e.quits;
                        e.srv.SendMsg(MsgType::Error,
                            m.request_id,
                            nlohmann::json::object({
                                { "code", (uint32_t)ErrCode::Unavailable },
                                { "error", "unavailable" },
                                { "message",
                                  "shutdown already in progress" },
                            }));
                        got = true;
                    }
                    else
                    {
                        /* Let the scripted path answer it. */
                        e.srv.SendMsg(MsgType::Error,
                            m.request_id,
                            nlohmann::json::object(
                                { { "code", 1u },
                                  { "error", "malformed" },
                                  { "message", "x" } }));
                    }
                }
            }
            CHECK(got);
        }
        CHECK(PumpUntil(e, [&] { return reply_type == "ERROR"; }));
        CHECK(reply_body.value("message").toString()
                  .contains("shutdown"));
        bridge.stop();
    }

    /*---- Engine restart mid-edit ------------------------------
       The working copy survives the drop; the new session's
       scene is ENGINE truth (a different document the
       restarted engine reports), and nothing the reconnect
       emits is a mutation the user didn't ask for. */
    std::printf("-- restart mid-edit keeps edits, restores truth\n");
    {
        ui::EngineBridge bridge;
        std::mutex pm;
        std::vector<std::shared_ptr<TestPipe>> pipes;
        WireBridge(bridge, pipes, pm);
        ui::SceneEditController edit(&bridge);
        bridge.start();
        CHECK(WaitFor([&] {
            std::lock_guard<std::mutex> l(pm);
            return pipes.size() >= 1;
        }));
        Engine e(pipes[0]->Peer());
        CHECK(PumpUntil(e, [&] {
            return bridge.connectionState() == "connected"
                   && edit.hasWorking();
        }));

        /* Uncommitted edits. */
        CHECK(edit.setSceneName(QStringLiteral("renamed-desk")));
        edit.setBrightness(0.5);
        CHECK(edit.dirty());

        /* Engine vanishes. */
        e.srv.Close();
        CHECK(WaitFor([&] {
            std::lock_guard<std::mutex> l(pm);
            return pipes.size() >= 2;
        }, 8000));
        Engine e2(pipes[1]->Peer());
        /* The "restarted" engine reports a DIFFERENT applied
           scene — replayed truth, not the UI's memory. */
        e2.doc      = DocFixture("desk-restarted");
        e2.revision = 9;
        CHECK(PumpUntil(e2, [&] {
            return bridge.connectionState() == "connected";
        }));
        CHECK(PumpUntil(e2, [&] {
            return bridge.scene().value("name").toString()
                   == "desk-restarted";
        }));

        /* UI-owned edits are still there — and the conflict
           flag honestly says the applied scene moved. */
        CHECK(edit.dirty());
        CHECK(edit.conflict());
        /* The restart itself must not restart hardware or
           re-issue user intent: audit every message the new
           session sent — only reads/subscribe, never a
           mutation. */
        const auto dl = std::chrono::steady_clock::now()
            + std::chrono::milliseconds(400);
        while(std::chrono::steady_clock::now() < dl)
        {
            QCoreApplication::processEvents(
                QEventLoop::AllEvents, 2);
            ServeOnce(e2, 5);
        }
        CHECK(!Counted(e2, MsgType::SetScene));
        CHECK(!Counted(e2, MsgType::SetPlaybackMode));
        CHECK(!Counted(e2, MsgType::LayoutCommand));
        CHECK(!Counted(e2, MsgType::Quit));
        bridge.stop();
    }

    /*---- Rejected SET_SCENE keeps the edits -------------------*/
    std::printf("-- rejected save keeps edits + error\n");
    {
        ui::EngineBridge bridge;
        std::mutex pm;
        std::vector<std::shared_ptr<TestPipe>> pipes;
        WireBridge(bridge, pipes, pm);
        ui::SceneEditController edit(&bridge);
        bridge.start();
        CHECK(WaitFor([&] {
            std::lock_guard<std::mutex> l(pm);
            return pipes.size() >= 1;
        }));
        Engine e(pipes[0]->Peer());
        CHECK(PumpUntil(e, [&] {
            return bridge.connectionState() == "connected"
                   && edit.hasWorking();
        }));
        CHECK(edit.setSceneName(QStringLiteral("rejected")));
        CHECK(edit.dirty());
        e.fail_set_scene = true;
        edit.save();
        CHECK(PumpUntil(e, [&] {
            return edit.saveError() != "";
        }));
        CHECK(e.scene_saves == 1);
        CHECK(edit.saveErrorSource() == "engine");
        /* The edit is still exactly what the user typed — a
           refused write never silently reverts. */
        CHECK(edit.dirty());
        CHECK(edit.sceneName() == "rejected");
        bridge.stop();
    }

    /*---- Engine loss mid-save: honest not-sent ----------------*/
    std::printf("-- engine loss mid-save keeps edits\n");
    {
        ui::EngineBridge bridge;
        std::mutex pm;
        std::vector<std::shared_ptr<TestPipe>> pipes;
        WireBridge(bridge, pipes, pm);
        ui::SceneEditController edit(&bridge);
        bridge.start();
        CHECK(WaitFor([&] {
            std::lock_guard<std::mutex> l(pm);
            return pipes.size() >= 1;
        }));
        Engine e(pipes[0]->Peer());
        CHECK(PumpUntil(e, [&] {
            return bridge.connectionState() == "connected"
                   && edit.hasWorking();
        }));
        CHECK(edit.setSceneName(QStringLiteral("in-flight")));
        e.swallow_set_scene = true;
        edit.save();
        CHECK(edit.saving());
        CHECK(PumpUntil(e, [&] { return e.scene_saves >= 1; }));
        e.srv.Close();               /* reply can never land */
        CHECK(PumpUntil(e, [&] {
            return !edit.saving();
        }));
        /* saving cleared, the loss is the reported failure,
           and the edit is still sitting there for retry. */
        CHECK(edit.saveError() != "");
        CHECK(edit.saveErrorSource() == "connection");
        CHECK(edit.dirty());
        CHECK(edit.sceneName() == "in-flight");
        bridge.stop();
    }

    /*---- Responsiveness probe ----------------------------------
       Method: the server floods the marshalled event queue
       with 400 frame_status pushes; mid-flood the test issues
       a control request (getState — the same SendRequest path
       every QML control takes) and measures wall time from
       issue to the replyReceived signal on the GUI thread.
       Bound: the flood must drain AND the control round-trip
       must land — the probe fails if the GUI thread stalls
       the reply past 2 s. Honest scope: this measures
       marshalling + event-loop latency in the test harness,
       not end-user input sampling. */
    std::printf("-- flood: bounded control latency\n");
    {
        ui::EngineBridge bridge;
        std::mutex pm;
        std::vector<std::shared_ptr<TestPipe>> pipes;
        WireBridge(bridge, pipes, pm);
        bridge.start();
        CHECK(WaitFor([&] {
            std::lock_guard<std::mutex> l(pm);
            return pipes.size() >= 1;
        }));
        Engine e(pipes[0]->Peer());
        CHECK(PumpUntil(e, [&] {
            return bridge.connectionState() == "connected";
        }));

        int frames = 0;
        QObject::connect(&bridge,
                         &ui::EngineBridge::frameStatusChanged,
                         [&] { ++frames; });
        int     reply_id = -1;
        bool    replied  = false;
        QObject::connect(&bridge, &ui::EngineBridge::replyReceived,
            [&](int id, const QString&, const QVariantMap&) {
                if(id == reply_id)
                {
                    replied = true;
                }
            });

        const auto t0 = std::chrono::steady_clock::now();
        for(int i = 0; i < 400; ++i)
        {
            e.srv.SendMsg(MsgType::Event, 0,
                nlohmann::json::object({
                    { "kind", "frame_status" },
                    { "data", nlohmann::json::object({
                        { "frame", i } }) },
                }));
        }
        reply_id = bridge.getState();
        CHECK(reply_id > 0);
        long long control_ms = -1;
        const bool drained = PumpUntil(e, [&] {
            return replied && frames >= 400;
        }, 4000);
        control_ms = std::chrono::duration_cast<
            std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - t0).count();
        std::printf("  flood drain + control round-trip: %lld ms\n",
                    control_ms);
        CHECK(drained);
        CHECK(frames >= 400);
        CHECK(control_ms < 2000);
        bridge.stop();
    }

    /*---- playbackView: stale phase_detail clears --------------*/
    std::printf("-- phase_detail clears on applying\n");
    {
        ui::EngineBridge bridge;
        std::mutex pm;
        std::vector<std::shared_ptr<TestPipe>> pipes;
        WireBridge(bridge, pipes, pm);
        bridge.start();
        CHECK(WaitFor([&] {
            std::lock_guard<std::mutex> l(pm);
            return pipes.size() >= 1;
        }));
        Engine e(pipes[0]->Peer());
        CHECK(PumpUntil(e, [&] {
            return bridge.connectionState() == "connected";
        }));

        /* Failed upload — the detail belongs to the failure. */
        e.srv.SendMsg(MsgType::Event, 0,
            nlohmann::json::object({
                { "kind", "playback" },
                { "data", nlohmann::json::object({
                    { "requested_mode", "animation_playback" },
                    { "pending", false },
                    { "detail", "upload rejected: bad preset" },
                    { "status", nlohmann::json::object({
                        { "mode",    "animation_playback" },
                        { "upload",  "failed" },
                        { "control", "playing" } }) },
                }) },
            }));
        CHECK(PumpUntil(e, [&] {
            return bridge.playbackView().value("phase")
                       .toString() == "failed";
        }));
        CHECK(bridge.playbackView().value("phase_detail")
                  .toString().contains("bad preset"));

        /* The next request starts applying — the old failure
           text must not linger beside it. */
        e.srv.SendMsg(MsgType::Event, 0,
            nlohmann::json::object({
                { "kind", "playback" },
                { "data", nlohmann::json::object({
                    { "requested_mode", "animation_playback" },
                    { "pending", true },
                    { "detail", "upload rejected: bad preset" },
                    { "status", nlohmann::json::object({
                        { "mode",    "animation_playback" },
                        { "upload",  "failed" },
                        { "control", "playing" } }) },
                }) },
            }));
        CHECK(PumpUntil(e, [&] {
            return bridge.playbackView().value("phase")
                       .toString() == "applying";
        }));
        CHECK(bridge.playbackView().value("phase_detail")
                  .toString().isEmpty());
        bridge.stop();
    }

    /*---- UiSettings round-trip --------------------------------*/
    std::printf("-- ui settings persist + bound\n");
    {
        QTemporaryDir dir;
        CHECK(dir.isValid());
        const QString path =
            dir.filePath(QStringLiteral("prefs.ini"));
        ui::UiSettings s;
        s.setPathForTesting(path);
        s.setLastView(3);
        s.setRestoreLastView(false);
        s.saveGeometry(11, 22, 1024, 700);
        s.setPaletteHue(0.25);
        QVariantMap p;
        p.insert("speed",         150);
        p.insert("reverse",       true);
        p.insert("color",         "#00FF00");
        p.insert("touched_speed", true);
        s.SetSceneParams(QStringLiteral("desk"), p);

        /* A second instance on the same file = a restarted UI. */
        ui::UiSettings s2;
        s2.setPathForTesting(path);
        CHECK(s2.lastView() == 3);
        CHECK(!s2.restoreLastView());
        CHECK(s2.windowX() == 11 && s2.windowY() == 22);
        CHECK(s2.windowWidth() == 1024 && s2.windowHeight() == 700);
        CHECK(s2.paletteHue() == 0.25);
        const QVariantMap p2 = s2.SceneParams(QStringLiteral("desk"));
        CHECK(p2.value("speed").toInt() == 150);
        CHECK(p2.value("touched_speed").toBool());
        CHECK(s2.storageLocation() == path);

        /* Bounded: 80 scene names can't grow the index past
           the bound — the oldest are evicted. */
        for(int i = 0; i < 80; ++i)
        {
            s2.SetSceneParams(QStringLiteral("s%1").arg(i),
                              QVariantMap({{ "speed", i }}));
        }
        CHECK(s2.SceneParams(QStringLiteral("s79"))
                  .value("speed").toInt() == 79);
        /* evicted oldest reads empty */
        CHECK(s2.SceneParams(QStringLiteral("s0")).isEmpty());

        s2.clear();
        ui::UiSettings s3;
        s3.setPathForTesting(path);
        CHECK(s3.lastView() == 0);
        CHECK(s3.restoreLastView());   /* default returns */
        CHECK(s3.SceneParams(QStringLiteral("desk")).isEmpty());
    }

    /*---- Per-scene params through the controller --------------*/
    std::printf("-- per-scene params persist + restore\n");
    {
        QTemporaryDir dir;
        CHECK(dir.isValid());
        ui::UiSettings prefs;
        prefs.setPathForTesting(
            dir.filePath(QStringLiteral("prefs.ini")));

        ui::EngineBridge bridge;
        std::mutex pm;
        std::vector<std::shared_ptr<TestPipe>> pipes;
        WireBridge(bridge, pipes, pm);
        ui::SceneEditController edit(&bridge);
        edit.setUiSettings(&prefs);
        bridge.start();
        CHECK(WaitFor([&] {
            std::lock_guard<std::mutex> l(pm);
            return pipes.size() >= 1;
        }));
        Engine e(pipes[0]->Peer());
        CHECK(PumpUntil(e, [&] {
            return bridge.connectionState() == "connected"
                   && edit.hasWorking();
        }));
        /* Touch persists eagerly under the applied scene's
           name — the restart seed. */
        edit.noteParamTouched(QStringLiteral("speed"));
        const QVariantMap stored =
            prefs.SceneParams(QStringLiteral("desk"));
        CHECK(stored.value("touched_speed").toBool());

        /* Seed a distinct value for "desk" — a UI restart's
           seed (the engine only replays revisions, not
           values). */
        QVariantMap seed;
        seed.insert("speed",         150);
        seed.insert("reverse",       false);
        seed.insert("color",         "#00FF00");
        seed.insert("touched_speed", true);
        prefs.SetSceneParams(QStringLiteral("desk"), seed);

        /* The applied name moves away... params reset to
           defaults (scene-scoped latches, Task 4.3). */
        e.doc = DocFixture("other");
        e.srv.SendMsg(MsgType::Event, 0,
            nlohmann::json::object({
                { "kind", "scene" },
                { "data", SceneEnvelope(e, "other") },
            }));
        CHECK(PumpUntil(e, [&] {
            return bridge.scene().value("name").toString()
                   == "other";
        }));
        CHECK(edit.paramSpeed() == 100);

        /* ...and back — the stored seed for "desk" rides
           back in: 150, not the 100 default. */
        e.doc = DocFixture("desk");
        e.srv.SendMsg(MsgType::Event, 0,
            nlohmann::json::object({
                { "kind", "scene" },
                { "data", SceneEnvelope(e, "desk") },
            }));
        CHECK(PumpUntil(e, [&] {
            return bridge.scene().value("name").toString()
                   == "desk";
        }));
        CHECK(edit.paramSpeed() == 150);
        bridge.stop();
    }

    /*---- ProfileLibrary failed-write rollback -----------------*/
    std::printf("-- library memory stays file-truth\n");
    {
        QTemporaryDir dir;
        CHECK(dir.isValid());
        const QString path =
            dir.filePath(QStringLiteral("lib.json"));
        ui::ProfileLibrary lib;
        lib.setPathForTesting(path);
        QVariantMap doc;
        doc.insert("name", QStringLiteral("alpha"));
        CHECK(lib.saveProfile(QStringLiteral("alpha"), doc));
        CHECK(lib.entries().size() == 1);

        /* Make the write fail but the read still work: the
           file becomes read-only — QSaveFile refuses. */
        QFile f(path);
        CHECK(QFile::setPermissions(
            path, QFileDevice::ReadOwner | QFileDevice::ReadUser
                  | QFileDevice::ReadGroup | QFileDevice::ReadOther));

        QVariantMap doc2;
        doc2.insert("name", QStringLiteral("alpha-v2"));
        CHECK(!lib.saveProfile(QStringLiteral("alpha"), doc2));
        CHECK(lib.lastError() != "");
        /* Memory rolled back to the file's truth: one entry,
           the OLD document — no phantom "v2". */
        CHECK(lib.entries().size() == 1);
        CHECK(lib.profileDocument(QStringLiteral("alpha"))
                  .value("name").toString() == "alpha");

        /* Remove fails the same way and restores the entry. */
        CHECK(!lib.removeProfile(QStringLiteral("alpha")));
        CHECK(lib.entries().size() == 1);
    }

    std::printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
