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

OutputState OutputControl::CommitLocked()
{
    OutputState s;
    s.master       = master_;
    s.blackout     = blackout_;
    s.hold         = hold_;
    s.contributors = contributors_;
    s.effective    = Arbitrate(master_, blackout_, contributors_);
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
        if(it != contributors_.end() && it->second == v)
        {
            return false;
        }
        contributors_[name] = v;
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
        s = CommitLocked();
    }
    Notify(s);
    return true;
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
    OutputState s;
    s.master       = master_;
    s.blackout     = blackout_;
    s.hold         = hold_;
    s.contributors = contributors_;
    s.effective    = Arbitrate(master_, blackout_, contributors_);
    return s;
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
