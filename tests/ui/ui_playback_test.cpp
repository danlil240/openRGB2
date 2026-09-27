// SPDX-License-Identifier: GPL-2.0-or-later
//
// Playback selector + mode-behavior test (Task 4.3) — a real
// EngineBridge + SceneEditController over the in-process wire
// pipe (test_pipe.h) against a scripted engine peer, proving:
//
//   - selector state machine: mode request -> pending -> acked
//     (the applied mode is status.mode and only moves on the
//     backend-acknowledged revision — never reported applied
//     before the ack, even when the pending flag is already
//     clear)
//   - unsupported/reactive scenes mirror the engine's
//     capability + reason through playbackView (the UI never
//     re-derives availability)
//   - failed upload surfaces the wire `detail`, not a stuck
//     "applying" spinner; reconnecting link outranks pending
//   - the new-instance default: an explicit uploaded-preset
//     pick requests Animation playback once the scene applies;
//     a staged (migrated) profile stays Live; an explicit Live
//     pick disarms the pending default
//   - param touched latches reset when the applied scene NAME
//     changes (engine persists selections per scene name)
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
#include <memory>

using namespace openrgb2;
using namespace openrgb2::ipc;
using namespace ui_test;

static int checks   = 0;
static int failures = 0;
#define CHECK(x) do { ++checks; if(!(x)) { ++failures; \
    std::printf("FAIL line %d: %s\n", __LINE__, #x); } } while(0)

/* The applied scene at handshake: a NON-playback preset — the
   engine's classifier reports animation unavailable with the
   scene-capability reason the selector must mirror. */
static nlohmann::json SceneFixture()
{
    const nlohmann::json pos = nlohmann::json::object(
        {{"x",0},{"y",0},{"z",0}});
    return nlohmann::json::object({
        { "version",    2 },
        { "name",       "desk" },
        { "brightness", 1.0 },
        { "bindings",  nlohmann::json::array() },
        { "objects",   nlohmann::json::array() },
        { "object_colors",  nlohmann::json::object() },
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
    int          scene_saves    = 0;
    int          mode_requests  = 0;
    int          anim_requests  = 0;
    nlohmann::json last_scene_body;
    nlohmann::json last_mode_body;

    /* Playback bookkeeping — the scripted engine's reported
       state (test mutates it, then pushes a playback event). */
    std::string  requested_mode = "live_effects";
    std::string  applied_mode   = "live_effects";
    std::string  upload         = "none";
    std::string  control        = "playing";
    std::string  reason         =
        "scene effect 'aurora' is not an uploaded playback preset";
    std::string  detail;
    bool         pending        = false;
    bool         caps_anim      = false;
    uint64_t     req_rev        = 0;
    uint64_t     preset_rev     = 0;
    uint64_t     param_rev      = 0;
    int          frame_count    = 0;
    double       frame_interval = 0.0;
};

/* The PLAYBACK body shape of docs/ipc-protocol.md §5. */
static nlohmann::json PlaybackBody(const Server& s)
{
    return nlohmann::json::object({
        { "requested_mode",     s.requested_mode },
        { "requested_revision", s.req_rev        },
        { "pending",            s.pending        },
        { "unavailable_reason",
          s.reason.empty() ? nlohmann::json(nullptr)
                           : nlohmann::json(s.reason) },
        { "detail",
          s.detail.empty() ? nlohmann::json(nullptr)
                           : nlohmann::json(s.detail) },
        { "capabilities", nlohmann::json::object({
            { "live_effects",       true         },
            { "animation_playback", s.caps_anim  } }) },
        { "status", nlohmann::json::object({
            { "mode",               s.applied_mode },
            { "preset_revision",    s.preset_rev   },
            { "parameter_revision", s.param_rev    },
            { "upload",             s.upload       },
            { "control",            s.control      },
            { "effective_frame_count",
                                    s.frame_count  },
            { "effective_frame_interval_s",
                                    s.frame_interval },
            { "clock_sample_s",     0.0            },
            { "clock_uncertainty_s",0.0            },
            { "sync",               "unknown"      },
            { "physical", nlohmann::json::object({
                { "frame_interval_s", 0.0       },
                { "cycle_s",          0.0       },
                { "provenance",       "unknown" } }) } }) },
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
                { "loaded", true }, { "revision", 1 },
                { "name", "desk" }, { "playing", true },
                { "document", s.doc } }) },
            { "sdk", nlohmann::json::object({
                { "state", "connected" },
                { "reconnect_attempt", 0 } }) },
            { "playback", PlaybackBody(s) },
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

static void PushPlayback(Server& s)
{
    s.srv.SendMsg(MsgType::Event, 0,
        nlohmann::json::object({
            { "kind", "playback" },
            { "data", PlaybackBody(s) },
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
                    "connection", "frame_status" }) } }));
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
                    {{"state","connected"}}) },
                { "scene",    SceneEnvelope(s) },
                { "playback", PlaybackBody(s) },
            }));
        break;
    case MsgType::GetScene:
        s.srv.SendMsg(MsgType::Scene, m.request_id,
                      SceneEnvelope(s));
        break;
    case MsgType::GetPlayback:
        s.srv.SendMsg(MsgType::Playback, m.request_id,
                      PlaybackBody(s));
        break;
    case MsgType::SetScene:
        s.last_scene_body = body;
        ++s.scene_saves;
        s.doc = body.value("document", nlohmann::json::object());
        ++s.revision;
        s.srv.SendMsg(MsgType::Ok, m.request_id,
            nlohmann::json::object(
                { { "revision", s.revision },
                  { "name",     s.doc.value("name",
                                    std::string("desk")) } }));
        /* Applied on the next tick — the scene event mirrors
           the engine's own post-apply push. */
        s.srv.SendMsg(MsgType::Event, 0,
            nlohmann::json::object({
                { "kind", "scene" },
                { "data", SceneEnvelope(s) },
            }));
        break;
    case MsgType::SetPlaybackMode:
    {
        s.last_mode_body = body;
        ++s.mode_requests;
        const std::string mode = body.value("mode", std::string());
        if(mode == "animation_playback")
        {
            ++s.anim_requests;
        }
        /* The engine's reply contract: accepted and serialized,
           applied only when the backend reports the revision —
           and it emits a playback event carrying the recorded
           requested mode + the still-old applied status. */
        s.requested_mode = mode;
        s.pending        = true;
        ++s.req_rev;
        s.srv.SendMsg(MsgType::Ok, m.request_id,
            nlohmann::json::object({
                { "requested_mode", mode  },
                { "applied",        false },
                { "pending",        true  },
                { "revision",       s.req_rev },
            }));
        PushPlayback(s);
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

/* Alternate a short server round with a short event-loop round
   — both sides only make progress while this thread pumps. */
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

static QVariantMap View(ui::EngineBridge& b)
{
    return b.playbackView();
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

    ui::SceneEditController edit(&bridge);
    bridge.start();

    /*---- handshake: reactive-look scene, animation ---------
           unavailable with the engine's own reason ----------*/
    std::printf("-- handshake + capability mirror\n");
    CHECK(PumpBoth(s, [&] { return edit.hasScene(); }));
    {
        const QVariantMap v = View(bridge);
        CHECK(v.value(QStringLiteral("phase")).toString()
              == QStringLiteral("live"));
        CHECK(v.value(QStringLiteral("applied_mode")).toString()
              == QStringLiteral("live_effects"));
        CHECK(v.value(QStringLiteral("requested_mode")).toString()
              == QStringLiteral("live_effects"));
        /* The classifier's reason travels verbatim — the UI
           mirrors it, it never re-derives availability. */
        CHECK(v.value(QStringLiteral("animation_available"))
                  .toBool() == false);
        CHECK(v.value(QStringLiteral("live_available"))
                  .toBool() == true);
        CHECK(v.value(QStringLiteral("unavailable_reason"))
                  .toString().contains(QStringLiteral("aurora")));
    }

    /*---- selector state machine: request -> pending -> ack --*/
    std::printf("-- mode request -> pending -> acked\n");
    CHECK(edit.requestMode(QStringLiteral("animation_playback"))
          > 0);
    CHECK(PumpBoth(s, [&] {
        return s.anim_requests >= 1;
    }));
    CHECK(s.last_mode_body.value("mode", std::string())
              == "animation_playback");
    /* The current control rides the mode request — an omitted
       control would default to "playing" and silently resume
       a paused upload. */
    CHECK(s.last_mode_body.value("control", std::string())
              == "playing");

    /* Requested-but-unacknowledged: applied stays the OLD mode,
       phase is "applying", and pending holds — the selector
       never reports "on device" before the revision confirms. */
    CHECK(PumpBoth(s, [&] {
        return View(bridge).value(QStringLiteral("pending"))
            .toBool();
    }));
    {
        const QVariantMap v = View(bridge);
        CHECK(v.value(QStringLiteral("phase")).toString()
              == QStringLiteral("applying"));
        CHECK(v.value(QStringLiteral("applied_mode")).toString()
              == QStringLiteral("live_effects"));
        CHECK(v.value(QStringLiteral("requested_mode")).toString()
              == QStringLiteral("animation_playback"));
    }

    /* Pending-until-ack honesty: even with the wire's pending
       flag CLEAR, a requested/applied mismatch is still
       "applying" — "applied" is only status.mode. */
    s.pending = false;
    PushPlayback(s);
    {
        const QVariantMap v = View(bridge);
        CHECK(v.value(QStringLiteral("pending")).toBool());
        CHECK(v.value(QStringLiteral("phase")).toString()
              == QStringLiteral("applying"));
    }

    /* The acknowledged revision lands: applied moves, upload
       reports active, configured geometry passes through. */
    s.applied_mode   = "animation_playback";
    s.upload         = "active";
    s.control        = "playing";
    s.preset_rev     = s.req_rev;
    s.param_rev      = s.req_rev;
    s.frame_count    = 300;
    s.frame_interval = 1.0 / 60.0;
    PushPlayback(s);
    CHECK(PumpBoth(s, [&] {
        return View(bridge).value(QStringLiteral("phase"))
                   .toString() == QStringLiteral("active");
    }));
    {
        const QVariantMap v = View(bridge);
        CHECK(!v.value(QStringLiteral("pending")).toBool());
        CHECK(v.value(QStringLiteral("applied_mode")).toString()
              == QStringLiteral("animation_playback"));
        /* The acknowledgement is revision-correlated: the
           applied preset/parameter revisions now match the
           request the wire recorded — the view passes all
           three through so a mismatch could never read as
           "applied". */
        CHECK(v.value(QStringLiteral("requested_revision"))
                  .toULongLong() == s.req_rev);
        CHECK(v.value(QStringLiteral("preset_revision"))
                  .toULongLong() == s.req_rev);
        CHECK(v.value(QStringLiteral("parameter_revision"))
                  .toULongLong() == s.req_rev);
        CHECK(v.value(QStringLiteral("frame_count")).toInt()
              == 300);
        CHECK(v.value(QStringLiteral("physical_measured"))
                  .toBool() == false);
    }

    /*---- failed upload surfaces detail, not a spinner ------*/
    std::printf("-- failed upload\n");
    s.applied_mode = "live_effects";   /* transition never took */
    s.upload       = "failed";
    s.detail       = "backend refused the mode select";
    PushPlayback(s);
    CHECK(PumpBoth(s, [&] {
        return View(bridge).value(QStringLiteral("phase"))
                   .toString() == QStringLiteral("failed");
    }));
    CHECK(View(bridge).value(QStringLiteral("phase_detail"))
              .toString().contains(QStringLiteral("refused")));

    /* Recovery back to live. */
    s.upload         = "none";
    s.detail.clear();
    s.requested_mode = "live_effects";
    PushPlayback(s);
    CHECK(PumpBoth(s, [&] {
        return View(bridge).value(QStringLiteral("phase"))
                   .toString() == QStringLiteral("live");
    }));

    /*---- reconnecting link outranks everything -------------*/
    std::printf("-- reconnecting\n");
    s.srv.SendMsg(MsgType::Event, 0,
        nlohmann::json::object({
            { "kind", "connection" },
            { "data", nlohmann::json::object(
                { { "state", "reconnecting" } }) },
        }));
    CHECK(PumpBoth(s, [&] {
        return View(bridge).value(QStringLiteral("phase"))
                   .toString() == QStringLiteral("reconnecting");
    }));
    s.srv.SendMsg(MsgType::Event, 0,
        nlohmann::json::object({
            { "kind", "connection" },
            { "data", nlohmann::json::object(
                { { "state", "connected" } }) },
        }));
    CHECK(PumpBoth(s, [&] {
        return View(bridge).value(QStringLiteral("phase"))
                   .toString() == QStringLiteral("live");
    }));

    /*---- new-instance default to Animation playback ---------
           An explicit uploaded-preset pick + save: once the
           engine applies a scene naming the preset, the UI
           issues the default animation request itself. ------*/
    std::printf("-- new-instance default\n");
    s.caps_anim = true;
    s.reason.clear();
    CHECK(edit.setPreset(QStringLiteral("rainbow_wave")));
    CHECK(edit.dirty());
    const int anim_before = s.anim_requests;
    edit.save();
    CHECK(PumpBoth(s, [&] {
        return s.anim_requests == anim_before + 1;
    }));
    CHECK(s.last_mode_body.value("mode", std::string())
              == "animation_playback");
    /* The default request preserves the recorded control and
       emits no params — the engine's per-scene values stay. */
    CHECK(s.last_mode_body.value("control", std::string())
              == "playing");
    CHECK(!s.last_mode_body.contains("params") ||
          s.last_mode_body["params"].empty());
    const int anim_seen = s.anim_requests;
    /* The accepted request leaves the recorded selection at
       animation (pending) — the persisted-selection round-trip
       is the wire's, not the UI's. */
    CHECK(PumpBoth(s, [&] {
        return View(bridge).value(QStringLiteral("requested_mode"))
                   .toString()
               == QStringLiteral("animation_playback");
    }));

    /* Back to a clean live state before the migration case —
       models the engine restoring the PERSISTED live
       selection for the applied scene (the round-trip the
       wire owns; the UI only mirrors it). requested_mode must
       be live here so a wrongly-armed default latch would
       actually fire and fail the checks below. */
    s.applied_mode = "live_effects";
    s.upload       = "none";
    s.pending      = false;
    s.requested_mode = "live_effects";
    PushPlayback(s);
    CHECK(PumpBoth(s, [&] {
        return View(bridge).value(QStringLiteral("phase"))
                   .toString() == QStringLiteral("live");
    }));

    /*---- migrated/staged profile stays Live ----------------*/
    std::printf("-- migrated profile stays live\n");
    /* LoadWorking IS the profile-load path — an existing
       instance must never flip to animation on apply. */
    {
        nlohmann::json prof = s.doc;
        prof["name"] = "migrated-evening";
        edit.ModelForTesting().LoadWorking(prof);
    }
    CHECK(edit.dirty());
    edit.save();
    CHECK(PumpBoth(s, [&] { return s.scene_saves >= 2; }));
    CHECK(!PumpBoth(s, [&] { return s.anim_requests > anim_seen; },
                    800));

    /*---- explicit Live pick disarms the pending default ----*/
    std::printf("-- explicit live wins\n");
    CHECK(edit.setPreset(QStringLiteral("spectrum_cycle")));
    const int modes_before = s.mode_requests;
    CHECK(edit.requestMode(QStringLiteral("live_effects")) > 0);
    CHECK(PumpBoth(s, [&] {
        return s.mode_requests == modes_before + 1;
    }));
    CHECK(s.last_mode_body.value("mode", std::string())
              == "live_effects");
    edit.save();
    CHECK(PumpBoth(s, [&] { return s.scene_saves >= 3; }));
    CHECK(!PumpBoth(s, [&] { return s.anim_requests > anim_seen; },
                    800));

    /*---- param latches reset on applied-scene change -------*/
    std::printf("-- scene-scoped param latches\n");
    /* Working doc now names spectrum_cycle — touch speed,
       then a renamed applied scene arrives: the trackers
       fall back to defaults so nothing stale rides into a
       scene that never touched them. */
    CHECK(edit.sendPlaybackParams(160, false,
                                  QStringLiteral("")) > 0);
    CHECK(edit.paramSpeed() == 160);
    {
        nlohmann::json ren = s.doc;
        ren["name"] = "other-scene";
        s.doc = ren;
        ++s.revision;
        s.srv.SendMsg(MsgType::Event, 0,
            nlohmann::json::object({
                { "kind", "scene" },
                { "data", SceneEnvelope(s) },
            }));
    }
    CHECK(PumpBoth(s, [&] { return edit.paramSpeed() == 100; }));

    bridge.stop();
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
