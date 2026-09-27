/*---------------------------------------------------------*\
|| PlaybackController.cpp                                    ||
||                                                           ||
||   SPDX-License-Identifier: GPL-2.0-or-later               ||
\*---------------------------------------------------------*/

#include "PlaybackController.h"

#include "DeviceMapper.h"        /* IsWirelessGroupController */

#include "scene/SceneJson.h"     /* studio::ToJson — doc hash  */

#include <cstdio>
#include <utility>

namespace openrgb2
{

namespace
{
constexpr const char* kOwnReason =
    "uploaded playback owns the wireless group";
}

PlaybackController::PlaybackController(Deps deps, Config cfg)
    : deps_(std::move(deps)), cfg_(cfg), clock_(cfg.clock)
{
    if(!deps_.now)
    {
        deps_.now = [] { return Clock::now(); };
    }
}

void PlaybackController::SetSceneProvider(
    std::function<std::shared_ptr<const studio::SceneDocument>()> fn)
{
    std::lock_guard<std::mutex> lock(mutex_);
    deps_.scene = std::move(fn);
}

/*---------------------------------------------------------*\
|| Small helpers                                            ||
\*---------------------------------------------------------*/

uint32_t PlaybackController::PackColor(RgbColor c)
{
    /* OpenRGB colors are 0x00BBGGRR on the wire. */
    return uint32_t(c.r) | (uint32_t(c.g) << 8) |
           (uint32_t(c.b) << 16);
}

std::string PlaybackController::SceneKey(
    const studio::SceneDocument& doc)
{
    if(!doc.name.empty())
    {
        return "name:" + doc.name;
    }
    /* Unnamed document: stable content identity (FNV-1a over
       the canonical scene JSON). */
    const std::string j = studio::ToJson(doc).dump();
    uint64_t h = 1469598103934665603ull;
    for(const char ch : j)
    {
        h ^= (uint64_t)(uint8_t)ch;
        h *= 1099511628211ull;
    }
    char buf[32];
    std::snprintf(buf, sizeof(buf), "doc:%016llx",
                  (unsigned long long)h);
    return buf;
}

void PlaybackController::Changed()
{
    std::function<void()> fn;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        fn = deps_.changed;
    }
    if(fn)
    {
        fn();
    }
}

void PlaybackController::PersistLocked()
{
    if(scene_key_.empty())
    {
        return;
    }
    PerScene& ps    = per_scene_[scene_key_];
    ps.mode         = requested_mode_;
    ps.params       = params_;
}

bool PlaybackController::FindWireless(DeviceSnapshot& out) const
{
    if(!deps_.snapshots)
    {
        return false;
    }
    for(const DeviceSnapshot& snap : deps_.snapshots())
    {
        if(IsWirelessGroupController(snap) &&
           snap.connection == ConnectionState::Connected)
        {
            out = snap;
            return true;
        }
    }
    return false;
}

PlaybackModePatch PlaybackController::BuildPatchLocked() const
{
    /* Only the controls the preset actually supports are
       patched — other fields keep the enumerated value. */
    const PlaybackPresetSupport sup = PlaybackPresetControls(preset_);
    PlaybackModePatch p;
    if(sup.speed)
    {
        p.has_speed = true;
        p.speed     = (uint32_t)ClampPlaybackSpeed(params_.speed);
    }
    if(sup.direction)
    {
        p.has_direction = true;
        /* MODE_DIRECTION_RIGHT == 1 (RGBControllerInterface.h). */
        p.direction     = params_.reverse ? 1u : 0u;
    }
    if(sup.color)
    {
        p.has_colors = true;
        p.colors.push_back(PackColor(params_.color));
    }
    return p;
}

PlaybackController::Expect PlaybackController::BuildExpectLocked(
    const char* mode_name) const
{
    /* The acknowledgement surface: what RefreshStatus must
       report for the applied revision to land. */
    Expect e;
    e.mode_name = mode_name;
    const PlaybackPresetSupport sup = PlaybackPresetControls(preset_);
    if(sup.speed)
    {
        e.speed = (uint32_t)ClampPlaybackSpeed(params_.speed);
    }
    if(sup.direction)
    {
        e.check_direction = true;
        e.direction       = params_.reverse ? 1u : 0u;
    }
    if(sup.color)
    {
        e.check_color = true;
        e.color       = PackColor(params_.color);
    }
    return e;
}

