/*---------------------------------------------------------*\
|| LianLiWirelessControllerDetect.cpp                        ||
||                                                           ||
||   Detector for Lian Li wireless fan groups driven         ||
||   through the TX/RX dongle pair (0416:8040 / 0416:8041).  ||
||                                                           ||
||   One LianLiWirelessService owns the pair for the life    ||
||   of the process — the detection context is kept in a     ||
||   static shared_ptr so rescans rebuild RGBControllers     ||
||   without reopening the dongles or restarting the         ||
||   keep-alive/discovery worker.                            ||
||                                                           ||
||   This file is part of the OpenRGB project                ||
||   SPDX-License-Identifier: GPL-2.0-or-later               ||
\*---------------------------------------------------------*/

#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "DetectionManager.h"
#include "LogManager.h"
#include "LianLiWirelessController.h"
#include "LianLiWirelessService.h"
#include "LianLiWirelessTransport.h"
#include "RGBController_LianLiWireless.h"

using namespace LianLiWireless;

/*---------------------------------------------------------*\
| How long the detector polls the RX GetDev table for fan   |
| group sightings. Records are transient — a few seconds    |
| of merged polls are needed for a reliable picture.        |
\*---------------------------------------------------------*/
#define LLW_DISCOVERY_WINDOW_MS     5000
#define LLW_DISCOVERY_STEP_MS       200

struct WirelessGroupInfo
{
    Mac                 mac;
    uint8_t             fan_count;
    WirelessFanInfo     info;
};

struct WirelessServiceContext
{
    /*-----------------------------------------------------*\
    | Member order matters: the service holds references    |
    | to link and clock, so it is declared last and         |
    | destroyed first.                                      |
    \*-----------------------------------------------------*/
    LianLiWirelessUsbLink                       link;
    SystemWirelessClock                         clock;
    Mac                                         master{};
    std::vector<WirelessGroupInfo>              groups;
    std::unique_ptr<LianLiWirelessService>      service;
};

static std::shared_ptr<WirelessServiceContext>    wireless_ctx;
static std::mutex                                 wireless_ctx_mutex;

/*---------------------------------------------------------*\
| Poll discovery for window_ms and merge sightings by MAC.  |
| Runs on the detector thread — only valid while the        |
| service worker is not driving the link.                   |
\*---------------------------------------------------------*/
static std::vector<Sighting> PollSightings(IWirelessLink& link, uint32_t window_ms)
{
    std::vector<Sighting> merged;
    auto deadline = std::chrono::steady_clock::now()
                  + std::chrono::milliseconds(window_ms);

    while(std::chrono::steady_clock::now() < deadline)
    {
        std::vector<uint8_t> resp;

        if(link.PollDiscovery(resp))
        {
            try
            {
                for(const Sighting& s : ParseDiscoveryResponse(resp.data(), resp.size()))
                {
                    bool known = false;

                    for(Sighting& m : merged)
                    {
                        if(m.mac == s.mac)
                        {
                            m       = s;
                            known   = true;
                        }
                    }

                    if(!known)
                    {
                        merged.push_back(s);
                    }
                }
            }
            catch(const std::invalid_argument&) {}
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(LLW_DISCOVERY_STEP_MS));
    }

    return merged;
}

DetectedControllers DetectLianLiWireless()
{
    DetectedControllers             detected_controllers;
    std::lock_guard<std::mutex>     lock(wireless_ctx_mutex);

    if(wireless_ctx == nullptr)
    {
        unsigned tx_count = 0, rx_count = 0;

        if(!CountWirelessDongles(tx_count, rx_count) || tx_count != 1 || rx_count != 1)
        {
            return detected_controllers;
        }

        std::shared_ptr<WirelessServiceContext> ctx = std::make_shared<WirelessServiceContext>();

        if(!ctx->link.ReadMasterMac(ctx->master))
        {
            LOG_WARNING("[Lian Li Wireless] dongle pair found but master MAC unreadable: %s — is L-Connect running?",
                        ctx->link.LastError());
            return detected_controllers;
        }

        std::vector<Sighting> sightings = PollSightings(ctx->link, LLW_DISCOVERY_WINDOW_MS);

        for(const Sighting& s : sightings)
        {
            WirelessFanInfo info = ClassifyFanType(s.fan_types[0]);

            if(s.master != ctx->master)
            {
                continue;   /* bound to a different master        */
            }
            if(!info.supported)
            {
                continue;   /* fan variant not validated yet      */
            }
            if(s.mb_rgb_sync)
            {
                LOG_WARNING("[Lian Li Wireless] group %02X:%02X:%02X:%02X:%02X:%02X reports motherboard RGB sync — refusing writes",
                            s.mac[0], s.mac[1], s.mac[2], s.mac[3], s.mac[4], s.mac[5]);
                continue;
            }

            ctx->groups.push_back({ s.mac, s.fan_count, info });
        }

        if(ctx->groups.empty())
        {
            LOG_WARNING("[Lian Li Wireless] no bound supported fan groups sighted (last error: %s)",
                        ctx->link.LastError());
            return detected_controllers;
        }

        ctx->service = std::make_unique<LianLiWirelessService>(ctx->link, ctx->clock);

        for(const WirelessGroupInfo& g : ctx->groups)
        {
            ctx->service->AddGroup(g.mac, g.fan_count);
        }

        ctx->service->Start();
        wireless_ctx = ctx;
    }

    for(const WirelessGroupInfo& g : wireless_ctx->groups)
    {
        LianLiWirelessController*       controller     = new LianLiWirelessController(wireless_ctx, *wireless_ctx->service, g.mac, g.fan_count, g.info);
        RGBController_LianLiWireless*   rgb_controller = new RGBController_LianLiWireless(controller);

        detected_controllers.push_back(rgb_controller);
    }

    return detected_controllers;
}

REGISTER_DETECTOR("Lian Li Wireless", DetectLianLiWireless);
