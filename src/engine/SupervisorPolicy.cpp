/*---------------------------------------------------------*\
|| SupervisorPolicy.cpp — see SupervisorPolicy.h.            ||
||                                                           ||
||   SPDX-License-Identifier: GPL-2.0-or-later               ||
\*---------------------------------------------------------*/

#include "SupervisorPolicy.h"

#include <algorithm>

namespace openrgb2
{

std::vector<std::string> BackendServerArgs(uint16_t port)
{
    return { "--server", "--server-host", "127.0.0.1",
             "--server-port", std::to_string(port),
             "--noautoconnect", "--localconfig" };
}

namespace
{

void EraseWithPrefix(std::set<std::string>& s, const std::string& pre)
{
    for(auto it = s.begin(); it != s.end();)
    {
        if(it->compare(0, pre.size(), pre) == 0)
        {
            it = s.erase(it);
        }
        else
        {
            ++it;
        }
    }
}

} /* anonymous namespace */

SupervisorPolicy::SupervisorPolicy(SupervisorPolicyOptions o)
    : opts_(std::move(o)),
      spawns_(opts_.spawn_budget),
      wedges_(opts_.wedge_budget)
{
}

SupervisorDecision SupervisorPolicy::Notice(const std::string& key,
                                            const std::string& reason,
                                            const std::string& detail)
{
    SupervisorDecision d;
    if(noticed_.insert(key).second)
    {
        d.notice = true;
        d.reason = reason;
        d.detail = detail;
    }
    return d;
}

bool SupervisorPolicy::ProbeDue(TimePoint now)
{
    return !last_probe_at_ ||
           now - *last_probe_at_ >= opts_.probe_interval;
}

SupervisorDecision SupervisorPolicy::Decide(
    const SupervisorObservation& obs,
    const std::function<BackendProbe()>& probe)
{
    const TimePoint now = obs.now;

    /*---- link tracking ------------------------------------*/
    if(obs.link_connected)
    {
        link_down_since_.reset();
        EraseWithPrefix(noticed_, "dead:");
    }
    else if(!link_down_since_)
    {
        link_down_since_ = now;
    }

    /*---- wedge tracking (only while status is live) -------*/
    if(!obs.link_connected || !obs.wireless_valid)
    {
        wedged_since_.clear();
    }
    else
    {
        std::map<std::string, TimePoint> next;
        for(const WirelessGroupObs& g : obs.groups)
        {
            /* "failed" + wedged only: foreign_master is another
               master's group — a restart cannot fix it. */
            if(g.wedged && g.state == "failed")
            {
                const auto it = wedged_since_.find(g.mac);
                next[g.mac] = it != wedged_since_.end() ? it->second
                                                        : now;
            }
        }
        wedged_since_.swap(next);
    }
    if(wedged_since_.empty())
    {
        EraseWithPrefix(noticed_, "wedge:");
    }

    if(obs.stopping)
    {
        return SupervisorDecision{};
    }

    auto get_probe = [&]() -> const BackendProbe&
    {
        if(ProbeDue(now))
        {
            last_probe_    = probe ? probe() : BackendProbe{};
            last_probe_at_ = now;
        }
        return last_probe_;
    };

    /*---- dead backend -------------------------------------*/
    if(!obs.link_connected && link_down_since_ &&
       now - *link_down_since_ >= opts_.dead_after)
    {
        /* Inside the backoff after the previous spawn: wait
           without probing. */
        if(last_spawn_at_ && !spawns_.Exhausted(now) &&
           now - *last_spawn_at_ < spawns_.NextDelay(now))
        {
            return SupervisorDecision{};
        }
        const BackendProbe& p = get_probe();
        if(p.own_pid != 0 || p.port == PortHolder::Own)
        {
            /* Ours is alive (starting, or hung): never a second
               copy — the engine's reconnect owns this case. */
            return SupervisorDecision{};
        }
        if(p.port == PortHolder::Foreign ||
           p.port == PortHolder::Unverifiable)
        {
            return Notice("dead:port", "spawn_refused",
                "backend is down but the managed port is held by a "
                "process that is not the pinned backend -- not "
                "spawning (never replaced, never killed)");
        }
        if(p.other_backend_running)
        {
            return Notice("dead:foreign_exe", "spawn_refused",
                "backend is down but another OpenRGB.exe is running -- "
                "not spawning (the pinned backend's single-instance "
                "prompt could terminate it); close it yourself");
        }
        if(spawns_.Exhausted(now))
        {
            return Notice("dead:budget", "budget_exhausted",
                "backend is down and the restart budget is spent (" +
                std::to_string(opts_.spawn_budget.max_restarts) +
                " per window) -- not restarting again; restart "
                "OpenRGB2 or check state\\crash");
        }
        if(last_spawn_at_ &&
           now - *last_spawn_at_ < spawns_.NextDelay(now))
        {
            return SupervisorDecision{};
        }
        SupervisorDecision d;
        d.action = SupervisorAction::Spawn;
        if(respawn_after_wedge_)
        {
            d.reason = "wireless_wedge";
            d.detail = "restarting the backend after closing it for a "
                       "wedged wireless group";
        }
        else
        {
            d.reason = "backend_dead";
            d.detail = "backend link down and no backend process -- "
                       "restarting it";
        }
        return d;
    }

    /*---- wireless wedge escalation ------------------------*/
    if(obs.link_connected && !wedged_since_.empty())
    {
        auto oldest = std::min_element(wedged_since_.begin(),
                                       wedged_since_.end(),
            [](const auto& a, const auto& b)
            { return a.second < b.second; });
        if(now - oldest->second >= opts_.wedge_after)
        {
            if(wedges_.Exhausted(now))
            {
                return Notice("wedge:budget", "budget_exhausted",
                    "wireless group " + oldest->first + " is still "
                    "wedged; the wedge restart budget (" +
                    std::to_string(opts_.wedge_budget.max_restarts) +
                    " per window) is spent -- not restarting");
            }
            if(spawns_.Exhausted(now))
            {
                return Notice("wedge:spawn_budget", "budget_exhausted",
                    "wireless group " + oldest->first + " is wedged; "
                    "the backend restart budget is spent, so it is "
                    "not closed (it could not be brought back)");
            }
            const BackendProbe& p = get_probe();
            if(p.own_pid == 0)
            {
                return Notice("wedge:unverifiable", "wedge_unverifiable",
                    "wireless group " + oldest->first + " is wedged but "
                    "the backend is not verifiably ours -- left alone");
            }
            SupervisorDecision d;
            d.action = SupervisorAction::CloseWedged;
            d.reason = "wireless_wedge";
            d.group  = oldest->first;
            d.pid    = p.own_pid;
            d.detail = "wireless group " + oldest->first +
                       " wedged (failed) past the backend's own re-arm "
                       "-- closing the backend once to restart it";
            return d;
        }
    }
    return SupervisorDecision{};
}

void SupervisorPolicy::RecordSpawn(TimePoint now, bool ok)
{
    (void)ok;
    spawns_.Record(now);
    last_spawn_at_ = now;
    last_probe_at_.reset();
    last_reason_ = respawn_after_wedge_ ? "wireless_wedge"
                                        : "backend_dead";
    respawn_after_wedge_ = false;
}

void SupervisorPolicy::RecordWedgeClose(TimePoint now)
{
    wedges_.Record(now);
    wedged_since_.clear();
    last_probe_at_.reset();
    last_reason_         = "wireless_wedge";
    respawn_after_wedge_ = true;
}

} /* namespace openrgb2 */
