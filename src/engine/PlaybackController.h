/*---------------------------------------------------------*\
|| PlaybackController.h                                      ||
||                                                           ||
||   The playback-mode state machine (plan sections 3.4     ||
||   and 4 — Task 3.6): owns the LiveEffects <->            ||
||   AnimationPlayback contract for the engine.             ||
||                                                           ||
||   Responsibilities:                                      ||
||   - persist the selected PlaybackMode + parameters per   ||
||     scene (scene documents carry no playback field —     ||
||     src/core is frozen — so the engine keeps a per-scene │
||     store keyed by scene identity; it survives UI        ||
||     restarts because the engine owns output state)       ||
||   - validate requests against preset capabilities: only  ||
||     Spectrum Cycle / Rainbow Wave / Breathing may use    ||
||     AnimationPlayback; everything else is rejected with  ||
||     the classifier's short reason                        ||
||   - run the serialized transition protocol: suppress the ||
||     wireless group's lane FIRST (new live submissions    ||
||     rejected with reason), drop queued work, wait for    ||
||     in-flight sends to drain, then UPDATEMODE-select the │
||     uploaded preset through the SDK                      ||
||   - revision acknowledgement: mode/parameter changes are │
||     fire-and-forget on the SDK wire, so the applied      ||
||     revision only lands when RefreshStatus() reports the │
||     expected mode — the status stays pending until then  │
||   - produce the matching wired-effect timeline: while    ||
||     the uploaded loop owns the group, other devices      ||
||     receive sampled frames from the same preset on the   ||
||     same timeline (validated wireless clock when a       ||
||     diagnostics channel feeds one; a local-timeline      ||
||     fallback otherwise — sync is reported honestly)      ||
||   - lifecycle: pause = sampled-frame hold, stop = black,  ||
||     return-to-live ends with the lane resumed and its    ||
||     delivered-state reset so the next freshly evaluated  ||
||     complete-group frame always delivers                  ||
||                                                           ||
||   The controller is transport-agnostic: every backend/   ||
||   pipeline/persistence operation is an injected          ||
||   std::function — production wires OpenRgbClient +       ||
||   DeliveryPipeline, tests wire fakes. No Qt.             ||
||                                                           ||
||   SPDX-License-Identifier: GPL-2.0-or-later               ||
\*---------------------------------------------------------*/

#pragma once

#include "EngineTypes.h"
#include "PlaybackClockSync.h"
#include "PlaybackPresets.h"

#include "scene/SceneTypes.h"

