/*---------------------------------------------------------*\
|| RestartLedger.h                                           ||
||                                                           ||
||   R-9: the bounded restart budget both supervisors use   ||
||   (UI -> engine, engine -> backend). Pure bookkeeping:  ||
||   no clock of its own, no I/O — callers pass `now`, so  ||
||   the suites drive it with a fake clock.                 ||
||                                                           ||
||   Budget: at most max_restarts inside any sliding        ||
||   `window`. The delay before the next restart is         ||
||   backoff[n], n = restarts already inside the window     ||
||   (clamped to the last entry).                           ||
||                                                           ||
||   Header-only, std-only: the UI includes it without     ||
||   linking anything from the engine.                      ||
||                                                           ||
||   SPDX-License-Identifier: GPL-2.0-or-later               ||
\*---------------------------------------------------------*/

#pragma once

#include <chrono>
#include <cstddef>
#include <deque>
#include <vector>

namespace openrgb2
{
namespace supervise
{

template<typename ClockT = std::chrono::steady_clock>
class BasicRestartLedger
{
public:
    using Duration  = typename ClockT::duration;
    using TimePoint = typename ClockT::time_point;

    struct Budget
    {
        size_t                max_restarts = 3;
        Duration              window       = std::chrono::minutes(10);
        std::vector<Duration> backoff;     /* empty = no delay */
    };

    BasicRestartLedger() = default;
    explicit BasicRestartLedger(Budget b) : budget_(std::move(b)) {}

    const Budget& GetBudget() const { return budget_; }

    /* Restarts recorded inside the window ending at `now`. */
    size_t CountInWindow(TimePoint now) const
    {
        size_t n = 0;
        for(const TimePoint& t : stamps_)
        {
            if(now - t < budget_.window)
            {
                ++n;
            }
        }
        return n;
    }

    /* No restart may be recorded at `now`. */
    bool Exhausted(TimePoint now) const
    {
        return CountInWindow(now) >= budget_.max_restarts;
    }

    /* Delay to wait before the next restart. */
    Duration NextDelay(TimePoint now) const
    {
        if(budget_.backoff.empty())
        {
            return Duration::zero();
        }
        size_t n = CountInWindow(now);
        if(n >= budget_.backoff.size())
        {
            n = budget_.backoff.size() - 1;
        }
        return budget_.backoff[n];
    }

    void Record(TimePoint now)
    {
        stamps_.push_back(now);
        ++total_;
        /* Bounded: only stamps that can still count are kept. */
        while(!stamps_.empty() &&
              now - stamps_.front() >= budget_.window)
        {
            stamps_.pop_front();
        }
    }

    /* Every restart ever recorded (diagnostics). */
    size_t Total() const { return total_; }

private:
    Budget                budget_;
    std::deque<TimePoint> stamps_;
    size_t                total_ = 0;
};

using RestartLedger = BasicRestartLedger<>;

} /* namespace supervise */
} /* namespace openrgb2 */
