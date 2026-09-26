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
    const size_t fans  = per_fan ? led_count / leds_per_fan : 1;

    for(size_t f = 0; f < frame_count; f++)
    {
        const int phase = (int)(360 * f / frame_count);

        for(size_t i = 0; i < led_count; i++)
        {
            /*-------------------------------------------------*\
            | Per fan: the slot walks the full wheel around the |
            | perimeter; each fan is shifted by 1/fans of the   |
            | wheel so the wave cascades fan -> fan. The write  |
            | goes through the perimeter order so the gradient  |
            | follows the frame ring instead of raw LED index.  |
            \*-------------------------------------------------*/
            const size_t fan  = per_fan ? i / leds_per_fan : 0;
            const size_t slot = per_fan ? i % leds_per_fan : i;
            const int offset  = per_fan
                ? (int)(360 * slot / leds_per_fan + 360 * fan / fans)
                : (int)(360 * i / led_count);
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
