/*---------------------------------------------------------*\
||| CalibrationSession.h                                      |
|||                                                           |
|||   Physical LED calibration session (studio-import Task   |
|||   11 — the engine-owned half of the LED-calibration       |
|||   wizard; semantics ported from the preserved plugin's    |
|||   StudioTab calib path, adapted to this engine's          |
|||   ownership model).                                       |
|||                                                           |
|||   While a session runs it owns ONE controller — for a    |
|||   wireless controller the WHOLE wireless group — so a    |
|||   single producer lights exactly one zone LED at a       |
|||   time:                                                  |
|||                                                           |
|||   - the delivery lane for the controller is suppressed   |
|||     (live scene frames reject at the boundary — they     |
|||     never queue up to replay stale state)                |
|||   - uploaded playback on the wireless group is taken     |
|||     out of hardware (output hold parks playback          |
|||     requests and the group's Poll; the backend is put    |
|||     into direct-color mode through the SAME             |
|||     set_custom_mode seam the mode controller uses)       |
|||   - the session's own worker writes complete-controller  |
|||     frames straight to the transport — the identical     |
|||     direct_submit seam playback's pause-hold uses, never |
|||     a second lane or a second SDK client                 |
|||                                                           |
|||   Semantics carried over from StudioTab::calib*:         |
|||                                                           |
|||   - Begin is asynchronous: the reply is "starting", the  |
|||     session activates only after the lane drained, the   |
|||     mode switch landed, and the first black-out frame    |
|||     was accepted. End during Starting sets a cancel      |
|||     flag — the begin worker re-checks it at every        |
|||     boundary and never activates.                        |
|||   - Show(led) is latest-only: rapid steps coalesce to    |
|||     the newest index; -1 blacks the zone (blink-off).    |
|||   - End restores through the CURRENT validated playback  |
|||     state (PlaybackController::RevalidateAndRestore) —   |
|||     never a saved scene or a saved frame. Live lanes     |
|||     resume on the worker's next tick from live scene     |
|||     state.                                               |
|||   - Epoch/topology checks end the session honestly when  |
|||     the controller leaves or the device list reorders.   |
|||   - Output pause (display-off / suspend) ends the        |
|||     session; restore is owned by the power lifecycle.    |
|||   - A session is bound to an IPC client + opaque token;  |
|||     teardown of the owning session ends it.              |
|||                                                           |
|||   Qt-free by boundary (src/engine).                       |
|||                                                           |
|||   SPDX-License-Identifier: GPL-2.0-or-later               |
|\*---------------------------------------------------------*/

#pragma once

#include "EngineTypes.h"          /* DeviceKey, DeviceSnapshot,
                                     RgbFrame, SubmitResult   */

