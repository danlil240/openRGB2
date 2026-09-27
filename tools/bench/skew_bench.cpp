/*---------------------------------------------------------*\
||| skew_bench.cpp                                            ||
|||                                                           ||
|||   Opt-in Milestone 5 cross-device skew measurement      ||
|||   harness (task 5.1). Measures frame-application skew   ||
|||   across controllers on the real topology by driving    ||
|||   the PRODUCTION delivery path — the same               ||
|||   DeliveryPipeline (per-controller lanes, qualified     ||
|||   budgets, latest-wins) and the same OpenRgbClient      ||
|||   (one mutex-serialized TCP stream) the engine wires    ||
|||   up — through an instrumented transport sink that      ||
|||   records per-send call/return timestamps.               ||
|||                                                           ||
|||   What it reports:                                       ||
|||   - per-controller submit -> transport-return latency    ||
|||     distribution (transport-ADMITTED, never             ||
|||     "displayed")                                         ||
|||   - per-tick inter-controller skew: for frames stamped   ||
|||     with the same logical timestamp (one producer        ||
|||     sequence across all lanes), the spread of send       ||
|||     call/return times across lanes                       ||
|||   - per-lane sequence lag (a paced lane sends an older   ||
|||     tick's frame — the wireless lane's 30 ms budget vs   ||
|||     a 60 Hz producer)                                    ||
|||   - protocol submit->ack RTT: the SDK's only             ||
|||     request/reply verbs (REQUEST_CONTROLLER_COUNT,       ||
|||     REQUEST_CONTROLLER_DATA) timed per controller on a   ||
|||     side connection — UPDATELEDS itself is               ||
|||     fire-and-forget and has NO ack, so this probe is     ||
|||     the honest wire-level "ack" observable               ||
|||   - per-controller DeliveryStats (the same counters      ||
|||     GET_DELIVERY_STATS exposes over the engine IPC)      ||
|||                                                           ||
|||   What it does NOT measure: physical LED timing. That    ||
|||   requires hardware observation (camera/photodiode).     ||
|||                                                           ||
|||   Offline-safe: --list, --dry-run and --rtt-only only    ||
|||   read. All writes are opt-in (no flags prints usage).   ||
|||                                                           ||
|||   SPDX-License-Identifier: GPL-2.0-or-later               ||
|\*---------------------------------------------------------*/

#include "sdk_client.h"
#include "sdk_wire.h"
#include "skew_stats.h"

#include "DeliveryPipeline.h"
#include "DeviceMapper.h"   /* IsWirelessGroupController */
#include "EngineTypes.h"
#include "OpenRgbClient.h"
#include "TcpSocket.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

using namespace openrgb_bench;
using Clock     = openrgb2::Clock;
using TimePoint = openrgb2::TimePoint;

