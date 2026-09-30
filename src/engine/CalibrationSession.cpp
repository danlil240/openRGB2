/*---------------------------------------------------------*\
||| CalibrationSession.cpp                                    |
|||                                                           |
|||   Implementation notes (StudioTab::calib* parity):        |
|||                                                           |
|||   - the worker is the ONLY hardware writer for the        |
|||     session: begin / show / end serialize on one thread   |
|||     so the "one producer" rule is structural              |
|||   - Show() is a latch, not a queue — rapid steps keep     |
|||     only the newest LED (calib_wanted + calib_pending)    |
|||   - Begin claims the lane + playback hold synchronously   |
|||     (cheap flags) so nothing writes between the reply     |
|||     and the worker's first checkpoint                     |
|||   - restore goes through PlaybackController::             |
|||     RevalidateAndRestore — the CURRENT validated state,   |
|||     never a saved scene/frame                             |
|||                                                           |
|||   SPDX-License-Identifier: GPL-2.0-or-later               |
|\*---------------------------------------------------------*/

#include "CalibrationSession.h"

#include "DeviceMapper.h"         /* IsWirelessGroupController   */
#include "PlaybackController.h"

namespace openrgb2
{

namespace
{

const char* StateName(CalibrationSession::State s)
{
    switch(s)
    {
    case CalibrationSession::State::Idle:     return "idle";
    case CalibrationSession::State::Starting: return "starting";
    case CalibrationSession::State::Active:   return "active";
    case CalibrationSession::State::Ending:   return "ending";
    }
    return "?";
}

/* The zone's [start,count) span inside the complete-controller
   buffer — SDK zone order, same math the mapper uses. */
bool ZoneSpan(const DeviceSnapshot& snap, const std::string& zone,
              int& zone_index, uint32_t& start, uint32_t& count)
{
    uint32_t off = 0;
    for(size_t i = 0; i < snap.zones.size(); ++i)
    {
        if(snap.zones[i].name == zone)
        {
            zone_index = (int)i;
            start      = off;
            count      = snap.zones[i].led_count;
            return true;
        }
        off += snap.zones[i].led_count;
    }
    return false;
}

const DeviceSnapshot* FindSnap(const std::vector<DeviceSnapshot>& v,
                               const DeviceKey& key)
{
    for(const DeviceSnapshot& s : v)
    {
        if(s.key == key)
        {
            return &s;
        }
    }
    return nullptr;
}

} /* anonymous namespace */

CalibrationSession::CalibrationSession(Deps deps, Options options)
    : deps_(std::move(deps))
    , options_(options)
{
    worker_ = std::thread([this] { WorkerMain(); });
}

CalibrationSession::~CalibrationSession()
{
    Stop();
}

TimePoint CalibrationSession::Now() const
{
    return deps_.now ? deps_.now() : Clock::now();
}

bool CalibrationSession::OutputPaused() const
{
    return deps_.output_paused && deps_.output_paused();
}

void CalibrationSession::EmitChanged()
{
    if(deps_.changed)
    {
        deps_.changed();
    }
}

void CalibrationSession::Log(const std::string& msg) const
{
    if(deps_.log)
    {
        deps_.log(msg);
    }
}

/*---------------------------------------------------------*\
|| Queries                                                  ||
\*---------------------------------------------------------*/

CalibrationSession::Status CalibrationSession::GetStatus() const
{
    Status out;
    std::lock_guard<std::mutex> lock(state_mutex_);
    out.active           = state_ == State::Active;
    out.starting         = state_ == State::Starting;
    out.ending           = state_ == State::Ending;
    out.client_session   = client_session_;
    out.token            = token_;
    out.controller       = ctx_.key;
    out.controller_name  = ctx_.controller_name;
    out.zone             = ctx_.zone;
    out.zone_index       = ctx_.zone_index;
    out.led_count        = (int)ctx_.led_count;
    out.last_shown       = last_shown_;
    out.owns_group       = ctx_.owns_group;
    out.generation       = ctx_.generation;
    out.frames_written   = frames_written_;
    out.message          = message_;
    return out;
}

bool CalibrationSession::Busy() const
{
    std::lock_guard<std::mutex> lock(state_mutex_);
    return state_ != State::Idle;
}

bool CalibrationSession::Active() const
{
    std::lock_guard<std::mutex> lock(state_mutex_);
    return state_ == State::Active;
}

bool CalibrationSession::OwnsGroup(const DeviceKey& key) const
{
    std::lock_guard<std::mutex> lock(state_mutex_);
    return state_ != State::Idle && ctx_.key == key;
}

/*---------------------------------------------------------*\
|| Begin — validate, claim, queue activation                ||
\*---------------------------------------------------------*/

CalibrationSession::BeginResult CalibrationSession::Begin(
    uint64_t client_session, const DeviceKey& controller,
    const std::string& zone)
{
    BeginResult r;

    if(!deps_.snapshots || !deps_.direct_submit ||
       !deps_.set_suppressed)
    {
        r.error = "calibration is not wired on this engine";
        return r;
    }

    /*---- validate against the LIVE topology (pre-lock) ----*/
    const std::vector<DeviceSnapshot> snaps = deps_.snapshots();
    const DeviceSnapshot* snap = FindSnap(snaps, controller);
    if(snap == nullptr)
    {
        r.error = "controller '" + controller.device +
                  "' is not in the current topology";
        return r;
    }
    if(snap->connection != ConnectionState::Connected)
    {
        r.error = "controller '" + snap->name +
                  "' is not connected";
        return r;
    }
    if(zone.empty())
    {
        r.error = "calibration needs an explicit zone — "
                  "pick one before starting";
        return r;
    }
    int      zi = -1;
    uint32_t start = 0, count = 0;
    if(!ZoneSpan(*snap, zone, zi, start, count))
    {
        r.error = "controller '" + snap->name +
                  "' has no zone '" + zone + "'";
        return r;
    }
    if(count == 0)
    {
        r.error = "zone '" + zone + "' reports no LEDs";
        return r;
    }
    if(OutputPaused())
    {
        r.error = "output is paused (display off / suspend) — "
                  "calibration cannot run";
        return r;
    }
    /* A playback transition in flight owns the group right
       now — the honest answer is busy, never a race with its
       blocking section (the output hold parks FUTURE
       requests; it does not join a running transition). */
    if(deps_.playback && deps_.playback->AckPending())
    {
        r.error = "a playback-mode transition is in flight — "
                  "try again in a moment";
        return r;
    }

    /*---- claim (state lock; cheap flags only) -------------*/
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if(state_ != State::Idle)
        {
            r.error = "a calibration session is already " +
                      std::string(StateName(state_));
            return r;
        }

        BeginCtx ctx;
        ctx.key             = controller;
        ctx.controller_name = snap->name;
        ctx.zone            = zone;
        ctx.zone_index      = zi;
        ctx.led_start       = start;
        ctx.led_count       = count;
        ctx.generation      = snap->topology_generation;

        /* Lane ownership FIRST — a live producer's next frame
           must not slip in between the reply and the worker.
           A lane ALREADY suppressed (uploaded playback owns
           the group) is left alone: its owner lifts it. */
        const bool already =
            deps_.is_suppressed && deps_.is_suppressed(controller);
        if(!already)
        {
            deps_.set_suppressed(controller, true,
                                 "calibration");
            ctx.lane_suppressed_by_us = true;
        }

        /* Wireless-group ownership: the whole group belongs
           to the session for its duration — playback
           requests park and the group's poll stays dark
           until End restores through the validated state. */
        ctx.owns_group =
            IsWirelessGroupController(*snap) &&
            deps_.playback != nullptr;
        if(ctx.owns_group)
        {
            deps_.playback->SetOutputHold(true);
            ctx.hold_asserted = true;
        }

        /* The hold is up — a transition that claimed its
           blocking section just before our hold is now
           visible on AckPending: abort rather than race it. */
        if(ctx.owns_group && deps_.playback->AckPending())
        {
            deps_.playback->SetOutputHold(false);
            if(ctx.lane_suppressed_by_us)
            {
                deps_.set_suppressed(controller, false, "");
            }
            r.error = "a playback-mode transition is in "
                      "flight — try again in a moment";
            return r;
        }

        /* Seed the complete-controller frame from the last
           enumerated colors (unrelated zones keep them) —
           zero-fill when the backend reports none. */
        ctx.base.controller          = controller;
        ctx.base.topology_generation = ctx.generation;
        ctx.base.synthetic           = true;
        ctx.base.colors.assign(snap->LedCount(), RgbColor{});
        if(snap->colors.size() == snap->LedCount())
        {
            ctx.base.colors = snap->colors;
        }

        state_          = State::Starting;
        client_session_ = client_session;
        token_          = "cal-" + std::to_string(++token_seq_);
        ctx_            = ctx;
        message_        = "starting on '" + snap->name +
                          "' / '" + zone + "'…";
        end_reason_.clear();
        last_shown_     = -2;
        frames_written_ = 0;
        cancel_         = false;
        last_activity_  = Now();
        r.accepted      = true;
        r.pending       = true;
        r.token         = token_;
    }

