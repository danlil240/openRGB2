// SPDX-License-Identifier: GPL-2.0-or-later
//
// Wire-layer tests for the OpenRGB SDK client
// (src/backends/openrgb/SdkWire.h): the bounded incremental
// packet parser, request builders, and controller-data
// decoding. Pure parsing — no sockets, no hardware.

#include "SdkWire.h"
#include "fake_sdk_server.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace openrgb2::sdk;
using sdk_test::FakeSdkServer;
using sdk_test::FakeController;
using sdk_test::FakeMode;
using sdk_test::FakeZone;

static int checks   = 0;
static int failures = 0;
#define CHECK(x) do { ++checks; if(!(x)) { ++failures; \
    std::printf("FAIL line %d: %s\n", __LINE__, #x); } } while(0)

static std::vector<uint8_t> Pkt(uint32_t dev, uint32_t id,
                                std::vector<uint8_t> body)
{
    return EncodePacket(dev, id, body);
}

/*---------------------------------------------------------*\
||| Fragmented / coalesced packet handling                  ||
\*---------------------------------------------------------*/

static void TestParserByteWise()
{
    PacketParser p;
    const std::vector<uint8_t> bytes =
        Pkt(3, kPktUpdateLEDs, { 0xAA, 0xBB, 0xCC });

    /* Feed one byte at a time: nothing over-read, one packet. */
    std::vector<Packet> out;
    for(uint8_t b : bytes)
    {
        CHECK(p.Feed(&b, 1, out));
    }
    CHECK(p.Ok());
    CHECK(out.size() == 1);
    CHECK(out[0].header.dev_id == 3);
    CHECK(out[0].header.pkt_id == kPktUpdateLEDs);
    CHECK(out[0].body.size() == 3);
    CHECK(out[0].body[0] == 0xAA && out[0].body[2] == 0xCC);
    CHECK(p.Buffered() == 0);
}

static void TestParserCoalesced()
{
    PacketParser p;
    std::vector<uint8_t> bytes = Pkt(0, 40, { 1, 0, 0, 0 });
    const std::vector<uint8_t> second = Pkt(1, 0, {});
    bytes.insert(bytes.end(), second.begin(), second.end());
    const std::vector<uint8_t> third = Pkt(7, 1051, { 9, 9 });
    bytes.insert(bytes.end(), third.begin(), third.end());

    /* Three packets arrive in ONE read (coalesced by TCP). */
    std::vector<Packet> out;
    CHECK(p.Feed(bytes.data(), bytes.size(), out));
    CHECK(out.size() == 3);
    CHECK(out[0].header.pkt_id == 40 && out[0].body.size() == 4);
    CHECK(out[1].header.pkt_id == 0 && out[1].body.empty());
    CHECK(out[2].header.dev_id == 7 && out[2].body.size() == 2);
}

static void TestParserArbitraryClumps()
{
    PacketParser p;
    const std::vector<uint8_t> bytes =
        Pkt(2, kPktUpdateMode,
            std::vector<uint8_t>(300, 0x5A));   /* body > one clump */

    /* Split at odd boundaries: 7, 16, 100, rest. */
    const size_t splits[] = { 7, 16, 100 };
    size_t               off = 0;
    std::vector<Packet>  out;
    for(size_t n : splits)
    {
        CHECK(p.Feed(bytes.data() + off, n, out));
        CHECK(out.empty());                   /* nothing complete yet */
        off += n;
    }
    CHECK(p.Feed(bytes.data() + off, bytes.size() - off, out));
    CHECK(out.size() == 1);
    CHECK(out[0].body.size() == 300);
}

/*---------------------------------------------------------*\
||| Malformed input: bounded parsing, no over-read, clean   ||
||| error state                                             ||
\*---------------------------------------------------------*/

static void TestParserBadMagic()
{
    PacketParser p;
    std::vector<uint8_t> bytes = Pkt(0, 0, {});
    bytes[0] = 'X';                           /* corrupt magic */

    std::vector<Packet> out;
    CHECK(!p.Feed(bytes.data(), bytes.size(), out));
    CHECK(!p.Ok());
    CHECK(!p.Error().empty());
    /* Poisoned until Reset(). */
    CHECK(!p.Feed(bytes.data(), bytes.size(), out));
    p.Reset();
    CHECK(p.Ok());
}

static void TestParserOversizedBody()
{
    PacketParser p;
    /* Declare a body above the 8 MiB bound — the parser must
       reject WITHOUT waiting for the bytes. */
    std::vector<uint8_t> bytes = Pkt(0, 1, {});
    const uint32_t huge = kMaxPacketSize + 1;
    memcpy(bytes.data() + 12, &huge, 4);

    std::vector<Packet> out;
    CHECK(!p.Feed(bytes.data(), bytes.size(), out));
    CHECK(!p.Ok());
    CHECK(out.empty());
}

static void TestParserTruncatedWaits()
{
    PacketParser p;
    const std::vector<uint8_t> bytes = Pkt(0, 0, { 1, 2, 3, 4 });

    /* A truncated header is not an error — it waits. */
    std::vector<Packet> out;
    CHECK(p.Feed(bytes.data(), 5, out));
    CHECK(p.Ok() && out.empty());
    /* Truncated body likewise waits. */
    CHECK(p.Feed(bytes.data() + 5, kHeaderSize - 5 + 2, out));
    CHECK(p.Ok() && out.empty());
    CHECK(p.Feed(bytes.data() + kHeaderSize + 2,
                 bytes.size() - kHeaderSize - 2, out));
    CHECK(out.size() == 1);
}

/*---------------------------------------------------------*\
||| Controller-data decoding                                ||
\*---------------------------------------------------------*/

static FakeController WirelessFixture()
{
    FakeController c;
    c.name        = "Lian Li SL Wireless";
    c.vendor      = "Lian Li";
    c.serial      = "W1";
    c.location    = "hid:1";
    c.active_mode = 1;

    FakeMode direct;   direct.name  = "Direct";        direct.speed = 50;
    FakeMode spectrum; spectrum.name = "Spectrum Cycle"; spectrum.speed = 100;
    spectrum.colors = { 0xFF0000, 0x00FF00 };
    FakeMode breath;   breath.name   = "Breathing";    breath.speed = 20;
    c.modes = { direct, spectrum, breath };

    c.zones = { { "fan-1", 0, 0, 40, 40 },
                { "fan-2", 0, 0, 40, 40 },
                { "fan-3", 0, 0, 40, 40 } };
    for(uint32_t i = 0; i < 120; ++i)
    {
        c.led_names.push_back("LED " + std::to_string(i));
        c.colors.push_back(0x00010203);
    }
    return c;
}

static void TestControllerDataRoundTrip()
{
    const FakeController fc = WirelessFixture();
    const std::vector<uint8_t> body =
        FakeSdkServer::BuildControllerBody(fc);

    SdkController c;
    CHECK(ParseControllerData(body.data(), body.size(), 4, c));
    CHECK(c.name == "Lian Li SL Wireless");
    CHECK(c.vendor == "Lian Li");
    CHECK(c.serial == "W1");
    CHECK(c.location == "hid:1");
    CHECK(c.active_mode == 1);
    CHECK(c.modes.size() == 3);
    CHECK(c.modes[1].name == "Spectrum Cycle");
    CHECK(c.modes[1].speed == 100);
    CHECK(c.modes[1].raw.size() > 0);
    CHECK(c.zones.size() == 3);
    CHECK(c.zones[0].name == "fan-1");
    CHECK(c.zones[2].leds_count == 40);
    CHECK(c.num_leds == 120);
    CHECK(c.colors.size() == 120);
    CHECK(c.colors[0] == 0x00010203u);
}

static void TestControllerDataTruncations()
{
    const std::vector<uint8_t> body =
        FakeSdkServer::BuildControllerBody(WirelessFixture());

    /* EVERY strict prefix of a valid body must fail cleanly —
       bounded parsing, never an over-read. */
    for(size_t len = 0; len < body.size(); ++len)
    {
        SdkController c;
        if(ParseControllerData(body.data(), len, 4, c))
        {
            CHECK(false);
            std::printf("  truncated parse accepted at %zu/%zu\n",
                        len, body.size());
            break;
        }
    }
    /* Trailing garbage is rejected (size field covers the
       whole body). */
    std::vector<uint8_t> padded = body;
    padded.push_back(0xEE);
    SdkController c;
    CHECK(!ParseControllerData(padded.data(), padded.size(), 4, c));
}

static void TestControllerDataMalformedCounts()
{
    std::vector<uint8_t> body =
        FakeSdkServer::BuildControllerBody(WirelessFixture());

    /* Inflate the mode count beyond what the body holds. */
    /* Layout: u32 size, u32 type, then 6 counted strings —
       find num_modes by scanning: strings each start with a
       u16 length. Compute offset. */
    size_t off = 8;
    for(int i = 0; i < 6; ++i)
    {
        uint16_t slen = 0;
        memcpy(&slen, body.data() + off, 2);
        off += 2 + slen;
    }
    /* off now points at u16 num_modes. */
    const uint16_t insane = 0xFFFF;
    memcpy(body.data() + off, &insane, 2);
    SdkController c;
    CHECK(!ParseControllerData(body.data(), body.size(), 4, c));
}

/*---------------------------------------------------------*\
||| Builders                                                ||
\*---------------------------------------------------------*/

static void TestBuilders()
{
    /* UpdateLEDs body: u32 size == body size, u16 count,
       colors 0x00BBGGRR. */
    const std::vector<uint32_t> colors = { 0x00112233, 0x00A0B0C0 };
    const std::vector<uint8_t> body = BuildUpdateLEDsBody(colors);
    CHECK(body.size() == 4 + 2 + 2 * 4);
    uint32_t size_field = 0;
    memcpy(&size_field, body.data(), 4);
    CHECK(size_field == body.size());
    uint16_t count = 0;
    memcpy(&count, body.data() + 4, 2);
    CHECK(count == 2);
    uint32_t c1 = 0;
    memcpy(&c1, body.data() + 6, 4);
    CHECK(c1 == 0x00112233u);

    /* ResizeZone: exactly { i32 zone_idx, i32 new_size }. */
    const std::vector<uint8_t> rz = BuildResizeZoneBody(2, 48);
    CHECK(rz.size() == 8);
    int32_t zi = -1, ns = -1;
    memcpy(&zi, rz.data(), 4);
    memcpy(&ns, rz.data() + 4, 4);
    CHECK(zi == 2 && ns == 48);

    /* UpdateMode: u32 size + i32 index + raw blob. */
    const std::vector<uint8_t> raw = { 1, 2, 3, 4, 5 };
    const std::vector<uint8_t> um = BuildUpdateModeBody(7, raw);
    CHECK(um.size() == 4 + 4 + 5);
    memcpy(&size_field, um.data(), 4);
    CHECK(size_field == um.size());
    int32_t idx = -1;
    memcpy(&idx, um.data() + 4, 4);
    CHECK(idx == 7);
    CHECK(memcmp(um.data() + 8, raw.data(), 5) == 0);
}

static void TestPatchModeSpeed()
{
    /* Parse a real body, patch speed in the captured raw mode
       blob, verify through the same parser path. */
    const std::vector<uint8_t> body =
        FakeSdkServer::BuildControllerBody(WirelessFixture());
    SdkController c;
    CHECK(ParseControllerData(body.data(), body.size(), 4, c));

    std::vector<uint8_t> raw = c.modes[0].raw;      /* "Direct" */
    CHECK(PatchModeSpeed(raw, 4, 123));
    /* Re-parse the patched blob as a standalone mode to prove
       the correct field was patched. The raw layout starts
       with name_len + name. */
    uint16_t name_len = 0;
    memcpy(&name_len, raw.data(), 2);
    const size_t speed_off = 2 + name_len + 16 + 8 + 8;
    uint32_t speed = 0;
    memcpy(&speed, raw.data() + speed_off, 4);
    CHECK(speed == 123);

    std::vector<uint8_t> bad = { 1 };
    CHECK(!PatchModeSpeed(bad, 4, 1));
}

int main()
{
    TestParserByteWise();
    TestParserCoalesced();
    TestParserArbitraryClumps();
    TestParserBadMagic();
    TestParserOversizedBody();
    TestParserTruncatedWaits();
    TestControllerDataRoundTrip();
    TestControllerDataTruncations();
    TestControllerDataMalformedCounts();
    TestBuilders();
    TestPatchModeSpeed();
    std::printf("sdk wire: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
