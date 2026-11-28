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
#include "EngineService.h"
#include "IpcProtocol.h"
#include "OwnershipScan.h"
#include "SdkWire.h"

#include "scene/SceneJson.h"

#include "../sdk/fake_sdk_server.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <process.h>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
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
    /* The zone enumerates EMPTY (resizable 0-256) like a real
       Gigabyte ARGB header after the backend loses its saved
       zone sizes — the scene binding declares 8 LEDs, and the
       engine's zone-size reconciliation must RESIZEZONE it
       back before frames can flow (the "motherboard scene not
       configured" recurrence this regression pins). */
    c.zones = { { "ARGB_V2_1", 0, 0, 256, 0 } };
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

/* Count events inside a locked snapshot (HandleSend appends
   on lane threads — iterate a copy, never the live vector). */
static size_t CountEvents(const std::vector<sdk_test::ServerEvent>& ev,
                          uint32_t pkt_id)
{
    size_t n = 0;
    for(const sdk_test::ServerEvent& e : ev)
    {
        if(e.pkt_id == pkt_id) ++n;
    }
    return n;
}

/* A Start() that fails AFTER the IPC listeners and the calibration
   worker are up (here: an unreadable --scene) must still be torn
   down by Stop() — previously Stop() returned early on !started_
   and the listener/calibration threads outlived the members they
   call into. */
static void TestPartialStartTeardown(const std::filesystem::path& tmp)
{
    const std::string pid = std::to_string(_getpid());
    EngineHostOptions o;
    o.sdk_enabled     = false;
    o.ipc_enabled     = true;
    o.command_pipe_name =
        "\\\\.\\pipe\\openrgb2-e2e-partial-" + pid;
    o.diagnostics_pipe_name =
        "\\\\.\\pipe\\openrgb2-e2e-partial-diag-" + pid;
    o.power_monitor   = false;
    o.scan_owners     = false;
    o.single_instance = true;
    o.instance_name   = "Local\\openrgb2-engine-e2e-partial-" + pid;
    o.scene_path      = (tmp / "missing-scene.json").string();
    o.state_path      = (tmp / "partial-state.json").string();
    o.log             = [](const std::string&) {};

    EngineHost host(std::move(o));
    std::string err;
    CHECK(!host.Start(err));
    CHECK(!host.Started());
    const OrderlyShutdown::Result& sd = host.Stop();
    bool saw_ipc = false, saw_cal = false;
    for(const OrderlyShutdown::StepResult& st : sd.steps)
    {
        saw_ipc = saw_ipc || st.name == "ipc-listeners";
        saw_cal = saw_cal || st.name == "calibration";
    }
    CHECK(saw_ipc);
    CHECK(saw_cal);
    CHECK(sd.all_within_bound);
}

/*---------------------------------------------------------*\
|| E-12b fixture: one wired controller sitting in a hardware |
|| effect ("Rainbow") before the engine switches it to       |
|| Direct, and a scene bound to its one 8-LED zone.          |
\*---------------------------------------------------------*/
static sdk_test::FakeController RainbowWired()
{
    sdk_test::FakeController c;
    c.type     = 2;
    c.name     = "E12B RAINBOW BOARD";
    c.vendor   = "GIGABYTE";
    c.serial   = "MB-E12B-1";
    c.location = "smbus:0-e12b";
    sdk_test::FakeMode direct;
    direct.name = "Direct";
    sdk_test::FakeMode rainbow;
    rainbow.name       = "Rainbow";
    rainbow.speed      = 77;
    rainbow.direction  = 1;
    rainbow.colors_max = 1;
    rainbow.colors     = { 0x00FF0000u };
    c.modes       = { direct, rainbow };
    c.active_mode = 1;                    /* hardware effect on */
    c.zones       = { { "ARGB", 0, 8, 8, 8 } };
    c.colors.assign(8, 0x00010101u);
    return c;
}

static nlohmann::json RainbowSceneDoc()
{
    nlohmann::json doc = SceneDoc();
    doc["name"]     = "e12b-scene";
    doc["bindings"] = nlohmann::json::array({ {
        { "id",              "b-rb"       },
        { "controller_name", "E12B RAINBOW" },
        { "zone_name",       "ARGB"       },
        { "zone_leds",       8            },
    } });
    doc["objects"] = nlohmann::json::array({ {
        { "id", "o-rb" }, { "kind", "device" },
        { "binding", "b-rb" }, { "verified", true },
        { "visible", true },
        { "emitters", EmitterArray(0, 8) },
    } });
    return doc;
}

static EngineHostOptions E12bOptions(sdk_test::FakeSdkServer& server,
                                     const std::filesystem::path& dir,
                                     const char* tag)
{
    EngineHostOptions o;
    o.sdk_enabled       = true;
    o.transport_factory =
        [&server]() -> std::unique_ptr<sdk::IStreamTransport>
        { return std::make_unique<sdk_test::FakeStream>(server); };
    o.ipc_enabled     = false;
    o.power_monitor   = false;
    o.scan_owners     = false;
    o.reactive_inputs = false;
    o.single_instance = true;
    o.instance_name   = std::string("Local\\openrgb2-engine-e12b-") +
                        tag + "-" + std::to_string(_getpid());
    o.scene_restore   = false;
    o.state_path      = (dir / (std::string(tag) + "-state.json"))
                            .string();
    o.log             = [](const std::string&) {};
    return o;
}

