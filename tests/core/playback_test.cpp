// SPDX-License-Identifier: GPL-2.0-or-later
#include "playback/PlaybackLoop.h"
#include "LianLiWirelessFanType.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <vector>

using namespace LianLiWireless;
static int checks = 0;
static int failures = 0;
#define CHECK(x) do { ++checks; if(!(x)) { ++failures; std::printf("FAIL line %d: %s\n", __LINE__, #x); } } while(0)

/*---------------------------------------------------------*\
| R-11 step 2 byte pin: every preset's loop (frame count,   |
| interval, raw RGB and the tinyuz payload) is hashed over  |
| a fixed matrix of layouts, speeds and directions. The     |
| table was captured from BuildPlaybackLoop BEFORE the      |
| BuildLoopFromRenderer refactor; any byte that moves fails |
| the case. Regenerate only for an intended change:         |
|   playback_test --print-golden                            |
\*---------------------------------------------------------*/
static uint64_t Fnv(uint64_t h, const uint8_t* p, size_t n)
{
    for(size_t i = 0; i < n; i++)
    {
        h ^= p[i];
        h *= 1099511628211ull;
    }
    return h;
}

static uint64_t LoopHash(const PlaybackLoop& loop)
{
    uint64_t h = 1469598103934665603ull;
    const uint8_t head[] = {
        uint8_t(loop.frame_count >> 8), uint8_t(loop.frame_count),
        uint8_t(loop.timing.interval_ticks >> 8), uint8_t(loop.timing.interval_ticks),
        loop.timing.interval_fraction };
    h = Fnv(h, head, sizeof(head));
    h = Fnv(h, loop.rgb.data(), loop.rgb.size());
    const uint8_t sep = 0xA5;
    h = Fnv(h, &sep, 1);
    return Fnv(h, loop.compressed.data(), loop.compressed.size());
}

struct PinCase
{
    PlaybackRequest request;
    char name[32];
};

static std::vector<PinCase> PinCases()
{
    static const PlaybackPreset presets[] = {
        PlaybackPreset::SpectrumCycle, PlaybackPreset::RainbowWave,
        PlaybackPreset::Breathing, PlaybackPreset::PerimeterWave,
        PlaybackPreset::PerimeterComet, PlaybackPreset::PerimeterChase,
        PlaybackPreset::PerimeterPingPong, PlaybackPreset::StackWave,
        PlaybackPreset::StackRainbow };
    std::vector<PinCase> out;
    for(int p = 0; p < 9; p++)
    {
        /* layout 0: the SL V3 group as the controller builds it
           (40 per fan, SLV3 tables, a custom color); at speed 10
           seven of the nine presets exceed 12 KiB at 1440 frames,
           so the frame reduction is pinned too. layout 1: the
           engine's wired canonical order (no tables, default red).
           layout 2: the 208-LED rainbow of the checks below.
           Runtime bound (tinyuz is ~1-5 s per slow loop): speed 10
           runs on layout 0 only, forward; reverse is pinned at 200;
           the full matrix took ~66 s. */
        for(int layout = 0; layout < 3; layout++)
        {
            for(unsigned speed : {10u, 37u, 200u})
            {
                if(layout == 2 && (p != 1 || speed != 10u))
                {
                    continue;
                }
                if(layout == 1 && speed == 10u)
                {
                    continue;
                }
                for(int rev = 0; rev < (speed == 200u ? 2 : 1); rev++)
                {
                    PinCase c;
                    PlaybackRequest& r = c.request;
                    r.preset = presets[p];
                    r.speed = speed;
                    r.reverse = rev != 0;
                    if(layout == 0)
                    {
                        r.led_count = 120;
                        r.leds_per_fan = 40;
                        r.led_order = FanLedOrder(WirelessFanType::Slv3Led);
                        r.stack_pos = FanStackPos(WirelessFanType::Slv3Led);
                        r.led_height = FanLedHeight(WirelessFanType::Slv3Led);
                        r.red = 30; r.green = 140; r.blue = 250;
                    }
                    else if(layout == 1)
                    {
                        r.led_count = 120;
                    }
                    else
                    {
                        r.led_count = 208;
                        r.leds_per_fan = 52;
                    }
                    std::snprintf(c.name, sizeof(c.name), "p%d/l%d/s%u/r%d",
                                  p, layout, speed, rev);
                    out.push_back(c);
                }
            }
        }
    }
    return out;
}

/* Captured from BuildPlaybackLoop at main 5630cea, before the
   BuildLoopFromRenderer refactor. */
