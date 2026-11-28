/*---------------------------------------------------------*\
||| WinTimerResolution.cpp                                    ||
|||                                                           ||
|||   SPDX-License-Identifier: GPL-2.0-or-later               ||
|\*---------------------------------------------------------*/

#include "WinTimerResolution.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#include <windows.h>
#include <timeapi.h>

/* Older SDKs lack the Windows 11 flag; the value is fixed by
   the OS ABI (processthreadsapi.h). */
#ifndef PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION
#define PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION 0x4
#endif

namespace openrgb2
{
namespace platform
{

namespace
{

/* Windows 11: keep honouring this process's resolution request
   while it has no visible window (the engine runs hidden).
   ControlMask names the policy, StateMask 0 turns the
   throttling off. Best effort — older systems reject the flag
   and simply keep the pre-Windows-11 behaviour. */
void KeepResolutionWhenHidden()
{
    PROCESS_POWER_THROTTLING_STATE state = {};
    state.Version     = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
    state.ControlMask = PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION;
    state.StateMask   = 0;
    SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling,
                          &state, sizeof(state));
}

} /* anonymous namespace */

bool ScopedTimerResolution::Acquire(unsigned period_ms, Api api)
{
    if(active_)
    {
        return true;
    }
    if(!api.begin_period || !api.end_period)
    {
        api.begin_period = [](unsigned p)
        {
            KeepResolutionWhenHidden();
            return (unsigned)timeBeginPeriod(p);
        };
        api.end_period = [](unsigned p)
        { return (unsigned)timeEndPeriod(p); };
    }
    if(api.begin_period(period_ms) != 0)
    {
        return false;               /* refused: nothing to undo */
    }
    api_       = std::move(api);
    period_ms_ = period_ms;
    active_    = true;
    return true;
}

void ScopedTimerResolution::Release()
{
    if(!active_)
    {
        return;
    }
    active_ = false;
    api_.end_period(period_ms_);
}

ScopedTimerResolution::~ScopedTimerResolution()
{
    Release();
}

} /* namespace platform */
} /* namespace openrgb2 */
