/*---------------------------------------------------------*\
|| LianLiWirelessFanType.h                                   ||
||                                                           ||
||   Wireless fan-variant classification from the fan-type   ||
||   byte in a discovery record. Table ported from           ||
||   sgtaziz/lian-li-linux fan_type.rs (MIT).                ||
||                                                           ||
||   This file is part of the OpenRGB project                ||
||   SPDX-License-Identifier: GPL-2.0-or-later               ||
\*---------------------------------------------------------*/

#pragma once

#include <cstdint>

namespace LianLiWireless
{
    enum class WirelessFanType
    {
        Slv3Led,        /* SL V3 wireless LED fans (bytes 20-22)   */
        Slv3Lcd,        /* SL V3 wireless LCD fans (bytes 23-26)  */
        Tlv2Lcd,        /* TL wireless LCD (27, 32-35)            */
        Tlv2Led,        /* TL wireless LED (28-31)                */
        SlInf,          /* SL-INF wireless (36-39)                */
        Clv1,           /* CL / RL120 (40-42)                     */
        SlInfV3,        /* SL-INF V3 (43-50, LCD on 43/44/47/48)  */
        TlV3,           /* TL V3 (51-58, LCD on 51/52/55/56)      */
        SlV4,           /* SL V4 (59-62)                          */
        P28V2,          /* P28 V2 (63)                            */
        ClV2,           /* CL V2 (126-127, 127 = reversed mount)  */
        Unknown,
    };

    struct WirelessFanInfo
    {
        WirelessFanType type;
        const char*     name;
        uint8_t         leds_per_fan;   /* 0 = not a per-fan layout  */
        /* initially supported set: the SLV3 hardware tested here   */
        bool            supported;
    };

    inline WirelessFanInfo ClassifyFanType(uint8_t b)
    {
        WirelessFanType t;
        bool            lcd = false;

        if(b >= 20 && b <= 22)       t = WirelessFanType::Slv3Led;
        else if(b >= 23 && b <= 26)  t = WirelessFanType::Slv3Lcd;
        else if(b == 27 || (b >= 32 && b <= 35)) t = WirelessFanType::Tlv2Lcd;
        else if(b >= 28 && b <= 31)  t = WirelessFanType::Tlv2Led;
        else if(b >= 36 && b <= 39)  t = WirelessFanType::SlInf;
        else if(b >= 40 && b <= 42)  t = WirelessFanType::Clv1;
        else if(b >= 43 && b <= 50)
        {
            t   = WirelessFanType::SlInfV3;
            lcd = (b == 43 || b == 44 || b == 47 || b == 48);
        }
        else if(b >= 51 && b <= 58)
        {
            t   = WirelessFanType::TlV3;
            lcd = (b == 51 || b == 52 || b == 55 || b == 56);
        }
        else if(b >= 59 && b <= 62)  t = WirelessFanType::SlV4;
        else if(b == 63)             t = WirelessFanType::P28V2;
        else if(b == 126 || b == 127) t = WirelessFanType::ClV2;
        else                         t = WirelessFanType::Unknown;

        switch(t)
        {
            case WirelessFanType::Slv3Led:
                return { t, "UNI FAN SL V3 Wireless", 40, true };
            case WirelessFanType::Slv3Lcd:
                return { t, "UNI FAN SL V3 Wireless LCD", 40, true };
            case WirelessFanType::Tlv2Lcd:
                return { t, "UNI FAN TL Wireless LCD", 26, false };
            case WirelessFanType::Tlv2Led:
                return { t, "UNI FAN TL Wireless", 26, false };
            case WirelessFanType::SlInf:
                return { t, "UNI FAN SL-INF Wireless", 44, false };
            case WirelessFanType::Clv1:
                return { t, "UNI FAN CL Wireless", 24, false };
            case WirelessFanType::SlInfV3:
                return { t, lcd ? "UNI FAN SL-INF V3 Wireless LCD"
                                : "UNI FAN SL-INF V3 Wireless", 44, false };
            case WirelessFanType::TlV3:
                return { t, lcd ? "UNI FAN TL V3 Wireless LCD"
                                : "UNI FAN TL V3 Wireless", 26, false };
            case WirelessFanType::SlV4:
                return { t, "UNI FAN SL V4 Wireless", 52, false };
            case WirelessFanType::P28V2:
                return { t, "UNI FAN P28 V2 Wireless", 9, false };
            case WirelessFanType::ClV2:
                return { t, "UNI FAN CL V2 Wireless", 24, false };
            default:
                return { t, "Unknown wireless fan", 0, false };
        }
    }

    /*---------------------------------------------------------*\
    | Physical perimeter order of an SL V3's 40 frame LEDs:     |
    | 12-LED top strip forward, 8-LED side strip reversed,      |
    | 12-LED bottom strip reversed, 8-LED side strip forward.   |
    | Iterating slots in this order follows the ring instead    |
    | of zig-zagging across it. nullptr means linear order.     |
    \*---------------------------------------------------------*/
    inline constexpr uint8_t SLV3_LED_ORDER[40] =
    {
        0,  1,  2,  3,  4,  5,  6,  7,  8,  9, 10, 11,
       19, 18, 17, 16, 15, 14, 13, 12,
       31, 30, 29, 28, 27, 26, 25, 24, 23, 22, 21, 20,
       32, 33, 34, 35, 36, 37, 38, 39
    };

    inline const uint8_t* FanLedOrder(WirelessFanType t)
    {
        switch(t)
        {
            case WirelessFanType::Slv3Led:
            case WirelessFanType::Slv3Lcd:
                return SLV3_LED_ORDER;
            default:
                return nullptr;
        }
    }
}
