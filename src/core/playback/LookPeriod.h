/*---------------------------------------------------------*\
||| LookPeriod.h                                              |
|||                                                           |
|||   R-11 step 1: does a look repeat, and after how long?   |
|||   Pure analysis of an effect layer stack over a scene    |
|||   document. A look that loops with a cycle T can be      |
|||   rendered once into T*60 frames and uploaded to the     |
|||   Lian Li wireless fans, which then play it on-device.   |
|||                                                           |
|||   Per-layer period (field conventions: EffectTypes.cpp): |
|||     wave, pulse      scale / |speed|                     |
|||     gradient, spin   1 / |speed|                         |
|||     comet            path length / |speed|               |
|||     static, speed 0, invisible layer   no motion         |
|||     noise (speed != 0)   not compilable (never repeats)  |
|||     ripple, screenfield, level   not compilable (live    |
|||                                  input)                  |
|||   Only layers that are enabled and reach at least one    |
|||   Device emitter of the document count.                  |
|||                                                           |
|||   The look's cycle is the shortest T <= max_cycle_s with |
|||   every period dividing it within `tolerance` (0.5%).    |
|||   With max_change set above the tolerance (SnapOptions), |
|||   a look with no such T may still be SNAPPED: each       |
|||   layer's period becomes exactly T/k for the T that      |
|||   needs the smallest relative speed change (<= 5%). The  |
|||   change is reported per layer; nothing applies it       |
|||   unless the caller asks (no UX is wired to it).         |
|||                                                           |
|||   SPDX-License-Identifier: GPL-2.0-or-later               |
\*---------------------------------------------------------*/

#pragma once

#include "../effects/EffectTypes.h"

#include <optional>
#include <string>
#include <vector>

namespace studio
{

constexpr double kLookMaxCycleSeconds = 20.0;   /* plan: 20 s                */
constexpr double kLookPeriodTolerance = 0.005;  /* "divides within 0.5%"     */
constexpr double kLookSnapMaxChange   = 0.05;   /* plan: snap only <= 5%     */
constexpr double kLookNominalFps      = 60.0;   /* BuildPlaybackLoop's rule  */

struct LookPeriodOptions
{
    double max_cycle_s = kLookMaxCycleSeconds;
    double tolerance   = kLookPeriodTolerance;
    /* Largest relative speed change allowed per layer. Equal to
       `tolerance` = no snapping (the default). */
    double max_change  = kLookPeriodTolerance;
};

/* The plan's recommended snapping: 5% within 20 s. */
inline LookPeriodOptions SnapOptions()
{
    LookPeriodOptions o;
    o.max_change = kLookSnapMaxChange;
    return o;
}

enum class LayerMotion
{
    Unused,          /* disabled, or reaches no Device emitter        */
    Static,          /* reaches the scene but does not change in time */
    Periodic,
    NotCompilable,
};

struct LayerPeriod
{
    std::string primitive;
    LayerMotion motion   = LayerMotion::Unused;
    double      period_s = 0.0;     /* Periodic: as authored (global speed folded in) */
    int         repeats  = 0;       /* Periodic + compilable: cycles per loop (k)     */
    double      speed_change = 0.0; /* Periodic + compilable: relative, signed        */
    std::string reason;             /* NotCompilable                                  */
};

struct LookPeriod
{
    bool   compilable = false;
    bool   is_static  = false;      /* nothing moves: a 1-frame loop                  */
    bool   snapped    = false;      /* a speed changed by more than the tolerance     */
    double cycle_s    = 0.0;        /* T; 0 when static or refused                    */
    double max_change = 0.0;        /* largest |speed_change| over the layers         */
    std::string reason;             /* why not compilable                             */

    /* Diagnostics for a periodic look, compilable or not: the
       smallest change any T <= max_cycle_s needs, and that T. */
    double best_change  = 0.0;
    double best_cycle_s = 0.0;

    /* Parallel to the input stack. */
    std::vector<LayerPeriod> layers;

    /* The stack to evaluate over [0, T): the input with global
       speed/intensity folded in (ApplyGlobalParams) and each
       periodic layer's speed scaled so its period is exactly
       T/repeats. Empty when not compilable. */
    std::vector<EffectLayer> runtime_layers;
};

/* `layers` is the authored/resolved stack BEFORE the global
   controls (EffectState.speed and .intensity of `doc` are folded
   in here, the same way StudioScenePreparation does). */
LookPeriod AnalyzeLookPeriod(const std::vector<EffectLayer>& layers,
                             const SceneDocument& doc,
                             const LookPeriodOptions& opts = {});

/* The plan's entry point: the cycle in seconds (0 = static),
   nullopt when the look cannot loop without snapping. */
std::optional<double> LookCycleSeconds(const std::vector<EffectLayer>& layers,
                                       const SceneDocument& doc);

/* Frames per cycle at 60 fps before any size reduction (the
   BuildPlaybackLoop rule: lround(T * 60), 2..4096); 1 for a static
   look, 0 when not compilable. */
size_t NominalFrameCount(const LookPeriod& period);

/*---------------------------------------------------------*\
||| The common-cycle search on bare periods (all > 0).      |
||| Exposed for the measurement tool and tests.             |
\*---------------------------------------------------------*/
struct CycleFit
{
    bool                found = false;
    double              cycle_s = 0.0;
    double              max_change = 0.0;
    std::vector<int>    repeats;        /* parallel to the periods */
    std::vector<double> speed_change;   /* k*p/T - 1, signed       */
    double              best_change  = 0.0;  /* min over T <= max  */
    double              best_cycle_s = 0.0;
};

CycleFit FitCommonCycle(const std::vector<double>& periods_s,
                        const LookPeriodOptions& opts);

} /* namespace studio */
