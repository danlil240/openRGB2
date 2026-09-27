// SPDX-License-Identifier: GPL-2.0-or-later
//
// Client tests for the OpenRGB SDK client
// (src/backends/openrgb/OpenRgbClient.h) against the
// in-process fake backend: version negotiation capped at 4,
// enumeration resolving modes/zones by NAME, complete-
// controller UpdateLEDs writes, mode selection, resize,
// clean rejection of unsupported versions/modes, and
// DeliveryStats reporting at the transport boundary.
// Deterministic and offline: no sockets, no hardware.

#include "OpenRgbClient.h"
#include "fake_sdk_server.h"

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

static FakeController WirelessGroup()
{
    FakeController c;
    c.name     = "Lian Li SL Wireless";
    c.vendor   = "Lian Li";
    c.serial   = "W1";
    c.location = "hid:1";

    FakeMode direct;   direct.name   = "Direct";         direct.speed = 50;
    FakeMode spectrum; spectrum.name = "Spectrum Cycle"; spectrum.speed = 100;
    FakeMode breath;   breath.name   = "Breathing";      breath.speed = 20;
    c.modes = { direct, spectrum, breath };

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
    o.client_name          = "sdk-client-test";
    o.backoff.initial_ms   = 0;            /* immediate retries */
    o.backoff.max_ms       = 1;
    o.backoff.max_attempts = 5;
    return o;
}

static std::unique_ptr<OpenRgbClient> MakeClient(FakeSdkServer& server,
                                                 FakeStream** out_stream = nullptr)
{
    auto stream = std::make_unique<FakeStream>(server);
    if(out_stream)
    {
        *out_stream = stream.get();
    }
    return std::make_unique<OpenRgbClient>(Opts(), std::move(stream));
}

static const ServerEvent* FindEvent(
    const std::vector<ServerEvent>& events, uint32_t pkt_id)
{
    for(const ServerEvent& e : events)
    {
        if(e.pkt_id == pkt_id)
        {
            return &e;
        }
    }
    return nullptr;
}

