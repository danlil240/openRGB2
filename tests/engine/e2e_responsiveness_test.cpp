/*---------------------------------------------------------*\
|| e2e_responsiveness_test.cpp                               ||
||                                                           ||
||   Plan section 6 acceptance row:                          ||
||     "UI responsiveness — p95 local control response      ||
||      below 100 ms during full output"                     ||
||   (deferred out of Milestone 4 task 4.8; this is the     ||
||   measurement that row was waiting for).                 ||
||                                                           ||
||   WHAT IS MEASURED — client-side wall time of one IPC   ||
||   request -> matching reply, over REAL named pipes,      ||
||   against the REAL EngineHost production composition     ||
||   (ownership -> SDK attach -> bind -> 60 Hz scene       ||
||   worker -> delivery lanes -> command/diagnostics       ||
||   listeners), while frames stream at full rate to the   ||
||   in-process fake SDK backend (tests/sdk). The client   ||
||   holds a SUBSCRIBE "all" session like the real UI, so  ||
||   request/reply round-trips interleave with the push    ||
||   channel (initial-state burst, bounded-rate            ||
||   frame_status, scene/playback change events).          ||
||                                                           ||
||   The verb mix is the UI's own surface: PING (floor),  ||
||   GET_STATE / GET_PLAYBACK / GET_DELIVERY_STATS /       ||
||   GET_SCENE (the reads the pages poll), SET_PLAYBACK_    ||
||   MODE (mode-selector control — the idempotent accept   ||
||   path, same dispatch a click takes), and SET_SCENE     ||
||   (Save — the heaviest mutation verb, full parse ->     ||
||   resolve -> worker submit -> re-bind -> scene event).  ||
||                                                           ||
||   WHAT IS NOT MEASURED — QML input sampling, GUI-thread  ||
||   marshalling and paint time (that half is bounded by   ||
||   the ui_hardening flood probe), and backend/USB side   ||
||   latency (irrelevant to UI response: frame writes run  ||
||   on delivery lanes, never inside a request reply).     ||
||   Reported in docs/benchmarks/ui-responsiveness.md.      ||
||                                                           ||
||   SPDX-License-Identifier: GPL-2.0-or-later               ||
\*---------------------------------------------------------*/

#include "EngineHost.h"
#include "IpcProtocol.h"
#include "NamedPipe.h"
#include "SdkWire.h"
#include "../sdk/fake_sdk_server.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace openrgb2;
using nlohmann::json;

#define CHECK(cond) do { if(!(cond)) { \
    fprintf(stderr, "FAIL %d: %s\n", __LINE__, #cond); \
    return 1; } } while(0)
#define CHECKV(cond, ...) do { if(!(cond)) { \
    fprintf(stderr, "FAIL %d: %s\n", __LINE__, #cond); \
    fprintf(stderr, "  " __VA_ARGS__); fputc('\n', stderr); \
    return 1; } } while(0)

/*---------------------------------------------------------*\
|| Fake SDK fixture (same shape as the other e2e suites)  ||
\*---------------------------------------------------------*/
static const char* kMac = "FA:KE:60:00:00:01";

static sdk_test::FakeController WirelessGroup()
{
    sdk_test::FakeController c;
    c.type     = 5;
    c.name     = "UNI FAN SL V3 Wireless RESP";
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
    c.name     = "X870E AORUS ELITE WIFI7 ICE RESP";
    c.vendor   = "GIGABYTE";
    c.serial   = "MB-RESP-1";
    c.location = "smbus:0-motherboard-resp";
    sdk_test::FakeMode m;
    m.name = "Direct";
    c.modes.push_back(m);
    c.zones = { { "ARGB_V2_1", 0, 8, 8, 8 } };
    c.colors.assign(8, 0x00010101u);
    return c;
}

static json EmitterArray(int first, int count)
{
    json e = json::array();
    for(int i = 0; i < count; ++i)
    {
        e.push_back({ { "pos", { {"x",0},{"y",0},{"z",0} } },
                      { "group", "" },
                      { "address", first + i } });
    }
    return e;
}

/* A moving world-space wave — every 60 Hz tick produces
   CHANGING colors on all 128 LEDs, so the delivery lanes
   write to the backend every frame (no unchanged-frame
   suppression collapsing the load). */
