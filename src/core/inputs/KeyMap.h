/*---------------------------------------------------------*\
||| KeyMap.h                                                  |
|||                                                           |
|||   Translates OpenRGB LED names ("Key: Q", "Key: Left    |
|||   Shift") to Windows virtual-key codes so a keyboard    |
|||   hook event can be resolved to the emitter at that     |
|||   key's position. Layout-accurate: names come from the  |
|||   bound device's own LED table. Qt-free header; the     |
|||   .cpp uses <windows.h> for VK_* constants.             |
|||                                                           |
|||   Only transient key events cross this boundary — no    |
|||   typed-text history is retained (plan privacy rule).   |
|||                                                           |
|||   SPDX-License-Identifier: GPL-2.0-or-later               |
\*---------------------------------------------------------*/

#pragma once

#include <string>

namespace studio
{

/* "Key: Q" -> VK 'Q'. Returns -1 for unmapped names
   (indicators, Fn, vendor-specific) and for names that don't
   carry the "Key: " prefix. */
int VkForKeyName(const char* led_name);
int VkForKeyName(const std::string& led_name);

} /* namespace studio */
