// SPDX-License-Identifier: GPL-2.0-or-later
//
// Owner-loop timing (audit E-2 / P-8). Windows wakes sleeps and
// condition-variable waits on its 15.6 ms default timer tick, so
// the 60 Hz scene clock ran in 15.6/31 ms steps and the owner
// loop's 20 ms sleep rounded to ~31 ms (frame_status and previews
// at ~16 Hz instead of ~30). The engine now holds a 1 ms timer
// resolution between Start() and Stop(), and the owner loop sleeps
// until the next slot of a fixed grid instead of a relative 20 ms.
//
// Covers:
//   - ScopedTimerResolution requests the period once, releases it
//     exactly once, and never releases a request that failed
//   - EngineHost makes the request in Start() and releases it in
//     Stop() (injected API — no global timer change in the test)
//   - EngineHost::NextOwnerWake: first grid slot strictly after
//     now, missed slots dropped, an earlier maintenance pass wins,
//     and every second slot clears the 33 ms publish cap
//
// Measured jitter is reported by the engine benchmarks, not here:
// a pass must not depend on this machine's timer.

#include "EngineHost.h"
#include "WinTimerResolution.h"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#ifdef _WIN32
#include <process.h>
#endif

using namespace openrgb2;

static int checks   = 0;
static int failures = 0;
#define CHECK(x) do { ++checks; if(!(x)) { ++failures; \
    std::printf("FAIL line %d: %s\n", __LINE__, #x); } } while(0)

using ms = std::chrono::milliseconds;

struct FakeTimerApi
{
    std::vector<unsigned> begins;
    std::vector<unsigned> ends;
    unsigned              begin_result = 0;   /* TIMERR_NOERROR */

    platform::ScopedTimerResolution::Api Api()
    {
        platform::ScopedTimerResolution::Api a;
        a.begin_period = [this](unsigned p)
        { begins.push_back(p); return begin_result; };
        a.end_period = [this](unsigned p)
        { ends.push_back(p); return 0u; };
        return a;
    }
};

static void TestScopedRequestRelease()
{
    FakeTimerApi fake;
    {
        platform::ScopedTimerResolution r;
        CHECK(!r.Active());
        CHECK(r.Acquire(1, fake.Api()));
        CHECK(r.Active());
        CHECK(fake.begins == std::vector<unsigned>{ 1u });
        CHECK(fake.ends.empty());
        /* A second Acquire while active never stacks requests. */
        CHECK(r.Acquire(1, fake.Api()));
        CHECK(fake.begins.size() == 1);
        r.Release();
        CHECK(!r.Active());
        CHECK(fake.ends == std::vector<unsigned>{ 1u });
        r.Release();                      /* idempotent */
        CHECK(fake.ends.size() == 1);
        CHECK(r.Acquire(1, fake.Api()));
    }
    /* The destructor releases what is still held. */
    CHECK(fake.begins.size() == 2);
    CHECK(fake.ends.size() == 2);

    /* A refused request is never "released". */
    FakeTimerApi refused;
    refused.begin_result = 97;            /* TIMERR_NOCANDO */
    {
        platform::ScopedTimerResolution r;
        CHECK(!r.Acquire(1, refused.Api()));
        CHECK(!r.Active());
    }
    CHECK(refused.begins.size() == 1);
    CHECK(refused.ends.empty());
}

static void TestHostHoldsResolution()
{
    std::error_code ec;
    const std::filesystem::path dir =
        std::filesystem::temp_directory_path(ec) /
        ("openrgb2-owner-loop-timing-" +
         std::to_string(_getpid()));
    std::filesystem::create_directories(dir, ec);

    FakeTimerApi fake;
    EngineHostOptions opts;
    opts.ipc_enabled     = false;
    opts.sdk_enabled     = false;
    opts.power_monitor   = false;
    opts.reactive_inputs = false;
    opts.scan_owners     = false;
    opts.single_instance = false;
    opts.scene_restore   = false;
    opts.state_path      =
        (dir / "engine-state.json").generic_string();
    opts.workspace_root  = (dir / "workspaces").generic_string();
    opts.timer_api       = fake.Api();
    opts.log             = [](const std::string&) {};
    {
        EngineHost host(std::move(opts));
        CHECK(fake.begins.empty());
        std::string err;
        CHECK(host.Start(err));
        CHECK(fake.begins == std::vector<unsigned>{ 1u });
        CHECK(host.TimerResolutionActive());
        CHECK(fake.ends.empty());
        host.Stop();
        CHECK(fake.ends == std::vector<unsigned>{ 1u });
        CHECK(!host.TimerResolutionActive());
    }
    CHECK(fake.ends.size() == 1);   /* never released twice */
    std::filesystem::remove_all(dir, ec);
}

static void TestNextOwnerWake()
{
    const TimePoint anchor = Clock::now();
    const Clock::duration tick = EngineHost::kOwnerTick;
    const TimePoint far_maint = anchor + std::chrono::hours(1);

    /* First slot strictly after now. */
    CHECK(EngineHost::NextOwnerWake(anchor, anchor, far_maint) ==
          anchor + tick);
    CHECK(EngineHost::NextOwnerWake(anchor, anchor + ms(1),
                                    far_maint) == anchor + tick);
    CHECK(EngineHost::NextOwnerWake(anchor, anchor + tick,
                                    far_maint) == anchor + 2 * tick);

    /* A long iteration (blocking PollOnce) drops the missed
       slots and stays on the grid — no catch-up burst, no
       drift. */
    const TimePoint late = anchor + 5 * tick + ms(3);
    CHECK(EngineHost::NextOwnerWake(anchor, late, far_maint) ==
          anchor + 6 * tick);

    /* An earlier maintenance pass wins; an overdue one wakes at
       once. */
    const TimePoint maint = anchor + ms(5);
    CHECK(EngineHost::NextOwnerWake(anchor, anchor, maint) == maint);
    CHECK(EngineHost::NextOwnerWake(anchor, anchor + ms(9),
                                    anchor) == anchor);

    /* Grid vs. the 33 ms publish cap: two slots must clear it
       with at least 1 ms for wake jitter (a cap missed by
       truncation would drop that publish to every third slot). */
    CHECK(2 * tick >= ms(33) + ms(1));
    CHECK(2 * tick < ms(40));   /* faster than the old 20 ms grid's
                                   two-slot cadence */
}

int main()
{
    TestScopedRequestRelease();
    TestHostHoldsResolution();
    TestNextOwnerWake();
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
