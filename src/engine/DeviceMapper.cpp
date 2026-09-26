/*---------------------------------------------------------*\
|| DeviceMapper.cpp                                          |
||                                                           |
||   SPDX-License-Identifier: GPL-2.0-or-later               |
\*---------------------------------------------------------*/

#include "DeviceMapper.h"

#include <algorithm>
#include <utility>

namespace openrgb2
{
namespace
{

/* Engine snapshot -> the Qt-free resolver's neutral copy.
   start_idx is the zone's offset in the complete-controller
   buffer: the running sum of preceding zone LED counts. */
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

RgbColor ScaleColor(RgbColor c, float brightness)
{
    RgbColor out;
    out.r = uint8_t((unsigned)(c.r * brightness) & 0xFFu);
    out.g = uint8_t((unsigned)(c.g * brightness) & 0xFFu);
    out.b = uint8_t((unsigned)(c.b * brightness) & 0xFFu);
    return out;
}

} /* anonymous namespace */

void DeviceMapper::SetScene(
    std::shared_ptr<const studio::SceneDocument> doc)
{
    std::lock_guard<std::mutex> lock(mutex_);
    doc_ = std::move(doc);
    /* The new scene may carry different bindings — resolution
       re-runs on the next SyncDevices. */
    resolved_.clear();
}

std::shared_ptr<const studio::SceneDocument> DeviceMapper::Scene() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return doc_;
}

void DeviceMapper::SyncDevices(
    const std::vector<DeviceSnapshot>& snapshots)
{
    std::lock_guard<std::mutex> lock(mutex_);

    std::vector<studio::ControllerSnapshot> controllers;
    controllers.reserve(snapshots.size());
    snapshots_.clear();
    for(const DeviceSnapshot& snap : snapshots)
    {
        controllers.push_back(ToControllerSnapshot(snap));
        snapshots_.emplace(snap.key, snap);
    }

    resolved_.clear();
    if(doc_)
    {
        for(const studio::DeviceBinding& binding : doc_->bindings)
        {
            Resolution res;
            const studio::ResolvedBinding r =
                studio::ResolveBinding(binding, controllers);
            res.status = r.status;
            res.reason = r.reason;

            if(r.status == studio::BindingStatus::Resolved)
            {
                const DeviceSnapshot& snap =
                    snapshots[(size_t)r.controller_index];
                res.zone.controller = snap.key;

                /* No zone requested -> zone 0 (single-zone
                   devices), matching the imported adapter. */
                int zi = r.zone_index;
                if(zi < 0 && !snap.zones.empty())
                {
                    zi = 0;
                }
                if(zi >= 0)
                {
                    const studio::ControllerSnapshot& cs =
                        controllers[(size_t)r.controller_index];
                    res.zone.zone_index = (uint32_t)zi;
                    res.zone.start_idx  = cs.zones[(size_t)zi].start_idx;
                    res.zone.led_count  = cs.zones[(size_t)zi].leds_count;
                }
                else
                {
                    /* Zoneless controller — treat as
                       unresolved for writes. */
                    res.status = studio::BindingStatus::Unresolved;
                    res.reason = "controller has no zones";
                }
            }
            /* else Unresolved/Ambiguous: the binding keeps its
               persisted identity — never reassigned to a
               same-named device (plan section 6). */
            resolved_[binding.id] = std::move(res);
        }
    }

    /* Drop emitted state for controllers absent from this
       enumeration — it can never seed a frame again. */
    for(auto it = emitted_.begin(); it != emitted_.end();)
    {
        if(snapshots_.find(it->first) == snapshots_.end())
        {
            it = emitted_.erase(it);
        }
        else
        {
            ++it;
        }
    }
}

