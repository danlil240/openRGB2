/*---------------------------------------------------------*\
| RGBController_GigabyteRGBFusion2BlackwellGPU.cpp          |
|                                                           |
|   RGBController for Gigabyte RGB Fusion 2 Blackwell GPU   |
|                                                           |
|   This file is part of the OpenRGB project                |
|   SPDX-License-Identifier: GPL-2.0-or-later               |
\*---------------------------------------------------------*/

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <thread>
#include "RGBController_GigabyteRGBFusion2BlackwellGPU.h"
#include "LogManager.h"
#include "GigabyteRGBFusion2BlackwellGPUDefinitions.h"

/*---------------------------------------------------------*\
| Temporal smoothing (see Sweep): the firmware visibly      |
| chases each new direct-mode target, so noisy per-frame    |
| jitter (aurora's noise layer on a 1-LED zone) shows up as |
| flicker toward dark. Each sent channel is an exponential  |
| moving average of the requested colors — high-frequency   |
| jitter is attenuated below PWM visibility while genuine   |
| motion passes with ~2 frames of lag.                      |
|                                                           |
| The alphas are per reference frame (the engine's 45 ms    |
| I2C GPU write budget); the filter converts them to a time |
| constant, tau = -ref / ln(1 - alpha) (~104 ms fans,       |
| ~277 ms single-LED zones), and steps by                   |
| 1 - exp(-dt / tau) so the call rate does not matter.      |
\*---------------------------------------------------------*/
#define BWGPU_SMOOTH_ALPHA        0.35
#define BWGPU_SMOOTH_ALPHA_SINGLE 0.15
#define BWGPU_SMOOTH_REF_FRAME_MS 45.0
#define BWGPU_SMOOTH_MAX_DT_MS    1000.0

/*---------------------------------------------------------*\
| Settle interval: how long the settle thread waits for a   |
| streamed frame before stepping the filter itself, and the |
| step period while it settles. Refresh: identical packets  |
| are re-sent at least this often while frames arrive.      |
\*---------------------------------------------------------*/
#define BWGPU_SETTLE_INTERVAL_MS  45
#define BWGPU_REFRESH_MS          2000

/**------------------------------------------------------------------*\
    @name Gigabyte Fusion 2 Blackwell GPU
    @category GPU
    @type I2C
    @save :white_check_mark:
    @direct :white_check_mark:
    @effects :white_check_mark:
    @detectors DetectGigabyteRGBFusion2BlackwellSingleZoneGPUControllers,DetectGigabyteRGBFusion2BlackwellGamingLayoutGPUControllers,DetectGigabyteRGBFusion2BlackwellWaterforceLayoutGPUControllers
    @comment
\*-------------------------------------------------------------------*/

