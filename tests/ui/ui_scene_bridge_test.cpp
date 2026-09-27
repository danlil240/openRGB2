// SPDX-License-Identifier: GPL-2.0-or-later
//
// Scene-edit integration test (Task 4.2) — a real
// EngineBridge + SceneEditController over the in-process
// wire pipe (test_pipe.h) against a scripted engine peer,
// proving end to end:
//
//   - scene edit -> SET_SCENE body carries the working
//     document (version as integer, effect, colors)
//   - OK reply commits; the engine's scene EVENT echo
//     (the post-apply refresh) keeps the clean state
//   - ERROR reply surfaces saveError AND preserves the
//     edit — retryable, never silently dropped
//   - sendPlaybackParams emits ONLY the fields the preset's
//     contract support row allows, and preserves the
//     engine's paused control rather than silently resuming
//   - device groups resolve against topology snapshots by
//     name, not enumeration index
//
// CHECK-macro main like the other UI suites — no framework.

#include "EngineBridge.h"
#include "SceneEditController.h"

#include "test_pipe.h"

#include <QCoreApplication>
#include <QThread>
#include <QVariantMap>

#include <chrono>
#include <cstdio>
#include <functional>
#include <memory>

using namespace openrgb2;
using namespace openrgb2::ipc;
using namespace ui_test;