static const struct { const char* name; uint64_t hash; } kGolden[] = {
    {"p0/l0/s10/r0", 0xcf6bba85a2b112b4ull}, /* 1440 frames */
    {"p0/l0/s37/r0", 0xe5812e81f4e4b3ccull}, /* 389 frames */
    {"p0/l0/s200/r0", 0x3083ec384544641full}, /* 72 frames */
    {"p0/l0/s200/r1", 0x3083ec384544641full}, /* 72 frames */
    {"p0/l1/s37/r0", 0xe5812e81f4e4b3ccull}, /* 389 frames */
    {"p0/l1/s200/r0", 0x3083ec384544641full}, /* 72 frames */
    {"p0/l1/s200/r1", 0x3083ec384544641full}, /* 72 frames */
    {"p1/l0/s10/r0", 0x029901431b3e9e5full}, /* 351 frames */
    {"p1/l0/s37/r0", 0x4cb07e07b5af2d40ull}, /* 334 frames */
    {"p1/l0/s200/r0", 0xf4ab84c4166ddbb2ull}, /* 72 frames */
    {"p1/l0/s200/r1", 0x94a2680dbcf05e0cull}, /* 72 frames */
    {"p1/l1/s37/r0", 0x94b020ade9234c4eull}, /* 389 frames */
    {"p1/l1/s200/r0", 0x9020fcf29867f8d4ull}, /* 72 frames */
    {"p1/l1/s200/r1", 0x9c737328c03afb56ull}, /* 72 frames */
    {"p1/l2/s10/r0", 0xabaf598ab38adb40ull}, /* 275 frames */
    {"p2/l0/s10/r0", 0xfe366364a345687cull}, /* 1440 frames */
    {"p2/l0/s37/r0", 0x17f31f1a6b8a103cull}, /* 389 frames */
    {"p2/l0/s200/r0", 0xd80fbd7bc58f0ff1ull}, /* 72 frames */
    {"p2/l0/s200/r1", 0xd80fbd7bc58f0ff1ull}, /* 72 frames */
    {"p2/l1/s37/r0", 0x5c8a7deff9e6f929ull}, /* 389 frames */
    {"p2/l1/s200/r0", 0x18ca45b82c23bfacull}, /* 72 frames */
    {"p2/l1/s200/r1", 0x18ca45b82c23bfacull}, /* 72 frames */
    {"p3/l0/s10/r0", 0x5ff12b2f8b671755ull}, /* 171 frames */
    {"p3/l0/s37/r0", 0xe9c9459f7aec9aa6ull}, /* 192 frames */
    {"p3/l0/s200/r0", 0x4c717be24bffb917ull}, /* 72 frames */
    {"p3/l0/s200/r1", 0x2a9d186ba32d62bcull}, /* 72 frames */
    {"p3/l1/s37/r0", 0x94b020ade9234c4eull}, /* 389 frames */
    {"p3/l1/s200/r0", 0x9020fcf29867f8d4ull}, /* 72 frames */
    {"p3/l1/s200/r1", 0x9c737328c03afb56ull}, /* 72 frames */
    {"p4/l0/s10/r0", 0x727603b3946558a9ull}, /* 182 frames */
    {"p4/l0/s37/r0", 0x4c73c9da4ddb686aull}, /* 163 frames */
    {"p4/l0/s200/r0", 0xb16b59dbad1eec9cull}, /* 72 frames */
    {"p4/l0/s200/r1", 0x69423a81cdd91ce1ull}, /* 72 frames */
    {"p4/l1/s37/r0", 0x61e844e4a85abcfbull}, /* 320 frames */
    {"p4/l1/s200/r0", 0x03ab41477ba1bfedull}, /* 72 frames */
    {"p4/l1/s200/r1", 0xe06b3aa55c30f29aull}, /* 72 frames */
    {"p5/l0/s10/r0", 0x264724cbab884b89ull}, /* 1440 frames */
    {"p5/l0/s37/r0", 0xc1304cae03f90314ull}, /* 389 frames */
    {"p5/l0/s200/r0", 0x89e511cb037f785bull}, /* 72 frames */
    {"p5/l0/s200/r1", 0x1ab86848bc22c1a5ull}, /* 72 frames */
    {"p5/l1/s37/r0", 0x27fad6adb4a52eb7ull}, /* 389 frames */
    {"p5/l1/s200/r0", 0x744ab08b9c332220ull}, /* 72 frames */
    {"p5/l1/s200/r1", 0x234c9ca1008d5dd0ull}, /* 72 frames */
    {"p6/l0/s10/r0", 0x6408eea353aa5758ull}, /* 212 frames */
    {"p6/l0/s37/r0", 0x5d15a5ad55da42b9ull}, /* 209 frames */
    {"p6/l0/s200/r0", 0xd4bdd24b8c13d1deull}, /* 72 frames */
    {"p6/l0/s200/r1", 0xd4bdd24b8c13d1deull}, /* 72 frames */
    {"p6/l1/s37/r0", 0x4a17e35104e33d03ull}, /* 389 frames */
    {"p6/l1/s200/r0", 0x73ffec207cc47461ull}, /* 72 frames */
    {"p6/l1/s200/r1", 0x73ffec207cc47461ull}, /* 72 frames */
    {"p7/l0/s10/r0", 0x729a0662bf53286dull}, /* 191 frames */
    {"p7/l0/s37/r0", 0xa6a22fdf3dda0352ull}, /* 193 frames */
    {"p7/l0/s200/r0", 0x65d26b17f45a90fbull}, /* 72 frames */
    {"p7/l0/s200/r1", 0x65d26b17f45a90fbull}, /* 72 frames */
    {"p7/l1/s37/r0", 0xcbbd9d7f8eff1d5eull}, /* 141 frames */
    {"p7/l1/s200/r0", 0xb5c0d614a0700351ull}, /* 72 frames */
    {"p7/l1/s200/r1", 0xb5c0d614a0700351ull}, /* 72 frames */
    {"p8/l0/s10/r0", 0x8b1f67445345fa2dull}, /* 224 frames */
    {"p8/l0/s37/r0", 0xdd31114cb82b3384ull}, /* 224 frames */
    {"p8/l0/s200/r0", 0x67bdef13cffb4e60ull}, /* 72 frames */
    {"p8/l0/s200/r1", 0x9fec21548c2cfc85ull}, /* 72 frames */
    {"p8/l1/s37/r0", 0x0869c86b3711bad9ull}, /* 389 frames */
    {"p8/l1/s200/r0", 0x77dd4bef004e6816ull}, /* 72 frames */
    {"p8/l1/s200/r1", 0xca64cde8bb22674bull}, /* 72 frames */
};