RGBController_RGBFusion2BlackwellGPU::RGBController_RGBFusion2BlackwellGPU(RGBFusion2BlackwellGPUController* controller_ptr, uint8_t led_layout)
{
    controller                      = controller_ptr;
    gpu_layout                      = led_layout;

    name                            = controller->GetDeviceName();
    vendor                          = "Gigabyte";
    description                     = "Gigabyte RGB Fusion 2 Blackwell GPU Device";
    location                        = controller->GetDeviceLocation();
    type                            = DEVICE_TYPE_GPU;

    mode Static;
    Static.name                     = "Static";
    Static.value                    = RGB_FUSION2_BLACKWELL_GPU_MODE_STATIC;
    Static.flags                    = MODE_FLAG_HAS_PER_LED_COLOR | MODE_FLAG_HAS_BRIGHTNESS | MODE_FLAG_MANUAL_SAVE;
    Static.color_mode               = MODE_COLORS_PER_LED;
    Static.brightness_min           = RGB_FUSION2_BLACKWELL_GPU_BRIGHTNESS_MIN;
    Static.brightness_max           = RGB_FUSION2_BLACKWELL_GPU_BRIGHTNESS_MAX;
    Static.brightness               = RGB_FUSION2_BLACKWELL_GPU_BRIGHTNESS_MAX;
    modes.push_back(Static);

    // Some GPU models (Gaming) dont maintain the colors but it has faster updates, useful for Effects (and the reason it has to be named Direct)
    mode Direct;
    Direct.name                     = "Direct";
    Direct.value                    = RGB_FUSION2_BLACKWELL_GPU_MODE_DIRECT;
    Direct.flags                    = MODE_FLAG_HAS_PER_LED_COLOR | MODE_FLAG_HAS_BRIGHTNESS | MODE_FLAG_MANUAL_SAVE;
    Direct.color_mode               = MODE_COLORS_PER_LED;
    Direct.speed_min                = RGB_FUSION2_BLACKWELL_GPU_SPEED_SLOWEST;
    Direct.speed_max                = RGB_FUSION2_BLACKWELL_GPU_SPEED_FASTEST;
    Direct.speed                    = RGB_FUSION2_BLACKWELL_GPU_SPEED_NORMAL;
    Direct.brightness_min           = RGB_FUSION2_BLACKWELL_GPU_BRIGHTNESS_MIN;
    Direct.brightness_max           = RGB_FUSION2_BLACKWELL_GPU_BRIGHTNESS_MAX;
    Direct.brightness               = RGB_FUSION2_BLACKWELL_GPU_BRIGHTNESS_MAX;
    modes.push_back(Direct);

    mode Breathing;
    Breathing.name                  = "Pulse";
    Breathing.value                 = RGB_FUSION2_BLACKWELL_GPU_MODE_BREATHING;
    Breathing.flags                 = MODE_FLAG_HAS_SPEED | MODE_FLAG_HAS_PER_LED_COLOR | MODE_FLAG_MANUAL_SAVE;
    Breathing.speed_min             = RGB_FUSION2_BLACKWELL_GPU_SPEED_SLOWEST;
    Breathing.speed_max             = RGB_FUSION2_BLACKWELL_GPU_SPEED_FASTEST;
    Breathing.speed                 = RGB_FUSION2_BLACKWELL_GPU_SPEED_NORMAL;
    Breathing.color_mode            = MODE_COLORS_PER_LED;
    Breathing.brightness_min        = RGB_FUSION2_BLACKWELL_GPU_BRIGHTNESS_MIN;
    Breathing.brightness_max        = RGB_FUSION2_BLACKWELL_GPU_BRIGHTNESS_MAX;
    Breathing.brightness            = RGB_FUSION2_BLACKWELL_GPU_BRIGHTNESS_MAX;
    modes.push_back(Breathing);

    mode Flashing;
    Flashing.name                   = "Flash";
    Flashing.value                  = RGB_FUSION2_BLACKWELL_GPU_MODE_FLASHING;
    Flashing.flags                  = MODE_FLAG_HAS_SPEED | MODE_FLAG_HAS_PER_LED_COLOR | MODE_FLAG_HAS_BRIGHTNESS | MODE_FLAG_MANUAL_SAVE;
    Flashing.speed_min              = RGB_FUSION2_BLACKWELL_GPU_SPEED_SLOWEST;
    Flashing.speed_max              = RGB_FUSION2_BLACKWELL_GPU_SPEED_FASTEST;
    Flashing.speed                  = RGB_FUSION2_BLACKWELL_GPU_SPEED_NORMAL;
    Flashing.color_mode             = MODE_COLORS_PER_LED;
    Flashing.brightness_min         = RGB_FUSION2_BLACKWELL_GPU_BRIGHTNESS_MIN;
    Flashing.brightness_max         = RGB_FUSION2_BLACKWELL_GPU_BRIGHTNESS_MAX;
    Flashing.brightness             = RGB_FUSION2_BLACKWELL_GPU_BRIGHTNESS_MAX;
    modes.push_back(Flashing);

    mode DualFlashing;
    DualFlashing.name               = "Double Flash";
    DualFlashing.value              = RGB_FUSION2_BLACKWELL_GPU_MODE_DUAL_FLASHING;
    DualFlashing.flags              = MODE_FLAG_HAS_SPEED | MODE_FLAG_HAS_PER_LED_COLOR | MODE_FLAG_HAS_BRIGHTNESS | MODE_FLAG_MANUAL_SAVE;
    DualFlashing.speed_min          = RGB_FUSION2_BLACKWELL_GPU_SPEED_SLOWEST;
    DualFlashing.speed_max          = RGB_FUSION2_BLACKWELL_GPU_SPEED_FASTEST;
    DualFlashing.speed              = RGB_FUSION2_BLACKWELL_GPU_SPEED_NORMAL;
    DualFlashing.color_mode         = MODE_COLORS_PER_LED;
    DualFlashing.brightness_min     = RGB_FUSION2_BLACKWELL_GPU_BRIGHTNESS_MIN;
    DualFlashing.brightness_max     = RGB_FUSION2_BLACKWELL_GPU_BRIGHTNESS_MAX;
    DualFlashing.brightness         = RGB_FUSION2_BLACKWELL_GPU_BRIGHTNESS_MAX;
    modes.push_back(DualFlashing);

    mode SpectrumCycle;
    SpectrumCycle.name              = "Color Cycle";
    SpectrumCycle.value             = RGB_FUSION2_BLACKWELL_GPU_MODE_COLOR_CYCLE;
    SpectrumCycle.flags             = MODE_FLAG_HAS_SPEED | MODE_FLAG_HAS_BRIGHTNESS | MODE_FLAG_MANUAL_SAVE;
    SpectrumCycle.speed_min         = RGB_FUSION2_BLACKWELL_GPU_SPEED_SLOWEST;
    SpectrumCycle.speed_max         = RGB_FUSION2_BLACKWELL_GPU_SPEED_FASTEST;
    SpectrumCycle.speed             = RGB_FUSION2_BLACKWELL_GPU_SPEED_NORMAL;
    SpectrumCycle.color_mode        = MODE_COLORS_NONE;
    SpectrumCycle.brightness_min    = RGB_FUSION2_BLACKWELL_GPU_BRIGHTNESS_MIN;
    SpectrumCycle.brightness_max    = RGB_FUSION2_BLACKWELL_GPU_BRIGHTNESS_MAX;
    SpectrumCycle.brightness        = RGB_FUSION2_BLACKWELL_GPU_BRIGHTNESS_MAX;
    modes.push_back(SpectrumCycle);

    if(led_layout != RGB_FUSION2_BLACKWELL_GPU_SINGLE_ZONE)
    {
        mode Wave;
        Wave.name                   = "Wave";
        Wave.value                  = RGB_FUSION2_BLACKWELL_GPU_MODE_WAVE;
        Wave.flags                  = MODE_FLAG_HAS_SPEED | MODE_FLAG_HAS_BRIGHTNESS | MODE_FLAG_MANUAL_SAVE;
        Wave.speed_min              = RGB_FUSION2_BLACKWELL_GPU_SPEED_SLOWEST;
        Wave.speed_max              = RGB_FUSION2_BLACKWELL_GPU_SPEED_FASTEST;
        Wave.speed                  = RGB_FUSION2_BLACKWELL_GPU_SPEED_NORMAL;
        Wave.color_mode             = MODE_COLORS_NONE;
        Wave.brightness_min         = RGB_FUSION2_BLACKWELL_GPU_BRIGHTNESS_MIN;
        Wave.brightness_max         = RGB_FUSION2_BLACKWELL_GPU_BRIGHTNESS_MAX;
        Wave.brightness             = RGB_FUSION2_BLACKWELL_GPU_BRIGHTNESS_MAX;
        modes.push_back(Wave);

        mode Gradient;
        Gradient.name               = "Gradient";
        Gradient.value              = RGB_FUSION2_BLACKWELL_GPU_MODE_GRADIENT;
        Gradient.flags              = MODE_FLAG_HAS_SPEED | MODE_FLAG_HAS_PER_LED_COLOR | MODE_FLAG_HAS_BRIGHTNESS | MODE_FLAG_MANUAL_SAVE;
        Gradient.speed_min          = RGB_FUSION2_BLACKWELL_GPU_SPEED_SLOWEST;
        Gradient.speed_max          = RGB_FUSION2_BLACKWELL_GPU_SPEED_FASTEST;
        Gradient.speed              = RGB_FUSION2_BLACKWELL_GPU_SPEED_NORMAL;
        Gradient.color_mode         = MODE_COLORS_PER_LED;
        Gradient.brightness_min     = RGB_FUSION2_BLACKWELL_GPU_BRIGHTNESS_MIN;
        Gradient.brightness_max     = RGB_FUSION2_BLACKWELL_GPU_BRIGHTNESS_MAX;
        Gradient.brightness         = RGB_FUSION2_BLACKWELL_GPU_BRIGHTNESS_MAX;
        modes.push_back(Gradient);

        mode ColorShift;
        ColorShift.name             = "Color Shift";
        ColorShift.value            = RGB_FUSION2_BLACKWELL_GPU_MODE_COLOR_SHIFT;
        ColorShift.flags            = MODE_FLAG_HAS_SPEED | MODE_FLAG_HAS_MODE_SPECIFIC_COLOR | MODE_FLAG_HAS_BRIGHTNESS | MODE_FLAG_MANUAL_SAVE;
        ColorShift.speed_min        = RGB_FUSION2_BLACKWELL_GPU_SPEED_SLOWEST;
        ColorShift.speed_max        = RGB_FUSION2_BLACKWELL_GPU_SPEED_FASTEST;
        ColorShift.speed            = RGB_FUSION2_BLACKWELL_GPU_SPEED_NORMAL;
        ColorShift.color_mode       = MODE_COLORS_MODE_SPECIFIC;
        ColorShift.colors_min       = 1;
        ColorShift.colors_max       = 8;
        ColorShift.colors.resize(8);
        ColorShift.brightness_min   = RGB_FUSION2_BLACKWELL_GPU_BRIGHTNESS_MIN;
        ColorShift.brightness_max   = RGB_FUSION2_BLACKWELL_GPU_BRIGHTNESS_MAX;
        ColorShift.brightness       = RGB_FUSION2_BLACKWELL_GPU_BRIGHTNESS_MAX;
        modes.push_back(ColorShift);

        if(led_layout != RGB_FUSION2_BLACKWELL_GPU_AORUS_WATERFORCE_LAYOUT)
        {
            mode Dazzle;
            Dazzle.name                 = "Dazzle";
            Dazzle.value                = RGB_FUSION2_BLACKWELL_GPU_MODE_DAZZLE;
            Dazzle.flags                = MODE_FLAG_HAS_SPEED | MODE_FLAG_HAS_MODE_SPECIFIC_COLOR | MODE_FLAG_HAS_BRIGHTNESS | MODE_FLAG_MANUAL_SAVE;
            Dazzle.speed_min            = RGB_FUSION2_BLACKWELL_GPU_SPEED_SLOWEST;
            Dazzle.speed_max            = RGB_FUSION2_BLACKWELL_GPU_SPEED_FASTEST;
            Dazzle.speed                = RGB_FUSION2_BLACKWELL_GPU_SPEED_NORMAL;
            Dazzle.color_mode           = MODE_COLORS_MODE_SPECIFIC;
            Dazzle.colors_min           = 1;
            Dazzle.colors_max           = 8;
            Dazzle.colors.resize(8);
            Dazzle.brightness_min       = RGB_FUSION2_BLACKWELL_GPU_BRIGHTNESS_MIN;
            Dazzle.brightness_max       = RGB_FUSION2_BLACKWELL_GPU_BRIGHTNESS_MAX;
            Dazzle.brightness           = RGB_FUSION2_BLACKWELL_GPU_BRIGHTNESS_MAX;
            modes.push_back(Dazzle);
        }
    }

    if(led_layout == RGB_FUSION2_BLACKWELL_GPU_AORUS_MASTER_5080_LAYOUT)
    {
        mode Claws;
        Claws.name           = "Claws";
        Claws.value          = RGB_FUSION2_BLACKWELL_GPU_MODE_CLAWS;
        Claws.flags          = MODE_FLAG_HAS_SPEED | MODE_FLAG_HAS_MODE_SPECIFIC_COLOR | MODE_FLAG_HAS_BRIGHTNESS | MODE_FLAG_MANUAL_SAVE;
        Claws.speed_min      = RGB_FUSION2_BLACKWELL_GPU_SPEED_SLOWEST;
        Claws.speed_max      = RGB_FUSION2_BLACKWELL_GPU_SPEED_FASTEST;
        Claws.speed          = RGB_FUSION2_BLACKWELL_GPU_SPEED_NORMAL;
        Claws.color_mode     = MODE_COLORS_MODE_SPECIFIC;
        Claws.brightness_min = RGB_FUSION2_BLACKWELL_GPU_BRIGHTNESS_MIN;
        Claws.brightness_max = RGB_FUSION2_BLACKWELL_GPU_BRIGHTNESS_MAX;
        Claws.brightness     = RGB_FUSION2_BLACKWELL_GPU_BRIGHTNESS_MAX;
        modes.push_back(Claws);

        for(size_t i = 0; i < modes.size(); i++)
        {
            if(modes[i].color_mode == MODE_COLORS_PER_LED)
            {
                modes[i].colors_min = 1;
                modes[i].colors_max = RGB_FUSION_2_BLACKWELL_AORUS_MASTER_FAN_LEDS;
                modes[i].colors.resize(RGB_FUSION_2_BLACKWELL_AORUS_MASTER_FAN_LEDS);
            }
        }
    }

    RGBController_RGBFusion2BlackwellGPU::SetupZones();

    clock_fn      = []() { return std::chrono::steady_clock::now(); };
    settle_thread = std::thread(&RGBController_RGBFusion2BlackwellGPU::SettleThreadFunction, this);
}