static json LiveSceneDoc()
{
    json bindings = json::array();
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
        { "id",              "b-argb"    },
        { "controller_name", "X870E"     },
        { "zone_name",       "ARGB_V2_1" },
        { "zone_leds",       8           },
    });

    json objects = json::array();
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

    return {
        { "version", 2 },
        { "name",    "resp-live" },
        { "brightness", 1.0 },
        { "bindings", bindings },
        { "objects",  objects },
        { "effect", {
            { "preset",    "" },
            { "seed",      1 },
            { "speed",     1.0 },
            { "intensity", 1.0 },
            { "playing",   true },
            { "layers", json::array({ {
                { "primitive", "wave" },
                { "space",     "world" },
                { "speed",     0.4 },
                { "scale",     0.5 },
                { "palette", json::array({
                    { { "pos", 0.0 }, { "color", "#FF0000" } },
                    { { "pos", 0.5 }, { "color", "#0000FF" } },
                }) },
            } }) },
        } },
    };
}

/*---------------------------------------------------------*\
|| Real named-pipe client (the openrgb2-ctl/UI session     ||
|| surface): HELLO -> SUBSCRIBE all -> request/reply with ||
|| EVENT pushes skipped while awaiting the matching reply ||
\*---------------------------------------------------------*/
struct PipeClient
{
    std::unique_ptr<ipc::IIpcChannel>   ch;
    std::unique_ptr<ipc::MessageReader> reader;
    uint32_t                            req_id = 0;
    uint64_t                            events_seen = 0;

    bool Connect(const std::string& pipe)
    {
        std::string err;
        ch = ipc::ConnectNamedPipeChannel(pipe, 5000, err);
        if(!ch)
        {
            fprintf(stderr, "pipe connect to %s failed: %s\n",
                    pipe.c_str(), err.c_str());
            return false;
        }
        reader = std::make_unique<ipc::MessageReader>(*ch);
        return SendRaw(ipc::MsgType::Hello, 0, json::object({
                { "client_name", "resp-probe" },
                { "min_version", 1 },
                { "max_version", 1 } })) &&
               AwaitType(0, ipc::MsgType::Welcome, 5000);
    }

    bool SendRaw(ipc::MsgType t, uint32_t id, const json& body)
    {
        std::string err;
        const std::vector<uint8_t> bytes =
            ipc::EncodeMessage(t, id, body);
        if(bytes.empty())
        {
            return false;
        }
        return ch->SendAll(bytes.data(), bytes.size(), 5000, err)
               == ipc::IpcIoResult::Ok;
    }

    /* Reads until a message carrying `want_id` arrives;
       EVENT pushes (request_id 0) are counted and skipped —
       the same drain discipline openrgb2-ctl uses. */
    bool AwaitType(uint32_t want_id, ipc::MsgType want_type,
                   uint32_t deadline_ms)
    {
        ipc::Message m;
        for(;;)
        {
            std::string detail;
            if(reader->Read(m, deadline_ms, detail)
               != ipc::ReadResult::Ok)
            {
                fprintf(stderr, "read failed awaiting %s: %s\n",
                        ipc::MsgTypeName(want_type), detail.c_str());
                return false;
            }
            if(m.type == ipc::MsgType::Event &&
               m.request_id == 0)
            {
                ++events_seen;
                continue;
            }
            if(m.request_id != want_id)
            {
                continue;   /* stale/other correlator — skip */
            }
            if(want_type != ipc::MsgType::Event &&
               m.type != want_type)
            {
                fprintf(stderr, "reply type mismatch: want %s "
                        "got %s\n", ipc::MsgTypeName(want_type),
                        ipc::MsgTypeName(m.type));
                return false;
            }
            return true;
        }
    }

    /* One request -> matching reply. Returns round-trip
       latency in milliseconds, or -1 on failure. */
    double Request(ipc::MsgType t, ipc::MsgType expect,
                   const json& body, uint32_t deadline_ms)
    {
        const uint32_t id = ++req_id;
        const auto t0 = std::chrono::steady_clock::now();
        if(!SendRaw(t, id, body) ||
           !AwaitType(id, expect, deadline_ms))
        {
            return -1.0;
        }
        return std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - t0).count();
    }
};