#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace openrgb2
{

/*---------------------------------------------------------*\
|| PlaybackModePatch — engine-side parameter patch for a    ||
|| backend mode select (mirrors the SDK ModePatch fields;  ||
|| unset fields keep the enumerated values). Colors are    ||
|| packed 0x00BBGGRR.                                      ||
\*---------------------------------------------------------*/
struct PlaybackModePatch
{
    bool                  has_speed     = false;   uint32_t speed     = 0;
    bool                  has_direction = false;   uint32_t direction = 0;
    bool                  has_colors    = false;
    std::vector<uint32_t> colors;
};

/*---------------------------------------------------------*\
|| BackendModeReport — the last server-reported active     ||
|| mode for a controller (the applied-mode acknowledgement ||
|| surface).                                               ||
\*---------------------------------------------------------*/
struct BackendModeReport
{
    bool                  valid     = false;
    std::string           name;
    uint32_t              speed     = 0;
    uint32_t              direction = 0;
    std::vector<uint32_t> colors;      /* 0x00BBGGRR */
};

class PlaybackController
{
public:
    /*-----------------------------------------------------*\
    || All backend/pipeline/scene operations are injected   ||
    || functions. A dep left empty means "not available":   ||
    || playback requests fail honestly rather than guess.   ||
    || Deps may block (socket I/O) — the controller never   ||
    || holds its mutex across a blocking dep call.          ||
    \*-----------------------------------------------------*/
    struct Deps
    {
        /* The applied scene document (capability source). */
        std::function<std::shared_ptr<const studio::SceneDocument>()>
            scene;

        /* Current controller snapshots (all + the wired
           playback targets — production filters bound,
           connected, non-wireless controllers). */
        std::function<std::vector<DeviceSnapshot>()> snapshots;
        std::function<std::vector<DeviceSnapshot>()> wired_targets;

        /* Backend operations. select_mode sends UPDATEMODE
           (mode_name resolved by name backend-side).           */
        std::function<bool(const DeviceKey&,
                           const std::string& mode_name,
                           const PlaybackModePatch&,
                           std::string& err)>            select_mode;
        std::function<bool(const DeviceKey&,
                           BackendModeReport&)>          active_mode;
        std::function<bool(std::string& err)>            refresh_status;
        std::function<bool(const DeviceKey&,
                           std::string& err)>            set_custom_mode;

        /* direct_submit writes a complete-controller frame
           straight to the transport (bypasses the lane —
           used by pause-hold and stop-black while the lane
           is suppressed; the controller itself is the one
           allowed producer). wired_submit goes through the
           normal lane path for other controllers. */
        std::function<SubmitResult(const RgbFrame&)>    direct_submit;
        std::function<SubmitResult(const RgbFrame&)>    wired_submit;

        /* DeliveryPipeline ownership primitives. */
        std::function<void(const DeviceKey&, bool,
                           const std::string&)>          set_suppressed;
        std::function<size_t(const DeviceKey&)>          drop_pending;
        std::function<void(const DeviceKey&)>            reset_delivered;
        std::function<bool(const DeviceKey&,
                           Clock::duration)>             wait_idle;

        /* Optional validated wireless-clock source. Absent =>
           sync honestly Unknown/Stale; wired devices still run
           on the local-timeline fallback. */
        std::function<bool(PlaybackClockSnapshot&)>     clock_sample;

        /* Status may have changed — production emits the IPC
           "playback" event. Called WITHOUT the mutex held. */
        std::function<void()>                           changed;

        /* Clock injection (tests). */
        std::function<TimePoint()>                      now;
    };

    struct Config
    {
        /* Bound for draining in-flight live sends before a
           mode select — must exceed the transport's own send
           timeout (4 s default). */
        Clock::duration drain_timeout       =
            std::chrono::milliseconds(5500);
        /* RefreshStatus cadence while an acknowledgement is
           outstanding, and the overall ack bound. */
        Clock::duration ack_poll_interval   =
            std::chrono::milliseconds(500);
        Clock::duration ack_timeout         =
            std::chrono::seconds(8);
        /* Validated-clock sampling cadence while Active. */
        Clock::duration clock_poll_interval =
            std::chrono::seconds(3);
        /* Baked wired-frame production cadence. */
        Clock::duration wired_frame_interval =
            std::chrono::milliseconds(33);

        PlaybackClockSync::Config clock;
    };

    struct Result
    {
        bool        accepted = false;
        std::string reason;             /* set on rejection      */
        bool        pending  = false;   /* backend ack pending   */
        uint64_t    revision = 0;       /* request revision      */
    };

    explicit PlaybackController(Deps deps, Config cfg = Config{});

    /* EngineService installs the scene provider after both
       objects exist. */
    void SetSceneProvider(
        std::function<std::shared_ptr<const studio::SceneDocument>()>
            fn);

    /* The one request entry point (IPC SET_PLAYBACK_MODE):
       mode + transport control + parameters in a single
       serialized transition. Rejected requests carry a short
       human reason and change nothing. */
    Result Request(PlaybackMode mode, PlaybackControl control,
                   const PlaybackParams& params);

    /* Scene-apply hook (Task 3.6 persistence): re-evaluates
       capabilities against the NEW document and re-applies
       the scene's persisted selection (or Live when it never
       had one / no longer qualifies). */
    void   OnSceneApplied(
        std::shared_ptr<const studio::SceneDocument> doc);

    /* Scene-clock hook (worker frame callback): while an
       acknowledged uploaded loop is Playing, produce the
       matching baked frame for each wired target at the
       configured cadence. Cheap early-out otherwise. */
    void   Tick(TimePoint now);

    /* Maintenance hook (engine owner loop, ~500 ms):
       acknowledgement polling, clock sampling, topology
       re-resolution, suppression re-assert. */
    void   Poll(TimePoint now);

    /*---- Reporting ----------------------------------------*\
    || Status(): the APPLIED PlaybackStatus — selected mode  ||
    || only moves on acknowledgement. RequestedMode/         ||
    || RequestedRevision/AckPending expose the pending side. ||
    \*------------------------------------------------------*/
    PlaybackStatus       Status() const;
    PlaybackMode         RequestedMode() const;
    uint64_t             RequestedRevision() const;
    bool                 AckPending() const;
    PlaybackCapabilities Caps() const;

    /* The last transition/ack/control failure detail (empty
       when none) — surfaced in PLAYBACK for honest error
       reporting. */
    std::string          Detail() const;

    /* Capability classification of the current scene,
       including the human reason animation is unavailable. */
    ScenePlaybackCaps    SceneCaps() const;

    /* True while this controller's lane is suppressed — the
       wireless group belongs to the uploaded loop (or a
       serialized transition). main.cpp's SetCustomMode
       maintenance must skip owned groups. */
    bool                 OwnsGroup(const DeviceKey& key) const;

    /* True while baked playback frames are the intended
       wired producers (animation mode owns the output) —
       the live frame callback then skips wired submissions
       and lets Tick() produce them. */
    bool                 PlaybackOwnsWired() const;

    /* Test introspection. */
    const PlaybackClockSync& ClockSync() const;
    size_t                   PersistedScenes() const;

private:
    /* What a mode request must get the backend to report. */
    struct Expect
    {
        std::string           mode_name;
        uint32_t              speed      = 0;
        bool                  check_direction = false;
        uint32_t              direction  = 0;
        bool                  check_color = false;
        uint32_t              color      = 0;
    };

    /* Per-scene persisted selection (core SceneDocument has
       no playback field — the engine keeps it). */
    struct PerScene
    {
        PlaybackMode   mode = PlaybackMode::LiveEffects;
        PlaybackParams params;
    };

    Result RequestImpl(PlaybackMode mode, PlaybackControl control,
                       const PlaybackParams& params,
                       const std::shared_ptr<const studio::SceneDocument>& doc);
    bool   FindWireless(DeviceSnapshot& out) const;
    bool   BackendReports(const Expect& exp, const DeviceKey& key);
    void   MarkFailedLocked(const std::string& detail);
    void   PersistLocked();
    void   DoControl(PlaybackControl control, std::string& err);
    void   Changed();

    static std::string SceneKey(const studio::SceneDocument& doc);
    static uint32_t    PackColor(RgbColor c);   /* 0x00BBGGRR */
    PlaybackModePatch  BuildPatchLocked() const;
    Expect             BuildExpectLocked(const char* mode_name) const;

    Deps               deps_;
    Config             cfg_;
    PlaybackFrameSampler sampler_;

    mutable std::mutex mutex_;

    /* Applied state (only moves on backend acknowledgement). */
    PlaybackMode       applied_mode_    = PlaybackMode::LiveEffects;
    PlaybackControl    control_         = PlaybackControl::Playing;
    UploadState        upload_          = UploadState::None;
    std::string        status_detail_;

    /* Requested/pending state. */
    PlaybackMode       requested_mode_   = PlaybackMode::LiveEffects;
    PlaybackControl    requested_control_= PlaybackControl::Playing;
    bool               transition_       = false;  /* serialized  */
    uint64_t           epoch_            = 0;      /* request gen */

    /* The owned wireless group + its upload geometry. */
    bool               has_wireless_    = false;
    bool               suppressed_      = false;  /* lane owned */
    DeviceKey          wireless_key_;
    uint64_t           wireless_gen_    = 0;
    uint32_t           wireless_leds_   = 0;
    LianLiWireless::PlaybackPreset preset_ =
        LianLiWireless::PlaybackPreset::SpectrumCycle;
    PlaybackParams     params_;
    PlaybackLoopGeometry geometry_;

    /* Revision acknowledgement. */
    uint64_t           next_rev_          = 0;
    uint64_t           applied_preset_rev_= 0;
    uint64_t           applied_param_rev_ = 0;
    uint64_t           pending_preset_rev_= 0;
    uint64_t           pending_param_rev_ = 0;
    uint64_t           requested_rev_     = 0;
    bool               ack_pending_       = false;
    Expect             expect_;
    TimePoint          ack_deadline_{};
    TimePoint          next_refresh_{};

    /* Validated wireless clock + wired timeline. */
    PlaybackClockSync  clock_;
    TimePoint          last_clock_poll_{};
    TimePoint          active_since_{};
    TimePoint          last_wired_send_{};
    uint64_t           wired_seq_       = 0;

    /* Per-scene persistence. */
    std::map<std::string, PerScene> per_scene_;
    std::string        scene_key_;
};

} /* namespace openrgb2 */