    {
        std::lock_guard<std::mutex> lock(work_mutex_);
        jobs_.push_back(Job::Begin);
        work_cv_.notify_one();
    }
    EmitChanged();
    return r;
}

/*---------------------------------------------------------*\
|| Show / End / teardown                                    ||
\*---------------------------------------------------------*/

bool CalibrationSession::Show(uint64_t client_session,
                              const std::string& token,
                              int led, std::string& err)
{
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if(state_ != State::Active)
        {
            err = state_ == State::Starting
                ? "the session is still starting"
                : "no active calibration session";
            return false;
        }
        if(client_session_ != client_session ||
           token.empty() || token != token_)
        {
            err = "the session belongs to a different client "
                  "or token";
            return false;
        }
        if(led < -1 || led >= (int)ctx_.led_count)
        {
            err = "led " + std::to_string(led) +
                  " is out of range (" +
                  std::to_string(ctx_.led_count) +
                  " LEDs in the zone)";
            return false;
        }
        last_activity_ = Now();
    }
    {
        /* Latest-only: a newer Show replaces an undrained one
           — the hardware never replays skipped positions. */
        std::lock_guard<std::mutex> lock(work_mutex_);
        pending_led_  = led;
        pending_show_ = true;
        work_cv_.notify_one();
    }
    return true;
}