static void SleepMs(int ms)
{
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

static volatile bool g_stop = false;

#ifdef _WIN32
static BOOL WINAPI ConsoleHandler(DWORD type)
{
    (void)type;
    g_stop = true;
    return TRUE;                    /* keep running: restore happens first */
}
#endif

static int64_t UsSince(const TimePoint& origin, const TimePoint& t)
{
    return std::chrono::duration_cast<std::chrono::microseconds>(
        t - origin).count();
}

/*---------------------------------------------------------*\
|| Deterministic workload patterns — same set as the M1     |
|| producer (lianli_bench). A moving pattern is required:   |
|| the pipeline suppresses unchanged frames, which would    |
|| silently remove the very sends this harness measures.    |
\*---------------------------------------------------------*/
enum class Pattern { Gradient, Band, Entropy, Static };

static const char* PatternName(Pattern p)
{
    switch(p)
    {
        case Pattern::Gradient: return "gradient";
        case Pattern::Band:     return "band";
        case Pattern::Entropy:  return "entropy";
        default:                return "static";
    }
}

static uint32_t RgbBytes(uint8_t r, uint8_t g, uint8_t b)
{
    return ((uint32_t)b << 16) | ((uint32_t)g << 8) | r;
}

static uint32_t HueToRgb(uint16_t hue)
{
    uint8_t region = (uint8_t)(hue / 60);
    uint8_t rem    = (uint8_t)((hue % 60) * 255 / 60);
    switch(region)
    {
        case 0:  return RgbBytes(255, rem, 0);
        case 1:  return RgbBytes((uint8_t)(255 - rem), 255, 0);
        case 2:  return RgbBytes(0, 255, rem);
        case 3:  return RgbBytes(0, (uint8_t)(255 - rem), 255);
        case 4:  return RgbBytes(rem, 0, 255);
        default: return RgbBytes(255, 0, (uint8_t)(255 - rem));
    }
}

static uint64_t SplitMix64(uint64_t x)
{
    x += 0x9E3779B97F4A7C15ull;
    x  = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x  = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}

static openrgb2::RgbColor PatternColor(Pattern p, uint64_t frame,
                                       uint32_t led, uint32_t led_count)
{
    uint32_t packed;
    switch(p)
    {
        case Pattern::Gradient:
        {
            const uint32_t span = 180;
            const uint16_t hue = (uint16_t)((led * span / (led_count ? led_count : 1)
                                             + frame * 3) % 360);
            packed = HueToRgb(hue);
            break;
        }
        case Pattern::Band:
        {
            const uint32_t width = led_count / 8 + 1;
            const uint32_t pos   = (uint32_t)(frame % led_count);
            const uint32_t dist  = (led + led_count - pos) % led_count;
            packed = (dist < width) ? RgbBytes(255, 255, 255)
                                    : RgbBytes(8, 8, 8);
            break;
        }
        case Pattern::Entropy:
            packed = (uint32_t)SplitMix64(frame * 1000003ull + led + 1);
            break;
        default:
            packed = RgbBytes(0x20, 0, 0);
            break;
    }
    openrgb2::RgbColor c;
    c.r = (uint8_t)(packed & 0xFF);
    c.g = (uint8_t)((packed >> 8) & 0xFF);
    c.b = (uint8_t)((packed >> 16) & 0xFF);
    return c;
}

/*---------------------------------------------------------*\
|| Instrumented transport sink: sits exactly where the      |
|| engine's DeliveryPipeline hands frames to OpenRgbClient |
|| (pipeline -> TimingSink -> client). The call/return      |
|| timestamps bracket the client's serialized send —        |
|| including the mutex queue wait behind other lanes.       |
\*---------------------------------------------------------*/
class TimingSink final : public openrgb2::IFrameSink
{
public:
    TimingSink(std::shared_ptr<openrgb2::IFrameSink> inner,
               std::map<openrgb2::DeviceKey, uint32_t> lanes)
        : inner_(std::move(inner)), lanes_(std::move(lanes))
    {
    }

    openrgb2::SubmitResult SubmitLatest(const openrgb2::RgbFrame& frame) override
    {
        const TimePoint t0 = Clock::now();
        openrgb2::SubmitResult r = inner_->SubmitLatest(frame);
        const TimePoint t1 = Clock::now();

        SendRecord rec;
        rec.seq     = frame.sequence;
        rec.lane    = 0xFFFFFFFFu;
        const auto it = lanes_.find(frame.controller);
        if(it != lanes_.end())
        {
            rec.lane = it->second;
        }
        rec.call_us = UsSince(origin_, t0);
        rec.done_us = UsSince(origin_, t1);
        rec.dur_us  = rec.done_us - rec.call_us;
        rec.status  = (int)r.status;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            records_.push_back(rec);
        }
        return r;
    }

    openrgb2::DeliveryStats Stats() const override
    {
        return inner_->Stats();
    }

    void SetOrigin(const TimePoint& origin)
    {
        origin_ = origin;
    }

    std::vector<SendRecord> Records() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return records_;
    }

private:
    std::shared_ptr<openrgb2::IFrameSink>    inner_;
    std::map<openrgb2::DeviceKey, uint32_t>  lanes_;
    TimePoint                                origin_;
    mutable std::mutex                       mutex_;
    std::vector<SendRecord>                  records_;
};

/*---------------------------------------------------------*\
|| Per-run bookkeeping                                        |
\*---------------------------------------------------------*/
struct Target
{
    uint32_t              dev_id;      /* bench-client index          */
    uint32_t              lane;        /* skew lane index             */
    SdkController         info;        /* bench-client enumeration    */
    openrgb2::DeviceKey   key;         /* engine-boundary key         */
    openrgb2::DeviceSnapshot snap;     /* pipeline registration basis */
    int32_t               restore_mode;
    std::vector<uint8_t>  restore_raw_mode;
    std::vector<uint32_t> restore_colors;
};

static bool ParseUint32(const char* s, uint32_t& out)
{
    char* end = nullptr;
    long v = strtol(s, &end, 10);
    if(end == s || *end != '\0' || v < 0)
    {
        return false;
    }
    out = (uint32_t)v;
    return true;
}

static std::string ToLower(std::string s)
{
    for(char& c : s)
    {
        c = (char)tolower((unsigned char)c);
    }
    return s;
}

