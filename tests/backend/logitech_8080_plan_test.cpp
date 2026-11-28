/*---------------------------------------------------------*\
| logitech_8080_plan_test.cpp                               |
|                                                           |
|   Offline regression test for LogitechPerKey8080Plan,     |
|   the Qt-free, hidapi-free planner behind the Logitech    |
|   HID++ 0x8080 per-key send path (G512, G410, G810        |
|   family). Plan P-7.                                      |
|                                                           |
|   LegacyFull*() below is a verbatim copy of the send      |
|   path as it stood before the extraction (commit          |
|   5630cea: the DeviceUpdateLEDs 0x8080 grouping loop and  |
|   the SetPerKey8080 filter/chunk/encode loop, with        |
|   SendVeryLongFrame replaced by a payload capture). The   |
|   planner's full repaint must match it byte for byte.     |
|   No hardware, no Qt, no hidapi.                          |
\*---------------------------------------------------------*/

#include "LogitechPerKey8080Plan.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <utility>
#include <vector>

static int failures = 0;
static int checks   = 0;

#define CHECK(cond, msg) do { checks++; if(!(cond)) { failures++; \
    printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, msg); } } while(0)

typedef std::vector<uint8_t>                       Payload;
typedef std::vector<std::pair<uint16_t, uint8_t>>  LedMap;
typedef std::vector<std::pair<uint16_t, std::vector<std::pair<uint8_t, RGBColor>>>> Groups;

/*---------------------------------------------------------*\
| Legacy reference (pre-extraction behaviour, 5630cea).     |
\*---------------------------------------------------------*/
static const size_t LEGACY_KEYS_PER_FRAME = 14;     /* HIDPP20_8080_KEYS_PER_FRAME */
static const size_t LEGACY_PAYLOAD_LEN    = 64 - 4; /* LOGITECH_VERY_LONG_MESSAGE_LEN - 4 */

static std::vector<Payload> LegacySetPerKey8080(const Groups& by_type, bool kt40_id12_only)
{
    std::vector<Payload> sent;

    for(size_t t = 0; t < by_type.size(); t++)
    {
        uint16_t key_type = by_type[t].first;
        const std::vector<std::pair<uint8_t, RGBColor>>& src_keys = by_type[t].second;

        std::vector<std::pair<uint8_t, RGBColor>>          filtered;
        const std::vector<std::pair<uint8_t, RGBColor>>*     keys = &src_keys;

        if(kt40_id12_only && key_type == 0x40)
        {
            for(size_t k = 0; k < src_keys.size(); k++)
            {
                if(src_keys[k].first == 1 || src_keys[k].first == 2)
                {
                    filtered.push_back(src_keys[k]);
                }
            }
            keys = &filtered;
        }

        for(size_t off = 0; off < keys->size(); off += LEGACY_KEYS_PER_FRAME)
        {
            size_t count = keys->size() - off;
            if(count > LEGACY_KEYS_PER_FRAME)
            {
                count = LEGACY_KEYS_PER_FRAME;
            }

            uint8_t payload[LEGACY_PAYLOAD_LEN];
            memset(payload, 0, sizeof(payload));

            payload[0] = (uint8_t)(key_type >> 8);
            payload[1] = (uint8_t)(key_type & 0xFF);
            payload[2] = (uint8_t)(count    >> 8);
            payload[3] = (uint8_t)(count    & 0xFF);

            size_t pos = 4;
            for(size_t k = 0; k < count; k++)
            {
                RGBColor color = (*keys)[off + k].second;
                payload[pos++] = (*keys)[off + k].first;
                payload[pos++] = RGBGetRValue(color);
                payload[pos++] = RGBGetGValue(color);
                payload[pos++] = RGBGetBValue(color);
            }

            /* SendVeryLongFrame(feat, FN_8080_SET_KEY_COLORS, payload, pos) */
            sent.push_back(Payload(payload, payload + pos));
        }
    }

    return sent;
}

