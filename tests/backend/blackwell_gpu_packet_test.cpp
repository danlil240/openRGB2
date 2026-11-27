/*---------------------------------------------------------*\
| blackwell_gpu_packet_test.cpp                             |
|                                                           |
|   Offline packet-level regression test for the Gigabyte   |
|   RGB Fusion 2 Blackwell GPU controller                   |
|   (AORUS MASTER 5080 layout). Captures the raw 64-byte    |
|   I2C block writes and asserts a multi-color fan palette  |
|   never leaks into the single-LED logo zones — the banner |
|   flicker seen under per-LED effects. No hardware, no Qt. |
\*---------------------------------------------------------*/

#include "GigabyteRGBFusion2BlackwellGPUController.h"
#include "RGBController_GigabyteRGBFusion2BlackwellGPU.h"
#include "i2c_smbus.h"
#include "LogManager.h"

#include <cstdarg>
#include <cstdio>
#include <chrono>
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
| CapturingBus — records every I2C block write              |
\*---------------------------------------------------------*/
class CapturingBus : public i2c_smbus_interface
{
public:
    std::vector<std::vector<u8>> writes;

    s32 i2c_smbus_xfer(u8, char, u8, int, i2c_smbus_data*) override
    {
        return -1;
    }

    s32 i2c_xfer(u8, char, int* size, u8* data) override
    {
        if(data != nullptr && size != nullptr)
        {
            writes.emplace_back(data, data + *size);
        }
        return 0;
    }
};

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
    while(bus.writes.size() < 6 &&
          std::chrono::steady_clock::now() < deadline)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    bus.writes.clear();
}