RGBController_RGBFusion2BlackwellGPU::~RGBController_RGBFusion2BlackwellGPU()
{
    /*-----------------------------------------------------*\
    | The settle thread takes AccessMutex and uses the      |
    | controller: stop it before Shutdown locks AccessMutex |
    | for good and before the controller is deleted.        |
    \*-----------------------------------------------------*/
    StopSettleThread();

    Shutdown();

    delete controller;
}

void RGBController_RGBFusion2BlackwellGPU::SetupZones()
{
    /*---------------------------------------------------------*\
    | This device only allows setting the entire zone for all   |
    | LED's in the zone and does not allow per LED control.     |
    \*---------------------------------------------------------*/
    if(gpu_layout == RGB_FUSION2_BLACKWELL_GPU_SINGLE_ZONE)
    {
        zone new_zone;
        led  new_led;

        new_zone.name       = "Side";
        new_zone.type       = ZONE_TYPE_SINGLE;
        new_zone.leds_min   = 1;
        new_zone.leds_max   = 1;
        new_zone.leds_count = 1;

        new_led.name        = new_zone.name;

        /*---------------------------------------------------------*\
        | Push the zone and LED on to device vectors                |
        \*---------------------------------------------------------*/
        leds.push_back(new_led);
        zones.push_back(new_zone);
    }
    else if(gpu_layout == RGB_FUSION2_BLACKWELL_GPU_GAMING_LAYOUT)
    {
        for(uint8_t zone_idx = 0; zone_idx < 4; zone_idx++)
        {
            zone new_zone;
            led  new_led;

            switch(zone_idx)
            {
                case 0:
                    new_zone.name = "Right fan";
                    break;

                case 1:
                    new_zone.name = "Left fan";
                    break;

                case 2:
                    new_zone.name = "Center fan";
                    break;

                case 3:
                    new_zone.name = "Side";
                    break;
            }

            new_zone.type       = ZONE_TYPE_SINGLE;
            new_zone.leds_min   = 1;
            new_zone.leds_max   = 1;
            new_zone.leds_count = 1;

            new_led.name        = new_zone.name;

            /*---------------------------------------------------------*\
            | Push the zone and LED on to device vectors                |
            \*---------------------------------------------------------*/
            leds.push_back(new_led);
            zones.push_back(new_zone);
        }
    }
    else if(gpu_layout == RGB_FUSION2_BLACKWELL_GPU_WATERFORCE_LAYOUT)
    {
        for(uint8_t zone_idx = 0; zone_idx < 2; zone_idx++)
        {
            zone new_zone;
            led  new_led;

            switch(zone_idx)
            {
                case 0:
                    new_zone.name = "Waterblock";
                    break;

                case 1:
                    new_zone.name = "Backplate";
                    break;
            }

            new_zone.type       = ZONE_TYPE_SINGLE;
            new_zone.leds_min   = 1;
            new_zone.leds_max   = 1;
            new_zone.leds_count = 1;

            new_led.name        = new_zone.name;

            /*---------------------------------------------------------*\
            | Push the zone and LED on to device vectors                |
            \*---------------------------------------------------------*/
            leds.push_back(new_led);
            zones.push_back(new_zone);
        }
    }
    else if(gpu_layout == RGB_FUSION2_BLACKWELL_GPU_AORUS_WATERFORCE_LAYOUT)
    {
        /*---------------------------------------------------------*\
        | Skip zone 0 - it doesn't exist on this card variant      |
        | Only add zones 1, 2, 3, 4 to the UI                      |
        \*---------------------------------------------------------*/
        for(uint8_t zone_idx = 1; zone_idx < 5; zone_idx++)
        {
            zone new_zone;
            led  new_led;

            switch(zone_idx)
            {
                case 1:
                    new_zone.name = "Bottom Logo";
                    break;

                case 2:
                    new_zone.name = "Radiator Fans";
                    break;

                case 3:
                    new_zone.name = "Top Logo";
                    break;

                case 4:
                    new_zone.name = "Side Logo";
                    break;
            }

            new_zone.type       = ZONE_TYPE_SINGLE;
            new_zone.leds_min   = 1;
            new_zone.leds_max   = 1;
            new_zone.leds_count = 1;

            new_led.name        = new_zone.name;

            /*---------------------------------------------------------*\
            | Push the zone and LED on to device vectors                |
            \*---------------------------------------------------------*/
            leds.push_back(new_led);
            zones.push_back(new_zone);
        }
    }
    else if(gpu_layout == RGB_FUSION2_BLACKWELL_GPU_AORUS_MASTER_5080_LAYOUT)
    {
        const char * fan_names[] = { "Right Fan", "Left Fan", "Middle Fan" };
        for(int i = 0; i < 3; i++)
        {
            zone fan_zone;
            fan_zone.name           = fan_names[i];
            fan_zone.type           = ZONE_TYPE_LINEAR;
            fan_zone.leds_min       = RGB_FUSION_2_BLACKWELL_AORUS_MASTER_FAN_LEDS;
            fan_zone.leds_max       = RGB_FUSION_2_BLACKWELL_AORUS_MASTER_FAN_LEDS;
            fan_zone.leds_count     = RGB_FUSION_2_BLACKWELL_AORUS_MASTER_FAN_LEDS;
            zones.push_back(fan_zone);

            for(unsigned int led_idx = 0; led_idx < fan_zone.leds_count; led_idx++)
            {
                led new_led;
                new_led.name = fan_names[i];
                new_led.name.append(" LED ");
                new_led.name.append(std::to_string(led_idx + 1));
                leds.push_back(new_led);
            }
        }

        zone side_logo;
        side_logo.name          = "Side Logo";
        side_logo.type          = ZONE_TYPE_SINGLE;
        side_logo.leds_min      = 1;
        side_logo.leds_max      = 1;
        side_logo.leds_count    = 1;
        zones.push_back(side_logo);

        led logo_led;
        logo_led.name = "Side Logo";
        leds.push_back(logo_led);

        zone top_logo;
        top_logo.name           = "Top Logo";
        top_logo.type           = ZONE_TYPE_SINGLE;
        top_logo.leds_min       = 1;
        top_logo.leds_max       = 1;
        top_logo.leds_count     = 1;
        zones.push_back(top_logo);

        led top_led;
        top_led.name = "Top Logo";
        leds.push_back(top_led);
    }

    else if(gpu_layout == RGB_FUSION2_BLACKWELL_GPU_AORUS_MASTER_5090D_V2_ICE_LAYOUT)
    {
        const char * fan_names[] = { "Right Fan", "Left Fan", "Middle Fan" };
        for(int i = 0; i < 3; i++)
        {
            zone fan_zone;
            fan_zone.name           = fan_names[i];
            fan_zone.type           = ZONE_TYPE_SINGLE;
            fan_zone.leds_min       = 1;
            fan_zone.leds_max       = 1;
            fan_zone.leds_count     = 1;
            zones.push_back(fan_zone);

            led new_led;
            new_led.name = fan_names[i];
            leds.push_back(new_led);
        }

        zone backplate;
        backplate.name          = "Backplate";
        backplate.type          = ZONE_TYPE_SINGLE;
        backplate.leds_min      = 1;
        backplate.leds_max      = 1;
        backplate.leds_count    = 1;
        zones.push_back(backplate);

        led bp_led;
        bp_led.name = "Backplate";
        leds.push_back(bp_led);

        zone side_logo;
        side_logo.name          = "Side Logo";
        side_logo.type          = ZONE_TYPE_SINGLE;
        side_logo.leds_min      = 1;
        side_logo.leds_max      = 1;
        side_logo.leds_count    = 1;
        zones.push_back(side_logo);

        led sl_led;
        sl_led.name = "Side Logo";
        leds.push_back(sl_led);
    }

    SetupColors();
}