/*---------------------------------------------------------*\
|| Percentiles (nearest-rank over sorted samples)         ||
\*---------------------------------------------------------*/
struct Stats
{
    size_t n     = 0;
    double min   = 0;
    double p50   = 0;
    double p95   = 0;
    double p99   = 0;
    double max   = 0;
    double mean  = 0;
};

static Stats Compute(std::vector<double> s)
{
    Stats r;
    if(s.empty())
    {
        return r;
    }
    std::sort(s.begin(), s.end());
    const auto pct = [&s](double p)
    {
        size_t idx = (size_t)((p / 100.0) * s.size() + 0.999999);
        if(idx == 0) idx = 1;
        if(idx > s.size()) idx = s.size();
        return s[idx - 1];
    };
    r.n    = s.size();
    r.min  = s.front();
    r.max  = s.back();
    r.p50  = pct(50);
    r.p95  = pct(95);
    r.p99  = pct(99);
    double sum = 0;
    for(double v : s) sum += v;
    r.mean = sum / (double)s.size();
    return r;
}

static void PrintRow(const char* name, const Stats& s)
{
    std::printf("  %-16s %4zu %8.3f %8.3f %8.3f %8.3f %8.3f\n",
                name, s.n, s.min, s.p50, s.p95, s.p99, s.max);
}

