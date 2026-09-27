/*---------------------------------------------------------*\
||| OrderlyShutdown.cpp                                       ||
|||                                                           ||
|||   SPDX-License-Identifier: GPL-2.0-or-later               ||
|\*---------------------------------------------------------*/

#include "OrderlyShutdown.h"

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

    const TimePoint start = now();
    for(const Step& s : steps_)
    {
        StepResult r;
        r.name  = s.name;
        r.bound = s.bound;
        const TimePoint t0 = now();
        if(s.fn)
        {
            s.fn();
        }
        r.elapsed = now() - t0;
        r.ran     = true;
        r.within  = r.elapsed <= r.bound;
        if(!r.within)
        {
            result_.all_within_bound = false;
        }
        result_.steps.push_back(std::move(r));
    }
    result_.elapsed = now() - start;
    return result_;
}

} /* namespace openrgb2 */
