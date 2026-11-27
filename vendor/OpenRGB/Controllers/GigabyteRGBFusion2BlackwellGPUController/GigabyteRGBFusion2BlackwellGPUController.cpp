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
#include <cstring>
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

int RGBFusion2BlackwellGPUController::BuildPacket(uint8_t type, uint8_t zone, uint8_t mode, const fusion2_config& zone_config, uint8_t* zone_pkt)
{
    if(zone_config.numberOfColors == 0 && zone < RGB_FUSION_2_BLACKWELL_GPU_NUMBER_OF_ZONES)
        this->zone_color[zone] = zone_config.colors[0];

    const RGBColor cached_color = (zone < RGB_FUSION_2_BLACKWELL_GPU_NUMBER_OF_ZONES) ? this->zone_color[zone] : zone_config.colors[0];

    /************************************************************************************\
    *                                                                                    *
    *       Packet (total size = 64 bytes)                                               *
    * TYPE      MODE SPD  BRT  R    G    B    0    ZONE SZ0-8                            *
    * 0x12 0x01 0x08 0x06 0x0A 0xFF 0xFF 0x00 0x00 0x00 0x08 [R] [G] [B] [R] [G] [B] ... *
    *                                                                                    *
    * SZ is the amount of colors that will be sent in the format of 3 bytes RGB          *
    *                                                                                    *
    \************************************************************************************/
    memset(zone_pkt, 0, 64);
    zone_pkt[0]  = type;
    zone_pkt[1]  = 0x01;
    zone_pkt[2]  = mode;
    zone_pkt[3]  = zone_config.speed;
    zone_pkt[4]  = zone_config.brightness;
    zone_pkt[5]  = (uint8_t)RGBGetRValue(cached_color);
    zone_pkt[6]  = (uint8_t)RGBGetGValue(cached_color);
    zone_pkt[7]  = (uint8_t)RGBGetBValue(cached_color);
    zone_pkt[8]  = 0x00;
    zone_pkt[9]  = zone;
    zone_pkt[10] = zone_config.numberOfColors;

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

        for(uint8_t i = 0; i < zone_config.numberOfColors && i < 8; i++)
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
        | palette entry. Colors go out exactly as requested: the    |
        | former luminance floor (an AORUS MASTER low-PWM flicker   |
        | workaround) never ran on MASTER, whose Direct writes use  |
        | the Static transport, and only recolored dark targets on  |
        | the other layouts.                                        |
        \*---------------------------------------------------------*/
        zone_pkt[10]              = 1;
        zone_pkt[palette_pos + 0] = zone_pkt[5];
        zone_pkt[palette_pos + 1] = zone_pkt[6];
        zone_pkt[palette_pos + 2] = zone_pkt[7];
    }

    return palette_pos;
}

void RGBFusion2BlackwellGPUController::WritePacket(uint8_t zone, const uint8_t* zone_pkt, int palette_pos, const fusion2_config& zone_config, std::chrono::steady_clock::time_point now)
{
    uint8_t pkt_copy[64];
    memcpy(pkt_copy, zone_pkt, sizeof(pkt_copy));

    const s32 write_result = bus->i2c_write_block(dev, sizeof(pkt_copy), pkt_copy);

    if(zone < RGB_FUSION_2_BLACKWELL_GPU_NUMBER_OF_ZONES)
    {
        /*-----------------------------------------------------*\
        | Only a successful write may suppress the next         |
        | identical one.                                        |
        \*-----------------------------------------------------*/
        memcpy(last_pkt[zone], zone_pkt, 64);
        last_pkt_valid[zone] = (write_result >= 0);
        last_pkt_time[zone]  = now;
    }

    BWGpuTrace(zone_pkt, palette_pos, (int)write_result,
               zone_config.diag_sm, zone_config.diag_who,
               zone_config.diag_tid, zone_config.colors[0]);
}

void RGBFusion2BlackwellGPUController::SetMode(uint8_t type, uint8_t zone, uint8_t mode, fusion2_config zone_config)
{
    uint8_t   zone_pkt[64];
    const int palette_pos = BuildPacket(type, zone, mode, zone_config, zone_pkt);

    WritePacket(zone, zone_pkt, palette_pos, zone_config, std::chrono::steady_clock::now());
}

uint8_t RGBFusion2BlackwellGPUController::ResolveTransport(uint8_t& mode, fusion2_config& zone_config)
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

    return type;
}

void RGBFusion2BlackwellGPUController::SetZone(uint8_t zone, uint8_t mode, fusion2_config zone_config)
{
    const uint8_t type = ResolveTransport(mode, zone_config);

    SetMode(type, zone, mode, zone_config);
}

bool RGBFusion2BlackwellGPUController::SetZoneIfChanged(uint8_t zone, uint8_t mode, fusion2_config zone_config,
                                                        std::chrono::steady_clock::time_point now,
                                                        std::chrono::milliseconds refresh)
{
    const uint8_t type = ResolveTransport(mode, zone_config);

    uint8_t   zone_pkt[64];
    const int palette_pos = BuildPacket(type, zone, mode, zone_config, zone_pkt);

    if(zone < RGB_FUSION_2_BLACKWELL_GPU_NUMBER_OF_ZONES &&
       last_pkt_valid[zone] &&
       memcmp(last_pkt[zone], zone_pkt, sizeof(zone_pkt)) == 0 &&
       now >= last_pkt_time[zone] &&
       (now - last_pkt_time[zone]) < refresh)
    {
        return false;
    }

    WritePacket(zone, zone_pkt, palette_pos, zone_config, now);
    return true;
}
