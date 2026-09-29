// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "LianLiWirelessProtocol.h"
#include <cstddef>
#include <cstdint>
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

    // Builds a complete loop, reducing its frame count only when the actual
    // tinyuz payload exceeds the firmware's 12 KiB upload limit.
    PlaybackLoop BuildPlaybackLoop(const PlaybackRequest& request);
}
