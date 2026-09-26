/*---------------------------------------------------------*\
|| RGBController_LianLiWireless.h                            ||
||                                                           ||
||   RGBController for one Lian Li wireless fan group        ||
||   (SL V3 and other ClassifyFanType-supported variants).   ||
||   Zones follow the physical fan layout: one linear zone   ||
||   per fan.                                                ||
||                                                           ||
||   This file is part of the OpenRGB project                ||
||   SPDX-License-Identifier: GPL-2.0-or-later               ||
\*---------------------------------------------------------*/

#pragma once

#include "LianLiWirelessController.h"
#include "RGBController.h"

class RGBController_LianLiWireless : public RGBController
{
public:
    RGBController_LianLiWireless(LianLiWirelessController* controller_ptr);
    ~RGBController_LianLiWireless();

    void SetupZones();

    void DeviceConfigureZone(int zone_idx);

    void DeviceUpdateLEDs();
    void DeviceUpdateZoneLEDs(int zone);
    void DeviceUpdateSingleLED(int led);

    void DeviceUpdateMode();

private:
    LianLiWirelessController*   controller;
};
