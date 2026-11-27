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

static FakeController KeyboardFixture()
{
    FakeController c;
    c.name        = "Corsair K95";
    c.vendor      = "Corsair";
    c.serial      = "K1";
    c.location    = "hid:k1";
    c.active_mode = 0;

    FakeMode direct; direct.name = "Direct"; direct.speed = 50;
    c.modes = { direct };

    FakeZone kb;
    kb.name          = "Keyboard";
    kb.type          = 2;               /* ZONE_TYPE_MATRIX */
    kb.leds_min      = 6;
    kb.leds_max      = 6;
    kb.leds_count    = 6;
    kb.matrix_rows   = 2;
    kb.matrix_cols   = 3;
    kb.matrix_map    = { 0, 1, 2,
                         3, 0xFFFFFFFFu, 5 };   /* one empty cell */
    c.zones = { kb };

    const char* names[] = { "Key: Escape", "Key: A", "Key: S",
                            "Key: D", "Key: F", "Key: G" };
    for(const char* n : names)
    {
        c.led_names.push_back(n);
        c.colors.push_back(0x00000000);
    }
    return c;
}

static void TestControllerMatrixRoundTrip()
{
    const FakeController fc = KeyboardFixture();
    const std::vector<uint8_t> body =
        FakeSdkServer::BuildControllerBody(fc);

    SdkController c;
    CHECK(ParseControllerData(body.data(), body.size(), 4, c));
    CHECK(c.zones.size() == 1);
    CHECK(c.zones[0].name == "Keyboard");
    CHECK(c.zones[0].matrix_rows == 2);
    CHECK(c.zones[0].matrix_cols == 3);
    CHECK(c.zones[0].matrix_map.size() == 6);
    CHECK(c.zones[0].matrix_map[0] == 0);
    CHECK(c.zones[0].matrix_map[4] == 0xFFFFFFFFu);  /* empty cell */
    /* Controller LED names land on the controller struct. */
    CHECK(c.led_names.size() == 6);
    CHECK(c.led_names[0] == "Key: Escape");
    CHECK(c.led_names[5] == "Key: G");
}

static void TestControllerNoMatrixRoundTrip()
{
    /* A zone with no matrix parses to an empty map. */
    const std::vector<uint8_t> body =
        FakeSdkServer::BuildControllerBody(WirelessFixture());
    SdkController c;
    CHECK(ParseControllerData(body.data(), body.size(), 4, c));
    CHECK(c.zones.size() == 3);
    CHECK(c.zones[0].matrix_map.empty());
    CHECK(c.zones[0].matrix_rows == 0);
    CHECK(c.zones[0].matrix_cols == 0);
    CHECK(c.led_names.size() == 120);
    CHECK(c.led_names[119] == "LED 119");
}

/* Offset of the zone matrix-map-size field inside a body
   built from KeyboardFixture: u32 size, u32 type, six strings,
   u16 num_modes, u32 active_mode, then modes, u16 num_zones,
   then zone: string + 4 u32s -> the u16 that follows. */
static size_t MatrixSizeOffset(const std::vector<uint8_t>& body)
{
    size_t off = 8;
    for(int i = 0; i < 6; ++i)
    {
        uint16_t slen = 0;
        memcpy(&slen, body.data() + off, 2);
        off += 2 + slen;
    }
    uint16_t num_modes = 0;
    memcpy(&num_modes, body.data() + off, 2);
    off += 2 + 4;                              /* count + active */
    for(uint16_t i = 0; i < num_modes; ++i)
    {
        uint16_t slen = 0;
        memcpy(&slen, body.data() + off, 2);
        off += 2 + slen;                       /* mode name */
        off += 12 * 4;                         /* 12 u32 fields */
        uint16_t ncolors = 0;
        memcpy(&ncolors, body.data() + off, 2);
        off += 2 + 4ull * ncolors;             /* colors */
    }
    uint16_t num_zones = 0;
    memcpy(&num_zones, body.data() + off, 2);
    off += 2;
    /* First zone: name + 4 u32 fields -> matrix size field. */
    uint16_t zlen = 0;
    memcpy(&zlen, body.data() + off, 2);
    off += 2 + zlen + 4 * 4;
    return off;
}

