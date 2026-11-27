/*---------------------------------------------------------*\
||| EffectEngine.cpp                                          |
|||                                                           |
|||   SPDX-License-Identifier: GPL-2.0-or-later               |
\*---------------------------------------------------------*/

#include "EffectEngine.h"

#include "../scene/SceneGraph.h"

#include <algorithm>

namespace studio
{

void EffectEngine::SetLayers(const std::vector<EffectLayer>& new_layers)
{
    layers = new_layers;
    drives.assign(layers.size(), LayerDrive{});
}

void EffectEngine::ReplaceLayers(const std::vector<EffectLayer>& new_layers,
                                 double t)
{
    std::vector<LayerDrive> next(new_layers.size());
    const size_t n = std::min(layers.size(), new_layers.size());
    for(size_t i = 0; i < n; i++)
    {
        next[i] = ContinueDrive(layers[i], drives[i], new_layers[i], t);
    }
    layers = new_layers;
    drives = std::move(next);
}

void EffectEngine::Evaluate(const SceneDocument& doc, double t,
                            FrameColors& frame, const InputState* input) const
{
    if(layers.empty())
    {
        frame.clear();
        return;
    }
    /* One transform graph for renderer and effects: world-space
       primitives sample each emitter at its resolved position —
       parent groups included — so a moved group moves its light. */
    Evaluate(doc, ResolveWorldMatrices(doc), t, frame, input);
}

void EffectEngine::Evaluate(const SceneDocument& doc,
                            const std::map<std::string, Mat4>& world,
                            double t, FrameColors& frame,
                            const InputState* input) const
{
    frame.clear();
    if(layers.empty())
    {
        return;
    }

    for(const SceneObject& obj : doc.objects)
    {
        /* Owners only — Linked copies share this object's group state
           and must never receive an independent evaluation (plan:
           mirrored outputs show identical colors). */
        if(obj.kind != ObjectKind::Device || obj.emitters.empty())
        {
            continue;
        }

        std::vector<SceneColor> colors(obj.emitters.size());
        bool touched = false;

        /* Per-object lookups hoisted out of the emitter loop —
           the same resolution EmitterColor(doc, obj.id, i) does
           (owner -> per-emitter override -> object color -> 0),
           with the owner scan and map finds done once. */
        const Mat4&        model    = world.at(obj.id);
        const SceneObject* owner    = OutputOwner(doc, obj.id);
        const std::string& owner_id = owner ? owner->id : obj.id;
        const auto         ov       = doc.emitter_colors.find(owner_id);
        const bool         has_ov   = ov != doc.emitter_colors.end();
        const auto         base     = doc.object_colors.find(owner_id);
        const SceneColor   base_c   =
            (base != doc.object_colors.end()) ? base->second : 0;

        for(size_t i = 0; i < obj.emitters.size(); i++)
        {
            const Emitter& e = obj.emitters[i];

            EvalInput in;
            in.local   = e.local_pos;
            in.world   = TransformPoint(model, e.local_pos);
            in.object  = &obj;
            in.emitter = &e;
            in.t       = t;
            in.input   = input;

            SceneColor painted = base_c;
            if(has_ov)
            {
                const auto it = ov->second.find((int)i);
                if(it != ov->second.end())
                {
                    painted = it->second;
                }
            }
            ColorF acc = ToColorF(painted);
            for(size_t k = 0; k < layers.size(); k++)
            {
                const EffectLayer& layer = layers[k];
                if(!layer.enabled || !LayerMatches(layer, obj, e))
                {
                    continue;
                }
                touched  = true;
                /* r.a already carries layer.opacity — every
                   primitive applies it (BlendOver's contract). */
                const ColorF r = EvalPrimitive(layer, in, drives[k]);
                acc = BlendOver(acc, r, layer.blend);
            }
            colors[i] = ToSceneColor(acc);
        }

        if(touched)
        {
            frame[obj.id] = std::move(colors);
        }
    }
}

} /* namespace studio */
