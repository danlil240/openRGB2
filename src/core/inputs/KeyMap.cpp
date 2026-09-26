/*---------------------------------------------------------*\
||| KeyMap.cpp                                                |
|||                                                           |
|||   SPDX-License-Identifier: GPL-2.0-or-later               |
\*---------------------------------------------------------*/

#include "KeyMap.h"
#include "VkDefs.h"

#include <cstring>

namespace studio
{

struct NameVk { const char* name; int vk; };

/* Names match RGBControllerKeyNames.cpp (KEY_EN_* strings). */
static const NameVk name_vk[] =
{
    { "Key: Escape",            VK_ESCAPE    },
    { "Key: F1",                VK_F1        },
    { "Key: F2",                VK_F1 + 1    },
    { "Key: F3",                VK_F1 + 2    },
    { "Key: F4",                VK_F1 + 3    },
    { "Key: F5",                VK_F1 + 4    },
    { "Key: F6",                VK_F1 + 5    },
    { "Key: F7",                VK_F1 + 6    },
    { "Key: F8",                VK_F1 + 7    },
    { "Key: F9",                VK_F1 + 8    },
    { "Key: F10",               VK_F1 + 9    },
    { "Key: F11",               VK_F1 + 10   },
    { "Key: F12",               VK_F1 + 11   },
    { "Key: F13",               VK_F1 + 12   },
    { "Key: F14",               VK_F1 + 13   },
    { "Key: F15",               VK_F1 + 14   },
    { "Key: F16",               VK_F1 + 15   },
    { "Key: F17",               VK_F1 + 16   },
    { "Key: F18",               VK_F1 + 17   },
    { "Key: F19",               VK_F1 + 18   },
    { "Key: F20",               VK_F1 + 19   },
    { "Key: F21",               VK_F1 + 20   },
    { "Key: F22",               VK_F1 + 21   },
    { "Key: F23",               VK_F1 + 22   },
    { "Key: F24",               VK_F1 + 23   },
    { "Key: Print Screen",      VK_SNAPSHOT  },
    { "Key: Scroll Lock",       VK_SCROLL    },
    { "Key: Pause/Break",       VK_PAUSE     },
    { "Key: `",                 VK_OEM_3     },
    { "Key: 1",                 '1'          },
    { "Key: 2",                 '2'          },
    { "Key: 3",                 '3'          },
    { "Key: 4",                 '4'          },
    { "Key: 5",                 '5'          },
    { "Key: 6",                 '6'          },
    { "Key: 7",                 '7'          },
    { "Key: 8",                 '8'          },
    { "Key: 9",                 '9'          },
    { "Key: 0",                 '0'          },
    { "Key: -",                 VK_OEM_MINUS },
    { "Key: =",                 VK_OEM_PLUS  },
    { "Key: +",                 VK_OEM_PLUS  },
    { "Key: Backspace",         VK_BACK      },
    { "Key: Insert",            VK_INSERT    },
    { "Key: Home",              VK_HOME      },
    { "Key: Page Up",           VK_PRIOR     },
    { "Key: Tab",               VK_TAB       },
    { "Key: Q",                 'Q'          },
    { "Key: W",                 'W'          },
    { "Key: E",                 'E'          },
    { "Key: R",                 'R'          },
    { "Key: T",                 'T'          },
    { "Key: Y",                 'Y'          },
    { "Key: U",                 'U'          },
    { "Key: I",                 'I'          },
    { "Key: O",                 'O'          },
    { "Key: P",                 'P'          },
    { "Key: [",                 VK_OEM_4     },
    { "Key: ]",                 VK_OEM_6     },
    { "Key: \\",                VK_OEM_5     },
    { "Key: \\ (ANSI)",         VK_OEM_5     },
    { "Key: \\ (ISO)",          VK_OEM_102   },
    { "Key: Delete",            VK_DELETE    },
    { "Key: End",               VK_END       },
    { "Key: Page Down",         VK_NEXT      },
    { "Key: Caps Lock",         VK_CAPITAL   },
    { "Key: A",                 'A'          },
    { "Key: S",                 'S'          },
    { "Key: D",                 'D'          },
    { "Key: F",                 'F'          },
    { "Key: G",                 'G'          },
    { "Key: H",                 'H'          },
    { "Key: J",                 'J'          },
    { "Key: K",                 'K'          },
    { "Key: L",                 'L'          },
    { "Key: ;",                 VK_OEM_1     },
    { "Key: '",                 VK_OEM_7     },
    { "Key: #",                 VK_OEM_102   },
    { "Key: Enter",             VK_RETURN    },
    { "Key: Enter (ISO)",       VK_RETURN    },
    { "Key: Left Shift",        VK_LSHIFT    },
    { "Key: Right Shift",       VK_RSHIFT    },
    { "Key: Z",                 'Z'          },
    { "Key: X",                 'X'          },
    { "Key: C",                 'C'          },
    { "Key: V",                 'V'          },
    { "Key: B",                 'B'          },
    { "Key: N",                 'N'          },
    { "Key: M",                 'M'          },
    { "Key: ,",                 VK_OEM_COMMA },
    { "Key: .",                 VK_OEM_PERIOD},
    { "Key: /",                 VK_OEM_2     },
    { "Key: Up Arrow",          VK_UP        },
    { "Key: Left Arrow",        VK_LEFT      },
    { "Key: Down Arrow",        VK_DOWN      },
    { "Key: Right Arrow",       VK_RIGHT     },
    { "Key: Left Control",      VK_LCONTROL  },
    { "Key: Right Control",     VK_RCONTROL  },
    { "Key: Left Windows",      VK_LWIN      },
    { "Key: Right Windows",     VK_RWIN      },
    { "Key: Left Alt",          VK_LMENU     },
    { "Key: Right Alt",         VK_RMENU     },
    { "Key: Space",             VK_SPACE     },
    { "Key: Menu",              VK_APPS      },
    { "Key: Num Lock",          VK_NUMLOCK   },
    { "Key: Number Pad /",      VK_DIVIDE    },
    { "Key: Number Pad *",      VK_MULTIPLY  },
    { "Key: Number Pad -",      VK_SUBTRACT  },
    { "Key: Number Pad +",      VK_ADD       },
    { "Key: Number Pad .",      VK_DECIMAL   },
    { "Key: Number Pad Enter",  VK_RETURN    },
    { "Key: Number Pad 0",      VK_NUMPAD0   },
    { "Key: Number Pad 1",      VK_NUMPAD0 + 1 },
    { "Key: Number Pad 2",      VK_NUMPAD0 + 2 },
    { "Key: Number Pad 3",      VK_NUMPAD0 + 3 },
    { "Key: Number Pad 4",      VK_NUMPAD0 + 4 },
    { "Key: Number Pad 5",      VK_NUMPAD0 + 5 },
    { "Key: Number Pad 6",      VK_NUMPAD0 + 6 },
    { "Key: Number Pad 7",      VK_NUMPAD0 + 7 },
    { "Key: Number Pad 8",      VK_NUMPAD0 + 8 },
    { "Key: Number Pad 9",      VK_NUMPAD0 + 9 },
    { "Key: Media Play/Pause",  VK_MEDIA_PLAY_PAUSE },
    { "Key: Media Previous",    VK_MEDIA_PREV_TRACK },
    { "Key: Media Next",        VK_MEDIA_NEXT_TRACK },
    { "Key: Media Stop",        VK_MEDIA_STOP       },
    { "Key: Media Mute",        VK_VOLUME_MUTE      },
    { "Key: Media Volume -",    VK_VOLUME_DOWN      },
    { "Key: Media Volume +",    VK_VOLUME_UP        },
    /* No VK exists for: Fn, Power, Brightness, Windows Lock,
       indicator LEDs — they return -1 below. */
};

int VkForKeyName(const char* led_name)
{
    if(led_name == nullptr)
    {
        return -1;
    }
    for(const NameVk& e : name_vk)
    {
        if(std::strcmp(led_name, e.name) == 0)
        {
            return e.vk;
        }
    }
    return -1;
}

int VkForKeyName(const std::string& led_name)
{
    return VkForKeyName(led_name.c_str());
}

} /* namespace studio */
