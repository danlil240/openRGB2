// SPDX-License-Identifier: GPL-2.0-or-later
//
// Reactive-input bridge test (Task 4.6) — a real EngineBridge
// over the in-process wire pipe (test_pipe.h) against a
// scripted engine peer, proving:
//
//   - the "reactive" surface lands from the welcome STATE and
//     from pushed events (available / capture_paused / per-input
//     enabled,running,status / displays)
//   - reactiveCommand() issues the exact REACTIVE_COMMAND
//     bodies ({verb:"set_enabled",input,enabled} /
//     {verb:"set_params",input,sensitivity|screen_index})
//   - the reply's full body and the pushed event both update
//     the store — the UI never assumes a toggle
//   - an engine ERROR surfaces through replyReceived with the
//     honest message, and the store is NOT fabricated
//   - available:false bodies carry the engine's reason — the
//     QML card gates on the engine's report, never guesses
//
// CHECK-macro main like the other UI suites — no framework.

#include "EngineBridge.h"

#include "test_pipe.h"

#include <QCoreApplication>
#include <QVariantMap>

#include <chrono>
#include <cstdio>
#include <memory>

using namespace openrgb2;
using namespace openrgb2::ipc;
using namespace ui_test;

static int checks   = 0;
static int failures = 0;
#define CHECK(x) do { ++checks; if(!(x)) { ++failures; \
    std::printf("FAIL line %d: %s\n", __LINE__, #x); } } while(0)

/* Scripted engine state. */
struct Server
{
    explicit Server(ServerPeer peer) : srv(std::move(peer)) {}
    ServerPeer    srv;
    int           reactive_requests = 0;
    nlohmann::json last_reactive_body;

    /* The reactive surface the scripted engine reports —
       tests mutate it, then push a reactive event. */
    bool   audio_enabled  = false;
    bool   screen_enabled = false;
    bool   key_enabled    = false;
    bool   paused         = false;
    bool   surface_avail  = true;
};

/* The REACTIVE body shape of docs/ipc-protocol.md §5. */
static nlohmann::json ReactiveBody(const Server& s)
{
    if(!s.surface_avail)
    {
        return nlohmann::json::object({
            { "available", false },
            { "reason",    "reactive inputs are not wired on "
                           "this engine" },
            { "inputs",    nlohmann::json::array() },
            { "displays",  nlohmann::json::array() },
        });
    }
    auto input = [](const char* id, bool on)
    {
        return nlohmann::json::object({
            { "id",           id     },
            { "available",    true   },
            { "enabled",      on     },
            { "running",      on     },
            { "paused",       false  },
            { "sensitivity",  1.0    },
            { "screen_index", 0      },
            { "status", on ? std::string(id) + ": listening"
                           : std::string(id) + ": off" },
        });
    };
    return nlohmann::json::object({
        { "available",      true       },
        { "capture_paused", s.paused   },
        { "inputs", nlohmann::json::array({
            input("audio",  s.audio_enabled),
            input("screen", s.screen_enabled),
            input("key",    s.key_enabled) }) },
        { "displays", nlohmann::json::array({
            nlohmann::json::object(
                {{"index",0},{"name","DISPLAY1"},
                 {"width",1920},{"height",1080}}),
            nlohmann::json::object(
                {{"index",1},{"name","DISPLAY2"},
                 {"width",2560},{"height",1440}}) }) },
    });
}

static nlohmann::json WelcomeBody(const Server& s)
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
                { "loaded", false }, { "revision", 0 } }) },
            { "sdk", nlohmann::json::object({
                { "state", "disconnected" } }) },
            { "reactive", ReactiveBody(s) },
        }) },
    });
}

