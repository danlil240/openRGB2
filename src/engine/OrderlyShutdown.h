/*---------------------------------------------------------*\
||| OrderlyShutdown.h                                         ||
|||                                                           ||
|||   Ordered, bounded shutdown sequencing for the engine  ||
|||   (plan section 4 "Process ownership" — "Quit All     ||
|||   shuts down owned processes in order" — and section 6  ||
|||   "bound shutdown time").                              ||
|||                                                           ||
|||   Owners register their teardown steps in the order     ||
|||   they must run (stop event/command intake first, then  ||
|||   drain in-flight work, then join workers, then close   ||
|||   the transport). Each step carries the bound its       ||
|||   implementation is designed to meet — the bounds are   ||
|||   construction-level: a lane thread join is bounded by  ||
|||   the transport's send timeout, a pipe listener by its  ||
|||   self-connect release, a worker by its tick. Run()     ||
|||   measures every step against its bound and reports     ||
|||   honestly — an overrun is flagged, never silently      ||
|||   dropped, and later steps still run (skipping teardown ||
|||   would strand later owners).                           ||
|||                                                           ||
|||   SPDX-License-Identifier: GPL-2.0-or-later               ||
|\*---------------------------------------------------------*/

#pragma once

#include "EngineTypes.h"   /* Clock, TimePoint */

#include <chrono>
#include <functional>
#include <string>
#include <vector>

namespace openrgb2
{

class OrderlyShutdown
{
public:
    struct StepResult
    {
        std::string     name;
        Clock::duration bound{};
        Clock::duration elapsed{};
        bool            ran    = false;
        bool            within = true;   /* elapsed <= bound */
    };

    struct Result
    {
        std::vector<StepResult> steps;
        Clock::duration         elapsed{};
        bool                    all_within_bound = true;
    };

    /* Register a teardown step (registration order is execution
       order). `bound` documents the time the step is designed to
       take — see the class comment. */
    void   Add(std::string name, Clock::duration bound,
               std::function<void()> fn);

    /* The designed total bound — the sum of the registered step
       bounds. */
    Clock::duration TotalBound() const;
    size_t          StepCount() const;
    bool            Ran() const;

    /* Run every step in registration order, timing each against
       its bound. Single-shot: a second Run() returns the same
       recorded result (shutdown is not restartable). `now` is
       injectable for deterministic tests. */
    Result Run(std::function<TimePoint()> now = Clock::now);

private:
    struct Step
    {
        std::string             name;
        Clock::duration         bound;
        std::function<void()>   fn;
    };

    std::vector<Step> steps_;
    bool              ran_ = false;
    Result            result_;
};

} /* namespace openrgb2 */
