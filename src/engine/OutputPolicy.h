/*---------------------------------------------------------*\
||| OutputPolicy.h                                            ||
|||                                                           ||
|||   Audit E-12b: what the LEDs show when the engine stops ||
|||   and while it has nothing to drive. Qt-free.           ||
|||                                                           ||
|||   - hold:    leave the last frame on the LEDs (today's  ||
|||              behaviour, the default)                    ||
|||   - black:   all lights off                             ||
|||   - restore: each device goes back to the hardware mode ||
|||              it was in before OpenRGB2 switched it to   ||
|||              Direct; a key with no usable recorded mode ||
|||              falls back to black (and says so)          ||
|||                                                           ||
|||   The model (global on_exit/on_idle + an ordered        ||
|||   per-controller override table) is persisted in        ||
|||   EngineState.output_policy. RunOutputPolicy is the ONE ||
|||   per-key executor the exit step and the idle rule      ||
|||   share; every hardware operation is an injected dep so ||
|||   the decision table is unit-tested without a backend.  ||
|||                                                           ||
|||   SPDX-License-Identifier: GPL-2.0-or-later               ||
\*---------------------------------------------------------*/

#pragma once

#include "EngineTypes.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace openrgb2
{

enum class OutputAction
{
    Hold,
    Black,
    Restore,
};

const char* OutputActionName(OutputAction a);
/* "hold" | "black" | "restore" (exact, lower case). */
bool        ParseOutputAction(const std::string& s, OutputAction& out);

/* Controller selector — the same {device?, location_prefix?,
   serial?} shape as E-12a. Every GIVEN field must match:
   device is the exact DeviceKey.device, location_prefix a
   prefix of DeviceKey.location, serial the exact serial. An
   empty match selects nothing (validation rejects it). */
struct OutputPolicyMatch
{
    std::string device;
    std::string location_prefix;
    std::string serial;

    bool Empty() const;
    bool Matches(const DeviceKey& key) const;
};

struct OutputPolicyRule
{
    OutputPolicyMatch           match;
    std::optional<OutputAction> on_exit;
    std::optional<OutputAction> on_idle;
};

struct OutputPolicy
{
    OutputAction                  on_exit = OutputAction::Hold;
    OutputAction                  on_idle = OutputAction::Hold;
    /* Ordered: the first rule that matches AND sets the field
       wins for that field; otherwise the global value. */
    std::vector<OutputPolicyRule> per_controller;

    OutputAction ExitFor(const DeviceKey& key) const;
    OutputAction IdleFor(const DeviceKey& key) const;
    bool         IsDefault() const;
};

bool operator==(const OutputPolicyMatch& a, const OutputPolicyMatch& b);
bool operator==(const OutputPolicyRule& a, const OutputPolicyRule& b);
bool operator==(const OutputPolicy& a, const OutputPolicy& b);
inline bool operator!=(const OutputPolicy& a, const OutputPolicy& b)
{ return !(a == b); }

/* Bound on the override table (a hand-edited profile or a
   set_policy body cannot grow it without limit). */
constexpr size_t kMaxOutputPolicyRules = 64;

/* {on_exit, on_idle, per_controller:[{match:{...}, on_exit?,
   on_idle?}]} — absent optional fields are omitted. */
nlohmann::json OutputPolicyToJson(const OutputPolicy& p);

/* Strict parse of the shape above. Absent on_exit/on_idle keep
   the defaults (hold); unknown members are ignored. Returns
   false + a "<path>: message" error on a wrong type, an
   unknown action, an empty match or too many rules — `out` is
   untouched then. This is the validator the set_policy verb
   and the profile load share. */
bool OutputPolicyFromJson(const nlohmann::json& j, OutputPolicy& out,
                          std::string& err);

/*---------------------------------------------------------*\
||| Prior hardware mode                                     ||
\*---------------------------------------------------------*/

/* What a controller reported as its active mode BEFORE the
   custom-mode upkeep's first SetCustomMode this run. Kept in
   memory only: a crash or restart loses it, and restore then
   falls back to black. */
struct PriorMode
{
    std::string           name;
    uint32_t              speed     = 0;
    uint32_t              direction = 0;
    std::vector<uint32_t> colors;      /* mode-specific colors */
};

/* "Direct" / "Custom" (case-insensitive): the per-LED modes
   SetCustomMode selects. A prior mode with one of these names
   was left by an earlier engine run, not a hardware effect —
   restoring it would only re-freeze the last frame, so restore
   falls back to black for it. */
bool IsDirectModeName(const std::string& name);

/*---------------------------------------------------------*\
||| Per-key executor                                        ||
\*---------------------------------------------------------*/

enum class OutputPhase
{
    Exit,
    Idle,
};

struct OutputPolicyOutcome
{
    DeviceKey    key;
    OutputAction policy  = OutputAction::Hold;
    /* "held" | "black" | "restored" | "black_fallback"
       (restore with no usable prior mode) | "loop_stopped"
       (uploaded loop replaced by black) | "loop_kept"
       (restore/hold on a loop-owned group: the loop IS the
       hardware mode) | "skipped_power_gate" |
       "skipped_calibration" | "failed" */
    std::string  outcome;
    std::string  detail;               /* reason / mode name   */
};

struct OutputPolicyDeps
{
    /* The power gate (display off / suspend): no write may
       cross that boundary — every key is skipped. */
    std::function<bool()>                      output_paused;
    std::function<bool(const DeviceKey&)>      calibration_owns;
    std::function<bool(const DeviceKey&)>      playback_owns;
    /* The existing owner write for a loop-owned group:
       playback stop-black. Called at most once per run. */
    std::function<bool(std::string& err)>      playback_stop_black;
    /* Queue one all-zero complete-controller frame on the
       live lane's reliable slot (does not wait). */
    std::function<bool(const DeviceSnapshot&,
                       std::string& err)>      submit_black;
    /* Bounded wait until the lane has nothing pending or in
       flight. */
    std::function<bool(const DeviceKey&,
                       Clock::duration)>       wait_idle;
    /* UPDATEMODE back to the recorded hardware mode. */
    std::function<bool(const DeviceKey&, const PriorMode&,
                       std::string& err)>      select_mode;
    /* Drop a not-yet-sent lane frame (a hold key must not
       inherit the black stop-black queued for wired peers). */
    std::function<void(const DeviceKey&)>      drop_pending;
    /* Bound for each lane's WaitIdle. */
    Clock::duration lane_idle_timeout = std::chrono::milliseconds(500);
};

/* Runs the policy over `targets` (connected controllers):
   1. the power gate skips everything;
   2. calibration-owned keys are skipped;
   3. a loop-owned group under black gets playback stop-black
      (once); under hold/restore the loop keeps playing;
   4. black keys queue their reliable black frame first, then
      every lane is waited on (the lanes deliver in parallel);
   5. restore keys wait for their lane to go idle, then
      UPDATEMODE back to the recorded mode — no usable record
      (absent, or Direct/Custom left by an earlier run) falls
      back to black; the wireless group always falls back to
      black (re-selecting a hardware mode there is not
      verified on hardware).
   One outcome per target, in `targets` order. */
std::vector<OutputPolicyOutcome> RunOutputPolicy(
    const std::vector<DeviceSnapshot>&  targets,
    OutputPhase                         phase,
    const OutputPolicy&                 policy,
    const std::map<DeviceKey, PriorMode>& priors,
    const OutputPolicyDeps&             deps);

nlohmann::json OutputPolicyOutcomesToJson(
    const std::vector<OutputPolicyOutcome>& outcomes);

} /* namespace openrgb2 */
