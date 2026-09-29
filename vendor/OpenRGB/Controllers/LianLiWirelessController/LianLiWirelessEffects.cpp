/*---------------------------------------------------------*\
|| LianLiWirelessEffects.cpp                                 ||
||                                                           ||
||   Software-rendered playback loops for Lian Li wireless   ||
||   fan groups.                                             ||
||                                                           ||
||   This file is part of the OpenRGB project                ||
||   SPDX-License-Identifier: GPL-2.0-or-later               ||
\*---------------------------------------------------------*/

#include "LianLiWirelessEffects.h"
#include <cmath>
#include <cstring>

/*---------------------------------------------------------*\
| Integer HSV -> RGB at full saturation/value. Kept local:  |
| this layer stays free of Qt and RGBController headers so  |
| the offline protocol tests can compile it standalone.     |
\*---------------------------------------------------------*/
static void HueToRgb(unsigned int hue, uint8_t* rgb)
{
    hue %= 360;

    const uint8_t rising  = (uint8_t)(((hue % 60) * 255) / 60);
    const uint8_t falling = (uint8_t)(255 - rising);

    switch(hue / 60)
    {
        case 0:  rgb[0] = 255;     rgb[1] = rising;  rgb[2] = 0;       break;
        case 1:  rgb[0] = falling; rgb[1] = 255;     rgb[2] = 0;       break;
        case 2:  rgb[0] = 0;       rgb[1] = 255;     rgb[2] = rising;  break;
        case 3:  rgb[0] = 0;       rgb[1] = falling; rgb[2] = 255;     break;
        case 4:  rgb[0] = rising;  rgb[1] = 0;       rgb[2] = 255;     break;
        default: rgb[0] = 255;     rgb[1] = 0;       rgb[2] = falling; break;
    }
}

std::vector<uint8_t> LianLiWireless::RenderSpectrumCycle(size_t led_count,
                                                       size_t frame_count)
{
    std::vector<uint8_t> out(led_count * frame_count * 3);
    if(led_count == 0 || frame_count == 0)
    {
        return out;
    }

    for(size_t f = 0; f < frame_count; f++)
    {
        uint8_t rgb[3];
        HueToRgb((unsigned int)(360 * f / frame_count), rgb);

        for(size_t i = 0; i < led_count; i++)
        {
            std::memcpy(&out[(f * led_count + i) * 3], rgb, 3);
        }
    }
    return out;
}

std::vector<uint8_t> LianLiWireless::RenderRainbowWave(size_t led_count,
                                                     size_t frame_count,
                                                     bool reverse,
                                                     size_t leds_per_fan,
                                                     const uint8_t* led_order)
{
    std::vector<uint8_t> out(led_count * frame_count * 3);
    if(led_count == 0 || frame_count == 0)
    {
        return out;
    }

    const bool per_fan = leds_per_fan > 0 && led_count % leds_per_fan == 0;
    const bool remap   = per_fan && led_order != nullptr;

    for(size_t f = 0; f < frame_count; f++)
    {
        const int phase = (int)(360 * f / frame_count);

        for(size_t i = 0; i < led_count; i++)
        {
            /*-------------------------------------------------*\
            | The hue follows the global chain position so one  |
            | wheel spans the whole group: each fan shows a     |
            | 1/fans slice and the next fan continues where the |
            | previous ended (L-Connect-style continuity). The  |
            | write goes through the perimeter order so the     |
            | gradient follows the frame ring instead of raw    |
            | LED index.                                      |
            \*-------------------------------------------------*/
            const size_t fan  = per_fan ? i / leds_per_fan : 0;
            const size_t slot = per_fan ? i % leds_per_fan : i;
            const int offset  = (int)(360 * i / led_count);
            const size_t dst  = remap ? fan * leds_per_fan
                                        + led_order[slot]
                                      : i;

            int hue = reverse ? offset - phase : offset + phase;
            hue = ((hue % 360) + 360) % 360;

            HueToRgb((unsigned int)hue, &out[(f * led_count + dst) * 3]);
        }
    }
    return out;
}