static bool ApplyDoc(EngineHost& host, const nlohmann::json& j)
{
    auto doc = std::make_shared<studio::SceneDocument>();
    std::vector<std::string> errs;
    if(!studio::FromJson(j, *doc, &errs))
    {
        return false;
    }
    std::string err;
    return host.GetService()->ApplySceneDocument(doc, err);
}

/* E-12b step 2: the upkeep records each controller's active
   hardware mode BEFORE its first SetCustomMode — and only the
   first observation per run counts. */
static void TestPriorModeRecording(const std::filesystem::path& tmp)
{
    sdk_test::FakeSdkServer server;
    server.controllers = { RainbowWired() };
    EngineHost host(E12bOptions(server, tmp, "prior"));
    std::string err;
    CHECK(host.Start(err));
    CHECK(host.PriorModes().empty());       /* nothing bound yet */
    CHECK(ApplyDoc(host, RainbowSceneDoc()));
    for(int i = 0; i < 20 &&
        server.CountEvents(sdk::kPktSetCustomMode) == 0; ++i)
    {
        host.PollOnce();
    }
    CHECK(server.CountEvents(sdk::kPktSetCustomMode) >= 1);
    std::map<DeviceKey, PriorMode> pm = host.PriorModes();
    CHECK(pm.size() == 1);
    if(pm.size() == 1)
    {
        const PriorMode& p = pm.begin()->second;
        CHECK(pm.begin()->first.serial == "MB-E12B-1");
        CHECK(p.name == "Rainbow");
        CHECK(p.speed == 77);
        CHECK(p.direction == 1);
        CHECK(p.colors.size() == 1 && p.colors[0] == 0x00FF0000u);
    }
    /* A re-enumeration bumps the generation, so the upkeep
       re-asserts Direct — the controller now REPORTS Direct,
       but the first observation stands. */
    const size_t custom_before =
        server.CountEvents(sdk::kPktSetCustomMode);
    CHECK(host.GetSdk()->Enumerate());
    host.PollOnce();
    CHECK(server.CountEvents(sdk::kPktSetCustomMode) > custom_before);
    pm = host.PriorModes();
    CHECK(pm.size() == 1 && pm.begin()->second.name == "Rainbow");
    host.RequestStop();
    host.Stop();
}

/* E-12b step 4: idle = no scene (or an empty one) applied for
   output_idle_after; on_idle runs ONCE per idle entry through
   the exit executor, the next applied scene clears it, and the
   power gate defers it. Clock injected — no 5 s sleeps. */
static bool LastFrameBlack(const sdk_test::FakeSdkServer& server,
                           uint32_t dev)
{
    bool black = false, seen = false;
    for(const sdk_test::ServerEvent& e : server.SnapshotEvents())
    {
        if(e.pkt_id != sdk::kPktUpdateLEDs || e.dev_id != dev ||
           e.body.size() < 6)
        {
            continue;
        }
        uint16_t count = 0;
        memcpy(&count, e.body.data() + 4, 2);
        if(e.body.size() < 6u + 4u * count)
        {
            continue;
        }
        seen  = true;
        black = count > 0;
        for(uint16_t i = 0; i < count; ++i)
        {
            uint32_t c = 0;
            memcpy(&c, e.body.data() + 6 + 4 * i, 4);
            black = black && (c & 0x00FFFFFFu) == 0;
        }
    }
    return seen && black;
}

static size_t CountRestores(const sdk_test::FakeSdkServer& server)
{
    size_t n = 0;
    for(const sdk_test::ServerEvent& e : server.SnapshotEvents())
    {
        if(e.pkt_id == sdk::kPktUpdateMode && e.dev_id == 0 &&
           e.body.size() >= 8)
        {
            int32_t idx = -1;
            memcpy(&idx, e.body.data() + 4, 4);
            n += idx == 1 ? 1 : 0;              /* "Rainbow" */
        }
    }
    return n;
}

/* R-9: the engine's backend supervisor end to end on the REAL
   EngineHost composition. The fake backend "dies" (listener
   gone, peer closed); after 5 s on the supervisor's fake clock
   the injected launcher is asked to spawn the backend with the
   script's exact arguments, "brings it up", and the SDK link
   recovers. Then: the stop marker suppresses a respawn; a
   foreign port holder is refused (reported once, never
   replaced); a wireless group wedged+failed for 8 min closes
   the verified-own backend through the injected closer and the
   dead-backend rule brings it back; STATE.supervisor and the
   "supervisor" event report every action. */