bool PlaybackController::BackendReports(const Expect& exp,
                                        const DeviceKey& key)
{
    if(!deps_.active_mode)
    {
        return false;
    }
    BackendModeReport rep;
    if(!deps_.active_mode(key, rep) || !rep.valid)
    {
        return false;
    }
    if(rep.name != exp.mode_name)
    {
        return false;
    }
    if(exp.speed != 0 && rep.speed != exp.speed)
    {
        return false;
    }
    if(exp.check_direction && rep.direction != exp.direction)
    {
        return false;
    }
    if(exp.check_color &&
       (rep.colors.empty() || rep.colors[0] != exp.color))
    {
        return false;
    }
    return true;
}

void PlaybackController::MarkFailedLocked(const std::string& detail)
{
    upload_        = UploadState::Failed;
    status_detail_ = detail;
    ack_pending_   = false;
}

/*---------------------------------------------------------*\
|| Requests                                                 ||
\*---------------------------------------------------------*/

PlaybackController::Result PlaybackController::Request(
    PlaybackMode mode, PlaybackControl control,
    const PlaybackParams& params)
{
    std::shared_ptr<const studio::SceneDocument> doc;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        doc = deps_.scene ? deps_.scene() : nullptr;
    }
    return RequestImpl(mode, control, params, doc);
}

