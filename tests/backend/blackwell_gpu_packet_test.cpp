/*---------------------------------------------------------*\
| blackwell_gpu_packet_test.cpp                             |
|                                                           |
|   Offline packet-level regression test for the Gigabyte   |
|   RGB Fusion 2 Blackwell GPU controller                   |
|   (AORUS MASTER 5080 layout). Captures the raw 64-byte    |
|   I2C block writes and asserts a multi-color fan palette  |
|   never leaks into the single-LED logo zones — the banner |
|   flicker seen under per-LED effects — plus the direct-   |
|   mode smoothing filter (time-based, settles without      |
|   further frames), unchanged-zone suppression, and exact  |
|   color encoding. No hardware, no Qt.                     |
\*---------------------------------------------------------*/

#include "GigabyteRGBFusion2BlackwellGPUController.h"
#include "RGBController_GigabyteRGBFusion2BlackwellGPU.h"
#include "i2c_smbus.h"
#include "LogManager.h"

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <chrono>
#include <mutex>
#include <thread>
#include <vector>

/*---------------------------------------------------------*\
| LogManager stub — the controller sources only touch       |
| LogManager::get()/LogEntry via the LOG_* macros.          |
\*---------------------------------------------------------*/
LogManager::LogManager()  {}
LogManager::~LogManager() {}

LogManager* LogManager::get()
{
    static LogManager instance;
    return &instance;
}

void LogManager::LogEntry(const char*, int, unsigned int, const char*, ...) {}
void LogManager::LogEntry_va(const char*, int, unsigned int, const char*, va_list) {}
void LogManager::LogEntry_message(PLogMessage) {}

static int failures = 0;
static int checks   = 0;

#define CHECK(cond, msg) do { checks++; if(!(cond)) { failures++; \
    printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, msg); } } while(0)

/*---------------------------------------------------------*\
| CapturingBus — records every I2C block write. Writes can  |
| come from the device call thread, so access is locked.    |
\*---------------------------------------------------------*/
class CapturingBus : public i2c_smbus_interface
{
public:
    std::vector<std::vector<u8>> writes;
    std::mutex                   lock;

    s32 i2c_smbus_xfer(u8, char, u8, int, i2c_smbus_data*) override
    {
        return -1;
    }

    s32 i2c_xfer(u8, char, int* size, u8* data) override
    {
        if(data != nullptr && size != nullptr)
        {
            std::lock_guard<std::mutex> guard(lock);
            writes.emplace_back(data, data + *size);
        }
        return 0;
    }

    size_t Count()
    {
        std::lock_guard<std::mutex> guard(lock);
        return writes.size();
    }

    void Clear()
    {
        std::lock_guard<std::mutex> guard(lock);
        writes.clear();
    }

    /* Latest packet written to a hardware zone, or nullptr. */
    const u8* Latest(uint8_t zone)
    {
        std::lock_guard<std::mutex> guard(lock);
        for(size_t i = writes.size(); i-- > 0;)
        {
            if(writes[i].size() == 64 && writes[i][9] == zone)
            {
                return writes[i].data();
            }
        }
        return nullptr;
    }
};

/*---------------------------------------------------------*\
| Fake clock — starts far from zero so nothing relies on a  |
| zero epoch. Every Frame() advances one 45 ms engine tick. |
\*---------------------------------------------------------*/
static std::atomic<long long> fake_ms{50000000};

static std::chrono::steady_clock::time_point FakeNow()
{
    return std::chrono::steady_clock::time_point(std::chrono::milliseconds(fake_ms.load()));
}

static void Frame(RGBController_RGBFusion2BlackwellGPU& dev, long long dt_ms = 45)
{
    fake_ms += dt_ms;
    dev.DeviceUpdateLEDs();
}

static void Converge(RGBController_RGBFusion2BlackwellGPU& dev)
{
    for(int f = 0; f < 80; f++)
    {
        Frame(dev);
    }
}

static void Settle(RGBController_RGBFusion2BlackwellGPU& dev)
{
    int guard = 0;
    while(dev.SettleStep() && guard++ < 1000)
    {
        fake_ms += 45;
    }
}

/* A mode update writes every zone regardless of the cache. */
static void FullFrame(RGBController_RGBFusion2BlackwellGPU& dev, CapturingBus& bus)
{
    bus.Clear();
    fake_ms += 45;
    dev.DeviceUpdateMode();
}