static void PushReactive(Server& s)
{
    s.srv.SendMsg(MsgType::Event, 0,
        nlohmann::json::object({
            { "kind", "reactive" },
            { "data", ReactiveBody(s) },
        }));
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
        s.srv.SendMsg(MsgType::Welcome, 0, WelcomeBody(s));
        break;
    case MsgType::Subscribe:
        s.srv.SendMsg(MsgType::Ok, m.request_id,
            nlohmann::json::object(
                { { "subscribed", nlohmann::json::array({
                    "topology", "scene", "playback",
                    "connection", "frame_status",
                    "reactive" }) } }));
        break;
    case MsgType::GetPresets:
        s.srv.SendMsg(MsgType::Presets, m.request_id,
            nlohmann::json::object({ { "presets",
                nlohmann::json::array() } }));
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
                { "sdk",      nlohmann::json::object(
                    {{"state","disconnected"}}) },
                { "reactive", ReactiveBody(s) },
            }));
        break;
    case MsgType::GetScene:
        s.srv.SendMsg(MsgType::Scene, m.request_id,
            nlohmann::json::object(
                { { "loaded", false }, { "revision", 0 },
                  { "document", nlohmann::json(nullptr) } }));
        break;
    case MsgType::ReactiveCommand:
    {
        ++s.reactive_requests;
        s.last_reactive_body = body;
        const std::string verb  = body.value("verb",
                                             std::string());
        const std::string input = body.value("input",
                                             std::string());
        if(verb == "set_enabled")
        {
            const bool en = body.value("enabled", false);
            if(input == "audio")       s.audio_enabled  = en;
            else if(input == "screen") s.screen_enabled = en;
            else if(input == "key")    s.key_enabled    = en;
            s.srv.SendMsg(MsgType::Ok, m.request_id,
                          ReactiveBody(s));
            PushReactive(s);
        }
        else if(verb == "set_params")
        {
            s.srv.SendMsg(MsgType::Ok, m.request_id,
                          ReactiveBody(s));
            PushReactive(s);
        }
        else
        {
            s.srv.SendMsg(MsgType::Error, m.request_id,
                nlohmann::json::object({
                    { "code",    (uint32_t)ErrCode::BadRequest },
                    { "error",   "bad_request" },
                    { "message", "unknown reactive verb" },
                }));
        }
        break;
    }
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

/* Alternate a short server round with a short event-loop
   round — both sides only make progress while this pumps. */
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

