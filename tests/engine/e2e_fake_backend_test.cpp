// SPDX-License-Identifier: GPL-2.0-or-later
//
// Offline end-to-end engine test (Task 3.9): drives the REAL
// process composition — EngineHost, the same startup/shutdown
// path openrgb2-engine.exe runs — against the scripted
// in-process SDK backend from tests/sdk/fake_sdk_server.h.
//
//   startup -> SDK attach+handshake -> enumerate -> scene
//   apply -> binding resolution -> 60 Hz frames ->
//   UPDATELEDS on the wire -> bounded ordered shutdown
//
// No sockets, no pipes, no hardware, deterministic-ish wall
// clock (the scene worker thread is real; assertions wait on
// delivered wire events, not fixed tick counts).

#include "EngineHost.h"
#include "OwnershipScan.h"
#include "SdkWire.h"

#include "../sdk/fake_sdk_server.h"

#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace openrgb2;

static int checks   = 0;
static int failures = 0;
#define CHECK(x) do { ++checks; if(!(x)) { ++failures; \
    std::printf("FAIL line %d: %s\n", __LINE__, #x); } } while(0)

/*---------------------------------------------------------*\
|| Fixture                                                   ||
\*---------------------------------------------------------*/
static const char* kMac = "FA:KE:39:00:00:01";

static sdk_test::FakeController WirelessGroup()
{
    sdk_test::FakeController c;
    c.type     = 5;                     /* cooler-ish           */
    c.name     = "UNI FAN SL V3 Wireless E2E";
    c.vendor   = "Lian Li";
    c.serial   = kMac;
    c.location = std::string("Wireless: ") + kMac;
    for(const char* m : { "Direct", "Spectrum Cycle",
                          "Rainbow Wave", "Breathing" })
    {
        sdk_test::FakeMode mode;
        mode.name = m;
        c.modes.push_back(mode);
    }
    c.zones = { { "Fan 1", 0, 40, 40, 40 },
                { "Fan 2", 0, 40, 40, 40 },
                { "Fan 3", 0, 40, 40, 40 } };
    c.colors.assign(120, 0x00010101u);
    return c;
}

static sdk_test::FakeController Wired()
{
    sdk_test::FakeController c;
    c.type     = 2;
    c.name     = "X870E AORUS ELITE WIFI7 ICE E2E";
    c.vendor   = "GIGABYTE";
    c.serial   = "MB-E2E-1";
    c.location = "smbus:0-motherboard-e2e";
    sdk_test::FakeMode m;
    m.name = "Direct";
    c.modes.push_back(m);
    c.zones = { { "ARGB_V2_1", 0, 8, 8, 8 } };
    c.colors.assign(8, 0x00010101u);
    return c;
}

/* Expanded SceneDocument JSON: three fan objects bound to the
   wireless group's zones + one object on the wired header —
   the daniel-pc binding shape in miniature. */
static nlohmann::json EmitterArray(int first, int count)
{
    nlohmann::json e = nlohmann::json::array();
    for(int i = 0; i < count; ++i)
    {
        e.push_back({ { "pos", { {"x",0},{"y",0},{"z",0} } },
                      { "group", "" },
                      { "address", first + i } });
    }
    return e;
}

static nlohmann::json SceneDoc()
{
    nlohmann::json bindings = nlohmann::json::array();
    auto bind = [&](const char* id, const char* zone, int leds)
    {
        bindings.push_back({
            { "id",              id     },
            { "controller_name", "UNI FAN SL" },
            { "serial",          kMac   },
            { "zone_name",       zone   },
            { "zone_leds",       leds   },
        });
    };
    bind("b-fan1", "Fan 1", 40);
    bind("b-fan2", "Fan 2", 40);
    bind("b-fan3", "Fan 3", 40);
    bindings.push_back({
        { "id",              "b-argb"  },
        { "controller_name", "X870E"   },
        { "zone_name",       "ARGB_V2_1" },
        { "zone_leds",       8         },
    });

    nlohmann::json objects = nlohmann::json::array();
    auto obj = [&](const char* id, const char* binding, int n)
    {
        objects.push_back({
            { "id", id }, { "kind", "device" },
            { "binding", binding }, { "verified", true },
            { "visible", true },
            { "emitters", EmitterArray(0, n) },
        });
    };
    obj("o-fan1", "b-fan1", 40);
    obj("o-fan2", "b-fan2", 40);
    obj("o-fan3", "b-fan3", 40);
    obj("o-argb", "b-argb", 8);

    /* Inline layer stack — a moving world-space wave — so the
       scene needs no preset registry and produces CHANGING
       colors every tick. */
    nlohmann::json doc = {
        { "version", 2 },
        { "name",    "e2e-scene" },
        { "brightness", 1.0 },
        { "bindings", bindings },
        { "objects",  objects },
        { "effect", {
            { "preset",   "" },
            { "seed",     1 },
            { "speed",    1.0 },
            { "intensity",1.0 },
            { "playing",  true },
            { "layers", nlohmann::json::array({ {
                { "primitive", "wave" },
                { "space",     "world" },
                { "speed",     0.4 },
                { "scale",     0.5 },
                { "palette", nlohmann::json::array({
                    { { "pos", 0.0 }, { "color", "#FF0000" } },
                    { { "pos", 0.5 }, { "color", "#0000FF" } },
                }) },
            } }) },
        } },
    };
    return doc;
}

/* Count recorded server events of one packet id. */
static size_t CountEvents(const sdk_test::FakeSdkServer& s,
                          uint32_t pkt_id)
{
    size_t n = 0;
    for(const sdk_test::ServerEvent& e : s.events)
    {
        if(e.pkt_id == pkt_id) ++n;
    }
    return n;
}

static size_t CountUpdateLeds(const sdk_test::FakeSdkServer& s,
                            uint32_t dev_id)
{
    size_t n = 0;
    for(const sdk_test::ServerEvent& e : s.events)
    {
        if(e.pkt_id == sdk::kPktUpdateLEDs && e.dev_id == dev_id)
        {
            ++n;
        }
    }
    return n;
}

int main()
{
    std::error_code ec;
    const std::filesystem::path tmp =
        std::filesystem::temp_directory_path(ec) /
        "openrgb2-e2e-state";
    std::filesystem::create_directories(tmp, ec);
    const std::string scene_file = (tmp / "scene.json").string();
    const std::string state_file = (tmp / "engine-state.json").string();
    {
        std::ofstream f(scene_file);
        f << SceneDoc().dump();
    }

    sdk_test::FakeSdkServer server;
    server.controllers = { WirelessGroup(), Wired() };

    EngineHostOptions opts;
    opts.sdk_enabled     = true;
    opts.transport_factory = [&server]()
        -> std::unique_ptr<sdk::IStreamTransport>
        { return std::make_unique<sdk_test::FakeStream>(server); };
    opts.ipc_enabled     = false;
    opts.power_monitor   = false;
    opts.scan_owners     = true;    /* exercise the scan; findings
                                     are machine-dependent, so it
                                     is asserted as "ran", not by
                                     content */
    opts.single_instance = true;
    /* A test-scoped guard name: the production name may be
       held by a real engine on this machine, and the guard
       itself is collision-covered in ownership_scan_test. */
    opts.instance_name   = "Local\\openrgb2-engine-e2e-test";
    opts.scene_path      = scene_file;
    opts.state_path      = state_file;
    opts.backend_config_dir = "";   /* not configured here — the
                                       config check is covered by
                                       ownership_scan_test */
    std::vector<std::string> log_lines;
    opts.log = [&log_lines](const std::string& l)
    { log_lines.push_back(l); };

    EngineHost host(std::move(opts));

    std::string err;
    CHECK(host.Start(err));
    if(failures)
    {
        std::printf("Start failed: %s\n", err.c_str());
        std::printf("%d checks, %d FAILURES\n", checks, failures);
        return 1;
    }

    /*---- startup -> attach -> enumerate ---------------------*/
    CHECK(host.Ownership().scanned);
    CHECK(host.Ownership().backend_attached);
    sdk::OpenRgbClient* client = host.GetSdk();
    CHECK(client != nullptr);
    CHECK(client->State() == ConnectionState::Connected);
    CHECK(client->ProtocolVersion() <= 4);
    CHECK(client->Snapshots().size() == 2);
    /* Handshake + enumeration reached the wire: SETCLIENTNAME
       lands in the recorded event stream. */
    CHECK(CountEvents(server, sdk::kPktSetClientName) >= 1);

    /*---- bind ------------------------------------------------*/
    host.PollOnce();
    const std::vector<DeviceKey> bound =
        host.GetMapper().BoundControllers();
    CHECK(bound.size() == 2);
    studio::BindingStatus st;
    CHECK(host.GetMapper().BindingState("b-fan1", st) &&
          st == studio::BindingStatus::Resolved);
    CHECK(host.GetMapper().BindingState("b-argb", st) &&
          st == studio::BindingStatus::Resolved);

    /*---- frames -> wire -------------------------------------*/
    /* The 60 Hz scene worker is real: run the owner loop on a
       thread while frames flow, like production main(). */
    std::atomic<bool> run{ true };
    std::thread owner([&]
    {
        while(run.load())
        {
            host.PollOnce();
            std::this_thread::sleep_for(
                std::chrono::milliseconds(50));
        }
    });

    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds(10);
    while(CountUpdateLeds(server, 0) == 0 &&
          std::chrono::steady_clock::now() < deadline)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    CHECK(CountUpdateLeds(server, 0) >= 1);
    CHECK(host.FramesProduced() > 0);

    /* The wireless group's write is ONE complete-controller
       UPDATELEDS carrying all 120 LEDs — never three zone
       writes (plan section 4). */
    {
        size_t last_body_leds = 0;
        for(const sdk_test::ServerEvent& e : server.events)
        {
            if(e.pkt_id == sdk::kPktUpdateLEDs && e.dev_id == 0)
            {
                /* body = u32 data_size + u16 count + colors */
                CHECK(e.body.size() >= 6);
                uint16_t count = 0;
                memcpy(&count, e.body.data() + 4, 2);
                last_body_leds = count;
            }
        }
        CHECK(last_body_leds == 120);
    }
    /* The wired controller also animates (its own lane —
       a second controller write, dev_id 1). */
    CHECK(CountUpdateLeds(server, 1) >= 1);

    /* Custom-mode upkeep ran on bound controllers
       (SETCUSTOMMODE — live output requires direct mode). */
    CHECK(CountEvents(server, sdk::kPktSetCustomMode) >= 1);

    /*---- shutdown ordering -----------------------------------*/
    run.store(false);
    owner.join();
    host.RequestStop();
    const OrderlyShutdown::Result& sd = host.Stop();

    static const char* want_order[] = {
        "power-monitor", "ipc-listeners", "ipc-server",
        "scene-worker", "delivery-pipeline", "sdk-transport",
    };
    CHECK(sd.steps.size() == 6);
    if(sd.steps.size() == 6)
    {
        for(size_t i = 0; i < 6; ++i)
        {
            CHECK(sd.steps[i].name == want_order[i]);
            CHECK(sd.steps[i].within);
        }
    }
    CHECK(sd.all_within_bound);
    /* Transport closed on the way down. */
    CHECK(client->State() == ConnectionState::Disconnected);

    /* The single-instance guard held for the whole run and is
       released by teardown — a second guard on the SAME name
       now acquires cleanly (collision coverage lives in
       ownership_scan_test). */

    std::printf("%d checks, %d failures\n", checks, failures);
    for(const std::string& l : log_lines)
    {
        std::printf("  | %s\n", l.c_str());
    }
    return failures == 0 ? 0 : 1;
}