void RGBController_RGBFusion2BlackwellGPU::DeviceUpdateLEDs()
{
    Sweep(true);
}

void RGBController_RGBFusion2BlackwellGPU::Sweep(bool throttle)
{
    std::lock_guard<std::mutex> sweep_lock(sweep_mutex);
    SweepLocked(throttle);
}

void RGBController_RGBFusion2BlackwellGPU::SweepLocked(bool throttle)
{
    const TimePoint now = clock_fn();

    /*---------------------------------------------------------*\
    | Diagnostic (flicker hunt): OPENRGB_BWGPU_MIN_INTERVAL_MS   |
    | drops whole sweeps that arrive closer than N ms. If the    |
    | MCU latches writes on its own tick or restarts a fade per  |
    | packet, write-rate sensitivity shows up as a changed       |
    | artifact. 0/unset = every frame passes. A dropped sweep    |
    | leaves the filter unsettled so the settle thread catches   |
    | up with the dropped target.                                |
    \*---------------------------------------------------------*/
    static const unsigned long min_interval_ms = []()
    {
        const char* s = getenv("OPENRGB_BWGPU_MIN_INTERVAL_MS");
        return (s != nullptr && *s != '\0') ? strtoul(s, nullptr, 10) : 0ul;
    }();

    if(throttle && min_interval_ms > 0)
    {
        if(now - last_throttle_sweep < std::chrono::milliseconds(min_interval_ms))
        {
            unsettled        = true;
            throttle_dropped = true;
            {
                std::lock_guard<std::mutex> settle_lock(settle_mutex);
                settle_wake = true;
            }
            settle_cv.notify_one();
            return;
        }
        last_throttle_sweep = now;
    }

    last_sweep       = now;
    throttle_dropped = false;

    const bool write_all = force_write;
    force_write          = false;
    bool any_unsettled   = false;

    uint8_t gpu_zones;
    switch(gpu_layout) // replicating GCC that sends more packets even when there is less zones
    {
        case RGB_FUSION2_BLACKWELL_GPU_SINGLE_ZONE:
            gpu_zones = 1;
            break;

        case RGB_FUSION2_BLACKWELL_GPU_GAMING_LAYOUT:
            gpu_zones = 6;
            break;

        case RGB_FUSION2_BLACKWELL_GPU_WATERFORCE_LAYOUT:
            gpu_zones = 3;
            break;

        case RGB_FUSION2_BLACKWELL_GPU_AORUS_WATERFORCE_LAYOUT:
            gpu_zones = 5; // Hardware zones 0-4, but zone 0 is skipped
            break;

        case RGB_FUSION2_BLACKWELL_GPU_AORUS_MASTER_5080_LAYOUT:
            gpu_zones = 6; // Zones 4 and 5 refer to ui zone 4
            break;

        case RGB_FUSION2_BLACKWELL_GPU_AORUS_MASTER_5090D_V2_ICE_LAYOUT:
            gpu_zones = 6;
            break;

        default:
            LOG_TRACE("[%s] Invalid GPU layout (%d) when updating LEDs.", name.c_str(), gpu_layout);
            return; // should not happen
    }

    const bool direct = (modes[active_mode].value == RGB_FUSION2_BLACKWELL_GPU_MODE_DIRECT);

    for(uint8_t zone_idx = 0; zone_idx < gpu_zones; zone_idx++)
    {
        /*---------------------------------------------------------*\
        | Build the zone config fresh for every zone — a multi-LED  |
        | palette (numberOfColors > 0) must never leak into a       |
        | following single-color zone write.                        |
        \*---------------------------------------------------------*/
        fusion2_config zone_config;
        zone_config.brightness      = modes[active_mode].brightness;
        zone_config.speed           = modes[active_mode].speed;

        /*---------------------------------------------------------*\
        | Diagnostic: OPENRGB_BWGPU_SPEED forces the packet SPD      |
        | byte — if direct-mode writes trigger an internal fade at   |
        | this speed, changing it changes the artifact's character.  |
        \*---------------------------------------------------------*/
        static const int speed_override = []()
        {
            const char* s = getenv("OPENRGB_BWGPU_SPEED");
            if(s == nullptr || *s == '\0')
            {
                return -1;
            }
            const long v = strtol(s, nullptr, 10);
            return (v >= 0 && v <= 255) ? (int)v : -1;
        }();
        if(speed_override >= 0)
        {
            zone_config.speed = (uint8_t)speed_override;
        }

        zone_config.direction       = modes[active_mode].direction;
        zone_config.numberOfColors  = 0;

        if(modes[active_mode].color_mode == MODE_COLORS_MODE_SPECIFIC)
        {
            zone_config.numberOfColors = (uint8_t)modes[active_mode].colors.size();
        }

        /*---------------------------------------------------------*\
        | For AORUS WATERFORCE layout, map UI zones to hardware    |
        | UI zone 0 -> HW zone 1 (Bottom Logo)                     |
        | UI zone 1 -> HW zone 2 (Radiator Fans)                   |
        | UI zone 2 -> HW zone 3 (Top Logo)                        |
        | UI zone 3 -> HW zone 4 (Side Logo)                       |
        | Skip HW zone 0 (doesn't exist on this card)              |
        \*---------------------------------------------------------*/
        uint8_t hardware_zone_idx = zone_idx;
        uint8_t ui_zone_idx = zone_idx;

        if(gpu_layout == RGB_FUSION2_BLACKWELL_GPU_AORUS_WATERFORCE_LAYOUT)
        {
            if(zone_idx == 0)
            {
                continue; // Skip hardware zone 0
            }
            ui_zone_idx = zone_idx - 1; // Map: HW zone 1->UI zone 0, HW zone 2->UI zone 1, HW zone 3->UI zone 2, HW zone 4->UI zone 3
        }
        else if(gpu_layout == RGB_FUSION2_BLACKWELL_GPU_AORUS_MASTER_5080_LAYOUT ||
                gpu_layout == RGB_FUSION2_BLACKWELL_GPU_AORUS_MASTER_5090D_V2_ICE_LAYOUT)
        {
            if(zone_idx == 5)
            {
                ui_zone_idx = zone_idx - 1;  // Map: HW zone 5->UI zone 4
            }
        }

        const bool    ui_zone_valid = (ui_zone_idx < zones.size());
        unsigned int  zone_leds     = 1;

        if(!ui_zone_valid)
        {
            zone_config.colors[0] = colors.back();
        }
        else
        {
            /*---------------------------------------------------------*\
            | Equivalent of:                                            |
            |   zone_config.colors[0] = colors[ui_zone_idx];            |
            | when all zones have 1 led.                                |
            \*---------------------------------------------------------*/
            uint8_t led_start = 0;
            for(uint8_t i = 0; i < ui_zone_idx; i++)
            {
                led_start += zones[i].leds_count;
            }
            zone_leds = zones[ui_zone_idx].leds_count;
            if(zone_leds > RGB_FUSION_2_BLACKWELL_AORUS_MASTER_FAN_LEDS)
            {
                zone_leds = RGB_FUSION_2_BLACKWELL_AORUS_MASTER_FAN_LEDS;
            }
            for(unsigned int i = 0; i < zone_leds; i++)
            {
                zone_config.colors[i] = colors[led_start + i];
            }
        }

        /*---------------------------------------------------------*\
        | Trace diagnostics: record whether smoother state existed  |
        | when this write was built, and which RGBController        |
        | instance built it (a second instance would interleave its |
        | own smoothed stream on the same hardware zones).          |
        \*---------------------------------------------------------*/
        zone_config.diag_sm  = has_smoothed[hardware_zone_idx] ? 1 : 0;
        zone_config.diag_who = (uint8_t)((uintptr_t)this >> 4);
        zone_config.diag_tid = (uint8_t)(std::hash<std::thread::id>{}(
                                             std::this_thread::get_id()) >> 8);

        /*---------------------------------------------------------*\
        | Temporal smoothing on direct-mode writes (see above):     |
        | filter every channel toward its target so noisy drift     |
        | never reaches the wire. The step is time-based            |
        | (alpha = 1 - exp(-dt/tau)) so the response does not       |
        | depend on how often frames arrive; the settle thread      |
        | keeps stepping after frames stop until every channel is   |
        | within 0.5 of its target. Non-direct writes bypass the    |
        | filter and reset its state so the next direct frame is    |
        | applied exactly (no lag out of a mode switch).            |
        \*---------------------------------------------------------*/
        if(direct)
        {
            /*---------------------------------------------------------*\
            | Single-LED zones (the logos) get the slower time constant |
            | — they have no spatial dimension to hide stepping behind. |
            | OPENRGB_BWGPU_SMOOTH_ALPHA forces one per-reference-frame |
            | alpha for every zone (flicker diagnostics; 1.0 disables   |
            | smoothing).                                               |
            \*---------------------------------------------------------*/
            static const double alpha_override = []()
            {
                const char* s = getenv("OPENRGB_BWGPU_SMOOTH_ALPHA");
                if(s == nullptr || *s == '\0')
                {
                    return -1.0;
                }
                char*       end = nullptr;
                const double v  = strtod(s, &end);
                return (end != s && v > 0.0 && v <= 1.0) ? v : -1.0;
            }();
            const double ref_alpha = (alpha_override > 0.0)
                                   ? alpha_override
                                   : (zone_leds > 1 ? BWGPU_SMOOTH_ALPHA
                                                    : BWGPU_SMOOTH_ALPHA_SINGLE);

            double alpha = 1.0;
            if(has_smoothed[hardware_zone_idx] && ref_alpha < 1.0)
            {
                double dt_ms = std::chrono::duration<double, std::milli>(now - last_step[hardware_zone_idx]).count();
                if(dt_ms < 0.0)
                {
                    dt_ms = 0.0;
                }
                if(dt_ms > BWGPU_SMOOTH_MAX_DT_MS)
                {
                    dt_ms = BWGPU_SMOOTH_MAX_DT_MS;
                }
                const double tau_ms = -BWGPU_SMOOTH_REF_FRAME_MS / std::log(1.0 - ref_alpha);
                alpha = 1.0 - std::exp(-dt_ms / tau_ms);
            }

            for(unsigned int i = 0; i < zone_leds; i++)
            {
                for(unsigned int c = 0; c < 3; c++)
                {
                    const double target  = (double)((zone_config.colors[i] >> (c * 8)) & 0xFF);
                    double       current = has_smoothed[hardware_zone_idx]
                                         ? smoothed[hardware_zone_idx][i][c]
                                         : target;
                    current += (target - current) * alpha;
                    if(std::fabs(target - current) < 0.5)
                    {
                        current = target;
                    }
                    else
                    {
                        any_unsettled = true;
                    }
                    smoothed[hardware_zone_idx][i][c] = current;
                }

                zone_config.colors[i] = ToRGBColor(
                    (unsigned int)(smoothed[hardware_zone_idx][i][0] + 0.5),
                    (unsigned int)(smoothed[hardware_zone_idx][i][1] + 0.5),
                    (unsigned int)(smoothed[hardware_zone_idx][i][2] + 0.5));
            }

            has_smoothed[hardware_zone_idx] = true;
            last_step[hardware_zone_idx]    = now;
        }
        else
        {
            has_smoothed[hardware_zone_idx] = false;
        }

        /*---------------------------------------------------------*\
        | If not all led are the same color, then we must pass all  |
        | led colors in the i2c write. Decided on the smoothed      |
        | outputs: while a fan ring converges onto a uniform color  |
        | its slots still differ, and collapsing to slot 0 early    |
        | would make slots 1-7 jump.                                |
        |                                                           |
        | Diagnostic switch: OPENRGB_BWGPU_NO_PALETTE=1 collapses   |
        | every zone to a single SZ=1 color (colors[0]) — used to   |
        | isolate whether multi-color palette writes trigger the    |
        | AORUS MASTER banner flicker.                              |
        \*---------------------------------------------------------*/
        static const bool no_palette =
            (getenv("OPENRGB_BWGPU_NO_PALETTE") != nullptr);

        if(ui_zone_valid && !no_palette &&
           !std::all_of(zone_config.colors, zone_config.colors + zone_leds, [first = zone_config.colors[0]](RGBColor x) { return x == first; }))
        {
            zone_config.numberOfColors = (uint8_t)zone_leds;
        }

        if(modes[active_mode].color_mode == MODE_COLORS_MODE_SPECIFIC)
        {
            for(uint8_t i = 0; i < zone_config.numberOfColors && i < 8; i++) // specific for MODE_COLORS_MODE_SPECIFIC
            {
                zone_config.colors[i] = modes[active_mode].colors[i];
            }
        }

        /*---------------------------------------------------------*\
        | Streamed (Direct) writes skip zones whose packet is       |
        | byte-identical to the last successful write, re-sending   |
        | every BWGPU_REFRESH_MS in case the firmware drifted. Mode |
        | changes and non-Direct modes always write every zone.     |
        \*---------------------------------------------------------*/
        const std::chrono::milliseconds refresh =
            (direct && !write_all) ? std::chrono::milliseconds(BWGPU_REFRESH_MS)
                                   : std::chrono::milliseconds(0);
        controller->SetZoneIfChanged(hardware_zone_idx, modes[active_mode].value, zone_config, now, refresh);
    }

    unsettled = any_unsettled;

    if(unsettled)
    {
        {
            std::lock_guard<std::mutex> settle_lock(settle_mutex);
            settle_wake = true;
        }
        settle_cv.notify_one();
    }
}

