/*---------------------------------------------------------*\
|| LianLiWirelessController.h                                ||
||                                                           ||
||   OpenRGB device-side handle for one bound Lian Li        ||
||   wireless fan group. All RF/USB traffic is owned by the  ||
||   shared LianLiWirelessService (one per TX/RX dongle      ||
||   pair); this class only builds compressed RGB uploads    ||
||   from OpenRGB color buffers and hands them to the        ||
||   service as the group's newest desired frame.            ||
||                                                           ||
||   RGB only — never sends fan/PWM or binding commands.     ||
||                                                           ||
||   This file is part of the OpenRGB project                ||
||   SPDX-License-Identifier: GPL-2.0-or-later               ||
\*---------------------------------------------------------*/

#pragma once

#include "LianLiWirelessFanType.h"
#include "LianLiWirelessService.h"
#include "RGBControllerInterface.h"
#include <memory>
#include <string>
#include <vector>

class LianLiWirelessController
{
public:
    /*---------------------------------------------------------*\
    | owner keeps the detection context (USB link + clock +     |
    | service) alive for as long as any controller exists —     |
    | the service holds references into the context members.    |
    \*---------------------------------------------------------*/
    LianLiWirelessController(std::shared_ptr<void> owner,
                             LianLiWireless::LianLiWirelessService& service,
                             const LianLiWireless::Mac& mac, uint8_t fan_count,
                             LianLiWireless::WirelessFanInfo fan_info);

    void        SetLEDs(const RGBColor* led_colors, size_t count);

    /*---------------------------------------------------------*\
    | Upload a pre-rendered animation loop: rgb_frames holds    |
    | frame_count frames of led_count RGB bytes, frame-major.   |
    | The firmware cycles the frames at interval_ms per frame   |
    | without further host traffic.                             |
    \*---------------------------------------------------------*/
    void        SetFrames(const std::vector<uint8_t>& rgb_frames,
                          uint16_t frame_count, uint16_t interval_ms);

    std::string GetMacString()  const;
    std::string GetFanName()    const { return fan_info.name; }
    LianLiWireless::WirelessFanType GetFanType() const { return fan_info.type; }
    uint8_t     GetFanCount()   const { return fan_count; }
    uint8_t     GetLEDsPerFan() const { return fan_info.leds_per_fan; }
    uint8_t     GetLEDCount()   const { return led_count; }
    std::string GetStatus()     const;

private:
    std::shared_ptr<void>               owner;
    LianLiWireless::LianLiWirelessService& service;
    LianLiWireless::Mac                 mac;
    uint8_t                             fan_count;
    LianLiWireless::WirelessFanInfo     fan_info;
    uint8_t                             led_count;
    std::string                         last_error;
};
