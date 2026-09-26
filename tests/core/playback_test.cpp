// SPDX-License-Identifier: GPL-2.0-or-later
#include "playback/PlaybackLoop.h"
#include <cmath>
#include <cstdio>
#include <stdexcept>

using namespace LianLiWireless;
static int checks = 0;
static int failures = 0;
#define CHECK(x) do { ++checks; if(!(x)) { ++failures; std::printf("FAIL line %d: %s\n", __LINE__, #x); } } while(0)

int main()
{
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