/* Mirrors EngineHost.cpp's BudgetFor(): the wireless group is
   paced at 30 ms (measured ~33.5 admitted/s ceiling), every
   other controller is unpaced. The harness drives the same
   qualification so the measurement covers the production
   pacing, not a hypothetical unpaced one. */
static Clock::duration BudgetFor(const openrgb2::DeviceSnapshot& snap)
{
    if(openrgb2::IsWirelessGroupController(snap))
    {
        return std::chrono::milliseconds(30);
    }
    return Clock::duration::zero();
}

static void Usage()
{
    printf(
    "skew_bench — Milestone 5 cross-device skew measurement (opt-in, writes to real devices)\n"
    "\n"
    "  --host <addr>        SDK host (default 127.0.0.1)\n"
    "  --port <n>           SDK port (default 6744, the private backend)\n"
    "  --list               enumerate controllers and exit (read-only)\n"
    "  --rtt-only           run only the request/reply RTT probe (read-only)\n"
    "  --device <substr>    select controllers by case-insensitive name substring\n"
    "                       (repeatable; e.g. --device \"Lian Li\")\n"
    "  --all                select every controller with LEDs\n"
    "  --pattern <name>     gradient | band | entropy | static (default gradient)\n"
    "  --fps <n>            single producer rate (default 60)\n"
    "  --sweep <list>       comma-separated rates, e.g. 30,60\n"
    "  --duration-s <n>     seconds per rate (default 20)\n"
    "  --settle-s <n>       quiet gap between sweep rates (default 2)\n"
    "  --rtt-rounds <n>     request/reply rounds per probe (default 50)\n"
    "  --out <path>         per-send CSV output\n"
    "  --dry-run            show the plan, send nothing\n"
    "\n"
    "Runs capture device state, enter Direct mode, drive all selected\n"
    "controllers through the production DeliveryPipeline + OpenRgbClient\n"
    "send path with synchronized per-tick frames, then restore state.\n"
    "Ctrl+C stops early and still restores.\n"
    "\n"
    "Note: UPDATELEDS is fire-and-forget — send completion means\n"
    "transport-admitted, never displayed.\n");
}