PlaybackController::Result PlaybackController::RequestImpl(
    PlaybackMode mode, PlaybackControl control,
    const PlaybackParams& params,
    const std::shared_ptr<const studio::SceneDocument>& doc)
{
    Result        r;
    const TimePoint now = deps_.now();
    DeviceSnapshot    wireless;
    bool              to_animation = false;
    bool              to_live      = false;
    bool              control_only_req = false;
    std::string       sdk_name;

    /*---- Validate + plan under the lock -------------------*/
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if(transition_)
        {
            /* Mode changes are serialized — a second request
               while one is in flight is rejected, never
               silently queued. */
            r.reason = "a playback transition is already in progress";
            return r;
        }
        if(doc)
        {
            scene_key_ = SceneKey(*doc);
        }

        const ScenePlaybackCaps caps = ClassifyScenePlayback(doc.get());
        if(mode == PlaybackMode::AnimationPlayback)
        {
            if(!caps.caps.animation_playback)
            {
                r.reason = caps.reason;
                return r;
            }
            if(!deps_.select_mode || !deps_.set_suppressed ||
               !deps_.snapshots)
            {
                r.reason = "no backend transport configured";
                return r;
            }
            if(!FindWireless(wireless))
            {
                r.reason = "no Lian Li wireless group is connected";
                return r;
            }
            sdk_name = PlaybackPresetSdkName(caps.preset);
            bool offered = false;
            for(const std::string& m : wireless.modes)
            {
                if(m == sdk_name)
                {
                    offered = true;
                    break;
                }
            }
            if(!offered)
            {
                r.reason = "the backend does not offer mode '" +
                           sdk_name + "' on the wireless group";
                return r;
            }
            try
            {
                geometry_ = EffectiveLoopGeometry(caps.preset,
                                                  params,
                                                  wireless.LedCount());
            }
            catch(const std::exception& e)
            {
                r.reason = e.what();
                return r;
            }
            preset_ = caps.preset;
        }

        /* Idempotent no-op: identical already-applied state. */
        const bool same_params =
            params.speed   == params_.speed   &&
            params.reverse == params_.reverse &&
            params.color   == params_.color;
        if(mode == applied_mode_ && !ack_pending_ &&
           (mode == PlaybackMode::LiveEffects ||
            (upload_ == UploadState::Active && same_params &&
             control == control_)))
        {
            /* The persisted per-scene selection records THIS
               request — a stale requested_mode_/params_ left
               by an earlier failed transition must not leak
               into the per-scene store. */
            requested_mode_ = mode;
            params_         = params;
            r.accepted = true;
            r.revision = applied_param_rev_;
            PersistLocked();
            return r;
        }

        /* Control-only request on an acknowledged upload —
           the uploaded loop already matches, so pause/stop/
           resume needs no new select: it runs the transport-
           control path directly. */
        const bool control_only =
            mode == PlaybackMode::AnimationPlayback &&
            applied_mode_ == PlaybackMode::AnimationPlayback &&
            upload_ == UploadState::Active && !ack_pending_ &&
            same_params && caps.preset == preset_ &&
            control != control_;
        if(control_only)
        {
            transition_        = true;
            ++epoch_;
            requested_mode_    = mode;
            requested_control_ = control;
            ++next_rev_;
            requested_rev_ = next_rev_;
            PersistLocked();
            control_only_req = true;
        }
        else
        {
            /* Claim the transition. */
            transition_ = true;
            ++epoch_;
            to_animation = (mode == PlaybackMode::AnimationPlayback);
            /* Back to live only needs a transition when an owned
               group actually exists — has_wireless_ is set and
               cleared together with upload state. */
            to_live      = (mode == PlaybackMode::LiveEffects &&
                            has_wireless_);
            params_            = params;
            requested_mode_    = mode;
            requested_control_ =
                to_animation ? control : PlaybackControl::Playing;
        }
    }

    /*---- Blocking section — no lock held ------------------*/
    std::string io_err;
    bool        live_needs_ack = false;
    if(control_only_req)
    {
        DoControl(control, io_err);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            transition_ = false;
            if(!io_err.empty())
            {
                status_detail_ = io_err;
                r.reason       = io_err;
                return r;
            }
            r.accepted = true;
            r.pending  = ack_pending_;
            r.revision = requested_rev_;
        }
        Changed();
        return r;
    }
    if(to_animation)
    {
        /* One producer: suppress FIRST (subsequent live
           submissions reject with reason), then invalidate
           queued work, then serialize behind the last
           in-flight send — a queued live frame can never
           overwrite the new uploaded loop. */
        deps_.set_suppressed(wireless.key, true, kOwnReason);
        if(deps_.drop_pending)
        {
            deps_.drop_pending(wireless.key);
        }
        if(deps_.wait_idle &&
           !deps_.wait_idle(wireless.key, cfg_.drain_timeout))
        {
            deps_.set_suppressed(wireless.key, false, "");
            std::lock_guard<std::mutex> lock(mutex_);
            transition_      = false;
            r.reason = "a live send could not drain before the "
                       "mode switch; retry";
            status_detail_   = r.reason;
            return r;
        }
        const PlaybackModePatch patch = [this]
        {
            std::lock_guard<std::mutex> lock(mutex_);
            return BuildPatchLocked();
        }();
        if(!deps_.select_mode(wireless.key, sdk_name, patch, io_err))
        {
            deps_.set_suppressed(wireless.key, false, "");
            std::lock_guard<std::mutex> lock(mutex_);
            transition_ = false;
            r.reason    = io_err.empty() ? "mode select failed"
                                         : io_err;
            status_detail_ = r.reason;
            return r;
        }
    }
    else if(to_live)
    {
        DeviceSnapshot ws;
        if(FindWireless(ws))
        {
            /* Invalidate obsolete live work and serialize
               behind in-flight sends BEFORE selecting Direct —
               then the lane stays suppressed until the backend
               reports Direct applied (the ack path resumes
               it). */
            if(deps_.drop_pending)
            {
                deps_.drop_pending(wireless_key_);
            }
            if(deps_.wait_idle)
            {
                deps_.wait_idle(wireless_key_, cfg_.drain_timeout);
            }
            if(deps_.set_custom_mode &&
               !deps_.set_custom_mode(wireless_key_, io_err))
            {
                std::lock_guard<std::mutex> lock(mutex_);
                transition_      = false;
                status_detail_   = io_err;
                r.reason = "could not restore the direct mode: " +
                           io_err;
                return r;
            }
            live_needs_ack = deps_.set_custom_mode != nullptr;
        }
        /* else: the group is gone — nothing on hardware needs
           restoring; the commit below lands live immediately. */
    }

    /*---- Commit -------------------------------------------*/
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ++next_rev_;
        requested_rev_ = next_rev_;

        if(to_animation)
        {
            has_wireless_   = true;
            wireless_key_   = wireless.key;
            wireless_gen_   = wireless.topology_generation;
            wireless_leds_  = wireless.LedCount();
            suppressed_     = true;
            upload_         = UploadState::Uploading;
            status_detail_.clear();
            expect_         = BuildExpectLocked(sdk_name.c_str());
            pending_param_rev_ = next_rev_;
            if(applied_mode_ != PlaybackMode::AnimationPlayback)
            {
                pending_preset_rev_ = next_rev_;
            }
            ack_pending_  = true;
            ack_deadline_ = now + cfg_.ack_timeout;
            next_refresh_ = now;
            r.pending     = true;
        }
        else if(to_live && live_needs_ack)
        {
            /* Applied state stays animation until Direct is
               reported — then the ack path resumes the lane. */
            expect_         = Expect{};
            expect_.mode_name = "Direct";
            pending_param_rev_ = next_rev_;
            ack_pending_  = true;
            ack_deadline_ = now + cfg_.ack_timeout;
            next_refresh_ = now;
            r.pending     = true;
        }
        else if(to_live)
        {
            /* The group is gone — nothing to wait on. */
            applied_mode_      = PlaybackMode::LiveEffects;
            control_           = PlaybackControl::Playing;
            upload_            = UploadState::None;
            status_detail_.clear();
            applied_param_rev_ = next_rev_;
            suppressed_        = false;
            has_wireless_      = false;
        }
        else
        {
            /* Already live: nothing to transition. */
            applied_mode_ = PlaybackMode::LiveEffects;
            control_      = PlaybackControl::Playing;
            upload_       = UploadState::None;
            status_detail_.clear();
            applied_param_rev_ = next_rev_;
            suppressed_        = false;
        }
        transition_ = false;
        PersistLocked();
        r.accepted = true;
        r.revision = requested_rev_;
    }
    if(to_live && !live_needs_ack)
    {
        /* Lane resume follows the same ordering as the ack
           path: delivered-state reset first, then submissions
           flow again. */
        if(deps_.reset_delivered)
        {
            deps_.reset_delivered(wireless_key_);
        }
        if(deps_.set_suppressed)
        {
            deps_.set_suppressed(wireless_key_, false, "");
        }
    }
    Changed();
    return r;
}

