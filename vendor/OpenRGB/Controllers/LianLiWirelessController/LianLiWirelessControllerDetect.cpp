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

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "DetectionManager.h"
#include "LogManager.h"
#include "LianLiWirelessController.h"
#include "LianLiWirelessDiagnostics.h"
#include "LianLiWirelessService.h"
#include "LianLiWirelessTransport.h"
#include "RGBController_LianLiWireless.h"

using namespace LianLiWireless;

/*---------------------------------------------------------*\
| How long the detector polls the RX GetDev table for fan   |
| group sightings. Records are transient — a few seconds    |
| of merged polls are needed for a reliable picture.        |
| Rescans read the running worker's own discovery rows      |
| instead, accepting groups seen within the rescan age.     |
\*---------------------------------------------------------*/
#define LLW_DISCOVERY_WINDOW_MS     5000
#define LLW_DISCOVERY_STEP_MS       200
#define LLW_RESCAN_SIGHTING_AGE_MS  15000

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
    std::vector<WirelessGroupInfo>              groups;     /* guarded by wireless_ctx_mutex */
    std::unique_ptr<LianLiWirelessService>      service;
};

/*---------------------------------------------------------*\
| wireless_ctx_mutex only publishes/reads the context and   |
| its group list — it is never held across USB I/O, so the |
| SDK status reply (CollectStatusJson) cannot be blocked by |
| the 5 s first-detection discovery window. Detection runs  |
| are serialized by wireless_detect_mutex instead.          |
\*---------------------------------------------------------*/
static std::shared_ptr<WirelessServiceContext>    wireless_ctx;
static std::mutex                                 wireless_ctx_mutex;
static std::mutex                                 wireless_detect_mutex;
static std::atomic<bool>                          wireless_detecting{false};

/*---------------------------------------------------------*\
| Poll discovery for window_ms and merge sightings by MAC   |
| (complete rows only, modal fan count — the same validity  |
| rule the runtime applies). Runs on the detector thread —  |
| only valid while no service worker drives the link.       |
\*---------------------------------------------------------*/
static std::vector<Sighting> PollSightings(IWirelessLink& link, IWirelessClock& clock,
                                           uint32_t window_ms)
{
    SightingAccumulator merged;
    auto deadline = std::chrono::steady_clock::now()
                  + std::chrono::milliseconds(window_ms);

    while(std::chrono::steady_clock::now() < deadline)
    {
        std::vector<uint8_t> resp;

        if(link.PollDiscovery(resp))
        {
            merged.AddResponse(resp, clock.NowMs());
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(LLW_DISCOVERY_STEP_MS));
    }

    return merged.Snapshot(clock.NowMs(), (uint64_t)window_ms + 1000);
}

/*---------------------------------------------------------*\
| Groups this backend may drive: bound to our master, a     |
| validated fan variant, and no motherboard RGB sync.       |
\*---------------------------------------------------------*/
static bool AcceptGroup(const Sighting& s, const Mac& master, WirelessFanInfo& info)
{
    info = ClassifyFanType(s.fan_types[0]);

    if(s.master != master)
    {
        return false;   /* bound to a different master        */
    }
    if(!info.supported)
    {
        return false;   /* fan variant not validated yet      */
    }
    if(s.mb_rgb_sync)
    {
        LOG_WARNING("[Lian Li Wireless] group %02X:%02X:%02X:%02X:%02X:%02X reports motherboard RGB sync — refusing writes",
                    s.mac[0], s.mac[1], s.mac[2], s.mac[3], s.mac[4], s.mac[5]);
        return false;
    }
    return true;
}