static int FindMode(RGBController& dev, const char* name)
{
    for(unsigned int i = 0; i < dev.GetModeCount(); i++)
    {
        if(dev.GetModeName(i) == name)
        {
            return (int)i;
        }
    }
    return -1;
}

/* SetActiveMode queues an UpdateMode on the controller's device
   thread. Wait for its packets to land, then clear the capture. */
static void DrainModeWrites(CapturingBus& bus)
{
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds(5);
    while(bus.Count() < 6 &&
          std::chrono::steady_clock::now() < deadline)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    bus.Clear();
}

static void CheckPacket(CapturingBus& bus, size_t idx, uint8_t zone,
                        uint8_t expected_sz, RGBColor single,
                        bool expect_single,
                        uint8_t expected_type = RGB_FUSION2_BLACKWELL_GPU_REG_MODE,
                        uint8_t expected_mode = RGB_FUSION2_BLACKWELL_GPU_MODE_STATIC)
{
    char msg[128];

    std::lock_guard<std::mutex> guard(bus.lock);
    CHECK(idx < bus.writes.size(), "packet exists");
    if(idx >= bus.writes.size())
    {
        return;
    }
    const std::vector<u8>& p = bus.writes[idx];

    CHECK(p.size() == 64, "packet is 64 bytes");
    CHECK(p[0] == expected_type, "packet uses the expected command register");
    CHECK(p[2] == expected_mode, "packet uses the expected hardware mode");
    snprintf(msg, sizeof(msg), "zone %u packet targets zone field %u", zone, zone);
    CHECK(p[9] == zone, msg);
    snprintf(msg, sizeof(msg),
             "zone %u carries a valid speed (%u), never 0",
             zone, RGB_FUSION2_BLACKWELL_GPU_SPEED_NORMAL);
    CHECK(p[3] == RGB_FUSION2_BLACKWELL_GPU_SPEED_NORMAL, msg);
    snprintf(msg, sizeof(msg),
             "zone %u SZ field matches its own palette size %u (got %u)",
             zone, expected_sz, p[10]);
    CHECK(p[10] == expected_sz, msg);

    if(expect_single)
    {
        /* Static single-color zones carry their color in bytes 5-7.
           Only a nonempty palette should carry it at offset 11 too. */
        snprintf(msg, sizeof(msg), "zone %u bytes 5-7 carry its own color (got %u,%u,%u)",
                 zone, p[5], p[6], p[7]);
        CHECK(p[5] == RGBGetRValue(single) &&
              p[6] == RGBGetGValue(single) &&
              p[7] == RGBGetBValue(single), msg);
        if(expected_sz > 0)
        {
            snprintf(msg, sizeof(msg), "zone %u palette[0] carries its own color", zone);
            CHECK(p[11] == RGBGetRValue(single) &&
                  p[12] == RGBGetGValue(single) &&
                  p[13] == RGBGetBValue(single), msg);
        }
    }
    else
    {
        /* Multi-color zone: bytes 5-7 must mirror palette[0] — a
           stale cached base color here bleeds onto the device. */
        snprintf(msg, sizeof(msg), "zone %u bytes 5-7 mirror palette[0]", zone);
        CHECK(p[5]  == p[11] &&
              p[6]  == p[12] &&
              p[7]  == p[13], msg);
    }
}