int main(int argc, char** argv)
{
    std::string host = "127.0.0.1";
    uint16_t    port = 6744;
    bool       list_only  = false;
    bool       dry_run    = false;
    bool       rtt_only   = false;
    bool       select_all = false;
    bool       sweep_set  = false;
    Pattern    pattern    = Pattern::Gradient;
    uint32_t   fps        = 60;
    uint32_t   duration_s = 20;
    uint32_t   settle_s   = 2;
    uint32_t   rtt_rounds = 50;
    std::string out_path;
    std::vector<std::string> device_filters;
    std::vector<uint32_t>    rates;

    for(int i = 1; i < argc; i++)
    {
        const std::string arg = argv[i];
        auto next = [&]() -> const char* {
            if(i + 1 >= argc)
            {
                fprintf(stderr, "missing value for %s\n", arg.c_str());
                exit(2);
            }
            return argv[++i];
        };
        if(arg == "--host")            { host = next(); }
        else if(arg == "--port")       { uint32_t p; if(!ParseUint32(next(), p) || p > 65535) { fprintf(stderr, "bad port\n"); return 2; } port = (uint16_t)p; }
        else if(arg == "--list")       { list_only = true; }
        else if(arg == "--rtt-only")   { rtt_only = true; }
        else if(arg == "--dry-run")    { dry_run = true; }
        else if(arg == "--all")        { select_all = true; }
        else if(arg == "--device")     { device_filters.push_back(next()); }
        else if(arg == "--out")        { out_path = next(); }
        else if(arg == "--fps")        { if(!ParseUint32(next(), fps) || fps == 0) { fprintf(stderr, "bad fps\n"); return 2; } }
        else if(arg == "--duration-s") { if(!ParseUint32(next(), duration_s)) { fprintf(stderr, "bad duration\n"); return 2; } }
        else if(arg == "--settle-s")   { if(!ParseUint32(next(), settle_s)) { fprintf(stderr, "bad settle\n"); return 2; } }
        else if(arg == "--rtt-rounds") { if(!ParseUint32(next(), rtt_rounds)) { fprintf(stderr, "bad rtt-rounds\n"); return 2; } }
        else if(arg == "--pattern")
        {
            const std::string p = next();
            if(p == "gradient")      { pattern = Pattern::Gradient; }
            else if(p == "band")     { pattern = Pattern::Band; }
            else if(p == "entropy")  { pattern = Pattern::Entropy; }
            else if(p == "static")   { pattern = Pattern::Static; }
            else { fprintf(stderr, "unknown pattern '%s'\n", p.c_str()); return 2; }
        }
        else if(arg == "--sweep")
        {
            sweep_set = true;
            std::istringstream list(next());
            std::string item;
            while(std::getline(list, item, ','))
            {
                uint32_t r;
                if(!ParseUint32(item.c_str(), r) || r == 0)
                {
                    fprintf(stderr, "bad sweep rate '%s'\n", item.c_str());
                    return 2;
                }
                rates.push_back(r);
            }
            if(rates.empty())
            {
                fprintf(stderr, "empty sweep list\n");
                return 2;
            }
        }
        else
        {
            fprintf(stderr, "unknown argument '%s'\n\n", arg.c_str());
            Usage();
            return 2;
        }
    }

    if(!sweep_set)
    {
        rates.push_back(fps);
    }

    const bool want_writes = !list_only && !dry_run && !rtt_only;
    if(want_writes && device_filters.empty() && !select_all)
    {
        Usage();
        return 2;
    }

#ifdef _WIN32
    SetConsoleCtrlHandler(ConsoleHandler, TRUE);
#endif

    /*---- Enumeration connection (bench client: dev_id indexed,
           raw-mode capture for restore, RTT probe) -------------*/
    SdkClient bench;
    try
    {
        printf("connecting to %s:%u ...\n", host.c_str(), port);
        bench.Connect(host, port, "skew-bench");
        printf("negotiated SDK protocol %u\n", bench.ProtocolVersion());
    }
    catch(const std::exception& e)
    {
        fprintf(stderr, "connect failed: %s\n", e.what());
        return 1;
    }

    std::vector<SdkController> controllers;
    try
    {
        const uint32_t count = bench.ControllerCount();
        for(uint32_t i = 0; i < count; i++)
        {
            controllers.push_back(bench.ControllerData(i));
        }
    }
    catch(const std::exception& e)
    {
        fprintf(stderr, "enumeration failed: %s\n", e.what());
        return 1;
    }

    printf("\n%zu controllers:\n", controllers.size());
    for(uint32_t i = 0; i < controllers.size(); i++)
    {
        const SdkController& c = controllers[i];
        printf("  [%u] %s | %s | LEDs: %u | serial: %s | location: %s\n",
               i, c.name.c_str(), c.vendor.c_str(), c.num_leds,
               c.serial.c_str(), c.location.c_str());
    }

    if(list_only)
    {
        return 0;
    }

    /*---- Selection -------------------------------------------*/
    std::vector<Target> targets;
    for(uint32_t i = 0; i < controllers.size(); i++)
    {
        const SdkController& c = controllers[i];
        if(c.num_leds == 0)
        {
            continue;
        }
        bool selected = select_all || rtt_only || dry_run;
        if(!selected)
        {
            const std::string haystack = ToLower(
                c.name + " " + c.vendor + " " + c.description + " "
                + c.location);
            for(const std::string& f : device_filters)
            {
                if(haystack.find(ToLower(f)) != std::string::npos)
                {
                    selected = true;
                    break;
                }
            }
        }
        if(!selected)
        {
            continue;
        }
        Target t;
        t.dev_id         = i;
        t.info           = c;
        t.key.device     = c.vendor.empty() ? c.name
                                            : c.vendor + ":" + c.name;
        t.key.serial     = c.serial;
        t.key.location   = c.location;
        t.key.zone.clear();
        t.restore_mode   = (int32_t)c.active_mode;
        if(!c.modes.empty() && !c.modes[c.active_mode % c.modes.size()].raw.empty())
        {
            t.restore_raw_mode = c.modes[c.active_mode % c.modes.size()].raw;
        }
        t.restore_colors = c.colors;
        targets.push_back(std::move(t));
    }

    if(targets.empty())
    {
        fprintf(stderr, "no controllers matched the selection\n");
        return 1;
    }

    /*---- RTT probe (read-only): the SDK's only request/reply   |
           verbs. UPDATELEDS is fire-and-forget — this probe is  |
           the protocol-level submit->ack observable. ----------*/
    auto time_call = [](const std::function<void()>& fn) -> int64_t
    {
        const TimePoint t0 = Clock::now();
        fn();
        return std::chrono::duration_cast<std::chrono::microseconds>(
            Clock::now() - t0).count();
    };

    printf("\nrequest/reply RTT probe (%u rounds each, idle baseline):\n",
           rtt_rounds);
    {
        std::vector<int64_t> samples;
        for(uint32_t r = 0; r < rtt_rounds; r++)
        {
            samples.push_back(
                time_call([&]() { (void)bench.ControllerCount(); }));
        }
        const Distribution d = Summarize(std::move(samples));
        printf("  REQUEST_CONTROLLER_COUNT:            n=%llu min=%lld p50=%lld p95=%lld max=%lld us\n",
               (unsigned long long)d.n, (long long)d.min,
               (long long)d.p50, (long long)d.p95, (long long)d.max);
        for(const Target& t : targets)
        {
            std::vector<int64_t> per;
            for(uint32_t r = 0; r < rtt_rounds; r++)
            {
                const uint32_t id = t.dev_id;
                per.push_back(
                    time_call([&]() { (void)bench.ControllerData(id); }));
            }
            const Distribution pd = Summarize(std::move(per));
            printf("  CONTROLLER_DATA [%2u] %-28.28s n=%llu min=%lld p50=%lld p95=%lld max=%lld us\n",
                   t.dev_id, t.info.name.c_str(),
                   (unsigned long long)pd.n, (long long)pd.min,
                   (long long)pd.p50, (long long)pd.p95,
                   (long long)pd.max);
        }
        printf("  (CONTROLLER_DATA fetches the full device description —\n"
               "   a heavier reply than any write path; UPDATELEDS has no\n"
               "   reply at all)\n");
    }

    if(rtt_only)
    {
        bench.Disconnect();
        return 0;
    }

    /*---- Production send path --------------------------------*\
    | OpenRgbClient (the engine's transport class) + a real    |
    | DeliveryPipeline with the engine's per-controller        |
    | budgets, wrapped in the instrumented sink.               |
    \*---------------------------------------------------------*/
    openrgb2::sdk::OpenRgbClientOptions copts;
    copts.host        = host;
    copts.port        = port;
    copts.client_name = "openrgb2-skew";
    auto sdk = std::make_shared<openrgb2::sdk::OpenRgbClient>(
        copts, std::make_unique<openrgb2::sdk::TcpSocket>());
    if(!sdk->Connect())
    {
        fprintf(stderr, "send-path connect failed: %s\n",
                sdk->LastError().c_str());
        return 1;
    }

    /* Resolve each target's DeviceSnapshot (pipeline registration
       basis + topology generation) from the engine client's own
       enumeration — never the bench client's index. */
    const std::vector<openrgb2::DeviceSnapshot> snaps = sdk->Snapshots();
    for(Target& t : targets)
    {
        bool found = false;
        for(const openrgb2::DeviceSnapshot& s : snaps)
        {
            if(s.key.device   == t.key.device &&
               s.key.serial   == t.key.serial &&
               s.key.location == t.key.location)
            {
                t.snap = s;
                found  = true;
                break;
            }
        }
        if(!found)
        {
            fprintf(stderr, "device '%s' (%s) not in the send-path "
                            "enumeration — skipped\n",
                    t.info.name.c_str(), t.key.location.c_str());
        }
    }

    openrgb2::DeliveryPipeline pipeline;
    std::map<openrgb2::DeviceKey, uint32_t> lane_index;
    uint32_t lane = 0;
    for(Target& t : targets)
    {
        if(t.snap.name.empty() && t.snap.zones.empty())
        {
            continue;               /* unresolved snapshot above */
        }
        t.lane = lane++;
        lane_index[t.snap.key] = t.lane;
        pipeline.RegisterDevice(t.snap);
        pipeline.SetBudget(t.snap.key, BudgetFor(t.snap));
    }

    auto sink = std::make_shared<TimingSink>(sdk, lane_index);
    pipeline.SetTransport(sink);

    printf("\nplan: pattern=%s rates=", PatternName(pattern));
    for(uint32_t r : rates) { printf("%u ", r); }
    printf("| duration=%us/rate settle=%us | lanes:\n",
           duration_s, settle_s);
    for(const Target& t : targets)
    {
        if(t.snap.name.empty() && t.snap.zones.empty())
        {
            continue;
        }
        Clock::duration b{};
        pipeline.Budget(t.snap.key, b);
        const long long budget_ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(b).count();
        printf("  lane %u: [%u] %s (%u LEDs) | budget=%lldms%s\n",
               t.lane, t.dev_id, t.info.name.c_str(), t.info.num_leds,
               budget_ms,
               openrgb2::IsWirelessGroupController(t.snap)
                   ? " (wireless-paced)" : "");
    }

    if(dry_run)
    {
        printf("dry run: no packets sent\n");
        sdk->Disconnect();
        bench.Disconnect();
        return 0;
    }

    /* shared restore: re-applies the captured mode, then colors */
    auto restore_all = [&]() -> bool
    {
        bool ok = true;
        for(const Target& t : targets)
        {
            try
            {
                if(!t.restore_raw_mode.empty())
                {
                    bench.UpdateMode(t.dev_id, t.restore_mode,
                                     t.restore_raw_mode);
                }
                if(!t.restore_colors.empty())
                {
                    bench.UpdateLEDs(t.dev_id, t.restore_colors);
                }
            }
            catch(const std::exception& e)
            {
                ok = false;
                fprintf(stderr, "device %u: RESTORE FAILED: %s\n",
                        t.dev_id, e.what());
            }
        }
        return ok;
    };

    /* capture and enter Direct mode */
    for(Target& t : targets)
    {
        try
        {
            bench.SetCustomMode(t.dev_id);
        }
        catch(const std::exception& e)
        {
            fprintf(stderr, "device %u: cannot enter direct mode: %s\n",
                    t.dev_id, e.what());
            sdk->Disconnect();
            bench.Disconnect();
            return 1;
        }
    }
    printf("entered Direct mode on %zu device(s); saved state for restore\n",
           targets.size());

    if(!pipeline.Start())
    {
        fprintf(stderr, "delivery lanes failed to start\n");
        restore_all();
        sdk->Disconnect();
        bench.Disconnect();
        return 1;
    }

    /*---- Synchronized producer --------------------------------*\
    | Every tick stamps ONE logical timestamp across all lanes:  |
    | each selected controller gets a frame with the same        |
    | sequence and the same sample_time — the scene worker's     |
    | submission shape. Lane workers send when their budget      |
    | allows; the instrumented sink records call/return times.   |
    \*---------------------------------------------------------*/
    const TimePoint run_begin = Clock::now();
    sink->SetOrigin(run_begin);
    std::map<uint64_t, int64_t> tick_us;          /* seq -> tick time */
    std::map<uint64_t, uint32_t> tick_rate;       /* seq -> fps       */
    std::map<uint32_t, uint64_t> submit_ok;       /* lane -> accepted */
    std::map<uint32_t, uint64_t> submit_replaced;
    std::map<uint32_t, uint64_t> submit_rejected;
    uint64_t seq = 0;
    bool aborted = false;

    for(size_t rate_idx = 0; rate_idx < rates.size() && !g_stop; rate_idx++)
    {
        const uint32_t rate = rates[rate_idx];
        if(rate_idx > 0)
        {
            printf("rate %u: settling %us ...\n", rate, settle_s);
            const auto settle_end = Clock::now() + std::chrono::seconds(settle_s);
            while(Clock::now() < settle_end && !g_stop)
            {
                SleepMs(50);
            }
            if(g_stop)
            {
                break;
            }
        }

        printf("rate %u: streaming %s for %us ...\n", rate,
               PatternName(pattern), duration_s);
        const auto deadline  = Clock::now() + std::chrono::seconds(duration_s);
        const auto period    = std::chrono::microseconds(1000000 / rate);
        auto       next_send = Clock::now();

        while(!g_stop && Clock::now() < deadline)
        {
            ++seq;
            const TimePoint t_tick = Clock::now();
            tick_us[seq]   = UsSince(run_begin, t_tick);
            tick_rate[seq] = rate;

            for(Target& t : targets)
            {
                if(t.snap.name.empty() && t.snap.zones.empty())
                {
                    continue;
                }
                openrgb2::RgbFrame frame;
                frame.controller          = t.snap.key;
                frame.topology_generation = t.snap.topology_generation;
                frame.sequence            = seq;
                frame.sample_time         = t_tick;
                frame.colors.reserve(t.info.num_leds);
                for(uint32_t led = 0; led < t.info.num_leds; led++)
                {
                    frame.colors.push_back(
                        PatternColor(pattern, seq, led, t.info.num_leds));
                }
                const openrgb2::SubmitResult r =
                    pipeline.SubmitLatest(frame);
                switch(r.status)
                {
                    case openrgb2::SubmitStatus::Accepted:
                        ++submit_ok[t.lane];            break;
                    case openrgb2::SubmitStatus::ReplacedPending:
                        ++submit_replaced[t.lane];      break;
                    default:
                        ++submit_rejected[t.lane];      break;
                }
            }

            next_send += period;
            const auto now = Clock::now();
            if(next_send > now)
            {
                SleepMs(1);
                while(!g_stop && Clock::now() < next_send)
                {
                    SleepMs(1);
                }
            }
        }
    }
    if(g_stop)
    {
        aborted = true;
    }

    /* let the lanes drain what is still pending, then stop */
    for(const Target& t : targets)
    {
        if(!(t.snap.name.empty() && t.snap.zones.empty()))
        {
            pipeline.WaitIdle(t.snap.key, std::chrono::milliseconds(500));
        }
    }
    pipeline.Stop();

    const std::vector<SendRecord> records = sink->Records();

    /*---- CSV --------------------------------------------------*/
    if(!out_path.empty())
    {
        FILE* csv = fopen(out_path.c_str(), "w");
        if(csv == nullptr)
        {
            fprintf(stderr, "cannot open output csv '%s'\n",
                    out_path.c_str());
        }
        else
        {
            fprintf(csv, "rate_fps,seq,lane,dev_name,leds,tick_us,"
                         "call_us,done_us,dur_us,status\n");
            for(const SendRecord& r : records)
            {
                const Target* t = nullptr;
                for(const Target& cand : targets)
                {
                    if(cand.lane == r.lane) { t = &cand; break; }
                }
                const auto tr = tick_rate.find(r.seq);
                const auto tt = tick_us.find(r.seq);
                fprintf(csv, "%u,%llu,%u,%s,%u,%lld,%lld,%lld,%lld,%d\n",
                        tr != tick_rate.end() ? tr->second : 0,
                        (unsigned long long)r.seq, r.lane,
                        t ? t->info.name.c_str() : "?",
                        t ? t->info.num_leds : 0,
                        tt != tick_us.end() ? (long long)tt->second : -1,
                        (long long)r.call_us, (long long)r.done_us,
                        (long long)r.dur_us, r.status);
            }
            fclose(csv);
            printf("wrote %s\n", out_path.c_str());
        }
    }

    /*---- Analysis --------------------------------------------*\
    | All times run-relative steady_clock microseconds.          |
    | - send duration: done-call (submit -> transport return)    |
    | - tick offset:  call/done minus the tick's logical time    |
    | - skew:         per-seq spread of call/done across lanes   |
    | - seq lag:      newest submitted seq at send time minus    |
    |                 the sent seq (paced lanes run behind)      |
    \*---------------------------------------------------------*/
    printf("\n==== cross-device skew summary (transport-admitted, "
           "never displayed) ====\n");

    /* newest submitted seq at a given run-relative time: seqs are
       monotonic and tick_us is ordered */
    auto latest_seq_at = [&](int64_t at_us) -> uint64_t
    {
        uint64_t s = 0;
        for(const auto& e : tick_us)
        {
            if(e.second <= at_us) { s = e.first; }
            else                  { break; }
        }
        return s;
    };

    for(const Target& t : targets)
    {
        if(t.snap.name.empty() && t.snap.zones.empty())
        {
            continue;
        }
        std::vector<int64_t> durs, call_off, done_off, seq_lag;
        uint64_t n = 0;
        for(const SendRecord& r : records)
        {
            if(r.lane != t.lane)
            {
                continue;
            }
            ++n;
            durs.push_back(r.dur_us);
            const auto tt = tick_us.find(r.seq);
            if(tt != tick_us.end())
            {
                call_off.push_back(r.call_us - tt->second);
                done_off.push_back(r.done_us - tt->second);
            }
            seq_lag.push_back((int64_t)latest_seq_at(r.call_us) -
                              (int64_t)r.seq);
        }
        const Distribution dd  = Summarize(std::move(durs));
        const Distribution dc  = Summarize(std::move(call_off));
        const Distribution ddn = Summarize(std::move(done_off));
        const Distribution dl  = Summarize(std::move(seq_lag));
        const uint64_t submitted = submit_ok[t.lane] +
                                   submit_replaced[t.lane];
        printf("lane %u [%u] %s:\n"
               "  submits accepted=%llu replaced=%llu rejected=%llu | "
               "sends=%llu\n"
               "  send duration   us: n=%llu min=%lld p50=%lld p95=%lld "
               "p99=%lld max=%lld mean=%.0f\n"
               "  call  offset    us: p50=%lld p95=%lld p99=%lld max=%lld\n"
               "  done  offset    us: p50=%lld p95=%lld p99=%lld max=%lld\n"
               "  seq lag     ticks: p50=%lld p95=%lld max=%lld\n",
               t.lane, t.dev_id, t.info.name.c_str(),
               (unsigned long long)submit_ok[t.lane],
               (unsigned long long)submit_replaced[t.lane],
               (unsigned long long)submit_rejected[t.lane],
               (unsigned long long)n,
               (unsigned long long)dd.n, (long long)dd.min,
               (long long)dd.p50, (long long)dd.p95, (long long)dd.p99,
               (long long)dd.max, dd.mean,
               (long long)dc.p50, (long long)dc.p95, (long long)dc.p99,
               (long long)dc.max,
               (long long)ddn.p50, (long long)ddn.p95, (long long)ddn.p99,
               (long long)ddn.max,
               (long long)dl.p50, (long long)dl.p95, (long long)dl.max);
    }

    /* per-tick inter-controller skew */
    {
        const std::vector<TickSkew> skews = PerTickSkew(records);
        std::vector<int64_t> call_spread, done_spread;
        std::vector<int64_t> full_call, full_done;
        const uint32_t lane_count =
            (uint32_t)std::count_if(targets.begin(), targets.end(),
                [](const Target& t) {
                    return !(t.snap.name.empty() && t.snap.zones.empty());
                });
        for(const TickSkew& t : skews)
        {
            if(t.lanes >= 2)
            {
                call_spread.push_back(t.call_spread_us);
                done_spread.push_back(t.done_spread_us);
            }
            if(t.lanes == lane_count)
            {
                full_call.push_back(t.call_spread_us);
                full_done.push_back(t.done_spread_us);
            }
        }
        const Distribution dsc = Summarize(std::move(call_spread));
        const Distribution dsd = Summarize(std::move(done_spread));
        const Distribution dfc = Summarize(std::move(full_call));
        const Distribution dfd = Summarize(std::move(full_done));
        printf("inter-controller skew per tick (n ticks=%zu, "
               "lanes=%u):\n"
               "  all ticks   call spread us: n=%llu p50=%lld p95=%lld "
               "p99=%lld max=%lld\n"
               "  all ticks   done spread us: n=%llu p50=%lld p95=%lld "
               "p99=%lld max=%lld\n"
               "  full-cover  call spread us: n=%llu p50=%lld p95=%lld "
               "p99=%lld max=%lld\n"
               "  full-cover  done spread us: n=%llu p50=%lld p95=%lld "
               "p99=%lld max=%lld\n",
               skews.size(), lane_count,
               (unsigned long long)dsc.n, (long long)dsc.p50,
               (long long)dsc.p95, (long long)dsc.p99, (long long)dsc.max,
               (unsigned long long)dsd.n, (long long)dsd.p50,
               (long long)dsd.p95, (long long)dsd.p99, (long long)dsd.max,
               (unsigned long long)dfc.n, (long long)dfc.p50,
               (long long)dfc.p95, (long long)dfc.p99, (long long)dfc.max,
               (unsigned long long)dfd.n, (long long)dfd.p50,
               (long long)dfd.p95, (long long)dfd.p99, (long long)dfd.max);
    }

    /* delivery stats — the same counters GET_DELIVERY_STATS
       exposes over the engine IPC */
    printf("\nper-lane DeliveryStats (pipeline, TransportMeasured):\n");
    for(const Target& t : targets)
    {
        if(t.snap.name.empty() && t.snap.zones.empty())
        {
            continue;
        }
        openrgb2::DeliveryStats s;
        if(pipeline.Stats(t.snap.key, s))
        {
            printf("  lane %u %s: submitted=%llu completed=%llu "
                   "suppressed=%llu rejected=%llu transport_rejected=%llu "
                   "| interval ms: mean=%.1f min=%.1f max=%.1f\n",
                   t.lane, t.info.name.c_str(),
                   (unsigned long long)s.submitted,
                   (unsigned long long)s.transport_completed,
                   (unsigned long long)s.suppressed,
                   (unsigned long long)s.rejected,
                   (unsigned long long)s.transport_rejected,
                   s.MeanIntervalS() * 1000.0,
                   s.min_interval_s * 1000.0,
                   s.max_interval_s * 1000.0);
        }
    }

    /* restore saved state */
    printf("\nrestoring saved device state ...\n");
    const bool restore_ok = restore_all();
    if(restore_ok)
    {
        printf("restore complete\n");
    }
    else
    {
        fprintf(stderr, "restore incomplete — check devices manually\n");
    }

    printf("note: UPDATELEDS is fire-and-forget — all timing above is\n"
           "      transport-admitted (SDK socket accepted the write),\n"
           "      never displayed. Physical LED skew requires hardware\n"
           "      observation (camera/photodiode); the backend trace\n"
           "      (OPENRGB_LIANLI_TRACE) is the wireless path's\n"
           "      closest device-side observable.\n");

    sdk->Disconnect();
    bench.Disconnect();
    return (aborted || !restore_ok) ? 3 : 0;
}