bool CalibrationSession::End(uint64_t client_session,
                             const std::string& token,
                             std::string& err)
{
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if(state_ == State::Idle)
        {
            return true;              /* idempotent, truthful */
        }
        if(client_session_ != client_session)
        {
            err = "the session belongs to a different client";
            return false;
        }
        if(!token.empty() && token != token_)
        {
            err = "unknown calibration token";
            return false;
        }
    }
    RequestEnd(client_session, "session ended");
    return true;
}

void CalibrationSession::EndForSession(uint64_t client_session)
{
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if(state_ == State::Idle ||
           client_session_ != client_session)
        {
            return;
        }
    }
    RequestEnd(client_session,
                   "the owning client disconnected");
}

void CalibrationSession::EndInternal(const std::string& reason)
{
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if(state_ == State::Idle)
        {
            return;
        }
    }
    RequestEnd(0, reason);
}

bool CalibrationSession::RequestEnd(
    uint64_t /*client_session*/, const std::string& reason)
{
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if(state_ == State::Idle)
        {
            return false;
        }
        /* Cancel a pending begin — the worker re-checks this
           at every boundary and must never activate. */
        if(state_ == State::Starting)
        {
            cancel_ = true;
        }
        if(end_reason_.empty())
        {
            end_reason_ = reason;
        }
        /* The drain becomes visible: a client polling status
           sees "ending" until the restore lands. */
        if(state_ != State::Ending)
        {
            state_ = State::Ending;
        }
    }
    {
        std::lock_guard<std::mutex> lock(work_mutex_);
        if(!end_posted_)
        {
            jobs_.push_back(Job::End);
            end_posted_ = true;
            work_cv_.notify_one();
        }
    }
    EmitChanged();
    return true;
}

/*---------------------------------------------------------*\
|| Poll — owner-loop maintenance                            ||
\*---------------------------------------------------------*/

void CalibrationSession::Poll(TimePoint now)
{
    BeginCtx ctx;
    TimePoint last{};
    bool ending = false;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if(state_ == State::Idle)
        {
            return;
        }
        ctx    = ctx_;
        last   = last_activity_;
        ending = state_ == State::Ending;
    }

    /* Output pause wins over everything — the lifecycle owns
       the post-pause restore (post-wake revalidation), so the
       session just releases its claims. */
    if(OutputPaused())
    {
        EndInternal("output paused (display off / suspend)");
        return;
    }
    /* Timeout guards a connected-but-abandoned session;
       Ending sessions are already draining, never timed. */
    if(!ending && now - last > options_.session_timeout)
    {
        EndInternal("session timed out");
        return;
    }
    std::string why;
    if(!ending && TopologyStale(ctx, why))
    {
        EndInternal(why);
    }
}