int main()
{
    CapturingBus bus;

    RGBFusion2BlackwellGPUController* ctrl =
        new RGBFusion2BlackwellGPUController(
            &bus, 0x75, "Gigabyte AORUS GeForce RTX 5080 MASTER ICE",
            RGB_FUSION2_BLACKWELL_GPU_AORUS_MASTER_5080_LAYOUT);
    RGBController_RGBFusion2BlackwellGPU dev(
        ctrl, RGB_FUSION2_BLACKWELL_GPU_AORUS_MASTER_5080_LAYOUT);

    /* Deterministic time; the test drives SettleStep itself. */
    dev.SetClockForTest(FakeNow);

    CHECK(dev.GetZoneCount() == 5, "5080 MASTER layout exposes 5 UI zones");
    CHECK(dev.GetLEDCount() == 26, "5080 MASTER layout exposes 26 LEDs");

    /*-----------------------------------------------------*\
    | Scenario 1: per-LED streaming (Direct mode) with      |
    | non-uniform fan rings — the Aurora condition.         |
    \*-----------------------------------------------------*/
    int direct_idx = FindMode(dev, "Direct");
    CHECK(direct_idx >= 0, "Direct mode present");
    dev.SetActiveMode(direct_idx);
    DrainModeWrites(bus);

    for(unsigned int i = 0; i < 24; i++)
    {
        dev.SetColor(i, ToRGBColor((unsigned char)(10 + i),
                                   (unsigned char)(40 + 2 * i),
                                   (unsigned char)(70 + 3 * i)));
    }
    const RGBColor side_logo = ToRGBColor(0x44, 0x66, 0x88);
    const RGBColor top_logo  = ToRGBColor(0xAA, 0xBB, 0xCC);
    dev.SetColor(24, side_logo);
    dev.SetColor(25, top_logo);

    /* Converge the direct-mode smoothing filter before
       asserting wire values (see scenario 4). */
    Converge(dev);
    FullFrame(dev, bus);

    CHECK(bus.Count() == 6, "one packet per hardware zone (0-5)");

    /* Fan zones keep their 8-color palettes ... */
    for(uint8_t z = 0; z < 3 && z < bus.Count(); z++)
    {
        CheckPacket(bus, z, z, 8, 0, false);
        for(uint8_t slot = 0; slot < 8; ++slot)
        {
            const unsigned int led = z * 8 + slot;
            const unsigned int offset = 11 + slot * 3;
            CHECK(bus.writes[z][offset] == 10 + led &&
                  bus.writes[z][offset + 1] == 40 + 2 * led &&
                  bus.writes[z][offset + 2] == 70 + 3 * led,
                  "Static streaming preserves each distinct fan palette entry in order");
        }
    }
    /* Single-LED zones use the Static base-color field without a
       fan palette. HW zone 5 mirrors UI zone 4 (Top Logo). */
    if(bus.Count() >= 6)
    {
        CheckPacket(bus, 3, 3, 0, side_logo, true);
        CheckPacket(bus, 4, 4, 0, top_logo,  true);
        CheckPacket(bus, 5, 5, 0, top_logo,  true);
    }

    /*-----------------------------------------------------*\
    | Scenario 1b (finding 12): unchanged-zone suppression. |
    | A settled, unchanged frame writes nothing; changing   |
    | one logo writes only that zone; identical packets are |
    | re-sent after the refresh period.                     |
    \*-----------------------------------------------------*/
    bus.Clear();
    Frame(dev);
    CHECK(bus.Count() == 0, "settled unchanged frame writes no packets");
    CHECK(!dev.SettleStep(), "settled filter reports settled");
    CHECK(bus.Count() == 0, "settled SettleStep writes no packets");

    dev.SetColor(24, ToRGBColor(0x45, 0x66, 0x88));
    bus.Clear();
    Frame(dev, 1000);
    {
        char msg[128];
        snprintf(msg, sizeof(msg), "one-logo change writes only zone 3 (got %zu packets)", bus.Count());
        CHECK(bus.Count() == 1 && bus.Latest(3) != nullptr, msg);
    }
    Settle(dev);
    bus.Clear();
    Frame(dev, 2100);
    CHECK(bus.Count() == 6, "identical packets are refreshed after 2 s");
    bus.Clear();
    Frame(dev);
    CHECK(bus.Count() == 0, "refresh does not repeat on the next frame");

    /*-----------------------------------------------------*\
    | Scenario 2: uniform frame — every zone single color.  |
    \*-----------------------------------------------------*/
    const RGBColor flat = ToRGBColor(0x40, 0x80, 0xC0);
    dev.SetAllColors(flat);
    Converge(dev);
    FullFrame(dev, bus);

    CHECK(bus.Count() == 6, "uniform frame still writes 6 packets");
    for(uint8_t z = 0; z < bus.Count() && z < 6; z++)
    {
        CheckPacket(bus, z, z, 0, flat, true);
    }

    /*-----------------------------------------------------*\
    | Scenario 3: mode-specific palette (Color Shift) must  |
    | still carry every mode color on every zone.           |
    \*-----------------------------------------------------*/
    int shift_idx = FindMode(dev, "Color Shift");
    CHECK(shift_idx >= 0, "Color Shift mode present on 5080 layout");
    if(shift_idx >= 0)
    {
        dev.SetActiveMode(shift_idx);
        DrainModeWrites(bus);
        Frame(dev);

        CHECK(bus.Count() == 6, "mode-specific frame writes 6 packets");
        for(uint8_t z = 0; z < bus.Count() && z < 6; z++)
        {
            char msg[128];
            snprintf(msg, sizeof(msg),
                     "zone %u carries the full mode palette", z);
            CHECK(bus.writes[z][10] ==
                  RGB_FUSION_2_BLACKWELL_AORUS_MASTER_FAN_LEDS, msg);
            CHECK(bus.writes[z][0] == RGB_FUSION2_BLACKWELL_GPU_REG_MODE,
                  "Color Shift retains its mode command");
            CHECK(bus.writes[z][2] == RGB_FUSION2_BLACKWELL_GPU_MODE_COLOR_SHIFT,
                  "Color Shift is not remapped to Static");
        }
    }

    /*-----------------------------------------------------*\
    | Scenario 4: temporal smoothing — a jump to a new      |
    | color must not land in one step; the wire value       |
    | converges exponentially instead. Logical Direct mode |
    | retains smoothing with the Static transport. Once   |
    | converged, the wire is exact.                       |
    \*-----------------------------------------------------*/
    dev.SetActiveMode(direct_idx);
    DrainModeWrites(bus);

    const RGBColor base = ToRGBColor(90, 90, 90);
    dev.SetAllColors(base);
    Converge(dev);
    FullFrame(dev, bus);
    CHECK(bus.Count() == 6, "baseline frame writes every zone");
    CheckPacket(bus, 3, 3, 0, base, true);
    bus.Clear();

    dev.SetColor(24, ToRGBColor(150, 90, 90));
    dev.SetColor(25, ToRGBColor(150, 90, 90));
    Frame(dev);
    CHECK(bus.Count() == 3, "first step writes only the changed zones (3, 4, 5)");
    {
        const u8* p3 = bus.Latest(3);
        CHECK(p3 != nullptr, "zone 3 written");
        if(p3 != nullptr)
        {
            char msg[128];
            /* Single-LED zone alpha is 0.15 per 45 ms: 90 + 60*0.15 = 99 */
            snprintf(msg, sizeof(msg), "zone 3 step 1 smoothed toward target, got %u", p3[5]);
            CHECK(p3[5] > 95 && p3[5] < 103, msg);
            CHECK(p3[5] != 150, "first step does not land on target");
        }
    }
    bus.Clear();

    Frame(dev);
    {
        const u8* p3 = bus.Latest(3);
        CHECK(p3 != nullptr, "zone 3 written on step 2");
        if(p3 != nullptr)
        {
            char msg[128];
            /* 99 + 51*0.15 ~= 107 */
            snprintf(msg, sizeof(msg), "zone 3 step 2 converges further, got %u", p3[5]);
            CHECK(p3[5] > 103 && p3[5] < 112, msg);
        }
    }
    bus.Clear();

    /*-----------------------------------------------------*\
    | Micro-jitter around a settled value is attenuated to  |
    | a sub-visible step on the wire.                       |
    \*-----------------------------------------------------*/
    Converge(dev);
    bus.Clear();
    dev.SetColor(24, ToRGBColor(152, 92, 92));
    Frame(dev);
    {
        /* Either the rounded wire value did not move at all (the
           packet is suppressed as unchanged) or it moved by one. */
        const u8* p3 = bus.Latest(3);
        if(p3 != nullptr)
        {
            char msg[128];
            snprintf(msg, sizeof(msg), "zone 3 jitter attenuated, got %u", p3[5]);
            CHECK(p3[5] >= 150 && p3[5] <= 151, msg);
        }
        else
        {
            CHECK(true, "zone 3 jitter attenuated below one wire step");
        }
    }
    bus.Clear();

    /*-----------------------------------------------------*\
    | Scenario 6 (finding 1): ONE target change, then no    |
    | more frames (the engine suppresses identical frames). |
    | The settle step alone must carry the wire to the      |
    | exact target and then stop. Logo black -> white used  |
    | to freeze at ~38.                                     |
    \*-----------------------------------------------------*/
    dev.SetAllColors(ToRGBColor(0, 0, 0));
    Converge(dev);
    Settle(dev);
    bus.Clear();

    dev.SetColor(24, ToRGBColor(255, 255, 255));
    Frame(dev);
    {
        const u8* p3 = bus.Latest(3);
        CHECK(p3 != nullptr && p3[5] > 0 && p3[5] < 255,
              "single frame leaves the logo at an intermediate value");
    }

    int settle_steps = 0;
    while(dev.SettleStep() && settle_steps < 1000)
    {
        fake_ms += 45;
        settle_steps++;
    }
    {
        char msg[128];
        snprintf(msg, sizeof(msg), "settle converges in bounded steps (%d)", settle_steps);
        CHECK(settle_steps > 0 && settle_steps < 200, msg);

        const u8* p3 = bus.Latest(3);
        CHECK(p3 != nullptr, "zone 3 written while settling");
        if(p3 != nullptr)
        {
            snprintf(msg, sizeof(msg), "settled logo reaches exact white, got %u,%u,%u", p3[5], p3[6], p3[7]);
            CHECK(p3[5] == 255 && p3[6] == 255 && p3[7] == 255, msg);
        }
    }
    bus.Clear();
    fake_ms += 45;
    CHECK(!dev.SettleStep(), "settle stops once converged");
    CHECK(bus.Count() == 0, "settled settle step writes nothing");

    /*-----------------------------------------------------*\
    | Time-based alpha: one 300 ms step lands where three   |
    | 100 ms steps do (response independent of call rate).  |
    \*-----------------------------------------------------*/
    {
        dev.SetColor(24, ToRGBColor(0, 0, 0));
        Converge(dev);
        Settle(dev);

        dev.SetColor(24, ToRGBColor(200, 200, 200));
        bus.Clear();
        Frame(dev, 300);
        const u8* one = bus.Latest(3);
        const int v_one = one != nullptr ? one[5] : -1;

        dev.SetColor(24, ToRGBColor(0, 0, 0));
        Converge(dev);
        Settle(dev);

        dev.SetColor(24, ToRGBColor(200, 200, 200));
        bus.Clear();
        Frame(dev, 100);
        Frame(dev, 100);
        Frame(dev, 100);
        const u8* three = bus.Latest(3);
        const int v_three = three != nullptr ? three[5] : -1;

        char msg[128];
        snprintf(msg, sizeof(msg), "one 300 ms step (%d) matches three 100 ms steps (%d)", v_one, v_three);
        CHECK(v_one > 0 && v_three > 0 && abs(v_one - v_three) <= 1, msg);
    }

    /*-----------------------------------------------------*\
    | Scenario 7 (finding 2): a non-uniform fan ring whose  |
    | targets all become equal keeps sending its full       |
    | palette while the smoothed slots still differ — slots |
    | 1-7 must not jump to slot 0's value — and collapses   |
    | to one color only once the outputs agree.             |
    \*-----------------------------------------------------*/
    {
        for(unsigned int i = 0; i < 8; i++)
        {
            dev.SetColor(i, ToRGBColor((unsigned char)(20 * i), 0, 0));
        }
        Converge(dev);
        Settle(dev);

        for(unsigned int i = 0; i < 8; i++)
        {
            dev.SetColor(i, ToRGBColor(100, 0, 0));
        }
        bus.Clear();
        Frame(dev);
        const u8* p0 = bus.Latest(0);
        CHECK(p0 != nullptr, "fan zone 0 written after the ring turns uniform");
        if(p0 != nullptr)
        {
            char msg[128];
            snprintf(msg, sizeof(msg), "fan ring keeps its palette while converging (SZ %u)", p0[10]);
            CHECK(p0[10] == 8, msg);
            /* Fan alpha 0.35 per 45 ms: slot i = 20i + (100 - 20i) * 0.35 */
            bool continuous = true;
            for(unsigned int i = 0; i < 8; i++)
            {
                const double expect = 20.0 * i + (100.0 - 20.0 * i) * 0.35;
                const int    got    = p0[11 + 3 * i];
                if(abs(got - (int)(expect + 0.5)) > 1)
                {
                    continuous = false;
                    printf("  slot %u: expected ~%.1f got %d\n", i, expect, got);
                }
            }
            CHECK(continuous, "every fan slot steps smoothly toward the shared target");
        }

        Settle(dev);
        const u8* settled = bus.Latest(0);
        CHECK(settled != nullptr && settled[10] == 0 &&
              settled[5] == 100 && settled[6] == 0 && settled[7] == 0,
              "converged uniform ring collapses to a single exact color");
    }

    /*-----------------------------------------------------*\
    | Scenario 8 (finding 8): zone/single-LED updates run   |
    | on another thread concurrently with the sweep and     |
    | the settle step. State stays consistent and the wire  |
    | ends at the exact target.                             |
    \*-----------------------------------------------------*/
    {
        dev.SetAllColors(ToRGBColor(10, 20, 30));
        std::thread other([&]()
        {
            for(int i = 0; i < 300; i++)
            {
                dev.UpdateZoneLEDs(i % 5);
                dev.UpdateSingleLED(i % 26);
            }
        });
        for(int i = 0; i < 300; i++)
        {
            fake_ms += 5;
            dev.SettleStep();
        }
        other.join();
        Settle(dev);
        FullFrame(dev, bus);
        CHECK(bus.Count() == 6, "concurrent updates leave a consistent full frame");
        for(uint8_t z = 0; z < bus.Count() && z < 6; z++)
        {
            CheckPacket(bus, z, z, 0, ToRGBColor(10, 20, 30), true);
        }
    }

    /*-----------------------------------------------------*\
    | Scenario 9 (review follow-up): a settle left pending  |
    | by a Direct change must not re-write every zone after |
    | a switch to a non-Direct mode — that re-sends each    |
    | mode packet and can restart the hardware effect.      |
    \*-----------------------------------------------------*/
    {
        dev.SetColor(24, ToRGBColor(200, 10, 10));
        Frame(dev);
        CHECK(dev.SettleStep(), "Direct change leaves the filter unsettled");

        const int pulse_idx = FindMode(dev, "Pulse");
        CHECK(pulse_idx >= 0, "Pulse (Breathing) mode present");
        dev.SetActiveMode(pulse_idx);
        DrainModeWrites(bus);

        fake_ms += 45;
        CHECK(!dev.SettleStep(), "non-Direct mode reports nothing to settle");
        fake_ms += 45;
        dev.SettleStep();
        {
            char msg[128];
            snprintf(msg, sizeof(msg),
                     "settle step after a switch to Pulse writes no zones (got %zu)", bus.Count());
            CHECK(bus.Count() == 0, msg);
        }

        dev.SetActiveMode(direct_idx);
        DrainModeWrites(bus);
    }

    /*-----------------------------------------------------*\
    | Scenario 5: Static transport preserves dark targets. |
    | Converge smoothing first.                             |
    \*-----------------------------------------------------*/
    dev.SetAllColors(ToRGBColor(6, 6, 6));
    Converge(dev);
    FullFrame(dev, bus);
    CHECK(bus.Count() == 6, "dark frame still writes every zone");
    const RGBColor dark = ToRGBColor(6, 6, 6);
    for(uint8_t z = 0; z < bus.Count() && z < 6; z++)
    {
        CheckPacket(bus, z, z, 0, dark, true);
    }

    /* The Direct-register encoder sends colors exactly as
       requested (finding 11: the luminance floor recolored dark
       targets, e.g. (40,0,0) -> (255,0,0)), every channel in its
       own RGB byte, with SZ=1 carrying the color as the palette. */
    CapturingBus floor_bus;
    RGBFusion2BlackwellGPUController floor_ctrl(
        &floor_bus, 0x75, "direct encoder regression",
        RGB_FUSION2_BLACKWELL_GPU_AORUS_MASTER_5080_LAYOUT);
    const RGBColor exact_cases[] =
    {
        ToRGBColor(0, 0, 80),
        ToRGBColor(40, 0, 0),
        ToRGBColor(0, 0, 255),
        ToRGBColor(100, 0, 255),
        ToRGBColor(180, 0, 240),
        ToRGBColor(0, 0, 0),
    };
    for(const RGBColor sample : exact_cases)
    {
        floor_bus.Clear();
        fusion2_config cfg = {};
        cfg.speed      = RGB_FUSION2_BLACKWELL_GPU_SPEED_NORMAL;
        cfg.brightness = RGB_FUSION2_BLACKWELL_GPU_BRIGHTNESS_MAX;
        cfg.colors[0]  = sample;
        floor_ctrl.SetMode(RGB_FUSION2_BLACKWELL_GPU_REG_COLOR, 3,
                           RGB_FUSION2_BLACKWELL_GPU_MODE_DIRECT, cfg);
        CheckPacket(floor_bus, 0, 3, 1, sample, true,
                    RGB_FUSION2_BLACKWELL_GPU_REG_COLOR,
                    RGB_FUSION2_BLACKWELL_GPU_MODE_DIRECT);
    }

    /* Other Blackwell layouts keep Direct; the 5090D V2 ICE retains
       its existing Static remap. Dark colors reach the wire
       unchanged on the Direct register too. */
    const int layouts[] = {
        RGB_FUSION2_BLACKWELL_GPU_SINGLE_ZONE,
        RGB_FUSION2_BLACKWELL_GPU_GAMING_LAYOUT,
        RGB_FUSION2_BLACKWELL_GPU_WATERFORCE_LAYOUT,
        RGB_FUSION2_BLACKWELL_GPU_AORUS_WATERFORCE_LAYOUT,
        RGB_FUSION2_BLACKWELL_GPU_AORUS_MASTER_5080_LAYOUT,
        RGB_FUSION2_BLACKWELL_GPU_AORUS_MASTER_5090D_V2_ICE_LAYOUT,
    };
    for(int layout : layouts)
    {
        CapturingBus layout_bus;
        RGBFusion2BlackwellGPUController layout_ctrl(
            &layout_bus, 0x75, "layout routing regression", layout);
        fusion2_config cfg = {};
        cfg.speed = RGB_FUSION2_BLACKWELL_GPU_SPEED_NORMAL;
        cfg.brightness = RGB_FUSION2_BLACKWELL_GPU_BRIGHTNESS_MAX;
        cfg.colors[0] = ToRGBColor(40, 0, 0);
        layout_ctrl.SetZone(0, RGB_FUSION2_BLACKWELL_GPU_MODE_DIRECT, cfg);
        CHECK(layout_bus.Count() == 1, "layout emits one frame packet");
        if(layout_bus.writes.empty()) continue;
        const bool use_static =
            layout == RGB_FUSION2_BLACKWELL_GPU_AORUS_MASTER_5080_LAYOUT ||
            layout == RGB_FUSION2_BLACKWELL_GPU_AORUS_MASTER_5090D_V2_ICE_LAYOUT;
        CHECK(layout_bus.writes[0][0] == (use_static
              ? RGB_FUSION2_BLACKWELL_GPU_REG_MODE
              : RGB_FUSION2_BLACKWELL_GPU_REG_COLOR),
              "only supported layouts use the Static transport");
        CHECK(layout_bus.writes[0][2] == (use_static
              ? RGB_FUSION2_BLACKWELL_GPU_MODE_STATIC
              : RGB_FUSION2_BLACKWELL_GPU_MODE_DIRECT),
              "layout retains the expected hardware mode");
        CHECK(layout_bus.writes[0][5] == 40 &&
              layout_bus.writes[0][6] == 0 &&
              layout_bus.writes[0][7] == 0,
              "dark (40,0,0) reaches the wire unchanged on every layout");
    }

    /*-----------------------------------------------------*\
    | Real settle thread (no test clock): one frame, then   |
    | nothing — the background thread must carry the logo  |
    | to its exact target on its own, then go quiet.        |
    \*-----------------------------------------------------*/
    {
        CapturingBus live_bus;
        RGBController_RGBFusion2BlackwellGPU live(
            new RGBFusion2BlackwellGPUController(
                &live_bus, 0x75, "settle thread regression",
                RGB_FUSION2_BLACKWELL_GPU_AORUS_MASTER_5080_LAYOUT),
            RGB_FUSION2_BLACKWELL_GPU_AORUS_MASTER_5080_LAYOUT);
        const int live_direct = FindMode(live, "Direct");
        live.SetAllColors(ToRGBColor(0, 0, 0));
        live.SetActiveMode(live_direct);
        DrainModeWrites(live_bus);

        live.SetColor(24, ToRGBColor(255, 255, 255));
        live.UpdateLEDs();

        bool reached = false;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while(!reached && std::chrono::steady_clock::now() < deadline)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            const u8* p3 = live_bus.Latest(3);
            reached = (p3 != nullptr && p3[5] == 255 && p3[6] == 255 && p3[7] == 255);
        }
        CHECK(reached, "settle thread converges the logo without further frames");

        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        live_bus.Clear();
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        CHECK(live_bus.Count() == 0, "settle thread goes quiet once converged");
    }

    printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
