/*---------------------------------------------------------*\
||| e2e_noninterference_test.cpp                             ||
|||                                                          ||
|||   Task 5.4 non-interference verification — drives the   ||
|||   REAL EngineHost production composition (SceneWorker   ||
|||   -> frame queue -> DeviceMapper -> DeliveryPipeline    ||
|||   lane threads -> OpenRgbClient) against the in-proc    ||
|||   fake SDK server:                                       ||
|||                                                          ||
|||   - display-off gate: while the display is off, ZERO    ||
|||     RGB writes reach ANY controller lane — counted       ||
|||     per device at the fake backend's event log, with     ||
|||     a distinct GPU-controller lane asserted separately   ||
|||     (a recurring GPU write is the display-wake hazard    ||
|||     the milestone exists to exclude)                     ||
|||   - parked requests: a playback Request issued while     ||
|||     the output gate holds must not cross the wire — it   ||
|||     is validated, recorded, and parked, then replays     ||
|||     through the normal path after the display returns    ||
|||   - missing-binding integrity across a RESTART: the      ||
|||     resolved-identity record persisted in the engine     ||
|||     profile pins the binding's serial; a same-named      ||
|||     device with a different serial never inherits it     ||
|||     and receives zero frames                             ||
|||                                                          ||
|||   Physical display-off evidence and the cooling/sensor   ||
|||   stress run are documented separately in                ||
|||   docs/benchmarks/non-interference.md                    ||
|||                                                          ||
|||   SPDX-License-Identifier: GPL-2.0-or-later              ||
\*---------------------------------------------------------*/

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
||| Fake SDK fixture                                        ||
\*---------------------------------------------------------*/

static const char* kMac       = "FA:KE:54:00:00:01";
static const char* kDimmSer   = "DIMM-SN-NONINT-A";
static const char* kImpostSer = "DIMM-SN-NONINT-FAKE";

static sdk_test::FakeController WirelessGroup()
{
    sdk_test::FakeController c;
    c.type     = 5;
    c.name     = "UNI FAN SL V3 Wireless NONINT";
    c.vendor   = "Lian Li";
    c.serial   = kMac;
    c.location = std::string("Wireless: ") + kMac;
    for(const char* m : { "Direct", "Spectrum Cycle",
                          "Rainbow Wave" })
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

static sdk_test::FakeController Gpu()
{
    sdk_test::FakeController c;
    c.type     = 1;   /* GPU — the display-wake hazard lane */
    c.name     = "NVIDIA GeForce RTX NONINT";
    c.vendor   = "NVIDIA";
    c.serial   = "GPU-NONINT-1";
    c.location = "pci:0-gpu-nonint";
    sdk_test::FakeMode m;
    m.name = "Direct";
    c.modes.push_back(m);
    c.zones = { { "GPU", 0, 4, 4, 4 } };
    c.colors.assign(4, 0x00010101u);
    return c;
}

static sdk_test::FakeController Dimm(const char* serial,
                                     const char* location)
{
    sdk_test::FakeController c;
    c.type     = 2;
    c.name     = "Corsair Vengeance RGB DDR5 NONINT";
    c.vendor   = "Corsair";
    c.serial   = serial;
    c.location = location;
    sdk_test::FakeMode m;
    m.name = "Direct";
    c.modes.push_back(m);
    c.zones = { { "DRAM", 0, 10, 10, 10 } };
    c.colors.assign(10, 0x00010101u);
    return c;
}

/* Every packet id that writes device state — reads
   (controller count/data, protocol version, client name)
   never count. */
static uint64_t WriteOps(const sdk_test::FakeSdkServer& server,
                         uint32_t dev)
{
    uint64_t n = 0;
    for(uint32_t pkt : { sdk::kPktUpdateLEDs,
                         sdk::kPktSetCustomMode,
                         sdk::kPktUpdateMode,
                         sdk::kPktSaveMode,
                         sdk::kPktResizeZone })
    {
        n += server.CountEvents(pkt, dev);
    }
    return n;
}
static uint64_t WriteOpsAll(const sdk_test::FakeSdkServer& s)
{
    return WriteOps(s, 0) + WriteOps(s, 1) + WriteOps(s, 2);
}

/*---------------------------------------------------------*\
||| Scene documents (version-2 schema)                     ||
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
    return json::array({
        { { "id", "b-fan1" },
          { "controller_name", "UNI FAN SL" },
          { "serial", kMac },
          { "zone_name", "Fan 1" }, { "zone_leds", 40 } },
        { { "id", "b-fan2" },
          { "controller_name", "UNI FAN SL" },
          { "serial", kMac },
          { "zone_name", "Fan 2" }, { "zone_leds", 40 } },
        { { "id", "b-fan3" },
          { "controller_name", "UNI FAN SL" },
          { "serial", kMac },
          { "zone_name", "Fan 3" }, { "zone_leds", 40 } },
        /* GPU binding — serial-pinned so it can never
           wander. */
        { { "id", "b-gpu" },
          { "controller_name", "GeForce RTX" },
          { "serial", "GPU-NONINT-1" },
          { "zone_name", "GPU" }, { "zone_leds", 4 } },
        /* Name-only DIMM binding — the identity record
           fills the serial after the first apply. The name
           must equal the recorded identity's: the augment
           refuses to touch a re-purposed binding id. */
        { { "id", "b-dimm" },
          { "controller_name",
            "Corsair Vengeance RGB DDR5 NONINT" },
          { "zone_name", "DRAM" }, { "zone_leds", 10 } },
    });
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
    obj("o-gpu",  "b-gpu",  4);
    obj("o-dimm", "b-dimm", 10);
    return objects;
}

