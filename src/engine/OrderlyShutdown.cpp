/*---------------------------------------------------------*\
||| OrderlyShutdown.cpp                                       ||
|||                                                           ||
|||   SPDX-License-Identifier: GPL-2.0-or-later               ||
|\*---------------------------------------------------------*/

#include "OrderlyShutdown.h"

#include <atomic>
#include <condition_variable>
#include <exception>
#include <mutex>
#include <thread>
#include <utility>

namespace openrgb2
{

void OrderlyShutdown::Add(std::string name, Clock::duration bound,
                          std::function<void()> fn)
{
    steps_.push_back(Step{ std::move(name), bound, std::move(fn) });
}

openrgb2::Clock::duration OrderlyShutdown::TotalBound() const
{
    Clock::duration total{};
    for(const Step& s : steps_)
    {
        total += s.bound;
    }
    return total;
}

size_t OrderlyShutdown::StepCount() const
{
    return steps_.size();
}

bool OrderlyShutdown::Ran() const
{
    return ran_;
}

void OrderlyShutdown::SetWatchdog(Clock::duration grace,
                                  WatchdogHandler on_expire)
{
    watchdog_grace_ = grace;
    watchdog_       = std::move(on_expire);
}

OrderlyShutdown::Result OrderlyShutdown::Run(
    std::function<TimePoint()> now)
{
    if(ran_)
    {
        return result_;
    }
    ran_ = true;
    if(!now)
    {
        now = [] { return Clock::now(); };
    }

    /* Watchdog (E-10): a step that blocks past every bound
       (a join that never returns) would otherwise hang the
       process forever. It runs on real time regardless of the
       injected `now`. */
    std::mutex              wd_mutex;
    std::condition_variable wd_cv;
    bool                    wd_done = false;
    std::string             wd_step;            /* under wd_mutex */
    std::atomic<bool>       wd_fired{ false };
    std::thread             wd_thread;
    if(watchdog_)
    {
        const Clock::time_point deadline =
            Clock::now() + TotalBound() + watchdog_grace_;
        wd_thread = std::thread([&, deadline]
        {
            std::string step;
            {
                std::unique_lock<std::mutex> lock(wd_mutex);
                if(wd_cv.wait_until(lock, deadline,
                                    [&] { return wd_done; }))
                {
                    return;
                }
                step = wd_step;
            }
            wd_fired.store(true);
            watchdog_(step);
        });
    }

    const TimePoint start = now();
    for(const Step& s : steps_)
    {
        StepResult r;
        r.name  = s.name;
        r.bound = s.bound;
        if(watchdog_)
        {
            std::lock_guard<std::mutex> lock(wd_mutex);
            wd_step = s.name;
        }
        const TimePoint t0 = now();
        if(s.fn)
        {
            /* E-10: a throwing step is recorded, never allowed
               to abort the sequence — every later owner still
               has to stop. */
            try
            {
                s.fn();
            }
            catch(const std::exception& e)
            {
                r.failed = true;
                r.error  = e.what();
            }
            catch(...)
            {
                r.failed = true;
                r.error  = "unknown exception";
            }
        }
        r.elapsed = now() - t0;
        r.ran     = true;
        r.within  = r.elapsed <= r.bound;
        if(!r.within)
        {
            result_.all_within_bound = false;
        }
        if(r.failed)
        {
            result_.all_ok = false;
        }
        result_.steps.push_back(std::move(r));
    }
    result_.elapsed = now() - start;

    if(wd_thread.joinable())
    {
        {
            std::lock_guard<std::mutex> lock(wd_mutex);
            wd_done = true;
        }
        wd_cv.notify_all();
        wd_thread.join();
        result_.watchdog_fired = wd_fired.load();
        if(result_.watchdog_fired)
        {
            result_.all_within_bound = false;
        }
    }
    return result_;
}

} /* namespace openrgb2 */
