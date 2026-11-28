// SPDX-License-Identifier: GPL-2.0-or-later
//
// OutputControl tests (src/engine/OutputControl.h — R-1): the
// engine-side runtime output overrides that live outside any
// document — master scale, blackout, the user's hold, and the
// named contributor slots (R-4 schedule/lock/idle) arbitrated
// in ONE place. Qt-free, no threads beyond the concurrency
// check.

#include "OutputControl.h"

#include <atomic>
#include <cmath>
#include <cstdio>
#include <limits>
#include <string>
#include <thread>
#include <vector>

using namespace openrgb2;

static int checks   = 0;
static int failures = 0;
#define CHECK(x) do { ++checks; if(!(x)) { ++failures; \
    std::printf("FAIL line %d: %s\n", __LINE__, #x); } } while(0)

static bool Near(float a, float b, float eps = 1e-6f)
{
    return std::fabs(a - b) < eps;
}

static void TestDefaults()
{
    OutputControl oc;
    const OutputState s = oc.State();
    CHECK(Near(s.master, 1.0f));
    CHECK(!s.blackout);
    CHECK(!s.hold);
    CHECK(Near(s.effective, 1.0f));
    CHECK(s.contributors.empty());
    CHECK(Near(oc.EffectiveScale(), 1.0f));
    CHECK(Near(oc.EffectiveScaleSource().load(), 1.0f));
    const PersistedOutput p = oc.Persisted();
    CHECK(Near(p.master, 1.0f));
    CHECK(!p.blackout);
}

static void TestMasterAndBlackout()
{
    OutputControl oc;
    CHECK(oc.SetMaster(0.5f));
    CHECK(Near(oc.EffectiveScale(), 0.5f));
    CHECK(Near(oc.State().master, 0.5f));
    /* Same value: no change reported. */
    CHECK(!oc.SetMaster(0.5f));

    /* Out of range clamps; NaN/inf are refused and change
       nothing. */
    CHECK(oc.SetMaster(1.7f));
    CHECK(Near(oc.State().master, 1.0f));
    CHECK(oc.SetMaster(-0.2f));
    CHECK(Near(oc.State().master, 0.0f));
    CHECK(oc.SetMaster(0.25f));
    CHECK(!oc.SetMaster(std::numeric_limits<float>::quiet_NaN()));
    CHECK(!oc.SetMaster(std::numeric_limits<float>::infinity()));
    CHECK(Near(oc.State().master, 0.25f));

    /* Blackout forces 0 but keeps master for the release. */
    CHECK(oc.SetBlackout(true));
    CHECK(!oc.SetBlackout(true));
    CHECK(Near(oc.EffectiveScale(), 0.0f));
    CHECK(Near(oc.State().master, 0.25f));
    CHECK(oc.State().blackout);
    CHECK(oc.SetBlackout(false));
    CHECK(Near(oc.EffectiveScale(), 0.25f));
}

static void TestContributorArbitration()
{
    /* effective = blackout ? 0 : master * min(contributors). */
    CHECK(Near(OutputControl::Arbitrate(0.8f, false, {}), 0.8f));
    CHECK(Near(OutputControl::Arbitrate(0.8f, true, {}), 0.0f));
    CHECK(Near(OutputControl::Arbitrate(0.8f, false,
        {{ "schedule", 0.5f }, { "idle", 0.25f }}), 0.2f));

    OutputControl oc;
    oc.SetMaster(0.8f);
    CHECK(oc.SetContributor("schedule", 0.5f));
    CHECK(Near(oc.EffectiveScale(), 0.4f));
    CHECK(oc.SetContributor("idle", 0.25f));
    CHECK(Near(oc.EffectiveScale(), 0.2f));   /* the minimum wins */
    CHECK(!oc.SetContributor("idle", 0.25f)); /* unchanged */
    CHECK(oc.State().contributors.size() == 2);
    CHECK(Near(oc.State().contributors.at("schedule"), 0.5f));
    /* Clamped into [0, 1]; empty names / NaN are refused. */
    CHECK(oc.SetContributor("lock", 3.0f));
    CHECK(Near(oc.State().contributors.at("lock"), 1.0f));
    CHECK(!oc.SetContributor("", 0.5f));
    CHECK(!oc.SetContributor("lock",
                             std::numeric_limits<float>::quiet_NaN()));
    CHECK(oc.ClearContributor("idle"));
    CHECK(!oc.ClearContributor("idle"));
    CHECK(Near(oc.EffectiveScale(), 0.4f));
    /* Contributors are runtime-only: never persisted. */
    const PersistedOutput p = oc.Persisted();
    CHECK(Near(p.master, 0.8f));
    CHECK(!p.blackout);
    /* Blackout beats every contributor. */
    oc.SetBlackout(true);
    CHECK(Near(oc.EffectiveScale(), 0.0f));
}

static void TestHoldIsNotPersisted()
{
    OutputControl oc;
    CHECK(oc.SetHold(true));
    CHECK(!oc.SetHold(true));
    CHECK(oc.State().hold);
    /* Hold never scales. */
    CHECK(Near(oc.EffectiveScale(), 1.0f));

    oc.SetMaster(0.5f);
    oc.SetBlackout(true);
    const PersistedOutput p = oc.Persisted();

    /* A restart: master + blackout come back, hold does not. */
    OutputControl next;
    next.Restore(p);
    CHECK(Near(next.State().master, 0.5f));
    CHECK(next.State().blackout);
    CHECK(!next.State().hold);
    CHECK(Near(next.EffectiveScale(), 0.0f));

    /* A hostile persisted value is clamped on restore. */
    PersistedOutput bad;
    bad.master = 9.0f;
    OutputControl clamp;
    clamp.Restore(bad);
    CHECK(Near(clamp.State().master, 1.0f));
    bad.master = std::numeric_limits<float>::quiet_NaN();
    OutputControl nan;
    nan.Restore(bad);
    CHECK(Near(nan.State().master, 1.0f));
}

static void TestChangedCallback()
{
    OutputControl oc;
    std::vector<OutputState> seen;
    oc.SetChanged([&](const OutputState& s) { seen.push_back(s); });

    oc.SetMaster(0.5f);
    oc.SetMaster(0.5f);          /* unchanged -> no callback */
    oc.SetHold(true);
    oc.SetBlackout(true);
    oc.SetContributor("idle", 0.5f);
    CHECK(seen.size() == 4);
    CHECK(Near(seen[0].effective, 0.5f));
    CHECK(seen[1].hold);
    CHECK(seen[2].blackout && Near(seen[2].effective, 0.0f));
    CHECK(seen[3].contributors.count("idle") == 1);

    /* The callback may read back into the control (it runs
       without the lock held). */
    OutputControl re;
    float read_back = -1.0f;
    re.SetChanged([&](const OutputState&)
                  { read_back = re.EffectiveScale(); });
    re.SetMaster(0.75f);
    CHECK(Near(read_back, 0.75f));

    /* Restore reports the restored state once. */
    OutputControl r;
    int restores = 0;
    r.SetChanged([&](const OutputState&) { ++restores; });
    PersistedOutput p;
    p.master = 0.3f;
    r.Restore(p);
    CHECK(restores == 1);
    r.Restore(p);
    CHECK(restores == 1);
}

static void TestConcurrentWriters()
{
    OutputControl oc;
    std::atomic<int> calls{ 0 };
    oc.SetChanged([&](const OutputState&) { ++calls; });
    std::vector<std::thread> ts;
    for(int t = 0; t < 4; ++t)
    {
        ts.emplace_back([&oc, t]
        {
            for(int i = 0; i < 500; ++i)
            {
                oc.SetMaster(float((i + t) % 100) / 100.0f);
                oc.SetHold((i & 1) != 0);
            }
        });
    }
    for(auto& th : ts)
    {
        th.join();
    }
    /* The atomic always agrees with the locked state. */
    const OutputState s = oc.State();
    CHECK(Near(oc.EffectiveScale(), s.effective));
    CHECK(calls.load() > 0);
}

int main()
{
    TestDefaults();
    TestMasterAndBlackout();
    TestContributorArbitration();
    TestHoldIsNotPersisted();
    TestChangedCallback();
    TestConcurrentWriters();
    std::printf("output control: %d checks, %d failures\n",
                checks, failures);
    return failures ? 1 : 0;
}