static Groups LegacyGroup(const std::vector<RGBColor>& snapshot, const LedMap& led_map, int brightness)
{
    Groups                     by_type;
    std::map<uint16_t, size_t> type_slot;

    for(size_t i = 0; i < snapshot.size() && i < led_map.size(); i++)
    {
        uint16_t key_type = led_map[i].first;
        uint8_t  key_id   = led_map[i].second;

        if(key_type == 0 && key_id == 0)
        {
            continue;
        }

        RGBColor color = snapshot[i];

        if(brightness < 100)
        {
            uint8_t r = RGBGetRValue(color) * brightness / 100;
            uint8_t g = RGBGetGValue(color) * brightness / 100;
            uint8_t b = RGBGetBValue(color) * brightness / 100;
            color = ToRGBColor(r, g, b);
        }

        std::map<uint16_t, size_t>::iterator slot = type_slot.find(key_type);

        if(slot == type_slot.end())
        {
            type_slot[key_type] = by_type.size();
            by_type.emplace_back(key_type, std::vector<std::pair<uint8_t, RGBColor>>());
            slot = type_slot.find(key_type);
        }

        by_type[slot->second].second.emplace_back(key_id, color);
    }

    return by_type;
}

static std::vector<Payload> LegacyFullUpdate(const std::vector<RGBColor>& snapshot, const LedMap& led_map,
                                             int brightness, bool kt40_id12_only)
{
    Groups by_type = LegacyGroup(snapshot, led_map, brightness);

    if(by_type.empty())
    {
        return std::vector<Payload>();
    }

    return LegacySetPerKey8080(by_type, kt40_id12_only);
}

/*---------------------------------------------------------*\
| Planner -> wire payloads.                                 |
\*---------------------------------------------------------*/
static std::vector<Payload> Encode(const std::vector<LogitechPerKey8080Frame>& frames)
{
    std::vector<Payload> out;

    for(const LogitechPerKey8080Frame& frame : frames)
    {
        uint8_t payload[LOGITECH_8080_FRAME_PAYLOAD_LEN];
        memset(payload, 0xA5, sizeof(payload));
        size_t len = LogitechPerKey8080Plan::EncodeSetKeyColors(frame, payload);
        out.push_back(Payload(payload, payload + len));
    }

    return out;
}

static std::vector<Payload> PlannerFull(const std::vector<RGBColor>& snapshot, const LedMap& led_map,
                                        int brightness, bool kt40_id12_only)
{
    return Encode(LogitechPerKey8080Plan::PlanLeds(snapshot, led_map, brightness, kt40_id12_only));
}

/*---------------------------------------------------------*\
| Deterministic generator (no <random> distribution         |
| differences between toolchains).                          |
\*---------------------------------------------------------*/
static uint32_t rng_state = 0x8080C33Cu;

static uint32_t Next()
{
    rng_state = rng_state * 1664525u + 1013904223u;
    return rng_state >> 8;
}

static void RandomBoard(size_t n_leds, std::vector<RGBColor>& snapshot, LedMap& led_map)
{
    static const uint16_t types[] = { 0x0000, 0x0001, 0x0002, 0x0004, 0x0008, 0x0040, 0x0100 };

    snapshot.clear();
    led_map.clear();

    for(size_t i = 0; i < n_leds; i++)
    {
        uint16_t key_type = types[Next() % (sizeof(types) / sizeof(types[0]))];
        uint8_t  key_id   = (uint8_t)(Next() % 8 == 0 ? 0 : (Next() % 0xE8));

        if(Next() % 10 == 0)
        {
            key_type = 0;
            key_id   = 0;           /* unaddressable matrix slot */
        }

        led_map.emplace_back(key_type, key_id);
        snapshot.push_back(ToRGBColor(Next() & 0xFF, Next() & 0xFF, Next() & 0xFF));
    }
}

/*---------------------------------------------------------*\
| Tests                                                     |
\*---------------------------------------------------------*/
static void TestMatchesLegacyRandomized()
{
    static const int brightness_values[] = { 0, 1, 37, 50, 99, 100, 120 };

    for(int iter = 0; iter < 2000; iter++)
    {
        std::vector<RGBColor> snapshot;
        LedMap                led_map;

        RandomBoard(Next() % 160, snapshot, led_map);

        /* Mismatched lengths are legal input; the loop runs to the shorter. */
        if(iter % 17 == 0 && !led_map.empty())
        {
            led_map.pop_back();
        }

        int  brightness = brightness_values[Next() % (sizeof(brightness_values) / sizeof(int))];
        bool kt40       = (Next() & 1) != 0;

        std::vector<Payload> legacy  = LegacyFullUpdate(snapshot, led_map, brightness, kt40);
        std::vector<Payload> planned = PlannerFull(snapshot, led_map, brightness, kt40);

        if(legacy != planned)
        {
            printf("  iter %d: leds=%zu brightness=%d kt40=%d legacy=%zu frames planned=%zu frames\n",
                   iter, snapshot.size(), brightness, kt40 ? 1 : 0, legacy.size(), planned.size());
        }

        CHECK(legacy == planned, "full repaint matches the pre-extraction wire bytes");
    }
}