std::vector<uint8_t> LianLiWireless::RenderBreathing(size_t led_count,
                                                   size_t frame_count,
                                                   uint8_t r, uint8_t g,
                                                   uint8_t b)
{
    std::vector<uint8_t> out(led_count * frame_count * 3);
    if(led_count == 0 || frame_count == 0)
    {
        return out;
    }

    for(size_t f = 0; f < frame_count; f++)
    {
        /*---------------------------------------------------------*\
        | Triangle wave: dark at the loop seam, full brightness at  |
        | the midpoint so the cycle repeats without a jump.         |
        \*---------------------------------------------------------*/
        const size_t half = frame_count / 2;
        const unsigned int scale = (f < half) ? (unsigned int)(255 * f / half)
                                              : (unsigned int)(255 * (frame_count - f) / half);

        const uint8_t rgb[3] = { (uint8_t)(r * scale / 255),
                                 (uint8_t)(g * scale / 255),
                                 (uint8_t)(b * scale / 255) };

        for(size_t i = 0; i < led_count; i++)
        {
            std::memcpy(&out[(f * led_count + i) * 3], rgb, 3);
        }
    }
    return out;
}

/*---------------------------------------------------------*\
| Position of LED i along the stack loop: left edge up      |
| through fans 0..N-1, then right edge down through         |
| N-1..0. Total path length is fans*24. Without a           |
| stack_pos table the raw LED index is used.                |
\*---------------------------------------------------------*/
static double StackCoord(size_t i, size_t leds_per_fan, size_t fans,
                         const uint8_t* stack_pos)
{
    if(stack_pos == nullptr || leds_per_fan == 0)
    {
        return (double)i;
    }
    const size_t fan = i / leds_per_fan;
    const unsigned r = stack_pos[i % leds_per_fan];
    if(r < 12)
    {
        return (double)(fan * 12 + r);
    }
    return (double)(fans * 12 + (fans - 1 - fan) * 12 + (r - 12));
}

/* Vertical coordinate 0..1 across the whole stack. */
static double StackHeight(size_t i, size_t led_count, size_t leds_per_fan,
                          size_t fans, const uint8_t* led_height)
{
    if(led_height == nullptr || leds_per_fan == 0 || fans == 0)
    {
        return led_count ? (double)i / (double)led_count : 0.0;
    }
    const size_t fan = i / leds_per_fan;
    return ((double)fan + (double)led_height[i % leds_per_fan] / 255.0)
           / (double)fans;
}

std::vector<uint8_t> LianLiWireless::RenderPerimeterWave(size_t led_count,
                                                       size_t frame_count,
                                                       bool reverse,
                                                       size_t leds_per_fan,
                                                       const uint8_t* stack_pos)
{
    std::vector<uint8_t> out(led_count * frame_count * 3);
    if(led_count == 0 || frame_count == 0)
    {
        return out;
    }
    const size_t fans   = leds_per_fan ? led_count / leds_per_fan : 1;
    const double length = (double)(stack_pos ? fans * 24 : led_count);

    for(size_t f = 0; f < frame_count; f++)
    {
        const int phase = (int)(360 * f / frame_count);
        for(size_t i = 0; i < led_count; i++)
        {
            int hue = (int)(360.0 * StackCoord(i, leds_per_fan, fans, stack_pos)
                            / length);
            hue = reverse ? hue - phase : hue + phase;
            hue = ((hue % 360) + 360) % 360;
            HueToRgb((unsigned int)hue, &out[(f * led_count + i) * 3]);
        }
    }
    return out;
}

std::vector<uint8_t> LianLiWireless::RenderPerimeterComet(size_t led_count,
                                                        size_t frame_count,
                                                        bool reverse,
                                                        size_t leds_per_fan,
                                                        const uint8_t* stack_pos,
                                                        uint8_t r, uint8_t g, uint8_t b)
{
    std::vector<uint8_t> out(led_count * frame_count * 3);
    if(led_count == 0 || frame_count == 0)
    {
        return out;
    }
    const size_t fans   = leds_per_fan ? led_count / leds_per_fan : 1;
    const double length = (double)(stack_pos ? fans * 24 : led_count);
    const double tail   = length / 4.0;

    for(size_t f = 0; f < frame_count; f++)
    {
        const double head = length * f / frame_count;
        for(size_t i = 0; i < led_count; i++)
        {
            double d = StackCoord(i, leds_per_fan, fans, stack_pos);
            d = reverse ? d - head : head - d;
            d = std::fmod(d + length, length);          /* distance behind head */
            const double s = d < tail ? 1.0 - d / tail : 0.0;
            uint8_t* dst = &out[(f * led_count + i) * 3];
            dst[0] = (uint8_t)(r * s);
            dst[1] = (uint8_t)(g * s);
            dst[2] = (uint8_t)(b * s);
        }
    }
    return out;
}