static void TestBackendSupervisor(const std::filesystem::path& tmp)
{
    sdk_test::FakeSdkServer server;
    server.controllers = { Wired() };
    /* The lianli-wireless fork answers the status request from
       the first connect (a healthy group), so the client
       latches "supported" before the wedge appears. */
    auto wireless = [](const char* state, bool wedged)
    {
        return nlohmann::json::object({
            { "groups", nlohmann::json::array({ nlohmann::json::object({
                { "mac", kMac }, { "state", state },
                { "wedged", wedged } }) }) } }).dump();
    };
    server.wireless_status_json = wireless("ready", false);
    const std::filesystem::path inst = tmp / "sup-install";
    const std::filesystem::path cfg  = inst / "state" / "backend";
    const std::filesystem::path marker =
        inst / "state" / "openrgb2.stopping";
    std::error_code ec;
    std::filesystem::create_directories(cfg, ec);
    std::filesystem::remove(marker, ec);

    std::mutex              mu;
    sdk_test::FakeStream*   stream = nullptr;
    BackendProbe            probe;            /* guarded by mu */
    std::vector<BackendLaunch> launches;      /* guarded by mu */
    std::vector<uint32_t>   closes;           /* guarded by mu */
    std::atomic<int64_t>    fake_ms{ 0 };
    const TimePoint         base = Clock::now();

    EngineHostOptions o = E12bOptions(server, tmp, "sup");
    o.transport_factory = [&server, &stream, &mu]()
        -> std::unique_ptr<sdk::IStreamTransport>
    {
        auto s = std::make_unique<sdk_test::FakeStream>(server);
        std::lock_guard<std::mutex> g(mu);
        stream = s.get();
        return s;
    };
    o.sdk_backoff.initial_ms   = 20;
    o.sdk_backoff.max_ms       = 100;
    o.sdk_backoff.max_attempts = 3;
    o.sdk_reprobe              = std::chrono::milliseconds(100);
    o.sdk_topology_poll        = std::chrono::milliseconds(20);
    o.backend_exe        = (inst / "backend" / "OpenRGB.exe").string();
    o.backend_config_dir = cfg.string();
    o.supervisor_clock = [&fake_ms, base]
    { return base + std::chrono::milliseconds(fake_ms.load()); };
    o.backend_probe = [&mu, &probe]
    {
        std::lock_guard<std::mutex> g(mu);
        return probe;
    };
    o.backend_launcher = [&mu, &probe, &launches, &server](
        const BackendLaunch& l, uint32_t& pid, std::string&)
    {
        std::lock_guard<std::mutex> g(mu);
        launches.push_back(l);
        probe.own_pid    = 4242;
        server.listening = true;            /* it comes up */
        pid = 4242;
        return true;
    };
    o.backend_closer = [&mu, &probe, &closes, &server, &stream](
        uint32_t pid, std::string&)
    {
        std::lock_guard<std::mutex> g(mu);
        closes.push_back(pid);
        probe.own_pid    = 0;               /* it exits */
        server.listening = false;
        if(stream) stream->SetPeerClosed(true);
        return true;
    };

    EngineHost host(std::move(o));
    std::string err;
    CHECK(host.Start(err));
    std::vector<nlohmann::json> events;
    std::mutex ev_mu;
    host.GetService()->SetEventSink([&events, &ev_mu](const ipc::IpcEvent& e)
    {
        if(e.kind == ipc::event_kind::kSupervisor)
        {
            std::lock_guard<std::mutex> g(ev_mu);
            events.push_back(e.data);
        }
    });
    auto connected = [&host]
    { return host.GetSdk()->State() == ConnectionState::Connected; };
    auto pump_until = [&host](const std::function<bool()>& f, int ms)
    {
        const auto end = std::chrono::steady_clock::now() +
                         std::chrono::milliseconds(ms);
        while(std::chrono::steady_clock::now() < end)
        {
            host.PollOnce();
            if(f()) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return f();
    };
    auto launch_count = [&mu, &launches]
    { std::lock_guard<std::mutex> g(mu); return launches.size(); };
    auto kill_backend = [&mu, &server, &stream, &probe]
    {
        std::lock_guard<std::mutex> g(mu);
        server.listening = false;
        probe.own_pid    = 0;
        if(stream) stream->SetPeerClosed(true);
    };
    auto event_count = [&ev_mu, &events]
    { std::lock_guard<std::mutex> g(ev_mu); return events.size(); };

    CHECK(pump_until(connected, 5000));
    CHECK(host.SupervisorJson().value("enabled", false));

    /*---- the backend dies -> spawn after 5 s ---------------*/
    kill_backend();
    CHECK(pump_until([&] { return !connected(); }, 5000));
    fake_ms += 4000;
    for(int i = 0; i < 20; ++i) host.PollOnce();
    CHECK(launch_count() == 0);              /* inside 5 s */
    fake_ms += 2000;
    CHECK(pump_until([&] { return launch_count() == 1; }, 2000));
    {
        std::lock_guard<std::mutex> g(mu);
        if(!launches.empty())
        {
            CHECK(launches[0].exe == (inst / "backend" / "OpenRGB.exe")
                                         .string());
            CHECK(launches[0].args == BackendServerArgs(6744));
            CHECK(launches[0].cwd == cfg.string());
        }
    }
    CHECK(pump_until(connected, 5000));      /* the link recovers */
    {
        const nlohmann::json sj = host.SupervisorJson();
        CHECK(sj.value("backend_restarts", 0) == 1);
        CHECK(sj.value("last_reason", std::string()) == "backend_dead");
        CHECK(sj["last_at"].is_string());
        CHECK(sj["history"].is_array() && sj["history"].size() == 1);
    }
    CHECK(event_count() == 1);
    {
        std::lock_guard<std::mutex> g(ev_mu);
        if(!events.empty())
        {
            CHECK(events[0].value("action", std::string()) ==
                  "backend_spawned");
            CHECK(events[0].value("reason", std::string()) ==
                  "backend_dead");
            CHECK(events[0]["status"].is_object());
        }
    }
    /* STATE carries the same status. */
    {
        const ipc::IpcReply r = host.GetService()->Dispatch(
            ipc::MsgType::GetState, nlohmann::json::object());
        CHECK(r.body["supervisor"].is_object() &&
              r.body["supervisor"].value("backend_restarts", 0) == 1);
    }

    /*---- a stop in progress: never respawned ----------------*/
    {
        std::ofstream(marker) << "{\"pid\":1}";
    }
    kill_backend();
    CHECK(pump_until([&] { return !connected(); }, 5000));
    fake_ms += 120000;
    for(int i = 0; i < 40; ++i) host.PollOnce();
    CHECK(launch_count() == 1);
    std::filesystem::remove(marker, ec);
    CHECK(pump_until([&] { return launch_count() == 2; }, 2000));
    CHECK(pump_until(connected, 5000));

    /*---- a foreign port holder: refused, reported once -----*/
    kill_backend();
    {
        std::lock_guard<std::mutex> g(mu);
        probe.port = PortHolder::Foreign;
    }
    CHECK(pump_until([&] { return !connected(); }, 5000));
    const size_t ev_before = event_count();
    fake_ms += 300000;
    for(int i = 0; i < 40; ++i)
    {
        host.PollOnce();
        fake_ms += 2000;
    }
    CHECK(launch_count() == 2);
    CHECK(event_count() == ev_before + 1);
    {
        std::lock_guard<std::mutex> g(ev_mu);
        if(!events.empty())
        {
            CHECK(events.back().value("action", std::string()) ==
                  "notice");
            CHECK(events.back().value("reason", std::string()) ==
                  "spawn_refused");
        }
    }
    {
        std::lock_guard<std::mutex> g(mu);
        probe.port = PortHolder::None;
    }
    CHECK(pump_until([&] { return launch_count() == 3; }, 2000));
    CHECK(pump_until(connected, 5000));

    /*---- wireless wedge for 8 min -> close + respawn --------*/
    {
        std::lock_guard<std::mutex> g(mu);
        server.wireless_status_json = wireless("failed", true);
    }
    /* The status reaches the client on its refresh cadence. */
    CHECK(pump_until([&] {
        sdk::WirelessStatusSnapshot ws;
        host.GetSdk()->WirelessStatus(ws);
        return ws.have_data && !ws.status.groups.empty() &&
               ws.status.groups[0].wedged;
    }, 5000));
    fake_ms += 7 * 60 * 1000;
    for(int i = 0; i < 20; ++i) host.PollOnce();
    {
        std::lock_guard<std::mutex> g(mu);
        CHECK(closes.empty());
    }
    fake_ms += 61 * 1000;
    CHECK(pump_until([&] {
        std::lock_guard<std::mutex> g(mu);
        return !closes.empty();
    }, 2000));
    {
        std::lock_guard<std::mutex> g(mu);
        CHECK(closes.size() == 1 && closes[0] == 4242);
        server.wireless_status_json = wireless("ready", false);
    }
    CHECK(pump_until([&] { return !connected(); }, 5000));
    fake_ms += 6000;
    CHECK(pump_until([&] { return launch_count() == 4; }, 2000));
    CHECK(pump_until(connected, 5000));
    {
        const nlohmann::json sj = host.SupervisorJson();
        CHECK(sj.value("wedge_restarts", 0) == 1);
        CHECK(sj.value("backend_restarts", 0) == 4);
        CHECK(sj.value("last_reason", std::string()) == "wireless_wedge");
    }
    {
        std::lock_guard<std::mutex> g(ev_mu);
        bool saw_close = false;
        for(const nlohmann::json& e : events)
        {
            saw_close = saw_close ||
                e.value("action", std::string()) ==
                    "backend_closed_for_wedge";
        }
        CHECK(saw_close);
    }
    std::printf("supervisor: %zu launches, %zu events, status %s\n",
                launch_count(), event_count(),
                host.SupervisorJson().dump().substr(0, 160).c_str());

    host.RequestStop();
    host.Stop();
}

static void TestIdlePolicy(const std::filesystem::path& tmp)
{
    sdk_test::FakeSdkServer server;
    server.controllers = { RainbowWired() };
    std::atomic<int64_t> now_ms{ 1000000 };
    EngineHostOptions o = E12bOptions(server, tmp, "idle");
    const TimePoint base = Clock::now();
    o.output_clock = [&now_ms, base]
    { return base + std::chrono::milliseconds(now_ms.load()); };
    EngineHost host(std::move(o));
    std::string err;
    CHECK(host.Start(err));

    OutputPolicy p;
    p.on_idle = OutputAction::Restore;
    CHECK(host.SetOutputPolicy(p, err));

    const nlohmann::json empty_doc = {
        { "version", 2 }, { "name", "empty" },
        { "objects", nlohmann::json::array() },
    };
    auto advance = [&](int64_t ms)
    {
        now_ms += ms;
        host.PollOnce();
    };

    /* Boot with nothing applied, but nothing was switched to
       Direct by us either: idle is entered with no target. */
    advance(0);
    advance(6000);
    CHECK(host.OutputIdle());
    CHECK(host.IdleReport().empty());
    CHECK(server.CountEvents(sdk::kPktUpdateMode) == 0);

    /* A scene clears idle; the upkeep records Rainbow. */
    CHECK(ApplyDoc(host, RainbowSceneDoc()));
    advance(0);
    CHECK(!host.OutputIdle());
    for(int i = 0; i < 20 &&
        server.CountEvents(sdk::kPktSetCustomMode) == 0; ++i)
    {
        advance(10);
    }
    CHECK(server.CountEvents(sdk::kPktSetCustomMode) >= 1);
    advance(60000);                      /* a scene is never idle */
    CHECK(!host.OutputIdle());

    /* Empty scene: idle only after the full 5 s. */
    CHECK(ApplyDoc(host, empty_doc));
    advance(0);
    advance(4900);
    CHECK(!host.OutputIdle());
    CHECK(CountRestores(server) == 0);
    advance(200);
    CHECK(host.OutputIdle());
    CHECK(CountRestores(server) == 1);
    {
        const std::vector<OutputPolicyOutcome> rep = host.IdleReport();
        CHECK(rep.size() == 1);
        CHECK(rep.size() == 1 && rep[0].outcome == "restored" &&
              rep[0].detail == "Rainbow");
    }
    advance(10000);                       /* once per idle entry */
    CHECK(CountRestores(server) == 1);

    /* The next scene clears idle and re-asserts Direct (the
       restore handed the device back to its hardware mode). */
    const size_t custom_before =
        server.CountEvents(sdk::kPktSetCustomMode);
    CHECK(ApplyDoc(host, RainbowSceneDoc()));
    advance(0);
    advance(10);
    CHECK(!host.OutputIdle());
    CHECK(server.CountEvents(sdk::kPktSetCustomMode) > custom_before);
    /* Wait for a live frame so the black below is provably the
       idle write, not an initial state. */
    for(int i = 0; i < 200 && LastFrameBlack(server, 0); ++i)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    /* Second idle entry, now black — deferred while the power
       gate (display off) holds: display-off is not idle. */
    p.on_idle = OutputAction::Black;
    CHECK(host.SetOutputPolicy(p, err));
    CHECK(ApplyDoc(host, empty_doc));
    advance(0);
    host.GetLifecycle()->PostEvent(PowerLifecycle::Event::DisplayOff);
    advance(0);
    CHECK(host.GetLifecycle()->OutputPaused());
    advance(6000);
    CHECK(!host.OutputIdle());
    host.GetLifecycle()->PostEvent(PowerLifecycle::Event::DisplayOn);
    for(int i = 0; i < 200 && !host.OutputIdle(); ++i)
    {
        /* the lifecycle's wake path runs on the real clock */
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        advance(100);
    }
    CHECK(host.OutputIdle());
    {
        const std::vector<OutputPolicyOutcome> rep = host.IdleReport();
        CHECK(rep.size() == 1 && rep[0].outcome == "black");
    }
    CHECK(LastFrameBlack(server, 0));
    CHECK(CountRestores(server) == 1);

    host.RequestStop();
    host.Stop();
}

int main()
{
    std::error_code ec;
    const std::filesystem::path tmp =
        std::filesystem::temp_directory_path(ec) /
        ("openrgb2-e2e-state-" + std::to_string(_getpid()));
    std::filesystem::create_directories(tmp, ec);
    const std::string scene_file = (tmp / "scene.json").string();
    const std::string state_file = (tmp / "engine-state.json").string();
    {
        std::ofstream f(scene_file);
        f << SceneDoc().dump();
    }

    sdk_test::FakeSdkServer server;
    server.controllers = { WirelessGroup(), Wired() };

    /* B-1: injected scan sources so the re-scan is
       deterministic (the real enumeration is covered by
       e2e_lifecycle_test's competitor fixture). */
    std::mutex                 scan_mutex;
    std::vector<ProcessEntry>  scan_procs = { { 4242, "Steam.exe" } };
    int                        scan_dl    = 0;

    EngineHostOptions opts;
    opts.process_source = [&scan_mutex, &scan_procs]()
    {
        std::lock_guard<std::mutex> g(scan_mutex);
        return scan_procs;
    };
    opts.dynamic_lighting_source = [&scan_mutex, &scan_dl]()
    {
        std::lock_guard<std::mutex> g(scan_mutex);
        return scan_dl;
    };
    opts.owner_rescan    = std::chrono::milliseconds(1);
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
    opts.instance_name   = ("Local\\openrgb2-engine-e2e-test-" + std::to_string(_getpid()));
    opts.scene_path      = scene_file;
    opts.state_path      = state_file;
    opts.backend_config_dir = "";   /* not configured here — the
                                       config check is covered by
                                       ownership_scan_test */
    std::mutex              log_mutex;
    std::vector<std::string> log_lines;
    opts.log = [&log_lines, &log_mutex](const std::string& l)
    {
        /* Log() is invoked from the main thread (startup) AND
           the owner-loop thread (lifecycle/playback) —
           serialize the append. */
        std::lock_guard<std::mutex> g(log_mutex);
        log_lines.push_back(l);
    };

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
    /* The wire form of the same report (Task 4.7 review): the
       REAL serializer's key names + scanned:true, asserted
       where the scan actually ran — a scripted copy would
       ship a field-name typo green. Findings are machine-
       dependent, so the lists are shape-asserted only. */
    {
        const nlohmann::json oj = host.OwnershipJson();
        CHECK(oj["scanned"].get<bool>() == true);
        CHECK(oj["backend"]["attached"].get<bool>() == true);
        CHECK(oj["backend"].contains("listening"));
        CHECK(oj["conflicts"].is_array());
        CHECK(oj["notices"].is_array());
        /* config.checked is machine-dependent (a real backend
           on this port would resolve an exe dir) — assert the
           key, not the value */
        CHECK(oj["config"].contains("checked"));
        CHECK(oj["config"]["findings"].is_array());
        CHECK(oj["config"]["legacy_producers"].is_array());
        CHECK(oj["dynamic_lighting"].get<std::string>() == "off");
        CHECK(oj["conflicts"].empty());
    }

    /*---- B-1: competitors appearing AFTER startup are found --*/
    {
        auto has_conflict = [&host](const std::string& owner)
        {
            for(const OwnerFinding& f : host.Ownership().findings)
            {
                if(f.conflict && f.owner == owner) return true;
            }
            return false;
        };
        auto log_has = [&log_lines, &log_mutex](const std::string& s)
        {
            std::lock_guard<std::mutex> g(log_mutex);
            for(const std::string& l : log_lines)
            {
                if(l.find(s) != std::string::npos) return true;
            }
            return false;
        };
        /* R-10: the re-scan pushes an "ownership" event per
           change — exactly one per scan that changed, none for
           a scan that did not. */
        std::mutex                   ev_mutex;
        std::vector<nlohmann::json>  own_events;
        CHECK(host.GetService() != nullptr);
        host.GetService()->SetEventSink(
            [&ev_mutex, &own_events](const ipc::IpcEvent& ev)
            {
                if(ev.kind == ipc::event_kind::kOwnership)
                {
                    std::lock_guard<std::mutex> g(ev_mutex);
                    own_events.push_back(ev.data);
                }
            });
        auto own_count = [&ev_mutex, &own_events]
        {
            std::lock_guard<std::mutex> g(ev_mutex);
            return own_events.size();
        };
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        host.PollOnce();                  /* unchanged scan */
        CHECK(own_count() == 0);
        {
            std::lock_guard<std::mutex> g(scan_mutex);
            scan_procs.push_back(
                { 4243, "logi_lamparray_service.AMD64.exe" });
            scan_dl = 1;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        host.PollOnce();
        CHECK(has_conflict("LampArray service (Dynamic Lighting)"));
        CHECK(has_conflict("Windows Dynamic Lighting"));
        CHECK(host.OwnershipJson()["dynamic_lighting"]
                  .get<std::string>() == "on");
        CHECK(log_has("new CONFLICT: LampArray service"));
        CHECK(log_has("new CONFLICT: Windows Dynamic Lighting"));
        /* R-10: the wire carries the engine's stable key — the
           UI picks guidance by it, never by the label. */
        {
            const nlohmann::json oj = host.OwnershipJson();
            std::vector<std::string> keys;
            for(const nlohmann::json& c : oj["conflicts"])
            {
                keys.push_back(c.value("key", std::string()));
            }
            CHECK(std::find(keys.begin(), keys.end(), "lamparray")
                  != keys.end());
            CHECK(std::find(keys.begin(), keys.end(),
                            "dynamic_lighting") != keys.end());
        }
        CHECK(own_count() == 1);   /* two writers, ONE scan */
        {
            std::lock_guard<std::mutex> g(ev_mutex);
            CHECK(!own_events.empty() &&
                  own_events.back()["conflicts"].size() == 2);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        host.PollOnce();                  /* same writers again */
        CHECK(own_count() == 1);

        {
            std::lock_guard<std::mutex> g(scan_mutex);
            scan_procs = { { 4242, "Steam.exe" } };
            scan_dl    = 0;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        host.PollOnce();
        CHECK(!has_conflict("LampArray service (Dynamic Lighting)"));
        CHECK(!has_conflict("Windows Dynamic Lighting"));
        CHECK(log_has("cleared: LampArray service"));
        CHECK(own_count() == 2);   /* the clear is one more */
        {
            std::lock_guard<std::mutex> g(ev_mutex);
            CHECK(own_events.size() == 2 &&
                  own_events.back()["conflicts"].empty());
        }
        host.GetService()->SetEventSink(nullptr);
    }
    sdk::OpenRgbClient* client = host.GetSdk();
    CHECK(client != nullptr);
    CHECK(client->State() == ConnectionState::Connected);
    CHECK(client->ProtocolVersion() <= 4);
    CHECK(client->Snapshots().size() == 2);
    /* Handshake + enumeration reached the wire: SETCLIENTNAME
       lands in the recorded event stream. */
    CHECK(server.CountEvents(sdk::kPktSetClientName) >= 1);

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
    while(server.CountEvents(sdk::kPktUpdateLEDs, 0) == 0 &&
          std::chrono::steady_clock::now() < deadline)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    CHECK(server.CountEvents(sdk::kPktUpdateLEDs, 0) >= 1);
    CHECK(host.FramesProduced() > 0);

    /* The wireless group's write is ONE complete-controller
       UPDATELEDS carrying all 120 LEDs — never three zone
       writes (plan section 4). */
    {
        const std::vector<sdk_test::ServerEvent> snap =
            server.SnapshotEvents();
        size_t last_body_leds = 0;
        for(const sdk_test::ServerEvent& e : snap)
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
    CHECK(server.CountEvents(sdk::kPktUpdateLEDs, 1) >= 1);

    /* Custom-mode upkeep ran on bound controllers
       (SETCUSTOMMODE — live output requires direct mode). */
    CHECK(server.CountEvents(sdk::kPktSetCustomMode) >= 1);

    /* Zone-size reconciliation: the scene declares 8 LEDs on a
       zone the backend enumerated at 0 — the owner loop must
       have sent RESIZEZONE(dev 1, zone 0 -> 8), and the
       re-enumerated snapshot must report the grown zone. The
       UPDATELEDS-on-dev-1 check above is the "frames flowed"
       half of the proof — a 0-LED zone emits nothing. */
    {
        size_t resize_hits = 0;
        for(const sdk_test::ServerEvent& e : server.SnapshotEvents())
        {
            if(e.pkt_id == sdk::kPktResizeZone && e.dev_id == 1 &&
               e.body.size() >= 8)
            {
                int32_t zi = 0, ns = 0;
                memcpy(&zi, e.body.data(), 4);
                memcpy(&ns, e.body.data() + 4, 4);
                if(zi == 0 && ns == 8) { ++resize_hits; }
            }
        }
        CHECK(resize_hits >= 1);
        /* Snapshots come back key-ordered, not enumeration
           order — find the wired controller by name. */
        bool healed = false;
        for(const DeviceSnapshot& s : client->Snapshots())
        {
            if(s.name.find("X870E") != std::string::npos &&
               s.zones.size() == 1 && s.zones[0].led_count == 8)
            {
                healed = true;
            }
        }
        CHECK(healed);
    }

    /*---- shutdown ordering -----------------------------------*/
    run.store(false);
    owner.join();
    host.RequestStop();
    const OrderlyShutdown::Result& sd = host.Stop();

    static const char* want_order[] = {
        "power-monitor", "ipc-listeners", "ipc-server",
        /* the calibration worker joins before anything its
           claim release or change hook reaches destructs */
        "calibration",
        /* studio-import Task 3: the workspace job worker joins
           before scene-worker/service teardown so an in-flight
           commit's apply_scene seam can never race a
           destructing IPC service. */
        "workspace-jobs",
        /* I-6: the background last-scene writer drains once
           no apply can arrive any more. */
        "scene-snapshot",
        "scene-worker", "reactive-inputs",
        /* E-12b: the exit output policy writes while the
           lanes and the transport are still up. */
        "output-policy",
        "delivery-pipeline", "sdk-transport",
    };
    CHECK(sd.steps.size() == 11);
    if(sd.steps.size() == 11)
    {
        for(size_t i = 0; i < 11; ++i)
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

    /*---- last-scene persistence + restore --------------------
       Host A's --scene apply ran through the shared apply
       path, which fired persist_scene — the document sits
       beside engine-state.json as last-scene.json. A fresh
       host with NO --scene over the same state dir restores
       it; --no-scene-restore suppresses the restore. */
    {
        const std::string snap_file =
            (std::filesystem::path(state_file).parent_path() /
             "last-scene.json").string();
        std::error_code ec2;
        CHECK(std::filesystem::exists(snap_file, ec2));

        auto make_opts = [&](const char* inst,
                             bool restore)
        {
            EngineHostOptions o;
            o.sdk_enabled       = true;
            o.transport_factory =
                [&server]() -> std::unique_ptr<sdk::IStreamTransport>
                { return std::make_unique<sdk_test::FakeStream>(
                      server); };
            o.ipc_enabled     = false;
            o.power_monitor   = false;
            o.scan_owners     = false;
            o.single_instance = true;
            o.instance_name   = inst;
            o.state_path      = state_file;
            o.scene_restore   = restore;
            o.log             = [](const std::string&) {};
            return o;
        };

        /* Host B: no --scene -> the persisted scene applies. */
        EngineHost host2(make_opts(
            ("Local\\openrgb2-engine-e2e-test-2-" + std::to_string(_getpid())).c_str(), true));
        CHECK(host2.Start(err));
        CHECK(host2.GetService() != nullptr);
        std::shared_ptr<const studio::SceneDocument> doc =
            host2.GetService()->Scene();
        CHECK(doc != nullptr);
        if(doc)
        {
            CHECK(doc->name == "e2e-scene");
            CHECK(doc->objects.size() == 4);
            CHECK(doc->bindings.size() == 4);
        }
        host2.RequestStop();
        host2.Stop();

        /* Host C: --no-scene-restore -> same state dir starts
           idle, snapshot untouched. */
        EngineHost host3(make_opts(
            ("Local\\openrgb2-engine-e2e-test-3-" + std::to_string(_getpid())).c_str(), false));
        CHECK(host3.Start(err));
        CHECK(host3.GetService() != nullptr);
        CHECK(host3.GetService()->Scene() == nullptr);
        host3.RequestStop();
        host3.Stop();

        /* R-1: the tray's output control through the real host
           wiring — master + blackout persist across a restart,
           hold does not (a restart resumes). */
        {
            EngineHost host4(make_opts(
                ("Local\\openrgb2-engine-e2e-test-4-" +
                 std::to_string(_getpid())).c_str(), true));
            CHECK(host4.Start(err));
            ipc::EngineService* svc = host4.GetService();
            CHECK(svc != nullptr);
            if(svc)
            {
                using ipc::MsgType;
                ipc::IpcReply r = svc->Dispatch(MsgType::OutputCommand,
                    nlohmann::json::object({{ "verb", "set_master" },
                                            { "value", 0.4 }}));
                CHECK(r.code == ipc::ErrCode::Ok);
                r = svc->Dispatch(MsgType::OutputCommand,
                    nlohmann::json::object({{ "verb", "set_blackout" },
                                            { "on", true }}));
                CHECK(r.code == ipc::ErrCode::Ok);
                r = svc->Dispatch(MsgType::OutputCommand,
                    nlohmann::json::object({{ "verb", "set_hold" },
                                            { "on", true }}));
                CHECK(r.code == ipc::ErrCode::Ok);
                CHECK(host4.GetEngine().Worker().Held());
                CHECK(host4.GetOutput().EffectiveScale() == 0.0f);
                /* E-12b x R-1: the production wiring of set_policy
                   reaches EngineHost::SetOutputPolicy */
                r = svc->Dispatch(MsgType::OutputCommand,
                    nlohmann::json::object({{ "verb", "set_policy" },
                                            { "on_idle", "black" }}));
                CHECK(r.code == ipc::ErrCode::Ok);
                CHECK(host4.GetOutputPolicy().on_idle ==
                      OutputAction::Black);
                CHECK(host4.GetOutputPolicy().on_exit ==
                      OutputAction::Hold);
                /* back to the default so later restarts in this
                   suite see hold/hold */
                r = svc->Dispatch(MsgType::OutputCommand,
                    nlohmann::json::object({{ "verb", "set_policy" }}));
                CHECK(r.code == ipc::ErrCode::Ok);
            }
            host4.RequestStop();
            host4.Stop();
        }
        {
            EngineHost host5(make_opts(
                ("Local\\openrgb2-engine-e2e-test-5-" +
                 std::to_string(_getpid())).c_str(), true));
            CHECK(host5.Start(err));
            const OutputState os = host5.GetOutput().State();
            CHECK(os.master == 0.4f);
            CHECK(os.blackout);
            CHECK(!os.hold);
            CHECK(!host5.GetEngine().Worker().Held());
            /* Releasing Off restores the persisted master. */
            ipc::EngineService* svc = host5.GetService();
            if(svc)
            {
                svc->Dispatch(ipc::MsgType::OutputCommand,
                    nlohmann::json::object({{ "verb", "set_blackout" },
                                            { "on", false }}));
            }
            CHECK(host5.GetOutput().EffectiveScale() == 0.4f);
            host5.RequestStop();
            host5.Stop();
        }
    }

    TestPartialStartTeardown(tmp);
    TestPriorModeRecording(tmp);
    TestIdlePolicy(tmp);
    TestBackendSupervisor(tmp);

    std::printf("%d checks, %d failures\n", checks, failures);
    {
        std::lock_guard<std::mutex> g(log_mutex);
        for(const std::string& l : log_lines)
        {
            std::printf("  | %s\n", l.c_str());
        }
    }
    return failures == 0 ? 0 : 1;
}
