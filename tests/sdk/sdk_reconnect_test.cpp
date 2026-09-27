// SPDX-License-Identifier: GPL-2.0-or-later
//
// Reconnect tests for the OpenRGB SDK client: bounded
// backoff math, liveness detection, scheduled retries,
// exhaustion to Disconnected, and — critically — DeviceKey
// re-resolution across reconnect so a reordered controller
// list can never receive a stale-index write (plan
// section 6 review focus: "reordered controllers ... must
// not misroute colors"). Deterministic and offline: the
// in-process fake backend drops/peers on demand.

#include "OpenRgbClient.h"
#include "ReconnectBackoff.h"
#include "fake_sdk_server.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>

using namespace openrgb2;
using namespace openrgb2::sdk;
using sdk_test::FakeSdkServer;
using sdk_test::FakeStream;
using sdk_test::FakeController;
using sdk_test::FakeMode;
using sdk_test::FakeZone;
using sdk_test::ServerEvent;

static int checks   = 0;
static int failures = 0;
#define CHECK(x) do { ++checks; if(!(x)) { ++failures; \
    std::printf("FAIL line %d: %s\n", __LINE__, #x); } } while(0)

static FakeController WirelessGroup(const std::string& serial)
{
    FakeController c;
    c.name     = "Lian Li SL Wireless";
    c.vendor   = "Lian Li";
    c.serial   = serial;
    c.location = "hid:" + serial;
    FakeMode direct; direct.name = "Direct";
    c.modes = { direct };
    c.zones = { { "fan-1", 0, 0, 40, 40 },
                { "fan-2", 0, 0, 40, 40 },
                { "fan-3", 0, 0, 40, 40 } };
    for(uint32_t i = 0; i < 120; ++i)
    {
        c.led_names.push_back("LED " + std::to_string(i));
        c.colors.push_back(0);
    }
    return c;
}

static FakeController Dimm()
{
    FakeController c;
    c.name     = "Corsair Vengeance RGB";
    c.vendor   = "Corsair";
    c.serial   = "D1";
    c.location = "i2c:0";
    FakeMode direct; direct.name = "Direct";
    c.modes = { direct };
    c.zones = { { "dimm", 0, 0, 10, 10 } };
    for(uint32_t i = 0; i < 10; ++i)
    {
        c.led_names.push_back("LED " + std::to_string(i));
        c.colors.push_back(0);
    }
    return c;
}

static OpenRgbClientOptions Opts()
{
    OpenRgbClientOptions o;
    o.backoff.initial_ms   = 0;
    o.backoff.max_ms       = 1;
    o.backoff.max_attempts = 5;
    return o;
}

static std::unique_ptr<OpenRgbClient> MakeClient(
    FakeSdkServer& server, FakeStream** out_stream)
{
    auto stream = std::make_unique<FakeStream>(server);
    *out_stream = stream.get();
    return std::make_unique<OpenRgbClient>(Opts(), std::move(stream));
}

static RgbFrame MakeFrame(const DeviceSnapshot& snap, RgbColor fill)
{
    RgbFrame f;
    f.controller          = snap.key;
    f.topology_generation = snap.topology_generation;
    f.sequence            = 1;
    f.sample_time         = Clock::now();
    f.colors.assign(snap.LedCount(), fill);
    return f;
}

/*---------------------------------------------------------*\
||| Bounded backoff math                                    ||
\*---------------------------------------------------------*/

static void TestBackoffMath()
{
    ReconnectBackoff b;
    b.initial_ms   = 100;
    b.multiplier   = 2.0;
    b.max_ms       = 1000;
    b.max_attempts = 4;

    CHECK(b.DelayFor(0) == std::chrono::milliseconds(100));
    CHECK(b.DelayFor(1) == std::chrono::milliseconds(200));
    CHECK(b.DelayFor(2) == std::chrono::milliseconds(400));
    CHECK(b.DelayFor(3) == std::chrono::milliseconds(800));
    /* Clamped at max_ms — the schedule is bounded. */
    CHECK(b.DelayFor(4) == std::chrono::milliseconds(1000));
    CHECK(b.DelayFor(20) == std::chrono::milliseconds(1000));
}

/*---------------------------------------------------------*\
||| Graceful close detected by the liveness check, then     ||
||| bounded-backoff reconnect                               ||
\*---------------------------------------------------------*/

static void TestGracefulDropReconnect()
{
    FakeSdkServer server;
    server.controllers = { WirelessGroup("W1") };

    /* A non-zero initial delay keeps the Reconnecting state
       observable: the first Poll detects the close, the
       attempt is not yet due. */
    OpenRgbClientOptions opts = Opts();
    opts.backoff.initial_ms   = 60000;
    opts.backoff.max_ms       = 60000;
    auto fake = std::make_unique<FakeStream>(server);
    FakeStream* stream = fake.get();
    auto client =
        std::make_unique<OpenRgbClient>(opts, std::move(fake));
    CHECK(client->Connect());

    stream->SetPeerClosed(true);           /* graceful FIN */

    /* Poll detects the peer close and starts the schedule. */
    const TimePoint t0 = Clock::now();
    CHECK(!client->Poll(t0));
    CHECK(client->State() == ConnectionState::Reconnecting);
    const std::vector<DeviceSnapshot> snaps = client->Snapshots();
    CHECK(snaps.size() == 1);
    CHECK(snaps[0].connection == ConnectionState::Reconnecting);

    /* Past the backoff deadline, the next Poll reconnects. */
    CHECK(client->Poll(t0 + std::chrono::seconds(61)));
    CHECK(client->State() == ConnectionState::Connected);
    CHECK(client->Snapshots()[0].topology_generation == 2);
}

/*---------------------------------------------------------*\
||| Backoff pacing: no attempt before the deadline, one     ||
||| attempt per due Poll                                    ||
\*---------------------------------------------------------*/

static void TestBackoffPacing()
{
    FakeSdkServer server;
    server.controllers = { WirelessGroup("W1") };

    FakeStream*           stream = nullptr;
    OpenRgbClientOptions  opts   = Opts();
    opts.backoff.initial_ms   = 60000;    /* far future */
    opts.backoff.multiplier   = 2.0;
    opts.backoff.max_ms       = 60000;
    auto fake = std::make_unique<FakeStream>(server);
    stream    = fake.get();
    auto client =
        std::make_unique<OpenRgbClient>(opts, std::move(fake));
    CHECK(client->Connect());
    stream->SetPeerClosed(true);

    const TimePoint t0 = Clock::now();
    CHECK(!client->Poll(t0));
    CHECK(client->State() == ConnectionState::Reconnecting);
    CHECK(client->ReconnectAttempt() == 0);

    /* Not due yet: no connection attempt is made. */
    CHECK(!client->Poll(t0 + std::chrono::seconds(30)));
    CHECK(client->State() == ConnectionState::Reconnecting);
    CHECK(client->ReconnectAttempt() == 0);
    CHECK(client->NextAttemptAt() > t0 + std::chrono::seconds(30));

    /* Past the deadline: the attempt runs and (server still
       listening) reconnects — exactly once. */
    CHECK(client->Poll(t0 + std::chrono::seconds(61)));
    CHECK(client->State() == ConnectionState::Connected);
}

/*---------------------------------------------------------*\
||| Bounded attempts: exhausted schedule -> Disconnected    ||
\*---------------------------------------------------------*/

static void TestBackoffExhaustion()
{
    /* Nothing listens — every attempt fails. */
    FakeSdkServer server;
    server.listening          = false;
    server.controllers        = { WirelessGroup("W1") };

    OpenRgbClientOptions opts = Opts();
    opts.backoff.initial_ms   = 0;
    opts.backoff.max_ms       = 0;
    opts.backoff.max_attempts = 3;
    auto client = std::make_unique<OpenRgbClient>(
        opts, std::make_unique<FakeStream>(server));

    CHECK(!client->Connect());
    CHECK(client->State() == ConnectionState::Disconnected);

    CHECK(!client->ReconnectNow());       /* attempt 1 fails */
    CHECK(client->State() == ConnectionState::Reconnecting);
    CHECK(client->ReconnectAttempt() == 1);

    CHECK(!client->Poll());               /* attempt 2 fails */
    CHECK(client->ReconnectAttempt() == 2);
    CHECK(!client->Poll());               /* attempt 3 fails */
    CHECK(client->ReconnectAttempt() == 3);

    /* Bounded: the schedule gives up and latches Disconnected
       until an explicit retry. */
    CHECK(!client->Poll());
    CHECK(client->State() == ConnectionState::Disconnected);
}

/*---------------------------------------------------------*\
||| Undetected reset: the send path fails -> Rejected +     ||
||| the reconnect schedule starts                           ||
\*---------------------------------------------------------*/

static void TestSendFailureRejected()
{
    FakeSdkServer server;
    server.controllers = { WirelessGroup("W1") };

    FakeStream* stream = nullptr;
    auto client = MakeClient(server, &stream);
    CHECK(client->Connect());
    DeviceSnapshot snap;
    CHECK(client->Snapshot(
        DeviceKey{ "Lian Li:Lian Li SL Wireless", "W1",
                   "hid:W1", "" },
        snap));

    /* RST the peer hasn't surfaced through liveness yet. */
    stream->SetSendFails(true);

    SubmitResult r = client->SubmitLatest(MakeFrame(snap, { 1, 0, 0 }));
    CHECK(r.status == SubmitStatus::Rejected);
    CHECK(!r.reason.empty());
    CHECK(client->State() == ConnectionState::Reconnecting);
    CHECK(client->Stats().rejected == 1);

    /* With the backend down, submissions while Reconnecting
       are refused and the due attempt keeps failing. */
    server.listening = false;
    stream->SetSendFails(false);
    CHECK(client->SubmitLatest(MakeFrame(snap, { 1, 0, 0 })).status ==
          SubmitStatus::Rejected);
    CHECK(client->State() == ConnectionState::Reconnecting);

    /* The backend returns: the next due attempt recovers. */
    server.listening = true;
    CHECK(client->Poll());
    CHECK(client->State() == ConnectionState::Connected);
}

/*---------------------------------------------------------*\
||| Changed controller indices across reconnect: writes     ||
||| re-resolve via DeviceKey — never a stale index          ||
\*---------------------------------------------------------*/

static void TestReorderedControllersNeverMisroute()
{
    FakeSdkServer server;
    /* Index 0 = wireless W1, index 1 = DIMM. */
    server.controllers = { WirelessGroup("W1"), Dimm() };

    FakeStream* stream = nullptr;
    auto client = MakeClient(server, &stream);
    CHECK(client->Connect());

    DeviceSnapshot dimm1;
    const DeviceKey dimm_key{ "Corsair:Corsair Vengeance RGB",
                              "D1", "i2c:0", "" };
    const DeviceKey wkey{ "Lian Li:Lian Li SL Wireless", "W1",
                          "hid:W1", "" };
    CHECK(client->Snapshot(dimm_key, dimm1));
    CHECK(dimm1.topology_generation == 1);

    /* First write to the DIMM hits dev_id 1. (The client-name
       packet is also recorded — filter on UPDATELEDS.) */
    CHECK(client->SubmitLatest(MakeFrame(dimm1, { 1, 0, 0 })).status ==
          SubmitStatus::Accepted);
    {
        size_t writes = 0;
        for(const ServerEvent& e : server.events)
        {
            if(e.pkt_id == kPktUpdateLEDs)
            {
                ++writes;
                CHECK(e.dev_id == 1);
            }
        }
        CHECK(writes == 1);
    }

    /* Reconnect with the controller list REORDERED: the DIMM
       is now index 0, the wireless group index 1. With
       zero-delay backoff the single Poll detects the close
       and reconnects in the same call. */
    stream->SetPeerClosed(true);
    server.controllers = { Dimm(), WirelessGroup("W1") };
    CHECK(client->Poll());
    CHECK(client->State() == ConnectionState::Connected);

    DeviceSnapshot dimm2, w2;
    CHECK(client->Snapshot(dimm_key, dimm2));
    CHECK(client->Snapshot(wkey, w2));
    CHECK(dimm2.topology_generation == 2);

    /* A frame stamped with the OLD generation is obsolete —
       the stale-index write is refused outright. */
    RgbFrame stale = MakeFrame(dimm2, { 9, 9, 9 });
    stale.topology_generation = 1;
    CHECK(client->SubmitLatest(stale).status == SubmitStatus::Rejected);

    /* The DeviceKey re-resolves: the DIMM write must go to
       dev_id 0 (its new index), and the wireless group to 1. */
    server.events.clear();
    CHECK(client->SubmitLatest(MakeFrame(dimm2, { 2, 0, 0 })).status ==
          SubmitStatus::Accepted);
    CHECK(client->SubmitLatest(MakeFrame(w2, { 3, 0, 0 })).status ==
          SubmitStatus::Accepted);
    CHECK(server.events.size() == 2);
    CHECK(server.events[0].dev_id == 0);   /* DIMM at new index */
    CHECK(server.events[1].dev_id == 1);   /* wireless at new idx */
    /* The DIMM packet carries exactly its 10 LEDs — proof the
       write went to the right controller. */
    uint16_t count = 0;
    memcpy(&count, server.events[0].body.data() + 4, 2);
    CHECK(count == 10);
    memcpy(&count, server.events[1].body.data() + 4, 2);
    CHECK(count == 120);
}

/*---------------------------------------------------------*\
||| Submissions during Reconnecting drive the schedule      ||
||| forward; stale-generation frames stay refused           ||
\*---------------------------------------------------------*/

static void TestSubmitDuringReconnect()
{
    FakeSdkServer server;
    server.controllers = { WirelessGroup("W1") };

    FakeStream* stream = nullptr;
    auto client = MakeClient(server, &stream);
    CHECK(client->Connect());
    DeviceSnapshot snap;
    CHECK(client->Snapshot(
        DeviceKey{ "Lian Li:Lian Li SL Wireless", "W1",
                   "hid:W1", "" },
        snap));

    /* Backend goes away: the due attempts keep failing, so
       the client stays Reconnecting and submissions are
       refused — never silently sent or queued. */
    stream->SetPeerClosed(true);
    server.listening = false;
    CHECK(!client->Poll());
    CHECK(client->State() == ConnectionState::Reconnecting);

    RgbFrame     old = MakeFrame(snap, { 1, 0, 0 });
    SubmitResult r   = client->SubmitLatest(old);
    CHECK(r.status == SubmitStatus::Rejected);
    CHECK(client->State() == ConnectionState::Reconnecting);

    /* The backend returns; the next due attempt reconnects
       and the old-generation frame is refused as stale. */
    server.listening = true;
    CHECK(client->Poll());
    CHECK(client->State() == ConnectionState::Connected);
    CHECK(client->SubmitLatest(old).status == SubmitStatus::Rejected);

    const std::vector<DeviceSnapshot> now = client->Snapshots();
    RgbFrame fresh = MakeFrame(now[0], { 1, 0, 0 });
    CHECK(client->SubmitLatest(fresh).status == SubmitStatus::Accepted);
}

int main()
{
    TestBackoffMath();
    TestGracefulDropReconnect();
    TestBackoffPacing();
    TestBackoffExhaustion();
    TestSendFailureRejected();
    TestReorderedControllersNeverMisroute();
    TestSubmitDuringReconnect();
    std::printf("sdk reconnect: %d checks, %d failures\n", checks,
                failures);
    return failures ? 1 : 0;
}