/*---------------------------------------------------------*\
| First detection: open the pair, read the master, poll a   |
| discovery window, start the service. Builds the context   |
| without holding wireless_ctx_mutex; null if no supported  |
| bound group was sighted (the dongles are released again). |
\*---------------------------------------------------------*/
static std::shared_ptr<WirelessServiceContext> BuildWirelessContext()
{
    unsigned tx_count = 0, rx_count = 0;

    if(!CountWirelessDongles(tx_count, rx_count) || tx_count != 1 || rx_count != 1)
    {
        return nullptr;
    }

    std::shared_ptr<WirelessServiceContext> ctx = std::make_shared<WirelessServiceContext>();

    if(!ctx->link.ReadMasterMac(ctx->master))
    {
        LOG_WARNING("[Lian Li Wireless] dongle pair found but master MAC unreadable: %s — is L-Connect running?",
                    ctx->link.LastError());
        return nullptr;
    }

    std::vector<Sighting> sightings = PollSightings(ctx->link, ctx->clock, LLW_DISCOVERY_WINDOW_MS);

    for(const Sighting& s : sightings)
    {
        WirelessFanInfo info;

        if(AcceptGroup(s, ctx->master, info))
        {
            ctx->groups.push_back({ s.mac, s.fan_count, info });
        }
    }

    if(ctx->groups.empty())
    {
        LOG_WARNING("[Lian Li Wireless] no bound supported fan groups sighted (last error: %s)",
                    ctx->link.LastError());
        return nullptr;
    }

    ctx->service = std::make_unique<LianLiWirelessService>(ctx->link, ctx->clock);

    for(const WirelessGroupInfo& g : ctx->groups)
    {
        ctx->service->AddGroup(g.mac, g.fan_count);
    }

    ctx->service->Start();
    return ctx;
}

/*---------------------------------------------------------*\
| Rescan: merge groups the running worker has sighted since |
| (powered on later, or missed in the first window) and     |
| correct a tracked group's fan count when the modal count  |
| disagrees. The worker owns the link; this only reads its  |
| accumulated rows. Removal is conservative: a group that   |
| vanished keeps its controller (the runtime reports Lost). |
\*---------------------------------------------------------*/
static void MergeRescanSightings(WirelessServiceContext& ctx)
{
    /*-----------------------------------------------------*\
    | Follow an operator-adopted transmitter: when new      |
    | intent re-armed a transmitter-identity wedge, the     |
    | runtime adopted the transmitter present then. Accept  |
    | groups on that master from now on, so a group bound   |
    | to it is added instead of driven only by an existing  |
    | runtime. Never changes without that operator action.  |
    | Written under the context mutex (CollectStatusJson    |
    | reads it); rescans are serialized by the detect mutex.|
    \*-----------------------------------------------------*/
    Mac adopted{};
    if(ctx.service->AdoptedMaster(adopted) && adopted != ctx.master)
    {
        LOG_INFO("[Lian Li Wireless] rescan follows adopted transmitter %02X:%02X:%02X:%02X:%02X:%02X",
                 adopted[0], adopted[1], adopted[2], adopted[3], adopted[4], adopted[5]);
        std::lock_guard<std::mutex> lock(wireless_ctx_mutex);
        ctx.master = adopted;
    }

    std::vector<Sighting> recent = ctx.service->RecentSightings(LLW_RESCAN_SIGHTING_AGE_MS);

    for(const Sighting& s : recent)
    {
        WirelessFanInfo info;

        if(!AcceptGroup(s, ctx.master, info))
        {
            continue;
        }

        bool    known       = false;
        uint8_t known_fans  = 0;
        {
            std::lock_guard<std::mutex> lock(wireless_ctx_mutex);
            for(const WirelessGroupInfo& g : ctx.groups)
            {
                if(g.mac == s.mac)
                {
                    known      = true;
                    known_fans = g.fan_count;
                }
            }
        }

        if(!known)
        {
            if(ctx.service->AddGroup(s.mac, s.fan_count))
            {
                LOG_INFO("[Lian Li Wireless] rescan added group %02X:%02X:%02X:%02X:%02X:%02X (%u fans)",
                         s.mac[0], s.mac[1], s.mac[2], s.mac[3], s.mac[4], s.mac[5], s.fan_count);
            }
            std::lock_guard<std::mutex> lock(wireless_ctx_mutex);
            ctx.groups.push_back({ s.mac, s.fan_count, info });
        }
        else if(known_fans != s.fan_count)
        {
            LOG_INFO("[Lian Li Wireless] rescan corrected group %02X:%02X:%02X:%02X:%02X:%02X fan count %u -> %u",
                     s.mac[0], s.mac[1], s.mac[2], s.mac[3], s.mac[4], s.mac[5], known_fans, s.fan_count);
            ctx.service->UpdateGroupFans(s.mac, s.fan_count);
            std::lock_guard<std::mutex> lock(wireless_ctx_mutex);
            for(WirelessGroupInfo& g : ctx.groups)
            {
                if(g.mac == s.mac)
                {
                    g.fan_count = s.fan_count;
                    g.info      = info;
                }
            }
        }
    }
}

