/*---------------------------------------------------------*\
||| KeyTranslate.h                                            |
|||   Native key codes -> Windows VK (the plugin's          |
|||   canonical key id). Pure tables, Qt-free, no platform  |
|||   headers — codes arrive as ints so this unit is        |
|||   testable on any OS. -1 = unmapped.                    |
|||   SPDX-License-Identifier: GPL-2.0-or-later             |
\*---------------------------------------------------------*/
#pragma once

namespace studio
{
int VkForEvdevCode(int code);   /* linux/input-event-codes.h KEY_* */
int VkForMacCode(int code);     /* Carbon kVK_* / CGKeyCode        */
}
