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

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>
#include <thread>
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

static FakeController Keyboard()
{
    FakeController c;
    c.name     = "Corsair K95";
    c.vendor   = "Corsair";
    c.serial   = "K1";
    c.location = "hid:k1";
    FakeMode direct; direct.name = "Direct";
    c.modes = { direct };
    FakeZone kb;
    kb.name          = "Keyboard";
    kb.type          = 2;
    kb.leds_min      = 6;
    kb.leds_max      = 6;
    kb.leds_count    = 6;
    kb.matrix_rows   = 2;
    kb.matrix_cols   = 3;
    kb.matrix_map    = { 0, 1, 2, 3, 0xFFFFFFFFu, 5 };
    c.zones = { kb };
    const char* names[] = { "Key: Escape", "Key: A", "Key: S",
                            "Key: D", "Key: F", "Key: G" };
    for(const char* n : names)
    {
        c.led_names.push_back(n);
        c.colors.push_back(0);
    }
    return c;
}

static void TestEnumerationMatrixAndLedNames()
{
    FakeSdkServer server;
    server.controllers = { Keyboard() };

    auto client = MakeClient(server);
    CHECK(client->Connect());

    DeviceSnapshot k;
    CHECK(client->Snapshot(
        DeviceKey{ "Corsair:Corsair K95", "K1", "hid:k1", "" },
        k));
    /* Matrix geometry + map survive the wire into the zone
       snapshot; LED names land on the device snapshot. */
    CHECK(k.zones.size() == 1);
    CHECK(k.zones[0].matrix_rows == 2);
    CHECK(k.zones[0].matrix_cols == 3);
    CHECK(k.zones[0].matrix_map.size() == 6);
    CHECK(k.zones[0].matrix_map[4] == 0xFFFFFFFFu);
    CHECK(k.led_names.size() == 6);
    CHECK(k.led_names[0] == "Key: Escape");
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

/*---------------------------------------------------------*\
|||| F12 regression — the state/wire lock split. A state    ||
|||| getter must NEVER wait behind in-flight wire I/O: the   ||
|||| old single-mutex client held its lock across every     ||
|||| request, so a delayed RefreshStatus stalled the        ||
|||| diagnostics surface (the ~9 acquisitions in a          ||
|||| DIAG_GET reply) past the CLI's 5 s read deadline.      ||
||||                                                           ||
|||| With the io_mutex_/mutex_ split the getters below take ||
|||| only the state lock — they must return while the        ||
|||| delayed refresh is still mid-wire.                     ||
\*---------------------------------------------------------*/

static void TestGettersNeverBlockBehindWireIo()
{
    FakeSdkServer server;
    server.controllers = { WirelessGroup(), Dimm() };

    FakeStream* stream = nullptr;
    auto client = MakeClient(server, &stream);
    CHECK(client->Connect());

    /* 300 ms per RecvExact: the refresh's count + 2x data
       requests each do two receives — ~1.8 s of wire time
       during which every state read must stay instant. */
    stream->recv_delay_ms.store(300);

    std::atomic<bool> refresh_running{ false };
    std::atomic<bool> refresh_done{ false };
    std::atomic<bool> refresh_ok{ false };
    std::thread th([&]
    {
        refresh_running = true;
        refresh_ok      = client->RefreshStatus();
        refresh_done    = true;
    });
    while(!refresh_running.load())
    {
        std::this_thread::yield();
    }
    /* Let the refresh thread get inside its first delayed
       receive before the getter burst. */
    std::this_thread::sleep_for(std::chrono::milliseconds(80));

    const auto t0 = Clock::now();
    CHECK(client->State() == ConnectionState::Connected);
    CHECK(client->ProtocolVersion() == 4);
    CHECK(client->LastError().empty());
    CHECK(client->ReconnectAttempt() == 0);
    const std::vector<DeviceSnapshot> snaps = client->Snapshots();
    CHECK(snaps.size() == 2);
    DeviceSnapshot one;
    CHECK(client->Snapshot(
        DeviceKey{ "Lian Li:Lian Li SL Wireless", "W1",
                   "hid:1", "" },
        one));
    int32_t mi = -1;
    CHECK(client->ActiveMode(one.key, mi));
    /* The consolidated read — one state-lock acquisition. */
    const ConnectionSnapshotInfo cs = client->ConnectionSnapshot();
    CHECK(cs.state == ConnectionState::Connected);
    CHECK(cs.protocol_version == 4);
    CHECK(cs.reconnect_attempt == 0);
    CHECK(cs.last_error.empty());
    CHECK(client->Stats().submitted == 0);

    const double ms =
        std::chrono::duration<double, std::milli>(
            Clock::now() - t0).count();
    /* Pre-fix this burst blocked for the full ~1.8 s wire
       sequence; post-fix it is a handful of state reads. The
       bound is generous on purpose — the point is the shape
       (prompt vs seconds), not the microseconds. */
    CHECK(ms < 1000.0);
    /* The refresh really was still in flight — otherwise the
       test proves nothing. */
    CHECK(!refresh_done.load());

    th.join();
    CHECK(refresh_ok.load());
    stream->recv_delay_ms.store(0);

    /* Post-split wire behavior unchanged: the refresh was
       in-place (generation stays 1 — pending frames stay
       valid), the device list is intact, and SubmitLatest
       still sends ONE UPDATELEDS for the whole controller. */
    const std::vector<DeviceSnapshot> after = client->Snapshots();
    CHECK(after.size() == 2);
    CHECK(after[0].topology_generation == 1);
    SubmitResult r =
        client->SubmitLatest(MakeFrame(after[0], { 9, 9, 9 }));
    CHECK(r.status == SubmitStatus::Accepted);
    CHECK(server.CountEvents(kPktUpdateLEDs) == 1);
}

/*---------------------------------------------------------*\
||| N01 observability — LIANLI_WIRELESS_STATUS fetch. A    |||
||| wedged group must be visible through the client's      |||
||| cached snapshot; an old backend that never answers     |||
||| must mark "unsupported" without breaking the           |||
||| connection.                                            |||
\*---------------------------------------------------------*/

static const char* kWedgeStatusJson =
    "{\"available\":true,\"master\":\"FA:C0:87:E5:66:E4\","
    "\"groups\":[{\"mac\":\"W1\",\"state\":\"failed\",\"wedged\":true,"
    "\"confirmed\":false,\"resends\":8,\"send_failures\":2,"
    "\"last_error\":\"resend budget exhausted: no HW ack\","
    "\"rgb_clock\":{\"valid\":true,\"ticks\":5000,"
    "\"sampled_at_ms\":0,\"age_ms\":50}}],"
    "\"counters\":{\"usb_send\":42}}";

static void TestWirelessStatusFetch()
{
    FakeSdkServer server;
    server.controllers = { WirelessGroup() };
    server.wireless_status_json = kWedgeStatusJson;

    auto client = MakeClient(server);
    CHECK(client->Connect());

    /* The connect-time probe answers immediately. */
    WirelessStatusSnapshot snap;
    client->WirelessStatus(snap);
    CHECK(snap.probed);
    CHECK(snap.supported);
    CHECK(snap.have_data);
    CHECK(snap.status.groups.size() == 1);
    /* THE wedge signal: terminal state + cause, previously
       invisible outside the backend process. */
    CHECK(snap.status.groups[0].wedged);
    CHECK(snap.status.groups[0].state == "failed");
    CHECK(snap.status.groups[0].last_error ==
          "resend budget exhausted: no HW ack");
    CHECK((*snap.status.raw)["counters"]["usb_send"] == 42);

    /* RefreshStatus re-fetches so the snapshot stays live. */
    const size_t before =
        server.CountEvents(kPktLianLiWirelessStatus);
    CHECK(before >= 1);
    CHECK(client->RefreshStatus());
    CHECK(server.CountEvents(kPktLianLiWirelessStatus) > before);

    /* Disconnected state keeps the last snapshot — it is a
       cache, not a wire op. */
    client->Disconnect();
    WirelessStatusSnapshot after;
    client->WirelessStatus(after);
    CHECK(after.have_data);
}

static void TestWirelessStatusUnsupportedBackend()
{
    FakeSdkServer server;         /* no scripted reply = stock */
    server.controllers = { WirelessGroup() };

    auto client = MakeClient(server);
    /* A stock backend drops packet 4000 — the probe times
       out, latches "unsupported", and MUST NOT break the
       connection. */
    CHECK(client->Connect());
    CHECK(client->State() == ConnectionState::Connected);

    WirelessStatusSnapshot snap;
    client->WirelessStatus(snap);
    CHECK(snap.probed);
    CHECK(!snap.supported);
    CHECK(!snap.have_data);

    /* Unsupported is latched for this connection: refresh
       does not re-probe (no per-2s stall against a stock
       backend). */
    const size_t probes =
        server.CountEvents(kPktLianLiWirelessStatus);
    CHECK(probes == 1);
    CHECK(client->RefreshStatus());
    CHECK(server.CountEvents(kPktLianLiWirelessStatus) == probes);
    CHECK(client->State() == ConnectionState::Connected);
}

static void TestWirelessStatusMalformedReply()
{
    FakeSdkServer server;
    server.controllers = { WirelessGroup() };
    server.wireless_status_json = "{ this is not valid json ";

    auto client = MakeClient(server);
    CHECK(client->Connect());
    WirelessStatusSnapshot snap;
    client->WirelessStatus(snap);
    /* The server answered — supported — but the reply did
       not parse, so no data is held. */
    CHECK(snap.probed && snap.supported && !snap.have_data);
    CHECK(client->State() == ConnectionState::Connected);
}

static void TestWirelessClockSample()
{
    FakeSdkServer server;
    server.controllers = { WirelessGroup() };
    server.wireless_status_json = kWedgeStatusJson;

    auto client = MakeClient(server);
    CHECK(client->Connect());
    const DeviceKey key{ "Lian Li:Lian Li SL Wireless", "W1",
                         "hid:1", "" };

    /* Group mac == controller serial joins status to the
       enumerated device; rgb_clock age 50 ms converts to a
       PlaybackClockSnapshot. */
    const TimePoint t0 = Clock::now();
    PlaybackClockSnapshot snap;
    CHECK(client->WirelessClockSample(key, snap));
    CHECK(snap.valid);
    CHECK(snap.ticks == 5000);
    CHECK(snap.uncertainty_s > 0.0);
    /* sampled_at = now - 50 ms, within a slack window. */
    const double sampled_age_ms =
        std::chrono::duration<double, std::milli>(
            t0 - snap.sampled_at).count();
    CHECK(sampled_age_ms > 30.0 && sampled_age_ms < 500.0);

    /* A key whose serial matches no group -> clean miss. */
    PlaybackClockSnapshot miss;
    CHECK(!client->WirelessClockSample(
        DeviceKey{ "v:n", "NOPE", "", "" }, miss));
    /* A group whose clock is invalid -> miss. */
    server.wireless_status_json =
        "{\"available\":true,\"groups\":[{\"mac\":\"W1\","
        "\"state\":\"holding\",\"rgb_clock\":{\"valid\":false}}]}";
    CHECK(!client->WirelessClockSample(key, miss));
    /* Unsupported backend -> miss. */
    server.wireless_status_json.clear();
    /* Re-fetch on the next call: unsupported latches, the
       miss is clean and the connection survives. */
    CHECK(!client->WirelessClockSample(key, miss));
    CHECK(client->State() == ConnectionState::Connected);
}

/*---------------------------------------------------------*\
||| Review fixes: a stalled backend's cached status is never |||
||| re-stamped as a fresh clock sample; a late reply to a    |||
||| timed-out probe is never read as the next probe's        |||
||| answer; a torn reply breaks the wire instead of          |||
||| desyncing it; a slow fork recovers from the latch;       |||
||| duplicate-key controllers do not re-enumerate forever.   |||
\*---------------------------------------------------------*/

static std::string StatusWithTicks(uint32_t ticks)
{
    return "{\"available\":true,\"groups\":[{\"mac\":\"W1\","
           "\"state\":\"holding\",\"rgb_clock\":{\"valid\":true,"
           "\"ticks\":" + std::to_string(ticks) +
           ",\"sampled_at_ms\":0,\"age_ms\":10}}]}";
}

static void TestStalledBackendSampleNotRestamped()
{
    FakeSdkServer server;
    server.controllers = { WirelessGroup() };
    server.wireless_status_json = StatusWithTicks(5000);
    auto client = MakeClient(server);
    CHECK(client->Connect());
    const DeviceKey key{ "Lian Li:Lian Li SL Wireless", "W1",
                         "hid:1", "" };

    PlaybackClockSnapshot s;
    CHECK(client->WirelessClockSample(key, s));
    CHECK(s.valid && s.ticks == 5000);

    /* The backend stops answering: the probe times out. The
       cache stays (diagnostics ages it via fetched_at) but the
       clock sample is a MISS — never the old ticks re-stamped
       as current. */
    server.wireless_status_delayed = true;
    PlaybackClockSnapshot stale;
    CHECK(!client->WirelessClockSample(key, stale));
    CHECK(!stale.valid);
    WirelessStatusSnapshot snap;
    client->WirelessStatus(snap);
    CHECK(snap.have_data && snap.supported);
    CHECK(client->State() == ConnectionState::Connected);
}

static void TestLateProbeReplyNotMisread()
{
    FakeSdkServer server;
    server.controllers = { WirelessGroup() };
    server.wireless_status_json = StatusWithTicks(5000);
    FakeStream* stream = nullptr;
    auto client = MakeClient(server, &stream);
    CHECK(client->Connect());
    const DeviceKey key{ "Lian Li:Lian Li SL Wireless", "W1",
                         "hid:1", "" };

    /* Probe A times out (its reply, ticks 1111, is late). */
    server.wireless_status_delayed = true;
    server.wireless_status_json    = StatusWithTicks(1111);
    PlaybackClockSnapshot s;
    CHECK(!client->WirelessClockSample(key, s));
    CHECK(server.delayed_wireless_replies.size() == 1);

    /* A's reply lands, then probe B is answered at once
       (ticks 2222). B must read ITS reply, not A's. */
    server.wireless_status_delayed = false;
    server.wireless_status_json    = StatusWithTicks(2222);
    stream->InjectServerBytes(server.delayed_wireless_replies[0]);
    CHECK(client->WirelessClockSample(key, s));
    CHECK(s.ticks == 2222);

    /* And the stream stays in sync for the next probe. */
    server.wireless_status_json = StatusWithTicks(3333);
    CHECK(client->WirelessClockSample(key, s));
    CHECK(s.ticks == 3333);

    /* A late reply that lands during a Request() reply wait is
       counted off there too. */
    server.wireless_status_delayed = true;
    server.wireless_status_json    = StatusWithTicks(4444);
    CHECK(!client->WirelessClockSample(key, s));
    CHECK(server.delayed_wireless_replies.size() == 2);
    stream->InjectServerBytes(server.delayed_wireless_replies[1]);
    server.wireless_status_delayed = false;
    server.wireless_status_json    = StatusWithTicks(5555);
    CHECK(client->RefreshStatus());
    CHECK(client->WirelessClockSample(key, s));
    CHECK(s.ticks == 5555);
    CHECK(client->State() == ConnectionState::Connected);
}

static void TestTornProbeReplyBreaksWire()
{
    /* Half a header, then silence: the probe must NOT treat
       this as a benign "unsupported" timeout — that would leave
       the torn bytes in the stream and desync every later
       reply. */
    {
        FakeSdkServer server;
        server.controllers = { WirelessGroup() };
        server.wireless_status_json = StatusWithTicks(1);
        FakeStream* stream = nullptr;
        auto client = MakeClient(server, &stream);
        CHECK(client->Connect());
        server.wireless_status_delayed = true;
        std::vector<uint8_t> pkt = EncodePacket(
            0, kPktLianLiWirelessStatus, { 1, 2, 3 });
        pkt.resize(8);
        stream->InjectServerBytes(pkt);
        CHECK(!client->FetchWirelessStatus());
        CHECK(client->State() == ConnectionState::Reconnecting);
    }
    /* Whole header, truncated body: same. */
    {
        FakeSdkServer server;
        server.controllers = { WirelessGroup() };
        server.wireless_status_json = StatusWithTicks(1);
        FakeStream* stream = nullptr;
        auto client = MakeClient(server, &stream);
        CHECK(client->Connect());
        server.wireless_status_delayed = true;
        std::vector<uint8_t> pkt = EncodePacket(
            0, kPktLianLiWirelessStatus,
            std::vector<uint8_t>(100, 'x'));
        pkt.resize(sizeof(PacketHeader) + 10);
        stream->InjectServerBytes(pkt);
        CHECK(!client->FetchWirelessStatus());
        CHECK(client->State() == ConnectionState::Reconnecting);
    }
}

static void TestSlowForkRecoversFromLatch()
{
    FakeSdkServer server;
    server.controllers = { WirelessGroup() };
    server.wireless_status_json    = StatusWithTicks(7);
    server.wireless_status_delayed = true;    /* slow first probe */
    FakeStream* stream = nullptr;
    auto client = MakeClient(server, &stream);
    CHECK(client->Connect());
    WirelessStatusSnapshot snap;
    client->WirelessStatus(snap);
    CHECK(snap.probed && !snap.supported);

    /* The reply arrives late; the next refresh sees it (in its
       count request), un-latches, and re-probes successfully. */
    CHECK(server.delayed_wireless_replies.size() == 1);
    stream->InjectServerBytes(server.delayed_wireless_replies[0]);
    server.wireless_status_delayed = false;
    CHECK(client->RefreshStatus());
    client->WirelessStatus(snap);
    CHECK(snap.supported && snap.have_data);
    CHECK(client->State() == ConnectionState::Connected);
}

static void TestDetectingReplyIsNotData()
{
    /* A backend still in first detection answers promptly with
       {"detecting":true,"available":false}: supported, no data
       yet, not a clock sample — and the next refresh asks
       again and picks up real data. */
    FakeSdkServer server;
    server.controllers = { WirelessGroup() };
    server.wireless_status_json =
        "{\"detecting\":true,\"available\":false}";
    auto client = MakeClient(server);
    CHECK(client->Connect());
    WirelessStatusSnapshot snap;
    client->WirelessStatus(snap);
    CHECK(snap.probed && snap.supported && !snap.have_data);
    const DeviceKey key{ "Lian Li:Lian Li SL Wireless", "W1",
                         "hid:1", "" };
    PlaybackClockSnapshot s;
    CHECK(!client->WirelessClockSample(key, s));
    CHECK(!s.valid);

    const size_t probes = server.CountEvents(kPktLianLiWirelessStatus);
    server.wireless_status_json = StatusWithTicks(9000);
    CHECK(client->RefreshStatus());
    CHECK(server.CountEvents(kPktLianLiWirelessStatus) > probes);
    client->WirelessStatus(snap);
    CHECK(snap.supported && snap.have_data);
    CHECK(client->WirelessClockSample(key, s));
    CHECK(s.ticks == 9000);
    CHECK(client->State() == ConnectionState::Connected);
}

static void TestUnsupportedReprobeSchedule()
{
    FakeSdkServer server;          /* stock: never answers */
    server.controllers = { WirelessGroup() };
    {
        auto client = MakeClient(server);
        CHECK(client->Connect());
        const size_t probes =
            server.CountEvents(kPktLianLiWirelessStatus);
        /* An explicit enumeration re-probes the latch. */
        CHECK(client->Enumerate());
        CHECK(server.CountEvents(kPktLianLiWirelessStatus) ==
              probes + 1);
        CHECK(client->State() == ConnectionState::Connected);
    }
    server.ClearEvents();
    {
        /* With the slow schedule elapsed (0 ms here) a refresh
           re-probes too. */
        OpenRgbClientOptions o = Opts();
        o.wireless_reprobe_ms = 0;
        auto client = std::make_unique<OpenRgbClient>(
            o, std::make_unique<FakeStream>(server));
        CHECK(client->Connect());
        const size_t probes =
            server.CountEvents(kPktLianLiWirelessStatus);
        CHECK(client->RefreshStatus());
        CHECK(server.CountEvents(kPktLianLiWirelessStatus) ==
              probes + 1);
    }
}

static void TestDuplicateKeysNoReenumeration()
{
    FakeSdkServer server;
    server.controllers = { WirelessGroup(), WirelessGroup(), Dimm() };
    auto client = MakeClient(server);
    CHECK(client->Connect());
    const std::vector<DeviceSnapshot> before = client->Snapshots();
    CHECK(before.size() == 2);            /* duplicate collapsed */
    CHECK(client->RefreshStatus());
    CHECK(client->RefreshStatus());
    const std::vector<DeviceSnapshot> after = client->Snapshots();
    CHECK(after.size() == 2);
    CHECK(!after.empty() && !before.empty() &&
          after[0].topology_generation ==
              before[0].topology_generation);

    /* A real count change still re-enumerates. */
    server.RemoveController(2);
    CHECK(client->RefreshStatus());
    const std::vector<DeviceSnapshot> removed = client->Snapshots();
    CHECK(removed.size() == 1);
    CHECK(!removed.empty() &&
          removed[0].topology_generation >
              before[0].topology_generation);
}

/*---------------------------------------------------------*\
||| Audit E-3: the 2 s topology refresh must not stall the  |||
||| frame path. RefreshStatus used to hold the wire lock    |||
||| across one round trip per controller, so every lane's   |||
||| SubmitLatest waited for the whole sweep; reconnect      |||
||| attempts also ran inside SubmitLatest (lane threads).   |||
\*---------------------------------------------------------*/

static FakeController NamedDimm(const std::string& serial)
{
    FakeController c = Dimm();
    c.serial   = serial;
    c.location = "i2c:" + serial;
    return c;
}

static void TestRefreshDoesNotStallFrames()
{
    FakeSdkServer server;
    server.controllers = { WirelessGroup(), NamedDimm("D1"),
                           NamedDimm("D2"), NamedDimm("D3"),
                           NamedDimm("D4") };

    FakeStream* stream = nullptr;
    auto client = MakeClient(server, &stream);
    CHECK(client->Connect());
    const std::vector<DeviceSnapshot> snaps = client->Snapshots();
    CHECK(snaps.size() == 5);

    /* 60 ms per receive: count + 5 controller replies, two
       receives each — ~720 ms of status traffic. Sends (the
       frame path) are never delayed by the fake. */
    stream->recv_delay_ms.store(60);

    std::atomic<bool> refresh_running{ false };
    std::atomic<bool> refresh_done{ false };
    std::atomic<bool> refresh_ok{ false };
    std::thread th([&]
    {
        refresh_running = true;
        refresh_ok      = client->RefreshStatus();
        refresh_done    = true;
    });
    while(!refresh_running.load())
    {
        std::this_thread::yield();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(30));

    /* Frames keep flowing while the refresh runs: each send
       may wait for at most one status round trip (~120 ms),
       never for the whole sweep. */
    double worst_ms = 0.0;
    int    sent_during = 0;
    while(!refresh_done.load())
    {
        const auto t0 = Clock::now();
        const SubmitResult r =
            client->SubmitLatest(MakeFrame(snaps[1], { 1, 2, 3 }));
        const double ms = std::chrono::duration<double, std::milli>(
            Clock::now() - t0).count();
        CHECK(r.status == SubmitStatus::Accepted);
        if(!refresh_done.load())
        {
            ++sent_during;
        }
        if(ms > worst_ms)
        {
            worst_ms = ms;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    th.join();
    stream->recv_delay_ms.store(0);
    std::printf("refresh stall: worst send wait %.0f ms, %d sends "
                "during the refresh\n", worst_ms, sent_during);
    CHECK(refresh_ok.load());
    CHECK(worst_ms < 350.0);
    CHECK(sent_during >= 3);

    /* Still an in-place refresh: generation unchanged. */
    const std::vector<DeviceSnapshot> after = client->Snapshots();
    CHECK(after.size() == 5);
    CHECK(!after.empty() && after[0].topology_generation == 1);
}

/* Reconnect attempts belong to the owner loop (Poll), never
   to the frame path: a SubmitLatest while Reconnecting is
   refused at once, without a connect attempt. */
static void TestSubmitNeverReconnects()
{
    FakeSdkServer server;
    server.controllers = { WirelessGroup() };

    FakeStream* stream = nullptr;
    auto client = MakeClient(server, &stream);
    CHECK(client->Connect());
    const DeviceSnapshot snap = client->Snapshots()[0];

    stream->SetSendFails(true);
    CHECK(client->SubmitLatest(MakeFrame(snap, { 1, 0, 0 })).status ==
          SubmitStatus::Rejected);
    CHECK(client->State() == ConnectionState::Reconnecting);
    stream->SetSendFails(false);
    server.listening = false;

    for(int i = 0; i < 4; ++i)
    {
        CHECK(client->SubmitLatest(MakeFrame(snap, { 1, 0, 0 })).status ==
              SubmitStatus::Rejected);
    }
    /* No attempt was made from the send path. */
    CHECK(client->ReconnectAttempt() == 0);
    CHECK(client->State() == ConnectionState::Reconnecting);

    /* The owner loop's Poll drives the schedule. */
    CHECK(!client->Poll());
    CHECK(client->ReconnectAttempt() == 1);
    server.listening = true;
    CHECK(client->Poll());
    CHECK(client->State() == ConnectionState::Connected);
}

/*---------------------------------------------------------*\
||| One controller whose description cannot be parsed is   ||
||| skipped -- the rest of the rig still enumerates and    ||
||| keeps its enumeration indices (audit P-14). Only a     ||
||| backend where EVERY description fails is an error.     ||
\*---------------------------------------------------------*/

static void TestUnparsableControllerSkipped()
{
    FakeSdkServer server;
    FakeController bad = Dimm();
    bad.serial         = "BAD";
    bad.malformed_body = true;
    server.controllers = { WirelessGroup(), bad, Keyboard() };

    auto client = MakeClient(server);
    CHECK(client->Connect());
    CHECK(client->State() == ConnectionState::Connected);
    CHECK(client->Snapshots().size() == 2);

    const ConnectionSnapshotInfo info = client->ConnectionSnapshot();
    CHECK(info.skipped_controllers.size() == 1);
    CHECK(!info.skipped_controllers.empty() &&
          info.skipped_controllers[0] == 1);

    /* The keyboard after the bad entry keeps its server index:
       its frames go to dev_id 2, not 1. */
    DeviceSnapshot k;
    CHECK(client->Snapshot(
        DeviceKey{ "Corsair:Corsair K95", "K1", "hid:k1", "" }, k));
    server.events.clear();
    RgbFrame frame = MakeFrame(k, { 1, 2, 3 });
    CHECK(client->SubmitLatest(frame).status == SubmitStatus::Accepted);
    const ServerEvent* ev = FindEvent(server.events, kPktUpdateLEDs);
    CHECK(ev != nullptr && ev->dev_id == 2);

    /* The periodic refresh neither fails nor re-enumerates
       (the server count still matches). */
    const uint64_t gen = k.topology_generation;
    CHECK(client->RefreshStatus());
    DeviceSnapshot k2;
    CHECK(client->Snapshot(k.key, k2) && k2.topology_generation == gen);

    /* Every description unparsable: a protocol-level problem,
       not one odd device -- the connection still fails. */
    FakeSdkServer all_bad;
    all_bad.controllers = { bad };
    auto client2 = MakeClient(all_bad);
    CHECK(!client2->Connect());
    CHECK(client2->LastError().find("malformed controller data")
          != std::string::npos);
}

/*---------------------------------------------------------*\
||| Audit P-5: the backend announces a changed controller  |||
||| list with DEVICE_LIST_UPDATED (packet 100) to every     |||
||| client, at any protocol version. The client must read  |||
||| it promptly: until it re-enumerates, a frame resolved  |||
||| against the old positional index would land on a       |||
||| shifted controller.                                    |||
\*---------------------------------------------------------*/

static void TestDeviceListUpdatedInvalidatesTopology()
{
    FakeSdkServer server;
    server.controllers = { WirelessGroup(), Dimm() };

    FakeStream* stream = nullptr;
    auto client = MakeClient(server, &stream);
    CHECK(client->Connect());
    std::vector<DeviceSnapshot> snaps = client->Snapshots();
    CHECK(snaps.size() == 2);
    const DeviceSnapshot* dimm = nullptr;
    for(const DeviceSnapshot& s : snaps)
    {
        if(s.key.serial == "D1") dimm = &s;
    }
    CHECK(dimm != nullptr);
    if(!dimm) return;
    const uint64_t gen0 = dimm->topology_generation;
    const RgbFrame stale = MakeFrame(*dimm, { 5, 5, 5 });

    /* The backend rescans: the wireless group is gone, the
       DIMM moves from index 1 to index 0, and the server
       tells every client. */
    server.RemoveController(0);
    stream->InjectServerBytes(EncodePacket(0, kPktDeviceListUpdated, {}));
    server.ClearEvents();

    /* The very next frame on the old topology is refused —
       never sent to the stale index 1. */
    const SubmitResult r = client->SubmitLatest(stale);
    CHECK(r.status == SubmitStatus::Rejected);
    CHECK(server.CountEvents(kPktUpdateLEDs) == 0);
    CHECK(client->State() == ConnectionState::Connected);

    /* The owner loop's Poll re-enumerates: one controller, a
       new generation, and frames route to the new index. */
    CHECK(client->Poll());
    snaps = client->Snapshots();
    CHECK(snaps.size() == 1);
    CHECK(!snaps.empty() && snaps[0].key.serial == "D1");
    CHECK(!snaps.empty() && snaps[0].topology_generation > gen0);
    if(!snaps.empty())
    {
        CHECK(client->SubmitLatest(MakeFrame(snaps[0], { 6, 6, 6 }))
                  .status == SubmitStatus::Accepted);
        CHECK(server.CountEvents(kPktUpdateLEDs, 0) == 1);
        CHECK(server.CountEvents(kPktUpdateLEDs, 1) == 0);
    }

    /* Read by Poll alone too (no frame in between). */
    server.AddController(WirelessGroup());
    stream->InjectServerBytes(EncodePacket(0, kPktDeviceListUpdated, {}));
    CHECK(client->Poll());
    CHECK(client->Snapshots().size() == 2);
}

/*---------------------------------------------------------*\
||| Audit P-13 step 2 — unsolicited server packets.         |||
|||                                                         |||
||| Request() drops up to 64 unrelated packets while it     |||
||| waits for its reply; DrainPending() consumes whatever   |||
||| is readable between operations. These pin today's      |||
||| behaviour (P-5 already landed: a DEVICE_LIST_UPDATED    |||
||| read anywhere bumps the topology generation).           |||
\*---------------------------------------------------------*/

/* n unrelated packets back to back (an id the client never
   requests). */
static std::vector<uint8_t> JunkPackets(unsigned n)
{
    std::vector<uint8_t> bytes;
    for(unsigned i = 0; i < n; ++i)
    {
        const std::vector<uint8_t> p =
            EncodePacket(0, 7777, { (uint8_t)i, 0xAA, 0x55 });
        bytes.insert(bytes.end(), p.begin(), p.end());
    }
    return bytes;
}

/* Nothing left unread in the fake stream. */
static bool StreamDrained(FakeStream& stream)
{
    bool        readable = true;
    std::string err;
    return stream.WaitReadable(0, readable, err) && !readable;
}

static void TestDeviceListUpdatedBeforeReply()
{
    FakeSdkServer server;
    server.controllers = { WirelessGroup(), Dimm() };
    FakeStream* stream = nullptr;
    auto client = MakeClient(server, &stream);
    CHECK(client->Connect());
    std::vector<DeviceSnapshot> snaps = client->Snapshots();
    CHECK(snaps.size() == 2);
    if(snaps.empty()) return;
    const uint64_t gen0  = snaps[0].topology_generation;
    const RgbFrame stale = MakeFrame(snaps[0], { 1, 1, 1 });

    /* The notice arrives ahead of the controller-count reply of
       a status refresh: Request() skips it as unrelated, still
       matches its own reply, and the read flags the topology. */
    stream->InjectServerBytes(EncodePacket(0, kPktDeviceListUpdated, {}));
    server.ClearEvents();
    CHECK(client->RefreshStatus());
    CHECK(client->State() == ConnectionState::Connected);
    /* Every byte consumed: the notice and the count reply (the
       fake records no events for count/data requests, so the
       drained stream is the evidence the reply was matched). */
    CHECK(StreamDrained(*stream));

    /* Generation bumped: a frame on the old topology is
       refused, never sent to a possibly shifted index. */
    CHECK(client->SubmitLatest(stale).status == SubmitStatus::Rejected);
    CHECK(server.CountEvents(kPktUpdateLEDs) == 0);

    /* Poll re-enumerates onto a newer generation. */
    CHECK(client->Poll());
    snaps = client->Snapshots();
    CHECK(snaps.size() == 2);
    if(snaps.empty()) return;
    CHECK(snaps[0].topology_generation > gen0);
    CHECK(client->SubmitLatest(MakeFrame(snaps[0], { 2, 2, 2 })).status ==
          SubmitStatus::Accepted);
}

static void TestUnrelatedPacketFloodHitsBound()
{
    /* 63 unrelated packets + the reply = 64 reads: inside the
       bound. The first junk packet is a controller-count reply
       for the WRONG dev_id (claiming 99 controllers) — a match
       on pkt_id alone would misread it, re-enumerate, and leave
       the real reply in the stream. */
    {
        FakeSdkServer server;
        server.controllers = { WirelessGroup(), Dimm() };
        FakeStream* stream = nullptr;
        auto client = MakeClient(server, &stream);
        CHECK(client->Connect());
        std::vector<uint8_t> bytes =
            EncodePacket(5, kPktControllerCount, { 99, 0, 0, 0 });
        const std::vector<uint8_t> junk = JunkPackets(62);
        bytes.insert(bytes.end(), junk.begin(), junk.end());
        stream->InjectServerBytes(bytes);
        CHECK(client->RefreshStatus());
        CHECK(client->State() == ConnectionState::Connected);
        CHECK(StreamDrained(*stream));
        CHECK(client->Snapshots().size() == 2);
    }
    /* 64 unrelated packets: the reply would be read 65th — the
       bound throws and the connection is torn down (a flooding
       peer cannot hold the wire lock forever). */
    {
        FakeSdkServer server;
        server.controllers = { WirelessGroup(), Dimm() };
        FakeStream* stream = nullptr;
        auto client = MakeClient(server, &stream);
        CHECK(client->Connect());
        stream->InjectServerBytes(JunkPackets(64));
        CHECK(!client->RefreshStatus());
        CHECK(client->State() == ConnectionState::Reconnecting);
        CHECK(client->LastError().find("too many unrelated packets") !=
              std::string::npos);
    }
}

static void TestUnsolicitedBetweenFramesIsDrained()
{
    FakeSdkServer server;
    server.controllers = { WirelessGroup(), Dimm() };
    FakeStream* stream = nullptr;
    auto client = MakeClient(server, &stream);
    CHECK(client->Connect());
    const std::vector<DeviceSnapshot> snaps = client->Snapshots();
    CHECK(!snaps.empty());
    if(snaps.empty()) return;
    server.ClearEvents();

    CHECK(client->SubmitLatest(MakeFrame(snaps[0], { 1, 0, 0 })).status ==
          SubmitStatus::Accepted);

    /* One unrelated packet between two frames: the next send
       path drains it (ReadAsyncIo -> DrainPending) and, being
       no topology notice, it costs the frame nothing. */
    stream->InjectServerBytes(JunkPackets(1));
    CHECK(!StreamDrained(*stream));
    CHECK(client->SubmitLatest(MakeFrame(snaps[0], { 0, 1, 0 })).status ==
          SubmitStatus::Accepted);
    CHECK(StreamDrained(*stream));
    CHECK(server.CountEvents(kPktUpdateLEDs) == 2);

    /* Several at once, drained by the owner loop's Poll. */
    stream->InjectServerBytes(JunkPackets(3));
    CHECK(client->Poll());
    CHECK(StreamDrained(*stream));
    CHECK(client->State() == ConnectionState::Connected);

    /* ...and by a mode command's trailing DrainPending. */
    stream->InjectServerBytes(JunkPackets(2));
    CHECK(client->SetCustomMode(snaps[0].key));
    CHECK(StreamDrained(*stream));

    /* The generation never moved: the original snapshot's
       frames are still current. */
    CHECK(client->SubmitLatest(MakeFrame(snaps[0], { 0, 0, 1 })).status ==
          SubmitStatus::Accepted);
    CHECK(client->State() == ConnectionState::Connected);
}

static void TestTornHeaderInDrainIsMidPacket()
{
    /* Half a header and silence, read by Poll's drain. */
    {
        FakeSdkServer server;
        server.controllers = { WirelessGroup() };
        FakeStream* stream = nullptr;
        auto client = MakeClient(server, &stream);
        CHECK(client->Connect());
        std::vector<uint8_t> torn =
            EncodePacket(0, kPktDeviceListUpdated, {});
        torn.resize(8);
        stream->InjectServerBytes(torn);
        CHECK(!client->Poll());
        CHECK(client->State() == ConnectionState::Reconnecting);
        CHECK(client->LastError().find("mid-packet") != std::string::npos);
    }
    /* The same on the frame send path: the frame is refused. */
    {
        FakeSdkServer server;
        server.controllers = { WirelessGroup() };
        FakeStream* stream = nullptr;
        auto client = MakeClient(server, &stream);
        CHECK(client->Connect());
        const std::vector<DeviceSnapshot> snaps = client->Snapshots();
        CHECK(!snaps.empty());
        if(snaps.empty()) return;
        std::vector<uint8_t> torn = JunkPackets(1);
        torn.resize(5);
        stream->InjectServerBytes(torn);
        server.ClearEvents();
        CHECK(client->SubmitLatest(MakeFrame(snaps[0], { 3, 3, 3 })).status ==
              SubmitStatus::Rejected);
        CHECK(server.CountEvents(kPktUpdateLEDs) == 0);
        CHECK(client->State() == ConnectionState::Reconnecting);
        CHECK(client->LastError().find("mid-packet") != std::string::npos);
    }
}

int main()
{
    TestHandshakeCapsAt4();
    TestUnsupportedVersionRejected();
    TestUnreachableBackend();
    TestEnumeration();
    TestEnumerationMatrixAndLedNames();
    TestCompleteControllerWrite();
    TestSubmitValidation();
    TestModeSelection();
    TestResizeZone();
    TestDisconnectedSubmit();
    TestMalformedServerReply();
    TestGettersNeverBlockBehindWireIo();
    TestWirelessStatusFetch();
    TestWirelessStatusUnsupportedBackend();
    TestWirelessStatusMalformedReply();
    TestWirelessClockSample();
    TestStalledBackendSampleNotRestamped();
    TestLateProbeReplyNotMisread();
    TestTornProbeReplyBreaksWire();
    TestSlowForkRecoversFromLatch();
    TestUnsupportedReprobeSchedule();
    TestDetectingReplyIsNotData();
    TestDuplicateKeysNoReenumeration();
    TestRefreshDoesNotStallFrames();
    TestSubmitNeverReconnects();
    TestUnparsableControllerSkipped();
    TestDeviceListUpdatedInvalidatesTopology();
    TestDeviceListUpdatedBeforeReply();
    TestUnrelatedPacketFloodHitsBound();
    TestUnsolicitedBetweenFramesIsDrained();
    TestTornHeaderInDrainIsMidPacket();
    std::printf("sdk client: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
