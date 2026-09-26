/*---------------------------------------------------------*\
||| key_translate_test.cpp                                    |
|||   Qt-free unit test for native->VK key translation.     |
|||   SPDX-License-Identifier: GPL-2.0-or-later             |
\*---------------------------------------------------------*/

#include "inputs/KeyTranslate.h"
#include "inputs/VkDefs.h"
#include <cstdio>

static int fails = 0;
#define CHECK(expr) do { if(!(expr)) { \
    std::printf("FAIL %d: %s\n", __LINE__, #expr); ++fails; } } while(0)

int main()
{
    using namespace studio;
    /* evdev positional sanity */
    CHECK(VkForEvdevCode(16) == 'Q');       /* KEY_Q            */
    CHECK(VkForEvdevCode(57) == VK_SPACE);  /* KEY_SPACE        */
    CHECK(VkForEvdevCode(28) == VK_RETURN); /* KEY_ENTER        */
    CHECK(VkForEvdevCode(42) == VK_LSHIFT); /* KEY_LEFTSHIFT    */
    CHECK(VkForEvdevCode(59) == VK_F1);     /* KEY_F1           */
    CHECK(VkForEvdevCode(0)   == -1);       /* KEY_RESERVED     */
    CHECK(VkForEvdevCode(999) == -1);       /* out of range     */
    /* macOS */
    CHECK(VkForMacCode(0x0C) == 'Q');       /* kVK_ANSI_Q       */
    CHECK(VkForMacCode(0x31) == VK_SPACE);
    CHECK(VkForMacCode(0x24) == VK_RETURN);
    CHECK(VkForMacCode(0x7A) == VK_F1);
    CHECK(VkForMacCode(0xFFFF) == -1);
    /* Cross-check: every name_vk VK that both OSes can produce —
       letters/digits agree with the KeyMap contract */
    CHECK(VkForEvdevCode(30) == VkForMacCode(0x00));  /* A */
    CHECK(VkForEvdevCode(44) == VkForMacCode(0x06));  /* Z */
    std::printf(fails ? "%d FAILURES\n" : "all key_translate tests pass\n", fails);
    return fails;
}