static QVariantMap InputAt(const QVariantMap& reactive,
                           const QString& id)
{
    for(const QVariant& v :
            reactive.value(QStringLiteral("inputs")).toList())
    {
        const QVariantMap in = v.toMap();
        if(in.value(QStringLiteral("id")).toString() == id)
        {
            return in;
        }
    }
    return {};
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

    /* Reply/error surface — the wire's honest report. */
    QString     last_reply_type;
    QVariantMap last_reply_body;
    QObject::connect(&bridge, &ui::EngineBridge::replyReceived,
        [&](int, const QString& typeName,
            const QVariantMap& body)
        {
            last_reply_type = typeName;
            last_reply_body = body;
        });

    bridge.start();

    /*---- handshake: the reactive surface lands from STATE ---*/
    std::printf("-- handshake + reactive surface\n");
    CHECK(PumpBoth(s, [&] {
        return bridge.connectionState() ==
               QStringLiteral("connected");
    }));
    {
        const QVariantMap r = bridge.reactive();
        CHECK(r.value(QStringLiteral("available")).toBool());
        CHECK(!r.value(QStringLiteral("capture_paused"))
                   .toBool());
        CHECK(r.value(QStringLiteral("inputs")).toList()
                  .size() == 3);
        CHECK(InputAt(r, QStringLiteral("audio"))
                  .value(QStringLiteral("enabled")).toBool()
              == false);
        CHECK(r.value(QStringLiteral("displays")).toList()
                  .size() == 2);
    }

    /*---- set_enabled: exact wire body + state re-lands ------*/
    std::printf("-- set_enabled round-trip\n");
    const int req = bridge.reactiveCommand(
        QStringLiteral("set_enabled"),
        QVariantMap({ { QStringLiteral("input"),
                        QStringLiteral("audio") },
                      { QStringLiteral("enabled"), true } }));
    CHECK(req > 0);
    CHECK(PumpBoth(s, [&] { return s.reactive_requests >= 1; }));
    CHECK(s.last_reactive_body.value("verb", std::string())
              == "set_enabled");
    CHECK(s.last_reactive_body.value("input", std::string())
              == "audio");
    CHECK(s.last_reactive_body.value("enabled", false)
              == true);
    /* The pushed event updated the store — the UI never
       assumed the toggle. */
    CHECK(PumpBoth(s, [&] {
        return InputAt(bridge.reactive(),
                       QStringLiteral("audio"))
            .value(QStringLiteral("enabled")).toBool();
    }));
    CHECK(InputAt(bridge.reactive(), QStringLiteral("audio"))
              .value(QStringLiteral("running")).toBool());
    CHECK(InputAt(bridge.reactive(), QStringLiteral("audio"))
              .value(QStringLiteral("status")).toString()
              == QStringLiteral("audio: listening"));

    /*---- set_params: sensitivity + display index ------------*/
    std::printf("-- set_params round-trip\n");
    CHECK(bridge.reactiveCommand(
              QStringLiteral("set_params"),
              QVariantMap({ { QStringLiteral("input"),
                              QStringLiteral("audio") },
                            { QStringLiteral("sensitivity"),
                              1.5 } })) > 0);
    CHECK(bridge.reactiveCommand(
              QStringLiteral("set_params"),
              QVariantMap({ { QStringLiteral("input"),
                              QStringLiteral("screen") },
                            { QStringLiteral("screen_index"),
                              1 } })) > 0);
    CHECK(PumpBoth(s, [&] { return s.reactive_requests >= 3; }));
    CHECK(s.last_reactive_body.value("screen_index", -1) == 1);

    /*---- engine ERROR: honest surface, no fabrication --------*/
    std::printf("-- error honesty\n");
    {
        /* An engine that refuses an enable answers ERROR —
           the store must not move and replyReceived carries
           the wire type + message. */
        const int before = s.reactive_requests;
        /* rewire the handler inline via state flag: script a
           refusal by making the verb unknown-path… — simpler:
           send the request, then answer ourselves? No: use a
           second scripted behavior — disable via a param the
           server rejects. The scripted server above always
           succeeds set_enabled, so instead verify an ERROR
           reply through a bogus verb handled at the bridge:
           unknown verbs are still sent (the engine is the
           validator). */
        (void)before;
        const int req2 = bridge.reactiveCommand(
            QStringLiteral("explode"),
            QVariantMap({ { QStringLiteral("input"),
                            QStringLiteral("audio") } }));
        CHECK(req2 > 0);
        CHECK(PumpBoth(s, [&] {
            return last_reply_type == QStringLiteral("ERROR");
        }));
        CHECK(last_reply_body.value(QStringLiteral("message"))
                  .toString()
                  .contains(QStringLiteral("unknown reactive")));
        /* State did not fabricate a change. */
        CHECK(InputAt(bridge.reactive(), QStringLiteral("key"))
                  .value(QStringLiteral("enabled")).toBool()
              == false);
    }

    /*---- event-driven updates: pause + honest absence --------*/
    std::printf("-- pushed pause + unavailable surface\n");
    s.paused = true;
    PushReactive(s);
    CHECK(PumpBoth(s, [&] {
        return bridge.reactive()
            .value(QStringLiteral("capture_paused")).toBool();
    }));

    /* The whole surface can report honest absence — the card
       shows the engine's reason, never fabricated toggles. */
    s.surface_avail = false;
    PushReactive(s);
    CHECK(PumpBoth(s, [&] {
        return !bridge.reactive()
            .value(QStringLiteral("available")).toBool();
    }));
    {
        const QVariantMap r = bridge.reactive();
        CHECK(r.value(QStringLiteral("reason")).toString()
                  .contains(QStringLiteral("not wired")));
        CHECK(r.value(QStringLiteral("inputs")).toList()
                  .isEmpty());
    }

    bridge.stop();
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
