/*---------------------------------------------------------*\
| GigabyteRGBFusion2BlackwellGPUController.cpp              |
|                                                           |
|   Driver for Gigabyte RGB Fusion 2 Blackwell GPU          |
|                                                           |
|   This file is part of the OpenRGB project                |
|   SPDX-License-Identifier: GPL-2.0-or-later               |
\*---------------------------------------------------------*/

#include <chrono>
#include <thread>
#include <cstdio>
#include <cstdlib>
#include "GigabyteRGBFusion2BlackwellGPUController.h"
#include "GigabyteRGBFusion2BlackwellGPUDefinitions.h"
#include "LogManager.h"

using namespace std::chrono_literals;

RGBFusion2BlackwellGPUController::RGBFusion2BlackwellGPUController(i2c_smbus_interface* bus, rgb_fusion_dev_id dev, std::string dev_name, int gpu_layout)
{
    this->bus           = bus;
    this->dev           = dev;
    this->name          = dev_name;
    this->gpu_layout    = gpu_layout;
}

RGBFusion2BlackwellGPUController::~RGBFusion2BlackwellGPUController()
{

}

std::string RGBFusion2BlackwellGPUController::GetDeviceLocation()
{
    std::string return_string(bus->info.device_name);
    char addr[5];
    snprintf(addr, 5, "0x%02X", dev);
    return_string.append(", address ");
    return_string.append(addr);
    return("I2C: " + return_string);
}

std::string RGBFusion2BlackwellGPUController::GetDeviceName()
{
    return(name);
}

