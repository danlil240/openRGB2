/*---------------------------------------------------------*\
|| LianLiWirelessController.cpp                              ||
||                                                           ||
||   OpenRGB device-side handle for one bound Lian Li        ||
||   wireless fan group.                                     ||
||                                                           ||
||   This file is part of the OpenRGB project                ||
||   SPDX-License-Identifier: GPL-2.0-or-later               ||
\*---------------------------------------------------------*/

#include "LianLiWirelessController.h"
#include "LianLiWirelessCodec.h"
#include <atomic>
#include <cstdio>

using namespace LianLiWireless;

/*---------------------------------------------------------*\
| Process-wide upload variant. The fan firmware ignores an  |
| effect ID it has already seen, so re-uploading identical  |
| content after a rescan or service restart still needs a   |
| fresh salt — keep the counter shared across controller    |
| instances.                                                |
\*---------------------------------------------------------*/
static std::atomic<uint32_t> upload_variant{0};

LianLiWirelessController::LianLiWirelessController(
        std::shared_ptr<void> owner_ptr, LianLiWirelessService& service_ref,
        const Mac& group_mac, uint8_t group_fan_count, WirelessFanInfo info)
    : owner(std::move(owner_ptr)),
      service(service_ref),
      mac(group_mac),
      fan_count(group_fan_count),
      fan_info(info)
{
    led_count = (uint8_t)(fan_count * fan_info.leds_per_fan);
}

void LianLiWirelessController::SetLEDs(const RGBColor* led_colors, size_t count)
{
    std::vector<uint8_t> payload(led_count * 3);

    size_t n = (count < led_count) ? count : led_count;

    for(size_t i = 0; i < n; i++)
    {
        payload[i * 3 + 0] = (uint8_t)RGBGetRValue(led_colors[i]);
        payload[i * 3 + 1] = (uint8_t)RGBGetGValue(led_colors[i]);
        payload[i * 3 + 2] = (uint8_t)RGBGetBValue(led_colors[i]);
    }

    /*---------------------------------------------------------*\
    | Static frame: single frame, 96-tick interval — same       |
    | timing as the validated smoke-test upload.                |
    \*---------------------------------------------------------*/
    RgbTiming timing;
    timing.interval_ticks = 96;

    try
    {
        auto upload = std::make_shared<RgbUpload>(
            LianLiWirelessCodec::Compress(payload.data(), payload.size()),
            led_count, 1, timing,
            (uint8_t)(upload_variant.fetch_add(1) & 0xFF));

        service.SetDesired(mac, std::move(upload));
    }
    catch(const std::exception& e)
    {
        last_error = e.what();
    }
}

void LianLiWirelessController::SetFrames(const std::vector<uint8_t>& rgb_frames,
                                       uint16_t frame_count, uint16_t interval_ms)
{
    if(frame_count == 0
       || rgb_frames.size() != (size_t)led_count * 3 * frame_count)
    {
        last_error = "animation payload does not match group layout";
        return;
    }

    /*---------------------------------------------------------*\
    | The wire format carries whole 0.625ms ticks plus a        |
    | hundredths fraction (vendor uploader semantics):          |
    | 1 ms = 1.6 ticks = 160 hundredths.                        |
    \*---------------------------------------------------------*/
    const uint32_t hundredths = (uint32_t)interval_ms * 160;

    RgbTiming timing;
    timing.interval_ticks    = (uint16_t)(hundredths / 100);
    timing.interval_fraction = (uint8_t)(hundredths % 100);

    try
    {
        auto upload = std::make_shared<RgbUpload>(
            LianLiWirelessCodec::Compress(rgb_frames.data(), rgb_frames.size()),
            led_count, frame_count, timing,
            (uint8_t)(upload_variant.fetch_add(1) & 0xFF));

        service.SetDesired(mac, std::move(upload));
    }
    catch(const std::exception& e)
    {
        last_error = e.what();
    }
}

std::string LianLiWirelessController::GetMacString() const
{
    char buf[18];
    snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return buf;
}

std::string LianLiWirelessController::GetStatus() const
{
    LianLiWirelessService::GroupStatus st;

    if(service.GetStatus(mac, st))
    {
        return st.last_error;
    }
    return last_error;
}
