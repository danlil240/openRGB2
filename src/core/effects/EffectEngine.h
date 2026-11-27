/*---------------------------------------------------------*\
||| EffectEngine.h                                            |
|||                                                           |
|||   Evaluates an ordered list of EffectLayers over the     |
|||   scene's device emitters into a FrameColors buffer.     |
|||   Pure: no Qt, no clock, no hardware — the caller        |
|||   supplies t (seconds).                                  |
|||                                                           |
|||   SPDX-License-Identifier: GPL-2.0-or-later               |
\*---------------------------------------------------------*/

#pragma once

#include "EffectTypes.h"
#include "../scene/SceneGraph.h"

#include <map>
#include <string>

namespace studio
{

class EffectEngine
{
public:
    /* Fresh start: every layer runs on absolute time (t * speed
       from t = 0). Deterministic per (layers, doc, t) — the
       golden fixtures and one-shot evaluators use this. */
    void SetLayers(const std::vector<EffectLayer>& new_layers);

    /* Live replacement at scene time t (audit C-2): layer i of
       the new stack continues layer i of the old one when the
       primitive matches — its time-driven motion term (wave and
       pulse phase, gradient/spin rotation, comet head, noise
       drift) evaluates the same at t as before the swap, so a
       speed, wavelength or direction edit changes the RATE from
       here on instead of jumping the pattern by an amount that
       grows with uptime. An unchanged layer is bit-exact. Other
       layers (new index, different primitive) start fresh. */
    void ReplaceLayers(const std::vector<EffectLayer>& new_layers,
                       double t);

    const std::vector<EffectLayer>& Layers() const             { return layers; }
    bool Empty() const                                         { return layers.empty(); }

    /* For every Device object: composite all matching layers (in
       order) over each emitter's painted color and store the result
       in frame[obj.id][index]. Objects no layer touches get no
       frame entry — downstream falls back to painted colors.
       Linked copies are skipped: they display their owner's frame.
       `input` carries reactive signals (audio onsets, key events,
       screen grid); nullptr disables reactive primitives.
       `frame` is overwritten, but its existing entries are reused
       (no reallocation): pass the previous tick's map back to make
       a steady-state tick allocation-free (audit C-10). */
    void Evaluate(const SceneDocument& doc, double t, FrameColors& frame,
                  const InputState* input = nullptr) const;

    /* Same, with the document's world matrices precomputed
       (ResolveWorldMatrices(doc)) — the 60 Hz caller resolves
       them once per applied document instead of every tick. */
    void Evaluate(const SceneDocument& doc,
                  const std::map<std::string, Mat4>& world, double t,
                  FrameColors& frame,
                  const InputState* input = nullptr) const;

private:
    void Prepare();

    std::vector<EffectLayer>   layers;
    std::vector<LayerDrive>    drives;    /* parallel to layers */
    std::vector<PreparedLayer> prepared;  /* parallel to layers */
};

} /* namespace studio */
