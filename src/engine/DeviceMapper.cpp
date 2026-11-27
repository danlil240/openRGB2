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

/* Brightness is clamped to [0, 1] (NaN -> 0) before scaling:
   documents validate it, but an out-of-range value must never
   wrap a channel (1.5 * 200 & 0xFF = 44) or hit the undefined
   float->unsigned conversion of a negative/huge product. */
RgbColor ScaleColor(RgbColor c, float brightness)
{
    const float b = !(brightness > 0.0f) ? 0.0f
                  : (brightness > 1.0f ? 1.0f : brightness);
    RgbColor out;
    out.r = uint8_t((unsigned)(c.r * b));
    out.g = uint8_t((unsigned)(c.g * b));
    out.b = uint8_t((unsigned)(c.b * b));
    return out;
}

} /* anonymous namespace */

/* Bound on revision-stamped applies awaiting their first
   frame. The worker is latest-wins, so only the newest few can
   still arrive; while output is gated (display off) a burst of
   applies must not grow the queue without limit. */
static constexpr size_t kMaxStagedScenes = 8;

void DeviceMapper::Resolve(BoundScene& s) const
{
    s.resolved.clear();
    s.bound_objects    = 0;
    s.writable_objects = 0;
    if(!s.doc)
    {
        return;
    }
    for(const studio::DeviceBinding& binding : s.doc->bindings)
    {
        Resolution res;
        const studio::ResolvedBinding r =
            studio::ResolveBinding(binding, controllers_);
        res.status = r.status;
        res.reason = r.reason;

        if(r.status == studio::BindingStatus::Resolved)
        {
            const studio::ControllerSnapshot& cs =
                controllers_[(size_t)r.controller_index];
            res.zone.controller =
                controller_keys_[(size_t)r.controller_index];

            /* No zone requested -> zone 0 (single-zone
               devices), matching the imported adapter. */
            int zi = r.zone_index;
            if(zi < 0 && !cs.zones.empty())
            {
                zi = 0;
            }
            if(zi >= 0)
            {
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
        s.resolved[binding.id] = std::move(res);
    }

    /* AUTO-VERIFY ON RESOLVE (QA F04, decided semantics):
       bound Device objects whose binding resolved are
       writable — resolution against live hardware IS the
       verification grant; the stored document's authored
       `verified` bit is never mutated. Unresolved bindings
       stay unwritable. */
    for(const studio::SceneObject& obj : s.doc->objects)
    {
        if(obj.kind != studio::ObjectKind::Device ||
           obj.binding.empty())
        {
            continue;
        }
        ++s.bound_objects;
        const auto rit = s.resolved.find(obj.binding);
        if(rit != s.resolved.end() &&
           rit->second.status == studio::BindingStatus::Resolved)
        {
            ++s.writable_objects;
        }
    }
}

const DeviceMapper::BoundScene& DeviceMapper::Latest() const
{
    return staged_.empty() ? active_ : staged_.back();
}

void DeviceMapper::LogSummary()
{
    /* One line per scene apply plus one per count change (a
       controller arriving later can resolve more bindings
       without a new apply). */
    const BoundScene& s = Latest();
    if(!s.doc)
    {
        return;
    }
    if(summary_pending_ ||
       s.bound_objects    != logged_bound_ ||
       s.writable_objects != logged_writable_)
    {
        summary_pending_ = false;
        logged_bound_    = s.bound_objects;
        logged_writable_ = s.writable_objects;
        Log("scene apply: " + std::to_string(s.bound_objects) +
            " bound device objects, " +
            std::to_string(s.writable_objects) +
            " writable (resolved bindings auto-verify)");
    }
}

void DeviceMapper::SetScene(
    std::shared_ptr<const studio::SceneDocument> doc,
    uint64_t revision)
{
    std::lock_guard<std::mutex> lock(mutex_);
    BoundScene next;
    next.doc      = std::move(doc);
    next.revision = revision;
    /* Resolve now against the last enumeration (E-1): waiting
       for the host's next SyncDevices left every controller
       frozen for up to one maintenance poll per apply. */
    Resolve(next);

    if(revision == 0 || revision <= active_.revision)
    {
        active_ = std::move(next);
        staged_.clear();
    }
    else
    {
        staged_.push_back(std::move(next));
        while(staged_.size() > kMaxStagedScenes)
        {
            staged_.pop_front();
        }
    }
    stats_           = MapStats{};
    summary_pending_ = Latest().doc != nullptr;
    if(synced_)
    {
        LogSummary();
    }
}

std::shared_ptr<const studio::SceneDocument> DeviceMapper::Scene() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return Latest().doc;
}

void DeviceMapper::SyncDevices(
    const std::vector<DeviceSnapshot>& snapshots)
{
    std::lock_guard<std::mutex> lock(mutex_);

    controllers_.clear();
    controller_keys_.clear();
    controllers_.reserve(snapshots.size());
    controller_keys_.reserve(snapshots.size());
    snapshots_.clear();
    for(const DeviceSnapshot& snap : snapshots)
    {
        controllers_.push_back(ToControllerSnapshot(snap));
        controller_keys_.push_back(snap.key);
        snapshots_.emplace(snap.key, snap);
    }
    synced_ = true;

    /* Every held document re-resolves — the mapped one and any
       staged apply still waiting for its first frame. */
    Resolve(active_);
    for(BoundScene& s : staged_)
    {
        Resolve(s);
    }
    LogSummary();

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
    /* Promote the newest staged apply this frame was evaluated
       against: from here on the frame's colors and the mapped
       objects belong to the same document. Older staged
       entries were superseded (the worker is latest-wins). */
    while(!staged_.empty() &&
          staged_.front().revision <= eval.scene_revision)
    {
        active_ = std::move(staged_.front());
        staged_.pop_front();
    }
    const std::shared_ptr<const studio::SceneDocument>& doc =
        active_.doc;
    const std::map<std::string, Resolution>& resolved =
        active_.resolved;
    std::vector<RgbFrame> out;
    if(doc)
    {

    /* Collect writes per controller BEFORE assembling frames —
       document order is preserved per zone so overlapping
       mapped addresses resolve by deterministic document
       precedence (last write wins). */
    stats_ = MapStats{};
    for(auto& kv : write_scratch_)
    {
        kv.second.used = false;
        kv.second.writes.clear();
    }
    for(const studio::SceneObject& obj : doc->objects)
    {
        ++stats_.objects_total;
        if(obj.kind != studio::ObjectKind::Device ||
           obj.binding.empty())
        {
            continue;      /* Linked copies share their source
                              object's group colors — they are
                              render-only (address == -1) and
                              never multiply LED counts. */
        }
        ++stats_.objects_bound;
        const auto rit = resolved.find(obj.binding);
        if(rit == resolved.end() ||
           rit->second.status != studio::BindingStatus::Resolved)
        {
            /* F04: the authored `verified` bit does NOT gate
               output — a binding resolved against live
               hardware auto-verifies the object (runtime
               grant, see SyncDevices); an unresolved binding
               stays unwritable regardless of the flag. */
            ++stats_.skipped_unverified_or_unresolved;
            continue;
        }
        const ZonePlacement& zone = rit->second.zone;
        WriteQueue&      wq    = write_scratch_[zone.controller];
        wq.used = true;
        auto&            queue = wq.writes;
        bool             wrote = false;
        /* Hoisted: one evaluated-frame lookup per object; the
           painted fallback (EmitterColor's owner scan + map
           finds) runs only for an emitter with no evaluated
           color. */
        const auto fit = eval.colors.find(obj.id);
        const std::vector<studio::SceneColor>* evaluated =
            fit != eval.colors.end() ? &fit->second : nullptr;
        for(size_t e = 0; e < obj.emitters.size(); ++e)
        {
            const int addr = obj.emitters[e].address;
            if(addr < 0 || (uint32_t)addr >= zone.led_count)
            {
                continue;
            }
            const studio::SceneColor c =
                (evaluated && e < evaluated->size())
                    ? (*evaluated)[e]
                    : studio::EmitterColor(*doc, obj.id, (int)e);
            queue.emplace_back(zone.start_idx + (uint32_t)addr,
                               ScaleColor(FromSceneColor(c),
                                          doc->brightness));
            wrote = true;
        }
        if(wrote)
        {
            ++stats_.objects_emitted;
        }
    }

    for(const auto& kv : write_scratch_)
    {
        if(!kv.second.used)
        {
            continue;
        }
        const DeviceKey&      key  = kv.first;
        const auto            sit  = snapshots_.find(key);
        if(sit == snapshots_.end())
        {
            ++stats_.frames_dropped;
            continue;
        }
        const DeviceSnapshot& snap = sit->second;
        if(snap.connection != ConnectionState::Connected)
        {
            /* Stop obsolete frames on disconnect (plan
               section 6) — the binding stays resolved for
               the device's return. */
            ++stats_.frames_dropped;
            continue;
        }

        /* Baseline: the last frame emitted for THIS topology
           generation, else the enumerated colors — unmapped
           LEDs keep their colors (plan section 4). */
        /* The baseline is updated IN PLACE and copied once into
           the emitted frame (it used to be copied out, written,
           copied into the frame and moved back). */
        Emitted& st = emitted_[key];
        const uint32_t led_count = snap.LedCount();
        if(st.generation != snap.topology_generation ||
           st.colors.size() != led_count)
        {
            /* Only a real topology change restarts the sequence
               space (the lane's guard resets with it). A
               baseline reseed WITHIN the generation — the first
               scene frame for a controller that so far only got
               identify overlay frames (no colors booked) — must
               continue the live space those frames already
               spent, or the lane rejects every frame as
               out-of-order until the scene catches up (E-4). */
            if(st.generation != snap.topology_generation)
            {
                st.seq = 0;
            }
            st.colors = snap.colors;
            st.colors.resize(led_count);   /* pad with black */
            st.generation = snap.topology_generation;
        }
        std::vector<RgbColor>& colors = st.colors;

        for(const auto& w : kv.second.writes)
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
        out.push_back(std::move(frame));
    }
    stats_.objects_writable = active_.writable_objects;
    stats_.frames_emitted   = (uint64_t)out.size();
    } /* doc */

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

bool DeviceMapper::CancelIdentify(const DeviceKey& controller,
                                  TimePoint now)
{
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = identify_.find(controller);
    if(it == identify_.end())
    {
        return false;
    }
    /* Stamp expiry, never erase outright: the restore frame
       exists only in ApplyIdentify's expiry branch, so an
       erased booking with no scene frame following would
       freeze the blink's last phase on the hardware (round-2
       review). The stamped entry keeps IdentifyActive() true —
       the frame path stays awake for the one tick that prunes
       the booking and emits the restore. Identifying() reports
       false at once (now >= until); the caller's reply
       contract is unchanged. */
    it->second.until = now;
    return true;
}

bool DeviceMapper::Identifying(const DeviceKey& controller,
                               TimePoint now) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = identify_.find(controller);
    return it != identify_.end() && now < it->second.until;
}

bool DeviceMapper::IdentifyActive() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    /* Booked, not merely unexpired: an expired entry is still
       work — ApplyIdentify prunes it and emits the restore
       frame on the NEXT produced frame, so the idle hook must
       keep the frame path awake until the map drains. */
    return !identify_.empty();
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
            /* Expiry is honest — but with no scene frame
               following (no scene loaded, or the controller
               unmapped) the blink's last phase would FREEZE on
               the hardware, possibly lit. Emit ONE restore
               frame on the pre-identify baseline — the same
               baseline the writes loop uses (last emitted
               frame for this generation, else the enumerated
               colors) — then drop the booking. A controller
               that left or disconnected keeps the obsolete-
               frame rule: nothing is sent to it. */
            const DeviceKey& key = it->first;
            const auto sit = snapshots_.find(key);
            if(sit != snapshots_.end() &&
               sit->second.connection == ConnectionState::Connected)
            {
                bool have_frame = false;
                for(const RgbFrame& f : out)
                {
                    if(f.controller == key)
                    {
                        have_frame = true;   /* the scene writes
                                                already restored
                                                the look */
                        break;
                    }
                }
                if(!have_frame)
                {
                    const DeviceSnapshot& snap = sit->second;
                    const uint32_t led_count = snap.LedCount();
                    Emitted& st = emitted_[key];
                    RgbFrame  f;
                    f.controller          = key;
                    f.topology_generation =
                        snap.topology_generation;
                    f.sample_time         = sample_time;
                    if(st.generation == snap.topology_generation &&
                       st.colors.size() == led_count)
                    {
                        f.colors = st.colors;
                    }
                    else
                    {
                        f.colors = snap.colors;
                        f.colors.resize(led_count);
                        st.generation = snap.topology_generation;
                        /* st.seq is NOT reset here: overlay-only
                           controllers book no colors, so this
                           branch also runs mid-generation — and
                           the blink frames already spent seqs in
                           the lane's live space. Only a real
                           topology change may restart sequences
                           (the lane's guard resets with it). */
                    }
                    f.sequence = ++st.seq;
                    st.colors  = f.colors;
                    out.push_back(std::move(f));
                }
            }
            it = identify_.erase(it);
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
            /* Tag the entry with the generation its sequence
               belongs to (no colors booked — the blink is not a
               baseline): a stale entry from an older generation
               restarts at 1 like the lane's guard, and a scene
               frame in this generation continues the space
               (E-4). */
            Emitted& st = emitted_[it->first];
            if(st.generation != snap.topology_generation)
            {
                st.generation = snap.topology_generation;
                st.seq        = 0;
                st.colors.clear();
            }
            RgbFrame f;
            f.controller          = it->first;
            f.topology_generation = snap.topology_generation;
            f.sequence            = ++st.seq;
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
    const std::map<std::string, Resolution>& resolved =
        Latest().resolved;
    const auto it = resolved.find(binding_id);
    if(it == resolved.end())
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
    const std::map<std::string, Resolution>& resolved =
        Latest().resolved;
    const auto it = resolved.find(binding_id);
    if(it == resolved.end() ||
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
    for(const auto& kv : Latest().resolved)
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

MapStats DeviceMapper::LastMapStats() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return stats_;
}

void DeviceMapper::SetLogger(EngineLogFn log)
{
    std::lock_guard<std::mutex> lock(mutex_);
    log_ = std::move(log);
}

void DeviceMapper::Log(const std::string& line) const
{
    /* Callers hold mutex_ — the sink must never re-enter the
       mapper (documented on SetLogger). */
    EmitEngineLog(log_, line);
}

} /* namespace openrgb2 */
