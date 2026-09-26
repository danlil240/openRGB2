/*---------------------------------------------------------*\
||| KeyTranslate.cpp                                          |
|||   Native key codes -> Windows VK. Two static lookup     |
|||   tables, same linear-scan shape as KeyMap's            |
|||   VkForKeyName. -1 on miss.                             |
|||   SPDX-License-Identifier: GPL-2.0-or-later             |
\*---------------------------------------------------------*/

#include "KeyTranslate.h"
#include "VkDefs.h"

#include <cstddef>

namespace studio
{

struct CodeVk { int code; int vk; };

/* evdev codes are linux/input-event-codes.h KEY_* values. */
static const CodeVk evdev_vk[] =
{
    {   1, VK_ESCAPE },   /* KEY_ESC              */
    {   2, '1' }, { 3, '2' }, { 4, '3' }, { 5, '4' }, { 6, '5' },
    {   7, '6' }, { 8, '7' }, { 9, '8' }, { 10, '9' }, { 11, '0' },
    {  12, VK_OEM_MINUS },/* KEY_MINUS            */
    {  13, VK_OEM_PLUS }, /* KEY_EQUAL            */
    {  14, VK_BACK },     /* KEY_BACKSPACE        */
    {  15, VK_TAB },      /* KEY_TAB              */
    {  16, 'Q' }, { 17, 'W' }, { 18, 'E' }, { 19, 'R' }, { 20, 'T' },
    {  21, 'Y' }, { 22, 'U' }, { 23, 'I' }, { 24, 'O' }, { 25, 'P' },
    {  26, VK_OEM_4 },    /* KEY_LEFTBRACE        */
    {  27, VK_OEM_6 },    /* KEY_RIGHTBRACE       */
    {  28, VK_RETURN },   /* KEY_ENTER            */
    {  29, VK_LCONTROL }, /* KEY_LEFTCTRL         */
    {  30, 'A' }, { 31, 'S' }, { 32, 'D' }, { 33, 'F' }, { 34, 'G' },
    {  35, 'H' }, { 36, 'J' }, { 37, 'K' }, { 38, 'L' },
    {  39, VK_OEM_1 },    /* KEY_SEMICOLON        */
    {  40, VK_OEM_7 },    /* KEY_APOSTROPHE       */
    {  41, VK_OEM_3 },    /* KEY_GRAVE            */
    {  42, VK_LSHIFT },   /* KEY_LEFTSHIFT        */
    {  43, VK_OEM_5 },    /* KEY_BACKSLASH        */
    {  44, 'Z' }, { 45, 'X' }, { 46, 'C' }, { 47, 'V' }, { 48, 'B' },
    {  49, 'N' }, { 50, 'M' },
    {  51, VK_OEM_COMMA },{ 52, VK_OEM_PERIOD }, { 53, VK_OEM_2 },
    {  54, VK_RSHIFT },   /* KEY_RIGHTSHIFT       */
    {  55, VK_MULTIPLY }, /* KEY_KPASTERISK       */
    {  56, VK_LMENU },    /* KEY_LEFTALT          */
    {  57, VK_SPACE },    /* KEY_SPACE            */
    {  58, VK_CAPITAL },  /* KEY_CAPSLOCK         */
    {  59, VK_F1     }, { 60, VK_F1+1 }, { 61, VK_F1+2 }, { 62, VK_F1+3 },
    {  63, VK_F1+4   }, { 64, VK_F1+5 }, { 65, VK_F1+6 }, { 66, VK_F1+7 },
    {  67, VK_F1+8   }, { 68, VK_F1+9 },
    {  69, VK_NUMLOCK },  /* KEY_NUMLOCK          */
    {  70, VK_SCROLL },   /* KEY_SCROLLLOCK       */
    {  71, VK_NUMPAD0+7 },{ 72, VK_NUMPAD0+8 }, { 73, VK_NUMPAD0+9 },
    {  74, VK_SUBTRACT }, /* KEY_KPMINUS          */
    {  75, VK_NUMPAD0+4 },{ 76, VK_NUMPAD0+5 }, { 77, VK_NUMPAD0+6 },
    {  78, VK_ADD },      /* KEY_KPPLUS           */
    {  79, VK_NUMPAD0+1 },{ 80, VK_NUMPAD0+2 }, { 81, VK_NUMPAD0+3 },
    {  82, VK_NUMPAD0   },{ 83, VK_DECIMAL },     /* KP0, KPDOT     */
    {  86, VK_OEM_102 },  /* KEY_102ND (ISO <> )  */
    {  87, VK_F1+10 }, { 88, VK_F1+11 },          /* KEY_F11, F12   */
    {  96, VK_RETURN },   /* KEY_KPENTER          */
    {  97, VK_RCONTROL }, /* KEY_RIGHTCTRL        */
    {  98, VK_DIVIDE },   /* KEY_KPSLASH          */
    {  99, VK_SNAPSHOT }, /* KEY_SYSRQ            */
    { 100, VK_RMENU },    /* KEY_RIGHTALT         */
    { 102, VK_HOME }, { 103, VK_UP }, { 104, VK_PRIOR }, { 105, VK_LEFT },
    { 106, VK_RIGHT }, { 107, VK_END }, { 108, VK_DOWN }, { 109, VK_NEXT },
    { 110, VK_INSERT }, { 111, VK_DELETE },
    { 113, VK_VOLUME_MUTE }, { 114, VK_VOLUME_DOWN }, { 115, VK_VOLUME_UP },
    { 119, VK_PAUSE },    /* KEY_PAUSE            */
    { 125, VK_LWIN }, { 126, VK_RWIN },           /* META keys      */
    { 127, VK_APPS },     /* KEY_COMPOSE          */
    { 163, VK_MEDIA_NEXT_TRACK }, { 164, VK_MEDIA_PLAY_PAUSE },
    { 165, VK_MEDIA_PREV_TRACK }, { 166, VK_MEDIA_STOP },
    { 183, VK_F1+12 }, { 184, VK_F1+13 }, { 185, VK_F1+14 }, { 186, VK_F1+15 },
    { 187, VK_F1+16 }, { 188, VK_F1+17 }, { 189, VK_F1+18 }, { 190, VK_F1+19 },
    { 191, VK_F1+20 }, { 192, VK_F1+21 }, { 193, VK_F1+22 }, { 194, VK_F1+23 },
};

/* macOS codes are Carbon kVK_* / CGKeyCode values. */
static const CodeVk mac_vk[] =
{
    { 0x00, 'A' }, { 0x01, 'S' }, { 0x02, 'D' }, { 0x03, 'F' },
    { 0x04, 'H' }, { 0x05, 'G' }, { 0x06, 'Z' }, { 0x07, 'X' },
    { 0x08, 'C' }, { 0x09, 'V' }, { 0x0A, VK_OEM_102 }, /* ISO §   */
    { 0x0B, 'B' }, { 0x0C, 'Q' }, { 0x0D, 'W' }, { 0x0E, 'E' },
    { 0x0F, 'R' }, { 0x10, 'Y' }, { 0x11, 'T' },
    { 0x12, '1' }, { 0x13, '2' }, { 0x14, '3' }, { 0x15, '4' },
    { 0x16, '6' }, { 0x17, '5' }, { 0x18, VK_OEM_PLUS }, { 0x19, '9' },
    { 0x1A, '7' }, { 0x1B, VK_OEM_MINUS }, { 0x1C, '8' }, { 0x1D, '0' },
    { 0x1E, VK_OEM_6 }, { 0x1F, 'O' }, { 0x20, 'U' }, { 0x21, VK_OEM_4 },
    { 0x22, 'I' }, { 0x23, 'P' }, { 0x24, VK_RETURN }, { 0x25, 'L' },
    { 0x26, 'J' }, { 0x27, VK_OEM_7 }, { 0x28, 'K' }, { 0x29, VK_OEM_1 },
    { 0x2A, VK_OEM_5 }, { 0x2B, VK_OEM_COMMA }, { 0x2C, VK_OEM_2 },
    { 0x2D, 'N' }, { 0x2E, 'M' }, { 0x2F, VK_OEM_PERIOD },
    { 0x30, VK_TAB }, { 0x31, VK_SPACE }, { 0x32, VK_OEM_3 },
    { 0x33, VK_BACK }, { 0x35, VK_ESCAPE },
    { 0x36, VK_RWIN }, { 0x37, VK_LWIN },   /* Cmd            */
    { 0x38, VK_LSHIFT }, { 0x39, VK_CAPITAL }, { 0x3A, VK_LMENU },
    { 0x3B, VK_LCONTROL }, { 0x3C, VK_RSHIFT }, { 0x3D, VK_RMENU },
    { 0x3E, VK_RCONTROL },
    { 0x40, VK_F1+16 },                     /* kVK_F17        */
    { 0x41, VK_DECIMAL }, { 0x43, VK_MULTIPLY }, { 0x45, VK_ADD },
    { 0x47, VK_NUMLOCK }, { 0x4B, VK_DIVIDE }, { 0x4C, VK_RETURN },
    { 0x4E, VK_SUBTRACT },
    { 0x4F, VK_F1+17 }, { 0x50, VK_F1+18 }, /* kVK_F18, F19   */
    { 0x52, VK_NUMPAD0 }, { 0x53, VK_NUMPAD0+1 }, { 0x54, VK_NUMPAD0+2 },
    { 0x55, VK_NUMPAD0+3 }, { 0x56, VK_NUMPAD0+4 }, { 0x57, VK_NUMPAD0+5 },
    { 0x58, VK_NUMPAD0+6 }, { 0x59, VK_NUMPAD0+7 },
    { 0x5A, VK_F1+19 },                     /* kVK_F20        */
    { 0x5B, VK_NUMPAD0+8 }, { 0x5C, VK_NUMPAD0+9 },
    { 0x5F, VK_OEM_COMMA },                 /* keypad comma   */
    { 0x60, VK_F1+4 }, { 0x61, VK_F1+5 }, { 0x62, VK_F1+6 },
    { 0x63, VK_F1+2 }, { 0x64, VK_F1+7 }, { 0x65, VK_F1+8 },
    { 0x67, VK_F1+10 }, { 0x69, VK_F1+12 }, { 0x6A, VK_F1+15 },
    { 0x6B, VK_F1+13 }, { 0x6D, VK_F1+9 }, { 0x6F, VK_F1+11 },
    { 0x71, VK_F1+14 },
    { 0x72, VK_INSERT }, { 0x73, VK_HOME }, { 0x74, VK_PRIOR },
    { 0x75, VK_DELETE }, { 0x76, VK_F1+3 }, { 0x77, VK_END },
    { 0x78, VK_F1+1 }, { 0x79, VK_NEXT },
    { 0x7A, VK_F1 }, { 0x7B, VK_LEFT }, { 0x7C, VK_RIGHT },
    { 0x7D, VK_DOWN }, { 0x7E, VK_UP },
    /* Media keys arrive as NX_SYSDEFINED events, not keycodes —
       intentionally unmapped on macOS. */
};

static int Lookup(const CodeVk* table, std::size_t count, int code)
{
    for(std::size_t i = 0; i < count; i++)
    {
        if(table[i].code == code)
        {
            return table[i].vk;
        }
    }
    return -1;
}

int VkForEvdevCode(int code)
{
    return Lookup(evdev_vk, sizeof(evdev_vk) / sizeof(evdev_vk[0]), code);
}

int VkForMacCode(int code)
{
    return Lookup(mac_vk, sizeof(mac_vk) / sizeof(mac_vk[0]), code);
}

} /* namespace studio */
