// SPDX-License-Identifier: GPL-2.0-or-later
//
// Ordered bounded-shutdown tests (Task 3.7 —
// src/engine/OrderlyShutdown.h). Deterministic: recorded step
// order + an injected clock — no threads, no sleeps.
//
// Covers:
//   - steps run in registration order (process teardown
//     ordering: intake -> production -> lanes -> transport)
//   - every step reports elapsed vs its declared bound;
//     an overrun is flagged, never silently dropped
//   - an overrun never strands later steps (teardown is not
//     skippable)
//   - Run() is single-shot — shutdown is not restartable
//   - TotalBound is the sum of step bounds
//   - audit E-10: a step that throws is recorded and never
//     strands later steps; the opt-in watchdog fires once the
//     whole sequence outlives TotalBound() + grace, naming the
//     step still running, and stays silent on a run in bounds

#include "OrderlyShutdown.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace openrgb2;

static int checks   = 0;
static int failures = 0;
#define CHECK(x) do { ++checks; if(!(x)) { ++failures; \
    std::printf("FAIL line %d: %s\n", __LINE__, #x); } } while(0)

/*---------------------------------------------------------*\
|| T1: registration order is execution order                ||
\*---------------------------------------------------------*/

static void TestOrder()
{
    OrderlyShutdown sd;
    std::vector<std::string> ran;

    /* The production order: stop event/command intake, then
       frame production, then delivery lanes, then the
       transport — later steps never feed earlier ones. */
    sd.Add("power-monitor",   std::chrono::milliseconds(2000),
           [&ran] { ran.push_back("power-monitor"); });
    sd.Add("ipc-intake",      std::chrono::milliseconds(2000),
           [&ran] { ran.push_back("ipc-intake"); });
    sd.Add("scene-worker",    std::chrono::milliseconds(2000),
           [&ran] { ran.push_back("scene-worker"); });
    sd.Add("delivery",        std::chrono::milliseconds(12000),
           [&ran] { ran.push_back("delivery"); });
    sd.Add("sdk-transport",   std::chrono::milliseconds(1000),
           [&ran] { ran.push_back("sdk-transport"); });

    CHECK(sd.StepCount() == 5);
    CHECK(sd.TotalBound() ==
          std::chrono::milliseconds(2000 + 2000 + 2000 +
                                  12000 + 1000));
    CHECK(!sd.Ran());

    const OrderlyShutdown::Result r = sd.Run();
    CHECK(sd.Ran());
    CHECK(r.steps.size() == 5);
    CHECK(r.all_within_bound);
    CHECK(ran.size() == 5);
    CHECK(ran[0] == "power-monitor");
    CHECK(ran[1] == "ipc-intake");
    CHECK(ran[2] == "scene-worker");
    CHECK(ran[3] == "delivery");
    CHECK(ran[4] == "sdk-transport");
    for(const OrderlyShutdown::StepResult& s : r.steps)
    {
        CHECK(s.ran);
        CHECK(s.within);
    }
}

/*---------------------------------------------------------*\
|| T2: an overrun is flagged AND later steps still run      ||
\*---------------------------------------------------------*/

static void TestOverrun()
{
    OrderlyShutdown sd;
    std::vector<std::string> ran;
    TimePoint fake_now{};

    sd.Add("fast",   std::chrono::milliseconds(100),
           [&] { ran.push_back("fast"); });
    sd.Add("slow",   std::chrono::milliseconds(100),
           [&]
           {
               ran.push_back("slow");
               fake_now += std::chrono::milliseconds(250);
           });
    sd.Add("after",  std::chrono::milliseconds(100),
           [&] { ran.push_back("after"); });

    const OrderlyShutdown::Result r =
        sd.Run([&] { return fake_now; });

    CHECK(ran.size() == 3);          /* nothing stranded   */
    CHECK(ran[2] == "after");
    CHECK(r.steps.size() == 3);
    CHECK(r.steps[0].within);
    CHECK(!r.steps[1].within);       /* overrun reported   */
    CHECK(r.steps[1].elapsed ==
          std::chrono::milliseconds(250));
    CHECK(r.steps[1].bound ==
          std::chrono::milliseconds(100));
    CHECK(r.steps[2].within);
    CHECK(!r.all_within_bound);
}

/*---------------------------------------------------------*\
|| T3: Run() is single-shot                                 ||
\*---------------------------------------------------------*/

static void TestSingleShot()
{
    OrderlyShutdown sd;
    int calls = 0;
    sd.Add("once", std::chrono::milliseconds(100),
           [&] { ++calls; });

    const OrderlyShutdown::Result r1 = sd.Run();
    const OrderlyShutdown::Result r2 = sd.Run();
    CHECK(calls == 1);               /* not restartable    */
    CHECK(r2.steps.size() == r1.steps.size());
    CHECK(r2.steps[0].name == "once");
}

/*---------------------------------------------------------*\
|| T4: engine teardown mirrors the declared order           ||
\*---------------------------------------------------------*/

static void TestEngineShape()
{
    /* The same step list main.cpp registers — order is the
       contract (plan section 4: owned components shut down in
       order; intake first, transport last). */
    OrderlyShutdown sd;
    std::vector<std::string> ran;
    const char* order[] = {
        "power-monitor", "ipc-listeners", "ipc-server",
        "scene-worker", "delivery-pipeline", "sdk-transport" };
    for(const char* name : order)
    {
        sd.Add(name, std::chrono::milliseconds(2000),
               [&ran, name] { ran.push_back(name); });
    }
    sd.Run();
    for(size_t i = 0; i < 6; ++i)
    {
        CHECK(ran[i] == order[i]);
    }
}

/*---------------------------------------------------------*\
|| T5: a throwing step is recorded, later steps still run   ||
\*---------------------------------------------------------*/

static void TestThrowingStep()
{
    OrderlyShutdown sd;
    std::vector<std::string> ran;
    sd.Add("ipc-listeners", std::chrono::milliseconds(2000),
           [&ran] { ran.push_back("ipc-listeners"); });
    sd.Add("workspace-jobs", std::chrono::milliseconds(2000),
           [] { throw std::runtime_error("job join failed"); });
    sd.Add("calibration", std::chrono::milliseconds(2000),
           [] { throw 42; });
    sd.Add("sdk-transport", std::chrono::milliseconds(1000),
           [&ran] { ran.push_back("sdk-transport"); });

    bool escaped = false;
    OrderlyShutdown::Result r;
    try
    {
        r = sd.Run();
    }
    catch(...)
    {
        escaped = true;
    }
    CHECK(!escaped);
    CHECK((ran == std::vector<std::string>{ "ipc-listeners",
                                            "sdk-transport" }));
    CHECK(r.steps.size() == 4);
    if(r.steps.size() == 4)
    {
        CHECK(!r.steps[0].failed);
        CHECK(r.steps[1].failed);
        CHECK(r.steps[1].error == "job join failed");
        CHECK(r.steps[2].failed);
        CHECK(!r.steps[2].error.empty());
        CHECK(!r.steps[3].failed);
        CHECK(r.steps[3].ran);
    }
    CHECK(!r.all_ok);
}

/*---------------------------------------------------------*\
|| T6: the watchdog fires past TotalBound + grace            ||
\*---------------------------------------------------------*/

static void TestWatchdog()
{
    /* A hung step: 300 ms against a 20 ms bound. The watchdog
       (20 ms total + 30 ms grace) must name it while it is
       still running — production exits the process there. */
    {
        OrderlyShutdown sd;
        std::mutex m;
        std::vector<std::string> fired;
        std::atomic<bool> hung_done{ false };
        sd.Add("quick", std::chrono::milliseconds(0), [] {});
        sd.Add("hung", std::chrono::milliseconds(20),
               [&hung_done]
               {
                   std::this_thread::sleep_for(
                       std::chrono::milliseconds(300));
                   hung_done.store(true);
               });
        std::atomic<bool> fired_before_done{ false };
        sd.SetWatchdog(std::chrono::milliseconds(30),
                       [&](const std::string& step)
                       {
                           fired_before_done.store(!hung_done.load());
                           std::lock_guard<std::mutex> g(m);
                           fired.push_back(step);
                       });
        const OrderlyShutdown::Result r = sd.Run();
        CHECK(fired.size() == 1);
        CHECK(!fired.empty() && fired.front() == "hung");
        CHECK(fired_before_done.load());
        CHECK(r.watchdog_fired);
        CHECK(!r.all_within_bound);
    }
    /* A sequence inside its bounds never trips it. */
    {
        OrderlyShutdown sd;
        int fired = 0;
        sd.Add("a", std::chrono::milliseconds(200), [] {});
        sd.Add("b", std::chrono::milliseconds(200), [] {});
        sd.SetWatchdog(std::chrono::milliseconds(50),
                       [&fired](const std::string&) { ++fired; });
        const OrderlyShutdown::Result r = sd.Run();
        CHECK(fired == 0);
        CHECK(!r.watchdog_fired);
        CHECK(r.all_ok);
    }
}

int main()
{
    TestOrder();
    TestOverrun();
    TestSingleShot();
    TestEngineShape();
    TestThrowingStep();
    TestWatchdog();

    std::printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
