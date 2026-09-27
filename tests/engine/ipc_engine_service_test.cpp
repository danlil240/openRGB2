// SPDX-License-Identifier: GPL-2.0-or-later
//
// EngineService tests (src/ipc/EngineService.h): the real
// IIpcService over a live Engine + DeliveryPipeline +
// DeviceMapper with no SDK transport. Direct Dispatch calls
// (transport is covered by ipc_server_test): scene get/set
// through the shared apply path, playback mode validation,
// delivery stats, layout-verb honesty, the diagnostics
// payload, and diff-driven event emission.

#include "EngineService.h"
#include "IpcProtocol.h"

#include "DeliveryPipeline.h"
#include "DeviceMapper.h"
#include "Engine.h"
#include "FrameSink.h"

#include <cstdio>
#include <string>
#include <vector>

using namespace openrgb2;
using namespace openrgb2::ipc;

static int checks   = 0;
static int failures = 0;
#define CHECK(x) do { ++checks; if(!(x)) { ++failures; \
    std::printf("FAIL line %d: %s\n", __LINE__, #x); } } while(0)

int main()
{
    Engine           engine;
    DeliveryPipeline pipeline;
    DeviceMapper     mapper;

    EngineService::Deps deps;
    deps.pipeline = &pipeline;
    deps.mapper   = &mapper;
    /* no sdk client — the service must report honest absence */
    deps.playback_caps = []
    {
        PlaybackCapabilities caps;
        caps.live_effects       = true;
        caps.animation_playback = false;
        return caps;
    };

    EngineService svc(engine, deps);

    std::vector<IpcEvent> events;
    svc.SetEventSink([&](const IpcEvent& ev) { events.push_back(ev); });

    /*---- snapshots without SDK: honest, not an error -------*/
    {
        IpcReply r = svc.Dispatch(MsgType::GetSnapshots,
                                  nlohmann::json::object());
        CHECK(r.code == ErrCode::Ok);
        CHECK(r.body["snapshots"].is_array());
        CHECK(r.body["snapshots"].empty());
        CHECK(r.body["connection"]["sdk_present"].get<bool>()
              == false);
        CHECK(r.body["connection"]["state"].get<std::string>()
              == "disconnected");
    }

    /*---- scene get/set -------------------------------------*/
    {
        IpcReply r = svc.Dispatch(MsgType::GetScene,
                                  nlohmann::json::object());
        CHECK(r.code == ErrCode::Ok);
        CHECK(r.body["loaded"].get<bool>() == false);
        CHECK(r.body["document"].is_null());

        const nlohmann::json doc = nlohmann::json::object({
            { "version", 2 },
            { "name",    "ipc-test" },
            { "objects", nlohmann::json::array({
                nlohmann::json::object(
                    {{"id", "o1"}, {"kind", "decor"}}) }) },
        });
        r = svc.Dispatch(MsgType::SetScene,
                         nlohmann::json::object(
                             {{"document", doc}}));
        CHECK(r.code == ErrCode::Ok);
        CHECK(r.body["revision"].get<uint64_t>() == 1);
        /* the pending scene applies on the next worker tick */
        CHECK(engine.Worker().Step(Clock::now()));
        CHECK(engine.Worker().HasScene());
        /* scene event emitted on apply — and its data is the
           full SCENE pull body, the documented contract
           (docs/ipc-protocol.md §7: event data matches the
           corresponding pull body) */
        CHECK(!events.empty());
        CHECK(events.back().kind == event_kind::kScene);
        {
            const nlohmann::json& evd = events.back().data;
            CHECK(evd.contains("loaded"));
            CHECK(evd["loaded"].get<bool>() == true);
            CHECK(evd["revision"].get<uint64_t>() == 1);
            CHECK(evd.contains("applied_revision"));
            CHECK(evd.contains("playing"));
            CHECK(evd["document"]["name"].get<std::string>()
                  == "ipc-test");
        }

        r = svc.Dispatch(MsgType::GetScene,
                         nlohmann::json::object());
        CHECK(r.code == ErrCode::Ok);
        CHECK(r.body["loaded"].get<bool>() == true);
        CHECK(r.body["document"]["name"].get<std::string>()
              == "ipc-test");
        CHECK(r.body["revision"].get<uint64_t>() == 1);

        /* second apply bumps the revision */
        r = svc.Dispatch(MsgType::SetScene,
                         nlohmann::json::object(
                             {{"document", doc}}));
        CHECK(r.body["revision"].get<uint64_t>() == 2);
    }

    /*---- scene set: malformed documents ---------------------*/
    {
        /* newer-than-supported schema version */
        IpcReply r = svc.Dispatch(MsgType::SetScene,
            nlohmann::json::object({
                {"document", nlohmann::json::object(
                                 {{"version", 99}})}}));
        CHECK(r.code == ErrCode::BadRequest);
        CHECK(r.body.contains("errors"));

        /* document is not an object */
        r = svc.Dispatch(MsgType::SetScene,
            nlohmann::json::object({{"document", "x"}}));
        CHECK(r.code == ErrCode::BadRequest);

        /* missing document field */
        r = svc.Dispatch(MsgType::SetScene,
                         nlohmann::json::object({{"foo", 1}}));
        CHECK(r.code == ErrCode::BadRequest);

        /* unknown preset is rejected, not silently dark */
        r = svc.Dispatch(MsgType::SetScene,
            nlohmann::json::object({
                {"document", nlohmann::json::object({
                    {"version", 2},
                    {"effect", nlohmann::json::object(
                        {{"preset",
                          "definitely-not-a-preset"}})}})}}));
        CHECK(r.code == ErrCode::BadRequest);
        /* the previously applied scene is untouched */
        r = svc.Dispatch(MsgType::GetScene,
                         nlohmann::json::object());
        CHECK(r.body["document"]["name"].get<std::string>()
              == "ipc-test");
    }

    /*---- playback mode --------------------------------------*/
    {
        IpcReply r = svc.Dispatch(MsgType::SetPlaybackMode,
            nlohmann::json::object({{"mode", "bogus"}}));
        CHECK(r.code == ErrCode::BadRequest);

        r = svc.Dispatch(MsgType::SetPlaybackMode,
            nlohmann::json::object(
                {{"mode", "animation_playback"}}));
        CHECK(r.code == ErrCode::Unavailable);

        r = svc.Dispatch(MsgType::SetPlaybackMode,
            nlohmann::json::object({{"mode", 5}}));
        CHECK(r.code == ErrCode::BadRequest);

        r = svc.Dispatch(MsgType::SetPlaybackMode,
            nlohmann::json::object(
                {{"mode", "live_effects"}}));
        CHECK(r.code == ErrCode::Ok);
        CHECK(r.body["requested_mode"].get<std::string>()
              == "live_effects");
        /* honest: recorded, not applied */
        CHECK(r.body["applied"].get<bool>() == false);

        r = svc.Dispatch(MsgType::GetPlayback,
                         nlohmann::json::object());
        CHECK(r.code == ErrCode::Ok);
        CHECK(r.body["requested_mode"].get<std::string>()
              == "live_effects");
        CHECK(r.body["status"]["mode"].get<std::string>()
              == "live_effects");
        CHECK(r.body["capabilities"]["animation_playback"]
                  .get<bool>() == false);
    }

    /*---- delivery stats --------------------------------------*/
    {
        IpcReply r = svc.Dispatch(MsgType::GetDeliveryStats,
                                  nlohmann::json::object());
        CHECK(r.code == ErrCode::Ok);
        CHECK(r.body["available"].get<bool>() == true);
        CHECK(r.body.contains("aggregate"));
        CHECK(r.body["aggregate"]["submitted"].get<uint64_t>()
              == 0);
        /* the pipeline reports its real provenance */
        CHECK(r.body["aggregate"]["provenance"]
                  .get<std::string>() == "transport_measured");

        /* per-controller query for an unregistered key */
        r = svc.Dispatch(MsgType::GetDeliveryStats,
            nlohmann::json::object({
                {"controller", nlohmann::json::object(
                    {{"device", "x"}, {"serial", ""},
                     {"location", ""}})}}));
        CHECK(r.code == ErrCode::Ok);
        CHECK(r.body["found"].get<bool>() == false);

        /* wrong-typed controller fields: bounded bad_request —
           never a type_error through the session thread */
        r = svc.Dispatch(MsgType::GetDeliveryStats,
            nlohmann::json::object({
                {"controller", nlohmann::json::object(
                                   {{"device", 5}})}}));
        CHECK(r.code == ErrCode::BadRequest);

        r = svc.Dispatch(MsgType::GetDeliveryStats,
            nlohmann::json::object({{"controller", "x"}}));
        CHECK(r.code == ErrCode::BadRequest);
    }

    /*---- state + diagnostics ---------------------------------*/
    {
        IpcReply r = svc.Dispatch(MsgType::GetState,
                                  nlohmann::json::object());
        CHECK(r.code == ErrCode::Ok);
        CHECK(r.body["engine"]["scene_clock_hz"].get<double>()
              == 60.0);
        CHECK(r.body["scene"]["loaded"].get<bool>() == true);
        CHECK(r.body["sdk"]["sdk_present"].get<bool>() == false);
        /* no server wired -> honest null ipc stats */
        CHECK(r.body["ipc"].is_null());

        r = svc.Dispatch(MsgType::DiagGet,
                         nlohmann::json::object());
        CHECK(r.code == ErrCode::Ok);
        CHECK(r.reply_type == MsgType::Diagnostics);
        CHECK(r.body["engine"].contains("uptime_s"));
        CHECK(r.body["clock"].contains("steady_ms"));
        CHECK(r.body.contains("delivery"));
        /* backend's own counters pipe is a later plan item —
           honest null, never a fake */
        CHECK(r.body["backend_counters"].is_null());
    }

    /*---- layout verbs ----------------------------------------*/
    {
        IpcReply r = svc.Dispatch(MsgType::LayoutCommand,
            nlohmann::json::object({{"verb", "identify"}}));
        CHECK(r.code == ErrCode::Unavailable);

        r = svc.Dispatch(MsgType::LayoutCommand,
            nlohmann::json::object({{"verb", "bogus"}}));
        CHECK(r.code == ErrCode::BadRequest);

        r = svc.Dispatch(MsgType::LayoutCommand,
                         nlohmann::json::object());
        CHECK(r.code == ErrCode::BadRequest);

        /* wrong-typed verb — bounded bad_request, no throw */
        r = svc.Dispatch(MsgType::LayoutCommand,
            nlohmann::json::object({{"verb", 5}}));
        CHECK(r.code == ErrCode::BadRequest);
    }

    /*---- presets ---------------------------------------------*/
    {
        IpcReply r = svc.Dispatch(MsgType::GetPresets,
                                  nlohmann::json::object());
        CHECK(r.code == ErrCode::Ok);
        CHECK(r.body["presets"].is_array());
        CHECK(!r.body["presets"].empty());
    }

    /*---- PollEvents: diff-driven pushes -----------------------*/
    {
        events.clear();
        svc.PollEvents();
        /* first poll: connection + topology + playback +
           frame_status signatures all differ from empty */
        CHECK(events.size() == 4);
        bool saw_conn = false, saw_topo = false,
             saw_play = false, saw_frame = false;
        for(const IpcEvent& e : events)
        {
            saw_conn  |= e.kind == event_kind::kConnection;
            saw_topo  |= e.kind == event_kind::kTopology;
            saw_play  |= e.kind == event_kind::kPlayback;
            saw_frame |= e.kind == event_kind::kFrameStatus;
        }
        CHECK(saw_conn && saw_topo && saw_play && saw_frame);

        events.clear();
        svc.PollEvents();
        CHECK(events.empty());           /* nothing changed   */
    }

    /*---- SnapshotEvent (initial subscribe push) --------------*/
    {
        nlohmann::json snap;
        CHECK(svc.SnapshotEvent(event_kind::kTopology, snap));
        CHECK(snap.contains("snapshots"));
        CHECK(svc.SnapshotEvent(event_kind::kConnection, snap));
        CHECK(snap.contains("state"));
        CHECK(!svc.SnapshotEvent("no-such-kind", snap));
    }

    /*---- Welcome carries current state ------------------------*/
    {
        const nlohmann::json w = svc.OnWelcome();
        CHECK(w["engine"].get<std::string>() == "openrgb2-engine");
        CHECK(w.contains("state"));
        CHECK(w["state"]["scene"]["loaded"].get<bool>() == true);
    }

    std::printf("ipc_engine_service_test: %d checks, %d failures\n",
                checks, failures);
    return failures ? 1 : 0;
}
