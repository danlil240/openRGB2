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
       enumeration — it can never seed a frame again. The
       same rule covers a pending identification overlay: a
       controller that left the topology is never flashed. */
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
    for(auto it = identify_.begin(); it != identify_.end();)
    {
        if(snapshots_.find(it->first) == snapshots_.end())
        {
            it = identify_.erase(it);
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
    if(doc_)
    {

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
    } /* doc_ */

    /* Identification overlays run with or without a scene —
       they exist to FIND hardware, so they cannot depend on
       the very bindings the user is still figuring out. */
    ApplyIdentify(out, eval.sample_time);
    return out;
}

/*---------------------------------------------------------*\
||| LED identification overlay                             ||
\|*---------------------------------------------------------*/

bool DeviceMapper::StartIdentify(const DeviceKey& controller,
                                 uint32_t led_start,
                                 uint32_t led_count,
                                 Clock::duration duration,
                                 TimePoint now,
                                 std::string& err)
{
    std::lock_guard<std::mutex> lock(mutex_);
    const auto sit = snapshots_.find(controller);
    if(sit == snapshots_.end())
    {
        err = "controller is not in the current topology";
        return false;
    }
    if(led_count == 0)
    {
        err = "identify range is empty";
        return false;
    }
    if(led_start + led_count > sit->second.LedCount() ||
       led_start + led_count < led_start)
    {
        err = "identify range exceeds the controller's " +
              std::to_string(sit->second.LedCount()) + " LEDs";
        return false;
    }
    Identify id;
    id.led_start = led_start;
    id.led_count = led_count;
    id.start     = now;
    id.until     = now + duration;
    identify_[controller] = id;
    return true;
}

bool DeviceMapper::CancelIdentify(const DeviceKey& controller)
{
    std::lock_guard<std::mutex> lock(mutex_);
    return identify_.erase(controller) != 0;
}

bool DeviceMapper::Identifying(const DeviceKey& controller,
                               TimePoint now) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = identify_.find(controller);
    return it != identify_.end() && now < it->second.until;
}

void DeviceMapper::ApplyIdentify(std::vector<RgbFrame>& out,
                                 TimePoint sample_time)
{
    /* Called with mutex_ held. Half-period blink: the
       identified range alternates lit/dark over a dark field
       so it reads unambiguously next to scene output. */
    static const RgbColor kLit  { 0xFF, 0xFF, 0xFF };
    static const RgbColor kDark { 0x00, 0x00, 0x00 };
    constexpr auto kHalfPeriod = std::chrono::milliseconds(250);

    for(auto it = identify_.begin(); it != identify_.end();)
    {
        if(sample_time >= it->second.until)
        {
            it = identify_.erase(it);   /* expiry is honest —
                                           the next frame is
                                           scene colors again */
            continue;
        }
        const Identify& id = it->second;
        const auto sit = snapshots_.find(it->first);
        if(sit == snapshots_.end() ||
           sit->second.connection != ConnectionState::Connected)
        {
            /* Same obsolete-frame rule as mapped writes —
               the overlay resumes if the controller returns
               before expiry. */
            ++it;
            continue;
        }
        const DeviceSnapshot& snap = sit->second;
        const bool lit =
            ((sample_time - id.start) / kHalfPeriod) % 2 == 0;

        RgbFrame* frame = nullptr;
        for(RgbFrame& f : out)
        {
            if(f.controller == it->first)
            {
                frame = &f;
                break;
            }
        }
        if(frame == nullptr)
        {
            /* No binding maps here — build the overlay frame
               from scratch on a dark baseline sized to the
               enumerated topology. The mapper is this
               controller's producer, so the frame joins the
               LIVE sequence space (emitted_) — the lane's
               out-of-order guard must see every frame it
               sends, identify or scene. */
            RgbFrame f;
            f.controller          = it->first;
            f.topology_generation = snap.topology_generation;
            f.sequence            = ++emitted_[it->first].seq;
            f.sample_time         = sample_time;
            f.colors.assign(snap.LedCount(), kDark);
            out.push_back(std::move(f));
            frame = &out.back();
        }
        else
        {
            /* The writes loop already stamped this frame's
               sequence from the same live space — keep it. */
            std::fill(frame->colors.begin(), frame->colors.end(),
                      kDark);
        }
        const uint32_t end =
            (uint32_t)std::min<size_t>(
                frame->colors.size(),
                (size_t)id.led_start + id.led_count);
        for(uint32_t i = id.led_start; i < end; ++i)
        {
            frame->colors[i] = lit ? kLit : kDark;
        }
        ++it;
    }
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
