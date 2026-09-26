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

namespace studio
{

class EffectEngine
{
public:
    void SetLayers(const std::vector<EffectLayer>& new_layers) { layers = new_layers; }
    const std::vector<EffectLayer>& Layers() const             { return layers; }
    bool Empty() const                                         { return layers.empty(); }

    /* For every Device object: composite all matching layers (in
       order) over each emitter's painted color and store the result
       in frame[obj.id][index]. Objects no layer touches get no
       frame entry — downstream falls back to painted colors.
       Linked copies are skipped: they display their owner's frame.
       `input` carries reactive signals (audio onsets, key events,
       screen grid); nullptr disables reactive primitives. */
    void Evaluate(const SceneDocument& doc, double t, FrameColors& frame,
                  const InputState* input = nullptr) const;

private:
    std::vector<EffectLayer> layers;
};

} /* namespace studio */
