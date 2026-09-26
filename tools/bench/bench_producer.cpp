/*---------------------------------------------------------*\
|| bench_producer.cpp                                        ||
||                                                           ||
||   Opt-in Milestone 1 physical benchmark producer for    ||
||   the Lian Li wireless path. Streams Direct-mode color  ||
||   updates over the OpenRGB SDK (loopback TCP) with      ||
||   deterministic workload patterns, a rate sweep, state   ||
||   capture/restore, and a hard stop. Never touches USB,  ||
||   fans, PWM, or sensors — the backend owns hardware.      ||
||                                                           ||
||   Offline-safe: --list and --dry-run only read. All      ||
||   writes are opt-in (run without flags prints usage).    ||
||                                                           ||
||   SPDX-License-Identifier: GPL-2.0-or-later               ||
\*---------------------------------------------------------*/

#include "sdk_client.h"
#include "sdk_wire.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

using namespace openrgb_bench;
using Clock = std::chrono::steady_clock;

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

/*---------------------------------------------------------*\
| Deterministic workload patterns (section 3.2):             |
|   gradient — moving rainbow gradient across all LEDs      |
|   band     — rotating bright band                          |
|   entropy  — high-entropy per-LED colors (compression)    |
|   static   — fixed dim frame (baseline)                    |
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
    /* RGBColor packing: 0x00BBGGRR (see RGBControllerInterface.h) */
    return ((uint32_t)b << 16) | ((uint32_t)g << 8) | r;
}

static uint32_t HueToRgb(uint16_t hue /* 0..359 */)
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

static uint32_t PatternColor(Pattern p, uint64_t frame, uint32_t led,
                             uint32_t led_count)
{
    switch(p)
    {
        case Pattern::Gradient:
        {
            const uint32_t span = 180;                     /* half wheel  */
            const uint16_t hue = (uint16_t)((led * span / (led_count ? led_count : 1)
                                             + frame * 3) % 360);
            return HueToRgb(hue);
        }
        case Pattern::Band:
        {
            const uint32_t width = led_count / 8 + 1;
            const uint32_t pos   = (uint32_t)(frame % led_count);
            const uint32_t dist  = (led + led_count - pos) % led_count;
            if(dist < width)
            {
                return RgbBytes(255, 255, 255);
            }
            return RgbBytes(8, 8, 8);
        }
        case Pattern::Entropy:
            return (uint32_t)SplitMix64(frame * 1000003ull + led + 1);
        default:
            return RgbBytes(0x20, 0, 0);
    }
}

/*---------------------------------------------------------*\
| Per-run bookkeeping                                        |
\*---------------------------------------------------------*/
struct Target
{
    uint32_t              dev_id;
    SdkController         info;
    int32_t               restore_mode;
    std::vector<uint8_t>  restore_raw_mode;
    std::vector<uint32_t> restore_colors;
};

struct FrameRecord
{
    uint32_t dev_id;
    uint32_t rate_fps;
    uint64_t frame;
    uint64_t timestamp_us;
};

static uint64_t Percentile(std::vector<uint64_t>& sorted, double pct)
{
    if(sorted.empty())
    {
        return 0;
    }
    const size_t idx = (size_t)(pct / 100.0 * (double)(sorted.size() - 1));
    return sorted[idx];
}

static void Usage()
{
    printf(
    "lianli_bench — Milestone 1 SDK benchmark producer (opt-in, writes to real devices)\n"
    "\n"
    "  --host <addr>        SDK host (default 127.0.0.1)\n"
    "  --port <n>           SDK port (default 6744, the private backend)\n"
    "  --list               enumerate controllers and exit (read-only)\n"
    "  --device <substr>    select controllers by case-insensitive name substring\n"
    "                       (repeatable; e.g. --device \"Lian Li\")\n"
    "  --all                select every controller with LEDs\n"
    "  --pattern <name>     gradient | band | entropy | static (default gradient)\n"
    "  --fps <n>            single producer rate (default 60)\n"
    "  --sweep <list>       comma-separated rates, e.g. 10,20,30,40,60\n"
    "  --duration-s <n>     seconds per rate (default 30)\n"
    "  --settle-s <n>       quiet gap between sweep rates (default 2)\n"
    "  --out <path>         per-send CSV output\n"
    "  --dry-run            show the plan, send nothing\n"
    "\n"
    "Runs capture device state, enter Direct mode, stream the pattern, then\n"
    "restore the saved mode and colors. Ctrl+C stops early and still restores.\n");
}