/*---------------------------------------------------------*\
| One settle sweep, decided under sweep_mutex (caller holds |
| it). Only the Direct-mode smoothing filter needs settling |
| — a non-Direct sweep has refresh 0 and would re-write     |
| every zone's mode packet, possibly restarting the         |
| hardware effect phase. So the settle path sweeps only     |
| while unsettled in Direct mode (or to deliver a frame     |
| dropped by the diagnostic throttle). With require_idle    |
| (the settle thread) it also yields to streamed frames     |
| that arrived within the last settle interval.             |
\*---------------------------------------------------------*/
bool RGBController_RGBFusion2BlackwellGPU::SettleSweepLocked(bool require_idle)
{
    const bool direct = (modes[active_mode].value == RGB_FUSION2_BLACKWELL_GPU_MODE_DIRECT);

    if(!throttle_dropped && !(unsettled && direct))
    {
        unsettled = false;
        return false;
    }

    if(require_idle && (clock_fn() - last_sweep) < std::chrono::milliseconds(BWGPU_SETTLE_INTERVAL_MS))
    {
        std::lock_guard<std::mutex> settle_lock(settle_mutex);
        settle_wake = true;
        return true;
    }

    SweepLocked(false);
    return unsettled;
}

bool RGBController_RGBFusion2BlackwellGPU::SettleStep()
{
    AccessMutex.lock_shared();
    bool still_unsettled;
    {
        std::lock_guard<std::mutex> sweep_lock(sweep_mutex);
        still_unsettled = SettleSweepLocked(false);
    }
    AccessMutex.unlock_shared();

    return still_unsettled;
}

