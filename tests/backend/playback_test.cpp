// SPDX-License-Identifier: GPL-2.0-or-later
#include "playback/PlaybackLoop.h"
#include "LianLiWirelessFanType.h"
#include <cstdio>
#include <cmath>

using namespace LianLiWireless;
static int checks = 0, failures = 0;
#define CHECK(x) do { ++checks; if(!(x)) { ++failures; std::printf("FAIL line %d: %s\n", __LINE__, #x); } } while(0)

int main()
{
    PlaybackRequest request;
    request.led_count = 120;
    request.leds_per_fan = 40;
    request.speed = 37;
    request.preset = PlaybackPreset::RainbowWave;
    auto forward = BuildPlaybackLoop(request);
    request.reverse = true;
    auto reverse = BuildPlaybackLoop(request);
    CHECK(forward.rgb != reverse.rgb);
    CHECK(forward.rgb.size() == reverse.rgb.size());
    CHECK(forward.compressed.size() <= MAX_COMPRESSED_BYTES);
    CHECK(std::abs(forward.effective_cycle_ms - 240000.0 / 37.0) < 15.0);

    RgbUpload upload(forward.compressed, 120, forward.frame_count, forward.timing);
    const Mac target = {1, 2, 3, 4, 5, 6};
    const Mac master = {7, 8, 9, 10, 11, 12};
    auto header = upload.Packet(0, target, master);
    CHECK(header[25] * 256 + header[26] == forward.frame_count);
    CHECK(header[32] * 256 + header[33] == forward.timing.interval_ticks);
    CHECK(header[34] == forward.timing.interval_fraction);
    CHECK(forward.timing.interval_fraction != 0); // 37 yields sub-ms protocol timing.
    const uint8_t clock_reply[] = {USB_CMD_GET_MAC, 7, 8, 9, 10, 11, 12,
                                   0xFF, 0xFF, 0xFF, 0xFE};
    uint32_t ticks = 0;
    CHECK(ParseRgbClockResponse(clock_reply, sizeof(clock_reply), master, ticks));
    CHECK(ticks == 0xFFFFFFFEu);
    CHECK(!ParseRgbClockResponse(clock_reply, sizeof(clock_reply) - 1, master, ticks));
    CHECK(!ParseRgbClockResponse(clock_reply, sizeof(clock_reply), target, ticks));
    const uint8_t zero_master_reply[] = {USB_CMD_GET_MAC, 0, 0, 0, 0, 0, 0,
                                         0, 0, 0, 1};
    CHECK(!ParseRgbClockResponse(zero_master_reply, sizeof(zero_master_reply),
                                 Mac{}, ticks));
    RgbUpload first(forward.compressed, 120, forward.frame_count, forward.timing, 0);
    RgbUpload after_wrap(forward.compressed, 120, forward.frame_count, forward.timing, 256);
    CHECK(first.EffectId() != after_wrap.EffectId());
    std::printf("playback backend: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
