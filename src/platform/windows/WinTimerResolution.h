/*---------------------------------------------------------*\
||| WinTimerResolution.h                                      ||
|||                                                           ||
|||   Process timer-resolution request for the engine       ||
|||   (audit E-2 / P-8). Windows wakes Sleep, sleep_for /   ||
|||   sleep_until and condition-variable timeouts on the    ||
|||   system timer tick, 15.6 ms by default: the 60 Hz      ||
|||   scene clock ran in 15.6/31 ms steps and the owner     ||
|||   loop's 20 ms sleep took ~31 ms. timeBeginPeriod(1)    ||
|||   for the engine's lifetime brings those waits to ~1 ms.||
|||                                                           ||
|||   Since Windows 10 2004 the request is per process; on  ||
|||   Windows 11 a process with no visible window may have  ||
|||   it ignored unless it opts out of timer-resolution     ||
|||   power throttling, so Acquire() also does that (the    ||
|||   engine runs hidden).                                  ||
|||                                                           ||
|||   RAII: Release() (or the destructor) undoes exactly    ||
|||   the request that succeeded. The API is injectable so  ||
|||   tests check request/release without changing the      ||
|||   real timer.                                           ||
|||                                                           ||
|||   SPDX-License-Identifier: GPL-2.0-or-later               ||
|\*---------------------------------------------------------*/

#pragma once

#include <functional>

namespace openrgb2
{
namespace platform
{

class ScopedTimerResolution
{
public:
    struct Api
    {
        /* timeBeginPeriod / timeEndPeriod shape: 0 is
           TIMERR_NOERROR. Empty = the system calls. */
        std::function<unsigned(unsigned)> begin_period;
        std::function<unsigned(unsigned)> end_period;
    };

    ScopedTimerResolution() = default;
    ~ScopedTimerResolution();                    /* Release() */

    ScopedTimerResolution(const ScopedTimerResolution&)            = delete;
    ScopedTimerResolution& operator=(const ScopedTimerResolution&) = delete;

    /* Requests `period_ms`. True when the request is held (a
       second call while held is a no-op). False when the system
       refused it — nothing to release then. */
    bool Acquire(unsigned period_ms = 1, Api api = Api{});
    void Release();
    bool Active() const { return active_; }

private:
    Api      api_;
    unsigned period_ms_ = 0;
    bool     active_    = false;
};

} /* namespace platform */
} /* namespace openrgb2 */
