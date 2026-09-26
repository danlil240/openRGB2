/*---------------------------------------------------------*\
|| bench_sdk_wire_test.cpp                                   ||
||                                                           ||
||   Offline tests for the benchmark producer's OpenRGB    ||
||   SDK wire layer. Builds synthetic controller-data      ||
||   blobs in the exact layout the vendored server          ||
||   serializes at protocol 4 (RGBController::               ||
||   GetDeviceDescriptionData) and verifies parsing,       ||
||   raw mode capture, and request body builders.          ||
||   No sockets, no hardware.                               ||
\*---------------------------------------------------------*/

#include "sdk_wire.h"

#include <cstdio>
#include <cstring>

using namespace openrgb_bench;

static int failures = 0;
static int checks   = 0;

#define CHECK(cond, msg) do { checks++; if(!(cond)) { failures++; \
    printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, msg); } } while(0)

/*-------------------------------------------------------------*\
| Writer mirroring the server-side serialization               |
\*-------------------------------------------------------------*/
struct Writer
{
    std::vector<uint8_t> buf;

    void U8(uint8_t v)                { buf.push_back(v); }
    void U16(uint16_t v)              { for(int i = 0; i < 2; i++) buf.push_back((uint8_t)(v >> (8 * i))); }
    void U32(uint32_t v)              { for(int i = 0; i < 4; i++) buf.push_back((uint8_t)(v >> (8 * i))); }
    void Str(const std::string& s)    { U16((uint16_t)(s.size() + 1)); buf.insert(buf.end(), s.begin(), s.end()); buf.push_back('\0'); }

    /* mode block exactly as GetModeDescriptionData emits at protocol 4 */
    void Mode(const std::string& name, uint32_t value, uint32_t flags,
              uint32_t speed, uint32_t color_mode,
              const std::vector<uint32_t>& colors)
    {
        Str(name);
        U32(value);
        U32(flags);
        U32(10);                       /* speed_min   */
        U32(200);                     /* speed_max   */
        U32(0);                       /* brightness_min  (v3+) */
        U32(100);                     /* brightness_max  (v3+) */
        U32(0);                       /* colors_min  */
        U32((uint32_t)colors.size()); /* colors_max  */
        U32(speed);
        U32(50);                       /* brightness      (v3+) */
        U32(0);                       /* direction   */
        U32(color_mode);
        U16((uint16_t)colors.size());
        for(uint32_t c : colors) { U32(c); }
    }
};

static std::vector<uint8_t> MakeControllerBlob(uint32_t* mode_direct_len,
                                                uint32_t* mode_spectrum_len)
{
    Writer w;

    w.U32(4);                                   /* placeholder data_size */

    w.U32(2);                                   /* device type */
    w.Str("Lian Li Wireless 66:e1");
    w.Str("Lian Li");
    w.Str("Lian Li Wireless Fan Group");
    w.Str("");
    w.Str("9B:10:76:E5:66:E1");
    w.Str("Wireless: 9B:10:76:E5:66:E1");

    w.U16(2);                                   /* num_modes */
    w.U32(1);                                   /* active_mode */

    const size_t direct_begin = w.buf.size();
    w.Mode("Direct", 0, 0x00000010, 0, 1, {});
    const uint32_t direct_len = (uint32_t)(w.buf.size() - direct_begin);

    const size_t spectrum_begin = w.buf.size();
    w.Mode("Spectrum Cycle", 2, 0x00000008, 100, 0,
           { 0x00FF0000, 0x0000FF00 });
    const uint32_t spectrum_len = (uint32_t)(w.buf.size() - spectrum_begin);

    w.U16(1);                                   /* num_zones */
    w.Str("Fan 1");
    w.U32(0);                                   /* zone type: linear */
    w.U32(40); w.U32(40); w.U32(40);            /* leds min/max/count */
    w.U16(0);                                   /* matrix map size: none */
    w.U16(1);                                   /* num_segments (v4) */
    w.Str("Fan 1"); w.U32(0); w.U32(0); w.U32(40);

    w.U16(3);                                   /* num_leds */
    w.Str("Fan 1, LED 1"); w.U32(0);
    w.Str("Fan 1, LED 2"); w.U32(0);
    w.Str("Fan 1, LED 3"); w.U32(0);

    w.U16(3);                                   /* num_colors */
    w.U32(0x000000FF); w.U32(0x0000FF00); w.U32(0x00FF0000);

    /* patch the leading data_size */
    const uint32_t total = (uint32_t)w.buf.size() - 4;
    memcpy(w.buf.data(), &total, 4);

    if(mode_direct_len)   { *mode_direct_len = direct_len; }
    if(mode_spectrum_len) { *mode_spectrum_len = spectrum_len; }
    return w.buf;
}

