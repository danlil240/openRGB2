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
#include <stdexcept>

using namespace LianLiWireless;

/*---------------------------------------------------------*\
| SightingAccumulator                                       |
\*---------------------------------------------------------*/
void SightingAccumulator::Add(const Sighting& s, uint64_t now_ms)
{
    if(!IsCompleteSighting(s))
    {
        return;     /* torn/short row: never sizes a group */
    }

    std::lock_guard<std::mutex> lock(mutex);
    Entry& e        = entries[s.mac];
    e.last          = s;
    e.last_seen_ms  = now_ms;

    /*-----------------------------------------------------*\
    | Bounded votes: halve every count once a group has     |
    | many rows so the mode follows a lasting change.       |
    \*-----------------------------------------------------*/
    uint32_t total = 0;
    for(const auto& kv : e.fan_votes)
    {
        total += kv.second;
    }
    if(total >= 64)
    {
        for(auto it = e.fan_votes.begin(); it != e.fan_votes.end();)
        {
            it->second /= 2;
            if(it->second == 0)
            {
                it = e.fan_votes.erase(it);
            }
            else
            {
                ++it;
            }
        }
    }
    e.fan_votes[s.fan_count]++;
}

void SightingAccumulator::AddResponse(const std::vector<uint8_t>& response, uint64_t now_ms)
{
    try
    {
        for(const Sighting& s : ParseDiscoveryResponse(response.data(), response.size()))
        {
            Add(s, now_ms);
        }
    }
    catch(const std::invalid_argument&)
    {
        /* malformed reply = miss */
    }
}

std::vector<Sighting> SightingAccumulator::Snapshot(uint64_t now_ms, uint64_t max_age_ms) const
{
    std::lock_guard<std::mutex> lock(mutex);
    std::vector<Sighting> out;

    for(const auto& kv : entries)
    {
        const Entry& e = kv.second;
        if(now_ms >= e.last_seen_ms && now_ms - e.last_seen_ms > max_age_ms)
        {
            continue;
        }

        uint8_t  best       = e.last.fan_count;
        uint32_t best_votes = 0;
        auto     latest     = e.fan_votes.find(e.last.fan_count);
        if(latest != e.fan_votes.end())
        {
            best_votes = latest->second;
        }
        for(const auto& vote : e.fan_votes)
        {
            if(vote.second > best_votes)
            {
                best       = vote.first;
                best_votes = vote.second;
            }
        }

        Sighting s  = e.last;
        s.fan_count = best;
        out.push_back(s);
    }
    return out;
}

/*---------------------------------------------------------*\
| LianLiWirelessService                                     |
\*---------------------------------------------------------*/
LianLiWirelessService::LianLiWirelessService(IWirelessLink& link_in,
                                             IWirelessClock& clock_in,
                                             WirelessRuntimeConfig cfg_in)
    : link(link_in), clock(clock_in), cfg(cfg_in),
      observed(link_in, clock_in, sightings)
{
}

LianLiWirelessService::~LianLiWirelessService()
{
    Stop();
}

void LianLiWirelessService::Wake()
{
    /*---------------------------------------------------------*\
    | Set the flag under wake_mutex: the worker evaluates its   |
    | wait predicate under that mutex, so a flag set between    |
    | its check and its block would otherwise be missed.        |
    \*---------------------------------------------------------*/
    {
        std::lock_guard<std::mutex> lock(wake_mutex);
        wake_requested = true;
    }
    wake_cv.notify_one();
}