void RGBController_RGBFusion2BlackwellGPU::SettleThreadFunction()
{
    std::unique_lock<std::mutex> lock(settle_mutex);

    while(!settle_stop)
    {
        settle_cv.wait(lock, [this]{ return settle_wake || settle_stop; });

        if(settle_stop)
        {
            break;
        }

        /*-----------------------------------------------------*\
        | Give streamed frames one interval to arrive; they     |
        | step the filter themselves.                           |
        \*-----------------------------------------------------*/
        settle_cv.wait_for(lock, std::chrono::milliseconds(BWGPU_SETTLE_INTERVAL_MS),
                           [this]{ return settle_stop; });

        if(settle_stop)
        {
            break;
        }

        /*-----------------------------------------------------*\
        | Clear before sweeping: a sweep that leaves the filter |
        | unsettled (this one or a concurrent frame) sets it    |
        | again, so no wakeup is lost.                          |
        \*-----------------------------------------------------*/
        settle_wake = false;
        lock.unlock();

        /*-----------------------------------------------------*\
        | Decide and sweep under one sweep_mutex hold, so a     |
        | mode change cannot slip between the check and the    |
        | write (see SettleSweepLocked).                        |
        \*-----------------------------------------------------*/
        AccessMutex.lock_shared();
        {
            std::lock_guard<std::mutex> sweep_lock(sweep_mutex);
            SettleSweepLocked(true);
        }
        AccessMutex.unlock_shared();

        lock.lock();
    }
}

