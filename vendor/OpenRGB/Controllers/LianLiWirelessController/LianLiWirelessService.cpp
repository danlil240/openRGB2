/*---------------------------------------------------------*\
|| LianLiWirelessService.cpp                                 ||
||                                                           ||
||   This file is part of the OpenRGB project                ||
||   SPDX-License-Identifier: GPL-2.0-or-later               ||
\*---------------------------------------------------------*/

#include "LianLiWirelessService.h"
#include <chrono>
#include <cstdlib>
#include <fstream>

using namespace LianLiWireless;

namespace
{
    using TraceClock = std::chrono::steady_clock;

    void TraceService(const char* event, long long first,
                      long long second, long long third)
    {
        static const char* trace_path = std::getenv("OPENRGB_LIANLI_TRACE");
        if(trace_path == nullptr || *trace_path == '\0')
        {
            return;
        }

        static std::mutex trace_mutex;
        static const TraceClock::time_point origin = TraceClock::now();
        static std::ofstream trace(trace_path, std::ios::out | std::ios::trunc);
        static unsigned int rows = 0;

        std::lock_guard<std::mutex> lock(trace_mutex);
        if(!trace.is_open())
        {
            return;
        }
        if(rows == 0)
        {
            trace << "time_us,event,first_us,second_us,third\n";
        }
        const long long time_us = std::chrono::duration_cast<std::chrono::microseconds>(
            TraceClock::now() - origin).count();
        trace << time_us << ',' << event << ',' << first << ','
              << second << ',' << third << '\n';
        if((++rows % 20) == 0)
        {
            trace.flush();
        }
    }
}

LianLiWirelessService::LianLiWirelessService(IWirelessLink& link_in,
                                             IWirelessClock& clock_in,
                                             WirelessRuntimeConfig cfg_in)
    : link(link_in), clock(clock_in), cfg(cfg_in)
{
}

LianLiWirelessService::~LianLiWirelessService()
{
    Stop();
}

bool LianLiWirelessService::AddGroup(const Mac& mac, uint8_t fan_count)
{
    std::lock_guard<std::mutex> lock(mutex);
    if(runtimes.count(mac) != 0)
    {
        return false;
    }
    auto rt = std::make_unique<WirelessRuntime>(link, clock, cfg);
    rt->SetTarget(mac, fan_count);
    runtimes[mac] = std::move(rt);
    return true;
}

void LianLiWirelessService::RemoveGroup(const Mac& mac)
{
    std::lock_guard<std::mutex> lock(mutex);
    auto it = runtimes.find(mac);
    if(it != runtimes.end())
    {
        it->second->Cancel();
        runtimes.erase(it);
    }
}

std::vector<Mac> LianLiWirelessService::Groups() const
{
    std::lock_guard<std::mutex> lock(mutex);
    std::vector<Mac> out;
    out.reserve(runtimes.size());
    for(const auto& kv : runtimes)
    {
        out.push_back(kv.first);
    }
    return out;
}

void LianLiWirelessService::SetDesired(const Mac& mac,
                                       std::shared_ptr<const RgbUpload> upload)
{
    const auto begin = TraceClock::now();
    const long long packets = upload ? (long long)upload->PacketCount() : 0;
    bool accepted = false;
    TraceClock::time_point acquired;
    TraceClock::time_point finished;
    {
        std::lock_guard<std::mutex> lock(mutex);
        acquired = TraceClock::now();
        auto it = runtimes.find(mac);
        if(it != runtimes.end())
        {
            it->second->SetDesired(std::move(upload));
            accepted = true;
        }
        finished = TraceClock::now();
    }
    TraceService("set_desired",
        std::chrono::duration_cast<std::chrono::microseconds>(acquired - begin).count(),
        std::chrono::duration_cast<std::chrono::microseconds>(finished - acquired).count(),
        packets);
    if(accepted)
    {
        wake_requested = true;
        wake_cv.notify_one();
    }
}

void LianLiWirelessService::ClearDesired(const Mac& mac)
{
    std::lock_guard<std::mutex> lock(mutex);
    auto it = runtimes.find(mac);
    if(it != runtimes.end())
    {
        it->second->ClearDesired();
    }
}

bool LianLiWirelessService::GetStatus(const Mac& mac, GroupStatus& out) const
{
    std::lock_guard<std::mutex> lock(mutex);
    auto it = runtimes.find(mac);
    if(it == runtimes.end())
    {
        return false;
    }
    WirelessRuntime& rt = *it->second;
    out.state          = rt.State();
    out.confirmed      = rt.Confirmed();
    out.resends        = rt.ResendCount();
    out.send_failures  = rt.SendFailures();
    out.last_error     = rt.LastError();
    const Sighting* s  = rt.Latest();
    out.have_sighting  = (s != nullptr);
    if(s != nullptr)
    {
        out.latest = *s;
    }
    return true;
}

void LianLiWirelessService::StepAll()
{
    const auto begin = TraceClock::now();
    TraceClock::time_point acquired;
    TraceClock::time_point finished;
    long long group_count = 0;
    {
        std::lock_guard<std::mutex> lock(mutex);
        acquired = TraceClock::now();
        group_count = (long long)runtimes.size();
        for(auto& kv : runtimes)
        {
            kv.second->Tick();
        }
        finished = TraceClock::now();
    }
    TraceService("step_all",
        std::chrono::duration_cast<std::chrono::microseconds>(acquired - begin).count(),
        std::chrono::duration_cast<std::chrono::microseconds>(finished - acquired).count(),
        group_count);
}

void LianLiWirelessService::Start(uint32_t tick_ms)
{
    if(worker.joinable())
    {
        return;
    }
    stop = false;
    wake_requested = false;
    worker = std::thread([this, tick_ms]
    {
        while(!stop.load())
        {
            StepAll();
            std::unique_lock<std::mutex> wait_lock(wake_mutex);
            wake_cv.wait_for(wait_lock, std::chrono::milliseconds(tick_ms),
                [this]
                {
                    return stop.load() || wake_requested.exchange(false);
                });
        }
    });
}

void LianLiWirelessService::Stop()
{
    stop = true;
    wake_cv.notify_all();
    if(worker.joinable())
    {
        worker.join();
    }
    std::lock_guard<std::mutex> lock(mutex);
    for(auto& kv : runtimes)
    {
        kv.second->Cancel();
    }
}