static void TestParseControllerData()
{
    uint32_t direct_len = 0, spectrum_len = 0;
    std::vector<uint8_t> blob = MakeControllerBlob(&direct_len, &spectrum_len);

    SdkController c;
    CHECK(ParseControllerData(blob.data(), blob.size(), 4, c),
          "protocol-4 controller blob parses");

    CHECK(c.type == 2, "device type decodes");
    CHECK(c.name == "Lian Li Wireless 66:e1", "device name decodes");
    CHECK(c.vendor == "Lian Li", "vendor decodes");
    CHECK(c.description == "Lian Li Wireless Fan Group", "description decodes");
    CHECK(c.serial == "9B:10:76:E5:66:E1", "serial decodes");
    CHECK(c.location == "Wireless: 9B:10:76:E5:66:E1", "location decodes");
    CHECK(c.active_mode == 1, "active mode index decodes");

    CHECK(c.modes.size() == 2, "both modes decode");
    CHECK(c.modes[0].name == "Direct" && c.modes[0].value == 0,
          "first mode name and value decode");
    CHECK(c.modes[1].name == "Spectrum Cycle" && c.modes[1].value == 2,
          "second mode name and value decode");
    CHECK(c.modes[1].raw.size() == spectrum_len,
          "raw mode block captured with exact length");
    CHECK(c.modes[0].raw.size() == direct_len,
          "first raw mode block captured");

    CHECK(c.zones.size() == 1 && c.zones[0].name == "Fan 1"
          && c.zones[0].leds_count == 40,
          "zone name and LED count decode");

    CHECK(c.num_leds == 3, "LED count decodes");
    CHECK(c.colors.size() == 3 && c.colors[0] == 0x000000FF
          && c.colors[2] == 0x00FF0000, "color buffer decodes");
}

static void TestParseRejectsMalformed()
{
    uint32_t dl = 0, sl = 0;
    std::vector<uint8_t> blob = MakeControllerBlob(&dl, &sl);
    SdkController c;

    /* data_size mismatch */
    {
        std::vector<uint8_t> bad = blob;
        uint32_t wrong = 0xDEADBEEF;
        memcpy(bad.data(), &wrong, 4);
        CHECK(!ParseControllerData(bad.data(), bad.size(), 4, c),
              "data_size mismatch rejected");
    }

    /* truncation at every offset must fail, not crash */
    bool all_rejected = true;
    for(size_t cut = 4; cut < blob.size(); cut++)
    {
        SdkController tmp;
        if(ParseControllerData(blob.data(), cut, 4, tmp))
        {
            all_rejected = false;
        }
    }
    CHECK(all_rejected, "every truncation is rejected");

    /* trailing garbage rejected (left != 0) */
    {
        std::vector<uint8_t> bad = blob;
        bad.push_back(0x00);
        CHECK(!ParseControllerData(bad.data(), bad.size(), 4, c),
              "trailing bytes rejected");
    }
}