/*---------------------------------------------------------*\
|| Scene persistence                                        ||
\*---------------------------------------------------------*/

void PlaybackController::OnSceneApplied(
    std::shared_ptr<const studio::SceneDocument> doc)
{
    if(!doc)
    {
        return;
    }
    PerScene   choice;
    const std::string key = SceneKey(*doc);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        scene_key_ = key;
        const auto it = per_scene_.find(key);
        if(it != per_scene_.end())
        {
            choice = it->second;
        }
    }
    /* Re-apply the scene's persisted selection. A scene that
       never chose playback defaults to live; one whose
       persisted animation no longer qualifies under the NEW
       document's capabilities falls back to live — the
       persisted choice is kept so a qualifying reload can
       still restore it. */
    if(choice.mode == PlaybackMode::AnimationPlayback &&
       !ClassifyScenePlayback(doc.get()).caps.animation_playback)
    {
        choice.mode = PlaybackMode::LiveEffects;
    }
    RequestImpl(choice.mode, PlaybackControl::Playing,
                choice.params, doc);
}

/*---------------------------------------------------------*\
|| Transport control (pause / stop / resume)                ||
\*---------------------------------------------------------*/

void PlaybackController::DoControl(PlaybackControl control,
                                   std::string& err)
{
    DeviceSnapshot    wireless;
    LianLiWireless::PlaybackPreset preset;
    PlaybackParams    params;
    PlaybackLoopGeometry geometry;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        preset   = preset_;
        params   = params_;
        geometry = geometry_;
    }

    if(control == PlaybackControl::Playing)
    {
        /* Resume: re-upload the preset — a direct-mode write
           (pause/stop) ended the device's own loop. */
        if(!FindWireless(wireless))
        {
            err = "the wireless group disappeared";
            return;
        }
        PlaybackModePatch patch;
        std::string       name;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            patch = BuildPatchLocked();
            name  = PlaybackPresetSdkName(preset_);
        }
        if(!deps_.select_mode(wireless.key, name, patch, err))
        {
            return;
        }
        const TimePoint now = deps_.now();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            ++next_rev_;
            requested_rev_     = next_rev_;
            pending_param_rev_ = next_rev_;
            expect_            = BuildExpectLocked(name.c_str());
            upload_            = UploadState::Uploading;
            ack_pending_       = true;
            ack_deadline_      = now + cfg_.ack_timeout;
            next_refresh_      = now;
        }
        return;
    }

    /* Paused / Stopped: the direct mode takes the group back
       from the uploaded loop, then ONE complete-controller
       frame lands the hold state — a sampled frame for pause,
       black for stop. */
    if(!FindWireless(wireless))
    {
        err = "the wireless group disappeared";
        return;
    }
    if(deps_.set_custom_mode &&
       !deps_.set_custom_mode(wireless.key, err))
    {
        return;
    }
    std::vector<RgbColor> colors;
    if(control == PlaybackControl::Paused)
    {
        const TimePoint now = deps_.now();
        double phase = 0.0;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            const auto p = clock_.Phase(now, geometry.cycle_s);
            if(p)
            {
                phase = *p;
            }
            else
            {
                /* Local-timeline fallback — the hold is the
                   frame the loop would show at this instant. */
                const double elapsed = std::chrono::duration<double>(
                    now - active_since_).count();
                phase = geometry.cycle_s > 0.0
                    ? elapsed / geometry.cycle_s : 0.0;
            }
        }
        colors = sampler_.Sample(preset, params,
                                 wireless.LedCount(),
                                 geometry.frame_count, phase);
    }
    else
    {
        colors.assign(wireless.LedCount(), RgbColor{ 0, 0, 0 });
    }

    if(deps_.direct_submit)
    {
        RgbFrame frame;
        frame.controller          = wireless.key;
        frame.topology_generation = wireless.topology_generation;
        frame.sample_time         = deps_.now();
        frame.colors              = std::move(colors);
        const SubmitResult r = deps_.direct_submit(frame);
        if(r.status == SubmitStatus::Rejected)
        {
            err = r.reason;
            return;
        }
    }

    /* Wired peers converge on the same held state. */
    if(control == PlaybackControl::Stopped && deps_.wired_submit &&
       deps_.wired_targets)
    {
        for(const DeviceSnapshot& t : deps_.wired_targets())
        {
            RgbFrame frame;
            frame.controller          = t.key;
            frame.topology_generation = t.topology_generation;
            frame.sample_time         = deps_.now();
            frame.colors.assign(t.LedCount(), RgbColor{0,0,0});
            deps_.wired_submit(frame);
        }
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        control_ = control;
    }
}