static void TestChunkGroupsMatchesLegacy()
{
    for(int iter = 0; iter < 500; iter++)
    {
        Groups groups;
        size_t n_groups = Next() % 5;

        for(size_t g = 0; g < n_groups; g++)
        {
            uint16_t key_type = (Next() % 3 == 0) ? 0x40 : (uint16_t)(1u << (Next() % 9));
            std::vector<std::pair<uint8_t, RGBColor>> keys;
            size_t n_keys = Next() % 40;

            for(size_t k = 0; k < n_keys; k++)
            {
                keys.emplace_back((uint8_t)(Next() % 6), ToRGBColor(Next() & 0xFF, Next() & 0xFF, Next() & 0xFF));
            }

            groups.emplace_back(key_type, keys);
        }

        bool kt40 = (Next() & 1) != 0;

        std::vector<Payload> legacy  = LegacySetPerKey8080(groups, kt40);
        std::vector<Payload> planned = Encode(LogitechPerKey8080Plan::ChunkGroups(groups, kt40));

        CHECK(legacy == planned, "ChunkGroups (the Off path) matches the pre-extraction wire bytes");
    }
}

static void TestChunkingAt14()
{
    std::vector<RGBColor> snapshot;
    LedMap                led_map;

    for(size_t i = 0; i < 30; i++)
    {
        led_map.emplace_back(0x0001, (uint8_t)(0x04 + i));
        snapshot.push_back(ToRGBColor(i, 0, 0));
    }

    std::vector<LogitechPerKey8080Frame> frames =
        LogitechPerKey8080Plan::PlanLeds(snapshot, led_map, 100, false);

    CHECK(frames.size() == 3, "30 keys of one type split into 3 frames");

    if(frames.size() == 3)
    {
        CHECK(frames[0].keys.size() == 14, "frame 0 carries 14 keys");
        CHECK(frames[1].keys.size() == 14, "frame 1 carries 14 keys");
        CHECK(frames[2].keys.size() == 2,  "frame 2 carries the remaining 2 keys");
        CHECK(frames[2].keys[1].led == 29, "frames carry their LED indices");
        CHECK(frames[1].keys[0].key_id == 0x04 + 14, "chunks are contiguous in LED order");
    }

    std::vector<Payload> payloads = Encode(frames);

    CHECK(payloads.size() == 3, "3 payloads");

    if(payloads.size() == 3)
    {
        CHECK(payloads[0].size() == 4 + 14 * 4, "a full frame is 60 bytes, the very-long payload");
        CHECK(payloads[2].size() == 4 + 2 * 4,  "a short frame is sent at its own length");

        const uint8_t head[] = { 0x00, 0x01, 0x00, 0x02, 0x04 + 28, 28, 0, 0, 0x04 + 29, 29, 0, 0 };
        CHECK(memcmp(payloads[2].data(), head, sizeof(head)) == 0,
              "payload = keyType BE, count BE, (keyId, R, G, B)...");
    }

    CHECK(HIDPP20_8080_KEYS_PER_FRAME == 14, "the 0x8080 frame holds 14 keys");
    CHECK(LOGITECH_8080_FRAME_PAYLOAD_LEN == 4 + HIDPP20_8080_KEYS_PER_FRAME * 4,
          "14 keys fill the very-long payload exactly");
}

static void TestGroupOrderAndUnaddressable()
{
    LedMap led_map =
    {
        { 0x0004, 0x10 },
        { 0x0000, 0x00 },       /* not enumerated: skipped */
        { 0x0001, 0x20 },
        { 0x0004, 0x11 },
        { 0x0001, 0x21 },
    };
    std::vector<RGBColor> snapshot(led_map.size(), ToRGBColor(1, 2, 3));

    std::vector<LogitechPerKey8080Frame> frames =
        LogitechPerKey8080Plan::PlanLeds(snapshot, led_map, 100, false);

    CHECK(frames.size() == 2, "two keyTypes -> two frames");

    if(frames.size() == 2)
    {
        CHECK(frames[0].key_type == 0x0004, "keyType order of first appearance");
        CHECK(frames[1].key_type == 0x0001, "second keyType after the first");
        CHECK(frames[0].keys.size() == 2 && frames[0].keys[0].led == 0 && frames[0].keys[1].led == 3,
              "keys keep LED order within a keyType");
        CHECK(frames[1].keys.size() == 2 && frames[1].keys[0].led == 2,
              "the unaddressable LED is skipped");
    }
}

