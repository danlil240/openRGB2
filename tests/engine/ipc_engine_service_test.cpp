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
    /* unbuffered so a hard failure still shows progress */
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    Engine           engine;
    DeliveryPipeline pipeline;
    DeviceMapper     mapper;

    EngineService::Deps deps;
    deps.pipeline = &pipeline;
    deps.mapper   = &mapper;
    /* Task 4.4: the layout verbs' snapshot seam — the same
       fixture the wireless group uses below, so identify and
       set_binding can resolve a real enumerated controller
       without an SDK client. */
    DeviceSnapshot fsnap;
    fsnap.key = DeviceKey{ "Lian Li: SL Wireless",
                           "AABBCCDDEEFF",
                           "Wireless: AA:BB:CC:DD:EE:FF", "" };
    fsnap.topology_generation = 1;
    fsnap.zones = { ZoneSnapshot{ "Fan 1", 40 },
                    ZoneSnapshot{ "Fan 2", 40 },
                    ZoneSnapshot{ "Fan 3", 40 } };
    fsnap.connection = ConnectionState::Connected;
    deps.snapshots = [&fsnap]
    {
        return std::vector<DeviceSnapshot>{ fsnap };
    };
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

        /* Task 3.8 calibration regression: an out-of-range LED
           address (emitter 8 on an 8-LED zone) is rejected at
           the persistence boundary — expanded documents bypass
           workspace resolution, so the check runs on apply. */
        const auto led_doc = [](int address)
        {
            char buf[1024];
            std::snprintf(buf, sizeof(buf),
                "{ \"document\": { \"version\": 2,"
                " \"bindings\": [{ \"id\": \"b1\","
                "  \"controller_name\": \"fan\","
                "  \"zone_name\": \"ring\", \"zone_leds\": 8 }],"
                " \"objects\": [{ \"id\": \"o9\", \"kind\": \"device\","
                "  \"binding\": \"b1\", \"verified\": true,"
                "  \"emitters\": [{ \"address\": %d }] }] } }",
                address);
            return nlohmann::json::parse(buf);
        };
        r = svc.Dispatch(MsgType::SetScene, led_doc(8));
        CHECK(r.code == ErrCode::BadRequest);
        r = svc.Dispatch(MsgType::GetScene,
                         nlohmann::json::object());
        CHECK(r.body["document"]["name"].get<std::string>()
              == "ipc-test");

        /* ...while the same document with an in-range address
           applies cleanly. */
        r = svc.Dispatch(MsgType::SetScene, led_doc(7));
        CHECK(r.code == ErrCode::Ok);
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

    /*---- layout verbs (Task 4.4) -------------------------------*/
    {
        /* verb surface still bounded first */
        IpcReply r = svc.Dispatch(MsgType::LayoutCommand,
            nlohmann::json::object({{"verb", "bogus"}}));
        CHECK(r.code == ErrCode::BadRequest);
        r = svc.Dispatch(MsgType::LayoutCommand,
                         nlohmann::json::object());
        CHECK(r.code == ErrCode::BadRequest);
        /* wrong-typed verb — bounded bad_request, no throw */
        r = svc.Dispatch(MsgType::LayoutCommand,
            nlohmann::json::object({{"verb", 5}}));
        CHECK(r.code == ErrCode::BadRequest);

        /* identify without a controller key is bad_request
           now — the verb is implemented, not reserved. */
        r = svc.Dispatch(MsgType::LayoutCommand,
            nlohmann::json::object({{"verb", "identify"}}));
        CHECK(r.code == ErrCode::BadRequest);

        /* A controller absent from the live topology is the
           honest Unavailable — never a silent no-op. */
        r = svc.Dispatch(MsgType::LayoutCommand,
            nlohmann::json::object({
                { "verb", "identify" },
                { "controller", nlohmann::json::object(
                    {{"device","ghost"},{"serial",""},
                     {"location",""}}) },
            }));
        CHECK(r.code == ErrCode::Unavailable);

        /* The mapper must know the topology too — sync the
           fixture snapshot in. */
        mapper.SyncDevices(std::vector<DeviceSnapshot>{ fsnap });

        /* Whole zone, one LED, duration bounds, cancel. */
        r = svc.Dispatch(MsgType::LayoutCommand,
            nlohmann::json::object({
                { "verb", "identify" },
                { "controller", nlohmann::json::object({
                    {"device","Lian Li: SL Wireless"},
                    {"serial","AABBCCDDEEFF"},
                    {"location","Wireless: AA:BB:CC:DD:EE:FF"}}) },
                { "zone", "Fan 2" },
            }));
        CHECK(r.code == ErrCode::Ok);
        if(r.code == ErrCode::Ok)
        {
            CHECK(r.body["identifying"].get<bool>() == true);
            CHECK(r.body["led_start"].get<uint32_t>() == 40);
            CHECK(r.body["led_count"].get<uint32_t>() == 40);
            CHECK(r.body["duration_ms"].get<int64_t>() == 3000);
        }
        CHECK(mapper.Identifying(fsnap.key, Clock::now()));

        r = svc.Dispatch(MsgType::LayoutCommand,
            nlohmann::json::object({
                { "verb", "identify" },
                { "controller", nlohmann::json::object({
                    {"device","Lian Li: SL Wireless"},
                    {"serial","AABBCCDDEEFF"},
                    {"location","Wireless: AA:BB:CC:DD:EE:FF"}}) },
                { "zone", "Fan 2" }, { "led_index", 5 },
                { "duration_ms", 100 },
            }));
        CHECK(r.code == ErrCode::Ok);
        if(r.code == ErrCode::Ok)
        {
            /* bounded to the 500 ms floor, absolute offset */
            CHECK(r.body["led_start"].get<uint32_t>() == 45);
            CHECK(r.body["led_count"].get<uint32_t>() == 1);
            CHECK(r.body["duration_ms"].get<int64_t>() == 500);
        }

        r = svc.Dispatch(MsgType::LayoutCommand,
            nlohmann::json::object({
                { "verb", "identify" },
                { "controller", nlohmann::json::object({
                    {"device","Lian Li: SL Wireless"},
                    {"serial","AABBCCDDEEFF"},
                    {"location","Wireless: AA:BB:CC:DD:EE:FF"}}) },
                { "zone", "Fan 2" }, { "led_index", 40 },
            }));
        CHECK(r.code == ErrCode::BadRequest);   /* out of range */
        r = svc.Dispatch(MsgType::LayoutCommand,
            nlohmann::json::object({
                { "verb", "identify" },
                { "controller", nlohmann::json::object({
                    {"device","Lian Li: SL Wireless"},
                    {"serial","AABBCCDDEEFF"},
                    {"location","Wireless: AA:BB:CC:DD:EE:FF"}}) },
                { "zone", "no-such-zone" },
            }));
        CHECK(r.code == ErrCode::BadRequest);

        r = svc.Dispatch(MsgType::LayoutCommand,
            nlohmann::json::object({
                { "verb", "identify" },
                { "controller", nlohmann::json::object({
                    {"device","Lian Li: SL Wireless"},
                    {"serial","AABBCCDDEEFF"},
                    {"location","Wireless: AA:BB:CC:DD:EE:FF"}}) },
                { "duration_ms", 0 },
            }));
        CHECK(r.code == ErrCode::Ok);
        if(r.code == ErrCode::Ok)
        {
            CHECK(r.body["cancelled"].get<bool>() == true);
        }
        CHECK(!mapper.Identifying(fsnap.key, Clock::now()));

        /* The blink overlay runs through the normal frame
           path — with no doc bound it still produces a frame
           sized to the enumerated topology. */
        {
            EvaluatedFrame ev;
            ev.sample_time = Clock::now();
            std::string i_err;
            CHECK(mapper.StartIdentify(fsnap.key, 0, 5,
                  std::chrono::seconds(5), ev.sample_time,
                  i_err));
            const std::vector<RgbFrame> frames =
                mapper.MapFrame(ev);
            CHECK(frames.size() == 1);
            if(frames.size() == 1)
            {
                CHECK(frames[0].colors.size() == 120);
                if(frames[0].colors.size() == 120)
                {
                    CHECK(frames[0].colors[0].r == 0xFF);
                    CHECK(frames[0].colors[5].r == 0x00);
                }
            }
            CHECK(mapper.CancelIdentify(fsnap.key));
        }

        /*---- scene mutations: a device-bound document ------*/
        events.clear();
        r = svc.Dispatch(MsgType::SetScene,
            nlohmann::json::object({
                { "document", nlohmann::json::object({
                    { "version", 2 }, { "name", "layout" },
                    { "bindings", nlohmann::json::array({
                        nlohmann::json::object({
                            { "id", "slw0" },
                            { "controller_name", "UNI FAN" },
                            { "vendor", "Lian Li" },
                            { "zone_name", "Fan 1" },
                            { "zone_leds", 40 } }) }) },
                    { "objects", nlohmann::json::array({
                        nlohmann::json::object({
                            { "id", "fan0" }, { "kind", "device" },
                            { "binding", "slw0" },
                            { "position", nlohmann::json::object(
                                {{"x",0},{"y",0},{"z",0}}) },
                            { "emitters", nlohmann::json::array({
                                nlohmann::json::object(
                                    {{"address",0}}),
                                nlohmann::json::object(
                                    {{"address",1}}) }) } }),
                        nlohmann::json::object({
                            { "id", "fan1" }, { "kind", "device" },
                            { "binding", "slw0" },
                            { "emitters", nlohmann::json::array({
                                nlohmann::json::object(
                                    {{"address",2}}) }) } }),
                        nlohmann::json::object({
                            { "id", "case" }, { "kind", "decor" } }),
                    }) },
                }) }}));
        CHECK(r.code == ErrCode::Ok);

        /* set_position: full vector applies; partial refuses. */
        r = svc.Dispatch(MsgType::LayoutCommand,
            nlohmann::json::object({
                { "verb", "set_position" }, { "object_id", "fan0" },
                { "position", nlohmann::json::object(
                    {{"x",0.5},{"y",0.0},{"z",0.25}}) },
            }));
        CHECK(r.code == ErrCode::Ok);
        CHECK(r.body.contains("revision"));
        CHECK(!events.empty()
              && events.back().kind == event_kind::kScene);
        r = svc.Dispatch(MsgType::GetScene,
                         nlohmann::json::object());
        CHECK(r.code == ErrCode::Ok);
        if(r.code == ErrCode::Ok)
        {
            CHECK(r.body["document"]["objects"][0]["position"]
                      ["x"].get<double>() == 0.5);
        }
        r = svc.Dispatch(MsgType::LayoutCommand,
            nlohmann::json::object({
                { "verb", "set_position" }, { "object_id", "fan0" },
                { "position", nlohmann::json::object(
                    {{"x",1.0}}) },
            }));
        CHECK(r.code == ErrCode::BadRequest);

        /* set_orientation. */
        r = svc.Dispatch(MsgType::LayoutCommand,
            nlohmann::json::object({
                { "verb", "set_orientation" },
                { "object_id", "fan0" },
                { "rotation", nlohmann::json::object(
                    {{"x",0},{"y",90},{"z",0}}) },
            }));
        CHECK(r.code == ErrCode::Ok);

        /* calibrate_led: the engine is the last bound —
           address >= zone_leds refuses; -1 marks render-only. */
        r = svc.Dispatch(MsgType::LayoutCommand,
            nlohmann::json::object({
                { "verb", "calibrate_led" },
                { "object_id", "fan0" },
                { "emitter_index", 0 }, { "address", 40 },
            }));
        CHECK(r.code == ErrCode::BadRequest);
        r = svc.Dispatch(MsgType::LayoutCommand,
            nlohmann::json::object({
                { "verb", "calibrate_led" },
                { "object_id", "fan0" },
                { "emitter_index", 9 }, { "address", 0 },
            }));
        CHECK(r.code == ErrCode::BadRequest);
        r = svc.Dispatch(MsgType::LayoutCommand,
            nlohmann::json::object({
                { "verb", "calibrate_led" },
                { "object_id", "fan0" },
                { "emitter_index", 0 }, { "address", 39 },
            }));
        CHECK(r.code == ErrCode::Ok);
        r = svc.Dispatch(MsgType::LayoutCommand,
            nlohmann::json::object({
                { "verb", "calibrate_led" },
                { "object_id", "fan0" },
                { "emitter_index", 0 }, { "address", -1 },
            }));
        CHECK(r.code == ErrCode::Ok);

        /* set_mirror: fan1 -> copy of fan0 (emitters/binding
           destroyed — the linked-copy contract); chains,
           self-mirror, and decor targets refuse. */
        r = svc.Dispatch(MsgType::LayoutCommand,
            nlohmann::json::object({
                { "verb", "set_mirror" }, { "object_id", "fan1" },
                { "mirror_of", "fan0" },
            }));
        CHECK(r.code == ErrCode::Ok);
        r = svc.Dispatch(MsgType::GetScene,
                         nlohmann::json::object());
        CHECK(r.code == ErrCode::Ok);
        if(r.code == ErrCode::Ok)
        {
            const nlohmann::json& f1 =
                r.body["document"]["objects"][1];
            CHECK(f1["kind"].get<std::string>() == "linked");
            CHECK(f1["mirror_of"].get<std::string>() == "fan0");
            CHECK(f1["emitters"].empty());
        }
        r = svc.Dispatch(MsgType::LayoutCommand,
            nlohmann::json::object({
                { "verb", "set_mirror" }, { "object_id", "fan0" },
                { "mirror_of", "fan1" },
            }));
        CHECK(r.code == ErrCode::BadRequest);   /* chain    */
        r = svc.Dispatch(MsgType::LayoutCommand,
            nlohmann::json::object({
                { "verb", "set_mirror" }, { "object_id", "fan0" },
                { "mirror_of", "fan0" },
            }));
        CHECK(r.code == ErrCode::BadRequest);   /* self     */
        r = svc.Dispatch(MsgType::LayoutCommand,
            nlohmann::json::object({
                { "verb", "set_mirror" }, { "object_id", "fan0" },
                { "mirror_of", "case" },
            }));
        CHECK(r.code == ErrCode::BadRequest);   /* decor    */
        r = svc.Dispatch(MsgType::LayoutCommand,
            nlohmann::json::object({
                { "verb", "set_mirror" }, { "object_id", "fan1" },
                { "mirror_of", "" },
            }));
        CHECK(r.code == ErrCode::Ok);           /* unlink   */
        r = svc.Dispatch(MsgType::GetScene,
                         nlohmann::json::object());
        CHECK(r.code == ErrCode::Ok);
        if(r.code == ErrCode::Ok)
        {
            CHECK(r.body["document"]["objects"][1]["kind"]
                      .get<std::string>() == "device");
        }

        /* set_binding: unbind, existing id, and snapshot-
           derived creation — a client can never fabricate
           a controller identity. */
        r = svc.Dispatch(MsgType::LayoutCommand,
            nlohmann::json::object({
                { "verb", "set_binding" }, { "object_id", "fan0" },
                { "binding", "" },
            }));
        CHECK(r.code == ErrCode::Ok);
        r = svc.Dispatch(MsgType::LayoutCommand,
            nlohmann::json::object({
                { "verb", "calibrate_led" },
                { "object_id", "fan0" },
                { "emitter_index", 0 }, { "address", 0 },
            }));
        CHECK(r.code == ErrCode::BadRequest);  /* unbound  */
        r = svc.Dispatch(MsgType::LayoutCommand,
            nlohmann::json::object({
                { "verb", "set_binding" }, { "object_id", "fan0" },
                { "binding", "slw0" },
            }));
        CHECK(r.code == ErrCode::Ok);
        r = svc.Dispatch(MsgType::LayoutCommand,
            nlohmann::json::object({
                { "verb", "set_binding" }, { "object_id", "fan0" },
                { "binding", "b_new" },
            }));
        CHECK(r.code == ErrCode::BadRequest);  /* no key   */
        r = svc.Dispatch(MsgType::LayoutCommand,
            nlohmann::json::object({
                { "verb", "set_binding" }, { "object_id", "fan0" },
                { "binding", "b_new" },
                { "controller", nlohmann::json::object({
                    {"device","ghost"},{"serial",""},
                    {"location",""},{"zone",""}}) },
            }));
        CHECK(r.code == ErrCode::Unavailable); /* absent   */
        r = svc.Dispatch(MsgType::LayoutCommand,
            nlohmann::json::object({
                { "verb", "set_binding" }, { "object_id", "fan0" },
                { "binding", "b_new" },
                { "controller", nlohmann::json::object({
                    {"device","Lian Li: SL Wireless"},
                    {"serial","AABBCCDDEEFF"},
                    {"location","Wireless: AA:BB:CC:DD:EE:FF"},
                    {"zone","Fan 2"}}) },
            }));
        CHECK(r.code == ErrCode::Ok);
        r = svc.Dispatch(MsgType::GetScene,
                         nlohmann::json::object());
        CHECK(r.code == ErrCode::Ok);
        if(r.code == ErrCode::Ok)
        {
            const nlohmann::json& bs =
                r.body["document"]["bindings"];
            bool found = false;
            for(const nlohmann::json& b : bs)
            {
                if(b.value("id", std::string()) == "b_new")
                {
                    found = true;
                    CHECK(b["zone_name"].get<std::string>()
                          == "Fan 2");
                    CHECK(b["zone_leds"].get<unsigned int>()
                          == 40);
                }
            }
            CHECK(found);
            CHECK(r.body["document"]["objects"][0]["binding"]
                      .get<std::string>() == "b_new");
        }

        /* No loaded scene: mutations are honest Unavailable. */
        Engine           engine3;
        DeliveryPipeline pipe3;
        DeviceMapper     mapper3;
        EngineService::Deps d3;
        d3.pipeline = &pipe3;
        d3.mapper   = &mapper3;
        EngineService svc3(engine3, d3);
        r = svc3.Dispatch(MsgType::LayoutCommand,
            nlohmann::json::object({
                { "verb", "set_position" }, { "object_id", "o" },
                { "position", nlohmann::json::object(
                    {{"x",0},{"y",0},{"z",0}}) },
            }));
        CHECK(r.code == ErrCode::Unavailable);
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

    /*---- SET_PLAYBACK_MODE params merge: omitted fields
           preserve the recorded selection -------------------
           The wire reports parameter revisions, not values —
           a client has nothing honest to seed fields it is
           not changing, so dispatch seeds the PlaybackParams
           from the controller's record and overlays only the
           fields present. An omitted color must NOT reset a
           persisted non-red loop color to the default. */
    {
        Engine           engine2;
        DeliveryPipeline pipe2;

        DeviceSnapshot wsnap;
        wsnap.key = DeviceKey{ "Lian Li: SL Wireless",
                               "AABBCCDDEEFF",
                               "Wireless: AA:BB:CC:DD:EE:FF", "" };
        wsnap.topology_generation = 1;
        wsnap.zones = { ZoneSnapshot{ "Fan 1", 40 } };
        wsnap.modes = { "Direct", "Breathing" };
        wsnap.connection = ConnectionState::Connected;
        pipe2.RegisterDevice(wsnap);

        std::vector<PlaybackModePatch> patches;
        const TimePoint fixed_now{};
        PlaybackController::Deps pd;
        pd.snapshots = [&wsnap]
        {
            return std::vector<DeviceSnapshot>{ wsnap };
        };
        pd.select_mode =
            [&patches](const DeviceKey&, const std::string&,
                       const PlaybackModePatch& p, std::string&)
            {
                patches.push_back(p);
                return true;
            };
        pd.set_suppressed =
            [&pipe2](const DeviceKey& k, bool s,
                     const std::string& r)
            { pipe2.SetSuppressed(k, s, r); };
        pd.drop_pending =
            [&pipe2](const DeviceKey& k)
            { return pipe2.DropPending(k); };
        pd.wait_idle =
            [&pipe2](const DeviceKey& k, Clock::duration t)
            { return pipe2.WaitIdle(k, t); };
        pd.now = [fixed_now] { return fixed_now; };
        PlaybackController ctl2(pd);

        EngineService::Deps d2;
        d2.pipeline = &pipe2;
        d2.playback = &ctl2;
        /* The service wires its scene provider onto the
           controller — capabilities classify from the doc
           applied through the shared path below. */
        EngineService svc2(engine2, d2);

        IpcReply r = svc2.Dispatch(MsgType::SetScene,
            nlohmann::json::object({
                { "document", nlohmann::json::object({
                    { "version", 2 },
                    { "name",    "b" },
                    { "effect",  nlohmann::json::object(
                        {{"preset","breathing"}}) },
                }) }}));
        CHECK(r.code == ErrCode::Ok);

        r = svc2.Dispatch(MsgType::SetPlaybackMode,
            nlohmann::json::object({
                { "mode", "animation_playback" },
                { "params", nlohmann::json::object(
                    {{"speed",150},{"color","#00FF00"}}) },
            }));
        CHECK(r.code == ErrCode::Ok);
        CHECK(patches.size() == 1);
        CHECK(patches[0].has_colors);
        /* 0x00BBGGRR: {r=0x00,g=0xFF,b=0x00} -> 0x0000FF00. */
        CHECK(patches[0].colors[0] == 0x0000FF00u);

        /* Speed-only request: the omitted color preserves the
           recorded green — the old dispatch seeded defaults,
           silently clobbering it with red. */
        r = svc2.Dispatch(MsgType::SetPlaybackMode,
            nlohmann::json::object({
                { "mode", "animation_playback" },
                { "params", nlohmann::json::object(
                    {{"speed",170}}) },
            }));
        CHECK(r.code == ErrCode::Ok);
        CHECK(patches.size() == 2);
        CHECK(patches[1].has_colors);
        CHECK(patches[1].colors[0] == 0x0000FF00u);
        CHECK(ctl2.CurrentParams().speed == 170);
        CHECK(ctl2.CurrentParams().color.g == 0xFF);

        /* An omitted params object preserves everything too. */
        r = svc2.Dispatch(MsgType::SetPlaybackMode,
            nlohmann::json::object({
                { "mode",    "animation_playback" },
                { "control", "paused" },
            }));
        CHECK(r.code == ErrCode::Ok);
        CHECK(ctl2.CurrentParams().speed == 170);
        CHECK(ctl2.CurrentParams().color.g == 0xFF);
    }

    std::printf("ipc_engine_service_test: %d checks, %d failures\n",
                checks, failures);
    return failures ? 1 : 0;
}