bool LianLiWirelessService::AddGroup(const Mac& mac, uint8_t fan_count)
{
    std::lock_guard<std::mutex> lock(mutex);
    if(runtimes.count(mac) != 0)
    {
        return false;
    }
    auto rt = std::make_unique<WirelessRuntime>(observed, clock, cfg);
    rt->SetTarget(mac, fan_count);
    PublishStatus(mac, *rt);
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
    {
        std::lock_guard<std::mutex> status_lock(status_mutex);
        status_snapshot.erase(mac);
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

bool LianLiWirelessService::UpdateGroupFans(const Mac& mac, uint8_t fan_count)
{
    {
        std::lock_guard<std::mutex> lock(mutex);
        auto it = runtimes.find(mac);
        if(it == runtimes.end())
        {
            return false;
        }
        it->second->SetTarget(mac, fan_count);
        PublishStatus(mac, *it->second);
    }
    Wake();
    return true;
}

std::vector<Sighting> LianLiWirelessService::RecentSightings(uint64_t max_age_ms) const
{
    return sightings.Snapshot(clock.NowMs(), max_age_ms);
}

bool LianLiWirelessService::AdoptedMaster(Mac& out) const
{
    std::lock_guard<std::mutex> status_lock(status_mutex);
    if(!has_adopted_master)
    {
        return false;
    }
    out = adopted_master;
    return true;
}

uint64_t LianLiWirelessService::BeginMode(const Mac& mac, bool playback)
{
    uint64_t revision = 0;
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
        revision = slot.revision;
    }
    Wake();
    return revision;
}

uint64_t LianLiWirelessService::AdoptMode(const Mac& mac, bool playback)
{
    {
        std::lock_guard<std::mutex> lock(pending_mutex);
        auto it = pending.find(mac);
        if(it == pending.end())
            return 0;
        const PendingUpload& slot = it->second;
        if(!slot.mode_selected)
            return 0;
        if(slot.playback == playback)
            return slot.revision;
    }
    return BeginMode(mac, playback);
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
        Wake();
    }
}

void LianLiWirelessService::ClearDesired(const Mac& mac)
{
    bool found = false;
    {
        std::lock_guard<std::mutex> lock(pending_mutex);
        auto it = pending.find(mac);
        if(it != pending.end())
        {
            it->second.upload.reset();
            it->second.queued = true;
            found = true;
        }
    }
    if(found)
    {
        Wake();
    }
}

LianLiWirelessService::GroupStatus LianLiWirelessService::BuildStatus(const WirelessRuntime& rt)
{
    GroupStatus out;
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
    return out;
}

void LianLiWirelessService::PublishStatus(const Mac& mac, const WirelessRuntime& rt)
{
    GroupStatus st = BuildStatus(rt);
    std::lock_guard<std::mutex> status_lock(status_mutex);
    status_snapshot[mac] = std::move(st);
}

bool LianLiWirelessService::GetStatus(const Mac& mac, GroupStatus& out) const
{
    {
        std::lock_guard<std::mutex> status_lock(status_mutex);
        auto it = status_snapshot.find(mac);
        if(it == status_snapshot.end())
        {
            return false;
        }
        out = it->second;
    }

    /*---------------------------------------------------------*\
    | The snapshot is up to one tick old: re-age the RGB clock  |
    | sample at read time (validity only ever expires).         |
    \*---------------------------------------------------------*/
    if(out.rgb_clock.valid)
    {
        const uint64_t now = clock.NowMs();
        out.rgb_clock.age_ms = now >= out.rgb_clock.sampled_at_ms
            ? now - out.rgb_clock.sampled_at_ms : UINT64_MAX;
        out.rgb_clock.valid = out.rgb_clock.age_ms <= 6000;
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
            Mac adopted{};
            if(kv.second->TakeAdoptedMaster(adopted))
            {
                std::lock_guard<std::mutex> status_lock(status_mutex);
                adopted_master     = adopted;
                has_adopted_master = true;
            }
            PublishStatus(kv.first, *kv.second);
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
    {
        std::lock_guard<std::mutex> lock(wake_mutex);
        stop = false;
        wake_requested = false;
    }
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
    {
        std::lock_guard<std::mutex> lock(wake_mutex);
        stop = true;
    }
    wake_cv.notify_all();
    if(worker.joinable())
    {
        worker.join();
    }
    std::lock_guard<std::mutex> lock(mutex);
    for(auto& kv : runtimes)
    {
        kv.second->Cancel();
        PublishStatus(kv.first, *kv.second);
    }
}