/*---------------------------------------------------------*\
|| Wired producer (scene-clock hook)                        ||
\*---------------------------------------------------------*/

void PlaybackController::Tick(TimePoint now)
{
    LianLiWireless::PlaybackPreset preset;
    PlaybackParams       params;
    PlaybackLoopGeometry geometry;
    double               cycle_s = 0.0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if(applied_mode_ != PlaybackMode::AnimationPlayback ||
           upload_      != UploadState::Active             ||
           control_     != PlaybackControl::Playing)
        {
            return;
        }
        if(now - last_wired_send_ < cfg_.wired_frame_interval)
        {
            return;
        }
        preset   = preset_;
        params   = params_;
        geometry = geometry_;
        cycle_s  = geometry_.cycle_s;
    }

    /* The validated wireless clock drives the phase when
       Healthy; otherwise the honest fallback is the local
       timeline anchored at activation (status reports the
       real sync health — never a false lock). */
    double phase = 0.0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto p = clock_.Phase(now, cycle_s);
        if(p)
        {
            phase = *p;
        }
        else if(cycle_s > 0.0)
        {
            phase = std::chrono::duration<double>(
                now - active_since_).count() / cycle_s;
        }
        last_wired_send_ = now;
    }

    if(!deps_.wired_targets || !deps_.wired_submit ||
       geometry.frame_count == 0)
    {
        return;
    }
    for(const DeviceSnapshot& t : deps_.wired_targets())
    {
        if(t.connection != ConnectionState::Connected)
        {
            continue;
        }
        std::vector<RgbColor> colors =
            sampler_.Sample(preset, params, t.LedCount(),
                            geometry.frame_count, phase);
        if(colors.empty())
        {
            continue;
        }
        RgbFrame frame;
        frame.controller          = t.key;
        frame.topology_generation = t.topology_generation;
        frame.sample_time         = now;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            frame.sequence = ++wired_seq_;
        }
        frame.colors = std::move(colors);
        /* Rejections (lane gone, generation bump) are absorbed
           by the pipeline's own counters — bounded, never
           silently lost. */
        deps_.wired_submit(frame);
    }
}