bool CalibrationSession::TopologyStale(const BeginCtx& ctx,
                                       std::string& why) const
{
    if(!deps_.snapshots)
    {
        return false;
    }
    const std::vector<DeviceSnapshot> snaps =
        deps_.snapshots();
    const DeviceSnapshot* snap = FindSnap(snaps, ctx.key);
    if(snap == nullptr)
    {
        why = "controller left the topology";
        return true;
    }
    if(snap->topology_generation != ctx.generation)
    {
        why = "the device list changed";
        return true;
    }
    if(snap->connection != ConnectionState::Connected)
    {
        why = "controller disconnected";
        return true;
    }
    int      zi = -1;
    uint32_t start = 0, count = 0;
    if(!ZoneSpan(*snap, ctx.zone, zi, start, count) ||
       start != ctx.led_start || count != ctx.led_count)
    {
        why = "the zone changed under the session";
        return true;
    }
    return false;
}

/*---------------------------------------------------------*\
|| Worker — the session's ONLY hardware writer              ||
\*---------------------------------------------------------*/

void CalibrationSession::WorkerMain()
{
    for(;;)
    {
        Job  job       = Job::End;
        int  led       = -2;
        bool have_job  = false;
        bool have_show = false;
        {
            std::unique_lock<std::mutex> lock(work_mutex_);
            work_cv_.wait(lock, [this]
            {
                return stop_ || !jobs_.empty() || pending_show_;
            });
            if(stop_)
            {
                return;
            }
            if(!jobs_.empty())
            {
                job      = jobs_.front();
                jobs_.pop_front();
                have_job = true;
            }
            else
            {
                led           = pending_led_;
                pending_show_ = false;
                have_show     = true;
            }
        }
        if(have_job)
        {
            if(job == Job::Begin)
            {
                DoBegin();
            }
            else
            {
                DoEnd();
            }
        }
        else if(have_show)
        {
            DoShow(led);
        }
    }
}

/* The begin worker's boundary checks — an End that arrived
   while the claim was in flight, an output pause, or an
   already-abandoned Starting state all mean "never light a
   LED". */
bool CalibrationSession::Aborted(const char* /*stage*/)
{
    std::lock_guard<std::mutex> lock(state_mutex_);
    return cancel_ || state_ != State::Starting ||
           OutputPaused();
}

void CalibrationSession::DoBegin()
{
    BeginCtx ctx;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        ctx = ctx_;
        if(state_ != State::Starting)
        {
            return;                /* end already landed      */
        }
    }
    auto fail = [this](const std::string& why)
    {
        RequestEnd(0, why);
    };

    if(Aborted("start"))
    {
        fail("cancelled before the lane was claimed");
        return;
    }
    /* Drain + silence the lane's in-flight sends — after this
       returns the session is the only writer that can reach
       the transport (lane suppressed, playback held). */
    if(deps_.drop_pending)
    {
        deps_.drop_pending(ctx.key);
    }
    if(deps_.wait_idle &&
       !deps_.wait_idle(ctx.key, options_.drain_timeout))
    {
        fail("the output lane did not drain in time");
        return;
    }
    if(Aborted("lane"))
    {
        fail("cancelled while claiming the lane");
        return;
    }
    /* Wireless group: take it out of the uploaded animation
       loop into direct color control — the SAME seam the
       mode controller uses; on refusal the session never
       existed. */
    if(ctx.owns_group && deps_.set_custom_mode)
    {
        std::string err;
        if(!deps_.set_custom_mode(ctx.key, err))
        {
            fail("the device refused direct color control" +
                 (err.empty() ? std::string()
                              : ": " + err));
            return;
        }
    }
    if(Aborted("mode"))
    {
        fail("cancelled during the mode switch");
        return;
    }

    /* First write: the bound zone goes dark — the session's
       deterministic starting point. */
    RgbFrame dark = ctx.base;
    for(uint32_t i = ctx.led_start;
        i < ctx.led_start + ctx.led_count &&
        i < dark.colors.size(); ++i)
    {
        dark.colors[i] = RgbColor{};
    }
    if(deps_.direct_submit(dark).status ==
       SubmitStatus::Rejected)
    {
        fail("the transport refused the calibration frame");
        return;
    }
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        ++frames_written_;
    }
    if(Aborted("activate"))
    {
        fail("cancelled before activation");
        return;
    }
    std::string why;
    if(TopologyStale(ctx, why))
    {
        fail(why);
        return;
    }
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        state_         = State::Active;
        last_activity_ = Now();
        message_       = "calibrating '" + ctx.controller_name +
                         "' / '" + ctx.zone + "' — " +
                         std::to_string(ctx.led_count) +
                         " LEDs";
    }
    EmitChanged();
}

