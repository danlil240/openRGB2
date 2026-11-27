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
    Prepare();
}

/* Audit C-9: layer-only invariants once per stack, not per LED. */
void EffectEngine::Prepare()
{
    prepared.clear();
    prepared.reserve(layers.size());
    for(const EffectLayer& L : layers)
    {
        prepared.push_back(PrepareLayer(L));
    }
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
    Prepare();
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
    if(layers.empty())
    {
        frame.clear();
        return;
    }

    /* Audit C-9: the layer's time terms (wrapped phase, comet head,
       noise drift) depend only on the layer and t — once per layer
       per frame, not once per emitter. Audit C-10: the scratch is
       per thread (Evaluate stays const and reentrant) and keeps its
       capacity, so a steady-state tick does not allocate it. */
    thread_local std::vector<LayerFrame> terms;
    terms.resize(layers.size());
    for(size_t k = 0; k < layers.size(); k++)
    {
        terms[k] = LayerFrameTerms(layers[k], prepared[k], drives[k], t);
    }

    /* Audit C-10: `frame` is REUSED, not cleared. A caller that
       hands back the previous tick's map keeps its nodes and color
       vectors (same object ids, same emitter counts in steady
       state), so a tick allocates nothing. Entries of objects that
       are no longer evaluated-and-touched are removed: in the loop
       for evaluated objects, by the sweep below for the rest. */
    size_t live = 0;
    std::vector<SceneColor> fresh;

    for(const SceneObject& obj : doc.objects)
    {
        /* Owners only — Linked copies share this object's group state
           and must never receive an independent evaluation (plan:
           mirrored outputs show identical colors). */
        if(obj.kind != ObjectKind::Device || obj.emitters.empty())
        {
            continue;
        }

        const auto entry  = frame.find(obj.id);
        const bool reuse  = entry != frame.end();
        std::vector<SceneColor>& colors = reuse ? entry->second : fresh;
        colors.resize(obj.emitters.size());
        bool touched = false;

        /* Per-object lookups hoisted out of the emitter loop —
           the same resolution EmitterColor(doc, obj.id, i) does
           (per-emitter override -> object color -> 0). Only
           Device objects get here, and a Device is its own output
           owner (OutputOwner only redirects Linked copies), so
           the owner is obj.id — no linear OutputOwner scan of the
           document per object per tick (audit C-10). */
        const Mat4&        model    = world.at(obj.id);
        const std::string& owner_id = obj.id;
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
                const ColorF r = EvalPrepared(layer, prepared[k],
                                              terms[k], in);
                acc = BlendOver(acc, r, layer.blend);
            }
            colors[i] = ToSceneColor(acc);
        }

        if(touched)
        {
            ++live;
            if(!reuse)
            {
                frame.emplace(obj.id, std::move(fresh));
                fresh = std::vector<SceneColor>();
            }
        }
        else if(reuse)
        {
            frame.erase(entry);
        }
    }

    /* Leftover entries belong to objects that left the document or
       are no longer evaluated Devices (a Linked copy now, or no
       emitters). Only a document change gets here. */
    if(frame.size() != live)
    {
        for(auto it = frame.begin(); it != frame.end();)
        {
            const SceneObject* o = FindObject(doc, it->first);
            if(o == nullptr || o->kind != ObjectKind::Device
               || o->emitters.empty())
            {
                it = frame.erase(it);
            }
            else
            {
                ++it;
            }
        }
    }
}

} /* namespace studio */