static bool ParseUint32(const char* s, uint32_t& out)
{
    char* end = nullptr;
    long v = strtol(s, &end, 10);
    if(end == s || *end != '\0' || v <= 0)
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

int main(int argc, char** argv)
{
    std::string host = "127.0.0.1";
    uint16_t    port = 6744;
    bool       list_only  = false;
    bool       dry_run    = false;
    bool       select_all = false;
    bool       sweep_set  = false;
    Pattern    pattern    = Pattern::Gradient;
    uint32_t   fps        = 60;
    uint32_t   duration_s = 30;
    uint32_t   settle_s   = 2;
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
        if(arg == "--host")           { host = next(); }
        else if(arg == "--port")       { uint32_t p; if(!ParseUint32(next(), p) || p > 65535) { fprintf(stderr, "bad port\n"); return 2; } port = (uint16_t)p; }
        else if(arg == "--list")       { list_only = true; }
        else if(arg == "--dry-run")    { dry_run = true; }
        else if(arg == "--all")        { select_all = true; }
        else if(arg == "--device")     { device_filters.push_back(next()); }
        else if(arg == "--out")        { out_path = next(); }
        else if(arg == "--fps")        { if(!ParseUint32(next(), fps)) { fprintf(stderr, "bad fps\n"); return 2; } }
        else if(arg == "--duration-s") { if(!ParseUint32(next(), duration_s)) { fprintf(stderr, "bad duration\n"); return 2; } }
        else if(arg == "--settle-s")   { if(!ParseUint32(next(), settle_s)) { fprintf(stderr, "bad settle\n"); return 2; } }
        else if(arg == "--pattern")
        {
            const std::string p = next();
            if(p == "gradient")      { pattern = Pattern::Gradient; }
            else if(p == "band")     { pattern = Pattern::Band; }
            else if(p == "entropy") { pattern = Pattern::Entropy; }
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
                if(!ParseUint32(item.c_str(), r))
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

    if(!list_only && !dry_run && device_filters.empty() && !select_all)
    {
        Usage();
        return 2;
    }

#ifdef _WIN32
    SetConsoleCtrlHandler(ConsoleHandler, TRUE);
#endif

    SdkClient client;
    try
    {
        printf("connecting to %s:%u ...\n", host.c_str(), port);
        client.Connect(host, port, "lianli-bench");
        printf("negotiated SDK protocol %u (server may advertise higher)\n",
               client.ProtocolVersion());
    }
    catch(const std::exception& e)
    {
        fprintf(stderr, "connect failed: %s\n", e.what());
        return 1;
    }

    /* enumerate */
    std::vector<SdkController> controllers;
    uint32_t count = 0;
    try
    {
        count = client.ControllerCount();
        for(uint32_t i = 0; i < count; i++)
        {
            controllers.push_back(client.ControllerData(i));
        }
    }
    catch(const std::exception& e)
    {
        fprintf(stderr, "enumeration failed: %s\n", e.what());
        return 1;
    }

    printf("\n%u controllers:\n", count);
    for(uint32_t i = 0; i < controllers.size(); i++)
    {
        const SdkController& c = controllers[i];
        printf("  [%u] %s | %s %s | LEDs: %u | modes:", i, c.name.c_str(),
               c.vendor.c_str(), c.description.c_str(), c.num_leds);
        for(const SdkMode& m : c.modes)
        {
            printf(" %s", m.name.c_str());
        }
        printf("\n       serial: %s | location: %s | active: %s\n",
               c.serial.c_str(), c.location.c_str(),
               c.modes.empty() ? "?" : c.modes[c.active_mode % c.modes.size()].name.c_str());
    }

    if(list_only)
    {
        return 0;
    }

    /* select */
    std::vector<Target> targets;
    for(uint32_t i = 0; i < controllers.size(); i++)
    {
        const SdkController& c = controllers[i];
        if(c.num_leds == 0)
        {
            continue;
        }
        bool selected = select_all;
        if(!selected)
        {
            const std::string lower_name = ToLower(c.name);
            for(const std::string& f : device_filters)
            {
                if(lower_name.find(ToLower(f)) != std::string::npos)
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

    printf("\nplan: pattern=%s rates=", PatternName(pattern));
    for(uint32_t r : rates) { printf("%u ", r); }
    printf("| duration=%us/rate settle=%us | devices:", duration_s, settle_s);
    for(const Target& t : targets)
    {
        printf(" [%u] %s (%u LEDs)", t.dev_id, t.info.name.c_str(), t.info.num_leds);
    }
    printf("\n");

    if(dry_run)
    {
        printf("dry run: no packets sent\n");
        return 0;
    }

    /* capture and enter Direct mode */
    for(Target& t : targets)
    {
        try
        {
            client.SetCustomMode(t.dev_id);
        }
        catch(const std::exception& e)
        {
            fprintf(stderr, "device %u: cannot enter direct mode: %s\n",
                    t.dev_id, e.what());
            return 1;
        }
    }
    printf("entered Direct mode on %zu device(s); saved state for restore\n",
           targets.size());

    /* stream */
    std::vector<FrameRecord> records;
    records.reserve((size_t)duration_s * rates.size() * 64 + 1024);
    const auto run_begin = Clock::now();
    bool aborted = false;

    FILE* csv = nullptr;
    if(!out_path.empty())
    {
        csv = fopen(out_path.c_str(), "w");
        if(csv == nullptr)
        {
            fprintf(stderr, "cannot open output csv '%s'\n", out_path.c_str());
            return 1;
        }
        fprintf(csv, "dev_id,dev_name,leds,pattern,rate_fps,frame,timestamp_us\n");
    }

    for(size_t rate_idx = 0; rate_idx < rates.size() && !g_stop; rate_idx++)
    {
        const uint32_t rate = rates[rate_idx];
        if(rate_idx > 0)
        {
            /* settle gap between rates: hold a static frame */
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
        const auto     deadline = Clock::now() + std::chrono::seconds(duration_s);
        const auto     period   = std::chrono::microseconds(1000000 / rate);
        auto           next_send = Clock::now();
        uint64_t       frame    = 0;
        uint64_t       errors   = 0;

        std::vector<std::vector<uint32_t>> buffers(targets.size());
        for(size_t i = 0; i < targets.size(); i++)
        {
            buffers[i].resize(targets[i].info.num_leds);
        }

        while(!g_stop && Clock::now() < deadline)
        {
            for(size_t i = 0; i < targets.size(); i++)
            {
                const Target& t = targets[i];
                std::vector<uint32_t>& colors = buffers[i];
                for(uint32_t led = 0; led < t.info.num_leds; led++)
                {
                    colors[led] = PatternColor(pattern, frame, led,
                                               t.info.num_leds);
                }
                try
                {
                    client.UpdateLEDs(t.dev_id, colors);
                }
                catch(const std::exception&)
                {
                    errors++;
                }
                const uint64_t us = (uint64_t)std::chrono::duration_cast<
                    std::chrono::microseconds>(Clock::now() - run_begin).count();
                records.push_back(FrameRecord{ t.dev_id, rate, frame, us });
                if(csv != nullptr)
                {
                    fprintf(csv, "%u,%s,%u,%s,%u,%llu,%llu\n", t.dev_id,
                            t.info.name.c_str(), t.info.num_leds,
                            PatternName(pattern), rate,
                            (unsigned long long)frame, (unsigned long long)us);
                }
            }
            frame++;

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

        printf("rate %u: sent %llu frames, %llu send errors\n", rate,
               (unsigned long long)frame, (unsigned long long)errors);
        if(g_stop)
        {
            aborted = true;
            break;
        }
    }

    if(csv != nullptr)
    {
        fclose(csv);
        printf("wrote %s\n", out_path.c_str());
    }

    /* per-device interval summary */
    for(const Target& t : targets)
    {
        std::vector<uint64_t> intervals;
        for(uint32_t rate : rates)
        {
            uint64_t prev = 0;
            bool first = true;
            for(const FrameRecord& r : records)
            {
                if(r.dev_id != t.dev_id || r.rate_fps != rate)
                {
                    continue;
                }
                if(!first)
                {
                    intervals.push_back(r.timestamp_us - prev);
                }
                prev  = r.timestamp_us;
                first = false;
            }
            if(intervals.size() >= 2)
            {
                std::sort(intervals.begin(), intervals.end());
                printf("device [%u] %s @ %u fps: n=%zu p50=%lluus p95=%lluus p99=%lluus\n",
                       t.dev_id, t.info.name.c_str(), rate, intervals.size(),
                       (unsigned long long)Percentile(intervals, 50),
                       (unsigned long long)Percentile(intervals, 95),
                       (unsigned long long)Percentile(intervals, 99));
            }
        }
    }

    /* restore saved state */
    printf("restoring saved device state ...\n");
    bool restore_ok = true;
    for(const Target& t : targets)
    {
        try
        {
            if(!t.restore_raw_mode.empty())
            {
                client.UpdateMode(t.dev_id, t.restore_mode, t.restore_raw_mode);
            }
            if(!t.restore_colors.empty())
            {
                client.UpdateLEDs(t.dev_id, t.restore_colors);
            }
        }
        catch(const std::exception& e)
        {
            restore_ok = false;
            fprintf(stderr, "device %u: RESTORE FAILED: %s\n", t.dev_id, e.what());
        }
    }
    if(restore_ok)
    {
        printf("restore complete\n");
    }
    else
    {
        fprintf(stderr, "restore incomplete — check devices manually\n");
    }

    const ClientStats st = client.Stats();
    printf("async packets drained: %llu (%llu bytes), send errors tracked above\n",
           (unsigned long long)st.packets_received,
           (unsigned long long)st.bytes_received);
    printf("note: SDK send success is not hardware confirmation; use the backend\n"
           "      trace (OPENRGB_LIANLI_TRACE) and physical measurement for that\n");

    client.Disconnect();
    return (aborted || !restore_ok) ? 3 : 0;
}