static int checks   = 0;
static int failures = 0;
#define CHECK(x) do { ++checks; if(!(x)) { ++failures; \
    std::printf("FAIL line %d: %s\n", __LINE__, #x); } } while(0)

static nlohmann::json SceneFixture()
{
    const nlohmann::json pos = nlohmann::json::object(
        {{"x",0},{"y",0},{"z",0}});
    return nlohmann::json::object({
        { "version",    2 },
        { "name",       "desk" },
        { "brightness", 1.0 },
        { "bindings", nlohmann::json::array({
            nlohmann::json::object({
                { "id",              "slw0" },
                { "controller_name", "UNI FAN" },
                { "vendor",          "Lian Li" },
                { "serial",          "" },
                { "location",        "" },
                { "device_type",     -1 },
                { "zone_name",       "Fan 1" },
                { "zone_leds",       40 },
            }),
        }) },
        { "objects", nlohmann::json::array({
            nlohmann::json::object({
                { "id",        "fan0" },
                { "label",     "SLW Fan 0" },
                { "kind",      "device" },
                { "position",  pos },
                { "rotation",  pos },
                { "scale",     nlohmann::json::object(
                                   {{"x",1},{"y",1},{"z",1}}) },
                { "size_m",    pos },
                { "binding",   "slw0" },
                { "geometry",  "fan-slw" },
                { "verified",  true },
                { "visible",   true },
                { "emitters",  nlohmann::json::array({
                    nlohmann::json::object({
                        { "pos", pos }, { "group", "fan0" },
                        { "address", 0 } }) }) },
            }),
        }) },
        { "object_colors", nlohmann::json::object(
            { { "fan0", "#00B4A0" } }) },
        { "emitter_colors", nlohmann::json::object() },
        { "effect", nlohmann::json::object({
            { "preset",    "aurora" },
            { "seed",      0   },
            { "speed",     1.0 },
            { "intensity", 1.0 },
            { "playing",   true },
            { "layers",    nlohmann::json::array() },
        }) },
    });
}

struct Server
{
    explicit Server(ServerPeer peer) : srv(std::move(peer)) {}
    ServerPeer   srv;
    nlohmann::json doc = SceneFixture();
    int          revision       = 1;
    bool         fail_next_scene = false;
    int          scene_saves    = 0;
    int          mode_requests  = 0;
    nlohmann::json last_scene_body;
    nlohmann::json last_mode_body;
};

/* The WELCOME body mirrors DriveServerHandshake — `state`
   carries the GET_STATE payload, including the full scene
   document and the playback status (control:"paused" here —
   the UI must preserve that rather than silently resume). */
static nlohmann::json WelcomeBody(const nlohmann::json& doc)
{
    return nlohmann::json::object({
        { "server_version",     1 },
        { "negotiated_version", 1 },
        { "channel",            "command" },
        { "engine",             "openrgb2-engine" },
        { "running",            true },
        { "state", nlohmann::json::object({
            { "engine", nlohmann::json::object({
                { "running", true }, { "scene_clock_hz", 60 } }) },
            { "scene", nlohmann::json::object({
                { "loaded", true }, { "revision", 1 },
                { "name", "desk" }, { "playing", true },
                { "document", doc } }) },
            { "sdk", nlohmann::json::object({
                { "state", "connected" },
                { "reconnect_attempt", 0 } }) },
            { "playback", nlohmann::json::object({
                { "requested_mode", "animation_playback" },
                { "pending", false },
                { "status", nlohmann::json::object({
                    { "control", "paused" },
                    { "running", true } }) } }) },
        }) },
    });
}

static nlohmann::json SceneEnvelope(const Server& s)
{
    return nlohmann::json::object({
        { "loaded",   true },
        { "revision", s.revision },
        { "name",     "desk" },
        { "playing",  true },
        { "document", s.doc },
    });
}

/* One request/response round on the scripted engine. */
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
    case MsgType::GetPresets:
        s.srv.SendMsg(MsgType::Presets, m.request_id,
            nlohmann::json::object(
                { { "presets", nlohmann::json::array({
                    nlohmann::json::object({
                        { "id",   "aurora" },
                        { "name", "Aurora" } }),
                }) } }));
        break;
    case MsgType::GetDeliveryStats:
        s.srv.SendMsg(MsgType::DeliveryStats, m.request_id,
                      nlohmann::json::object());
        break;
    case MsgType::GetState:
        s.srv.SendMsg(MsgType::State, m.request_id,
            nlohmann::json::object({
                { "engine",   nlohmann::json::object(
                    {{"running",true}}) },
                { "scene",    SceneEnvelope(s) },
                { "playback", nlohmann::json::object({
                    { "requested_mode", "animation_playback" },
                    { "status", nlohmann::json::object(
                        {{"control","paused"}}) } }) },
            }));
        break;
    case MsgType::GetScene:
        s.srv.SendMsg(MsgType::Scene, m.request_id,
                      SceneEnvelope(s));
        break;
    case MsgType::GetPlayback:
        s.srv.SendMsg(MsgType::Playback, m.request_id,
            nlohmann::json::object({
                { "requested_mode", "animation_playback" },
                { "status", nlohmann::json::object(
                    {{"control","paused"}}) },
            }));
        break;
    case MsgType::SetScene:
        s.last_scene_body = body;
        ++s.scene_saves;
        if(s.fail_next_scene)
        {
            s.fail_next_scene = false;
            s.srv.SendMsg(MsgType::Error, m.request_id,
                nlohmann::json::object({
                    { "code",    (uint32_t)ErrCode::BadRequest },
                    { "error",   "bad_request" },
                    { "message", "refused by test" },
                }));
        }
        else
        {
            s.doc = body.value("document",
                               nlohmann::json::object());
            ++s.revision;
            s.srv.SendMsg(MsgType::Ok, m.request_id,
                nlohmann::json::object(
                    { { "revision", s.revision },
                      { "name",     "desk" } }));
            /* The engine emits a scene event on apply — the
               applied-state refresh the task requires. Echo
               the stored document so the UI converges. */
            s.srv.SendMsg(MsgType::Event, 0,
                nlohmann::json::object({
                    { "kind", "scene" },
                    { "data", SceneEnvelope(s) },
                }));
        }
        break;
    case MsgType::SetPlaybackMode:
        s.last_mode_body = body;
        ++s.mode_requests;
        s.srv.SendMsg(MsgType::Ok, m.request_id,
            nlohmann::json::object({
                { "requested_mode",
                  body.value("mode", std::string()) },
                { "applied", true },
                { "pending", false },
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

/* The scripted engine and the GUI-side bridge both make
   progress only while this thread pumps — alternate a short
   server round with a short event-loop round. */
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
    QCoreApplication app(argc, argv);

    auto            pipe = std::make_shared<TestPipe>();
    Server          s(pipe->Peer());
    ui::EngineBridge bridge;

    bridge.setChannelFactoryForTesting(
        [&](std::string& err)
            -> std::unique_ptr<IIpcChannel>
        {
            (void)err;
            return pipe->ClientChannel();
        });
    ui::EngineWireClient::Options wopts;
    wopts.keepalive_ms = 0;
    bridge.setWireOptionsForTesting(wopts);

    /* The controller must exist before WELCOME lands so the
       applied scene seeds the baseline. */
    ui::SceneEditController edit(&bridge);
    bridge.start();

    /*---- handshake + applied scene baseline ---------------*/
    std::printf("-- handshake + applied scene\n");
    CHECK(PumpBoth(s, [&] { return edit.hasScene(); }));
    CHECK(!edit.dirty());
    CHECK(bridge.scene()
              .value(QStringLiteral("document")).toMap()
              .value(QStringLiteral("effect")).toMap()
              .value(QStringLiteral("preset")).toString()
          == QStringLiteral("aurora"));

    /*---- groups resolve against topology by name ----------*/
    std::printf("-- device groups\n");
    s.srv.SendMsg(MsgType::Event, 0,
        nlohmann::json::object({
            { "kind", "topology" },
            { "data", nlohmann::json::object({
                { "snapshots", nlohmann::json::array({
                    nlohmann::json::object({
                        { "key", nlohmann::json::object({
                            { "device",   "Lian Li SL Wireless" },
                            { "serial",   "SN-1" },
                            { "location", "usb:1/3" },
                            { "zone",     "" } }) },
                        { "name",       "Lian Li UNI FAN SL "
                                        "Wireless" },
                        { "connection", "connected" },
                        { "led_count",  120 },
                        { "zones", nlohmann::json::array({
                            nlohmann::json::object({
                                { "name",      "Fan 1" },
                                { "led_count", 40 } }) }) },
                    }),
                }) },
            }) },
        }));
    CHECK(PumpBoth(s, [&] {
        const QVariantList g = edit.deviceGroups();
        return g.size() == 1
               && g[0].toMap().value(QStringLiteral("state"))
                      .toString() == QStringLiteral("resolved")
               && g[0].toMap().value(QStringLiteral("connected"))
                      .toBool();
    }));

    /*---- scene edit -> SET_SCENE body ----------------------*/
    std::printf("-- SET_SCENE body\n");
    CHECK(edit.setPreset(QStringLiteral("breathing")));
    CHECK(edit.dirty());
    edit.setBrightness(0.5);
    edit.toggleGroup(QStringLiteral("fan0"));
    edit.save();
    CHECK(PumpBoth(s, [&] { return s.scene_saves >= 1; }));
    const nlohmann::json& body = s.last_scene_body;
    CHECK(body.contains("document"));
    const nlohmann::json& d = body["document"];
    CHECK(d["version"].is_number_integer());
    CHECK(d["version"].get<int>() == 2);
    CHECK(d["effect"]["preset"] == "breathing");
    CHECK(d["effect"]["playing"].get<bool>());
    CHECK(std::abs(d["brightness"].get<double>() - 0.5)
          < 0.001);
    CHECK(d["object_colors"]["fan0"] == "#00B4A0");

    /* OK commits; the scene EVENT echo (post-apply refresh)
       converges to the stored document — clean state. */
    CHECK(PumpBoth(s, [&] {
        return !edit.saving() && !edit.dirty();
    }));
    CHECK(edit.saveError().isEmpty());
    CHECK(bridge.scene()
              .value(QStringLiteral("document")).toMap()
              .value(QStringLiteral("effect")).toMap()
              .value(QStringLiteral("preset")).toString()
          == QStringLiteral("breathing"));

    /*---- save failure surfaces + preserves the edit --------*/
    std::printf("-- save failure\n");
    edit.setBrightness(0.25);
    CHECK(edit.dirty());
    s.fail_next_scene = true;
    edit.save();
    CHECK(PumpBoth(s, [&] { return s.scene_saves >= 2; }));
    CHECK(PumpBoth(s, [&] {
        return !edit.saving() && !edit.saveError().isEmpty();
    }));
    CHECK(edit.saveError().contains(QStringLiteral("refused")));
    /* The uncommitted edit survived the failure — retryable. */
    CHECK(edit.dirty());
    edit.save();                    /* retry now succeeds   */
    CHECK(PumpBoth(s, [&] { return s.scene_saves >= 3; }));
    CHECK(PumpBoth(s, [&] {
        return !edit.saving() && !edit.dirty();
    }));
    CHECK(edit.saveError().isEmpty());
    CHECK(std::abs(s.doc["brightness"].get<double>() - 0.25)
          < 0.001);

    /*---- params emit only supported fields ------------------*/
    std::printf("-- playback param filtering\n");
    /* breathing supports speed + color, NOT direction — the
       emitted params must contain only supported keys, and
       the engine's paused control must be preserved rather
       than silently resumed. */
    const qlonglong sent =
        edit.sendPlaybackParams(150, true,
                                QStringLiteral("#00FF10"));
    CHECK(sent > 0);
    CHECK(PumpBoth(s, [&] { return s.mode_requests >= 1; }));
    CHECK(s.last_mode_body.value("control", "") == "paused");
    const nlohmann::json& params = s.last_mode_body["params"];
    CHECK(params.is_object());
    CHECK(params["speed"].get<double>() == 150.0);
    CHECK(params["color"] == "#00FF10");
    CHECK(!params.contains("reverse"));

    /* A non-playback preset must not emit params at all. */
    CHECK(edit.setPreset(QStringLiteral("aurora")));
    CHECK(edit.sendPlaybackParams(150, false, "") == 0);
    /* Give the wire a beat to deliver anything it shouldn't. */
    CHECK(!PumpBoth(s, [&] { return s.mode_requests >= 2; },
                    400));

    bridge.stop();
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
