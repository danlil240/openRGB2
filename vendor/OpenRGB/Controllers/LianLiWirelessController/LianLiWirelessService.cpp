/*---------------------------------------------------------*\
|| LianLiWirelessService.cpp                                 ||
||                                                           ||
||   This file is part of the OpenRGB project                ||
||   SPDX-License-Identifier: GPL-2.0-or-later               ||
\*---------------------------------------------------------*/

#include "LianLiWirelessService.h"
#include "LianLiWirelessDiagnostics.h"
#include <algorithm>
#include <chrono>

using namespace LianLiWireless;

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
    {
        std::lock_guard<std::mutex> pending_lock(pending_mutex);
        pending.emplace(mac, PendingUpload{});
    }
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
    {
        std::lock_guard<std::mutex> pending_lock(pending_mutex);
        pending.erase(mac);
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

uint64_t LianLiWirelessService::BeginMode(const Mac& mac, bool playback)
{
    std::lock_guard<std::mutex> lock(pending_mutex);
    auto it = pending.find(mac);
    if(it == pending.end())
        return 0;
    auto& slot = it->second;
    slot.mode_selected = true;
    slot.playback = playback;
    ++slot.revision;
    if(slot.revision == 0) ++slot.revision;
    slot.upload.reset();
    slot.queued = true;
    wake_requested = true;
    wake_cv.notify_one();
    return slot.revision;
}

void LianLiWirelessService::SetDesired(const Mac& mac,
                                       std::shared_ptr<const RgbUpload> upload,
                                       uint64_t revision)
{
    using TraceClock = std::chrono::steady_clock;

    const auto begin = TraceClock::now();
    const long long packets = upload ? (long long)upload->PacketCount() : 0;
    bool accepted = false;
    bool replaced_pending = false;
    TraceClock::time_point acquired;
    TraceClock::time_point finished;
    {
        std::lock_guard<std::mutex> lock(pending_mutex);
        acquired = TraceClock::now();
        auto it = pending.find(mac);
        if(it != pending.end()
           && (!it->second.mode_selected
               || (revision == it->second.revision && upload
                   && (upload->FrameCount() > 1) == it->second.playback)))
        {
            replaced_pending = it->second.queued;
            it->second.upload = std::move(upload);
            it->second.queued = true;
            accepted = true;
        }
        finished = TraceClock::now();
    }
    Diagnostics::Get().RecordSetDesired(
        accepted, replaced_pending,
        (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(
            acquired - begin).count(),
        (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(
            finished - acquired).count(),
        (uint64_t)packets);
    if(accepted)
    {
        wake_requested = true;
        wake_cv.notify_one();
    }
}

void LianLiWirelessService::ClearDesired(const Mac& mac)
{
    std::lock_guard<std::mutex> lock(pending_mutex);
    auto it = pending.find(mac);
    if(it != pending.end())
    {
        it->second.upload.reset();
        it->second.queued = true;
        wake_requested = true;
        wake_cv.notify_one();
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
    out = GroupStatus{};
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
    out.rgb_clock = rt.ClockSnapshot();
    if(const RgbUpload* upload = rt.DesiredUpload())
    {
        out.frame_count = upload->FrameCount();
        const RgbTiming timing = upload->Timing();
        out.interval_hundredths = uint32_t(timing.interval_ticks) * 100
                                   + timing.interval_fraction;
        if(out.interval_hundredths != 0)
        {
            out.effective_cycle_ms = double(out.frame_count)
                                     * out.interval_hundredths / 160.0;
            out.nominal_fps = 160000.0 / out.interval_hundredths;
        }
        out.compressed_bytes = upload->CompressedBytes();
        out.effect_id = upload->EffectId();
    }
    return true;
}

void LianLiWirelessService::StepAll()
{
    using TraceClock = std::chrono::steady_clock;

    const auto begin = TraceClock::now();
    TraceClock::time_point acquired;
    TraceClock::time_point finished;
    long long group_count = 0;
    struct Change
    {
        Mac mac;
        std::shared_ptr<const RgbUpload> upload;
        uint64_t revision;
    };
    std::vector<Change> changes;
    {
        std::lock_guard<std::mutex> pending_lock(pending_mutex);
        for(auto& kv : pending)
        {
            if(kv.second.queued)
            {
                changes.push_back({kv.first, std::move(kv.second.upload),
                                   kv.second.revision});
                kv.second.queued = false;
            }
        }
    }
    {
        std::lock_guard<std::mutex> lock(mutex);
        acquired = TraceClock::now();
        group_count = (long long)runtimes.size();
        for(auto& kv : runtimes)
        {
            for(auto& change : changes)
            {
                if(change.mac != kv.first) continue;
                bool current = false;
                {
                    std::lock_guard<std::mutex> pending_lock(pending_mutex);
                    auto slot = pending.find(change.mac);
                    current = slot != pending.end()
                           && slot->second.revision == change.revision;
                }
                if(current)
                {
                    if(change.upload)
                        kv.second->SetDesired(std::move(change.upload));
                    else
                        kv.second->ClearDesired();
                }
                break;
            }
            kv.second->Tick();
        }
        finished = TraceClock::now();
    }
    Diagnostics::Get().RecordStepAll(
        (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(
            acquired - begin).count(),
        (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(
            finished - acquired).count(),
        (uint64_t)group_count);
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
            uint64_t now = clock.NowMs();
            uint64_t due = now + tick_ms;
            {
                std::lock_guard<std::mutex> lock(mutex);
                for(const auto& kv : runtimes)
                    due = std::min(due, kv.second->NextDeadlineMs());
            }
            const uint64_t wait_ms = due > now ? due - now : 1;
            std::unique_lock<std::mutex> wait_lock(wake_mutex);
            wake_cv.wait_for(wait_lock, std::chrono::milliseconds(wait_ms),
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