static void TestRequestBodyBuilders()
{
    /* UpdateLEDs body: u32 size + u16 count + colors */
    std::vector<uint32_t> colors{ 0x000000FF, 0x0000FF00, 0x00FF0000 };
    std::vector<uint8_t> leds = BuildUpdateLEDsBody(colors);
    CHECK(leds.size() == 4 + 2 + 12, "update-leds body size");
    uint32_t size = 0;
    uint16_t count = 0;
    memcpy(&size, leds.data(), 4);
    memcpy(&count, leds.data() + 4, 2);
    CHECK(size == leds.size(), "update-leds body carries its size");
    CHECK(count == 3, "update-leds body carries color count");
    CHECK(memcmp(leds.data() + 6, colors.data(), 12) == 0,
          "update-leds body carries raw colors");

    /* UpdateMode body: u32 size + i32 mode index + raw mode */
    const uint8_t raw[] = { 0x01, 0x02, 0x03, 0x04, 0x05 };
    std::vector<uint8_t> mode = BuildUpdateModeBody(7,
        std::vector<uint8_t>(raw, raw + sizeof(raw)));
    CHECK(mode.size() == 4 + 4 + sizeof(raw), "update-mode body size");
    memcpy(&size, mode.data(), 4);
    int32_t index = 0;
    memcpy(&index, mode.data() + 4, 4);
    CHECK(size == mode.size(), "update-mode body carries its size");
    CHECK(index == 7, "update-mode body carries mode index");
    CHECK(memcmp(mode.data() + 8, raw, sizeof(raw)) == 0,
          "update-mode body replays the raw mode block verbatim");

    /* version + name + controller-data request bodies */
    std::vector<uint8_t> ver = BuildProtocolVersionRequest(4);
    CHECK(ver.size() == 4 && ver[0] == 4, "version request body");
    std::vector<uint8_t> name = BuildClientNameRequest("bench");
    CHECK(name.size() == 6 && name[5] == '\0',
          "client name body is NUL-terminated");
    std::vector<uint8_t> data = BuildControllerDataRequest(4);
    CHECK(data.size() == 4 && data[0] == 4, "controller-data request body");
}

static void TestRawModeRoundTrip()
{
    /* parse, then re-embed the captured raw mode into an UpdateMode body,
       then re-parse a blob built from it — the raw block must be stable */
    uint32_t dl = 0, sl = 0;
    std::vector<uint8_t> blob = MakeControllerBlob(&dl, &sl);

    SdkController c;
    CHECK(ParseControllerData(blob.data(), blob.size(), 4, c), "blob parses");

    /* the captured raw block is rebuilt into a fresh mode-only blob and
       re-parsed — it must be byte-stable so UpdateMode restore works */
    Writer w;
    w.U32(4);        /* data_size placeholder */
    w.U32(2);
    w.Str("Lian Li Wireless 66:e1");
    w.Str("Lian Li");
    w.Str("Lian Li Wireless Fan Group");
    w.Str("");
    w.Str("9B:10:76:E5:66:E1");
    w.Str("Wireless: 9B:10:76:E5:66:E1");
    w.U16(1);
    w.U32(0);
    w.buf.insert(w.buf.end(), c.modes[0].raw.begin(), c.modes[0].raw.end());
    w.U16(0);        /* no zones */
    w.U16(0);        /* no leds  */
    w.U16(0);        /* no colors*/
    const uint32_t total = (uint32_t)w.buf.size() - 4;
    memcpy(w.buf.data(), &total, 4);

    SdkController c2;
    CHECK(ParseControllerData(w.buf.data(), w.buf.size(), 4, c2),
          "re-embedded raw mode block reparses");
    CHECK(c2.modes.size() == 1 && c2.modes[0].name == "Direct"
          && c2.modes[0].value == 0,
          "re-embedded mode fields survive the round trip");
    CHECK(c2.modes[0].raw == c.modes[0].raw,
          "raw mode block is byte-stable across re-parse");
}

int main()
{
    TestParseControllerData();
    TestParseRejectsMalformed();
    TestRequestBodyBuilders();
    TestRawModeRoundTrip();

    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