/*---------------------------------------------------------*\
|| Main                                                   ||
\*---------------------------------------------------------*/
int main(int argc, char** argv)
{
    /* Optional: write the raw result JSON here (manual bench
       runs record the numbers; the CTest run just asserts). */
    std::string out_json = argc > 1 ? argv[1] : "";

    std::error_code ec;
    const std::filesystem::path tmpdir =
        std::filesystem::temp_directory_path(ec) /
        "openrgb2-resp-state";
    std::filesystem::create_directories(tmpdir, ec);
    std::filesystem::remove(tmpdir / "engine-state.json", ec);
    const std::string scene_file = (tmpdir / "scene.json").string();
    {
        std::ofstream f(scene_file);
        f << LiveSceneDoc().dump();
    }

    sdk_test::FakeSdkServer server;
    server.controllers = { WirelessGroup(), Wired() };

    EngineHostOptions opts;
    opts.sdk_enabled    = true;
    opts.ipc_enabled    = true;
    const std::string cmd_pipe =
        "\\\\.\\pipe\\openrgb2-e2e-resp";
    opts.command_pipe_name     = cmd_pipe;
    opts.diagnostics_pipe_name =
        "\\\\.\\pipe\\openrgb2-e2e-resp-diag";
    opts.power_monitor   = false;   /* no OS events needed —
                                       the gate is covered by
                                       e2e_lifecycle_test    */
    opts.reactive_inputs = false;   /* same                  */
    opts.scan_owners     = false;   /* startup-only check;
                                       steady-state latency
                                       is unaffected        */
    opts.single_instance = true;
    opts.instance_name   = "Local\\openrgb2-engine-resp-test";
    opts.scene_path      = scene_file;
    opts.state_path      = (tmpdir / "engine-state.json").string();
    opts.backend_config_dir = "";
    opts.sdk_port        = 64997;   /* unused — the fake
                                       stream injects the
                                       transport; the port
                                       only feeds log text  */
    opts.transport_factory =
        [&server]() -> std::unique_ptr<sdk::IStreamTransport>
        { return std::make_unique<sdk_test::FakeStream>(server); };
    std::mutex              log_mutex;
    std::vector<std::string> log_lines;
    opts.log = [&log_lines, &log_mutex](const std::string& l)
    {
        std::lock_guard<std::mutex> g(log_mutex);
        log_lines.push_back(l);
    };

    EngineHost host(std::move(opts));
    std::string err;
    CHECKV(host.Start(err), "Start: %s", err.c_str());
    CHECK(host.GetSdk() != nullptr &&
          host.GetSdk()->State() == ConnectionState::Connected);

    /* Owner loop on a thread at the PRODUCTION cadence
       (Run() = PollOnce every ~500 ms) — IPC event diffing,
       SDK poll, topology refresh, playback maintenance. */
    std::atomic<bool> run{ true };
    std::thread owner([&]
    {
        while(run.load())
        {
            host.PollOnce();
            std::this_thread::sleep_for(
                std::chrono::milliseconds(500));
        }
    });
    /* Early-return cleanup: any failing CHECK below must
       still join the owner thread and stop the host — a
       joinable std::thread at main() exit is std::terminate. */
    struct RunGuard
    {
        EngineHost&        host;
        std::atomic<bool>& run;
        std::thread&       owner;
        ~RunGuard()
        {
            run.store(false);
            if(owner.joinable()) owner.join();
            host.RequestStop();
            host.Stop();
        }
    } guard{ host, run, owner };

    /*---- full output confirmed before measuring ----------*/
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds(15);
    while((server.CountEvents(sdk::kPktUpdateLEDs, 0) < 3 ||
           server.CountEvents(sdk::kPktUpdateLEDs, 1) < 3) &&
          std::chrono::steady_clock::now() < deadline)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    CHECKV(server.CountEvents(sdk::kPktUpdateLEDs, 0) >= 3,
           "wireless lane never produced UPDATELEDS");
    CHECKV(server.CountEvents(sdk::kPktUpdateLEDs, 1) >= 3,
           "wired lane never produced UPDATELEDS");

    /*---- UI session: HELLO + SUBSCRIBE all ---------------*/
    PipeClient client;
    CHECK(client.Connect(cmd_pipe));
    {
        const uint32_t id = ++client.req_id;
        CHECK(client.SendRaw(ipc::MsgType::Subscribe, id,
                             json::object({{ "events", "all" }})));
        CHECK(client.AwaitType(id, ipc::MsgType::Ok, 5000));
    }
    /* Drain the initial-state burst, then warm up the
       session (pipe buffers, codec paths). */
    {
        ipc::Message m;
        std::string  detail;
        while(client.reader->Read(m, 300, detail)
              == ipc::ReadResult::Ok)
        {
            if(m.type == ipc::MsgType::Event) ++client.events_seen;
        }
    }
    for(int i = 0; i < 10; ++i)
    {
        CHECK(client.Request(ipc::MsgType::Ping, ipc::MsgType::Pong,
                             json::object(), 5000) >= 0);
    }

    /*---- measurement ------------------------------------*/
    /* The UI verb surface — round-robin so every verb is
       sampled across the whole window. */
    struct Verb { const char* name; ipc::MsgType req;
                  ipc::MsgType expect; json body; };
    const json set_playback = json::object({
        { "mode",    "live_effects" },
        { "control", "playing"      },
    });
    const json set_scene = json::object({
        { "document", LiveSceneDoc() },
    });
    const Verb verbs[] = {
        { "ping",            ipc::MsgType::Ping,
          ipc::MsgType::Pong,          json::object() },
        { "get_state",       ipc::MsgType::GetState,
          ipc::MsgType::State,         json::object() },
        { "get_playback",    ipc::MsgType::GetPlayback,
          ipc::MsgType::Playback,      json::object() },
        { "get_stats",       ipc::MsgType::GetDeliveryStats,
          ipc::MsgType::DeliveryStats, json::object() },
        { "set_playback",    ipc::MsgType::SetPlaybackMode,
          ipc::MsgType::Ok,            set_playback   },
        { "set_scene",       ipc::MsgType::SetScene,
          ipc::MsgType::Ok,            set_scene      },
    };
    constexpr size_t kVerbs   = sizeof(verbs) / sizeof(verbs[0]);
    constexpr int    kSamples = 300;

    std::vector<double> lat_all;
    std::vector<double> lat_verb[kVerbs];
    lat_all.reserve(kSamples);
    for(auto& v : lat_verb) v.reserve(kSamples / kVerbs + 1);

    const uint64_t frames0  = host.FramesProduced();
    const size_t   upd0     = server.CountEvents(sdk::kPktUpdateLEDs);
    const uint64_t events0  = client.events_seen;
    const auto win0 = std::chrono::steady_clock::now();

    int failed_at = -1;
    for(int i = 0; i < kSamples; ++i)
    {
        const Verb& v = verbs[i % kVerbs];
        const double ms = client.Request(v.req, v.expect, v.body,
                                         5000);
        if(ms < 0)
        {
            failed_at = i;
            break;
        }
        lat_all.push_back(ms);
        lat_verb[i % kVerbs].push_back(ms);
        /* ~5 ms spacing — a clicking user does not fire a
           back-to-back request storm; keeps the window long
           enough for frame_status pushes to interleave. */
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    const double window_s =
        std::chrono::duration<double>(
            std::chrono::steady_clock::now() - win0).count();
    const uint64_t frames_d =
        host.FramesProduced() - frames0;
    const size_t   upd_d    =
        server.CountEvents(sdk::kPktUpdateLEDs) - upd0;
    const uint64_t events_d = client.events_seen - events0;
    const double frames_hz = frames_d / window_s;
    const double writes_s  = (double)upd_d / window_s;

    /*---- report -----------------------------------------*/
    std::printf("\n== UI responsiveness under full output ==\n");
    std::printf("window %.2f s | scene frames %llu (%.1f Hz) "
                "| UPDATELEDS %zu (%.1f/s) | events skipped %llu\n",
                window_s, (unsigned long long)frames_d, frames_hz,
                upd_d, writes_s,
                (unsigned long long)events_d);
    std::printf("samples %d over %zu verbs — client-side "
                "request->reply wall time over real named pipes\n",
                kSamples, kVerbs);
    std::printf("  %-16s %4s %8s %8s %8s %8s %8s\n",
                "verb", "n", "min", "p50", "p95", "p99", "max");
    for(size_t i = 0; i < kVerbs; ++i)
    {
        PrintRow(verbs[i].name, Compute(lat_verb[i]));
    }
    const Stats all = Compute(lat_all);
    PrintRow("ALL", all);
    std::printf("criterion: p95 < 100 ms -> overall p95 %.3f ms "
                "=> %s\n", all.p95,
                all.p95 < 100.0 ? "PASS" : "FAIL");

    if(!out_json.empty())
    {
        json jverbs = json::object();
        for(size_t i = 0; i < kVerbs; ++i)
        {
            const Stats s = Compute(lat_verb[i]);
            jverbs[verbs[i].name] = {
                { "n", s.n }, { "min", s.min }, { "p50", s.p50 },
                { "p95", s.p95 }, { "p99", s.p99 },
                { "max", s.max }, { "mean", s.mean },
            };
        }
        const json doc = {
            { "measurement",
              "ipc request->reply wall time, real named pipes, "
              "subscribed client, engine at full 60 Hz output "
              "to in-process fake SDK backend" },
            { "window_s",        window_s  },
            { "frames_produced", frames_d  },
            { "frames_hz",       frames_hz },
            { "updateleds",      upd_d     },
            { "writes_per_s",    writes_s  },
            { "events_skipped",  events_d  },
            { "samples",         kSamples  },
            { "verbs",           jverbs    },
            { "all", { { "n", all.n }, { "min", all.min },
                       { "p50", all.p50 }, { "p95", all.p95 },
                       { "p99", all.p99 }, { "max", all.max },
                       { "mean", all.mean } } },
            { "criterion", "p95 < 100 ms" },
            { "pass", all.p95 < 100.0 },
        };
        std::ofstream f(out_json);
        f << doc.dump(2) << "\n";
    }

    /*---- ordered shutdown --------------------------------*/
    run.store(false);
    owner.join();
    host.RequestStop();
    const OrderlyShutdown::Result& sd = host.Stop();
    CHECK(sd.all_within_bound);

    /* The acceptance assertions: every sampled request got
       its reply, the engine kept producing full output for
       the whole window, AND the measured p95 met the
       section-6 bound. */
    CHECKV(failed_at < 0,
           "request %d failed mid-measurement", failed_at);
    CHECKV(frames_hz >= 30.0,
           "scene clock sagged below half rate (%.1f Hz) — "
           "the window was not 'full output'", frames_hz);
    CHECKV(all.p95 < 100.0,
           "ACCEPTANCE FAIL: p95 %.3f ms >= 100 ms", all.p95);
    return 0;
}
