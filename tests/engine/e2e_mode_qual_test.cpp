/*---------------------------------------------------------*\
|||| e2e_mode_qual_test.cpp                                  ||
||||                                                           ||
||||   Task 5.2 automated mode qualification: drives the    ||
||||   REAL EngineHost production composition (SceneWorker  ||
||||   -> frame queue -> DeviceMapper -> DeliveryPipeline   ||
||||   lane threads -> OpenRgbClient) against the in-proc   ||
||||   fake SDK server, for the simulable rows of the       ||
||||   qualification matrix:                                ||
||||                                                           ||
||||   - rapid mode/parameter changes: a two-producer       ||
||||     request storm must never wedge — every request     ||
||||     returns a definitive accepted/rejected answer,     ||
||||     pending always converges to applied or failed,     ||
||||     and the final state matches the last accepted      ||
||||     request (no lost/silent requests)                  ||
||||   - disconnected device during upload: removing the    ||
||||     wireless group from the backend's enumeration      ||
||||     while an apply is in flight must surface an        ||
||||     honest failure (upload=failed + reason), never a   ||
||||     silent partial success                             ||
||||   - recovery: re-adding the controller + reconnect     ||
||||     restores the still-selected playback state         ||
||||   - lane ownership: while the uploaded loop owns the   ||
||||     wireless group, live frames for it are rejected    ||
||||     at the lane and wired devices keep receiving the   ||
||||     baked preset output                                ||
||||   - pause/stop transport control: Direct-mode hold /   ||
||||     black writes reach the wire                        ||
||||                                                           ||
||||   Physical-topology runs and clock-drift evidence are  ||
||||   documented separately in docs/benchmarks/mode-qual.  ||
||||                                                           ||
||||   SPDX-License-Identifier: GPL-2.0-or-later               ||
||\*---------------------------------------------------------*/

#include "EngineHost.h"
#include "EngineService.h"
#include "SdkWire.h"
#include "../sdk/fake_sdk_server.h"

#include "scene/SceneJson.h"
#include "scene/SceneTypes.h"

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <system_error>
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
|||| Fake SDK fixture (same shape as the e2e test)          ||
\*---------------------------------------------------------*/

static const char* kMac = "FA:KE:52:00:00:01";

static sdk_test::FakeController WirelessGroup()
{
    sdk_test::FakeController c;
    c.type     = 5;
    c.name     = "UNI FAN SL V3 Wireless MODEQUAL";
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
    c.name     = "X870E AORUS ELITE WIFI7 ICE MODEQUAL";
    c.vendor   = "GIGABYTE";
    c.serial   = "MB-MODEQUAL-1";
    c.location = "smbus:0-motherboard-modequal";
    sdk_test::FakeMode m;
    m.name = "Direct";
    c.modes.push_back(m);
    c.zones = { { "ARGB_V2_1", 0, 8, 8, 8 } };
    c.colors.assign(8, 0x00010101u);
    return c;
}

/*---------------------------------------------------------*\
|||| Scene documents (version-2 schema, e2e shape)          ||
\*---------------------------------------------------------*/

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

static json Bindings()
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
    return bindings;
}

static json Objects()
{
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
    return objects;
}

/* Ordinary live scene: one inline wave layer (animation
   playback unavailable for layered scenes). */
