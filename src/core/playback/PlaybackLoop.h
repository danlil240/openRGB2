// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "LianLiWirelessProtocol.h"
#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

namespace LianLiWireless
{
    enum class PlaybackPreset
    {
        SpectrumCycle,
        RainbowWave,
        Breathing,
        PerimeterWave,
        PerimeterComet,
        PerimeterChase,
        PerimeterPingPong,
        StackWave,
        StackRainbow,
    };

    struct PlaybackRequest
    {
        PlaybackPreset preset = PlaybackPreset::SpectrumCycle;
        size_t led_count = 0;
        unsigned speed = 100; // OpenRGB 10..200; the 48-frame legacy cycle is 240000/speed ms.
        bool reverse = false;
        size_t leds_per_fan = 0;
        const uint8_t* led_order = nullptr;
        const uint8_t* stack_pos = nullptr;
        const uint8_t* led_height = nullptr;
        uint8_t red = 255, green = 0, blue = 0;
    };

    struct PlaybackLoop
    {
        std::vector<uint8_t> rgb;
        std::vector<uint8_t> compressed;
        uint16_t frame_count = 0;
        RgbTiming timing;
        double effective_cycle_ms = 0;
        double nominal_fps = 0;
        uint64_t compression_us = 0;
    };

    // Renders `frame_count` frames of one seamless cycle, frame-major:
    // frame f, LED i at [(f * led_count + i) * 3 .. +2]. Frame f shows
    // t = f * cycle / frame_count. Must return exactly
    // frame_count * led_count * 3 bytes.
    using LoopRenderer = std::function<std::vector<uint8_t>(size_t frame_count)>;

    // The shared loop builder (R-11 step 2): starts at 60 fps
    // (lround(cycle_ms * 0.06) frames, 2..4096), quantizes the interval
    // to 0.00625 ms protocol units, and reduces the frame count only
    // when the actual tinyuz payload exceeds the firmware's 12 KiB
    // upload limit; the cycle duration stays fixed. cycle_ms == 0 is a
    // still look: one frame, held (1 s interval).
    PlaybackLoop BuildLoopFromRenderer(double cycle_ms, size_t led_count,
                                       const LoopRenderer& render);

    // Builds a complete preset loop through BuildLoopFromRenderer.
    PlaybackLoop BuildPlaybackLoop(const PlaybackRequest& request);
}