/*---------------------------------------------------------*\
|| Maintenance (owner-loop hook)                            ||
\*---------------------------------------------------------*/

void PlaybackController::Poll(TimePoint now)
{
    /*---- Gather due work ----------------------------------*/
    bool     do_refresh  = false;
    bool     do_clock    = false;
    bool     do_control  = false;
    uint64_t epoch       = 0;
    DeviceKey key;
    Expect   exp;
    PlaybackControl want_control = PlaybackControl::Playing;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if(transition_)
        {
            return;   /* a serialized transition is in flight */
        }
        epoch = epoch_;
        key   = wireless_key_;

        if(suppressed_ && has_wireless_ && deps_.set_suppressed)
        {
            /* A lane recreated by a topology change loses its
               suppression flag — re-assert it (idempotent). */
            deps_.set_suppressed(wireless_key_, true, kOwnReason);
        }

        if(ack_pending_ && now >= next_refresh_ &&
           deps_.refresh_status)
        {
            do_refresh = true;
            exp        = expect_;
        }
        if(upload_ == UploadState::Active &&
           applied_mode_ == PlaybackMode::AnimationPlayback &&
           control_ == PlaybackControl::Playing &&
           deps_.clock_sample &&
           now - last_clock_poll_ >= cfg_.clock_poll_interval)
        {
            do_clock = true;
        }
        if(upload_ == UploadState::Active &&
           applied_mode_ == PlaybackMode::AnimationPlayback &&
           !ack_pending_ &&
           control_ != requested_control_)
        {
            do_control   = true;
            want_control = requested_control_;
            transition_  = true;   /* serialize like a request */
        }
    }

    /*---- Acknowledgement polling (unlocked I/O) ------------*/
    if(do_refresh)
    {
        bool applied = false;
        std::string io_err;
        if(deps_.refresh_status(io_err))
        {
            applied = BackendReports(exp, key);
        }
        bool           resumed_live = false;
        bool           run_control  = false;
        PlaybackControl want        = PlaybackControl::Playing;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if(epoch_ == epoch && ack_pending_)
            {
                if(applied)
                {
                    /* The backend reported the expected mode:
                       the requested revision is now applied. */
                    ack_pending_       = false;
                    applied_param_rev_ = pending_param_rev_;
                    if(requested_mode_ ==
                       PlaybackMode::AnimationPlayback)
                    {
                        applied_preset_rev_ = pending_preset_rev_;
                        applied_mode_       =
                            PlaybackMode::AnimationPlayback;
                        upload_             = UploadState::Active;
                        active_since_       = now;
                        clock_.Reset();
                        if(requested_control_ ==
                           PlaybackControl::Playing)
                        {
                            control_ = PlaybackControl::Playing;
                        }
                        else
                        {
                            run_control = true;
                            want        = requested_control_;
                        }
                    }
                    else
                    {
                        /* "Direct" reported — the uploaded loop
                           is released. Resume the lane AFTER
                           its delivered-state reset so the
                           next freshly evaluated complete-
                           group frame always delivers. */
                        applied_mode_ = PlaybackMode::LiveEffects;
                        upload_       = UploadState::None;
                        control_      = PlaybackControl::Playing;
                        suppressed_   = false;
                        status_detail_.clear();
                        resumed_live  = true;
                    }
                }
                else if(now >= ack_deadline_)
                {
                    MarkFailedLocked(
                        "the backend did not report the applied "
                        "mode before the acknowledgement bound");
                }
                else
                {
                    next_refresh_ = now + cfg_.ack_poll_interval;
                }
            }
        }
        if(resumed_live)
        {
            /* Outside the lock: clear the delivered-state
               memory, then let live submissions through — a
               stale delivered frame from before the loop can
               never eat the fresh return-to-live frame. */
            if(deps_.reset_delivered)
            {
                deps_.reset_delivered(key);
            }
            if(deps_.set_suppressed)
            {
                deps_.set_suppressed(key, false, "");
            }
        }
        if(run_control)
        {
            std::string err;
            DoControl(want, err);
            if(!err.empty())
            {
                std::lock_guard<std::mutex> lock(mutex_);
                status_detail_ = err;
            }
        }
        Changed();
    }

    /*---- Validated clock sampling --------------------------*/
    if(do_clock)
    {
        PlaybackClockSnapshot snap;
        if(deps_.clock_sample(snap))
        {
            std::lock_guard<std::mutex> lock(mutex_);
            clock_.Update(snap);
            last_clock_poll_ = now;
        }
        else
        {
            std::lock_guard<std::mutex> lock(mutex_);
            last_clock_poll_ = now;
        }
    }

    /*---- Topology re-resolution ----------------------------*/
    {
        bool lost = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            lost = has_wireless_ &&
                   upload_ != UploadState::None;
        }
        if(lost)
        {
            bool found = false;
            DeviceSnapshot snap;
            if(deps_.snapshots)
            {
                for(const DeviceSnapshot& s : deps_.snapshots())
                {
                    if(s.key == wireless_key_ &&
                       s.connection == ConnectionState::Connected)
                    {
                        snap  = s;
                        found = true;
                        break;
                    }
                }
            }
            std::lock_guard<std::mutex> lock(mutex_);
            if(!found)
            {
                MarkFailedLocked("the wireless group disappeared");
                Changed();
            }
            else
            {
                wireless_gen_  = snap.topology_generation;
                wireless_leds_ = snap.LedCount();
            }
        }
    }

    /*---- Post-ack transport control ------------------------*/
    if(do_control)
    {
        std::string err;
        DoControl(want_control, err);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            transition_ = false;
            if(!err.empty())
            {
                status_detail_ = err;
            }
        }
        Changed();
    }
}

