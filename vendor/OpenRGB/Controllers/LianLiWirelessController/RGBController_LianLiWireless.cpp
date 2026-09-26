/*---------------------------------------------------------*\
|| RGBController_LianLiWireless.cpp                          ||
||                                                           ||
||   RGBController for one Lian Li wireless fan group.       ||
||   The firmware receives full per-LED frames rather than   ||
||   hardware effect commands, so every mode renders to a    ||
||   compressed frame upload.                                ||
||                                                           ||
||   This file is part of the OpenRGB project                ||
||   SPDX-License-Identifier: GPL-2.0-or-later               ||
\*---------------------------------------------------------*/

#include "RGBController_LianLiWireless.h"
#include "LianLiWirelessEffects.h"
#include <algorithm>
#include <string>
#include <vector>

/**------------------------------------------------------------------*\
    @name Lian Li Wireless Fan Group
    @type USB
    @save :x:
    @direct :rotating_light:
    @effects :white_check_mark:
    @detectors DetectLianLiWireless
    @comment Fan groups are discovered over the wireless RX dongle at
             detect time. Groups that appear later (or while L-Connect
             holds the receiver) need an app restart/rescan with the
             dongles free. Effect modes render to a pre-built frame
             loop uploaded once — the firmware cycles it on its own.
\*-------------------------------------------------------------------*/

enum
{
    LLW_MODE_DIRECT    = 0,
    LLW_MODE_STATIC    = 1,
    LLW_MODE_SPECTRUM  = 2,
    LLW_MODE_RAINBOW   = 3,
    LLW_MODE_BREATHING = 4,
};

/*---------------------------------------------------------*\
| Speed slider 10..200 maps to a 24s..1.2s loop period at   |
| EFFECT_LOOP_FRAMES frames per cycle.                      |
\*---------------------------------------------------------*/
#define LLW_SPEED_MIN   10
#define LLW_SPEED_MAX   200
#define LLW_SPEED_DEF   100

RGBController_LianLiWireless::RGBController_LianLiWireless(LianLiWirelessController* controller_ptr)
{
    controller  = controller_ptr;

    name        = controller->GetFanName() + " " + controller->GetMacString().substr(12);
    vendor      = "Lian Li";
    type        = DEVICE_TYPE_COOLER;
    description = "Lian Li Wireless Fan Group";
    location    = "Wireless: " + controller->GetMacString();
    serial      = controller->GetMacString();

    mode Direct;
    Direct.name                 = "Direct";
    Direct.value                = LLW_MODE_DIRECT;
    Direct.flags                = MODE_FLAG_HAS_PER_LED_COLOR;
    Direct.color_mode           = MODE_COLORS_PER_LED;
    modes.push_back(Direct);

    mode Static;
    Static.name                 = "Static";
    Static.value                = LLW_MODE_STATIC;
    Static.flags                = MODE_FLAG_HAS_MODE_SPECIFIC_COLOR;
    Static.colors_min           = 1;
    Static.colors_max           = 1;
    Static.color_mode           = MODE_COLORS_MODE_SPECIFIC;
    Static.colors.resize(1);
    modes.push_back(Static);

    /*-------------------------------------------------*\
    | Software effects: each renders to one multi-frame  |
    | upload the firmware then loops without host       |
    | traffic.                                          |
    \*-------------------------------------------------*/
    mode Spectrum;
    Spectrum.name               = "Spectrum Cycle";
    Spectrum.value              = LLW_MODE_SPECTRUM;
    Spectrum.flags              = MODE_FLAG_HAS_SPEED;
    Spectrum.speed_min          = LLW_SPEED_MIN;
    Spectrum.speed_max          = LLW_SPEED_MAX;
    Spectrum.speed              = LLW_SPEED_DEF;
    Spectrum.color_mode         = MODE_COLORS_NONE;
    modes.push_back(Spectrum);

    mode Rainbow;
    Rainbow.name                = "Rainbow Wave";
    Rainbow.value               = LLW_MODE_RAINBOW;
    Rainbow.flags               = MODE_FLAG_HAS_SPEED
                                | MODE_FLAG_HAS_DIRECTION_LR;
    Rainbow.speed_min           = LLW_SPEED_MIN;
    Rainbow.speed_max           = LLW_SPEED_MAX;
    Rainbow.speed               = LLW_SPEED_DEF;
    Rainbow.color_mode          = MODE_COLORS_NONE;
    modes.push_back(Rainbow);

    mode Breathing;
    Breathing.name              = "Breathing";
    Breathing.value             = LLW_MODE_BREATHING;
    Breathing.flags             = MODE_FLAG_HAS_SPEED
                                | MODE_FLAG_HAS_MODE_SPECIFIC_COLOR;
    Breathing.speed_min         = LLW_SPEED_MIN;
    Breathing.speed_max         = LLW_SPEED_MAX;
    Breathing.speed             = LLW_SPEED_DEF;
    Breathing.colors_min        = 1;
    Breathing.colors_max        = 1;
    Breathing.color_mode        = MODE_COLORS_MODE_SPECIFIC;
    Breathing.colors.resize(1);
    Breathing.colors[0]         = ToRGBColor(0xFF, 0x00, 0x00);
    modes.push_back(Breathing);

    SetupZones();
}

