/*---------------------------------------------------------*\
|| OutputControl.cpp — see OutputControl.h (R-1).            ||
||                                                           ||
||   SPDX-License-Identifier: GPL-2.0-or-later               ||
\*---------------------------------------------------------*/

#include "OutputControl.h"

#include <cmath>

namespace openrgb2
{

namespace
{

/* [0, 1]; the caller has already refused non-finite values. */
float Clamp01(float v)
{
    return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
}

} /* anonymous namespace */

float OutputControl::Arbitrate(
    float master, bool blackout,
    const std::map<std::string, float>& contributors)
{
    if(blackout)
    {
        return 0.0f;
    }
    float scale = Clamp01(std::isfinite(master) ? master : 1.0f);
    float least = 1.0f;
    for(const auto& kv : contributors)
    {
        if(kv.second < least)
        {
            least = kv.second;
        }
    }
    return scale * Clamp01(least);
}

void OutputControl::SetChanged(ChangedFn fn)
{
    std::lock_guard<std::mutex> lock(cb_mutex_);
    changed_ = std::move(fn);
}

OutputState OutputControl::SnapshotLocked() const
{
    OutputState s;
    s.master   = master_;
    s.blackout = blackout_;
    s.hold     = hold_;
    for(const auto& kv : contributors_)
    {
        const auto r = ramps_.find(kv.first);
        if(r == ramps_.end())
        {
            s.contributors[kv.first] = kv.second;
        }
        else if(!r->second.release)
        {
            s.contributors[kv.first] = r->second.to;
        }
    }
    s.ramping   = !ramps_.empty();
    s.effective = Arbitrate(master_, blackout_, contributors_);
    return s;
}

OutputState OutputControl::CommitLocked()
{
    const OutputState s = SnapshotLocked();
    effective_.store(s.effective);
    return s;
}

void OutputControl::Notify(const OutputState& s)
{
    ChangedFn fn;
    {
        std::lock_guard<std::mutex> lock(cb_mutex_);
        fn = changed_;
    }
    if(fn)
    {
        fn(s);
    }
}

bool OutputControl::SetMaster(float value)
{
    if(!std::isfinite(value))
    {
        return false;
    }
    OutputState s;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const float v = Clamp01(value);
        if(v == master_)
        {
            return false;
        }
        master_ = v;
        s = CommitLocked();
    }
    Notify(s);
    return true;
}

bool OutputControl::SetBlackout(bool on)
{
    OutputState s;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if(on == blackout_)
        {
            return false;
        }
        blackout_ = on;
        s = CommitLocked();
    }
    Notify(s);
    return true;
}

bool OutputControl::SetHold(bool on)
{
    OutputState s;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if(on == hold_)
        {
            return false;
        }
        hold_ = on;
        s = CommitLocked();
    }
    Notify(s);
    return true;
}

bool OutputControl::SetContributor(const std::string& name, float scale)
{
    if(name.empty() || !std::isfinite(scale))
    {
        return false;
    }
    OutputState s;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const float v  = Clamp01(scale);
        const auto  it = contributors_.find(name);
        if(it != contributors_.end() && it->second == v &&
           ramps_.count(name) == 0)
        {
            return false;
        }
        contributors_[name] = v;
        ramps_.erase(name);
        s = CommitLocked();
    }
    Notify(s);
    return true;
}

bool OutputControl::ClearContributor(const std::string& name)
{
    OutputState s;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if(contributors_.erase(name) == 0)
        {
            return false;
        }
        ramps_.erase(name);
        s = CommitLocked();
    }
    Notify(s);
    return true;
}

bool OutputControl::SetContributorTarget(const std::string& name,
                                         float scale, TimePoint now,
                                         std::chrono::milliseconds down,
                                         std::chrono::milliseconds up)
{
    if(name.empty() || !std::isfinite(scale))
    {
        return false;
    }
    OutputState s;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const float v  = Clamp01(scale);
        const auto  cit = contributors_.find(name);
        const float cur = cit != contributors_.end() ? cit->second
                                                     : 1.0f;
        const auto  rit = ramps_.find(name);
        if(rit != ramps_.end())
        {
            if(!rit->second.release && rit->second.to == v)
            {
                return false;          /* already heading there */
            }
        }
        else if(cit != contributors_.end() && cur == v)
        {
            return false;
        }
        const std::chrono::milliseconds dur = v < cur ? down : up;
        if(dur.count() <= 0 || v == cur)
        {
            contributors_[name] = v;
            ramps_.erase(name);
        }
        else
        {
            contributors_[name] = cur;
            ramps_[name] = Ramp{ cur, v, now, dur, false };
        }
        s = CommitLocked();
    }
    Notify(s);
    return true;
}

bool OutputControl::ReleaseContributor(const std::string& name,
                                       TimePoint now,
                                       std::chrono::milliseconds up)
{
    OutputState s;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto cit = contributors_.find(name);
        if(cit == contributors_.end())
        {
            return false;
        }
        const auto rit = ramps_.find(name);
        if(rit != ramps_.end() && rit->second.release)
        {
            return false;              /* already releasing */
        }
        const float cur = cit->second;
        if(up.count() <= 0 || cur >= 1.0f)
        {
            contributors_.erase(cit);
            ramps_.erase(name);
        }
        else
        {
            ramps_[name] = Ramp{ cur, 1.0f, now, up, true };
        }
        s = CommitLocked();
    }
    Notify(s);
    return true;
}

bool OutputControl::Tick(TimePoint now)
{
    OutputState s;
    bool completed = false;
    bool moved     = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if(ramps_.empty())
        {
            return false;
        }
        const float before = effective_.load();
        for(auto it = ramps_.begin(); it != ramps_.end();)
        {
            const Ramp& r = it->second;
            double frac = 1.0;
            if(r.dur.count() > 0)
            {
                frac = std::chrono::duration<double>(now - r.start)
                           .count() /
                       std::chrono::duration<double>(r.dur).count();
            }
            frac = frac < 0.0 ? 0.0 : (frac > 1.0 ? 1.0 : frac);
            const float v = float(r.from + (r.to - r.from) * frac);
            if(frac >= 1.0)
            {
                if(r.release)
                {
                    contributors_.erase(it->first);
                }
                else
                {
                    contributors_[it->first] = r.to;
                }
                it = ramps_.erase(it);
                completed = true;
            }
            else
            {
                contributors_[it->first] = v;
                ++it;
            }
        }
        s = CommitLocked();
        moved = s.effective != before;
    }
    if(completed)
    {
        Notify(s);
    }
    return moved;
}

bool OutputControl::AnyContributorOff(const OutputState& s)
{
    for(const auto& kv : s.contributors)
    {
        if(kv.second <= 0.0f)
        {
            return true;
        }
    }
    return false;
}

void OutputControl::Restore(const PersistedOutput& p)
{
    OutputState s;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const float m = std::isfinite(p.master) ? Clamp01(p.master)
                                                : 1.0f;
        if(m == master_ && p.blackout == blackout_)
        {
            return;
        }
        master_   = m;
        blackout_ = p.blackout;
        s = CommitLocked();
    }
    Notify(s);
}

OutputState OutputControl::State() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return SnapshotLocked();
}

PersistedOutput OutputControl::Persisted() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    PersistedOutput p;
    p.master   = master_;
    p.blackout = blackout_;
    return p;
}

} /* namespace openrgb2 */
