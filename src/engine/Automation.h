/*---------------------------------------------------------*\
||| Automation.h                                              ||
|||                                                           ||
|||   R-2: engine-side automation — system-wide hotkeys     ||
|||   that work with the UI closed. Qt-free; shared with    ||
|||   R-3 (per-app profiles) and R-4 (schedule / lock /     ||
|||   idle dimming), which add sections to the same         ||
|||   AUTOMATION body and the same EngineState member.      ||
|||                                                           ||
|||   Pieces:                                                 ||
|||   - ParseChord / FormatChord: "Ctrl+Alt+PgUp" <->       ||
|||     {Win32 MOD_* bits, virtual-key code}                ||
|||   - ValidateHotkeys: known actions, parsable chords, at ||
|||     least one of Ctrl/Alt/Win, no duplicate chord, at   ||
|||     most kMaxHotkeys bindings                           ||
|||   - AutomationConfig <-> JSON (EngineState.automation;  ||
|||     unknown members ride `extensions`)                  ||
|||   - IHotkeyRegistrar: the platform seam (WinHotkeys in  ||
|||     production; the offline suites inject fakes)        ||
|||   - Automation: the owner of the live config, the       ||
|||     per-binding registration state and the action       ||
|||     dispatcher (workspace cycle + burst debounce,       ||
|||     master brightness steps, blackout and hold toggles  ||
|||     on OutputControl)                                    ||
|||                                                           ||
|||   Nothing is bound by default: the proposed chords are  ||
|||   a UI hint only (ProposedChord), never registered.     ||
|||                                                           ||
|||   Threading: SetHotkeys / Restore / ToJson are safe     ||
|||   from any thread (IPC sessions). PostHotkey is called  ||
|||   on the registrar's thread and only queues. Poll runs  ||
|||   on the owner loop and is the only place an action     ||
|||   runs, so every OutputControl write and every reload   ||
|||   enqueue happens on one thread.                        ||
|||                                                           ||
|||   SPDX-License-Identifier: GPL-2.0-or-later               ||
\*---------------------------------------------------------*/

#pragma once

#include "EngineTypes.h"

