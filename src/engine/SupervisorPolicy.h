/*---------------------------------------------------------*\
|| SupervisorPolicy.h                                        ||
||                                                           ||
||   R-9: the engine's backend supervisor, as a pure        ||
||   decision table (no clock, no processes, no sockets —  ||
||   supervisor_policy_test drives it with a fake clock).  ||
||   EngineHost gathers the observation, executes the       ||
||   decision through the verified-own paths, and reports.  ||
||                                                           ||
||   Dead backend -> Spawn, only when ALL hold:              ||
||     - the SDK link has been down >= dead_after (5 s)     ||
||     - no verified-own backend process exists (one that   ||
||       is starting or hung is never doubled)              ||
||     - nothing holds the managed port                     ||
||     - no OpenRGB.exe that is not verifiably ours runs —  ||
||       the pinned backend's single-instance prompt could  ||
||       TerminateProcess it (start-openrgb2.ps1 refuses    ||
||       for the same reason)                               ||
||     - the spawn budget allows it (5 per hour, backoff    ||
||       0/10/30/60/120 s after the previous spawn)         ||
||                                                           ||
||   Wireless wedge -> CloseWedged, only when a group has   ||
||   reported wedged + state "failed" continuously for      ||
||   wedge_after (8 min = 2 x the backend's rearm_max_ms,  ||
||   so its own re-arm had at least two tries), the backend ||
||   is verified-own, and both the wedge budget (2 per      ||
||   hour) and a spawn slot to bring it back remain.        ||
||   "foreign_master" never escalates: another master owns  ||
||   the group and a restart cannot win against it.        ||
||                                                           ||
||   A stop in progress (engine stopping, or the stop       ||
||   script's state\openrgb2.stopping marker) suppresses   ||
||   everything. Refusals and exhausted budgets come back  ||
||   as one `notice` per episode, never every tick.         ||
||                                                           ||
||   SPDX-License-Identifier: GPL-2.0-or-later               ||
\*---------------------------------------------------------*/

#pragma once

#include "EngineTypes.h"
#include "RestartLedger.h"

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace openrgb2
{

/* The managed SDK port start-openrgb2.ps1 binds ($ManagedPort). */
constexpr uint16_t kManagedBackendPort = 6744;

/* The backend's argument list — EXACTLY start-openrgb2.ps1's
   (parity asserted by supervisor_policy_test). The working
   directory is the private config dir (state\backend). */
std::vector<std::string> BackendServerArgs(uint16_t port);

enum class PortHolder
{
    None,           /* nothing listens                       */
    Own,            /* our verified backend image            */
    Foreign,        /* another image — never ours to replace */
    Unverifiable    /* image unreadable — treated as foreign */
};

/* What the I/O side found when the policy asked. */
struct BackendProbe
{
    uint32_t   own_pid = 0;     /* image == --backend-exe, 0 none */
    PortHolder port    = PortHolder::None;
    /* An OpenRGB.exe whose image is NOT our backend (or is
       unreadable) is running. */
    bool       other_backend_running = false;
};

struct WirelessGroupObs
{
    std::string mac;
    std::string state;          /* "failed", "foreign_master", ... */
    bool        wedged = false;
};

struct SupervisorObservation
{
    TimePoint                     now{};
    bool                          link_connected = false;
    bool                          stopping       = false;
    bool                          wireless_valid = false;  /* have_data */
    std::vector<WirelessGroupObs> groups;
};

enum class SupervisorAction
{
    None,
    Spawn,          /* start the backend (dead-backend rule) */
    CloseWedged     /* gracefully close the verified-own pid */
};

struct SupervisorDecision
{
    SupervisorAction action = SupervisorAction::None;
    std::string      reason;    /* "backend_dead" | "wireless_wedge" |
                                   "spawn_refused" | "budget_exhausted" |
                                   "wedge_unverifiable" */
    std::string      detail;    /* operator-readable */
    std::string      group;     /* wedge: the group's mac */
    uint32_t         pid    = 0;    /* CloseWedged: the pid to close */
    bool             notice = false; /* None + something to report once */
};

struct SupervisorPolicyOptions
{
    Clock::duration dead_after  = std::chrono::seconds(5);
    Clock::duration wedge_after = std::chrono::minutes(8);
    /* The probe (process + listener enumeration) runs at most
       this often while a decision needs it. */
    Clock::duration probe_interval = std::chrono::seconds(1);
    supervise::RestartLedger::Budget spawn_budget{
        5, std::chrono::hours(1),
        { std::chrono::seconds(0),  std::chrono::seconds(10),
          std::chrono::seconds(30), std::chrono::seconds(60),
          std::chrono::seconds(120) } };
    supervise::RestartLedger::Budget wedge_budget{
        2, std::chrono::hours(1), {} };
};

class SupervisorPolicy
{
public:
    SupervisorPolicy() : SupervisorPolicy(SupervisorPolicyOptions{}) {}
    explicit SupervisorPolicy(SupervisorPolicyOptions o);

    /* One owner-loop tick. `probe` is called only when a rule
       needs it (and at most once per probe_interval). */
    SupervisorDecision Decide(const SupervisorObservation& obs,
                              const std::function<BackendProbe()>& probe);

    /* The executor reports what it did. A failed launch still
       spends a slot — a launch that keeps failing must not
       become a hot loop. */
    void RecordSpawn(TimePoint now, bool ok);
    void RecordWedgeClose(TimePoint now);

    size_t      BackendRestarts() const { return spawns_.Total(); }
    size_t      WedgeRestarts()   const { return wedges_.Total(); }
    std::string LastReason()      const { return last_reason_; }
    bool SpawnBudgetExhausted(TimePoint now) const
    { return spawns_.Exhausted(now); }
    bool WedgeBudgetExhausted(TimePoint now) const
    { return wedges_.Exhausted(now); }
    const SupervisorPolicyOptions& Options() const { return opts_; }

private:
    SupervisorDecision Notice(const std::string& key,
                              const std::string& reason,
                              const std::string& detail);
    bool ProbeDue(TimePoint now);

    SupervisorPolicyOptions          opts_;
    supervise::RestartLedger         spawns_;
    supervise::RestartLedger         wedges_;
    std::optional<TimePoint>         link_down_since_;
    std::optional<TimePoint>         last_spawn_at_;
    std::optional<TimePoint>         last_probe_at_;
    BackendProbe                     last_probe_;
    std::map<std::string, TimePoint> wedged_since_;   /* mac -> since */
    std::set<std::string>            noticed_;        /* this episode */
    std::string                      last_reason_;
    bool                             respawn_after_wedge_ = false;
};

} /* namespace openrgb2 */
