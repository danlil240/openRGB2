/*---------------------------------------------------------*\
||| ReconnectBackoff.h                                        |
|||                                                           |
|||   Bounded exponential backoff for SDK reconnects (plan   ||
|||   section 6: "bound retry/backoff"). DelayFor(attempt)   ||
|||   grows geometrically from initial_ms, clamped at        ||
|||   max_ms; a reconnect schedule gives up after            ||
|||   max_attempts (the client then reports Disconnected     ||
|||   until explicitly asked again).                          ||
|||                                                           |
|||   SPDX-License-Identifier: GPL-2.0-or-later               ||
\*---------------------------------------------------------*/

#pragma once

#include <chrono>
#include <cstdint>

namespace openrgb2
{
namespace sdk
{

struct ReconnectBackoff
{
    uint32_t initial_ms   = 250;
    double   multiplier   = 2.0;
    uint32_t max_ms       = 30000;
    uint32_t max_attempts = 12;

    /* Delay before reconnect attempt `attempt` (0-based). */
    std::chrono::milliseconds DelayFor(uint32_t attempt) const
    {
        double ms = double(initial_ms);
        for(uint32_t i = 0; i < attempt; ++i)
        {
            ms *= multiplier;
            if(ms >= double(max_ms))
            {
                break;
            }
        }
        if(ms > double(max_ms))
        {
            ms = double(max_ms);
        }
        return std::chrono::milliseconds((long long)ms);
    }
};

} /* namespace sdk */
} /* namespace openrgb2 */
