/*---------------------------------------------------------*\
||| OnsetDetect.h                                             |
|||                                                           |
|||   Adaptive audio onset detector — Qt-free, testable.    |
|||   Short/long energy EMAs: an onset fires when the fast  |
|||   envelope overshoots the slow one by a sensitivity-    |
|||   scaled factor, with a refractory period so one hit    |
|||   can't machine-gun events. Silence collapses both      |
|||   envelopes, so quiet input decays cleanly.             |
|||                                                           |
|||   This is onset detection, not musical beat tracking.   |
|||                                                           |
|||   SPDX-License-Identifier: GPL-2.0-or-later               |
\*---------------------------------------------------------*/

#pragma once

namespace studio
{

class OnsetDetect
{
public:
    /* Feed one chunk's mean-square energy; dt is the chunk length in
       seconds. Returns onset strength 0..1 (0 = no onset). */
    float Feed(float energy, double dt);

    /* 0..1 UI scale — higher = more sensitive (lower threshold). */
    void  SetSensitivity(float s) { sens = s; }

    /* Smoothed output level 0..1 for the `level` primitive. */
    float Level() const { return level; }

    void  Reset();

private:
    float env_fast = 0.0f;
    float env_slow = 0.0f;
    float level    = 0.0f;
    float sens     = 1.0f;
    float refract  = 0.0f;      /* seconds left before next onset allowed */
};

} /* namespace studio */
