/*---------------------------------------------------------*\
|| LianLiWirelessService.h                                   ||
||                                                           ||
||   Owns one TX/RX dongle pair for the lifetime of the      ||
||   driver. All link access is serialized on a single       ||
||   worker thread that drives one WirelessRuntime per fan   ||
||   group — fan-group adapters share this service and never ||
||   open the dongles independently.                         ||
||                                                           ||
||   This file is part of the OpenRGB project                ||
||   SPDX-License-Identifier: GPL-2.0-or-later               ||
\*---------------------------------------------------------*/

#pragma once

#include "LianLiWirelessRuntime.h"
#include <atomic>
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace LianLiWireless
{
    class LianLiWirelessService
    {
    public:
        struct GroupStatus
        {
            WirelessState state         = WirelessState::Boot;
            bool          confirmed     = false;
            size_t        resends       = 0;
            size_t        send_failures = 0;
            std::string   last_error;
            bool          have_sighting = false;
            Sighting      latest{};
        };

        /*---------------------------------------------------------*\
        | link/clock are owned by the caller and must outlive the   |
        | service — tests inject fakes, production passes the USB   |
        | link and system clock.                                    |
        \*---------------------------------------------------------*/
        LianLiWirelessService(IWirelessLink& link, IWirelessClock& clock,
                              WirelessRuntimeConfig cfg = {});
        ~LianLiWirelessService();

        LianLiWirelessService(const LianLiWirelessService&) = delete;
        LianLiWirelessService& operator=(const LianLiWirelessService&) = delete;

        /* returns false when the group is already tracked */
        bool AddGroup(const Mac& mac, uint8_t fan_count);
        void RemoveGroup(const Mac& mac);
        std::vector<Mac> Groups() const;

        /* newest-frame-wins; unknown MAC is ignored */
        void SetDesired(const Mac& mac, std::shared_ptr<const RgbUpload> upload);
        void ClearDesired(const Mac& mac);

        bool GetStatus(const Mac& mac, GroupStatus& out) const;

        /*---------------------------------------------------------*\
        | Worker thread: StepAll every tick_ms until Stop. Stop     |
        | cancels every runtime and joins the thread.               |
        \*---------------------------------------------------------*/
        void Start(uint32_t tick_ms = 50);
        void Stop();

        /* one Tick for every group; called by the worker, safe for tests */
        void StepAll();

    private:
        IWirelessLink&        link;
        IWirelessClock&       clock;
        WirelessRuntimeConfig cfg;

        mutable std::mutex    mutex;
        std::map<Mac, std::unique_ptr<WirelessRuntime>> runtimes;
        std::thread           worker;
        std::atomic<bool>     stop{false};
        std::mutex            wake_mutex;
        std::condition_variable wake_cv;
        std::atomic<bool>     wake_requested{false};
    };
}
