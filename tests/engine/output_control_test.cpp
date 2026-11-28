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

/* R-4: ramped contributor targets. Dimming ramps (the scale is
   monotonic over the ticks), lifting is immediate unless a
   restore ramp is given; the target shows at once, the
   effective scale follows the ticks. */
static void TestContributorRamps()
{
    using ms = std::chrono::milliseconds;
    OutputControl oc;
    int notes = 0;
    oc.SetChanged([&notes](const OutputState&) { ++notes; });
    const TimePoint t0 = TimePoint{} + std::chrono::hours(1);

    CHECK(oc.SetContributorTarget("idle", 0.3f, t0, ms(600), ms(0)));
    CHECK(notes == 1);
    OutputState s = oc.State();
    CHECK(s.ramping);
    CHECK(Near(s.contributors.at("idle"), 0.3f));   /* the target */
    CHECK(Near(oc.EffectiveScale(), 1.0f));         /* not yet    */
    CHECK(OutputControl::AnyContributorOff(s) == false);

    oc.Tick(t0 + ms(300));
    CHECK(Near(oc.EffectiveScale(), 0.65f, 1e-3f));
    CHECK(notes == 1);                  /* ticks never notify */

    float prev = oc.EffectiveScale();
    bool  mono = true;
    for(int i = 19; i <= 40; ++i)
    {
        oc.Tick(t0 + ms(i * 17));
        const float e = oc.EffectiveScale();
        mono = mono && e <= prev + 1e-6f;
        prev = e;
    }
    CHECK(mono);
    CHECK(Near(oc.EffectiveScale(), 0.3f));
    CHECK(!oc.State().ramping);
    CHECK(notes == 2);                  /* completion notifies */

    /* The same target again changes nothing, restarts nothing. */
    CHECK(!oc.SetContributorTarget("idle", 0.3f, t0 + ms(700),
                                   ms(600), ms(0)));
    CHECK(!oc.Tick(t0 + ms(720)));

    /* Restore is immediate (restore ramp 0). */
    CHECK(oc.ReleaseContributor("idle", t0 + ms(800), ms(0)));
    CHECK(Near(oc.EffectiveScale(), 1.0f));
    CHECK(oc.State().contributors.empty());
    CHECK(!oc.ReleaseContributor("idle", t0 + ms(810), ms(0)));

    /* A brighter target (0.3 -> 0.8) is a lift: immediate. */
    CHECK(oc.SetContributorTarget("schedule", 0.3f, t0, ms(0), ms(0)));
    CHECK(Near(oc.EffectiveScale(), 0.3f));
    CHECK(oc.SetContributorTarget("schedule", 0.8f, t0, ms(600), ms(0)));
    CHECK(Near(oc.EffectiveScale(), 0.8f));

    /* A deeper target mid-ramp starts from where the ramp is. */
    CHECK(oc.SetContributorTarget("schedule", 0.4f, t0, ms(400), ms(0)));
    oc.Tick(t0 + ms(200));
    CHECK(Near(oc.EffectiveScale(), 0.6f, 1e-3f));
    CHECK(oc.SetContributorTarget("schedule", 0.0f, t0 + ms(200),
                                  ms(400), ms(0)));
    CHECK(Near(oc.EffectiveScale(), 0.6f, 1e-3f));
    CHECK(OutputControl::AnyContributorOff(oc.State()));
    oc.Tick(t0 + ms(400));
    CHECK(Near(oc.EffectiveScale(), 0.3f, 1e-3f));
    oc.Tick(t0 + ms(700));
    CHECK(Near(oc.EffectiveScale(), 0.0f));

    /* A configured restore ramp lifts gradually, then the slot
       disappears. */
    CHECK(oc.ReleaseContributor("schedule", t0 + ms(1000), ms(500)));
    CHECK(oc.State().contributors.empty());       /* not a dim */
    CHECK(oc.State().ramping);
    prev = oc.EffectiveScale();
    mono = true;
    for(int i = 1; i <= 35; ++i)
    {
        oc.Tick(t0 + ms(1000 + i * 17));
        const float e = oc.EffectiveScale();
        mono = mono && e >= prev - 1e-6f;
        prev = e;
    }
    CHECK(mono);
    CHECK(Near(oc.EffectiveScale(), 1.0f));
    CHECK(!oc.State().ramping);

    /* Master multiplies, blackout overrides a ramp, the minimum
       contributor wins. */
    oc.SetMaster(0.5f);
    CHECK(oc.SetContributorTarget("lock", 0.5f, t0, ms(0), ms(0)));
    CHECK(oc.SetContributorTarget("idle", 0.2f, t0, ms(0), ms(0)));
    CHECK(Near(oc.EffectiveScale(), 0.1f));
    oc.SetBlackout(true);
    CHECK(Near(oc.EffectiveScale(), 0.0f));
    /* An immediate SetContributor cancels a ramp in flight. */
    oc.SetBlackout(false);
    CHECK(oc.SetContributorTarget("idle", 0.0f, t0, ms(600), ms(0)));
    CHECK(oc.SetContributor("idle", 0.6f));
    CHECK(!oc.State().ramping);
    CHECK(Near(oc.EffectiveScale(), 0.25f));
    /* Refusals. */
    CHECK(!oc.SetContributorTarget("", 0.5f, t0, ms(0), ms(0)));
    CHECK(!oc.SetContributorTarget("x",
        std::numeric_limits<float>::infinity(), t0, ms(0), ms(0)));
}

int main()
{
    TestDefaults();
    TestMasterAndBlackout();
    TestContributorArbitration();
    TestHoldIsNotPersisted();
    TestChangedCallback();
    TestConcurrentWriters();
    TestContributorRamps();
    std::printf("output control: %d checks, %d failures\n",
                checks, failures);
    return failures ? 1 : 0;
}