#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace openrgb2
{

class PlaybackController;

class CalibrationSession
{
public:
    enum class State
    {
        Idle,
        Starting,    /* begin worker owns the pause, not yet lit */
        Active,      /* one-LED-at-a-time writes                 */
        Ending,      /* end worker draining the restore          */
    };

    struct Deps
    {
        /* Live topology — the same enumeration surface the
           identify verb and the playback controller classify
           against. */
        std::function<std::vector<DeviceSnapshot>()>  snapshots;

        /* The uploaded-playback owner — non-null in
           production. Wireless-group sessions assert its
           output hold so playback requests park and the
           group poll stays dark, and end through
           RevalidateAndRestore (current validated state). */
        PlaybackController*                           playback = nullptr;

        /* DeliveryPipeline ownership primitives — the SAME
           seams playback uses; calibration never gets a
           second producer path. */
        std::function<void(const DeviceKey&, bool,
                           const std::string&)>       set_suppressed;
        std::function<bool(const DeviceKey&)>         is_suppressed;
        std::function<size_t(const DeviceKey&)>       drop_pending;
        std::function<void(const DeviceKey&)>         reset_delivered;
        std::function<bool(const DeviceKey&,
                           Clock::duration)>          wait_idle;

        /* The ONE hardware writer: a complete-controller
           frame straight to the transport while the lane is
           suppressed — identical to playback's pause-hold
           path (plan 3.6 "the controller itself is the one
           allowed producer"). */
        std::function<SubmitResult(const RgbFrame&)>  direct_submit;

        /* Direct color control for the controller (SDK
           SetCustomMode) — required to lift a wireless group
           out of an uploaded animation loop. */
        std::function<bool(const DeviceKey&,
                           std::string& err)>         set_custom_mode;

        /* Power gate: display-off / suspend / post-wake
           restore. A paused engine refuses begins and ends
           live sessions. */
        std::function<bool()>                         output_paused;

        std::function<void(const std::string&)>       log;

        /* Status changed (state/message/last LED) —
           production emits the session-targeted
           "calibration" IPC event. Called off-mutex. */
        std::function<void()>                         changed;

        /* Clock injection for tests. */
        std::function<TimePoint()>                    now;
    };

    struct Options
    {
        /* Bound for draining the lane's in-flight sends
           before the session claims the hardware — the
           "one producer" hand-off point. */
        Clock::duration drain_timeout =
            std::chrono::milliseconds(4000);

        /* Inactivity bound. Show() calls are the only
           activity; expiring ends the session through the
           normal restore path. */
        Clock::duration session_timeout =
            std::chrono::minutes(10);
    };

    struct Status
    {
        bool        active    = false;
        bool        starting  = false;
        bool        ending    = false;
        uint64_t    client_session = 0;
        std::string token;            /* owner-only capability  */
        DeviceKey   controller;
        std::string controller_name;
        std::string zone;
        int         zone_index = -1;
        int         led_count  = 0;
        int         last_shown = -2;  /* -2 none, -1 dark, >=0  */
        bool        owns_group = false;
        uint64_t    generation = 0;
        uint64_t    frames_written = 0;
        std::string message;          /* honest status/error    */
    };

    struct BeginResult
    {
        bool        accepted = false;
        bool        pending  = false;   /* activation in flight */
        std::string token;              /* set when accepted    */
        std::string error;              /* set on rejection     */
    };

    explicit CalibrationSession(Deps deps,
                                Options options = Options{});
    ~CalibrationSession();                              /* Stop() */

    CalibrationSession(const CalibrationSession&)            = delete;
    CalibrationSession& operator=(const CalibrationSession&) = delete;

    /* Begin a session on controller+zone. Validates the LIVE
       topology, takes the lane + (wireless) playback hold,
       then lets the worker drain/activate. Rejections carry
       an honest reason and change nothing. */
    BeginResult Begin(uint64_t client_session,
                      const DeviceKey& controller,
                      const std::string& zone);

    /* Light exactly one zone LED (0..led_count-1) or -1 for
       "zone dark" — latest-only coalesced. Owner + token
       validated; bounds checked against the session's own
       LED count. */
    bool Show(uint64_t client_session, const std::string& token,
              int led, std::string& err);

    /* Owner end: session_id must match; an empty token is
       accepted for the owning session, a provided one must
       equal the issued token. Idempotent — ending an idle
       engine reports true, not an error. */
    bool End(uint64_t client_session, const std::string& token,
             std::string& err);

    /* Owning-client teardown — disconnect ends its session
       without needing the token (the client is gone). */
    void EndForSession(uint64_t client_session);

    /* Engine/lifecycle end — output pause, timeout,
       topology churn, wrong-owner protection bypassed by
       design (the engine owns the session). */
    void EndInternal(const std::string& reason);

    Status GetStatus() const;
    bool   Busy() const;              /* any non-Idle state     */
    bool   Active() const;            /* lit LED writes         */
    /* True while a session owns this controller's group —
       the host's upkeep must not issue mode writes to it. */
    bool   OwnsGroup(const DeviceKey& key) const;

    /* Owner-loop maintenance (~500 ms cadence): output-pause
       end, inactivity timeout, topology-generation/zone
       re-validation. */
    void   Poll(TimePoint now = Clock::now());

    /* Engine teardown — joins the worker; a live session's
       claims are released WITHOUT a playback revalidate
       (OrderlyShutdown owns the final hardware state). */
    void   Stop();

private:
    enum class Job { Begin, End };

    struct BeginCtx
    {
        DeviceKey   key;
        std::string controller_name;
        std::string zone;
        int         zone_index = -1;
        uint32_t    led_start  = 0;
        uint32_t    led_count  = 0;
        uint64_t    generation = 0;
        bool        owns_group = false;
        bool        lane_suppressed_by_us = false;
        bool        hold_asserted         = false;
        RgbFrame    base;   /* seeded complete-controller frame */
    };

    void WorkerMain();
    void DoBegin();
    void DoShow(int led);
    void DoEnd();

    /* Shared end bookkeeping: posts Job{End} once, marks the
       begin worker's cancel flag, records the honest reason
       the status will surface. */
    bool RequestEnd(uint64_t client_session,
                    const std::string& reason);

    /* Release lane/hold and (wireless, unpaused) restore the
       CURRENT validated playback state. Called by the end
       worker; `revalidate` is skipped on the Stop() path. */
    void ReleaseClaims(const BeginCtx& ctx, bool revalidate);

    /* The begin worker's checkpoints — a cancelled or
       output-paused session must never light a LED. */
    bool Aborted(const char* stage);

    /* Topology revalidation: the bound controller must still
       be present/connected at its recorded generation with
       the zone intact. */
    bool TopologyStale(const BeginCtx& ctx,
                       std::string& why) const;

    TimePoint Now() const;
    bool      OutputPaused() const;
    void      EmitChanged();
    void      Log(const std::string& msg) const;

    Deps    deps_;
    Options options_;

    /* Session state — the IPC-facing record. */
    mutable std::mutex state_mutex_;
    State       state_         = State::Idle;
    uint64_t    client_session_= 0;
    std::string token_;
    BeginCtx    ctx_;
    std::string message_;
    std::string end_reason_;          /* pending status line    */
    int         last_shown_   = -2;
    uint64_t    frames_written_ = 0;
    uint64_t    token_seq_    = 0;
    TimePoint   last_activity_{};
    bool        cancel_       = false;

    /* One worker serializes begin/show/end — the single-
       producer guarantee is structural: every hardware
       write in a session happens on this thread. */
    std::mutex              work_mutex_;
    std::condition_variable work_cv_;
    std::deque<Job>         jobs_;
    bool                    end_posted_ = false;
    bool                    stop_       = false;
    std::thread             worker_;

    /* Latest-only show latch: rapid Show() calls collapse to
       the newest index — the hardware never replays the
       skipped positions. */
    int                     pending_led_   = -2;
    bool                    pending_show_  = false;
};

} /* namespace openrgb2 */