static size_t CountEvents(const std::vector<ServerEvent>& events,
                          uint32_t pkt_id)
{
    size_t n = 0;
    for(const ServerEvent& e : events)
    {
        if(e.pkt_id == pkt_id)
        {
            ++n;
        }
    }
    return n;
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
||| Handshake: negotiated version is capped at 4 even       ||
||| though the server advertises 6 (the real fork).         ||
\*---------------------------------------------------------*/

static void TestHandshakeCapsAt4()
{
    FakeSdkServer server;
    server.controllers = { WirelessGroup(), Dimm() };

    auto client = MakeClient(server);
    CHECK(client->Connect());
    CHECK(client->State() == ConnectionState::Connected);
    CHECK(client->ProtocolVersion() == 4);
    CHECK(client->LastError().empty());
}

static void TestUnsupportedVersionRejected()
{
    FakeSdkServer server;
    server.reported_version = 0;          /* unusable */
    server.controllers      = { WirelessGroup() };

    auto client = MakeClient(server);
    CHECK(!client->Connect());
    CHECK(client->State() == ConnectionState::Disconnected);
    CHECK(!client->LastError().empty());
    CHECK(client->Snapshots().empty());
}

static void TestUnreachableBackend()
{
    FakeSdkServer server;
    server.listening = false;             /* nothing on 6744 */

    auto client = MakeClient(server);
    CHECK(!client->Connect());
    CHECK(client->State() == ConnectionState::Disconnected);
    CHECK(!client->LastError().empty());
}

/*---------------------------------------------------------*\
||| Enumeration: DeviceKeys + modes/zones resolved from     ||
||| enumeration, never hard-coded indices                   ||
\*---------------------------------------------------------*/

static void TestEnumeration()
{
    FakeSdkServer server;
    server.controllers = { WirelessGroup(), Dimm() };

    auto client = MakeClient(server);
    CHECK(client->Connect());

    const std::vector<DeviceSnapshot> snaps = client->Snapshots();
    CHECK(snaps.size() == 2);

    DeviceSnapshot w;
    const DeviceKey key{ "Lian Li:Lian Li SL Wireless", "W1",
                         "hid:1", "" };
    CHECK(client->Snapshot(key, w));
    CHECK(w.topology_generation == 1);
    CHECK(w.connection == ConnectionState::Connected);
    CHECK(w.LedCount() == 120);
    CHECK(w.zones.size() == 3);
    CHECK(w.zones[1].name == "fan-2");
    CHECK(w.zones[2].led_count == 40);
    CHECK(w.modes.size() == 3);
    CHECK(w.modes[0] == "Direct" && w.modes[1] == "Spectrum Cycle");

    /* The DIMM resolves by its own stable identity. */
    DeviceSnapshot d;
    CHECK(client->Snapshot(
        DeviceKey{ "Corsair:Corsair Vengeance RGB", "D1",
                   "i2c:0", "" },
        d));
    CHECK(d.LedCount() == 10);
}

/*---------------------------------------------------------*\
||| submitLatest: ONE complete-controller UPDATELEDS        ||
||| packet — never three zone uploads                       ||
\*---------------------------------------------------------*/

static void TestCompleteControllerWrite()
{
    FakeSdkServer server;
    server.controllers = { WirelessGroup(), Dimm() };

    auto client = MakeClient(server);
    CHECK(client->Connect());
    DeviceSnapshot target;
    CHECK(client->Snapshot(
        DeviceKey{ "Lian Li:Lian Li SL Wireless", "W1",
                   "hid:1", "" },
        target));

    RgbFrame frame = MakeFrame(target, { 1, 2, 3 });
    SubmitResult r = client->SubmitLatest(frame);
    CHECK(r.status == SubmitStatus::Accepted);

    const std::vector<ServerEvent>& events = server.events;
    /* Exactly ONE packet for the whole 120-LED controller —
       the three zones must not produce three uploads. */
    CHECK(CountEvents(events, kPktUpdateLEDs) == 1);
    const ServerEvent* ev = FindEvent(events, kPktUpdateLEDs);
    CHECK(ev != nullptr);
    /* dev_id is the controller's enumeration index (0 here:
       the wireless group is first in the fake's list). */
    CHECK(ev->dev_id == 0);
    uint32_t size_field = 0;
    memcpy(&size_field, ev->body.data(), 4);
    CHECK(size_field == ev->body.size());
    uint16_t count = 0;
    memcpy(&count, ev->body.data() + 4, 2);
    CHECK(count == 120);
    uint32_t first = 0;
    memcpy(&first, ev->body.data() + 6, 4);
    CHECK(first == 0x00030201u);           /* {r,g,b}->{0x00BBGGRR} */

    /* Result reporting into DeliveryStats (transport boundary —
       never "displayed"). */
    const DeliveryStats d = client->Stats();
    CHECK(d.submitted == 1);
    CHECK(d.rejected == 0);
    CHECK(d.transport_completed == 1);
    CHECK(d.provenance == MeasurementProvenance::TransportMeasured);
}

static void TestSubmitValidation()
{
    FakeSdkServer server;
    server.controllers = { WirelessGroup() };

    auto client = MakeClient(server);
    CHECK(client->Connect());
    DeviceSnapshot w;
    CHECK(client->Snapshot(
        DeviceKey{ "Lian Li:Lian Li SL Wireless", "W1",
                   "hid:1", "" },
        w));

    /* Unknown controller. */
    RgbFrame f = MakeFrame(w, { 1, 0, 0 });
    f.controller.serial = "NOPE";
    SubmitResult r = client->SubmitLatest(f);
    CHECK(r.status == SubmitStatus::Rejected && !r.reason.empty());

    /* Stale topology generation. */
    f = MakeFrame(w, { 1, 0, 0 });
    f.topology_generation = 99;
    CHECK(client->SubmitLatest(f).status == SubmitStatus::Rejected);

    /* LED-count mismatch: a partial frame is never sent. */
    f = MakeFrame(w, { 1, 0, 0 });
    f.colors.resize(40);
    CHECK(client->SubmitLatest(f).status == SubmitStatus::Rejected);

    /* Nothing reached the wire. */
    CHECK(CountEvents(server.events, kPktUpdateLEDs) == 0);
    CHECK(client->Stats().rejected == 3);
}

/*---------------------------------------------------------*\
||| Mode selection/parameters + resize                      ||
\*---------------------------------------------------------*/

static void TestModeSelection()
{
    FakeSdkServer server;
    server.controllers = { WirelessGroup() };

    auto client = MakeClient(server);
    CHECK(client->Connect());
    const DeviceKey key{ "Lian Li:Lian Li SL Wireless", "W1",
                         "hid:1", "" };

    /* Select by NAME — "Spectrum Cycle" is index 1. */
    CHECK(client->SelectMode(key, "Spectrum Cycle"));
    const ServerEvent* ev = FindEvent(server.events, kPktUpdateMode);
    CHECK(ev != nullptr);
    int32_t mode_idx = -1;
    memcpy(&mode_idx, ev->body.data() + 4, 4);
    CHECK(mode_idx == 1);

    /* Speed parameter patches the replayed raw description. */
    server.events.clear();
    CHECK(client->SelectMode(key, "Breathing", 77));
    ev = FindEvent(server.events, kPktUpdateMode);
    CHECK(ev != nullptr);
    memcpy(&mode_idx, ev->body.data() + 4, 4);
    CHECK(mode_idx == 2);
    /* Raw mode description: name_len + name, then
       value/flags/speed_min/max (16) + brightness_min/max (8)
       + colors_min/max (8), then speed. */
    uint16_t name_len = 0;
    memcpy(&name_len, ev->body.data() + 8, 2);
    const size_t speed_off = 8 + 2 + name_len + 16 + 8 + 8;
    uint32_t speed = 0;
    memcpy(&speed, ev->body.data() + speed_off, 4);
    CHECK(speed == 77);

    /* Unsupported mode: rejected cleanly, nothing sent. */
    server.events.clear();
    CHECK(!client->SelectMode(key, "Vortex"));
    CHECK(!client->LastError().empty());
    CHECK(CountEvents(server.events, kPktUpdateMode) == 0);
    CHECK(client->State() == ConnectionState::Connected);

    /* Direct mode via SETCUSTOMMODE. */
    CHECK(client->SetCustomMode(key));
    CHECK(CountEvents(server.events, kPktSetCustomMode) == 1);
}

static void TestResizeZone()
{
    FakeSdkServer server;
    server.controllers = { WirelessGroup() };

    auto client = MakeClient(server);
    CHECK(client->Connect());
    const DeviceKey key{ "Lian Li:Lian Li SL Wireless", "W1",
                         "hid:1", "" };

    CHECK(client->ResizeZone(key, "fan-2", 48));
    const ServerEvent* ev = FindEvent(server.events, kPktResizeZone);
    CHECK(ev != nullptr);
    CHECK(ev->body.size() == 8);
    int32_t zone_idx = -1, new_size = -1;
    memcpy(&zone_idx, ev->body.data(), 4);
    memcpy(&new_size, ev->body.data() + 4, 4);
    CHECK(zone_idx == 1);                    /* resolved by name */
    CHECK(new_size == 48);

    /* Unknown zone name: rejected cleanly. */
    CHECK(!client->ResizeZone(key, "fan-9", 8));
    CHECK(!client->LastError().empty());
}

/*---------------------------------------------------------*\
||| Connection state surfaced through DeviceSnapshot        ||
\*---------------------------------------------------------*/

static void TestDisconnectedSubmit()
{
    FakeSdkServer server;
    server.controllers = { WirelessGroup() };

    auto client = MakeClient(server);
    client->Connect();
    client->Disconnect();
    CHECK(client->State() == ConnectionState::Disconnected);

    /* Snapshots keep the last topology but report the
       disconnected connection state. */
    const std::vector<DeviceSnapshot> snaps = client->Snapshots();
    CHECK(snaps.size() == 1);
    CHECK(snaps[0].connection == ConnectionState::Disconnected);

    RgbFrame f = MakeFrame(snaps[0], { 1, 0, 0 });
    CHECK(client->SubmitLatest(f).status == SubmitStatus::Rejected);
    CHECK(CountEvents(server.events, kPktUpdateLEDs) == 0);
}

/*---------------------------------------------------------*\
||| Malformed server traffic: desync breaks the connection  ||
||| cleanly rather than over-reading                        ||
\*---------------------------------------------------------*/

static void TestMalformedServerReply()
{
    FakeSdkServer server;
    server.controllers = { WirelessGroup() };

    auto client = MakeClient(server);

    /* The peer's replies arrive with a bad magic: the client
       must reject the desynced stream and fail Connect. */
    server.corrupt_reply_magic = true;

    CHECK(!client->Connect());
    CHECK(client->State() == ConnectionState::Disconnected);
    CHECK(!client->LastError().empty());
}

int main()
{
    TestHandshakeCapsAt4();
    TestUnsupportedVersionRejected();
    TestUnreachableBackend();
    TestEnumeration();
    TestCompleteControllerWrite();
    TestSubmitValidation();
    TestModeSelection();
    TestResizeZone();
    TestDisconnectedSubmit();
    TestMalformedServerReply();
    std::printf("sdk client: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