static void TestControllerMatrixMalformed()
{
    const std::vector<uint8_t> good =
        FakeSdkServer::BuildControllerBody(KeyboardFixture());
    const size_t moff = MatrixSizeOffset(good);

    /* matrix_map_size nonzero but smaller than the 8-byte
       header -> reject. */
    {
        std::vector<uint8_t> b = good;
        const uint16_t bad = 4;
        memcpy(b.data() + moff, &bad, 2);
        SdkController c;
        CHECK(!ParseControllerData(b.data(), b.size(), 4, c));
    }
    /* Declared cell count larger than the serialized blob:
       bump rows without adding cells -> reject. */
    {
        std::vector<uint8_t> b = good;
        const uint32_t huge = 1000;
        memcpy(b.data() + moff + 2, &huge, 4);
        SdkController c;
        CHECK(!ParseControllerData(b.data(), b.size(), 4, c));
    }
    /* Zero-dimension matrix serialized as a non-empty blob
       -> reject. */
    {
        std::vector<uint8_t> b = good;
        const uint16_t sz = 8;
        memcpy(b.data() + moff, &sz, 2);
        const uint32_t zero = 0;
        memcpy(b.data() + moff + 2, &zero, 4);   /* rows = 0 */
        /* blob says 8 bytes but zone body still carries the
           old cells; shrink the size field leaves garbage
           trailing -> must reject either way */
        SdkController c;
        CHECK(!ParseControllerData(b.data(), b.size(), 4, c));
    }
    /* Truncated mid-matrix: every strict prefix must fail —
       reuse the exhaustive prefix sweep on the keyboard body. */
    for(size_t len = MatrixSizeOffset(good);
        len < good.size() && len < MatrixSizeOffset(good) + 64;
        ++len)
    {
        SdkController c;
        CHECK(!ParseControllerData(good.data(), len, 4, c));
    }
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

/*---------------------------------------------------------*\
|| Vendor extension: LIANLI_WIRELESS_STATUS reply parsing   ||
|| (N01 observability — the wedged group must surface).    ||
\*---------------------------------------------------------*/

static void TestWirelessStatusParse()
{
    /* The real backend answers pkt 4000 with a null-
       terminated JSON body (SETTINGS convention). */
    const char* body =
        "{\"available\":true,\"master\":\"FA:C0:87:E5:66:E4\","
        "\"groups\":[{\"mac\":\"9B:10:76:E5:66:E1\",\"state\":\"failed\","
        "\"wedged\":true,\"confirmed\":false,\"resends\":8,"
        "\"send_failures\":2,\"last_error\":\"resend budget exhausted\","
        "\"fan_type\":\"SL V3\",\"fan_count\":3,"
        "\"sighting\":{\"master\":\"FA:C0:87:E5:66:E4\",\"fan_count\":3},"
        "\"upload\":{\"frame_count\":4096,\"compressed_bytes\":9000},"
        "\"rgb_clock\":{\"valid\":true,\"ticks\":123456,"
        "\"sampled_at_ms\":777,\"age_ms\":42}}],"
        "\"counters\":{\"set_desired\":5,\"uploads_admitted\":4}}";
    std::vector<uint8_t> bytes(body, body + strlen(body) + 1);

    SdkWirelessStatus st;
    CHECK(ParseWirelessStatus(bytes.data(), bytes.size(), st));
    CHECK(st.groups.size() == 1);
    CHECK(st.groups[0].mac == "9B:10:76:E5:66:E1");
    CHECK(st.groups[0].state == "failed");
    CHECK(st.groups[0].wedged);
    CHECK(!st.groups[0].confirmed);
    CHECK(st.groups[0].resends == 8);
    CHECK(st.groups[0].send_failures == 2);
    CHECK(st.groups[0].last_error == "resend budget exhausted");
    CHECK(st.groups[0].clock.valid);
    CHECK(st.groups[0].clock.ticks == 123456);
    CHECK(st.groups[0].clock.age_ms == 42);
    /* The full reply is retained verbatim for diagnostics
       passthrough — backend-side fields surface without a
       client rev. */
    CHECK(st.raw["available"] == true);
    CHECK(st.raw["counters"]["set_desired"] == 5);

    /* A healthy group parses the same way. */
    const char* ok =
        "{\"available\":true,\"groups\":[{\"mac\":\"W1\","
        "\"state\":\"holding\",\"wedged\":false,\"confirmed\":true,"
        "\"rgb_clock\":{\"valid\":false,\"ticks\":0,\"age_ms\":0}}]}";
    std::vector<uint8_t> okb(ok, ok + strlen(ok) + 1);
    SdkWirelessStatus st2;
    CHECK(ParseWirelessStatus(okb.data(), okb.size(), st2));
    CHECK(st2.groups.size() == 1 && st2.groups[0].state == "holding");
    CHECK(!st2.groups[0].wedged && st2.groups[0].confirmed);

    /* No-groups reply (no dongle pair detected). */
    const char* empty = "{\"available\":false,\"groups\":[]}";
    SdkWirelessStatus st3;
    CHECK(ParseWirelessStatus(
        (const uint8_t*)empty, strlen(empty) + 1, st3));
    CHECK(st3.groups.empty());
    CHECK(st3.raw["available"] == false);
}

static void TestWirelessStatusParseRejects()
{
    SdkWirelessStatus st;

    /* Not JSON at all. */
    const char* junk = "this is not json";
    CHECK(!ParseWirelessStatus(
        (const uint8_t*)junk, strlen(junk), st));

    /* JSON but not the object shape. */
    const char* arr = "[1,2,3]";
    CHECK(!ParseWirelessStatus(
        (const uint8_t*)arr, strlen(arr), st));

    /* Truncated mid-string. */
    const char* good =
        "{\"available\":true,\"groups\":[{\"mac\":\"W1\",\"state\":\"holding\"}]}";
    for(size_t len = 1; len < strlen(good); ++len)
    {
        SdkWirelessStatus t;
        CHECK(!ParseWirelessStatus((const uint8_t*)good, len, t));
    }

    /* Empty body. */
    CHECK(!ParseWirelessStatus(nullptr, 0, st));
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
    TestControllerMatrixRoundTrip();
    TestControllerNoMatrixRoundTrip();
    TestControllerMatrixMalformed();
    TestControllerDataTruncations();
    TestControllerDataMalformedCounts();
    TestBuilders();
    TestPatchModeSpeed();
    TestWirelessStatusParse();
    TestWirelessStatusParseRejects();
    std::printf("sdk wire: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
