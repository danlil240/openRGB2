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

/* The engine's apply path reserializes the document it
   accepted (studio::ToJson(studio::FromJson(doc))): floats
   narrow to their stored precision and defaulted fields
   appear (parent_id/mirror_of/layout/label, binding
   vendor/device_type). Model that normalization so the
   post-save scene echo differs from the submitted document —
   the regression was the UI reading its OWN save's echo as a
   remote change (dirty + conflict after a successful save). */
static nlohmann::json NormalizedDoc(const nlohmann::json& doc)
{
    nlohmann::json out = doc;
    if(out.contains("brightness") && out["brightness"].is_number())
    {
        out["brightness"] =
            (double)(float)out["brightness"].get<double>();
    }
    for(nlohmann::json& o : out["objects"])
    {
        if(!o.is_object())
        {
            continue;
        }
        if(!o.contains("parent_id")) o["parent_id"] = "";
        if(!o.contains("mirror_of")) o["mirror_of"] = "";
        if(!o.contains("layout"))    o["layout"]    = "";
        if(!o.contains("label"))     o["label"]     =
            o.value("id", std::string());
        if(!o.contains("size_m"))
        {
            o["size_m"] = nlohmann::json::object(
                {{"x",0},{"y",0},{"z",0}});
        }
    }
    for(nlohmann::json& b : out["bindings"])
    {
        if(!b.is_object())
        {
            continue;
        }
        if(!b.contains("vendor"))      b["vendor"]      = "";
        if(!b.contains("device_type")) b["device_type"] = -1;
    }
    return out;
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
        { "name",     s.doc.value("name", std::string("desk")) },
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
            /* The stored + echoed document is the RESERIALIZED
               one — like the engine's ToJson(FromJson(doc)),
               which is a different json value than the client
               submitted (narrowed floats, filled defaults). */
            s.doc = NormalizedDoc(
                body.value("document",
                           nlohmann::json::object()));
            ++s.revision;
            s.srv.SendMsg(MsgType::Ok, m.request_id,
                nlohmann::json::object(
                    { { "revision", s.revision },
                      { "name",     "desk" } }));
            /* The engine emits a scene event on apply — the
               applied-state refresh the task requires, with
               the SAME revision the OK carried. */
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
                        { "vendor",     "Lian Li" },
                        { "device_type", 2 },
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

    /*---- vendor disambiguates same-name snapshots ----------*/
    std::printf("-- vendor disambiguation\n");
    /* Two snapshots with the SAME name but different vendors:
       the binding pins "Lian Li", so the resolver chain (and
       the hint) resolves unambiguously — name+vendor matching
       must not report ambiguous. */
    s.srv.SendMsg(MsgType::Event, 0,
        nlohmann::json::object({
            { "kind", "topology" },
            { "data", nlohmann::json::object({
                { "snapshots", nlohmann::json::array({
                    nlohmann::json::object({
                        { "key", nlohmann::json::object({
                            { "device",   "Other SL Wireless" },
                            { "serial",   "SN-2" },
                            { "location", "usb:9/9" },
                            { "zone",     "" } }) },
                        { "name",       "Lian Li UNI FAN SL "
                                        "Wireless" },
                        { "vendor",     "OtherVendor" },
                        { "device_type", 2 },
                        { "connection", "connected" },
                        { "led_count",  120 },
                        { "zones", nlohmann::json::array({
                            nlohmann::json::object({
                                { "name",      "Fan 1" },
                                { "led_count", 40 } }) }) },
                    }),
                    nlohmann::json::object({
                        { "key", nlohmann::json::object({
                            { "device",   "Lian Li SL Wireless" },
                            { "serial",   "SN-1" },
                            { "location", "usb:1/3" },
                            { "zone",     "" } }) },
                        { "name",       "Lian Li UNI FAN SL "
                                        "Wireless" },
                        { "vendor",     "Lian Li" },
                        { "device_type", 2 },
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
               && g[0].toMap().value(QStringLiteral("led_count"))
                      .toInt() == 40;
    }));

    /*---- scene edit -> SET_SCENE body ----------------------*/
    std::printf("-- SET_SCENE body\n");
    CHECK(edit.setPreset(QStringLiteral("breathing")));
    CHECK(edit.dirty());
    edit.setBrightness(0.33);
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
    CHECK(std::abs(d["brightness"].get<double>() - 0.33)
          < 0.001);
    CHECK(d["object_colors"]["fan0"] == "#00B4A0");

    /* OK commits; the scene EVENT echo (post-apply refresh)
       carries the SAME revision and the RESERIALIZED document
       — narrowed brightness (0.33 -> 0.33000001311302185) and
       defaulted fields the client never sent. It is our own
       save echoed, adopted by revision correlation: clean
       state, no spurious dirty+conflict. */
    CHECK(PumpBoth(s, [&] {
        return !edit.saving() && !edit.dirty();
    }));
    CHECK(edit.saveError().isEmpty());
    /* The OK alone can clear saving/dirty — wait until the
       normalized echo has actually landed before asserting
       the adopted state. */
    CHECK(PumpBoth(s, [&] {
        const QVariantList objs = bridge.scene()
            .value(QStringLiteral("document")).toMap()
            .value(QStringLiteral("objects")).toList();
        return !objs.isEmpty()
               && objs[0].toMap().contains(
                      QStringLiteral("parent_id"));
    }));
    CHECK(!edit.dirty());
    CHECK(!edit.conflict());
    /* Prove the echo really was the normalized document — the
       regression only bites when it differs from working. */
    CHECK(s.doc["objects"][0].contains("parent_id"));
    CHECK(s.doc["objects"][0].contains("mirror_of"));
    CHECK(std::abs(s.doc["brightness"].get<double>()
                   - (double)(float)0.33) < 1e-12);
    CHECK(s.doc != d);
    CHECK(bridge.scene()
              .value(QStringLiteral("document")).toMap()
              .value(QStringLiteral("effect")).toMap()
              .value(QStringLiteral("preset")).toString()
          == QStringLiteral("breathing"));

    /*---- remote apply under pending edits -> conflict ------*/
    std::printf("-- remote change still conflicts\n");
    /* A scene event carrying a DIFFERENT (newer) revision is a
       genuine remote apply — pending edits must flag the
       conflict, never be silently merged. */
    edit.setBrightness(0.9);
    CHECK(edit.dirty());
    {
        nlohmann::json remote = s.doc;
        remote["name"] = "remote-change";
        s.doc = remote;
        ++s.revision;            /* > our committed revision */
        s.srv.SendMsg(MsgType::Event, 0,
            nlohmann::json::object({
                { "kind", "scene" },
                { "data", SceneEnvelope(s) },
            }));
    }
    CHECK(PumpBoth(s, [&] { return edit.conflict(); }));
    CHECK(edit.dirty());         /* edits kept */
    edit.discardEdits();
    CHECK(PumpBoth(s, [&] {
        return !edit.dirty() && !edit.conflict();
    }));

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
    /* The refusal is attributed to its source — an engine
       ERROR reply never reads as a local input refusal
       (Task 4.5). */
    CHECK(edit.saveErrorSource() == QStringLiteral("engine"));
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

    /*---- empty draft refuses before send -------------------*/
    std::printf("-- empty scene refused\n");
    {
        /* A staged-but-empty working document (a profile that
           was never given objects, or a fresh draft) must
           never reach the wire: the engine accepts a doc with
           no device objects and applies it — every binding
           drops, output freezes on the last frame, and the
           save reports success. save() refuses before send
           with the same rule setGroupColor already enforces. */
        nlohmann::json empty = s.doc;
        empty["objects"]        = nlohmann::json::array();
        empty["bindings"]       = nlohmann::json::array();
        empty["object_colors"]  = nlohmann::json::object();
        empty["emitter_colors"] = nlohmann::json::object();
        edit.ModelForTesting().LoadWorking(empty);
        CHECK(edit.dirty());
        const int saves_before = s.scene_saves;
        edit.save();
        /* Give the wire a beat — nothing may arrive. */
        CHECK(!PumpBoth(s, [&] {
                  return s.scene_saves > saves_before;
              }, 400));
        CHECK(!edit.saving());
        CHECK(edit.saveErrorSource() == QStringLiteral("input"));
        CHECK(edit.saveError().contains(
            QStringLiteral("no device objects")));
        CHECK(edit.dirty());         /* staged doc kept */
        edit.discardEdits();
        CHECK(PumpBoth(s, [&] { return !edit.dirty(); }));
    }

    /*---- params emit only touched supported fields ----------*/
    std::printf("-- playback param filtering\n");
    /* A speed-only change on a FRESH controller must not emit
       the untouched color: the engine preserves its recorded
       per-scene value for omitted fields (protocol §5), so a
       speed drag can no longer clobber a persisted non-red
       loop color with the UI default. */
    const qlonglong sent =
        edit.sendPlaybackParams(150, true,
                                QStringLiteral("#FF0000"));
    CHECK(sent > 0);
    CHECK(PumpBoth(s, [&] { return s.mode_requests >= 1; }));
    CHECK(s.last_mode_body.value("control", "") == "paused");
    {
        const nlohmann::json& p = s.last_mode_body["params"];
        CHECK(p.is_object());
        CHECK(p["speed"].get<double>() == 150.0);
        CHECK(!p.contains("color"));
        CHECK(!p.contains("reverse"));
    }

    /* breathing supports speed + color, NOT direction — a
       changed color marks the field touched and is emitted;
       direction stays out of the request entirely, and the
       engine's paused control is preserved rather than
       silently resumed. */
    CHECK(edit.sendPlaybackParams(150, true,
                                  QStringLiteral("#00FF10")) > 0);
    CHECK(PumpBoth(s, [&] { return s.mode_requests >= 2; }));
    CHECK(s.last_mode_body.value("control", "") == "paused");
    const nlohmann::json& params = s.last_mode_body["params"];
    CHECK(params.is_object());
    CHECK(params["speed"].get<double>() == 150.0);
    CHECK(params["color"] == "#00FF10");
    CHECK(!params.contains("reverse"));

    /* A refused input is a DIFFERENT state than the engine
       rejection above — the UI refused it before a body
       existed (Task 4.5). */
    CHECK(!edit.setPreset(QString()));
    CHECK(edit.saveErrorSource() == QStringLiteral("input"));

    /* A non-playback preset must not emit params at all. */
    CHECK(edit.setPreset(QStringLiteral("aurora")));
    CHECK(edit.sendPlaybackParams(150, false, "") == 0);
    /* Give the wire a beat to deliver anything it shouldn't. */
    CHECK(!PumpBoth(s, [&] { return s.mode_requests >= 3; },
                    400));

    bridge.stop();
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
