/*---------------------------------------------------------*\
|| LianLiWirelessEffects.h                                   ||
||                                                           ||
||   Software-rendered playback loops for Lian Li wireless   ||
||   fan groups. The firmware plays an uploaded frame        ||
||   sequence autonomously, so a base effect is rendered     ||
||   once into frame-major RGB bytes and sent as a single    ||
||   multi-frame upload — no host streaming.                 ||
||                                                           ||
||   Pure functions, no Qt, no OpenRGB types: frame f, LED   ||
||   i occupies payload[(f * led_count + i) * 3 .. +2].      ||
||                                                           ||
||   This file is part of the OpenRGB project                ||
||   SPDX-License-Identifier: GPL-2.0-or-later               ||
\*---------------------------------------------------------*/

#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace LianLiWireless
{
    /*---------------------------------------------------------*\
    | 48 frames per cycle keeps every supported group well      |
    | under MAX_COMPRESSED_BYTES after tinyuz compression       |
    | (worst case: 4-fan SL V4, 208 LEDs -> 29952 raw bytes).   |
    \*---------------------------------------------------------*/
    constexpr size_t EFFECT_LOOP_FRAMES = 48;

    std::vector<uint8_t> RenderSpectrumCycle(size_t led_count, size_t frame_count);

    /*---------------------------------------------------------*\
    | With leds_per_fan set, each fan carries the full wheel    |
    | around its perimeter and the fans are phase-offset by     |
    | 360/fan_count so the wave cascades down the group.        |
    | led_order optionally remaps slot -> physical LED (see     |
    | FanLedOrder in LianLiWirelessFanType.h) so the gradient   |
    | follows the frame ring instead of raw LED index.          |
    \*---------------------------------------------------------*/
    std::vector<uint8_t> RenderRainbowWave(size_t led_count, size_t frame_count,
                                           bool reverse, size_t leds_per_fan = 0,
                                           const uint8_t* led_order = nullptr);
    std::vector<uint8_t> RenderBreathing(size_t led_count, size_t frame_count,
                                         uint8_t r, uint8_t g, uint8_t b);

    /*---------------------------------------------------------*\
    | Stack-aware effects: the whole group is one continuous    |
    | path — up the left edge through fans 0..N-1, down the     |
    | right edge through fans N-1..0 (or a vertical coordinate  |
    | for the height effects). stack_pos maps each per-fan LED  |
    | index to its slot on that loop (inner and outer strips    |
    | share a slot at the same height); led_height maps it to   |
    | a 0-255 vertical position. Both come from                 |
    | LianLiWirelessFanType.h; nullptr falls back to linear.    |
    \*---------------------------------------------------------*/
    std::vector<uint8_t> RenderPerimeterWave(size_t led_count, size_t frame_count,
                                             bool reverse, size_t leds_per_fan,
                                             const uint8_t* stack_pos);
    std::vector<uint8_t> RenderPerimeterComet(size_t led_count, size_t frame_count,
                                              bool reverse, size_t leds_per_fan,
                                              const uint8_t* stack_pos,
                                              uint8_t r, uint8_t g, uint8_t b);
    std::vector<uint8_t> RenderPerimeterChase(size_t led_count, size_t frame_count,
                                              bool reverse, size_t leds_per_fan,
                                              const uint8_t* stack_pos);
    std::vector<uint8_t> RenderPerimeterPingPong(size_t led_count, size_t frame_count,
                                                 size_t leds_per_fan,
                                                 const uint8_t* stack_pos,
                                                 uint8_t r, uint8_t g, uint8_t b);
    std::vector<uint8_t> RenderStackWave(size_t led_count, size_t frame_count,
                                         size_t leds_per_fan,
                                         const uint8_t* led_height);
    std::vector<uint8_t> RenderStackRainbow(size_t led_count, size_t frame_count,
                                            bool reverse, size_t leds_per_fan,
                                            const uint8_t* led_height);
}