/* Playback-eligible scene: names the Rainbow Wave preset
   with no inline layers — the shape an animation request
   may target. It still streams live frames in LiveEffects
   mode, so it doubles as the live-traffic source. */
static json SceneDoc(const char* name)
{
    return {
        { "version", 2 },
        { "name",    name },
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
||| Helpers                                                ||
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

static EngineHostOptions MakeOpts(
    sdk_test::FakeSdkServer& server,
    const std::string& state_path)
{
    EngineHostOptions opts;
    opts.sdk_enabled        = true;
    opts.ipc_enabled        = false;
    opts.power_monitor      = false;
    opts.reactive_inputs    = false;
    opts.scan_owners        = false;
    opts.single_instance    = true;
    opts.instance_name      = "Local\\openrgb2-engine-nonint-test";
    opts.state_path         = state_path;
    opts.backend_config_dir = "";
    opts.log                = [](const std::string&) {};
    opts.transport_factory  =
        [&server]() -> std::unique_ptr<sdk::IStreamTransport>
        { return std::make_unique<sdk_test::FakeStream>(server); };
    return opts;
}

/*---------------------------------------------------------*\
||| Main                                                   ||
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

/*=======================================================*\
||| Case 1 + 2: display-off zero writes + parked request ||
\*=======================================================*/
static int TestDisplayOffGate(const std::string& state_path)
{
    sdk_test::FakeSdkServer server;
    /* Lane indices: 0 wireless, 1 GPU, 2 DIMM-A. */
    server.controllers = { WirelessGroup(), Gpu(),
                           Dimm(kDimmSer, "smbus:0-0053") };

    EngineHost host(MakeOpts(server, state_path));
    std::string err;
    CHECKV(host.Start(err), "Start: %s", err.c_str());

    PowerLifecycle*     life     = host.GetLifecycle();
    PlaybackController* playback = host.GetPlayback();
    ipc::EngineService* service  = host.GetService();
    CHECK(life && playback && service);

    /* Live scene streaming to all three lanes. */
    CHECKV(ApplyScene(service, SceneDoc("nonint-live"), err),
           "apply live scene: %s", err.c_str());
    CHECK(WaitFor(host, [&]{
        return WriteOpsAll(server) > 0 &&
               server.CountEvents(sdk::kPktUpdateLEDs, 1) > 0;
    }, 10000));

    /* Display off: the output gate asserts on the next poll;
       from the assertion on, ZERO writes reach ANY lane —
       counted per device so the GPU lane is proven
       separately, not hidden inside an aggregate. */
    life->PostEvent(PowerLifecycle::Event::DisplayOff);
    CHECK(WaitFor(host, [&]{ return life->OutputPaused(); },
                  5000));

    const uint64_t w0 = WriteOps(server, 0);
    const uint64_t w1 = WriteOps(server, 1);
    const uint64_t w2 = WriteOps(server, 2);
    PumpFor(host, 1200);
    CHECKV(WriteOps(server, 0) == w0,
           "wireless lane wrote %llu times while gated",
           (unsigned long long)(WriteOps(server, 0) - w0));
    CHECKV(WriteOps(server, 1) == w1,
           "GPU lane wrote %llu times while display off",
           (unsigned long long)(WriteOps(server, 1) - w1));
    CHECKV(WriteOps(server, 2) == w2,
           "DIMM lane wrote %llu times while gated",
           (unsigned long long)(WriteOps(server, 2) - w2));

    /*------------------------------------------------*\
    || A playback request inside the gated window:     ||
    || parked, never written, replayed after wake.     ||
    \*-------------------------------------------------*/
    {
        PlaybackParams params;
        params.speed = 60;
        const PlaybackController::Result r = playback->Request(
            PlaybackMode::AnimationPlayback,
            PlaybackControl::Playing, params);
        CHECKV(r.accepted,
               "gated request refused: %s", r.reason.c_str());
        CHECKV(r.pending, "parked request not pending");
        CHECK(playback->AckPending());
    }
    /* Still zero writes — the parked request must not cross
       the wire while the gate holds. */
    PumpFor(host, 700);
    CHECKV(WriteOpsAll(server) == w0 + w1 + w2,
           "parked request issued %llu writes while gated",
           (unsigned long long)(WriteOpsAll(server) -
                                (w0 + w1 + w2)));

    /* Display on: gate lifts after the revalidation; the
       parked request replays and the upload lands — every
       byte strictly AFTER the wake. */
    life->PostEvent(PowerLifecycle::Event::DisplayOn);
    CHECK(WaitFor(host, [&]{ return !life->OutputPaused(); },
                  10000));
    CHECK(WaitFor(host, [&]{
        const PlaybackStatus s = playback->Status();
        return s.mode == PlaybackMode::AnimationPlayback &&
               s.upload == UploadState::Active;
    }, 15000));
    CHECKV(server.CountEvents(sdk::kPktUpdateMode, 0) >= 1,
           "uploaded loop never reached the wireless group");
    CHECKV(playback->Detail().empty(),
           "detail after parked replay: %s",
           playback->Detail().c_str());

    /* Live frames still reach the GPU lane only while the
       display is ON — captured baseline proves streaming
       resumed. */
    const uint64_t g0 = server.CountEvents(sdk::kPktUpdateLEDs, 1);
    PumpFor(host, 600);
    CHECKV(server.CountEvents(sdk::kPktUpdateLEDs, 1) > g0,
           "GPU lane did not resume live frames post-wake");

    host.Stop();
    return 0;
}

/*=======================================================*\
||| Case 3: missing-binding integrity across a restart   ||
\*=======================================================*/
static int TestMissingBindingAcrossRestart(
    const std::string& state_path)
{
    /* Phase A — the real DIMM present: the name-only binding
       resolves and the resolved identity (serial/location)
       is recorded into the engine profile. */
    {
        sdk_test::FakeSdkServer server;
        server.controllers = { WirelessGroup(), Gpu(),
                               Dimm(kDimmSer, "smbus:0-0053") };
        EngineHost host(MakeOpts(server, state_path));
        std::string err;
        CHECKV(host.Start(err), "Start: %s", err.c_str());
        CHECKV(ApplyScene(host.GetService(),
                          SceneDoc("nonint-id"), err),
               "apply identity scene: %s", err.c_str());
        CHECK(WaitFor(host, [&]{
            return server.CountEvents(sdk::kPktUpdateLEDs, 2) > 0;
        }, 10000));
        PumpFor(host, 400);   /* identity record persists */
        host.Stop();
    }

    /* Phase B — RESTART with the original DIMM absent and a
       same-named imposter (different serial, different bus)
       in its place. The binding must stay Unresolved and the
       imposter must receive ZERO frames — name match alone
       never reassigns a serial-pinned binding. */
    {
        sdk_test::FakeSdkServer server;
        server.controllers = {
            WirelessGroup(), Gpu(),
            Dimm(kImpostSer, "smbus:0-0099"),   /* idx 2 */
        };
        EngineHost host(MakeOpts(server, state_path));
        std::string err;
        CHECKV(host.Start(err), "Start: %s", err.c_str());
        CHECKV(ApplyScene(host.GetService(),
                          SceneDoc("nonint-id"), err),
               "apply identity scene (imposter): %s",
               err.c_str());

        /* Frames flow to the bound lanes; the binding states
           surface through the engine state snapshot. */
        CHECK(WaitFor(host, [&]{
            return server.CountEvents(sdk::kPktUpdateLEDs, 0) > 0;
        }, 10000));
        PumpFor(host, 800);
        CHECKV(server.CountEvents(sdk::kPktUpdateLEDs, 2) == 0,
               "imposter received %llu live frames",
               (unsigned long long)
                   server.CountEvents(sdk::kPktUpdateLEDs, 2));
        {
            studio::BindingStatus bs =
                studio::BindingStatus::Resolved;
            CHECKV(host.GetMapper().BindingState("b-dimm", bs) &&
                   bs == studio::BindingStatus::Unresolved,
                   "b-dimm not Unresolved with imposter present");
        }

        /* The real DIMM returns — the SAME binding re-resolves
           to it and only it. */
        server.AddController(Dimm(kDimmSer, "smbus:0-0053"));
        CHECK(WaitFor(host, [&]{
            return server.CountEvents(sdk::kPktUpdateLEDs, 3) > 0;
        }, 10000));
        CHECKV(server.CountEvents(sdk::kPktUpdateLEDs, 2) == 0,
               "imposter grabbed frames after return");
        {
            studio::BindingStatus bs =
                studio::BindingStatus::Unresolved;
            CHECKV(host.GetMapper().BindingState("b-dimm", bs) &&
                   bs == studio::BindingStatus::Resolved,
                   "b-dimm not Resolved after return");
        }
        host.Stop();
    }
    return 0;
}

static int TestBody()
{
    std::error_code ec;
    const std::filesystem::path tmpdir =
        std::filesystem::temp_directory_path(ec) /
        "openrgb2-nonint-state";
    std::filesystem::create_directories(tmpdir, ec);
    const std::string state_path =
        (tmpdir / "engine-state.json").generic_string();
    std::filesystem::remove(state_path, ec);

    if(int rc = TestDisplayOffGate(state_path))
    {
        return rc;
    }

    /* Case 3 needs a fresh profile (its own identity record). */
    std::filesystem::remove(state_path, ec);
    if(int rc = TestMissingBindingAcrossRestart(state_path))
    {
        return rc;
    }
    return 0;
}