static void CheckPacket(const CapturingBus& bus, size_t idx, uint8_t zone,
                        uint8_t expected_sz, RGBColor single,
                        bool expect_single,
                        uint8_t expected_type = RGB_FUSION2_BLACKWELL_GPU_REG_MODE,
                        uint8_t expected_mode = RGB_FUSION2_BLACKWELL_GPU_MODE_STATIC)
{
    char msg[128];

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
             "zone %u SZ field matches its own palette size %u",
             zone, expected_sz);
    CHECK(p[10] == expected_sz, msg);

    if(expect_single)
    {
        /* Static single-color zones carry their color in bytes 5-7.
           Only a nonempty palette should carry it at offset 11 too. */
        snprintf(msg, sizeof(msg), "zone %u bytes 5-7 carry its own color", zone);
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
    for(int f = 0; f < 60; f++)
    {
        dev.DeviceUpdateLEDs();
    }
    bus.writes.clear();
    dev.DeviceUpdateLEDs();

    CHECK(bus.writes.size() == 6, "one packet per hardware zone (0-5)");

    /* Fan zones keep their 8-color palettes ... */
    for(uint8_t z = 0; z < 3 && z < bus.writes.size(); z++)
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
    if(bus.writes.size() >= 6)
    {
        CheckPacket(bus, 3, 3, 0, side_logo, true);
        CheckPacket(bus, 4, 4, 0, top_logo,  true);
        CheckPacket(bus, 5, 5, 0, top_logo,  true);
    }

    /*-----------------------------------------------------*\
    | Scenario 2: uniform frame — every zone single color.  |
    \*-----------------------------------------------------*/
    bus.writes.clear();
    const RGBColor flat = ToRGBColor(0x40, 0x80, 0xC0);
    dev.SetAllColors(flat);
    for(int f = 0; f < 60; f++)
    {
        dev.DeviceUpdateLEDs();
    }
    bus.writes.clear();
    dev.DeviceUpdateLEDs();

    CHECK(bus.writes.size() == 6, "uniform frame still writes 6 packets");
    for(uint8_t z = 0; z < bus.writes.size() && z < 6; z++)
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
        dev.DeviceUpdateLEDs();

        CHECK(bus.writes.size() == 6, "mode-specific frame writes 6 packets");
        for(uint8_t z = 0; z < bus.writes.size() && z < 6; z++)
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
    for(int f = 0; f < 60; f++)
    {
        dev.DeviceUpdateLEDs();
    }
    bus.writes.clear();
    dev.DeviceUpdateLEDs();
    CHECK(bus.writes.size() == 6, "baseline frame writes every zone");
    CheckPacket(bus, 3, 3, 0, base, true);
    bus.writes.clear();

    dev.SetColor(24, ToRGBColor(150, 90, 90));
    dev.SetColor(25, ToRGBColor(150, 90, 90));
    dev.DeviceUpdateLEDs();
    CHECK(bus.writes.size() == 6, "first step still writes every zone");
    {
        const uint8_t* p3 = bus.writes[3].data();
        char msg[128];
        /* Single-LED zone alpha is 0.15: 90 + 60*0.15 = 99 */
        snprintf(msg, sizeof(msg), "zone 3 step 1 smoothed toward target, got %u", p3[5]);
        CHECK(p3[5] > 95 && p3[5] < 103, msg);
        CHECK(p3[5] != 150, "first step does not land on target");
    }
    bus.writes.clear();

    dev.DeviceUpdateLEDs();
    {
        const uint8_t* p3 = bus.writes[3].data();
        char msg[128];
        /* 99 + 51*0.15 ~= 107 */
        snprintf(msg, sizeof(msg), "zone 3 step 2 converges further, got %u", p3[5]);
        CHECK(p3[5] > 103 && p3[5] < 112, msg);
    }
    bus.writes.clear();

    /*-----------------------------------------------------*\
    | Micro-jitter around a settled value is attenuated to  |
    | a sub-visible step on the wire.                       |
    \*-----------------------------------------------------*/
    for(int f = 0; f < 60; f++)
    {
        dev.DeviceUpdateLEDs();
    }
    bus.writes.clear();
    dev.SetColor(24, ToRGBColor(152, 92, 92));
    dev.DeviceUpdateLEDs();
    {
        const uint8_t* p3 = bus.writes[3].data();
        char msg[128];
        snprintf(msg, sizeof(msg), "zone 3 jitter attenuated, got %u", p3[5]);
        CHECK(p3[5] >= 150 && p3[5] <= 151, msg);
    }
    bus.writes.clear();

    /*-----------------------------------------------------*\
    | Scenario 5: Static transport preserves dark targets. |
    | The old Direct-register floor must not brighten the  |
    | 5080's streamed colors. Converge smoothing first.     |
    \*-----------------------------------------------------*/
    bus.writes.clear();
    dev.SetAllColors(ToRGBColor(6, 6, 6));
    for(int f = 0; f < 60; f++)
    {
        dev.DeviceUpdateLEDs();
    }
    bus.writes.clear();
    dev.DeviceUpdateLEDs();
    CHECK(bus.writes.size() == 6, "dark frame still writes every zone");
    const RGBColor dark = ToRGBColor(6, 6, 6);
    for(uint8_t z = 0; z < bus.writes.size() && z < 6; z++)
    {
        CheckPacket(bus, z, z, 0, dark, true);
    }

    /* A saturated channel boosted by the floor must stay in its
       own RGB byte. Blue reaches the clamp well before luma 64:
       (0,0,80) must become blue, never a red flash. */
    CapturingBus floor_bus;
    RGBFusion2BlackwellGPUController floor_ctrl(
        &floor_bus, 0x75, "floor regression",
        RGB_FUSION2_BLACKWELL_GPU_AORUS_MASTER_5080_LAYOUT);
    const struct
    {
        RGBColor input;
        RGBColor expected;
    } floor_cases[] =
    {
        { ToRGBColor(0, 0, 80),     ToRGBColor(0, 0, 255) },
        { ToRGBColor(0, 0, 255),    ToRGBColor(0, 0, 255) },
        { ToRGBColor(100, 0, 255),  ToRGBColor(161, 0, 255) },
        { ToRGBColor(180, 0, 240),  ToRGBColor(207, 0, 255) },
        { ToRGBColor(0, 0, 0),      ToRGBColor(0, 0, 0) },
    };
    for(const auto& sample : floor_cases)
    {
        floor_bus.writes.clear();
        fusion2_config cfg = {};
        cfg.speed      = RGB_FUSION2_BLACKWELL_GPU_SPEED_NORMAL;
        cfg.brightness = RGB_FUSION2_BLACKWELL_GPU_BRIGHTNESS_MAX;
        cfg.colors[0]  = sample.input;
        /* Exercise the underlying Direct-register encoder explicitly;
           5080 SetZone now routes streamed frames around this floor. */
        floor_ctrl.SetMode(RGB_FUSION2_BLACKWELL_GPU_REG_COLOR, 3,
                           RGB_FUSION2_BLACKWELL_GPU_MODE_DIRECT, cfg);
        CheckPacket(floor_bus, 0, 3, 1, sample.expected, true,
                    RGB_FUSION2_BLACKWELL_GPU_REG_COLOR,
                    RGB_FUSION2_BLACKWELL_GPU_MODE_DIRECT);
    }

    /* Other Blackwell layouts keep Direct; the 5090D V2 ICE retains
       its existing Static remap. */
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
        cfg.colors[0] = ToRGBColor(90, 90, 90);
        layout_ctrl.SetZone(0, RGB_FUSION2_BLACKWELL_GPU_MODE_DIRECT, cfg);
        CHECK(layout_bus.writes.size() == 1, "layout emits one frame packet");
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
    }

    printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
