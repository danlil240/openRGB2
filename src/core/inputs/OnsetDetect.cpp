/*---------------------------------------------------------*\
||| OnsetDetect.cpp                                           |
|||                                                           |
|||   SPDX-License-Identifier: GPL-2.0-or-later               |
\*---------------------------------------------------------*/

#include "OnsetDetect.h"

#include <cmath>

namespace studio
{

/* Envelope time constants (s) and refractory (s). */
static constexpr float FAST_TC = 0.020f;
static constexpr float SLOW_TC = 0.500f;
static constexpr float REFRACT = 0.120f;
/* Minimum energy before onsets can fire — keeps noise-floor
   hiss from ringing the desk. */
static constexpr float ENERGY_FLOOR = 1e-5f;

static float StepEma(float prev, float x, float tc, double dt)
{
    const float a = (tc > 0.0f) ? 1.0f - std::exp(-(float)dt / tc) : 1.0f;
    return prev + (x - prev) * a;
}

float OnsetDetect::Feed(float energy, double dt)
{
    if(dt <= 0.0)
    {
        return 0.0f;
    }
    env_fast = StepEma(env_fast, energy, FAST_TC, dt);
    env_slow = StepEma(env_slow, energy, SLOW_TC, dt);
    refract  = (refract > 0.0f) ? refract - (float)dt : 0.0f;

    /* Output level: fast envelope normalized by a gentle compressor
       so quiet and loud sources both reach mid-scale. */
    const float shaped = env_fast / (env_fast + 0.05f);
    level = StepEma(level, shaped, 0.080f, dt);

    /* Sensitivity scales the over-threshold factor: sens 1.0 -> 1.35x,
       sens 0 -> 3.0x the slow envelope. */
    const float factor = 3.0f - 1.65f * (sens < 0.0f ? 0.0f
                                      : sens > 1.0f ? 1.0f : sens);
    if(env_fast > ENERGY_FLOOR && env_fast > env_slow * factor
       && refract <= 0.0f)
    {
        refract = REFRACT;
        const float over = env_fast / (env_slow + 1e-6f);
        return over > 3.0f ? 1.0f : (over - factor) / (3.0f - factor);
    }
    return 0.0f;
}

void OnsetDetect::Reset()
{
    env_fast = env_slow = level = refract = 0.0f;
}

} /* namespace studio */