DetectedControllers DetectLianLiWireless()
{
    DetectedControllers             detected_controllers;
    std::lock_guard<std::mutex>     detect_lock(wireless_detect_mutex);

    std::shared_ptr<WirelessServiceContext> ctx;
    {
        std::lock_guard<std::mutex> lock(wireless_ctx_mutex);
        ctx = wireless_ctx;
    }

    if(ctx == nullptr)
    {
        wireless_detecting = true;
        ctx = BuildWirelessContext();
        wireless_detecting = false;

        if(ctx == nullptr)
        {
            return detected_controllers;
        }

        std::lock_guard<std::mutex> lock(wireless_ctx_mutex);
        wireless_ctx = ctx;
    }
    else
    {
        MergeRescanSightings(*ctx);
    }

    std::vector<WirelessGroupInfo> groups;
    {
        std::lock_guard<std::mutex> lock(wireless_ctx_mutex);
        groups = ctx->groups;
    }

    for(const WirelessGroupInfo& g : groups)
    {
        LianLiWirelessController*       controller     = new LianLiWirelessController(ctx, *ctx->service, g.mac, g.fan_count, g.info);
        RGBController_LianLiWireless*   rgb_controller = new RGBController_LianLiWireless(controller);

        detected_controllers.push_back(rgb_controller);
    }

    return detected_controllers;
}

REGISTER_DETECTOR("Lian Li Wireless", DetectLianLiWireless);

/*---------------------------------------------------------*\
|| CollectStatusJson — the SDK wireless-status reply body  ||
|| (NET_PACKET_ID_LIANLI_WIRELESS_STATUS). Reads the       ||
|| service through the persistent detection context, so    ||
|| the surface stays live across device-list rescans that  ||
|| rebuild the RGBControllers. GetStatus reads the         ||
|| worker's published snapshot — never drives the link and ||
|| never waits for a tick; the context mutex is never held ||
|| across detection I/O, so the reply is always prompt.    ||
\*---------------------------------------------------------*/
nlohmann::json LianLiWireless::CollectStatusJson()
{
    std::lock_guard<std::mutex> lock(wireless_ctx_mutex);

    if(wireless_ctx == nullptr || wireless_ctx->service == nullptr)
    {
        return nlohmann::json::object({
            { "available", false                     },
            { "detecting", wireless_detecting.load() },
            { "groups",    nlohmann::json::array()   },
        });
    }

    nlohmann::json groups = nlohmann::json::array();
    for(const WirelessGroupInfo& g : wireless_ctx->groups)
    {
        LianLiWirelessService::GroupStatus st;
        if(wireless_ctx->service->GetStatus(g.mac, st))
        {
            nlohmann::json gj = GroupStatusToJson(g.mac, st);
            gj["fan_type"]  = g.info.name;
            gj["fan_count"] = g.fan_count;
            groups.push_back(std::move(gj));
        }
    }

    return nlohmann::json::object({
        { "available", true                                  },
        { "master",    MacToString(wireless_ctx->master)     },
        { "groups",    std::move(groups)                     },
        { "counters",  CountersToJson(
                          Diagnostics::Get().Snapshot())     },
    });
}
