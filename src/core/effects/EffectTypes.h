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

/*---------------------------------------------------------*\
|||| LayerDrive — a per-layer offset on the time-driven term |
|||| (audit C-2). Every moving primitive advances by          |
|||| t * speed (/ scale), and t is seconds since the worker   |
|||| started, so swapping in a layer with a new speed or      |
|||| wavelength moved its pattern by an amount proportional   |
|||| to uptime. EffectEngine::ReplaceLayers picks an offset   |
|||| per layer so the motion term is continuous at the        |
|||| replacement time. Zero (the default) is the plain        |
|||| absolute-time evaluation, bit-for-bit.                   |
||||                                                         |
||||   cycles — wave/pulse/gradient/spin: phase cycles added  |
||||            to the term before it is wrapped;             |
||||            comet: metres added to the head distance      |
||||   nx..nz — noise: lattice-space drift added to the       |
||||            sample point (noise is not periodic)          |
\*---------------------------------------------------------*/
struct LayerDrive
{
    double cycles = 0.0;
    double nx     = 0.0;
    double ny     = 0.0;
    double nz     = 0.0;
};

/*---------------------------------------------------------*\
|||| Prepared evaluation (audit C-9). The per-LED hot path   |
|||| used to string-compare the primitive name, re-normalise |
|||| the direction, allocate the comet's segment table and   |
|||| recompute the time phase for EVERY emitter.             |
||||                                                         |
||||   PreparedLayer — layer-only invariants, built once per |
||||                   SetLayers/ReplaceLayers              |
||||   LayerFrame    — layer + time terms, built once per    |
||||                   layer per Evaluate                    |
||||                                                         |
|||| EvalPrepared(L, P, F, in) is bit-identical to           |
|||| EvalPrimitive(L, in, drive) for the same t and drive.  |
\*---------------------------------------------------------*/
enum class Primitive : unsigned char
{
    Static, Wave, Pulse, Gradient, Spin, Comet, Noise,
    Ripple, ScreenField, Level,
};

struct PreparedLayer
{
    Primitive          prim     = Primitive::Static;
    Vec3               dir;             /* Normalize(direction): wave, gradient */
    std::vector<float> seg_len;         /* comet: closed-path segment lengths   */
    float              path_len = 0.0f; /* comet: loop length (0 = degenerate)  */
};

struct LayerFrame
{
    float  tphase = 0.0f;               /* wave/pulse/gradient/spin: wrapped    */
                                        /* time phase (drive included)          */
    float  head   = 0.0f;               /* comet: head arc position (m)         */
    double ox = 0.0, oy = 0.0, oz = 0.0;/* noise: direction * t * speed         */
    double nx = 0.0, ny = 0.0, nz = 0.0;/* noise: the layer's drive offset      */
};

PreparedLayer PrepareLayer(const EffectLayer& layer);
LayerFrame    LayerFrameTerms(const EffectLayer& layer,
                              const PreparedLayer& prepared,
                              const LayerDrive& drive, double t);
ColorF        EvalPrepared(const EffectLayer& layer,
                           const PreparedLayer& prepared,
                           const LayerFrame& frame, const EvalInput& in);

/* Evaluate one primitive — returns color + coverage (a). Prepares
   the layer on every call: for one-off samples; the engine uses
   the prepared path above. */
ColorF EvalPrimitive(const EffectLayer& layer, const EvalInput& in);
ColorF EvalPrimitive(const EffectLayer& layer, const EvalInput& in,
                     const LayerDrive& drive);

/* The drive for `to` that continues `from` (running with
   `from_drive`) at time t: both evaluate the same motion term at
   t. Returns `from_drive` unchanged when nothing that feeds the
   term changed, and a zero drive when the primitive differs or
   has no time term (ripple/screenfield/level/static). */
LayerDrive ContinueDrive(const EffectLayer& from,
                         const LayerDrive& from_drive,
                         const EffectLayer& to, double t);

/* Composite src over dst with src.a already including opacity. */
ColorF BlendOver(ColorF dst, ColorF src, BlendMode mode);

/* Deterministic helpers (integer hash — platform-stable). */
unsigned int HashU32(unsigned int x);
float          Noise3(const Vec3& p, unsigned int seed);   /* 0..1 value noise */
Vec3           Normalize(const Vec3& v);
float          Dot(const Vec3& a, const Vec3& b);
float          Length(const Vec3& v);

} /* namespace studio */