/*---------------------------------------------------------*\
|| Reporting                                                ||
\*---------------------------------------------------------*/

PlaybackStatus PlaybackController::Status() const
{
    const TimePoint now = deps_.now();
    std::lock_guard<std::mutex> lock(mutex_);
    PlaybackStatus st;
    st.mode               = applied_mode_;
    st.preset_revision    = applied_preset_rev_;
    st.parameter_revision = applied_param_rev_;
    st.upload             = upload_;
    st.control            = control_;
    if(applied_mode_ == PlaybackMode::AnimationPlayback ||
       upload_ == UploadState::Uploading)
    {
        st.effective_frame_count      = geometry_.frame_count;
        st.effective_frame_interval_s = geometry_.frame_interval_s;
        st.clock_sample_s             = clock_.ClockSeconds(now);
        st.clock_uncertainty_s        = clock_.UncertaintyS(now);
        st.sync                       = clock_.Health(now);
    }
    /* physical stays provenance Unknown — no hardware-side
       measurement channel exists over the SDK protocol. */
    return st;
}

PlaybackMode PlaybackController::RequestedMode() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return requested_mode_;
}

uint64_t PlaybackController::RequestedRevision() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return requested_rev_;
}

bool PlaybackController::AckPending() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return ack_pending_ || transition_;
}

std::string PlaybackController::Detail() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return status_detail_;
}

PlaybackCapabilities PlaybackController::Caps() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    const std::shared_ptr<const studio::SceneDocument> doc =
        deps_.scene ? deps_.scene() : nullptr;
    return ClassifyScenePlayback(doc.get()).caps;
}

ScenePlaybackCaps PlaybackController::SceneCaps() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    const std::shared_ptr<const studio::SceneDocument> doc =
        deps_.scene ? deps_.scene() : nullptr;
    return ClassifyScenePlayback(doc.get());
}

bool PlaybackController::OwnsGroup(const DeviceKey& key) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return suppressed_ && has_wireless_ && wireless_key_ == key;
}

bool PlaybackController::PlaybackOwnsWired() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return applied_mode_ == PlaybackMode::AnimationPlayback &&
           upload_       == UploadState::Active;
}

const PlaybackClockSync& PlaybackController::ClockSync() const
{
    return clock_;
}

size_t PlaybackController::PersistedScenes() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return per_scene_.size();
}

} /* namespace openrgb2 */