void CalibrationSession::DoShow(int led)
{
    BeginCtx ctx;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if(state_ != State::Active)
        {
            return;               /* end landed first         */
        }
        ctx = ctx_;
    }
    if(OutputPaused())
    {
        return;                   /* Poll ends the session    */
    }
    RgbFrame f   = ctx.base;
    f.sample_time = Now();
    for(uint32_t i = ctx.led_start;
        i < ctx.led_start + ctx.led_count &&
        i < f.colors.size(); ++i)
    {
        f.colors[i] = RgbColor{};
    }
    if(led >= 0 && (uint32_t)led < ctx.led_count &&
       ctx.led_start + (uint32_t)led < f.colors.size())
    {
        f.colors[ctx.led_start + (uint32_t)led] =
            RgbColor{ 255, 255, 255 };
    }
    const SubmitResult r = deps_.direct_submit(f);
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if(r.status == SubmitStatus::Rejected)
        {
            message_ = "transport rejected the frame" +
                       (r.reason.empty() ? std::string()
                                         : ": " + r.reason);
        }
        else
        {
            ++frames_written_;
            last_shown_    = led;
            last_activity_ = Now();
        }
    }
    EmitChanged();
}

void CalibrationSession::DoEnd()
{
    BeginCtx ctx;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if(state_ == State::Idle)
        {
            std::lock_guard<std::mutex> wlock(work_mutex_);
            end_posted_ = false;
            return;
        }
        ctx = ctx_;
    }
    /* A straggler show must never fire after the restore. */
    {
        std::lock_guard<std::mutex> wlock(work_mutex_);
        pending_show_ = false;
        end_posted_   = false;
    }
    ReleaseClaims(ctx, /*revalidate*/true);
    std::string msg;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        msg = end_reason_.empty() ? "calibration ended"
                                  : end_reason_;
        state_          = State::Idle;
        client_session_ = 0;
        token_.clear();
        ctx_            = BeginCtx{};
        message_        = msg;
        end_reason_.clear();
        last_shown_     = -2;
        cancel_         = false;
    }
    Log("calibration ended: " + msg);
    EmitChanged();
}

void CalibrationSession::ReleaseClaims(const BeginCtx& ctx,
                                       bool revalidate)
{
    /* Order matters: the lane's delivered state resets BEFORE
       it unsuppresses so the worker's first post-session
       frame is never eaten by stale-color suppression. */
    if(ctx.lane_suppressed_by_us)
    {
        if(deps_.reset_delivered)
        {
            deps_.reset_delivered(ctx.key);
        }
        if(deps_.set_suppressed)
        {
            deps_.set_suppressed(ctx.key, false, "");
        }
    }
    if(ctx.hold_asserted && deps_.playback)
    {
        deps_.playback->SetOutputHold(false);
    }
    /* Wireless group: restore through the CURRENT validated
       playback state — the parked request replays, the
       uploaded loop re-arms, or live frames resume. NEVER a
       saved scene or a saved frame (plan §hardware), and the
       power lifecycle owns the restore while output is
       gated. */
    if(revalidate && ctx.owns_group && deps_.playback &&
       !OutputPaused() && !deps_.playback->PowerGate())
    {
        deps_.playback->RevalidateAndRestore();
    }
}

/*---------------------------------------------------------*\
|| Teardown                                                 ||
\*---------------------------------------------------------*/

void CalibrationSession::Stop()
{
    {
        std::lock_guard<std::mutex> lock(work_mutex_);
        stop_ = true;
        work_cv_.notify_all();
    }
    if(worker_.joinable())
    {
        worker_.join();
    }
    /* A live session's claims release WITHOUT a playback
       revalidate — engine shutdown sequences hardware state
       through OrderlyShutdown, not a mode re-arm. */
    BeginCtx ctx;
    bool     live = false;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        live = state_ != State::Idle;
        ctx  = ctx_;
    }
    if(live)
    {
        ReleaseClaims(ctx, /*revalidate*/false);
        std::lock_guard<std::mutex> lock(state_mutex_);
        state_ = State::Idle;
    }
}

} /* namespace openrgb2 */