void RGBFusion2BlackwellGPUController::SaveConfig()
{
    uint8_t data_pkt[64] = { 0x13, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
    bus->i2c_write_block(dev, sizeof(data_pkt), data_pkt);
}

/*---------------------------------------------------------*\
|  Packet trace (flicker diagnostics): set the environment  |
|  variable OPENRGB_BWGPU_TRACE to a file path to append    |
|  one CSV row per I2C block write. Off unless the variable |
|  is set at process start.                                 |
\*---------------------------------------------------------*/
static FILE* BWGpuTraceFile()
{
    static FILE* trace_file = []() -> FILE*
    {
        const char* path = getenv("OPENRGB_BWGPU_TRACE");

        if(path == nullptr || *path == '\0')
        {
            return nullptr;
        }

        FILE* f = fopen(path, "a");

        if(f != nullptr)
        {
            fprintf(f, "t_ms,type,mode,zone,spd,brt,zr,zg,zb,sz,p0,p1,p2,p3,p4,p5,p6,p7,ret,sm,who,tid,want\n");
        }

        return f;
    }();

    return trace_file;
}

static unsigned long long BWGpuTraceMs()
{
    static const std::chrono::steady_clock::time_point t0 =
        std::chrono::steady_clock::now();

    return (unsigned long long)std::chrono::duration_cast<std::chrono::milliseconds>
           (std::chrono::steady_clock::now() - t0).count();
}

static void BWGpuTrace(const uint8_t* pkt, int palette_pos, int write_result,
                       unsigned int diag_sm, unsigned int diag_who,
                       unsigned int diag_tid, RGBColor diag_want)
{
    FILE* f = BWGpuTraceFile();

    if(f == nullptr)
    {
        return;
    }

    fprintf(f, "%llu,0x%02X,0x%02X,%u,%u,%u,%u,%u,%u,%u",
            BWGpuTraceMs(), pkt[0], pkt[2], pkt[9], pkt[3], pkt[4],
            pkt[5], pkt[6], pkt[7], pkt[10]);

    for(uint8_t i = 0; i < 8; i++)
    {
        if(i < pkt[10] && palette_pos + 3 * i + 2 < 64)
        {
            fprintf(f, ",%02X%02X%02X", pkt[palette_pos + 3 * i],
                    pkt[palette_pos + 3 * i + 1], pkt[palette_pos + 3 * i + 2]);
        }
        else
        {
            fprintf(f, ",");
        }
    }

    fprintf(f, ",%d,%u,%u,%u,%06X\n", write_result, diag_sm, diag_who,
            diag_tid, (unsigned int)diag_want);
    fflush(f);
}

/*---------------------------------------------------------*\
|  Luminance floor (flicker workaround): AORUS MASTER       |
|  firmware visibly flickers when a direct-mode color       |
|  drifts through its low-PWM region. Dark colors are       |
|  scaled up to the floor, hue-preserving. Pure black       |
|  stays black so "off" still turns the LED off.            |
\*---------------------------------------------------------*/
static RGBColor BWGpuFloorColor(RGBColor color)
{
    const int r = (int)RGBGetRValue(color);
    const int g = (int)RGBGetGValue(color);
    const int b = (int)RGBGetBValue(color);

    const double luma = (0.2126 * r) + (0.7152 * g) + (0.0722 * b);
    if(luma >= 64.0 || luma < 2.0)
    {
        return color;
    }

    const double scale  = 64.0 / luma;
    const int    fr     = (int)((r * scale) + 0.5);
    const int    fg     = (int)((g * scale) + 0.5);
    const int    fb     = (int)((b * scale) + 0.5);

    /* ToRGBColor shifts its macro arguments without parentheses.
       Pass clamped values, not ternary expressions: otherwise a
       clamped blue/green channel is packed into the red byte. */
    const int cr = fr > 255 ? 255 : fr;
    const int cg = fg > 255 ? 255 : fg;
    const int cb = fb > 255 ? 255 : fb;
    return ToRGBColor(cr, cg, cb);
}

void RGBFusion2BlackwellGPUController::SetMode(uint8_t type, uint8_t zone, uint8_t mode, fusion2_config zone_config)
{
    if(zone_config.numberOfColors == 0 && zone < RGB_FUSION_2_BLACKWELL_GPU_NUMBER_OF_ZONES)
        this->zone_color[zone] = zone_config.colors[0];

    /************************************************************************************\
    *                                                                                    *
    *       Packet (total size = 64 bytes)                                               *
    * TYPE      MODE SPD  BRT  R    G    B    0    ZONE SZ0-8                            *
    * 0x12 0x01 0x08 0x06 0x0A 0xFF 0xFF 0x00 0x00 0x00 0x08 [R] [G] [B] [R] [G] [B] ... *
    *                                                                                    *
    * SZ is the amount of colors that will be sent in the format of 3 bytes RGB          *
    *                                                                                    *
    \************************************************************************************/
    uint8_t zone_pkt[64] = {type, 0x01, mode, zone_config.speed, zone_config.brightness, (uint8_t)RGBGetRValue(this->zone_color[zone]), (uint8_t)RGBGetGValue(this->zone_color[zone]), (uint8_t)RGBGetBValue(this->zone_color[zone]), 0x00, zone, zone_config.numberOfColors, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};

    int palette_pos = 12;
    switch(gpu_layout)
    {
        case RGB_FUSION2_BLACKWELL_GPU_AORUS_MASTER_5080_LAYOUT:
        case RGB_FUSION2_BLACKWELL_GPU_AORUS_MASTER_5090D_V2_ICE_LAYOUT:
            palette_pos = 11;
            break;
        default:
            break;
    }

    if(zone_config.numberOfColors > 0)
    {
        /*---------------------------------------------------------*\
        | A palette packet must carry the palette's first color in  |
        | the base-color field too — a stale cached base bleeds     |
        | onto the device alongside the fresh palette.              |
        \*---------------------------------------------------------*/
        zone_pkt[5] = (uint8_t)RGBGetRValue(zone_config.colors[0]);
        zone_pkt[6] = (uint8_t)RGBGetGValue(zone_config.colors[0]);
        zone_pkt[7] = (uint8_t)RGBGetBValue(zone_config.colors[0]);

        int currentPos = palette_pos;

        for(uint8_t i = 0; i < zone_config.numberOfColors; i++)
        {
            zone_pkt[currentPos + 0] = RGBGetRValue(zone_config.colors[i]);
            zone_pkt[currentPos + 1] = RGBGetGValue(zone_config.colors[i]);
            zone_pkt[currentPos + 2] = RGBGetBValue(zone_config.colors[i]);
            currentPos += 3;
        }
    }
    else if(type == RGB_FUSION2_BLACKWELL_GPU_REG_COLOR)
    {
        /*---------------------------------------------------------*\
        | The color register's payload is the palette (SZ entries) |
        | — an empty palette is a no-op on AORUS MASTER firmware,   |
        | which then keeps re-rendering its stored profile. A       |
        | single-color zone must send SZ=1 with the color as the    |
        | palette entry. The luminance floor keeps drifting colors  |
        | out of the firmware's low-PWM flicker region.             |
        \*---------------------------------------------------------*/
        const RGBColor base_color = BWGpuFloorColor(this->zone_color[zone]);
        zone_pkt[5]               = (uint8_t)RGBGetRValue(base_color);
        zone_pkt[6]               = (uint8_t)RGBGetGValue(base_color);
        zone_pkt[7]               = (uint8_t)RGBGetBValue(base_color);
        zone_pkt[10]              = 1;
        zone_pkt[palette_pos + 0] = (uint8_t)RGBGetRValue(base_color);
        zone_pkt[palette_pos + 1] = (uint8_t)RGBGetGValue(base_color);
        zone_pkt[palette_pos + 2] = (uint8_t)RGBGetBValue(base_color);
    }

    const s32 write_result = bus->i2c_write_block(dev, sizeof(zone_pkt), zone_pkt);

    BWGpuTrace(zone_pkt, palette_pos, (int)write_result,
               zone_config.diag_sm, zone_config.diag_who,
               zone_config.diag_tid, zone_config.colors[0]);
}

void RGBFusion2BlackwellGPUController::SetZone(uint8_t zone, uint8_t mode, fusion2_config zone_config)
{
    if(mode == RGB_FUSION2_BLACKWELL_GPU_MODE_BREATHING)
        zone_config.brightness = RGB_FUSION2_BLACKWELL_GPU_BRIGHTNESS_MAX;

    switch(gpu_layout)
    {
        case RGB_FUSION2_BLACKWELL_GPU_AORUS_MASTER_5080_LAYOUT:
        case RGB_FUSION2_BLACKWELL_GPU_AORUS_MASTER_5090D_V2_ICE_LAYOUT:
            /* The 5080 MASTER's Intelligent (0x16) path flickers
               while changing colors, even with one isolated logo.
               Streaming through Static (0x12/0x01) is stable. Keep
               logical Direct mode and its fan palettes, but use the
               Static transport, as the 5090D V2 ICE already does. */
            if(mode == RGB_FUSION2_BLACKWELL_GPU_MODE_DIRECT)
                mode = RGB_FUSION2_BLACKWELL_GPU_MODE_STATIC;
            break;

        default:
            break;
    }

    uint8_t type = RGB_FUSION2_BLACKWELL_GPU_REG_COLOR;
    if(mode != RGB_FUSION2_BLACKWELL_GPU_MODE_DIRECT)
        type = RGB_FUSION2_BLACKWELL_GPU_REG_MODE;

    SetMode(type, zone, mode, zone_config);
}