std::vector<RgbFrame> DeviceMapper::MapFrame(const EvaluatedFrame& eval)
{
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<RgbFrame> out;
    if(!doc_)
    {
        return out;
    }

    /* Collect writes per controller BEFORE assembling frames —
       document order is preserved per zone so overlapping
       mapped addresses resolve by deterministic document
       precedence (last write wins). */
    std::map<DeviceKey, std::vector<std::pair<uint32_t, RgbColor>>>
        writes;
    for(const studio::SceneObject& obj : doc_->objects)
    {
        if(obj.kind != studio::ObjectKind::Device || !obj.verified ||
           obj.binding.empty())
        {
            continue;      /* Linked copies share their source
                              object's group colors — they are
                              render-only (address == -1) and
                              never multiply LED counts. */
        }
        const auto rit = resolved_.find(obj.binding);
        if(rit == resolved_.end() ||
           rit->second.status != studio::BindingStatus::Resolved)
        {
            continue;
        }
        const ZonePlacement& zone = rit->second.zone;
        auto&            queue = writes[zone.controller];
        for(size_t e = 0; e < obj.emitters.size(); ++e)
        {
            const int addr = obj.emitters[e].address;
            if(addr < 0 || (uint32_t)addr >= zone.led_count)
            {
                continue;
            }
            studio::SceneColor c =
                studio::EmitterColor(*doc_, obj.id, (int)e);
            const auto fit = eval.colors.find(obj.id);
            if(fit != eval.colors.end() && e < fit->second.size())
            {
                c = fit->second[e];
            }
            queue.emplace_back(zone.start_idx + (uint32_t)addr,
                               ScaleColor(FromSceneColor(c),
                                          doc_->brightness));
        }
    }

    for(const auto& kv : writes)
    {
        const DeviceKey&      key  = kv.first;
        const auto            sit  = snapshots_.find(key);
        if(sit == snapshots_.end())
        {
            continue;
        }
        const DeviceSnapshot& snap = sit->second;
        if(snap.connection != ConnectionState::Connected)
        {
            /* Stop obsolete frames on disconnect (plan
               section 6) — the binding stays resolved for
               the device's return. */
            continue;
        }

        /* Baseline: the last frame emitted for THIS topology
           generation, else the enumerated colors — unmapped
           LEDs keep their colors (plan section 4). */
        Emitted& st = emitted_[key];
        std::vector<RgbColor> colors;
        const uint32_t led_count = snap.LedCount();
        if(st.generation == snap.topology_generation &&
           st.colors.size() == led_count)
        {
            colors = st.colors;
        }
        else
        {
            colors = snap.colors;
            colors.resize(led_count);   /* pad with black */
            st.generation = snap.topology_generation;
            st.seq        = 0;
        }

        for(const auto& w : kv.second)
        {
            if(w.first < colors.size())
            {
                colors[w.first] = w.second;
            }
        }

        RgbFrame frame;
        frame.controller          = key;
        frame.topology_generation = snap.topology_generation;
        frame.sequence            = ++st.seq;
        frame.sample_time         = eval.sample_time;
        frame.colors              = colors;
        st.colors                 = std::move(colors);
        out.push_back(std::move(frame));
    }
    return out;
}

bool DeviceMapper::BindingState(const std::string& binding_id,
                                studio::BindingStatus& out) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = resolved_.find(binding_id);
    if(it == resolved_.end())
    {
        return false;
    }
    out = it->second.status;
    return true;
}

bool DeviceMapper::BoundZone(const std::string& binding_id,
                             ZonePlacement& out) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = resolved_.find(binding_id);
    if(it == resolved_.end() ||
       it->second.status != studio::BindingStatus::Resolved)
    {
        return false;
    }
    out = it->second.zone;
    return true;
}

std::vector<DeviceKey> DeviceMapper::BoundControllers() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<DeviceKey> keys;
    for(const auto& kv : resolved_)
    {
        if(kv.second.status == studio::BindingStatus::Resolved)
        {
            keys.push_back(kv.second.zone.controller);
        }
    }
    std::sort(keys.begin(), keys.end());
    keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
    return keys;
}

} /* namespace openrgb2 */
