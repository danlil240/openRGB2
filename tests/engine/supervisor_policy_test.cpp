// SPDX-License-Identifier: GPL-2.0-or-later
//
// R-9 supervisor policy (pure, no I/O) with a table-driven fake
// clock:
//
//   - RestartLedger: sliding-window budget + backoff schedule
//   - dead backend: link down > 5 s, no own backend, nothing on
//     the port, no foreign OpenRGB.exe -> spawn; the probe is
//     not even consulted before the 5 s
//   - never spawns while a foreign / unverifiable process holds
//     the port or a foreign OpenRGB.exe runs (its single-
//     instance prompt could kill that process)
//   - spawn backoff (0/10/30/60/120 s) and budget (5 per hour)
//   - wireless wedge: "failed"+wedged for 8 min -> close the
//     verified-own backend; "foreign_master" never escalates;
//     an interrupted wedge restarts its timer; 2 per hour
//   - a stop in progress suppresses everything
//   - the spawn arguments match start-openrgb2.ps1 exactly
//     (parsed from the script — parity, like T1's owner rules)

#include "SupervisorPolicy.h"
#include "RestartLedger.h"

#include <chrono>
#include <cstdio>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

using namespace openrgb2;
using namespace std::chrono_literals;

static int checks   = 0;
static int failures = 0;
#define CHECK(x) do { ++checks; if(!(x)) { ++failures; \
    std::printf("FAIL line %d: %s\n", __LINE__, #x); } } while(0)

static const TimePoint T0 = TimePoint(std::chrono::hours(1000));

struct Probe
{
    BackendProbe result;
    int          calls = 0;
    std::function<BackendProbe()> Fn()
    {
        return [this] { ++calls; return result; };
    }
};

static SupervisorObservation Obs(TimePoint now, bool connected)
{
    SupervisorObservation o;
    o.now            = now;
    o.link_connected = connected;
    return o;
}

static SupervisorObservation WedgeObs(TimePoint now, const char* state,
                                      bool wedged)
{
    SupervisorObservation o = Obs(now, true);
    o.wireless_valid = true;
    o.groups.push_back({ "AA:BB:CC:DD:EE:01", state, wedged });
    return o;
}

static void TestLedger()
{
    supervise::RestartLedger::Budget b;
    b.max_restarts = 3;
    b.window       = 10min;
    b.backoff      = { 2s, 10s, 30s };
    supervise::RestartLedger l(b);
    CHECK(!l.Exhausted(T0));
    CHECK(l.NextDelay(T0) == 2s);
    l.Record(T0);
    CHECK(l.NextDelay(T0 + 1s) == 10s);
    l.Record(T0 + 1min);
    CHECK(l.NextDelay(T0 + 2min) == 30s);
    l.Record(T0 + 2min);
    CHECK(l.Exhausted(T0 + 3min));          /* 4th in 10 min: no */
    CHECK(l.NextDelay(T0 + 3min) == 30s);   /* clamped           */
    CHECK(l.CountInWindow(T0 + 3min) == 3);
    /* The first restart ages out of the window. */
    CHECK(!l.Exhausted(T0 + 10min));
    CHECK(l.CountInWindow(T0 + 10min) == 2);
    CHECK(l.Total() == 3);
}

static void TestDeadBackendSpawns()
{
    SupervisorPolicy pol;
    Probe pr;
    /* Connected: nothing, never probes. */
    CHECK(pol.Decide(Obs(T0, true), pr.Fn()).action ==
          SupervisorAction::None);
    /* Link drops at T0+1s; inside 5 s nothing (no probe). */
    CHECK(pol.Decide(Obs(T0 + 1s, false), pr.Fn()).action ==
          SupervisorAction::None);
    CHECK(pol.Decide(Obs(T0 + 5900ms, false), pr.Fn()).action ==
          SupervisorAction::None);
    CHECK(pr.calls == 0);
    /* 5 s down, nothing alive, nothing on the port: spawn. */
    const SupervisorDecision d = pol.Decide(Obs(T0 + 6s, false), pr.Fn());
    CHECK(d.action == SupervisorAction::Spawn);
    CHECK(d.reason == "backend_dead");
    CHECK(pr.calls == 1);
    pol.RecordSpawn(T0 + 6s, true);
    CHECK(pol.BackendRestarts() == 1);
    CHECK(pol.LastReason() == "backend_dead");
    /* The new backend is starting (own pid alive, port not yet
       bound): never a second spawn. */
    pr.result.own_pid = 4242;
    for(int i = 7; i < 60; ++i)
    {
        CHECK(pol.Decide(Obs(T0 + std::chrono::seconds(i), false),
                         pr.Fn()).action == SupervisorAction::None);
    }
    /* The link comes back: the down-timer resets. */
    pol.Decide(Obs(T0 + 61s, true), pr.Fn());
    pr.result.own_pid = 0;
    const int before = pr.calls;
    CHECK(pol.Decide(Obs(T0 + 62s, false), pr.Fn()).action ==
          SupervisorAction::None);
    CHECK(pr.calls == before);   /* inside the new 5 s again */
}

static void TestNeverSpawnsOverForeign()
{
    struct Row
    {
        const char* name;
        BackendProbe probe;
    };
    std::vector<Row> rows;
    {
        BackendProbe p; p.port = PortHolder::Foreign;
        rows.push_back({ "foreign listener", p });
    }
    {
        BackendProbe p; p.port = PortHolder::Unverifiable;
        rows.push_back({ "unverifiable listener", p });
    }
    {
        BackendProbe p; p.other_backend_running = true;
        rows.push_back({ "foreign OpenRGB.exe", p });
    }
    for(const Row& r : rows)
    {
        SupervisorPolicy pol;
        Probe pr;
        pr.result = r.probe;
        pol.Decide(Obs(T0, false), pr.Fn());
        int notices = 0;
        for(int i = 6; i < 600; i += 1)
        {
            const SupervisorDecision d = pol.Decide(
                Obs(T0 + std::chrono::seconds(i), false), pr.Fn());
            if(d.action != SupervisorAction::None)
            {
                std::printf("  row '%s' acted\n", r.name);
            }
            CHECK(d.action == SupervisorAction::None);
            if(d.notice) ++notices;
        }
        /* Reported once per episode, not every tick. */
        CHECK(notices == 1);
        CHECK(pol.BackendRestarts() == 0);
    }
}

static void TestSpawnBackoffAndBudget()
{
    SupervisorPolicy pol;   /* default: 5 per hour, 0/10/30/60/120 s */
    Probe pr;
    TimePoint t = T0;
    pol.Decide(Obs(t, false), pr.Fn());
    t += 5s;
    std::vector<TimePoint> spawns;
    int exhausted_notices = 0;
    for(int i = 0; i < 3600; ++i, t += 1s)
    {
        const SupervisorDecision d = pol.Decide(Obs(t, false), pr.Fn());
        if(d.action == SupervisorAction::Spawn)
        {
            spawns.push_back(t);
            pol.RecordSpawn(t, true);
        }
        if(d.notice) ++exhausted_notices;
    }
    CHECK(spawns.size() == 5);
    if(spawns.size() == 5)
    {
        CHECK(spawns[1] - spawns[0] >= 10s && spawns[1] - spawns[0] < 12s);
        CHECK(spawns[2] - spawns[1] >= 30s && spawns[2] - spawns[1] < 32s);
        CHECK(spawns[3] - spawns[2] >= 60s && spawns[3] - spawns[2] < 62s);
        CHECK(spawns[4] - spawns[3] >= 120s && spawns[4] - spawns[3] < 122s);
    }
    CHECK(exhausted_notices == 1);
    CHECK(pol.SpawnBudgetExhausted(t - 1s));
    /* An hour after the first spawn the window frees a slot. */
    bool again = false;
    for(int i = 0; i < 600 && !again; ++i, t += 1s)
    {
        again = pol.Decide(Obs(t, false), pr.Fn()).action ==
                SupervisorAction::Spawn;
    }
    CHECK(again);
}

static void TestWedgeEscalation()
{
    SupervisorPolicy pol;
    Probe pr;
    pr.result.own_pid = 777;
    /* Wedged + failed: 7:59 nothing, 8:00 close the backend. */
    CHECK(pol.Decide(WedgeObs(T0, "failed", true), pr.Fn()).action ==
          SupervisorAction::None);
    CHECK(pol.Decide(WedgeObs(T0 + 7min + 59s, "failed", true),
                     pr.Fn()).action == SupervisorAction::None);
    CHECK(pr.calls == 0);
    const SupervisorDecision d =
        pol.Decide(WedgeObs(T0 + 8min, "failed", true), pr.Fn());
    CHECK(d.action == SupervisorAction::CloseWedged);
    CHECK(d.reason == "wireless_wedge");
    CHECK(d.pid == 777);
    CHECK(d.group == "AA:BB:CC:DD:EE:01");
    pol.RecordWedgeClose(T0 + 8min);
    CHECK(pol.WedgeRestarts() == 1);
    /* The backend goes away; the dead-backend rule respawns it. */
    pr.result.own_pid = 0;
    pol.Decide(Obs(T0 + 8min + 1s, false), pr.Fn());
    const SupervisorDecision s =
        pol.Decide(Obs(T0 + 8min + 7s, false), pr.Fn());
    CHECK(s.action == SupervisorAction::Spawn);
}

static void TestWedgeTimerResets()
{
    SupervisorPolicy pol;
    Probe pr;
    pr.result.own_pid = 777;
    pol.Decide(WedgeObs(T0, "failed", true), pr.Fn());
    /* The backend's own re-arm recovered it at 5 min. */
    pol.Decide(WedgeObs(T0 + 5min, "ready", false), pr.Fn());
    pol.Decide(WedgeObs(T0 + 6min, "failed", true), pr.Fn());
    CHECK(pol.Decide(WedgeObs(T0 + 13min, "failed", true),
                     pr.Fn()).action == SupervisorAction::None);
    CHECK(pol.Decide(WedgeObs(T0 + 14min, "failed", true),
                     pr.Fn()).action == SupervisorAction::CloseWedged);
    /* Losing the status (link down) also clears the timers. */
    SupervisorPolicy pol2;
    pol2.Decide(WedgeObs(T0, "failed", true), pr.Fn());
    SupervisorObservation blind = Obs(T0 + 4min, true);
    blind.wireless_valid = false;
    pol2.Decide(blind, pr.Fn());
    CHECK(pol2.Decide(WedgeObs(T0 + 9min, "failed", true),
                      pr.Fn()).action == SupervisorAction::None);
}

static void TestForeignMasterNeverEscalates()
{
    SupervisorPolicy pol;
    Probe pr;
    pr.result.own_pid = 777;
    for(int m = 0; m <= 120; ++m)
    {
        CHECK(pol.Decide(WedgeObs(T0 + std::chrono::minutes(m),
                                  "foreign_master", true),
                         pr.Fn()).action == SupervisorAction::None);
    }
    CHECK(pr.calls == 0);
    CHECK(pol.WedgeRestarts() == 0);
}

static void TestWedgeBudgetAndUnverifiable()
{
    SupervisorPolicy pol;
    Probe pr;
    pr.result.own_pid = 777;
    TimePoint t = T0;
    int closes = 0, notices = 0;
    for(int round = 0; round < 3; ++round)
    {
        pol.Decide(WedgeObs(t, "failed", true), pr.Fn());
        for(int s = 0; s <= 9 * 60; s += 10)
        {
            const SupervisorDecision d = pol.Decide(
                WedgeObs(t + std::chrono::seconds(s), "failed", true),
                pr.Fn());
            if(d.action == SupervisorAction::CloseWedged)
            {
                ++closes;
                pol.RecordWedgeClose(t + std::chrono::seconds(s));
                break;
            }
            if(d.notice) ++notices;
        }
        t += 10min;
    }
    CHECK(closes == 2);           /* 2 per hour */
    CHECK(notices == 1);          /* the 3rd is reported once */
    CHECK(pol.WedgeBudgetExhausted(t));

    /* A wedge with no verified-own backend is reported, never
       acted on — a foreign or unreadable backend is not ours
       to close. */
    SupervisorPolicy pol2;
    Probe none;
    pol2.Decide(WedgeObs(T0, "failed", true), none.Fn());
    const SupervisorDecision d =
        pol2.Decide(WedgeObs(T0 + 9min, "failed", true), none.Fn());
    CHECK(d.action == SupervisorAction::None);
    CHECK(d.notice);
}

static void TestStoppingSuppresses()
{
    SupervisorPolicy pol;
    Probe pr;
    pol.Decide(Obs(T0, false), pr.Fn());
    for(int i = 1; i < 120; ++i)
    {
        SupervisorObservation o = Obs(T0 + std::chrono::seconds(i), false);
        o.stopping = true;
        CHECK(pol.Decide(o, pr.Fn()).action == SupervisorAction::None);
    }
    CHECK(pr.calls == 0);
    /* Wedge too. */
    pr.result.own_pid = 777;
    SupervisorPolicy pol2;
    pol2.Decide(WedgeObs(T0, "failed", true), pr.Fn());
    SupervisorObservation o = WedgeObs(T0 + 10min, "failed", true);
    o.stopping = true;
    CHECK(pol2.Decide(o, pr.Fn()).action == SupervisorAction::None);
}

static std::string ReadFile(const std::string& p)
{
    std::ifstream f(p, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

static void TestArgsParityWithStartScript()
{
    const std::vector<std::string> args = BackendServerArgs(6744);
    const std::vector<std::string> want = {
        "--server", "--server-host", "127.0.0.1", "--server-port",
        "6744", "--noautoconnect", "--localconfig" };
    CHECK(args == want);

    const std::string src = ReadFile(
        std::string(OPENRGB2_SOURCE_DIR) +
        "/packaging/windows/start-openrgb2.ps1");
    CHECK(!src.empty());
    std::smatch m;
    /* $ManagedPort = 6744 */
    CHECK(std::regex_search(src, m,
          std::regex(R"(\$ManagedPort\s*=\s*(\d+))")));
    const std::string port = m.size() > 1 ? m[1].str() : "";
    CHECK(port == std::to_string(kManagedBackendPort));
    /* The backend Start-Process block: FilePath = $BackendExe,
       WorkingDirectory = $BackendCfg, ArgumentList = @(...). */
    const size_t at = src.find("FilePath = $BackendExe");
    CHECK(at != std::string::npos);
    const size_t open  = src.find("ArgumentList = @(", at);
    const size_t close = src.find(')', open);
    CHECK(open != std::string::npos && close != std::string::npos);
    const std::string block = src.substr(at, close - at);
    CHECK(block.find("WorkingDirectory = $BackendCfg") != std::string::npos);
    std::vector<std::string> script_args;
    const std::string list = src.substr(open, close - open);
    std::regex q("\"([^\"]*)\"");
    for(std::sregex_iterator it(list.begin(), list.end(), q), end;
        it != end; ++it)
    {
        std::string a = (*it)[1].str();
        if(a == "$ManagedPort") a = port;
        script_args.push_back(a);
    }
    CHECK(script_args == BackendServerArgs((uint16_t)std::stoi(
              port.empty() ? "0" : port)));
    /* $BackendCfg is state\backend under the install. */
    CHECK(std::regex_search(src,
          std::regex(R"(\$BackendCfg\s*=\s*Join-Path\s+\$StateDir\s+"backend")")));
}

int main()
{
    TestLedger();
    TestDeadBackendSpawns();
    TestNeverSpawnsOverForeign();
    TestSpawnBackoffAndBudget();
    TestWedgeEscalation();
    TestWedgeTimerResets();
    TestForeignMasterNeverEscalates();
    TestWedgeBudgetAndUnverifiable();
    TestStoppingSuppresses();
    TestArgsParityWithStartScript();
    std::printf("supervisor_policy_test: %d checks, %d failures\n",
                checks, failures);
    return failures == 0 ? 0 : 1;
}
