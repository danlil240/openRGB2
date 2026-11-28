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
#include "EngineHost.h"
#include "FrameSink.h"

#include "../sdk/fake_sdk_server.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <future>
#include <memory>
#include <mutex>
#include <thread>
#include <string>
#include <thread>
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
    /* Task 4.5: a second enumerated controller carrying a
       SMALL zone — rebinding an object whose calibrated
       addresses exceed it must fail the same apply-path
       ValidateSceneLedAddresses, so set_binding is not a
       validation bypass. */
    DeviceSnapshot tsnap;
    tsnap.key = DeviceKey{ "Lian Li: SL Wireless Hub",
                           "112233445566",
                           "Wireless: 11:22:33:44:55:66", "" };
    tsnap.topology_generation = 1;
    tsnap.zones = { ZoneSnapshot{ "Tiny", 8 } };
    tsnap.connection = ConnectionState::Connected;
    deps.snapshots = [&fsnap, &tsnap]
    {
        return std::vector<DeviceSnapshot>{ fsnap, tsnap };
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
        /* no SDK client wired -> honest null, never a fake */
        CHECK(r.body["backend_counters"].is_null());
    }

    /*---- backend_counters: the wireless-status extension ------
           N01 observability: a wedged wireless group (terminal
           "failed" state inside LianLiWirelessService) was
           invisible over the SDK — DIAG said Direct + black and
           nothing else. The fork's packet 4000 now carries the
           GroupStatus; this drives the REAL OpenRgbClient over
           the scripted in-process backend so the serializer,
           wire parser, cache, and diagnostics surface are all
           exercised at once. */
    {
        sdk_test::FakeSdkServer server;
        sdk_test::FakeController wc;
        wc.name     = "Lian Li SL Wireless";
        wc.vendor   = "Lian Li";
        wc.serial   = "AABBCCDDEEFF";
        wc.location = "Wireless: AA:BB:CC:DD:EE:FF";
        sdk_test::FakeMode dm; dm.name = "Direct";
        wc.modes     = { dm };
        wc.zones     = { { "g", 0, 0, 1, 1 } };
        wc.led_names = { "l" };
        wc.colors    = { 0 };
        server.controllers = { wc };
        server.wireless_status_json =
            "{\"available\":true,\"master\":\"FA:C0:87:E5:66:E4\","
            "\"groups\":[{\"mac\":\"AABBCCDDEEFF\",\"state\":\"failed\","
            "\"wedged\":true,\"confirmed\":false,\"resends\":8,"
            "\"last_error\":\"resend budget exhausted: no HW ack\","
            "\"rgb_clock\":{\"valid\":true,\"ticks\":5000,"
            "\"age_ms\":50}}],"
            "\"counters\":{\"usb_send\":7}}";

        sdk::OpenRgbClient sdk_client(
            std::make_unique<sdk_test::FakeStream>(server));
        CHECK(sdk_client.Connect());

        Engine           engine7;
        DeliveryPipeline pipe7;
        DeviceMapper     mapper7;
        EngineService::Deps d7;
        d7.pipeline = &pipe7;
        d7.mapper   = &mapper7;
        d7.sdk      = &sdk_client;
        EngineService svc7(engine7, d7);

        IpcReply r = svc7.Dispatch(MsgType::DiagGet,
                                   nlohmann::json::object());
        CHECK(r.code == ErrCode::Ok);
        const nlohmann::json& bc = r.body["backend_counters"];
        CHECK(bc.is_object());
        /* THE wedge: the group's terminal state + cause are
           visible verbatim in diagnostics. */
        CHECK(bc["available"].get<bool>() == true);
        CHECK(bc["groups"].is_array() && bc["groups"].size() == 1);
        CHECK(bc["groups"][0]["state"].get<std::string>() ==
              "failed");
        CHECK(bc["groups"][0]["wedged"].get<bool>() == true);
        CHECK(bc["groups"][0]["last_error"].get<std::string>() ==
              "resend budget exhausted: no HW ack");
        CHECK(bc["counters"]["usb_send"].get<int>() == 7);
        CHECK(bc.contains("fetched_age_ms"));

        /* A stock backend (no packet 4000) reports the honest
           unsupported marker — and the connection survives. */
        sdk_test::FakeSdkServer stock;
        stock.controllers = { wc };
        sdk::OpenRgbClient sdk_stock(
            std::make_unique<sdk_test::FakeStream>(stock));
        CHECK(sdk_stock.Connect());
        EngineService::Deps d8;
        d8.pipeline = &pipe7;
        d8.mapper   = &mapper7;
        d8.sdk      = &sdk_stock;
        EngineService svc8(engine7, d8);
        r = svc8.Dispatch(MsgType::DiagGet, nlohmann::json::object());
        CHECK(r.code == ErrCode::Ok);
        CHECK(r.body["backend_counters"]["available"].get<bool>()
              == false);
        CHECK(r.body["backend_counters"]["supported"].get<bool>()
              == false);
        CHECK(sdk_stock.State() == ConnectionState::Connected);
    }

    /*---- Task 4.7 ownership wire surface ----------------------
           The UI suite scripts its own OwnershipBody, so a
           field-name typo in the REAL serializers would ship
           green. Wire the real provider — EngineHost::
           OwnershipJson on a never-started host (the honest
           scanned:false surface; the same serializer
           production wires after the startup scan) — through
           GetState and DiagGet, then the unwired and
           non-object cases for the honest null. */
    {
        EngineHostOptions oopts;
        oopts.ipc_enabled     = false;
        oopts.power_monitor   = false;
        oopts.scan_owners     = false;
        oopts.single_instance = false;
        EngineHost host(oopts);

        Engine           engine6;
        DeliveryPipeline pipe6;
        DeviceMapper     mapper6;
        EngineService::Deps d6;
        d6.pipeline  = &pipe6;
        d6.mapper    = &mapper6;
        d6.ownership = [&host] { return host.OwnershipJson(); };
        EngineService svc6(engine6, d6);

        IpcReply r = svc6.Dispatch(MsgType::GetState,
                                   nlohmann::json::object());
        CHECK(r.code == ErrCode::Ok);
        const nlohmann::json& ow = r.body["ownership"];
        CHECK(ow.is_object());
        /* the real serializer's key names — scanned:false is
           "the scan never ran", never "verified clean" */
        CHECK(ow["scanned"].get<bool>() == false);
        CHECK(ow["backend"]["listening"].get<bool>() == false);
        CHECK(ow["backend"]["attached"].get<bool>() == false);
        CHECK(ow["conflicts"].is_array());
        CHECK(ow["notices"].is_array());
        CHECK(ow["config"]["checked"].get<bool>() == false);
        CHECK(ow["config"]["findings"].is_array());
        CHECK(ow["config"]["enabled_plugins"].is_array());
        CHECK(ow["config"]["legacy_producers"].is_array());

        r = svc6.Dispatch(MsgType::DiagGet,
                          nlohmann::json::object());
        CHECK(r.code == ErrCode::Ok);
        CHECK(r.body["ownership"]["scanned"].get<bool>()
              == false);
        CHECK(r.body["ownership"]["config"]["legacy_producers"]
                  .is_array());

        /* unwired -> honest null on both pulls (the schema's
           "null when no ownership report is wired"), never an
           empty conflict list that would read as clean */
        Engine           engine7;
        DeliveryPipeline pipe7;
        DeviceMapper     mapper7;
        EngineService::Deps d7;
        d7.pipeline = &pipe7;
        d7.mapper   = &mapper7;
        EngineService svc7(engine7, d7);
        r = svc7.Dispatch(MsgType::GetState,
                          nlohmann::json::object());
        CHECK(r.code == ErrCode::Ok);
        CHECK(r.body.contains("ownership"));
        CHECK(r.body["ownership"].is_null());
        r = svc7.Dispatch(MsgType::DiagGet,
                          nlohmann::json::object());
        CHECK(r.code == ErrCode::Ok);
        CHECK(r.body["ownership"].is_null());

        /* a non-object provider result is a bug in the owner —
           the surface still answers null, never a bogus
           shape */
        Engine           engine8;
        EngineService::Deps d8;
        d8.ownership = []
        {
            return nlohmann::json("not-an-object");
        };
        EngineService svc8(engine8, d8);
        r = svc8.Dispatch(MsgType::GetState,
                          nlohmann::json::object());
        CHECK(r.code == ErrCode::Ok);
        CHECK(r.body["ownership"].is_null());

        /* R-10: the push path — EmitOwnership sends the same
           body as the pull, and SUBSCRIBE's snapshot serves it */
        std::vector<IpcEvent> ev6;
        svc6.SetEventSink([&ev6](const IpcEvent& e)
                          { ev6.push_back(e); });
        svc6.EmitOwnership();
        CHECK(ev6.size() == 1 &&
              ev6[0].kind == event_kind::kOwnership &&
              ev6[0].data.is_object() &&
              ev6[0].data["conflicts"].is_array());
        nlohmann::json osnap;
        CHECK(svc6.SnapshotEvent(event_kind::kOwnership, osnap));
        CHECK(osnap.is_object() && osnap.contains("scanned"));
    }

    /*---- E-12b exit/idle output policy surface -------------------
           STATE carries the policy (the REAL EngineHost
           serializer on a never-started host), QUIT's reply
           names the exit policy so a client can say "lights
           will turn off"; unwired -> STATE null, QUIT omits it. */
    {
        EngineHostOptions oopts;
        oopts.ipc_enabled     = false;
        oopts.power_monitor   = false;
        oopts.scan_owners     = false;
        oopts.single_instance = false;
        EngineHost host(oopts);

        Engine           engine9;
        DeliveryPipeline pipe9;
        DeviceMapper     mapper9;
        EngineService::Deps d9;
        d9.pipeline      = &pipe9;
        d9.mapper        = &mapper9;
        d9.output_policy = [&host] { return host.OutputPolicyJson(); };
        bool quit_called = false;
        d9.request_shutdown = [&quit_called](bool)
        {
            quit_called = true;
            return nlohmann::json::object();
        };
        EngineService svc9(engine9, d9);

        /* default: hold/hold, not idle */
        IpcReply r = svc9.Dispatch(MsgType::GetState,
                                   nlohmann::json::object());
        CHECK(r.code == ErrCode::Ok);
        {
            const nlohmann::json& op = r.body["output_policy"];
            CHECK(op.is_object());
            CHECK(op["on_exit"] == "hold");
            CHECK(op["on_idle"] == "hold");
            CHECK(op["per_controller"].is_array());
            CHECK(op["idle"].get<bool>() == false);
        }

        OutputPolicy p;
        p.on_exit = OutputAction::Black;
        p.on_idle = OutputAction::Restore;
        OutputPolicyRule rule;
        rule.match.location_prefix = "Wireless:";
        rule.on_exit = OutputAction::Hold;
        p.per_controller.push_back(rule);
        std::string perr;
        CHECK(host.SetOutputPolicy(p, perr));
        r = svc9.Dispatch(MsgType::GetState, nlohmann::json::object());
        CHECK(r.body["output_policy"]["on_exit"] == "black");
        CHECK(r.body["output_policy"]["on_idle"] == "restore");
        CHECK(r.body["output_policy"]["per_controller"].size() == 1);

        r = svc9.Dispatch(MsgType::Quit, nlohmann::json::object());
        CHECK(r.code == ErrCode::Ok);
        CHECK(quit_called);
        CHECK(r.body["exit_policy"] == "black");
        CHECK(r.body["shutting_down"].get<bool>() == true);

        /* unwired -> honest null / omitted */
        Engine           engine10;
        DeliveryPipeline pipe10;
        DeviceMapper     mapper10;
        EngineService::Deps d10;
        d10.pipeline = &pipe10;
        d10.mapper   = &mapper10;
        d10.request_shutdown = [](bool)
        { return nlohmann::json::object(); };
        EngineService svc10(engine10, d10);
        r = svc10.Dispatch(MsgType::GetState, nlohmann::json::object());
        CHECK(r.body.contains("output_policy"));
        CHECK(r.body["output_policy"].is_null());
        r = svc10.Dispatch(MsgType::Quit, nlohmann::json::object());
        CHECK(r.code == ErrCode::Ok);
        CHECK(!r.body.contains("exit_policy"));

        /* a non-object provider result answers null, never a
           bogus shape */
        Engine           engine11;
        EngineService::Deps d11;
        d11.output_policy = [] { return nlohmann::json("black"); };
        d11.request_shutdown = [](bool)
        { return nlohmann::json::object(); };
        EngineService svc11(engine11, d11);
        r = svc11.Dispatch(MsgType::GetState, nlohmann::json::object());
        CHECK(r.body["output_policy"].is_null());
        r = svc11.Dispatch(MsgType::Quit, nlohmann::json::object());
        CHECK(!r.body.contains("exit_policy"));
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
            CHECK(mapper.CancelIdentify(fsnap.key,
                                        Clock::now()));
        }

        /*---- scene mutations: a device-bound document ------*/
        events.clear();
        /* `fan2` exists so the mirror-source chain check has a
           valid Device target to refuse. */
        const nlohmann::json layout_doc = nlohmann::json::object({
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
                    { "id", "fan2" }, { "kind", "device" },
                    { "emitters", nlohmann::json::array({
                        nlohmann::json::object(
                            {{"address",3}}) }) } }),
                nlohmann::json::object({
                    { "id", "case" }, { "kind", "decor" } }),
            }) },
        });
        r = svc.Dispatch(MsgType::SetScene,
            nlohmann::json::object({{ "document", layout_doc }}));
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
        /* Review fix: fan0 is now fan1's mirror SOURCE —
           linking IT (fan0 -> fan2) would strand fan1.mirror_of
           on a Linked object, the chain ValidateSceneGraph
           forbids. Refused at the verb, before any apply. */
        r = svc.Dispatch(MsgType::LayoutCommand,
            nlohmann::json::object({
                { "verb", "set_mirror" }, { "object_id", "fan0" },
                { "mirror_of", "fan2" },
            }));
        CHECK(r.code == ErrCode::BadRequest);
        /* ...while a second copy sharing the same source is
           legal — fan2 -> fan0 is no chain. */
        r = svc.Dispatch(MsgType::LayoutCommand,
            nlohmann::json::object({
                { "verb", "set_mirror" }, { "object_id", "fan2" },
                { "mirror_of", "fan0" },
            }));
        CHECK(r.code == ErrCode::Ok);
        r = svc.Dispatch(MsgType::LayoutCommand,
            nlohmann::json::object({
                { "verb", "set_mirror" }, { "object_id", "fan2" },
                { "mirror_of", "" },
            }));
        CHECK(r.code == ErrCode::Ok);           /* unlink   */
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
        /* With fan1 unlinked, fan0 is a source no more — the
           same refused link is legal now. It consumes fan0's
           emitters/binding, so re-apply the document to
           restore them for the checks below. */
        r = svc.Dispatch(MsgType::LayoutCommand,
            nlohmann::json::object({
                { "verb", "set_mirror" }, { "object_id", "fan0" },
                { "mirror_of", "fan2" },
            }));
        CHECK(r.code == ErrCode::Ok);
        r = svc.Dispatch(MsgType::LayoutCommand,
            nlohmann::json::object({
                { "verb", "set_mirror" }, { "object_id", "fan0" },
                { "mirror_of", "" },
            }));
        CHECK(r.code == ErrCode::Ok);
        r = svc.Dispatch(MsgType::SetScene,
            nlohmann::json::object({{ "document", layout_doc }}));
        CHECK(r.code == ErrCode::Ok);

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

        /* Task 4.5 boundary regression: set_binding is NOT a
           validation bypass. fan0's emitter 0 calibrated to
           39 is legal under the 40-LED b_new zone; rebinding
           the object to the 8-LED "Tiny" zone strands that
           address out of range, so the mutated document fails
           the apply-path ValidateSceneLedAddresses and the
           WHOLE mutation refuses — binding, stranded address,
           and the new binding record all stay unapplied. */
        r = svc.Dispatch(MsgType::LayoutCommand,
            nlohmann::json::object({
                { "verb", "calibrate_led" },
                { "object_id", "fan0" },
                { "emitter_index", 0 }, { "address", 39 },
            }));
        CHECK(r.code == ErrCode::Ok);
        r = svc.Dispatch(MsgType::LayoutCommand,
            nlohmann::json::object({
                { "verb", "set_binding" }, { "object_id", "fan0" },
                { "binding", "b_tiny" },
                { "controller", nlohmann::json::object({
                    {"device","Lian Li: SL Wireless Hub"},
                    {"serial","112233445566"},
                    {"location","Wireless: 11:22:33:44:55:66"},
                    {"zone","Tiny"}}) },
            }));
        CHECK(r.code == ErrCode::BadRequest);
        /* the refused mutation applied nothing: the binding
           stays b_new and the new "b_tiny" record never
           landed */
        r = svc.Dispatch(MsgType::GetScene,
                         nlohmann::json::object());
        CHECK(r.code == ErrCode::Ok);
        if(r.code == ErrCode::Ok)
        {
            const nlohmann::json& o0 =
                r.body["document"]["objects"][0];
            CHECK(o0["binding"].get<std::string>() == "b_new");
            CHECK(o0["emitters"][0]["address"].get<int>()
                  == 39);
            for(const nlohmann::json& b :
                    r.body["document"]["bindings"])
            {
                CHECK(b.value("id", std::string()) != "b_tiny");
            }
        }
        /* a compliant rebind applies cleanly — the refusal
           was the address, not the verb */
        r = svc.Dispatch(MsgType::LayoutCommand,
            nlohmann::json::object({
                { "verb", "calibrate_led" },
                { "object_id", "fan0" },
                { "emitter_index", 0 }, { "address", 7 },
            }));
        CHECK(r.code == ErrCode::Ok);
        r = svc.Dispatch(MsgType::LayoutCommand,
            nlohmann::json::object({
                { "verb", "set_binding" }, { "object_id", "fan0" },
                { "binding", "b_tiny" },
                { "controller", nlohmann::json::object({
                    {"device","Lian Li: SL Wireless Hub"},
                    {"serial","112233445566"},
                    {"location","Wireless: 11:22:33:44:55:66"},
                    {"zone","Tiny"}}) },
            }));
        CHECK(r.code == ErrCode::Ok);
        r = svc.Dispatch(MsgType::GetScene,
                         nlohmann::json::object());
        CHECK(r.code == ErrCode::Ok);
        if(r.code == ErrCode::Ok)
        {
            CHECK(r.body["document"]["objects"][0]["binding"]
                      .get<std::string>() == "b_tiny");
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

    /*---- Task 4.4 review: identify with NO scene loaded must
           still put frames on the wire -------------------------
           The worker's no-scene ticks used to sleep, so the
           identify ACK lied — the blink never left the mapper.
           The service now wires the mapper's overlay ledger as
           the worker's idle-frame hook: Dispatch -> Step ->
           MapFrame -> pipeline lane, verified end-to-end with
           no scene anywhere in the composition. */
    {
        Engine           engine4;
        DeliveryPipeline pipe4;
        DeviceMapper     mapper4;

        DeviceSnapshot isnap;
        isnap.key = DeviceKey{ "Lian Li: SL Wireless",
                               "AABBCCDDEEFF",
                               "Wireless: AA:BB:CC:DD:EE:FF", "" };
        isnap.topology_generation = 1;
        isnap.zones = { ZoneSnapshot{ "Fan 1", 40 } };
        isnap.colors.assign(40, RgbColor{ 0x11, 0x22, 0x33 });
        isnap.connection = ConnectionState::Connected;
        pipe4.RegisterDevice(isnap);
        mapper4.SyncDevices(std::vector<DeviceSnapshot>{ isnap });

        EngineService::Deps d4;
        d4.pipeline = &pipe4;
        d4.mapper   = &mapper4;
        d4.snapshots = [&isnap]
        {
            return std::vector<DeviceSnapshot>{ isnap };
        };
        EngineService svc4(engine4, d4);

        /* The host's frame path, replicated: evaluated tick ->
           mapper -> delivery lane (EngineHost wires the same,
           plus playback bookkeeping). The service's ctor wired
           the idle hook against mapper4 already. */
        engine4.Worker().SetFrameCallback(
            [&mapper4, &pipe4](const EvaluatedFrame& ev)
            {
                for(const RgbFrame& f : mapper4.MapFrame(ev))
                {
                    pipe4.SubmitLatest(f);
                }
            });

        /* No scene and no overlay booked — the frame path
           sleeps exactly as before. */
        CHECK(engine4.Worker().Step(Clock::now()) == false);
        CHECK(pipe4.PendingCount(isnap.key) == 0);
        CHECK(!mapper4.IdentifyActive());

        IpcReply r = svc4.Dispatch(MsgType::LayoutCommand,
            nlohmann::json::object({
                { "verb", "identify" },
                { "controller", nlohmann::json::object({
                    {"device","Lian Li: SL Wireless"},
                    {"serial","AABBCCDDEEFF"},
                    {"location","Wireless: AA:BB:CC:DD:EE:FF"}}) },
                { "zone", "Fan 1" },
                { "duration_ms", 1000 },
            }));
        CHECK(r.code == ErrCode::Ok);
        CHECK(r.body["identifying"].get<bool>() == true);
        CHECK(mapper4.IdentifyActive());

        /* With no scene anywhere, a Step still produces the
           blink frame and the lane admits it — the whole
           40-LED zone lit on the first half-period. */
        CHECK(engine4.Worker().Step(Clock::now()) == true);
        RgbFrame pending;
        CHECK(pipe4.Pending(isnap.key, &pending));
        CHECK(pending.colors.size() == 40);
        if(pending.colors.size() == 40)
        {
            CHECK(pending.colors[0].r  == 0xFF);
            CHECK(pending.colors[39].r == 0xFF);
        }

        /* Past expiry exactly one more frame lands — the
           pre-identify baseline (the enumerated colors) so the
           blink can't freeze the LEDs on a lit phase — then
           the booking drains and the path sleeps again. */
        const TimePoint after =
            Clock::now() + std::chrono::milliseconds(1200);
        CHECK(engine4.Worker().Step(after) == true);
        CHECK(pipe4.Pending(isnap.key, &pending));
        CHECK(pending.colors.size() == 40);
        if(pending.colors.size() == 40)
        {
            CHECK(pending.colors[0].r == 0x11);
            CHECK(pending.colors[0].g == 0x22);
        }
        CHECK(!mapper4.IdentifyActive());
        CHECK(engine4.Worker().Step(
            after + std::chrono::milliseconds(20)) == false);
    }

    /*---- Task 4.4 review round 2: CANCEL with no scene must
           still land the restore frame -------------------------
           duration_ms:0 used to erase the booking outright —
           the restore frame lives only in the expiry branch,
           so with no scene driving the lane nothing ever ran it
           and the blink's last phase froze on the hardware.
           Cancel now stamps the booking expired; the normal
           prune+restore tick ends it. */
    {
        Engine           engine5;
        DeliveryPipeline pipe5;
        DeviceMapper     mapper5;

        DeviceSnapshot csnap;
        csnap.key = DeviceKey{ "Lian Li: SL Wireless",
                               "AABBCCDDEEFF",
                               "Wireless: AA:BB:CC:DD:EE:FF", "" };
        csnap.topology_generation = 1;
        csnap.zones = { ZoneSnapshot{ "Fan 1", 40 } };
        csnap.colors.assign(40, RgbColor{ 0x11, 0x22, 0x33 });
        csnap.connection = ConnectionState::Connected;
        pipe5.RegisterDevice(csnap);
        mapper5.SyncDevices(std::vector<DeviceSnapshot>{ csnap });

        EngineService::Deps d5;
        d5.pipeline = &pipe5;
        d5.mapper   = &mapper5;
        d5.snapshots = [&csnap]
        {
            return std::vector<DeviceSnapshot>{ csnap };
        };
        EngineService svc5(engine5, d5);

        engine5.Worker().SetFrameCallback(
            [&mapper5, &pipe5](const EvaluatedFrame& ev)
            {
                for(const RgbFrame& f : mapper5.MapFrame(ev))
                {
                    pipe5.SubmitLatest(f);
                }
            });

        IpcReply r = svc5.Dispatch(MsgType::LayoutCommand,
            nlohmann::json::object({
                { "verb", "identify" },
                { "controller", nlohmann::json::object({
                    {"device","Lian Li: SL Wireless"},
                    {"serial","AABBCCDDEEFF"},
                    {"location","Wireless: AA:BB:CC:DD:EE:FF"}}) },
                { "zone", "Fan 1" },
                { "duration_ms", 30000 },
            }));
        CHECK(r.code == ErrCode::Ok);
        CHECK(r.body["identifying"].get<bool>() == true);

        /* One blink frame lands — the first half-period is
           lit, the freeze case the review flagged. */
        CHECK(engine5.Worker().Step(Clock::now()) == true);
        RgbFrame pending;
        CHECK(pipe5.Pending(csnap.key, &pending));
        CHECK(pending.colors.size() == 40);
        if(pending.colors.size() == 40)
        {
            CHECK(pending.colors[0].r == 0xFF);
        }

        /* UI Stop -> duration_ms:0. The reply contract is
           unchanged — and the booking is only STAMPED expired,
           so Identifying() reports false at once while
           IdentifyActive() keeps the frame path awake for the
           one tick that still owes the restore. */
        r = svc5.Dispatch(MsgType::LayoutCommand,
            nlohmann::json::object({
                { "verb", "identify" },
                { "controller", nlohmann::json::object({
                    {"device","Lian Li: SL Wireless"},
                    {"serial","AABBCCDDEEFF"},
                    {"location","Wireless: AA:BB:CC:DD:EE:FF"}}) },
                { "duration_ms", 0 },
            }));
        CHECK(r.code == ErrCode::Ok);
        CHECK(r.body["identifying"].get<bool>() == false);
        CHECK(r.body["cancelled"].get<bool>() == true);
        CHECK(!mapper5.Identifying(csnap.key, Clock::now()));
        CHECK(mapper5.IdentifyActive());

        /* The very next Step lands the restore — the pre-
           identify baseline (the enumerated colors), replacing
           the lit blink frame in the lane — then the booking
           drains and the path sleeps. */
        CHECK(engine5.Worker().Step(Clock::now()) == true);
        CHECK(pipe5.Pending(csnap.key, &pending));
        CHECK(pending.colors.size() == 40);
        if(pending.colors.size() == 40)
        {
            CHECK(pending.colors[0].r  == 0x11);
            CHECK(pending.colors[0].g  == 0x22);
            CHECK(pending.colors[39].r == 0x11);
        }
        CHECK(!mapper5.IdentifyActive());
        CHECK(engine5.Worker().Step(Clock::now()) == false);
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
           frame_status + reactive (Task 4.6) signatures all
           differ from empty */
        CHECK(events.size() == 5);
        bool saw_conn = false, saw_topo = false,
             saw_play = false, saw_frame = false,
             saw_react = false;
        for(const IpcEvent& e : events)
        {
            saw_conn  |= e.kind == event_kind::kConnection;
            saw_topo  |= e.kind == event_kind::kTopology;
            saw_play  |= e.kind == event_kind::kPlayback;
            saw_frame |= e.kind == event_kind::kFrameStatus;
            saw_react |= e.kind == event_kind::kReactive;
        }
        CHECK(saw_conn && saw_topo && saw_play && saw_frame
              && saw_react);
        /* I-7: a diff body travels with the dump its signature
           already made — exactly data.dump() — and the framed
           EVENT is byte-identical to the old envelope encode. */
        int pre_serialized = 0;
        for(const IpcEvent& e : events)
        {
            if(e.data_json)
            {
                ++pre_serialized;
                CHECK(*e.data_json == e.data.dump());
            }
            CHECK(EncodeEventMessage(e) ==
                  EncodeMessage(MsgType::Event, 0,
                      nlohmann::json::object({
                          { "kind", e.kind },
                          { "data", e.data } })));
        }
        CHECK(pre_serialized == 3);   /* connection/playback/reactive */

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
        /* R-10: "ownership" snapshots the wired report; this
           service has no provider -> the honest null, still a
           served kind (a reconnecting UI must get an answer). */
        CHECK(svc.SnapshotEvent(event_kind::kOwnership, snap));
        CHECK(snap.is_null());
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

        /* BUG-1: 'breathing' is an uploaded-only preset —
           the applied scene resolves to no live layer stack,
           so a Live switch while its upload is in flight must
           be REFUSED with a reason, never accepted into a
           silent all-black evaluation. */
        r = svc2.Dispatch(MsgType::SetPlaybackMode,
            nlohmann::json::object({
                { "mode", "live_effects" }}));
        CHECK(r.code == ErrCode::Unavailable);
        CHECK(!r.message.empty());
        CHECK(r.message.find("breathing") !=
              std::string::npos);
        /* The refusal recorded nothing and the wire reports
           the symmetric capability + reason. */
        r = svc2.Dispatch(MsgType::GetPlayback,
                          nlohmann::json::object());
        CHECK(r.code == ErrCode::Ok);
        CHECK(r.body["requested_mode"].get<std::string>()
              == "animation_playback");
        CHECK(r.body["capabilities"]["live_effects"]
                  .get<bool>() == false);
        CHECK(r.body["capabilities"]["animation_playback"]
                  .get<bool>() == true);
        CHECK(r.body["unavailable_reason"].is_string());
        CHECK(r.body["unavailable_reason"].get<std::string>()
                  .find("breathing") != std::string::npos);
    }

    /*---- add_object: scene objects authored from LIVE
           topology -------------------------------------------
           The scene-creation verb: a DeviceKey array resolves
           against the snapshot seam (the same one identify and
           set_binding use) into a DeviceBinding + device object
           with a default emitter layout — one apply, one
           revision, one scene event. Clients never fabricate
           controller identity; the engine derives it. */
    {
        Engine           engine6;
        DeliveryPipeline pipe6;
        DeviceMapper     mapper6;

        DeviceSnapshot add_snap;
        add_snap.key = DeviceKey{ "Lian Li: SL Wireless",
                                  "AABBCCDDEEFF",
                                  "Wireless: AA:BB:CC:DD:EE:FF",
                                  "" };
        add_snap.name        = "Lian Li UNI FAN SL Wireless";
        add_snap.vendor      = "Lian Li";
        add_snap.device_type = 2;
        add_snap.zones       = { ZoneSnapshot{ "Fan 1", 40 },
                                 ZoneSnapshot{ "Fan 2", 40 },
                                 ZoneSnapshot{ "Fan 3", 40 } };
        add_snap.connection  = ConnectionState::Connected;
        /* Two small zones pin the emitter-layout rules:
           12 LEDs -> a row, 1 LED -> a single point. */
        DeviceSnapshot dimm;
        dimm.key  = DeviceKey{ "Corsair: Vengeance RGB",
                               "DDR5-1", "smbus:0-DDR5-1", "" };
        dimm.name        = "Corsair Vengeance RGB DDR5";
        dimm.vendor      = "Corsair";
        dimm.device_type = 3;
        dimm.zones       = { ZoneSnapshot{ "DIMM", 12 },
                             ZoneSnapshot{ "Logo", 1 } };
        dimm.connection  = ConnectionState::Connected;

        EngineService::Deps d6;
        d6.pipeline  = &pipe6;
        d6.mapper    = &mapper6;
        d6.snapshots = [&add_snap, &dimm]
        {
            return std::vector<DeviceSnapshot>{ add_snap, dimm };
        };
        /* persist_scene — the last-scene snapshot hook: fires
           once per successful apply that carries objects, on
           the background writer (I-6) — flush before counting. */
        int         persist_calls = 0;
        std::string persisted_name;
        d6.persist_scene =
            [&](const studio::SceneDocument& dd)
            {
                ++persist_calls;
                persisted_name = dd.name;
            };
        EngineService svc6(engine6, d6);

        const nlohmann::json fan3 = nlohmann::json::object({
            { "device",   "Lian Li: SL Wireless" },
            { "serial",   "AABBCCDDEEFF" },
            { "location", "Wireless: AA:BB:CC:DD:EE:FF" },
            { "zone",     "Fan 3" } });

        /* No scene loaded -> Unavailable, like its
           object-mutation siblings. */
        IpcReply r = svc6.Dispatch(MsgType::LayoutCommand,
            nlohmann::json::object({
                { "verb",        "add_object" },
                { "controllers", nlohmann::json::array({ fan3 }) },
            }));
        CHECK(r.code == ErrCode::Unavailable);

        /* Malformed payloads refuse BEFORE any resolution. */
        r = svc6.Dispatch(MsgType::LayoutCommand,
            nlohmann::json::object({{ "verb", "add_object" }}));
        CHECK(r.code == ErrCode::BadRequest);
        r = svc6.Dispatch(MsgType::LayoutCommand,
            nlohmann::json::object({
                { "verb",        "add_object" },
                { "controllers", "Fan 3" },
            }));
        CHECK(r.code == ErrCode::BadRequest);
        r = svc6.Dispatch(MsgType::LayoutCommand,
            nlohmann::json::object({
                { "verb",        "add_object" },
                { "controllers", nlohmann::json::array() },
            }));
        CHECK(r.code == ErrCode::BadRequest);

        /* Seed a non-empty scene — one apply, one persist. */
        const nlohmann::json seed_doc = nlohmann::json::object({
            { "version", 2 }, { "name", "seed" },
            { "bindings", nlohmann::json::array() },
            { "objects", nlohmann::json::array({
                nlohmann::json::object({
                    { "id", "case" }, { "kind", "decor" } }) }) },
        });
        r = svc6.Dispatch(MsgType::SetScene,
            nlohmann::json::object({{ "document", seed_doc }}));
        CHECK(r.code == ErrCode::Ok);
        svc6.FlushPersistedScene();
        CHECK(persist_calls == 1);
        CHECK(persisted_name == "seed");

        /* Audit I-13: the controllers list is bounded. 257
           copies of one live zone refuse up front instead of
           authoring 257 objects on the reader thread (every
           new id scans the whole document — a ~1 MiB body of
           repeats ran for minutes). The scene is untouched. */
        {
            nlohmann::json many = nlohmann::json::array();
            for(int i = 0; i < 257; ++i)
            {
                many.push_back(fan3);
            }
            r = svc6.Dispatch(MsgType::LayoutCommand,
                nlohmann::json::object({
                    { "verb",        "add_object" },
                    { "controllers", many         },
                }));
            CHECK(r.code == ErrCode::BadRequest);
            CHECK(persist_calls == 1);
        }

        /* All-fail batch: an unknown controller -> Unavailable;
           the per-entry reason lands in `skipped` and the scene
           is untouched (no revision bump, no persist). */
        const uint64_t rev0 = svc6.SceneRevision();
        r = svc6.Dispatch(MsgType::LayoutCommand,
            nlohmann::json::object({
                { "verb", "add_object" },
                { "controllers", nlohmann::json::array({
                    nlohmann::json::object({
                        {"device","ghost"},{"serial",""},
                        {"location",""},{"zone",""}}) }) },
            }));
        CHECK(r.code == ErrCode::Unavailable);
        CHECK(r.body["skipped"].is_array());
        CHECK(r.body["skipped"].size() == 1);
        CHECK(svc6.SceneRevision() == rev0);
        svc6.FlushPersistedScene();
        CHECK(persist_calls == 1);

        /* Happy path: Fan 3 (40 LEDs) -> one new binding whose
           identity is snapshot-derived, one device object with
           40 addressed emitters. */
        r = svc6.Dispatch(MsgType::LayoutCommand,
            nlohmann::json::object({
                { "verb",        "add_object" },
                { "controllers", nlohmann::json::array({ fan3 }) },
            }));
        CHECK(r.code == ErrCode::Ok);
        CHECK(r.body["added"].is_array());
        CHECK(r.body["added"].size() == 1);
        if(r.body["added"].size() == 1)
        {
            CHECK(!r.body["added"][0]["object_id"]
                       .get<std::string>().empty());
            CHECK(!r.body["added"][0]["binding"]
                       .get<std::string>().empty());
            CHECK(r.body["added"][0]["zone"].get<std::string>()
                  == "Fan 3");
            CHECK(r.body["added"][0]["zone_leds"].get<int>()
                  == 40);
        }
        const std::string fan3_oid =
            r.body["added"][0].value("object_id", std::string());
        const std::string fan3_bid =
            r.body["added"][0].value("binding", std::string());
        svc6.FlushPersistedScene();
        CHECK(persist_calls == 2);

        r = svc6.Dispatch(MsgType::GetScene, nlohmann::json::object());
        CHECK(r.code == ErrCode::Ok);
        if(r.code == ErrCode::Ok)
        {
            const nlohmann::json& doc = r.body["document"];
            bool binding_found = false;
            for(const nlohmann::json& b : doc["bindings"])
            {
                if(b.value("id", std::string()) == fan3_bid)
                {
                    binding_found = true;
                    /* identity came from the snapshot, never
                       the client body */
                    CHECK(b["controller_name"].get<std::string>()
                          == "Lian Li UNI FAN SL Wireless");
                    CHECK(b["serial"].get<std::string>()
                          == "AABBCCDDEEFF");
                    CHECK(b["zone_name"].get<std::string>()
                          == "Fan 3");
                    CHECK(b["zone_leds"].get<unsigned int>()
                          == 40);
                }
            }
            CHECK(binding_found);
            bool object_found = false;
            for(const nlohmann::json& o : doc["objects"])
            {
                if(o.value("id", std::string()) == fan3_oid)
                {
                    object_found = true;
                    CHECK(o["kind"].get<std::string>() ==
                          "device");
                    CHECK(o["binding"].get<std::string>() ==
                          fan3_bid);
                    /* verified — the identity resolved against
                       live topology, and verified is what lets
                       the mapper write the zone's LEDs */
                    CHECK(o["verified"].get<bool>() == true);
                    CHECK(o["emitters"].size() == 40);
                    /* ring layout for >12 LEDs — positions span
                       a disc, not a line */
                    bool spans_z = false;
                    for(size_t i = 0;
                        i < o["emitters"].size(); ++i)
                    {
                        const nlohmann::json& e =
                            o["emitters"][i];
                        CHECK(e["address"].get<int>() ==
                              (int)i);
                        if(e["pos"]["z"].get<double>() != 0.0)
                        {
                            spans_z = true;
                        }
                    }
                    CHECK(spans_z);
                }
            }
            CHECK(object_found);
        }

        /* Binding dedupe: the identical key reuses the existing
           binding — a second object, never a second row. */
        r = svc6.Dispatch(MsgType::LayoutCommand,
            nlohmann::json::object({
                { "verb",        "add_object" },
                { "controllers", nlohmann::json::array({ fan3 }) },
            }));
        CHECK(r.code == ErrCode::Ok);
        CHECK(r.body["added"][0]["binding"].get<std::string>()
              == fan3_bid);
        CHECK(r.body["added"][0]["object_id"].get<std::string>()
              != fan3_oid);
        r = svc6.Dispatch(MsgType::GetScene, nlohmann::json::object());
        CHECK(r.code == ErrCode::Ok);
        if(r.code == ErrCode::Ok)
        {
            CHECK(r.body["document"]["bindings"].size() == 1);
            /* objects: seed decor + fan3 obj + fan3 obj2 */
            CHECK(r.body["document"]["objects"].size() == 3);
        }

        /* Partial batch: one good, one ghost, one bad zone —
           successes apply; every failure reports in skipped. */
        r = svc6.Dispatch(MsgType::LayoutCommand,
            nlohmann::json::object({
                { "verb", "add_object" },
                { "controllers", nlohmann::json::array({
                    nlohmann::json::object({
                        {"device","Corsair: Vengeance RGB"},
                        {"serial","DDR5-1"},
                        {"location","smbus:0-DDR5-1"},
                        {"zone","DIMM"}}),
                    nlohmann::json::object({
                        {"device","ghost"},{"serial",""},
                        {"location",""},{"zone",""}}),
                    nlohmann::json::object({
                        {"device","Lian Li: SL Wireless"},
                        {"serial","AABBCCDDEEFF"},
                        {"location","Wireless: AA:BB:CC:DD:EE:FF"},
                        {"zone","Nope"}}),
                }) },
            }));
        CHECK(r.code == ErrCode::Ok);
        CHECK(r.body["added"].size() == 1);
        CHECK(r.body["skipped"].size() == 2);
        /* 12-LED zone -> row layout along x, addresses 0..11 */
        {
            const std::string dimm_oid =
                r.body["added"][0].value("object_id",
                                         std::string());
            r = svc6.Dispatch(MsgType::GetScene,
                              nlohmann::json::object());
            for(const nlohmann::json& o :
                    r.body["document"]["objects"])
            {
                if(o.value("id", std::string()) != dimm_oid)
                {
                    continue;
                }
                CHECK(o["emitters"].size() == 12);
                for(size_t i = 0; i < o["emitters"].size(); ++i)
                {
                    const nlohmann::json& e = o["emitters"][i];
                    CHECK(e["address"].get<int>() == (int)i);
                    CHECK(e["pos"]["z"].get<double>() == 0.0);
                    if(i > 0)
                    {
                        CHECK(e["pos"]["x"].get<double>() >
                              o["emitters"][i - 1]["pos"]["x"]
                                  .get<double>());
                    }
                }
            }
        }

        /* 1-LED zone -> a single point emitter. */
        r = svc6.Dispatch(MsgType::LayoutCommand,
            nlohmann::json::object({
                { "verb", "add_object" },
                { "controllers", nlohmann::json::array({
                    nlohmann::json::object({
                        {"device","Corsair: Vengeance RGB"},
                        {"serial","DDR5-1"},
                        {"location","smbus:0-DDR5-1"},
                        {"zone","Logo"}}) }) },
            }));
        CHECK(r.code == ErrCode::Ok);
        CHECK(r.body["added"].size() == 1);
        {
            const std::string logo_oid =
                r.body["added"][0].value("object_id",
                                         std::string());
            r = svc6.Dispatch(MsgType::GetScene,
                              nlohmann::json::object());
            for(const nlohmann::json& o :
                    r.body["document"]["objects"])
            {
                if(o.value("id", std::string()) == logo_oid)
                {
                    CHECK(o["emitters"].size() == 1);
                    CHECK(o["emitters"][0]["address"].get<int>()
                          == 0);
                }
            }
        }

        /* An EMPTY scene apply is a deliberate "new scene" —
           it applies, but persist_scene does NOT fire: an
           empty apply never clobbers the last good snapshot. */
        {
            const int pc = persist_calls;
            r = svc6.Dispatch(MsgType::SetScene,
                nlohmann::json::object({
                    { "document", nlohmann::json::object({
                        { "version",  2 },
                        { "name",     "scratch" },
                        { "bindings", nlohmann::json::array() },
                        { "objects",  nlohmann::json::array() },
                    }) },
                }));
            CHECK(r.code == ErrCode::Ok);
            svc6.FlushPersistedScene();
            CHECK(persist_calls == pc);

            /* ...and the bootstrap path: adding a device to the
               empty scene applies AND persists — the first
               added object makes the scene saveable again. */
            r = svc6.Dispatch(MsgType::LayoutCommand,
                nlohmann::json::object({
                    { "verb", "add_object" },
                    { "controllers", nlohmann::json::array({
                        nlohmann::json::object({
                            {"device","Lian Li: SL Wireless"},
                            {"serial","AABBCCDDEEFF"},
                            {"location",
                             "Wireless: AA:BB:CC:DD:EE:FF"},
                            {"zone","Fan 1"}}) }) },
                }));
            CHECK(r.code == ErrCode::Ok);
            svc6.FlushPersistedScene();
            CHECK(persist_calls == pc + 1);
            CHECK(persisted_name == "scratch");
        }
    }

    /*---- QA F11 + F04: wireless lane + never-delivered rows --
           frame_status/delivery_stats carry the wireless lane's
           OWN counters (the pipeline-wide aggregate mixes wired
           lanes and must never be read as a wireless rate) and
           list lanes that never delivered — "bound but never
           delivered" is a visible row, not an absent one. */
    {
        Engine           engine9;
        DeliveryPipeline pipe9;
        DeviceMapper     mapper9;

        DeviceSnapshot wsnap;
        wsnap.key = DeviceKey{ "Lian Li: SL Wireless",
                               "FAKEWIRELESS",
                               "Wireless: FA:KE:WI:RE:LE:SS", "" };
        wsnap.topology_generation = 1;
        wsnap.zones = { ZoneSnapshot{ "Fan 1", 40 } };
        wsnap.connection = ConnectionState::Connected;
        DeviceSnapshot wsnap_wired;
        wsnap_wired.key = DeviceKey{ "Corsair: Vengeance RGB",
                                     "DIMM-9", "smbus:0-DIMM-9", "" };
        wsnap_wired.topology_generation = 1;
        wsnap_wired.zones = { ZoneSnapshot{ "DIMM", 12 } };
        wsnap_wired.connection = ConnectionState::Connected;

        /* Both lanes registered; the transport accepts
           everything, but only the WIRED lane ever delivers —
           the wireless lane is the F04 "never delivered"
           shape. */
        pipe9.RegisterDevice(wsnap);
        pipe9.RegisterDevice(wsnap_wired);
        pipe9.SetTransport(std::make_shared<NullFrameSink>());

        RgbFrame wf;
        wf.controller           = wsnap_wired.key;
        wf.topology_generation  = 1;
        wf.sequence             = 1;
        wf.colors.assign(12, RgbColor{ 7, 8, 9 });
        CHECK(pipe9.SubmitLatest(wf).status ==
              SubmitStatus::Accepted);
        CHECK(pipe9.DrainDue(wsnap_wired.key, Clock::now()) == 1);

        EngineService::Deps d9;
        d9.pipeline  = &pipe9;
        d9.mapper    = &mapper9;
        d9.snapshots = [&wsnap, &wsnap_wired]
        {
            return std::vector<DeviceSnapshot>{ wsnap,
                                                wsnap_wired };
        };
        EngineService svc9(engine9, d9);

        /* frame_status: the wireless block is present and
           reports the WIRELESS lane's counters — zero
           deliveries — while the aggregate counts the wired
           delivery. */
        nlohmann::json fs;
        CHECK(svc9.SnapshotEvent(event_kind::kFrameStatus, fs));
        CHECK(fs.contains("wireless"));
        CHECK(fs["wireless"].is_object());
        if(fs["wireless"].is_object())
        {
            CHECK(fs["wireless"]["key"]["location"]
                      .get<std::string>()
                  == "Wireless: FA:KE:WI:RE:LE:SS");
            CHECK(fs["wireless"]["stats"]["interval_count"]
                      .get<uint64_t>() == 0);
            CHECK(fs["wireless"]["stats"]["transport_completed"]
                      .get<uint64_t>() == 0);
            CHECK(fs["wireless"]["suppressed"].get<bool>()
                  == false);
            CHECK(fs["wireless"]["owned"].get<bool>() == false);
            CHECK(fs["wireless"]["pending"].get<uint64_t>() == 0);
        }
        CHECK(fs["aggregate"]["transport_completed"]
                  .get<uint64_t>() == 1);
        {
            const nlohmann::json& rows = fs["controllers"];
            CHECK(rows.is_array());
            CHECK(rows.size() == 2);
            bool saw_wireless_row = false, saw_wired_row = false;
            for(const nlohmann::json& row : rows)
            {
                if(row["wireless"].get<bool>())
                {
                    /* lane exists, never delivered — visible
                       with zeroed stats and no colors */
                    saw_wireless_row = true;
                    CHECK(row["delivered"].get<bool>() == false);
                    CHECK(row["lane"].get<bool>() == true);
                    CHECK(row["colors"].empty());
                    CHECK(row["stats"]["submitted"]
                              .get<uint64_t>() == 0);
                }
                else
                {
                    saw_wired_row = true;
                    CHECK(row["delivered"].get<bool>() == true);
                    CHECK(row["colors"].size() == 12);
                }
            }
            CHECK(saw_wireless_row && saw_wired_row);
        }

        /* DELIVERY_STATS carries the same wireless block and
           the F04 map_stats observability object. */
        IpcReply r = svc9.Dispatch(MsgType::GetDeliveryStats,
                                   nlohmann::json::object());
        CHECK(r.code == ErrCode::Ok);
        CHECK(r.body["wireless"].is_object());
        CHECK(r.body["map_stats"].is_object());
        if(r.body["map_stats"].is_object())
        {
            CHECK(r.body["map_stats"].contains("objects_bound"));
            CHECK(r.body["map_stats"].contains(
                      "objects_writable"));
            CHECK(r.body["map_stats"].contains(
                      "skipped_unverified_or_unresolved"));
            CHECK(r.body["map_stats"].contains("frames_emitted"));
        }
        {
            const nlohmann::json& rows = r.body["controllers"];
            CHECK(rows.size() == 2);
            for(const nlohmann::json& row : rows)
            {
                CHECK(row.contains("delivered"));
                CHECK(row.contains("wireless"));
            }
        }

        /* DIAG_GET embeds both — and answers promptly (the
           F12 fix: no whole-StateJson re-run for the ipc
           field, one shared topology capture). */
        const TimePoint t0 = Clock::now();
        r = svc9.Dispatch(MsgType::DiagGet,
                          nlohmann::json::object());
        const double diag_ms =
            std::chrono::duration<double, std::milli>(
                Clock::now() - t0).count();
        CHECK(r.code == ErrCode::Ok);
        /* generous bound — the assertion guards the shape,
           the timing guards the contention regression */
        CHECK(diag_ms < 1000.0);
        CHECK(r.body["delivery"]["wireless"].is_object());
        CHECK(r.body["delivery"]["map_stats"].is_object());

        /* A wireless-absent engine reports wireless:null —
           "not measured", never wired numbers. */
        Engine           engine10;
        DeliveryPipeline pipe10;
        DeviceSnapshot   only_wired = wsnap_wired;
        pipe10.RegisterDevice(only_wired);
        EngineService::Deps d10;
        d10.pipeline  = &pipe10;
        d10.snapshots = [&only_wired]
        {
            return std::vector<DeviceSnapshot>{ only_wired };
        };
        EngineService svc10(engine10, d10);
        nlohmann::json fs10;
        CHECK(svc10.SnapshotEvent(event_kind::kFrameStatus,
                                  fs10));
        CHECK(fs10.contains("wireless"));
        CHECK(fs10["wireless"].is_null());
        /* the wired-only row still reports (delivered:false) */
        CHECK(fs10["controllers"].size() == 1);
        CHECK(fs10["controllers"][0]["delivered"].get<bool>()
              == false);

        /* No mapper wired -> map_stats is an honest null. */
        r = svc10.Dispatch(MsgType::GetDeliveryStats,
                           nlohmann::json::object());
        CHECK(r.code == ErrCode::Ok);
        CHECK(r.body["map_stats"].is_null());
    }

    /*---- QA F03: an apply-time playback rejection is a
           persistent, visible failure — never silent ---------
           OnSceneApplied now returns a Result; a rejected
           re-apply (the persisted selection can't be honored —
           here the wireless group vanished) must surface in
           PLAYBACK.detail AND emit a playback event, while the
           scene apply itself still succeeds — the commit stays
           good, the retained old effect is reported. */
    {
        Engine           engine11;
        DeliveryPipeline pipe11;

        std::vector<DeviceSnapshot> live_snaps;
        DeviceSnapshot wsnap;
        wsnap.key = DeviceKey{ "Lian Li: SL Wireless",
                               "FAKEWIRELESS",
                               "Wireless: FA:KE:WI:RE:LE:SS", "" };
        wsnap.topology_generation = 1;
        wsnap.zones = { ZoneSnapshot{ "Fan 1", 40 } };
        wsnap.modes = { "Direct", "Breathing" };
        wsnap.connection = ConnectionState::Connected;
        live_snaps.push_back(wsnap);
        pipe11.RegisterDevice(wsnap);

        PlaybackController::Deps pd;
        pd.snapshots = [&live_snaps] { return live_snaps; };
        pd.select_mode =
            [](const DeviceKey&, const std::string&,
               const PlaybackModePatch&, std::string&)
            { return true; };
        pd.set_suppressed =
            [&pipe11](const DeviceKey& k, bool s,
                      const std::string& r)
            { pipe11.SetSuppressed(k, s, r); };
        pd.drop_pending =
            [&pipe11](const DeviceKey& k)
            { return pipe11.DropPending(k); };
        pd.wait_idle =
            [&pipe11](const DeviceKey& k, Clock::duration t)
            { return pipe11.WaitIdle(k, t); };
        const TimePoint fixed_now{};
        pd.now = [fixed_now] { return fixed_now; };
        PlaybackController ctl11(pd);

        EngineService::Deps d11;
        d11.pipeline = &pipe11;
        d11.playback = &ctl11;
        EngineService svc11(engine11, d11);
        std::vector<IpcEvent> evs11;
        svc11.SetEventSink(
            [&](const IpcEvent& ev) { evs11.push_back(ev); });

        const nlohmann::json doc11 = nlohmann::json::object({
            { "version", 2 },
            { "name",    "f03" },
            { "effect",  nlohmann::json::object(
                {{"preset","breathing"}}) },
        });
        IpcReply r = svc11.Dispatch(MsgType::SetScene,
            nlohmann::json::object({{"document", doc11}}));
        CHECK(r.code == ErrCode::Ok);

        /* Persist an animation selection for this scene —
           the controller records it under the document key. */
        r = svc11.Dispatch(MsgType::SetPlaybackMode,
            nlohmann::json::object(
                {{"mode","animation_playback"}}));
        CHECK(r.code == ErrCode::Ok);
        CHECK(r.body["pending"].get<bool>() == true);

        /* The wireless group vanishes; re-applying the SAME
           scene asks the controller to restore a selection it
           cannot honor. The apply still succeeds — and the
           rejection now reaches the wire instead of vanishing
           with the dropped Result. */
        live_snaps.clear();
        evs11.clear();
        r = svc11.Dispatch(MsgType::SetScene,
            nlohmann::json::object({{"document", doc11}}));
        CHECK(r.code == ErrCode::Ok);
        CHECK(ctl11.LastApplyResult().accepted == false);
        bool saw_play_ev = false;
        for(const IpcEvent& ev : evs11)
        {
            if(ev.kind == event_kind::kPlayback)
            {
                saw_play_ev = true;
                CHECK(ev.data["detail"].is_string());
                CHECK(ev.data["detail"].get<std::string>()
                          .find("wireless") !=
                      std::string::npos);
            }
        }
        /* the persistent surface: PLAYBACK.detail carries the
           reason until a later transition succeeds */
        CHECK(saw_play_ev);
        r = svc11.Dispatch(MsgType::GetPlayback,
                           nlohmann::json::object());
        CHECK(r.code == ErrCode::Ok);
        CHECK(r.body["detail"].is_string());
        CHECK(r.body["detail"].get<std::string>()
                  .find("wireless") != std::string::npos);
    }

    /*---- concurrent layout verbs never lose an update -----
           (audit I-13). Each verb is read-copy-modify-apply on
           the current scene; two sessions (two reader threads)
           editing DIFFERENT objects must both stick. Thread A
           only ever moves "a", thread B only "b": after A's own
           apply, a.x must still be A's value — a revert means
           B applied a copy taken before A's apply. */
    {
        Engine           engine12;
        DeliveryPipeline pipe12;
        DeviceMapper     mapper12;
        EngineService::Deps d12;
        d12.pipeline = &pipe12;
        d12.mapper   = &mapper12;
        EngineService svc12(engine12, d12);
        const nlohmann::json two = nlohmann::json::object({
            { "version", 2 }, { "name", "two" },
            { "bindings", nlohmann::json::array() },
            { "objects", nlohmann::json::array({
                nlohmann::json::object({
                    { "id", "a" }, { "kind", "decor" } }),
                nlohmann::json::object({
                    { "id", "b" }, { "kind", "decor" } }) }) },
        });
        IpcReply r = svc12.Dispatch(MsgType::SetScene,
            nlohmann::json::object({{ "document", two }}));
        CHECK(r.code == ErrCode::Ok);

        std::atomic<int> lost{ 0 }, failed{ 0 };
        auto mover = [&](const char* oid)
        {
            for(int i = 1; i <= 300; ++i)
            {
                const double x = (double)i;
                const IpcReply m = svc12.Dispatch(
                    MsgType::LayoutCommand,
                    nlohmann::json::object({
                        { "verb", "set_position" },
                        { "object_id", oid },
                        { "position", nlohmann::json::object(
                            {{"x", x}, {"y", 0.0}, {"z", 0.0}}) },
                    }));
                if(m.code != ErrCode::Ok)
                {
                    ++failed;
                    continue;
                }
                const IpcReply g = svc12.Dispatch(MsgType::GetScene,
                    nlohmann::json::object());
                for(const nlohmann::json& o :
                        g.body["document"]["objects"])
                {
                    if(o.value("id", "") == oid &&
                       o["position"].value("x", -1.0) != x)
                    {
                        ++lost;
                    }
                }
            }
        };
        std::thread ta(mover, "a");
        std::thread tb(mover, "b");
        ta.join();
        tb.join();
        CHECK(failed.load() == 0);
        CHECK(lost.load() == 0);
        if(lost.load() != 0)
        {
            std::printf("  (lost layout updates: %d)\n",
                        lost.load());
        }
        r = svc12.Dispatch(MsgType::GetScene, nlohmann::json::object());
        for(const nlohmann::json& o : r.body["document"]["objects"])
        {
            CHECK(o["position"].value("x", -1.0) == 300.0);
        }
    }

    /*---- I-6: last-scene persistence runs OFF the request
       thread, coalesced latest-wins, and is durable: a slow
       snapshot write never stalls SET_SCENE, a burst lands
       the LAST document, and teardown drains the pending
       write (the restore-after-restart contract). ---------*/
    {
        Engine           engine12;
        DeliveryPipeline pipe12;
        DeviceMapper     mapper12;
        EngineService::Deps d12;
        d12.pipeline = &pipe12;
        d12.mapper   = &mapper12;
        std::mutex              gate_m;
        std::condition_variable gate_cv;
        bool                    gate_open = false;
        std::atomic<int>        calls12{ 0 };
        std::string             last12;          /* gate_m */
        std::thread::id         hook_thread;     /* gate_m */
        d12.persist_scene =
            [&](const studio::SceneDocument& dd)
            {
                std::unique_lock<std::mutex> lk(gate_m);
                gate_cv.wait(lk, [&] { return gate_open; });
                ++calls12;
                last12      = dd.name;
                hook_thread = std::this_thread::get_id();
            };
        auto scene_named = [](const std::string& name)
        {
            return nlohmann::json::object({{ "document",
                nlohmann::json::object({
                    { "version", 2 }, { "name", name },
                    { "bindings", nlohmann::json::array() },
                    { "objects", nlohmann::json::array({
                        nlohmann::json::object({
                            { "id", "case" },
                            { "kind", "decor" } }) }) } }) }});
        };
        auto svc12 = std::make_unique<EngineService>(engine12, d12);
        /* Three SET_SCENEs while the snapshot write is stuck:
           every one must answer promptly. */
        auto burst = std::async(std::launch::async, [&]
        {
            bool ok = true;
            for(const char* n : { "p1", "p2", "p3" })
            {
                ok = ok && svc12->Dispatch(MsgType::SetScene,
                               scene_named(n)).code == ErrCode::Ok;
            }
            return ok;
        });
        const bool prompt =
            burst.wait_for(std::chrono::seconds(3)) ==
            std::future_status::ready;
        CHECK(prompt);
        {
            std::lock_guard<std::mutex> lk(gate_m);
            gate_open = true;
        }
        gate_cv.notify_all();
        CHECK(burst.get());
        svc12->FlushPersistedScene();
        {
            std::lock_guard<std::mutex> lk(gate_m);
            /* p1 may already be in flight when p2/p3 land;
               p2 is superseded by p3 — never three writes,
               and the last write is the last applied scene. */
            CHECK(calls12.load() >= 1 && calls12.load() <= 2);
            CHECK(last12 == "p3");
            CHECK(hook_thread != std::this_thread::get_id());
        }

        /* Durability: a pending write is drained by teardown
           even when nobody flushed. */
        {
            std::lock_guard<std::mutex> lk(gate_m);
            gate_open = false;
        }
        auto second = std::async(std::launch::async, [&]
        {
            return svc12->Dispatch(MsgType::SetScene,
                       scene_named("q1")).code == ErrCode::Ok &&
                   svc12->Dispatch(MsgType::SetScene,
                       scene_named("q2")).code == ErrCode::Ok;
        });
        const bool prompt2 =
            second.wait_for(std::chrono::seconds(3)) ==
            std::future_status::ready;
        CHECK(prompt2);
        if(!prompt2)
        {
            /* Never tear the service down under a Dispatch
               still stuck in the hook. */
            {
                std::lock_guard<std::mutex> lk(gate_m);
                gate_open = true;
            }
            gate_cv.notify_all();
        }
        CHECK(second.get());
        auto teardown = std::async(std::launch::async,
                                   [&] { svc12.reset(); });
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        {
            std::lock_guard<std::mutex> lk(gate_m);
            gate_open = true;
        }
        gate_cv.notify_all();
        teardown.get();
        {
            std::lock_guard<std::mutex> lk(gate_m);
            CHECK(last12 == "q2");
        }
    }

    std::printf("ipc_engine_service_test: %d checks, %d failures\n",
                checks, failures);
    return failures ? 1 : 0;
}