static void TestKt40Filter()
{
    LedMap led_map =
    {
        { 0x0040, 1 }, { 0x0040, 2 }, { 0x0040, 3 }, { 0x0040, 4 },
        { 0x0001, 3 },          /* other keyTypes are never filtered */
    };
    std::vector<RGBColor> snapshot(led_map.size(), ToRGBColor(9, 9, 9));

    std::vector<LogitechPerKey8080Frame> on  = LogitechPerKey8080Plan::PlanLeds(snapshot, led_map, 100, true);
    std::vector<LogitechPerKey8080Frame> off = LogitechPerKey8080Plan::PlanLeds(snapshot, led_map, 100, false);

    CHECK(on.size() == 2, "G410 quirk: both keyTypes still sent");

    if(on.size() == 2)
    {
        CHECK(on[0].key_type == 0x40 && on[0].keys.size() == 2, "G410 quirk: keyType 0x40 keeps 2 keys");
        CHECK(on[0].keys[0].key_id == 1 && on[0].keys[1].key_id == 2, "G410 quirk: only keyIds 1 and 2");
        CHECK(on[1].keys.size() == 1 && on[1].keys[0].key_id == 3, "G410 quirk: keyType 0x01 keyId 3 kept");
    }

    CHECK(off.size() == 2 && off[0].keys.size() == 4, "no quirk: keyType 0x40 sends all 4 keys");

    /* A 0x40 group that filters to nothing sends no frame. */
    LedMap only40 = { { 0x0040, 5 }, { 0x0040, 6 } };
    std::vector<RGBColor> two(2, ToRGBColor(1, 1, 1));
    CHECK(LogitechPerKey8080Plan::PlanLeds(two, only40, 100, true).empty(),
          "G410 quirk: a fully filtered keyType sends nothing");
}

static void TestBrightnessScaling()
{
    CHECK(LogitechPerKey8080Plan::ScaleBrightness(ToRGBColor(200, 100, 51), 50) == ToRGBColor(100, 50, 25),
          "50% halves each channel, truncating");
    CHECK(LogitechPerKey8080Plan::ScaleBrightness(ToRGBColor(255, 255, 255), 99) == ToRGBColor(252, 252, 252),
          "99% of 255 truncates to 252");
    CHECK(LogitechPerKey8080Plan::ScaleBrightness(ToRGBColor(255, 128, 1), 100) == ToRGBColor(255, 128, 1),
          "100% is untouched");
    CHECK(LogitechPerKey8080Plan::ScaleBrightness(ToRGBColor(255, 128, 1), 0) == ToRGBColor(0, 0, 0),
          "0% is black");
    CHECK(LogitechPerKey8080Plan::ScaleBrightness(ToRGBColor(10, 20, 30), 150) == ToRGBColor(10, 20, 30),
          "above 100% is not scaled");

    LedMap                led_map  = { { 0x0001, 0x04 } };
    std::vector<RGBColor> snapshot = { ToRGBColor(200, 100, 51) };

    std::vector<Payload> payloads = PlannerFull(snapshot, led_map, 50, false);
    const uint8_t expect[] = { 0x00, 0x01, 0x00, 0x01, 0x04, 100, 50, 25 };

    CHECK(payloads.size() == 1 && payloads[0].size() == sizeof(expect) &&
          memcmp(payloads[0].data(), expect, sizeof(expect)) == 0,
          "the wire carries the dimmed color");
}

static void TestEmpty()
{
    std::vector<RGBColor> none;
    LedMap                no_map;

    CHECK(LogitechPerKey8080Plan::PlanLeds(none, no_map, 100, false).empty(), "no LEDs -> no frames");
    CHECK(LogitechPerKey8080Plan::ChunkGroups(Groups(), false).empty(), "no groups -> no frames");

    Groups empty_group = { { 0x0001, std::vector<std::pair<uint8_t, RGBColor>>() } };
    CHECK(LogitechPerKey8080Plan::ChunkGroups(empty_group, false).empty(), "an empty group sends nothing");
}

int main()
{
    TestMatchesLegacyRandomized();
    TestChunkGroupsMatchesLegacy();
    TestChunkingAt14();
    TestGroupOrderAndUnaddressable();
    TestKt40Filter();
    TestBrightnessScaling();
    TestEmpty();

    printf("logitech_8080_plan_test: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