void RGBController_RGBFusion2BlackwellGPU::StopSettleThread()
{
    {
        std::lock_guard<std::mutex> settle_lock(settle_mutex);
        settle_stop = true;
    }
    settle_cv.notify_all();

    if(settle_thread.joinable())
    {
        settle_thread.join();
    }
}

void RGBController_RGBFusion2BlackwellGPU::SetClockForTest(ClockFn test_clock)
{
    StopSettleThread();

    std::lock_guard<std::mutex> sweep_lock(sweep_mutex);
    clock_fn = test_clock;
}

void RGBController_RGBFusion2BlackwellGPU::DeviceUpdateZoneLEDs(int /*zone*/)
{
    DeviceUpdateLEDs();
}

void RGBController_RGBFusion2BlackwellGPU::DeviceUpdateSingleLED(int /*led*/)
{
    DeviceUpdateLEDs();
}

void RGBController_RGBFusion2BlackwellGPU::DeviceUpdateMode()
{
    /*-----------------------------------------------------*\
    | A mode change supersedes any pending settle left from |
    | Direct frames; the full sweep below re-arms one only  |
    | if the new mode is Direct and still unsettled.        |
    \*-----------------------------------------------------*/
    {
        std::lock_guard<std::mutex> settle_lock(settle_mutex);
        settle_wake = false;
    }

    std::lock_guard<std::mutex> sweep_lock(sweep_mutex);
    unsettled        = false;
    throttle_dropped = false;
    force_write      = true;
    SweepLocked(false);
}

void RGBController_RGBFusion2BlackwellGPU::DeviceSaveMode()
{
    controller->SaveConfig();
}