std::vector<uint8_t> LianLiWireless::RenderPerimeterChase(size_t led_count,
                                                        size_t frame_count,
                                                        bool reverse,
                                                        size_t leds_per_fan,
                                                        const uint8_t* stack_pos)
{
    std::vector<uint8_t> out(led_count * frame_count * 3);
    if(led_count == 0 || frame_count == 0)
    {
        return out;
    }
    const size_t fans   = leds_per_fan ? led_count / leds_per_fan : 1;
    const double length = (double)(stack_pos ? fans * 24 : led_count);
    const double seg    = 4.0;                     /* LEDs per chase block */

    for(size_t f = 0; f < frame_count; f++)
    {
        const double shift = length * f / frame_count;
        for(size_t i = 0; i < led_count; i++)
        {
            double p = StackCoord(i, leds_per_fan, fans, stack_pos);
            p = std::fmod(p + (reverse ? -shift : shift) + length, length);
            /* three colored segments chasing around the loop */
            const unsigned block = (unsigned)std::fmod(p / seg, 3.0);
            uint8_t* dst = &out[(f * led_count + i) * 3];
            dst[0] = block == 0 ? 255 : 0;
            dst[1] = block == 1 ? 255 : 0;
            dst[2] = block == 2 ? 255 : 0;
        }
    }
    return out;
}

std::vector<uint8_t> LianLiWireless::RenderPerimeterPingPong(size_t led_count,
                                                           size_t frame_count,
                                                           size_t leds_per_fan,
                                                           const uint8_t* stack_pos,
                                                           uint8_t r, uint8_t g, uint8_t b)
{
    std::vector<uint8_t> out(led_count * frame_count * 3);
    if(led_count == 0 || frame_count == 0)
    {
        return out;
    }
    const size_t fans   = leds_per_fan ? led_count / leds_per_fan : 1;
    const double length = (double)(stack_pos ? fans * 24 : led_count);
    const double tail   = length / 5.0;

    for(size_t f = 0; f < frame_count; f++)
    {
        /* triangle wave: 0 -> length -> 0 over the loop */
        double t = 2.0 * f / frame_count;
        if(t > 1.0) t = 2.0 - t;
        const double head = t * length;
        for(size_t i = 0; i < led_count; i++)
        {
            const double d = head - StackCoord(i, leds_per_fan, fans, stack_pos);
            const double s = (d >= 0.0 && d < tail) ? 1.0 - d / tail : 0.0;
            uint8_t* dst = &out[(f * led_count + i) * 3];
            dst[0] = (uint8_t)(r * s);
            dst[1] = (uint8_t)(g * s);
            dst[2] = (uint8_t)(b * s);
        }
    }
    return out;
}

std::vector<uint8_t> LianLiWireless::RenderStackWave(size_t led_count,
                                                   size_t frame_count,
                                                   size_t leds_per_fan,
                                                   const uint8_t* led_height)
{
    std::vector<uint8_t> out(led_count * frame_count * 3);
    if(led_count == 0 || frame_count == 0)
    {
        return out;
    }
    const size_t fans = leds_per_fan ? led_count / leds_per_fan : 1;

    for(size_t f = 0; f < frame_count; f++)
    {
        const double center = (double)f / frame_count;   /* rising crest */
        for(size_t i = 0; i < led_count; i++)
        {
            double d = StackHeight(i, led_count, leds_per_fan, fans, led_height) - center;
            d = std::fmod(d + 1.5, 1.0) - 0.5;           /* wrapped distance */
            const double s = d > 0.0 ? 0.0 : -d * 4.0;   /* glow below crest */
            const double v = s > 1.0 ? 1.0 : s;
            uint8_t* dst = &out[(f * led_count + i) * 3];
            HueToRgb((unsigned int)(360 * f / frame_count), dst);
            dst[0] = (uint8_t)(dst[0] * v);
            dst[1] = (uint8_t)(dst[1] * v);
            dst[2] = (uint8_t)(dst[2] * v);
        }
    }
    return out;
}

std::vector<uint8_t> LianLiWireless::RenderStackRainbow(size_t led_count,
                                                      size_t frame_count,
                                                      bool reverse,
                                                      size_t leds_per_fan,
                                                      const uint8_t* led_height)
{
    std::vector<uint8_t> out(led_count * frame_count * 3);
    if(led_count == 0 || frame_count == 0)
    {
        return out;
    }
    const size_t fans = leds_per_fan ? led_count / leds_per_fan : 1;

    for(size_t f = 0; f < frame_count; f++)
    {
        const int phase = (int)(360 * f / frame_count);
        for(size_t i = 0; i < led_count; i++)
        {
            int hue = (int)(360.0 * StackHeight(i, led_count, leds_per_fan,
                                                fans, led_height));
            hue = reverse ? hue + phase : hue - phase;
            hue = ((hue % 360) + 360) % 360;
            HueToRgb((unsigned int)hue, &out[(f * led_count + i) * 3]);
        }
    }
    return out;
}
