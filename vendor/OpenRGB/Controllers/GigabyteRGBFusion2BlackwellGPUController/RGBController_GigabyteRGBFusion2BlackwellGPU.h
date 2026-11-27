/*---------------------------------------------------------*\
| RGBController_GigabyteRGBFusion2BlackwellGPU.h            |
|                                                           |
|   RGBController for Gigabyte RGB Fusion 2 Blackwell GPU   |
|                                                           |
|   This file is part of the OpenRGB project                |
|   SPDX-License-Identifier: GPL-2.0-or-later               |
\*---------------------------------------------------------*/

#pragma once

#include "RGBController.h"
#include "GigabyteRGBFusion2BlackwellGPUController.h"

class RGBController_RGBFusion2BlackwellGPU : public RGBController
{
public:
    RGBController_RGBFusion2BlackwellGPU(RGBFusion2BlackwellGPUController* controller_ptr, uint8_t led_layout);
    ~RGBController_RGBFusion2BlackwellGPU();

    void        SetupZones();

    void        DeviceUpdateLEDs();
    void        DeviceUpdateZoneLEDs(int zone);
    void        DeviceUpdateSingleLED(int led);

    void        DeviceUpdateMode();
    void        DeviceSaveMode();

private:
    RGBFusion2BlackwellGPUController* controller;
    uint8_t     gpu_layout;

    /*-----------------------------------------------------*\
    | Temporal smoothing state (firmware chase-the-target   |
    | flicker workaround): last smoothed channel value per  |
    | hardware zone and LED slot. See DeviceUpdateLEDs.     |
    \*-----------------------------------------------------*/
    double                              smoothed[RGB_FUSION_2_BLACKWELL_GPU_NUMBER_OF_ZONES][RGB_FUSION_2_BLACKWELL_AORUS_MASTER_FAN_LEDS][3] = {};
    bool                                has_smoothed[RGB_FUSION_2_BLACKWELL_GPU_NUMBER_OF_ZONES] = {};
};
