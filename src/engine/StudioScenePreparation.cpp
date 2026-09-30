/*---------------------------------------------------------*\
|| StudioScenePreparation.cpp — see header for the        ||
|| contract. Source-parity anchors:                        ||
||   SceneBridge::rebuildMatrixLayouts (matrix emitters)  ||
||   SceneBridge::rebuildKeyLookup   (key world positions)||
||   SceneBridge::rebuildEffect      (runtime layer copy) ||
||   SceneBridge::tick               (key event positions)||
||                                                           |
||   SPDX-License-Identifier: GPL-2.0-or-later               |
\*---------------------------------------------------------*/

#include "StudioScenePreparation.h"

#include "effects/Presets.h"
#include "inputs/KeyMap.h"
#include "scene/BindingResolver.h"
#include "scene/EmitterLayout.h"
#include "scene/SceneGraph.h"

#include <algorithm>

namespace openrgb2
{

namespace
{

/* Engine snapshot -> the Qt-free resolver's neutral copy.
   start_idx is the zone's offset in the complete-controller
   buffer: the running sum of preceding zone LED counts —
   identical to DeviceMapper's conversion. */
studio::ControllerSnapshot ToControllerSnapshot(const DeviceSnapshot& snap)
{
    studio::ControllerSnapshot cs;
    cs.name        = snap.name;
    cs.vendor      = snap.vendor;
    cs.serial      = snap.key.serial;
    cs.location    = snap.key.location;
    cs.device_type = snap.device_type;

    uint32_t start = 0;
    for(const ZoneSnapshot& z : snap.zones)
    {
        studio::ZoneSnapshot zs;
        zs.name       = z.name;
        zs.leds_count = z.led_count;
        zs.start_idx  = start;
        start        += z.led_count;
        cs.zones.push_back(std::move(zs));
    }
    return cs;
}

/* Bound zone for one binding id — resolved through the same
   identity rules the device mapper uses. Returns the snapshot
   and zone index, or false when the binding is unknown,
   unresolved, ambiguous, or the controller/zone is absent —
   missing hardware is a skip, never an error. */
bool BoundZone(const studio::SceneDocument& doc,
               const std::vector<DeviceSnapshot>& snapshots,
               const std::vector<studio::ControllerSnapshot>& controllers,
               const std::string& binding_id,
               const DeviceSnapshot*& snap_out, int& zone_out,
               uint32_t& zone_start_out)
{
    snap_out  = nullptr;
    zone_out  = -1;
    zone_start_out = 0;
    if(binding_id.empty())
    {
        return false;
    }
    const studio::DeviceBinding* binding = nullptr;
    for(const studio::DeviceBinding& b : doc.bindings)
    {
        if(b.id == binding_id)
        {
            binding = &b;
            break;
        }
    }
    if(binding == nullptr)
    {
        return false;
    }
    const studio::ResolvedBinding r =
        studio::ResolveBinding(*binding, controllers);
    if(r.status != studio::BindingStatus::Resolved ||
       r.controller_index < 0 ||
       (size_t)r.controller_index >= snapshots.size())
    {
        return false;
    }
    const DeviceSnapshot& snap = snapshots[(size_t)r.controller_index];
    /* No zone requested -> zone 0 (single-zone devices),
       matching the mapper/imported adapter. */
    int zi = r.zone_index;
    if(zi < 0 && !snap.zones.empty())
    {
        zi = 0;
    }
    if(zi < 0 || (size_t)zi >= snap.zones.size())
    {
        return false;
    }
    snap_out       = &snap;
    zone_out       = zi;
    zone_start_out =
        controllers[(size_t)r.controller_index].zones[(size_t)zi].start_idx;
    return true;
}

/* The controller-level LED name for a zone-local emitter
   address — ControllerAdapter::LEDName parity. Empty on any
   miss. */
std::string LedName(const DeviceSnapshot& snap, uint32_t zone_start,
                    int address)
{
    if(address < 0)
    {
        return std::string();
    }
    const uint64_t led = (uint64_t)zone_start + (uint32_t)address;
    if(led >= snap.led_names.size())
    {
        return std::string();
    }
    return snap.led_names[(size_t)led];
}

} /* anonymous namespace */

bool PrepareStudioScene(const WorkspaceCandidate& cand,
                        const std::vector<DeviceSnapshot>& snapshots,
                        PreparedStudioScene& out,
                        std::string& error)
{
    out = PreparedStudioScene();
    if(!cand.resolved)
    {
        error = "candidate carries no resolved scene";
        return false;
    }

    std::vector<studio::ControllerSnapshot> controllers;
    controllers.reserve(snapshots.size());
    for(const DeviceSnapshot& snap : snapshots)
    {
        controllers.push_back(ToControllerSnapshot(snap));
    }

    /*---- Matrix expansion ---------------------------------*\
    |  SceneBridge::rebuildMatrixLayouts: every matrix_map   |
    |  device object gets emitters from the bound zone's     |
    |  LIVE matrix map. The copy is lazy — a candidate with  |
    |  no hardware-dependent geometry keeps sharing the      |
    |  authored document pointer.                            |
    \*-------------------------------------------------------*/
    std::shared_ptr<studio::SceneDocument> editable;
    const studio::SceneDocument*           doc = cand.resolved.get();

    for(size_t oi = 0; oi < doc->objects.size(); ++oi)
    {
        const studio::SceneObject& src_obj = doc->objects[oi];
        if(src_obj.layout != "matrix_map" ||
           src_obj.kind != studio::ObjectKind::Device)
        {
            continue;
        }
        const DeviceSnapshot* snap = nullptr;
        int zi = -1;
        uint32_t zone_start = 0;
        if(!BoundZone(*doc, snapshots, controllers, src_obj.binding,
                      snap, zi, zone_start))
        {
            continue;   /* unresolved/missing keyboard: keep
                           authored emitters (usually empty) */
        }
        const ZoneSnapshot& zone = snap->zones[(size_t)zi];
        if(zone.matrix_map.empty() ||
           zone.matrix_rows == 0 || zone.matrix_cols == 0)
        {
            continue;   /* zone reports no matrix */
        }
        /* Out-of-range indices reject: a cell naming a LED
           past the zone's count is malformed hardware data —
           never emit a silently mis-bound emitter. */
        for(uint32_t cell : zone.matrix_map)
        {
            if(cell != 0xFFFFFFFFu && cell >= zone.led_count)
            {
                error = "zone '" + zone.name + "' matrix cell " +
                        std::to_string(cell) +
                        " is out of range (" +
                        std::to_string(zone.led_count) + " LEDs)";
                return false;
            }
        }
        if(!editable)
        {
            editable = std::make_shared<studio::SceneDocument>(*doc);
            doc      = editable.get();
        }
        studio::SceneObject& obj = editable->objects[oi];
        /* 19 mm key pitch, 16 mm key-top height, centered on
           the object origin — the imported adapter's grid. */
        const float pitch = 0.019f;
        const studio::Vec3 origin {
            -((float)zone.matrix_cols - 1) * pitch * 0.5f,
            0.016f,
            ((float)zone.matrix_rows - 1) * pitch * 0.5f };
        obj.emitters = studio::layout::KeyboardMatrix(
            zone.matrix_rows, zone.matrix_cols, zone.matrix_map,
            0xFFFFFFFFu, pitch, pitch, origin, obj.id);
    }

    /*---- Runtime layer stack -----------------------------*\
    |  SceneBridge::rebuildEffect: authored inline stack     |
    |  wins; else the named look resolves through the        |
    |  CANDIDATE's registry snapshot (the versioned set it   |
    |  committed against). Global speed/intensity first,     |
    |  then inputs.decay_pct scales ONLY ripple density —    |
    |  all on this copy, so prepares never compound.         |
    \*-------------------------------------------------------*/
    out.layers = doc->effect.layers;
    if(out.layers.empty() && !doc->effect.preset.empty() &&
       cand.effect_registry.Contains(doc->effect.preset))
    {
        if(!cand.effect_registry.Build(doc->effect.preset,
                                       doc->effect.seed,
                                       out.layers, nullptr))
        {
            error = "effect look '" + doc->effect.preset +
                    "' failed to resolve";
            return false;
        }
    }
    if(!out.layers.empty())
    {
        studio::ApplyGlobalParams(out.layers, doc->effect.speed,
                                  doc->effect.intensity);
        /* Bounded at parse (50..300); clamp defensively so a
           hand-rolled candidate can never scale unboundedly. */
        const int decay_pct =
            std::max(50, std::min(300, cand.document.inputs.decay_pct));
        const float decay_mult = decay_pct / 100.0f;
        for(studio::EffectLayer& L : out.layers)
        {
            if(L.primitive == "ripple")
            {
                L.density *= decay_mult;
            }
        }
    }

    /*---- Key world positions ------------------------------*\
    |  SceneBridge::rebuildKeyLookup: for each bound         |
    |  matrix_map object, emitter local positions go through |
    |  the resolved world matrix — keyboard and PARENT       |
    |  transforms both apply. First emitter wins on a        |
    |  duplicate LED name.                                   |
    \*-------------------------------------------------------*/
    const std::map<std::string, studio::Mat4> world =
        studio::ResolveWorldMatrices(*doc);
    for(const studio::SceneObject& obj : doc->objects)
    {
        if(obj.kind != studio::ObjectKind::Device ||
           obj.layout != "matrix_map" || obj.binding.empty())
        {
            continue;
        }
        const DeviceSnapshot* snap = nullptr;
        int zi = -1;
        uint32_t zone_start = 0;
        if(!BoundZone(*doc, snapshots, controllers, obj.binding,
                      snap, zi, zone_start))
        {
            continue;
        }
        const auto wit = world.find(obj.id);
        if(wit == world.end())
        {
            continue;
        }
        for(const studio::Emitter& e : obj.emitters)
        {
            const int vk =
                studio::VkForKeyName(LedName(*snap, zone_start,
                                             e.address));
            if(vk >= 0)
            {
                /* First emitter wins if two LEDs share a name. */
                out.key_positions.emplace(
                    vk, studio::TransformPoint(wit->second,
                                               e.local_pos));
            }
        }
    }

    if(editable)
    {
        out.doc = std::move(editable);
    }
    else
    {
        out.doc = cand.resolved;
    }
    return true;
}

studio::InputState PositionStudioInput(
    const studio::InputState& in,
    const PreparedStudioScene& scene)
{
    /* Source parity (SceneBridge::tick): only position-less
       key events consult the lookup; everything else passes
       through on the returned copy. */
    studio::InputState out = in;
    for(studio::InputEvent& e : out.events)
    {
        if(e.source == "key" && !e.has_pos)
        {
            const auto it = scene.key_positions.find(e.code);
            if(it != scene.key_positions.end())
            {
                e.pos     = it->second;
                e.has_pos = true;
            }
        }
    }
    return out;
}

} /* namespace openrgb2 */