RGBController_LianLiWireless::~RGBController_LianLiWireless()
{
    Shutdown();

    delete controller;
}

void RGBController_LianLiWireless::SetupZones()
{
    /*-------------------------------------------------*\
    | One linear zone per fan — the frame payload is    |
    | fan-major: fan i owns LEDs [i*leds_per_fan,       |
    | (i+1)*leds_per_fan).                              |
    \*-------------------------------------------------*/
    leds.clear();
    colors.clear();
    zones.resize(controller->GetFanCount());

    for(std::size_t fan_idx = 0; fan_idx < zones.size(); fan_idx++)
    {
        zone& z = zones[fan_idx];

        z.name          = "Fan " + std::to_string(fan_idx + 1);
        z.type          = ZONE_TYPE_LINEAR;
        z.leds_min      = controller->GetLEDsPerFan();
        z.leds_max      = controller->GetLEDsPerFan();
        z.leds_count    = controller->GetLEDsPerFan();

        for(unsigned int led_idx = 0; led_idx < z.leds_count; led_idx++)
        {
            led new_led;
            new_led.name    = z.name + ", LED " + std::to_string(led_idx + 1);
            new_led.value   = (unsigned int)fan_idx;
            leds.push_back(new_led);
        }
    }

    SetupColors();
}

void RGBController_LianLiWireless::DeviceConfigureZone(int /* zone_idx */)
{
    SetupZones();
}

void RGBController_LianLiWireless::DeviceUpdateLEDs()
{
    /*-------------------------------------------------*\
    | Per-LED writes only apply to Direct mode — in an   |
    | effect mode the uploaded loop owns the lights      |
    | until the next mode update.                        |
    \*-------------------------------------------------*/
    if(modes[active_mode].value != LLW_MODE_DIRECT)
    {
        return;
    }
    controller->SetLEDs(colors.data(), colors.size());
}

void RGBController_LianLiWireless::DeviceUpdateZoneLEDs(int /* zone */)
{
    /*-------------------------------------------------*\
    | The firmware takes whole-group frames — updating  |
    | one zone still uploads the full color buffer.     |
    \*-------------------------------------------------*/
    DeviceUpdateLEDs();
}

void RGBController_LianLiWireless::DeviceUpdateSingleLED(int /* led */)
{
    DeviceUpdateLEDs();
}

void RGBController_LianLiWireless::DeviceUpdateMode()
{
    const mode& active = modes[active_mode];

    switch(active.value)
    {
        case LLW_MODE_STATIC:
        {
            std::vector<RGBColor> solid(colors.size(), active.colors[0]);
            controller->SetLEDs(solid.data(), solid.size());
            break;
        }

        case LLW_MODE_SPECTRUM:
        case LLW_MODE_RAINBOW:
        case LLW_MODE_BREATHING:
        {
            /*-----------------------------------------*\
            | Render one full cycle; the firmware loops |
            | it until a new upload arrives.            |
            \*-----------------------------------------*/
            const uint16_t interval_ms = (uint16_t)std::max(1u,
                    5000 / std::max(1u, active.speed));

            std::vector<uint8_t> frames;

            if(active.value == LLW_MODE_SPECTRUM)
            {
                frames = LianLiWireless::RenderSpectrumCycle(
                        leds.size(), LianLiWireless::EFFECT_LOOP_FRAMES);
            }
            else if(active.value == LLW_MODE_RAINBOW)
            {
                frames = LianLiWireless::RenderRainbowWave(
                        leds.size(), LianLiWireless::EFFECT_LOOP_FRAMES,
                        active.direction == MODE_DIRECTION_RIGHT,
                        controller->GetLEDsPerFan(),
                        LianLiWireless::FanLedOrder(controller->GetFanType()));
            }
            else
            {
                frames = LianLiWireless::RenderBreathing(
                        leds.size(), LianLiWireless::EFFECT_LOOP_FRAMES,
                        (uint8_t)RGBGetRValue(active.colors[0]),
                        (uint8_t)RGBGetGValue(active.colors[0]),
                        (uint8_t)RGBGetBValue(active.colors[0]));
            }

            controller->SetFrames(frames, LianLiWireless::EFFECT_LOOP_FRAMES,
                                  interval_ms);
            break;
        }

        default:
            DeviceUpdateLEDs();
            break;
    }
}