#include <nlohmann/json.hpp>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace openrgb2
{

class OutputControl;

/*---------------------------------------------------------*\
||| Chords                                                  ||
\*---------------------------------------------------------*/

/* Modifier bits — the Win32 MOD_* values, so the platform
   registrar passes them through unchanged. */
constexpr uint32_t kModAlt   = 0x0001;
constexpr uint32_t kModCtrl  = 0x0002;
constexpr uint32_t kModShift = 0x0004;
constexpr uint32_t kModWin   = 0x0008;

struct Chord
{
    uint32_t mods = 0;   /* kMod* bits */
    uint32_t vk   = 0;   /* Windows virtual-key code */

    bool operator==(const Chord& o) const
    { return mods == o.mods && vk == o.vk; }
    bool operator!=(const Chord& o) const { return !(*this == o); }
};

/* "Ctrl+Alt+Shift+PgUp" (case-insensitive, '+' separated,
   spaces ignored). Modifier aliases: Ctrl/Control,
   Alt, Shift, Win/Meta/Super. Exactly one non-modifier key.
   Does NOT apply the "needs Ctrl/Alt/Win" policy — that is
   ValidateHotkeys' job, so a bare key still parses. */
bool        ParseChord(const std::string& text, Chord& out,
                       std::string& err);
/* Canonical text: modifiers in Ctrl, Alt, Shift, Win order,
   then the key's canonical name. "" for an unknown vk. */
std::string FormatChord(const Chord& c);

/*---------------------------------------------------------*\
||| Actions + bindings                                      ||
\*---------------------------------------------------------*/

enum class HotkeyAction
{
    NextWorkspace,
    PrevWorkspace,
    BrightnessUp,
    BrightnessDown,
    LightsToggle,
    PauseToggle,
};

const char* HotkeyActionName(HotkeyAction a);
bool        HotkeyActionFromName(const std::string& s,
                                 HotkeyAction& out);
const std::vector<HotkeyAction>& AllHotkeyActions();
/* The plan's suggested default chord — a UI hint; nothing is
   registered until the user binds it. */
std::string ProposedChord(HotkeyAction a);

struct HotkeyBinding
{
    std::string action;   /* HotkeyActionName */
    std::string chord;    /* canonical after ValidateHotkeys */

    bool operator==(const HotkeyBinding& o) const
    { return action == o.action && chord == o.chord; }
};

constexpr size_t kMaxHotkeys = 16;

/* Whole-set validation. On success `out` receives the set
   with every chord in canonical form; on failure `err` names
   the first offending binding and `out` is untouched. */
bool ValidateHotkeys(const std::vector<HotkeyBinding>& in,
                     std::vector<HotkeyBinding>& out,
                     std::string& err);

/*---------------------------------------------------------*\
||| R-4 dimming rules (pure)                                 ||
|||                                                           ||
|||   Three OutputControl contributors — "schedule", "lock",  ||
|||   "idle" — each a scale in [0, 1] (0 = off) while its     ||
|||   condition holds. OutputControl arbitrates min(), so     ||
|||   overlapping rules give the strongest dim. Wall time is  ||
|||   an injected LOCAL time (no clock, no time zone here):   ||
|||   a window is plain HH:MM on the wall clock, so a DST     ||
|||   spring-forward start inside the skipped hour begins at  ||
|||   the next valid minute and a fall-back end is crossed    ||
|||   twice.                                                  ||
\*---------------------------------------------------------*/

/* Local wall time. weekday: 0 = Sunday .. 6 = Saturday
   (struct tm's tm_wday). */
struct LocalTime
{
    int year    = 2026;
    int month   = 1;
    int day     = 1;
    int weekday = 4;
    int hour    = 0;
    int minute  = 0;
    int second  = 0;
};

/* The process's local time (localtime_s) — the production
   wall_now source. */
LocalTime SystemLocalTime();

/* {days, start, end, level}: active from start (inclusive) to
   end (exclusive) on the wall clock. start > end crosses
   midnight; the window then belongs to its START day (Friday
   23:30-07:00 covers Saturday morning). Empty days = every
   day. start == end is refused (validation). */
struct DimWindow
{
    std::vector<int> days;          /* 0..6, Sunday = 0 */
    int              start_min = 0; /* minute of day     */
    int              end_min   = 0;
    float            level     = 0.3f;

    bool operator==(const DimWindow& o) const
    {
        return days == o.days && start_min == o.start_min &&
               end_min == o.end_min && level == o.level;
    }
};

struct LockDim
{
    bool  enabled = false;
    /* Conservative default: dim, not off (a level-0 rule also
       stops an uploaded loop — see the R-4 plan). */
    float level   = 0.3f;

    bool operator==(const LockDim& o) const
    { return enabled == o.enabled && level == o.level; }
};

struct IdleDim
{
    bool     enabled         = false;
    uint32_t after_s         = 600;   /* 30 .. 86400 */
    float    level           = 0.3f;
    /* A fullscreen app (game, presentation) never counts as
       idle — gamepad play does not move GetLastInputInfo. */
    bool     skip_fullscreen = true;

    bool operator==(const IdleDim& o) const
    {
        return enabled == o.enabled && after_s == o.after_s &&
               level == o.level &&
               skip_fullscreen == o.skip_fullscreen;
    }
};

constexpr size_t kMaxDimWindows = 8;

struct DimmingConfig
{
    std::vector<DimWindow> schedule;
    LockDim                lock;
    IdleDim                idle;
    /* Dimming ramps down over ramp_ms; lifting ramps up over
       restore_ms (0 = immediate). Both 0 .. 10000. */
    uint32_t               ramp_ms    = 600;
    uint32_t               restore_ms = 0;

    bool operator==(const DimmingConfig& o) const
    {
        return schedule == o.schedule && lock == o.lock &&
               idle == o.idle && ramp_ms == o.ramp_ms &&
               restore_ms == o.restore_ms;
    }
    bool operator!=(const DimmingConfig& o) const
    { return !(*this == o); }
};

struct DimInputs
{
    LocalTime wall;
    bool      locked     = false;
    double    idle_s     = 0.0;   /* < 0 = unknown -> never idle */
    bool      fullscreen = false;
};

/* One level per active contributor (nullopt = inactive). */
struct DimResult
{
    std::optional<float> schedule;
    std::optional<float> lock;
    std::optional<float> idle;
};

bool      ScheduleWindowActive(const DimWindow& w, const LocalTime& t);
DimResult EvaluateDimRules(const DimmingConfig& c, const DimInputs& in);

/* Wire/profile form: {schedule:[{days, start:"HH:MM",
   end:"HH:MM", level}], lock:{enabled, level}, idle:{enabled,
   after_s, level, skip_fullscreen}, ramp_ms, restore_ms}. */
nlohmann::json DimmingConfigToJson(const DimmingConfig& c);
/* Strict: absent members default, any malformed one fails
   with a reason (`out` untouched). */
bool DimmingConfigFromJson(const nlohmann::json& j,
                           DimmingConfig& out, std::string& err);

/*---------------------------------------------------------*\
||| AutomationConfig — EngineState.automation               ||
\*---------------------------------------------------------*/
struct AutomationConfig
{
    std::vector<HotkeyBinding> hotkeys;          /* empty by default */
    /* Optional curated workspace order for next/prev; empty =
       every managed workspace in list order. */
    std::vector<std::string>   cycle;
    /* Master step per brightness press (0.01 .. 0.5). */
    double                     brightness_step = 0.10;
    /* R-4: absent until set_dimming stored something. */
    std::optional<DimmingConfig> dimming;
    /* Unknown members (R-3 sections a newer build wrote) ride
       here and are written back verbatim. */
    nlohmann::json             extensions = nlohmann::json::object();

    bool operator==(const AutomationConfig& o) const
    {
        return hotkeys == o.hotkeys && cycle == o.cycle &&
               brightness_step == o.brightness_step &&
               dimming == o.dimming &&
               extensions == o.extensions;
    }
    bool operator!=(const AutomationConfig& o) const
    { return !(*this == o); }
};

nlohmann::json AutomationConfigToJson(const AutomationConfig& c);
/* Lenient profile read: a malformed member is dropped with a
   warning (never fails the profile), a binding that no longer
   validates is KEPT verbatim (it reports state "invalid"). */
AutomationConfig AutomationConfigFromJson(
    const nlohmann::json& j, std::vector<std::string>* warnings);

/*---------------------------------------------------------*\
||| Registrar seam                                          ||
\*---------------------------------------------------------*/
enum class HotkeyState
{
    Ok,           /* registered                                 */
    InUse,        /* taken by another app / reserved by Windows */
    Invalid,      /* the chord cannot be registered at all      */
    Unavailable,  /* no registrar on this engine                */
};
const char* HotkeyStateName(HotkeyState s);

struct HotkeyRequest
{
    int      id   = 0;
    uint32_t mods = 0;
    uint32_t vk   = 0;
};

struct HotkeyResult
{
    int         id    = 0;
    HotkeyState state = HotkeyState::Unavailable;
    std::string detail;
};

class IHotkeyRegistrar
{
public:
    virtual ~IHotkeyRegistrar() = default;
    /* Replaces the whole registered set (an empty set
       unregisters everything). One result per request, same
       order. Bounded: returns even if the platform thread is
       wedged (those requests report Unavailable). */
    virtual std::vector<HotkeyResult>
        Apply(const std::vector<HotkeyRequest>& set) = 0;
};

/*---------------------------------------------------------*\
||| Automation                                              ||
\*---------------------------------------------------------*/
class Automation
{
public:
    struct Deps
    {
        OutputControl* output = nullptr;
        /* Null -> every binding reports "unavailable". */
        std::shared_ptr<IHotkeyRegistrar> registrar;
        /* Workspace cycle: the managed ids in list order, the
           active id, and the reload enqueue (internal session
           0 — IPC sessions start at 1). */
        std::function<std::vector<std::string>()> list_workspaces;
        std::function<std::string()>              active_workspace;
        std::function<bool(const std::string& id, std::string& err)>
                                                  enqueue_reload;
        /* Profile write; false + err leaves the state as it
           was. Empty -> nothing is persisted. */
        std::function<bool(const AutomationConfig&, std::string& err)>
                                                  persist;
        /* The AUTOMATION body moved (emit the event). */
        std::function<void()>                     changed;
        std::function<void(const std::string&)>  log;

        /* R-4 dimming sources. Each is consulted only while a
           rule needs it (nothing enabled -> never called):
           wall_now for schedule windows (empty = the process's
           local time), idle_seconds since the last user input
           (< 0 = unknown -> never idle), fullscreen only once
           the idle threshold is reached. */
        std::function<LocalTime()> wall_now;
        std::function<double()>    idle_seconds;
        std::function<bool()>      fullscreen;
    };

    /* Presses inside this window move the target only. */
    static constexpr std::chrono::milliseconds kWorkspaceDebounce{ 1000 };
    /* R-4: the dimming rules are re-evaluated at most this
       often (a lock/unlock or a config change re-evaluates on
       the next Poll). Minute-scale rules; ramps tick every
       Poll. */
    static constexpr std::chrono::milliseconds kDimEvalInterval{ 100 };

    explicit Automation(Deps deps);

    Automation(const Automation&)            = delete;
    Automation& operator=(const Automation&) = delete;

    /* Startup: adopt the persisted config and register its
       bindings (no persist). */
    void Restore(const AutomationConfig& config);

    /* set_hotkeys: validate -> persist -> register. False +
       err (state unchanged) on a validation or save failure;
       a binding the registrar refuses is NOT a failure — it
       reports its state. `cycle` / `step` replace the stored
       value only when given. */
    bool SetHotkeys(const std::vector<HotkeyBinding>& bindings,
                    const std::optional<std::vector<std::string>>& cycle,
                    const std::optional<double>& step,
                    std::string& err);

    AutomationConfig Config() const;

    /* Registrar thread: queue a WM_HOTKEY id. */
    void PostHotkey(int id);

    /* Owner loop: run queued presses, the trailing workspace
       debounce, the R-4 dimming rules, then OutputControl's
       contributor ramps. */
    void Poll(TimePoint now);

    /* R-4 set_dimming: validate (DimmingConfigFromJson did the
       shape) -> persist -> adopt; re-evaluated on the next Poll.
       False + err (state unchanged) on a save failure. */
    bool SetDimming(const DimmingConfig& d, std::string& err);
    /* Session lock/unlock (WinPowerMonitor's thread): records
       the state; the next Poll applies it. */
    void PostSessionLock(bool locked);
    bool SessionLocked() const { return locked_.load(); }

    /* Runs one action now (owner loop / tests). */
    void RunAction(HotkeyAction a, TimePoint now);

    /* The AUTOMATION body: {hotkeys:[{action, chord, state,
       detail?}], actions:[...], proposed:{...}, cycle,
       brightness_step, registrar}. */
    nlohmann::json ToJson() const;

private:
    struct Live
    {
        HotkeyBinding binding;
        int           id = 0;          /* 0 = not registered */
        HotkeyState   state = HotkeyState::Unavailable;
        std::string   detail;
    };

    /* Registers `config_.hotkeys` (apply_mutex_ held). */
    void RegisterLocked();
    /* R-4: evaluate the rules and drive the contributors. */
    void PollDimming(TimePoint now);
    void Notify();
    void Log(const std::string& line) const;
    void StepWorkspace(int dir, TimePoint now);

    Deps deps_;

    /* Serializes SetHotkeys/Restore end to end (persist +
       platform round trip) without holding mutex_ across the
       registrar call. */
    std::mutex        apply_mutex_;

    mutable std::mutex mutex_;
    AutomationConfig  config_;
    std::vector<Live> live_;
    uint32_t          generation_ = 0;
    std::vector<int>  pressed_;     /* PostHotkey queue */

    /* Workspace burst state (owner loop only). */
    TimePoint         burst_until_{};
    bool              in_burst_   = false;
    std::string       cursor_;      /* latest target         */
    std::string       fired_;       /* last reload enqueued  */
    bool              pending_    = false;

    /* R-4 dimming state. locked_/dim_dirty_ are written from
       other threads; the rest is owner-loop only, except
       active_ which ToJson reads under mutex_. */
    std::atomic<bool> locked_    { false };
    std::atomic<bool> dim_dirty_ { true };
    TimePoint         next_dim_eval_{};
    DimResult         active_;      /* guarded by mutex_ */
};

} /* namespace openrgb2 */
