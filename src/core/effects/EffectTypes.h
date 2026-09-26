/*---------------------------------------------------------*\
|||| EffectTypes.h                                             |
||||                                                           |
||||   Desktop Lighting Studio effect model — Qt-free core.   |
||||   An EffectLayer is a parametric color field evaluated   |
||||   at emitter positions; layers composite in order onto   |
||||   the emitter's painted base color. Deterministic: same  |
||||   (doc, t, params) always gives the same frame.          |
||||                                                           |
||||   The layer model types (ColorF, BlendMode, CoordSpace, |
||||   PaletteStop, Palette, MakePalette, EffectLayer) are   |
||||   declared in scene/SceneTypes.h — the persisted inline |
||||   layer stack lives on EffectState, and this header     |
||||   already depends on that one.                          |
||||                                                           |
||||   SPDX-License-Identifier: GPL-2.0-or-later               |
\*---------------------------------------------------------*/

#pragma once

#include "../scene/SceneTypes.h"

#include <string>
#include <vector>

namespace studio
{

/*---------------------------------------------------------*\
|||| InputEvent / InputState — reactive signals fed to the   |
|||| engine by the caller (bridge). The engine never reads   |
|||| hardware or clocks itself: an empty/null state simply   |
|||| gives every reactive primitive zero coverage.           |
||||                                                         |
||||   source  — "audio" | "key" | provider tag; a layer's   |
||||             `source` filters which events a ripple      |
||||             consumes ("" = all events).                 |
||||   has_pos — false -> spawn at the layer's origin        |
||||   code    — raw source code (e.g. VK) until resolved    |
\*---------------------------------------------------------*/
struct InputEvent
{
    double      t        = 0.0;     /* event time on the caller's clock */
    Vec3        pos;                /* world position (m)               */
    bool        has_pos  = false;
    float       strength = 1.0f;
    std::string source;
    int         code     = 0;
};

struct InputState
{
    std::vector<InputEvent> events;             /* recent, bounded      */
    std::vector<ColorF>     screen_cells;       /* row-major grid       */
    int                     screen_cols = 0;
    int                     screen_rows = 0;
    float                   audio_level = 0.0f; /* smoothed 0..1        */
};

/*---------------------------------------------------------*\
|||| EvalInput — everything a primitive sees for one        |
|||| emitter sample.                                        |
\*---------------------------------------------------------*/
struct EvalInput
{
    Vec3               world;       /* emitter world position (m)      */
    Vec3               local;       /* emitter object-local position   */
    const SceneObject* object;
    const Emitter*     emitter;
    double             t;           /* seconds, bridge-owned clock     */
    const InputState*  input = nullptr;   /* reactive signals, may be null */
};

/* Does this layer touch this emitter? Empty targets = all.
   Matches object id, object geometry tag, or emitter group. */
bool   LayerMatches(const EffectLayer& layer, const SceneObject& obj,
                    const Emitter& emitter);

/* Evaluate one primitive — returns color + coverage (a). */
ColorF EvalPrimitive(const EffectLayer& layer, const EvalInput& in);

/* Composite src over dst with src.a already including opacity. */
ColorF BlendOver(ColorF dst, ColorF src, BlendMode mode);

/* Deterministic helpers (integer hash — platform-stable). */
unsigned int HashU32(unsigned int x);
float          Noise3(const Vec3& p, unsigned int seed);   /* 0..1 value noise */
Vec3           Normalize(const Vec3& v);
float          Dot(const Vec3& a, const Vec3& b);
float          Length(const Vec3& v);

} /* namespace studio */