static void CheckBytePin(bool print)
{
    const std::vector<PinCase> cases = PinCases();
    const size_t golden = sizeof(kGolden) / sizeof(kGolden[0]);
    size_t i = 0;
    for(const PinCase& c : cases)
    {
        const auto t0 = std::chrono::steady_clock::now();
        const PlaybackLoop loop = BuildPlaybackLoop(c.request);
        const uint64_t h = LoopHash(loop);
        if(print)
        {
            const long long ms = (long long)std::chrono::duration_cast<
                std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
            std::printf("    {\"%s\", 0x%016llxull}, /* %u frames, %lld ms */\n",
                        c.name, (unsigned long long)h, loop.frame_count, ms);
            continue;
        }
        const bool known = i < golden && std::strcmp(kGolden[i].name, c.name) == 0;
        CHECK(known);
        if(known && kGolden[i].hash != h)
        {
            CHECK(kGolden[i].hash == h);
            std::printf("  byte pin moved: %s\n", c.name);
        }
        ++i;
    }
    if(!print)
    {
        CHECK(i == golden);
    }
}

int main(int argc, char** argv)
{
    if(argc > 1 && std::strcmp(argv[1], "--print-golden") == 0)
    {
        CheckBytePin(true);
        return 0;
    }
    CheckBytePin(false);
    for(unsigned speed : {10u, 37u, 100u, 200u})
    {
        PlaybackRequest r;
        r.preset = PlaybackPreset::SpectrumCycle;
        r.led_count = 120;
        r.speed = speed;
        auto loop = BuildPlaybackLoop(r);
        CHECK(loop.frame_count > 0 && loop.frame_count <= 4096);
        CHECK(loop.rgb.size() == size_t(loop.frame_count) * 120 * 3);
        CHECK(loop.compressed.size() <= 12288);
        CHECK(loop.compression_us > 0);
        CHECK(std::abs(loop.effective_cycle_ms - 240000.0 / speed) < 5.0);
        CHECK(loop.nominal_fps > 59.0);
        CHECK(loop.timing.interval_fraction < 100);
    }
    PlaybackRequest r;
    r.preset = PlaybackPreset::Breathing;
    r.led_count = 120;
    r.speed = 100;
    r.red = 30; r.green = 140; r.blue = 250;
    auto breathing = BuildPlaybackLoop(r);
    CHECK(breathing.rgb[0] == 0);
    CHECK(breathing.rgb[breathing.rgb.size() - 3] <= 30);
    CHECK(breathing.rgb[breathing.rgb.size() - 2] <= 140);
    CHECK(breathing.rgb[breathing.rgb.size() - 1] <= 250);
    r.preset = PlaybackPreset::RainbowWave;
    r.led_count = 208;
    r.leds_per_fan = 52;
    r.speed = 10;
    auto large = BuildPlaybackLoop(r);
    CHECK(large.compressed.size() <= MAX_COMPRESSED_BYTES);
    CHECK(large.frame_count <= 1440);
    CHECK(std::abs(large.effective_cycle_ms - 24000.0) < 15.0);
    std::printf("208-LED rainbow at speed 10: %u frames, %.2f nominal fps, %zu compressed bytes\n",
        large.frame_count, large.nominal_fps, large.compressed.size());
    r.led_count = 120;
    r.leds_per_fan = 40;
    for(unsigned speed : {10u, 100u, 200u})
    {
        r.speed = speed;
        auto rainbow = BuildPlaybackLoop(r);
        CHECK(rainbow.compressed.size() <= MAX_COMPRESSED_BYTES);
        CHECK(std::abs(rainbow.effective_cycle_ms - 240000.0 / speed) < 15.0);
        std::printf("120-LED rainbow speed %u: %u frames, %.2f nominal fps, %zu compressed bytes\n",
            speed, rainbow.frame_count, rainbow.nominal_fps, rainbow.compressed.size());
    }
    r.speed = 0;
    try { (void)BuildPlaybackLoop(r); CHECK(false); }
    catch(const std::invalid_argument&) { CHECK(true); }
    std::printf("playback core: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