static json LiveSceneDoc()
{
    return {
        { "version", 2 },
        { "name",    "modequal-live" },
        { "brightness", 1.0 },
        { "bindings", Bindings() },
        { "objects",  Objects() },
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

/* Playback scene: names the Rainbow Wave preset with no
   inline layers — the only scene shape eligible for
   Animation playback. */
static json PlaybackSceneDoc()
{
    return {
        { "version", 2 },
        { "name",    "modequal-playback" },
        { "brightness", 1.0 },
        { "bindings", Bindings() },
        { "objects",  Objects() },
        { "effect", {
            { "preset",    "rainbow_wave" },
            { "seed",      1 },
            { "speed",     1.0 },
            { "intensity", 1.0 },
            { "playing",   true },
            { "layers",    json::array() },
        } },
    };
}

/*---------------------------------------------------------*\
|||| Helpers                                                ||
\*---------------------------------------------------------*/

static bool ApplyScene(ipc::EngineService* svc, const json& j,
                       std::string& err)
{
    auto doc = std::make_shared<studio::SceneDocument>();
    std::vector<std::string> errs;
    if(!studio::FromJson(j, *doc, &errs))
    {
        err = errs.empty() ? "scene parse failed" : errs.front();
        return false;
    }
    return svc->ApplySceneDocument(doc, err);
}

static void PumpFor(EngineHost& host, int ms)
{
    const auto end = std::chrono::steady_clock::now() +
                     std::chrono::milliseconds(ms);
    while(std::chrono::steady_clock::now() < end)
    {
        host.PollOnce();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}

template<class Pred>
static bool WaitFor(EngineHost& host, Pred pred, int timeout_ms)
{
    const auto end = std::chrono::steady_clock::now() +
                     std::chrono::milliseconds(timeout_ms);
    while(std::chrono::steady_clock::now() < end)
    {
        host.PollOnce();
        if(pred())
        {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    host.PollOnce();
    return pred();
}

static const DeviceSnapshot* FindByName(
    const std::vector<DeviceSnapshot>& snaps,
    const char* needle)
{
    for(const DeviceSnapshot& s : snaps)
    {
        if(s.name.find(needle) != std::string::npos)
        {
            return &s;
        }
    }
    return nullptr;
}

/*---------------------------------------------------------*\
|||| Main                                                   ||
\*---------------------------------------------------------*/

static int TestBody();
int main(int /*argc*/, char** /*argv*/)
{
    try
    {
        return TestBody();
    }
    catch(const std::exception& e)
    {
        fprintf(stderr, "UNCAUGHT EXCEPTION: %s\n", e.what());
        return 1;
    }
    catch(...)
    {
        fprintf(stderr, "UNCAUGHT UNKNOWN EXCEPTION\n");
        return 1;
    }
}

static int TestBody()
{
    std::error_code ec;
    const std::filesystem::path tmpdir =
        std::filesystem::temp_directory_path(ec) /
        "openrgb2-modequal-state";
    std::filesystem::create_directories(tmpdir, ec);
    std::filesystem::remove(tmpdir / "engine-state.json", ec);

    sdk_test::FakeSdkServer server;
    server.controllers = { WirelessGroup(), Wired() };
    sdk_test::FakeStream* stream = nullptr;

    EngineHostOptions opts;
    opts.sdk_enabled        = true;
    opts.ipc_enabled        = false;
    opts.power_monitor      = false;
    opts.reactive_inputs    = false;
    opts.scan_owners        = false;
    opts.single_instance    = true;
    opts.instance_name      = "Local\\openrgb2-engine-modequal-test";
    opts.state_path         =
        (tmpdir / "engine-state.json").generic_string();
    opts.backend_config_dir = "";
    opts.transport_factory  =
        [&server, &stream]() -> std::unique_ptr<sdk::IStreamTransport>
        {
            auto s = std::make_unique<sdk_test::FakeStream>(server);
            stream = s.get();
            return s;
        };

    EngineHost host(std::move(opts));
    std::string err;
    CHECKV(host.Start(err), "Start: %s", err.c_str());

    sdk::OpenRgbClient*   client   = host.GetSdk();
    DeliveryPipeline*     pipeline = host.GetPipeline();
    PlaybackController*   playback = host.GetPlayback();
    ipc::EngineService*   service  = host.GetService();
    CHECK(client && pipeline && playback && service);

    /*------------------------------------------------*/
    /* Phase A — capability honesty + live baseline    */
    /*------------------------------------------------*/

    /* No scene: an animation request must refuse with a
       reason (honest capability, never a silent fall). */
    {
        PlaybackController::Result r =
            playback->Request(PlaybackMode::AnimationPlayback,
                              PlaybackControl::Playing,
                              PlaybackParams{});
        CHECKV(!r.accepted, "animation without scene accepted");
        CHECKV(!r.reason.empty(), "rejection carried no reason");
    }

    /* Live scene -> live frames reach the wireless group. */
    CHECKV(ApplyScene(service, LiveSceneDoc(), err),
           "apply live scene: %s", err.c_str());
    CHECK(WaitFor(host, [&]{
        return server.CountEvents(sdk::kPktUpdateLEDs, 0) >= 1;
    }, 10000));

    CHECKV(ApplyScene(service, PlaybackSceneDoc(), err),
           "apply playback scene: %s", err.c_str());
    /* Fresh per-scene selection defaults to Live — the scene
       swap alone must not upload. */
    PumpFor(host, 300);
    CHECK(playback->Status().mode == PlaybackMode::LiveEffects);

    /*------------------------------------------------*/
    /* Phase B — animation apply + lane ownership      */
    /*------------------------------------------------*/

    PlaybackParams params;
    params.speed   = 100;
    params.reverse = false;

    {
        PlaybackController::Result r =
            playback->Request(PlaybackMode::AnimationPlayback,
                              PlaybackControl::Playing,
                              params);
        CHECKV(r.accepted, "animation request rejected: %s",
               r.reason.c_str());
        CHECKV(r.pending,
               "accepted animation request was not pending");
    }

    /* The mode write reached the wire. */
    const uint64_t first_upload =
        server.CountEvents(sdk::kPktUpdateMode, 0);
    CHECK(first_upload >= 1);

    /* Pending -> applied (backend reports the preset). */
    CHECK(WaitFor(host, [&]{
        const PlaybackStatus s = playback->Status();
        return s.mode == PlaybackMode::AnimationPlayback &&
               s.upload == UploadState::Active;
    }, 10000));
    CHECK(playback->Detail().empty());

    std::vector<DeviceSnapshot> snaps = client->Snapshots();
    const DeviceSnapshot* wireless =
        FindByName(snaps, "Wireless MODEQUAL");
    const DeviceSnapshot* wired = FindByName(snaps, "X870E");
    CHECK(wireless && wired);
    const DeviceKey wireless_key = wireless->key;
    const uint64_t  wireless_gen = wireless->topology_generation;

    /* Lane ownership: suppressed lane, live submission
       rejected with a reason, no live UPDATELEDS reaches
       the wireless group while the wired lane still gets
       baked output. */
    CHECK(pipeline->IsSuppressed(wireless_key));
    CHECK(playback->PlaybackOwnsWired());
    {
        RgbFrame f;
        f.controller          = wireless_key;
        f.topology_generation = wireless_gen;
        f.sequence            = 9000;
        f.colors.assign(120, RgbColor{ 1, 2, 3 });
        const SubmitResult sr = pipeline->SubmitLatest(f);
        CHECKV(sr.status == SubmitStatus::Rejected &&
               !sr.reason.empty(),
               "suppressed lane accepted a live frame");
    }
    {
        const uint64_t w0 =
            server.CountEvents(sdk::kPktUpdateLEDs, 0);
        const uint64_t w1 =
            server.CountEvents(sdk::kPktUpdateLEDs, 1);
        PumpFor(host, 600);
        CHECKV(server.CountEvents(sdk::kPktUpdateLEDs, 0) == w0,
               "live UPDATELEDS reached the owned group");
        CHECKV(server.CountEvents(sdk::kPktUpdateLEDs, 1) > w1,
               "wired baked frames stopped during playback");
    }

    /*------------------------------------------------*/
    /* Phase C — rapid mode/parameter storm            */
    /*------------------------------------------------*/
    /* Two concurrent request producers + the owner poll —
       the real IPC-vs-owner-thread shape. Every request
       must return a definitive answer; rejections must
       carry a reason; pending must converge. */

    struct Tally
    {
        std::mutex m;
        int        total                = 0;
        int        accepted             = 0;
        int        rejected_with_reason = 0;
        int        empty_reason         = 0;
    };
    Tally tally;

    const int32_t speeds[] = { 100, 60, 150, 90, 200, 40,
                               120, 75, 130, 10, 170, 55 };
    auto storm_body = [&](int seed, int count)
    {
        PlaybackParams p;
        for(int i = 0; i < count; ++i)
        {
            PlaybackMode    mode    = PlaybackMode::AnimationPlayback;
            PlaybackControl control = PlaybackControl::Playing;
            const int pick = (i + seed) % 6;
            if(pick == 5)
            {
                mode = PlaybackMode::LiveEffects;
            }
            else
            {
                p.speed   = speeds[(i + seed) %
                                   (sizeof(speeds)/sizeof(speeds[0]))];
                p.reverse = ((i + seed) % 3) == 0;
                p.color   = RgbColor{ (uint8_t)((i*7+seed)%255),
                                      (uint8_t)((i*5)%255), 0x10 };
            }
            const PlaybackController::Result r =
                playback->Request(mode, control, p);
            {
                std::lock_guard<std::mutex> g(tally.m);
                ++tally.total;
                if(r.accepted)
                {
                    ++tally.accepted;
                }
                else if(!r.reason.empty())
                {
                    ++tally.rejected_with_reason;
                }
                else
                {
                    ++tally.empty_reason;
                }
            }
            std::this_thread::sleep_for(
                std::chrono::milliseconds((i + seed) % 4));
        }
    };

    std::thread storm_a(storm_body, 1, 30);
    std::thread storm_b(storm_body, 6, 30);
    /* The main thread acts as the owner pump while the two
       producers race their requests — the same shape as the
       IPC thread vs the owner thread in production. */
    while(true)
    {
        int done;
        {
            std::lock_guard<std::mutex> g(tally.m);
            done = tally.total;
        }
        if(done >= 60)
        {
            break;
        }
        host.PollOnce();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    storm_a.join();
    storm_b.join();

    CHECKV(tally.total == 60,
           "storm issued %d requests, expected 60", tally.total);
    CHECKV(tally.empty_reason == 0,
           "%d rejections carried no reason", tally.empty_reason);
    CHECKV(tally.accepted + tally.rejected_with_reason ==
           tally.total,
           "request accounting mismatch");
    CHECKV(tally.accepted >= 15,
           "only %d/60 storm requests accepted",
           tally.accepted);
    fprintf(stderr,
            "storm: %d accepted, %d rejected-with-reason\n",
            tally.accepted, tally.rejected_with_reason);

    /* No wedge: pending always converges. */
    CHECK(WaitFor(host, [&]{
        return !playback->AckPending();
    }, 20000));

    /* Deterministic tail: live -> applied, then animation
       -> applied. The last accepted request rules. */
    {
        PlaybackController::Result r =
            playback->Request(PlaybackMode::LiveEffects,
                              PlaybackControl::Playing,
                              params);
        CHECKV(r.accepted || !r.reason.empty(),
               "live request gave no definitive answer");
    }
    CHECK(WaitFor(host, [&]{
        const PlaybackStatus s = playback->Status();
        return s.mode == PlaybackMode::LiveEffects &&
               s.upload == UploadState::None;
    }, 10000));
    CHECK(!pipeline->IsSuppressed(wireless_key));

    params.speed = 120;
    {
        PlaybackController::Result r =
            playback->Request(PlaybackMode::AnimationPlayback,
                              PlaybackControl::Playing,
                              params);
        CHECKV(r.accepted, "post-storm animation rejected: %s",
               r.reason.c_str());
    }
    CHECK(WaitFor(host, [&]{
        const PlaybackStatus s = playback->Status();
        return s.mode == PlaybackMode::AnimationPlayback &&
               s.upload == UploadState::Active;
    }, 10000));
    CHECKV(playback->Detail().empty(),
           "detail after clean convergence: %s",
           playback->Detail().c_str());

    /*------------------------------------------------*/
    /* Phase D — pause / stop transport control        */
    /*------------------------------------------------*/

    {
        const uint64_t leds0_before =
            server.CountEvents(sdk::kPktUpdateLEDs, 0);
        const uint64_t scm_before =
            server.CountEvents(sdk::kPktSetCustomMode, 0);
        PlaybackController::Result r =
            playback->Request(PlaybackMode::AnimationPlayback,
                              PlaybackControl::Paused,
                              params);
        CHECKV(r.accepted, "pause rejected: %s", r.reason.c_str());
        CHECK(playback->Status().control ==
              PlaybackControl::Paused);
        CHECKV(server.CountEvents(sdk::kPktSetCustomMode, 0) ==
               scm_before + 1, "pause did not select Direct");
        CHECKV(server.CountEvents(sdk::kPktUpdateLEDs, 0) ==
               leds0_before + 1,
               "pause did not land the hold frame");
    }
    {
        const uint64_t leds0_before =
            server.CountEvents(sdk::kPktUpdateLEDs, 0);
        PlaybackController::Result r =
            playback->Request(PlaybackMode::AnimationPlayback,
                              PlaybackControl::Stopped,
                              params);
        CHECKV(r.accepted, "stop rejected: %s", r.reason.c_str());
        CHECK(playback->Status().control ==
              PlaybackControl::Stopped);
        CHECKV(server.CountEvents(sdk::kPktUpdateLEDs, 0) ==
               leds0_before + 1,
               "stop did not land the black frame");
        const std::vector<sdk_test::ServerEvent> ev =
            server.SnapshotEvents();
        bool found = false;
        for(auto it = ev.rbegin(); it != ev.rend() && !found; ++it)
        {
            if(it->pkt_id != sdk::kPktUpdateLEDs || it->dev_id != 0)
            {
                continue;
            }
            found = true;
            /* body = u32 data_size + u16 count + colors */
            CHECK(it->body.size() >= 6);
            uint16_t led_count = 0;
            memcpy(&led_count, it->body.data() + 4, 2);
            CHECK(led_count == 120);
            for(size_t i = 0; i < led_count; ++i)
            {
                const uint8_t* c = &it->body[6 + i * 4];
                CHECKV(c[0] == 0 && c[1] == 0 && c[2] == 0,
                       "stop frame not black at led %u",
                       (unsigned)i);
            }
        }
        CHECKV(found, "no UPDATELEDS dev-0 event found");
    }
    {
        PlaybackController::Result r =
            playback->Request(PlaybackMode::AnimationPlayback,
                              PlaybackControl::Playing,
                              params);
        CHECKV(r.accepted, "resume rejected: %s",
               r.reason.c_str());
        CHECK(WaitFor(host, [&]{
            const PlaybackStatus s = playback->Status();
            return s.mode == PlaybackMode::AnimationPlayback &&
                   s.upload == UploadState::Active &&
                   s.control == PlaybackControl::Playing;
        }, 10000));
    }

    /*------------------------------------------------*/
    /* Phase E — disconnect during upload + recovery   */
    /*------------------------------------------------*/

    /* Return to live first so the disconnect lands during
       an uploaded-playback APPLY, not just a parameter
       revision. */
    {
        PlaybackController::Result r =
            playback->Request(PlaybackMode::LiveEffects,
                              PlaybackControl::Playing,
                              params);
        CHECKV(r.accepted, "live before disconnect rejected: %s",
               r.reason.c_str());
        CHECK(WaitFor(host, [&]{
            const PlaybackStatus s = playback->Status();
            return s.mode == PlaybackMode::LiveEffects &&
                   s.upload == UploadState::None;
        }, 10000));
    }

    params.speed = 55;
    const uint64_t uploads_before_disconnect =
        server.CountEvents(sdk::kPktUpdateMode, 0);
    {
        PlaybackController::Result r =
            playback->Request(PlaybackMode::AnimationPlayback,
                              PlaybackControl::Playing,
                              params);
        CHECKV(r.accepted,
               "pre-disconnect animation rejected: %s",
               r.reason.c_str());
        CHECKV(r.pending,
               "apply completed before the disconnect");
    }
    /* The apply is genuinely in flight — UPDATEMODE for the
       group reached the wire before the controller vanishes
       from enumeration. */
    CHECKV(server.CountEvents(sdk::kPktUpdateMode, 0) ==
           uploads_before_disconnect + 1,
           "the in-flight apply never reached the wire");

    /* The group disappears: the next device fetch either
       returns the wrong controller (structural change ->
       re-enumerate) or fails — either path must surface. */
    server.RemoveController(0);
    CHECK(WaitFor(host, [&]{
        return playback->Status().upload == UploadState::Failed;
    }, 10000));
    {
        CHECKV(!playback->Detail().empty(),
               "failed upload reported no reason");
        /* Honest surface: the first apply never acked, so the
           APPLIED mode remains Live — never a silent partial
           success. */
        const PlaybackStatus s = playback->Status();
        CHECKV(s.mode == PlaybackMode::LiveEffects,
               "status claimed animation without confirmation");
    }
    CHECK(playback->RequestedMode() ==
          PlaybackMode::AnimationPlayback);

    /* The engine re-enumerated: the group is gone from the
       snapshots, the wired controller remains. */
    CHECK(WaitFor(host, [&]{
        const std::vector<DeviceSnapshot> sn = client->Snapshots();
        return sn.size() == 1 &&
               FindByName(sn, "X870E") != nullptr;
    }, 10000));

    /* A live frame for the vanished group is still rejected
       honestly (unknown controller), never silently dropped
       or claimed delivered. */
    {
        RgbFrame f;
        f.controller          = wireless_key;
        f.topology_generation = wireless_gen;
        f.sequence            = 9100;
        f.colors.assign(120, RgbColor{ 5, 5, 5 });
        const SubmitResult sr = pipeline->SubmitLatest(f);
        CHECKV(sr.status == SubmitStatus::Rejected &&
               !sr.reason.empty(),
               "frame for vanished group was not rejected");
    }

    /* Recovery: the group re-appears at a NEW positional
       index (wired controller now at 0) and the transport
       cycles. Identity must re-resolve by DeviceKey and the
       still-selected animation state must restore. */
    server.AddController(WirelessGroup());
    stream->SetPeerClosed(true);

    CHECK(WaitFor(host, [&]{
        return client->State() == ConnectionState::Connected &&
               client->Snapshots().size() == 2;
    }, 15000));

    CHECK(WaitFor(host, [&]{
        const PlaybackStatus s = playback->Status();
        return s.mode == PlaybackMode::AnimationPlayback &&
               s.upload == UploadState::Active;
    }, 15000));
    CHECKV(playback->Detail().empty(),
           "restored playback still reports: %s",
           playback->Detail().c_str());
    /* The restore issued a fresh apply — not assumed state. */
    CHECKV(server.CountEvents(sdk::kPktUpdateMode) >
           uploads_before_disconnect + 1,
           "no restore UPDATEMODE after reconnect");

    const std::vector<DeviceSnapshot> after = client->Snapshots();
    const DeviceSnapshot* wireless2 =
        FindByName(after, "Wireless MODEQUAL");
    CHECK(wireless2);
    CHECKV(wireless2->key == wireless_key,
           "wireless identity shifted across reconnect");
    CHECK(pipeline->IsSuppressed(wireless2->key));

    /* Wired lane is alive again (baked frames resume on the
       wired controller's CURRENT positional index). */
    {
        const uint32_t widx =
            (after[0].name.find("X870E") != std::string::npos)
                ? 0u : 1u;
        const uint64_t w1 =
            server.CountEvents(sdk::kPktUpdateLEDs, widx);
        CHECK(WaitFor(host, [&]{
            return server.CountEvents(sdk::kPktUpdateLEDs, widx) > w1;
        }, 5000));
    }

    /*------------------------------------------------*/
    /* Teardown                                        */
    /*------------------------------------------------*/

    host.RequestStop();
    const OrderlyShutdown::Result& sd = host.Stop();
    CHECKV(sd.all_within_bound, "stop overrun");
    CHECK(client->State() == ConnectionState::Disconnected);

    fprintf(stderr, "e2e mode qualification OK\n");
    return 0;
}
