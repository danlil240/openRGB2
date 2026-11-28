/*---------------------------------------------------------*\
||| Automation.cpp — see Automation.h (R-2 / R-4).            ||
|||                                                           ||
|||   SPDX-License-Identifier: GPL-2.0-or-later               ||
\*---------------------------------------------------------*/

#include "Automation.h"

#include "OutputControl.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <set>

namespace openrgb2
{

namespace
{

/*---------------------------------------------------------*\
||| Key-name table. Values are Windows virtual-key codes    ||
||| (the same constants src/core/inputs/VkDefs.h carries);  ||
||| written as literals so the Qt-free engine never pulls   ||
||| <windows.h>. The FIRST name of an entry is canonical.   ||
\*---------------------------------------------------------*/
struct KeyName
{
    uint32_t                 vk;
    std::vector<const char*> names;
};

const std::vector<KeyName>& KeyTable()
{
    static const std::vector<KeyName> table = []
    {
        std::vector<KeyName> t = {
            { 0x08, { "Backspace", "Back" } },
            { 0x09, { "Tab" } },
            { 0x0D, { "Enter", "Return" } },
            { 0x13, { "Pause", "Break" } },
            { 0x14, { "CapsLock", "Caps" } },
            { 0x1B, { "Esc", "Escape" } },
            { 0x20, { "Space" } },
            { 0x21, { "PgUp", "PageUp", "Prior" } },
            { 0x22, { "PgDn", "PageDown", "Next" } },
            { 0x23, { "End" } },
            { 0x24, { "Home" } },
            { 0x25, { "Left" } },
            { 0x26, { "Up" } },
            { 0x27, { "Right" } },
            { 0x28, { "Down" } },
            { 0x2C, { "PrintScreen", "PrtSc", "Print" } },
            { 0x2D, { "Insert", "Ins" } },
            { 0x2E, { "Delete", "Del" } },
            { 0x6A, { "NumMul", "Multiply" } },
            { 0x6B, { "NumAdd", "Add" } },
            { 0x6D, { "NumSub", "Subtract" } },
            { 0x6E, { "NumDecimal", "Decimal" } },
            { 0x6F, { "NumDiv", "Divide" } },
            { 0x90, { "NumLock" } },
            { 0x91, { "ScrollLock", "Scroll" } },
            { 0xAD, { "VolumeMute", "Mute" } },
            { 0xAE, { "VolumeDown" } },
            { 0xAF, { "VolumeUp" } },
            { 0xB0, { "MediaNext" } },
            { 0xB1, { "MediaPrev" } },
            { 0xB2, { "MediaStop" } },
            { 0xB3, { "MediaPlayPause", "PlayPause" } },
            { 0xBA, { "Semicolon" } },
            { 0xBB, { "Plus", "Equals" } },
            { 0xBC, { "Comma" } },
            { 0xBD, { "Minus" } },
            { 0xBE, { "Period" } },
            { 0xBF, { "Slash" } },
            { 0xC0, { "Backquote", "Tilde" } },
            { 0xDB, { "LeftBracket" } },
            { 0xDC, { "Backslash" } },
            { 0xDD, { "RightBracket" } },
            { 0xDE, { "Quote" } },
        };
        /* Letters and digits are their own ASCII codes. */
        static const char* kLetters[26] = {
            "A","B","C","D","E","F","G","H","I","J","K","L","M",
            "N","O","P","Q","R","S","T","U","V","W","X","Y","Z" };
        static const char* kDigits[10] = {
            "0","1","2","3","4","5","6","7","8","9" };
        static const char* kNum[10] = {
            "Num0","Num1","Num2","Num3","Num4",
            "Num5","Num6","Num7","Num8","Num9" };
        static const char* kF[24] = {
            "F1","F2","F3","F4","F5","F6","F7","F8","F9","F10",
            "F11","F12","F13","F14","F15","F16","F17","F18","F19",
            "F20","F21","F22","F23","F24" };
        for(int i = 0; i < 26; ++i)
        {
            t.push_back({ uint32_t('A' + i), { kLetters[i] } });
        }
        for(int i = 0; i < 10; ++i)
        {
            t.push_back({ uint32_t('0' + i), { kDigits[i] } });
            t.push_back({ uint32_t(0x60 + i), { kNum[i] } });
        }
        for(int i = 0; i < 24; ++i)
        {
            t.push_back({ uint32_t(0x70 + i), { kF[i] } });
        }
        return t;
    }();
    return table;
}

std::string Lower(std::string s)
{
    for(char& c : s)
    {
        c = char(std::tolower((unsigned char)c));
    }
    return s;
}

std::string Strip(const std::string& s)
{
    std::string out;
    for(char c : s)
    {
        if(!std::isspace((unsigned char)c))
        {
            out.push_back(c);
        }
    }
    return out;
}

bool ModifierBit(const std::string& lower, uint32_t& bit)
{
    if(lower == "ctrl" || lower == "control")
    {
        bit = kModCtrl;
        return true;
    }
    if(lower == "alt")
    {
        bit = kModAlt;
        return true;
    }
    if(lower == "shift")
    {
        bit = kModShift;
        return true;
    }
    if(lower == "win" || lower == "meta" || lower == "super")
    {
        bit = kModWin;
        return true;
    }
    return false;
}

bool KeyByName(const std::string& lower, uint32_t& vk)
{
    for(const KeyName& k : KeyTable())
    {
        for(const char* n : k.names)
        {
            if(Lower(n) == lower)
            {
                vk = k.vk;
                return true;
            }
        }
    }
    return false;
}

const char* KeyCanonical(uint32_t vk)
{
    for(const KeyName& k : KeyTable())
    {
        if(k.vk == vk)
        {
            return k.names.front();
        }
    }
    return nullptr;
}

constexpr double kMinStep = 0.01;
constexpr double kMaxStep = 0.5;

} /* anonymous namespace */

/*---------------------------------------------------------*\
||| Chords                                                  ||
\*---------------------------------------------------------*/

bool ParseChord(const std::string& text, Chord& out, std::string& err)
{
    const std::string s = Strip(text);
    if(s.empty())
    {
        err = "empty chord";
        return false;
    }
    Chord c;
    bool  have_key = false;
    size_t start = 0;
    while(true)
    {
        const size_t plus = s.find('+', start);
        const std::string part =
            s.substr(start, plus == std::string::npos
                                ? std::string::npos
                                : plus - start);
        if(part.empty())
        {
            err = "chord '" + text + "' has an empty part";
            return false;
        }
        const std::string lower = Lower(part);
        uint32_t bit = 0;
        uint32_t vk  = 0;
        if(ModifierBit(lower, bit))
        {
            if(c.mods & bit)
            {
                err = "chord '" + text + "' repeats '" + part + "'";
                return false;
            }
            c.mods |= bit;
        }
        else if(KeyByName(lower, vk))
        {
            if(have_key)
            {
                err = "chord '" + text + "' names more than one key";
                return false;
            }
            have_key = true;
            c.vk     = vk;
        }
        else
        {
            err = "chord '" + text + "': unknown key '" + part + "'";
            return false;
        }
        if(plus == std::string::npos)
        {
            break;
        }
        start = plus + 1;
    }
    if(!have_key)
    {
        err = "chord '" + text + "' has no key (only modifiers)";
        return false;
    }
    out = c;
    return true;
}

std::string FormatChord(const Chord& c)
{
    const char* key = KeyCanonical(c.vk);
    if(key == nullptr)
    {
        return std::string();
    }
    std::string s;
    if(c.mods & kModCtrl)  s += "Ctrl+";
    if(c.mods & kModAlt)   s += "Alt+";
    if(c.mods & kModShift) s += "Shift+";
    if(c.mods & kModWin)   s += "Win+";
    return s + key;
}

/*---------------------------------------------------------*\
||| Actions                                                 ||
\*---------------------------------------------------------*/

const char* HotkeyActionName(HotkeyAction a)
{
    switch(a)
    {
    case HotkeyAction::NextWorkspace:  return "next_workspace";
    case HotkeyAction::PrevWorkspace:  return "prev_workspace";
    case HotkeyAction::BrightnessUp:   return "brightness_up";
    case HotkeyAction::BrightnessDown: return "brightness_down";
    case HotkeyAction::LightsToggle:   return "lights_toggle";
    case HotkeyAction::PauseToggle:    return "pause_toggle";
    }
    return "?";
}

const std::vector<HotkeyAction>& AllHotkeyActions()
{
    static const std::vector<HotkeyAction> all = {
        HotkeyAction::NextWorkspace,  HotkeyAction::PrevWorkspace,
        HotkeyAction::BrightnessUp,   HotkeyAction::BrightnessDown,
        HotkeyAction::LightsToggle,   HotkeyAction::PauseToggle,
    };
    return all;
}

bool HotkeyActionFromName(const std::string& s, HotkeyAction& out)
{
    for(HotkeyAction a : AllHotkeyActions())
    {
        if(s == HotkeyActionName(a))
        {
            out = a;
            return true;
        }
    }
    return false;
}

std::string ProposedChord(HotkeyAction a)
{
    /* Plan R-2 suggestion: Ctrl+Alt+Shift + PgUp/PgDn
       (workspace), Up/Down (brightness), End (lights), Pause
       (hold). Proposed, never registered by default. */
    switch(a)
    {
    case HotkeyAction::NextWorkspace:  return "Ctrl+Alt+Shift+PgDn";
    case HotkeyAction::PrevWorkspace:  return "Ctrl+Alt+Shift+PgUp";
    case HotkeyAction::BrightnessUp:   return "Ctrl+Alt+Shift+Up";
    case HotkeyAction::BrightnessDown: return "Ctrl+Alt+Shift+Down";
    case HotkeyAction::LightsToggle:   return "Ctrl+Alt+Shift+End";
    case HotkeyAction::PauseToggle:    return "Ctrl+Alt+Shift+Pause";
    }
    return std::string();
}

bool ValidateHotkeys(const std::vector<HotkeyBinding>& in,
                     std::vector<HotkeyBinding>& out,
                     std::string& err)
{
    if(in.size() > kMaxHotkeys)
    {
        err = "at most " + std::to_string(kMaxHotkeys) +
              " hotkeys (got " + std::to_string(in.size()) + ")";
        return false;
    }
    std::vector<HotkeyBinding> result;
    std::set<std::pair<uint32_t, uint32_t>> seen;
    for(size_t i = 0; i < in.size(); ++i)
    {
        const HotkeyBinding& b = in[i];
        const std::string where = "hotkeys[" + std::to_string(i) + "]";
        HotkeyAction a;
        if(!HotkeyActionFromName(b.action, a))
        {
            err = where + ": unknown action '" + b.action + "'";
            return false;
        }
        Chord c;
        std::string perr;
        if(!ParseChord(b.chord, c, perr))
        {
            err = where + ": " + perr;
            return false;
        }
        if((c.mods & (kModCtrl | kModAlt | kModWin)) == 0)
        {
            err = where + ": chord '" + b.chord +
                  "' needs at least one of Ctrl, Alt or Win";
            return false;
        }
        if(!seen.insert({ c.mods, c.vk }).second)
        {
            err = where + ": duplicate chord '" + FormatChord(c) + "'";
            return false;
        }
        result.push_back({ b.action, FormatChord(c) });
    }
    out = std::move(result);
    return true;
}

/*---------------------------------------------------------*\
||| R-4 dimming rules                                        ||
\*---------------------------------------------------------*/

LocalTime SystemLocalTime()
{
    const std::time_t now = std::time(nullptr);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &now);
#else
    localtime_r(&now, &tm);
#endif
    LocalTime t;
    t.year    = tm.tm_year + 1900;
    t.month   = tm.tm_mon + 1;
    t.day     = tm.tm_mday;
    t.weekday = tm.tm_wday;
    t.hour    = tm.tm_hour;
    t.minute  = tm.tm_min;
    t.second  = tm.tm_sec;
    return t;
}

bool ScheduleWindowActive(const DimWindow& w, const LocalTime& t)
{
    auto on_day = [&w](int wd)
    {
        return w.days.empty() ||
               std::find(w.days.begin(), w.days.end(), wd) !=
                   w.days.end();
    };
    const int m = t.hour * 60 + t.minute;
    const int today = ((t.weekday % 7) + 7) % 7;
    if(w.start_min < w.end_min)
    {
        return on_day(today) && m >= w.start_min && m < w.end_min;
    }
    if(w.start_min > w.end_min)
    {
        /* Evening half belongs to today; the morning half to
           the window that started yesterday. */
        const int yesterday = (today + 6) % 7;
        return (on_day(today) && m >= w.start_min) ||
               (on_day(yesterday) && m < w.end_min);
    }
    return false;   /* empty window */
}

DimResult EvaluateDimRules(const DimmingConfig& c, const DimInputs& in)
{
    DimResult r;
    for(const DimWindow& w : c.schedule)
    {
        if(ScheduleWindowActive(w, in.wall))
        {
            r.schedule = r.schedule ? std::min(*r.schedule, w.level)
                                    : w.level;
        }
    }
    if(c.lock.enabled && in.locked)
    {
        r.lock = c.lock.level;
    }
    if(c.idle.enabled && in.idle_s >= 0.0 &&
       in.idle_s >= double(c.idle.after_s) &&
       !(c.idle.skip_fullscreen && in.fullscreen))
    {
        r.idle = c.idle.level;
    }
    return r;
}

namespace
{

constexpr uint32_t kMinIdleS  = 30;
constexpr uint32_t kMaxIdleS  = 86400;
constexpr uint32_t kMaxRampMs = 10000;

std::string HhMm(int minutes)
{
    char buf[8];
    std::snprintf(buf, sizeof(buf), "%02d:%02d", minutes / 60,
                  minutes % 60);
    return buf;
}

bool ParseHhMm(const nlohmann::json& v, int& out)
{
    if(!v.is_string())
    {
        return false;
    }
    const std::string s = v.get<std::string>();
    if(s.size() != 5 || s[2] != ':' ||
       !std::isdigit((unsigned char)s[0]) ||
       !std::isdigit((unsigned char)s[1]) ||
       !std::isdigit((unsigned char)s[3]) ||
       !std::isdigit((unsigned char)s[4]))
    {
        return false;
    }
    const int h = (s[0] - '0') * 10 + (s[1] - '0');
    const int m = (s[3] - '0') * 10 + (s[4] - '0');
    if(h > 23 || m > 59)
    {
        return false;
    }
    out = h * 60 + m;
    return true;
}

bool Level(const nlohmann::json& v, float& out)
{
    if(!v.is_number())
    {
        return false;
    }
    const double d = v.get<double>();
    if(!(d >= 0.0 && d <= 1.0))
    {
        return false;
    }
    out = float(d);
    return true;
}

double Round4(double v)
{
    return std::round(v * 10000.0) / 10000.0;
}

} /* anonymous namespace */

nlohmann::json DimmingConfigToJson(const DimmingConfig& c)
{
    nlohmann::json sched = nlohmann::json::array();
    for(const DimWindow& w : c.schedule)
    {
        sched.push_back({
            { "days",  w.days },
            { "start", HhMm(w.start_min) },
            { "end",   HhMm(w.end_min) },
            { "level", Round4(w.level) },
        });
    }
    return nlohmann::json::object({
        { "schedule", std::move(sched) },
        { "lock", { { "enabled", c.lock.enabled },
                    { "level",   Round4(c.lock.level) } } },
        { "idle", { { "enabled",         c.idle.enabled },
                    { "after_s",         c.idle.after_s },
                    { "level",           Round4(c.idle.level) },
                    { "skip_fullscreen", c.idle.skip_fullscreen } } },
        { "ramp_ms",    c.ramp_ms },
        { "restore_ms", c.restore_ms },
    });
}

bool DimmingConfigFromJson(const nlohmann::json& j, DimmingConfig& out,
                           std::string& err)
{
    if(!j.is_object())
    {
        err = "dimming must be an object";
        return false;
    }
    DimmingConfig c;
    const auto sit = j.find("schedule");
    if(sit != j.end() && !sit->is_null())
    {
        if(!sit->is_array())
        {
            err = "schedule must be an array of windows";
            return false;
        }
        if(sit->size() > kMaxDimWindows)
        {
            err = "at most " + std::to_string(kMaxDimWindows) +
                  " schedule windows";
            return false;
        }
        for(size_t i = 0; i < sit->size(); ++i)
        {
            const nlohmann::json& e = (*sit)[i];
            const std::string where =
                "schedule[" + std::to_string(i) + "]";
            if(!e.is_object())
            {
                err = where + " must be an object";
                return false;
            }
            DimWindow w;
            if(!ParseHhMm(e.contains("start") ? e["start"]
                                              : nlohmann::json(),
                          w.start_min) ||
               !ParseHhMm(e.contains("end") ? e["end"]
                                            : nlohmann::json(),
                          w.end_min))
            {
                err = where + ": start and end are \"HH:MM\" "
                      "(00:00 .. 23:59)";
                return false;
            }
            if(w.start_min == w.end_min)
            {
                err = where + ": start and end must differ";
                return false;
            }
            if(!Level(e.contains("level") ? e["level"]
                                          : nlohmann::json(),
                      w.level))
            {
                err = where + ": level is a number in [0, 1]";
                return false;
            }
            if(e.contains("days") && !e["days"].is_null())
            {
                if(!e["days"].is_array())
                {
                    err = where + ": days is an array of 0..6 "
                          "(Sunday = 0)";
                    return false;
                }
                for(const nlohmann::json& d : e["days"])
                {
                    if(!d.is_number_integer() || d.get<int>() < 0 ||
                       d.get<int>() > 6)
                    {
                        err = where + ": days is an array of 0..6 "
                              "(Sunday = 0)";
                        return false;
                    }
                    const int v = d.get<int>();
                    if(std::find(w.days.begin(), w.days.end(), v) ==
                       w.days.end())
                    {
                        w.days.push_back(v);
                    }
                }
                std::sort(w.days.begin(), w.days.end());
            }
            c.schedule.push_back(std::move(w));
        }
    }

    const auto lit = j.find("lock");
    if(lit != j.end() && !lit->is_null())
    {
        if(!lit->is_object())
        {
            err = "lock must be {enabled, level}";
            return false;
        }
        if(lit->contains("enabled"))
        {
            if(!(*lit)["enabled"].is_boolean())
            {
                err = "lock.enabled is a bool";
                return false;
            }
            c.lock.enabled = (*lit)["enabled"].get<bool>();
        }
        if(lit->contains("level") &&
           !Level((*lit)["level"], c.lock.level))
        {
            err = "lock.level is a number in [0, 1]";
            return false;
        }
    }

    const auto iit = j.find("idle");
    if(iit != j.end() && !iit->is_null())
    {
        if(!iit->is_object())
        {
            err = "idle must be {enabled, after_s, level, "
                  "skip_fullscreen}";
            return false;
        }
        const nlohmann::json& o = *iit;
        if(o.contains("enabled"))
        {
            if(!o["enabled"].is_boolean())
            {
                err = "idle.enabled is a bool";
                return false;
            }
            c.idle.enabled = o["enabled"].get<bool>();
        }
        if(o.contains("after_s"))
        {
            if(!o["after_s"].is_number_integer() ||
               o["after_s"].get<int64_t>() < kMinIdleS ||
               o["after_s"].get<int64_t>() > kMaxIdleS)
            {
                err = "idle.after_s is whole seconds in [30, 86400]";
                return false;
            }
            c.idle.after_s = uint32_t(o["after_s"].get<int64_t>());
        }
        if(o.contains("level") && !Level(o["level"], c.idle.level))
        {
            err = "idle.level is a number in [0, 1]";
            return false;
        }
        if(o.contains("skip_fullscreen"))
        {
            if(!o["skip_fullscreen"].is_boolean())
            {
                err = "idle.skip_fullscreen is a bool";
                return false;
            }
            c.idle.skip_fullscreen = o["skip_fullscreen"].get<bool>();
        }
    }

    for(const char* k : { "ramp_ms", "restore_ms" })
    {
        const auto it = j.find(k);
        if(it == j.end() || it->is_null())
        {
            continue;
        }
        if(!it->is_number_integer() || it->get<int64_t>() < 0 ||
           it->get<int64_t>() > kMaxRampMs)
        {
            err = std::string(k) + " is milliseconds in [0, 10000]";
            return false;
        }
        (std::string(k) == "ramp_ms" ? c.ramp_ms : c.restore_ms) =
            uint32_t(it->get<int64_t>());
    }
    out = std::move(c);
    return true;
}

/*---------------------------------------------------------*\
||| AutomationConfig <-> JSON                               ||
\*---------------------------------------------------------*/

nlohmann::json AutomationConfigToJson(const AutomationConfig& c)
{
    nlohmann::json j = nlohmann::json::object();
    if(c.extensions.is_object())
    {
        for(auto it = c.extensions.begin(); it != c.extensions.end();
            ++it)
        {
            j[it.key()] = it.value();
        }
    }
    nlohmann::json hk = nlohmann::json::array();
    for(const HotkeyBinding& b : c.hotkeys)
    {
        hk.push_back({ { "action", b.action }, { "chord", b.chord } });
    }
    j["hotkeys"]         = std::move(hk);
    j["cycle"]           = c.cycle;
    j["brightness_step"] =
        std::round(c.brightness_step * 10000.0) / 10000.0;
    if(c.dimming)
    {
        j["dimming"] = DimmingConfigToJson(*c.dimming);
    }
    return j;
}

AutomationConfig AutomationConfigFromJson(
    const nlohmann::json& j, std::vector<std::string>* warnings)
{
    auto warn = [warnings](const std::string& w)
    {
        if(warnings)
        {
            warnings->push_back("automation: " + w);
        }
    };
    AutomationConfig c;
    if(!j.is_object())
    {
        warn("expected an object — ignored");
        return c;
    }
    for(auto it = j.begin(); it != j.end(); ++it)
    {
        const std::string& k = it.key();
        const nlohmann::json& v = it.value();
        if(k == "hotkeys")
        {
            if(!v.is_array())
            {
                warn("hotkeys: expected an array — ignored");
                continue;
            }
            for(const nlohmann::json& e : v)
            {
                if(!e.is_object() ||
                   !e.contains("action") || !e["action"].is_string() ||
                   !e.contains("chord")  || !e["chord"].is_string())
                {
                    warn("hotkeys: dropped a malformed entry");
                    continue;
                }
                if(c.hotkeys.size() >= kMaxHotkeys)
                {
                    warn("hotkeys: more than " +
                         std::to_string(kMaxHotkeys) +
                         " entries — the rest dropped");
                    break;
                }
                c.hotkeys.push_back({ e["action"].get<std::string>(),
                                      e["chord"].get<std::string>() });
            }
        }
        else if(k == "cycle")
        {
            if(!v.is_array())
            {
                warn("cycle: expected an array of ids — ignored");
                continue;
            }
            for(const nlohmann::json& e : v)
            {
                if(e.is_string())
                {
                    c.cycle.push_back(e.get<std::string>());
                }
                else
                {
                    warn("cycle: dropped a non-string id");
                }
            }
        }
        else if(k == "brightness_step")
        {
            if(v.is_number() && v.get<double>() >= kMinStep &&
               v.get<double>() <= kMaxStep)
            {
                c.brightness_step = v.get<double>();
            }
            else
            {
                warn("brightness_step: expected 0.01 .. 0.5 — "
                     "default kept");
            }
        }
        else if(k == "dimming")
        {
            DimmingConfig d;
            std::string derr;
            if(DimmingConfigFromJson(v, d, derr))
            {
                c.dimming = std::move(d);
            }
            else
            {
                warn("dimming: " + derr + " — ignored (no dimming)");
            }
        }
        else
        {
            c.extensions[k] = v;
        }
    }
    return c;
}

const char* HotkeyStateName(HotkeyState s)
{
    switch(s)
    {
    case HotkeyState::Ok:          return "ok";
    case HotkeyState::InUse:       return "in_use";
    case HotkeyState::Invalid:     return "invalid";
    case HotkeyState::Unavailable: return "unavailable";
    }
    return "?";
}

/*---------------------------------------------------------*\
||| Automation                                              ||
\*---------------------------------------------------------*/

Automation::Automation(Deps deps)
    : deps_(std::move(deps))
{
}

void Automation::Log(const std::string& line) const
{
    if(deps_.log)
    {
        deps_.log("automation: " + line);
    }
}

void Automation::Notify()
{
    if(deps_.changed)
    {
        deps_.changed();
    }
}

void Automation::RegisterLocked()
{
    /* Build the request set from the stored bindings. Ids are
       unique per generation so a WM_HOTKEY queued against the
       previous set can never run the new set's action. Win32
       accepts 0x0000..0xBFFF; 1000 generations x 16 slots
       stays far below. */
    std::vector<Live>          live;
    std::vector<HotkeyRequest> requests;
    {
        std::lock_guard<std::mutex> g(mutex_);
        generation_ = (generation_ + 1) % 1000;
        for(size_t i = 0; i < config_.hotkeys.size(); ++i)
        {
            Live l;
            l.binding = config_.hotkeys[i];
            std::vector<HotkeyBinding> one;
            std::string verr;
            Chord c;
            if(!ValidateHotkeys({ l.binding }, one, verr) ||
               !ParseChord(one[0].chord, c, verr))
            {
                l.state  = HotkeyState::Invalid;
                l.detail = verr;
            }
            else
            {
                l.id = int(generation_ * kMaxHotkeys + i + 1);
                requests.push_back({ l.id, c.mods, c.vk });
                l.state = HotkeyState::Unavailable;
                if(!deps_.registrar)
                {
                    l.detail = "no hotkey registrar on this engine";
                }
            }
            live.push_back(std::move(l));
        }
    }

    /* The platform round trip runs without mutex_ (PostHotkey
       from the registrar thread must never wait on it). */
    std::vector<HotkeyResult> results;
    if(deps_.registrar)
    {
        results = deps_.registrar->Apply(requests);
    }
    for(Live& l : live)
    {
        if(l.id == 0)
        {
            continue;
        }
        for(const HotkeyResult& r : results)
        {
            if(r.id == l.id)
            {
                l.state  = r.state;
                l.detail = r.detail;
            }
        }
        if(l.state != HotkeyState::Ok)
        {
            Log("hotkey " + l.binding.chord + " (" + l.binding.action +
                ") not active: " + HotkeyStateName(l.state) +
                (l.detail.empty() ? "" : " — " + l.detail));
            /* Never fires: only "ok" bindings map presses. */
        }
    }
    {
        std::lock_guard<std::mutex> g(mutex_);
        live_ = std::move(live);
        pressed_.clear();
    }
}

void Automation::Restore(const AutomationConfig& config)
{
    {
        std::lock_guard<std::mutex> apply(apply_mutex_);
        {
            std::lock_guard<std::mutex> g(mutex_);
            config_ = config;
        }
        RegisterLocked();
    }
    Notify();
}

bool Automation::SetHotkeys(
    const std::vector<HotkeyBinding>& bindings,
    const std::optional<std::vector<std::string>>& cycle,
    const std::optional<double>& step,
    std::string& err)
{
    std::vector<HotkeyBinding> canonical;
    if(!ValidateHotkeys(bindings, canonical, err))
    {
        return false;
    }
    if(step && !(*step >= kMinStep && *step <= kMaxStep))
    {
        err = "brightness_step must be within [0.01, 0.5]";
        return false;
    }
    if(cycle && cycle->size() > 64)
    {
        err = "cycle holds at most 64 workspace ids";
        return false;
    }
    {
        std::lock_guard<std::mutex> apply(apply_mutex_);
        AutomationConfig next;
        {
            std::lock_guard<std::mutex> g(mutex_);
            next = config_;
        }
        next.hotkeys = std::move(canonical);
        if(cycle)
        {
            next.cycle = *cycle;
        }
        if(step)
        {
            next.brightness_step = *step;
        }
        if(deps_.persist)
        {
            std::string serr;
            if(!deps_.persist(next, serr))
            {
                err = "could not save the hotkeys: " + serr;
                return false;
            }
        }
        {
            std::lock_guard<std::mutex> g(mutex_);
            config_ = std::move(next);
        }
        RegisterLocked();
    }
    Notify();
    return true;
}

AutomationConfig Automation::Config() const
{
    std::lock_guard<std::mutex> g(mutex_);
    return config_;
}

void Automation::PostHotkey(int id)
{
    std::lock_guard<std::mutex> g(mutex_);
    /* Bounded: a stuck owner loop never grows this. */
    if(pressed_.size() < 64)
    {
        pressed_.push_back(id);
    }
}

void Automation::Poll(TimePoint now)
{
    std::vector<HotkeyAction> run;
    {
        std::lock_guard<std::mutex> g(mutex_);
        for(int id : pressed_)
        {
            for(const Live& l : live_)
            {
                HotkeyAction a;
                if(l.id == id && l.state == HotkeyState::Ok &&
                   HotkeyActionFromName(l.binding.action, a))
                {
                    run.push_back(a);
                }
            }
        }
        pressed_.clear();
    }
    for(HotkeyAction a : run)
    {
        RunAction(a, now);
    }

    /* Trailing edge of a workspace burst: the latest target,
       once, if it differs from what is already loading. */
    if(in_burst_ && now >= burst_until_)
    {
        in_burst_ = false;
        if(pending_ && !cursor_.empty())
        {
            pending_ = false;
            std::string err;
            if(deps_.enqueue_reload &&
               !deps_.enqueue_reload(cursor_, err))
            {
                Log("workspace switch to '" + cursor_ +
                    "' refused: " + err);
            }
            fired_ = cursor_;
        }
        pending_ = false;
    }
}

void Automation::StepWorkspace(int dir, TimePoint now)
{
    std::vector<std::string> ids =
        deps_.list_workspaces ? deps_.list_workspaces()
                              : std::vector<std::string>{};
    std::vector<std::string> cycle;
    {
        std::lock_guard<std::mutex> g(mutex_);
        cycle = config_.cycle;
    }
    if(!cycle.empty())
    {
        std::vector<std::string> order;
        for(const std::string& c : cycle)
        {
            if(std::find(ids.begin(), ids.end(), c) != ids.end() &&
               std::find(order.begin(), order.end(), c) == order.end())
            {
                order.push_back(c);
            }
        }
        ids = std::move(order);
    }
    if(ids.empty())
    {
        Log("workspace hotkey: no workspace to switch to");
        return;
    }

    const bool burst = in_burst_ && now < burst_until_;
    const std::string base =
        burst ? cursor_
              : (deps_.active_workspace ? deps_.active_workspace()
                                        : std::string());
    const auto it = std::find(ids.begin(), ids.end(), base);
    std::string target;
    if(it == ids.end())
    {
        target = dir > 0 ? ids.front() : ids.back();
    }
    else
    {
        const long n = long(ids.size());
        const long i = long(it - ids.begin());
        target = ids[size_t(((i + dir) % n + n) % n)];
    }
    cursor_      = target;
    burst_until_ = now + kWorkspaceDebounce;
    if(!burst)
    {
        /* Leading edge: reload at once. */
        in_burst_ = true;
        pending_  = false;
        fired_    = target;
        std::string err;
        if(deps_.enqueue_reload && !deps_.enqueue_reload(target, err))
        {
            Log("workspace switch to '" + target + "' refused: " + err);
        }
        return;
    }
    pending_ = target != fired_;
}

void Automation::RunAction(HotkeyAction a, TimePoint now)
{
    OutputControl* oc = deps_.output;
    double step = 0.10;
    {
        std::lock_guard<std::mutex> g(mutex_);
        step = config_.brightness_step;
    }
    switch(a)
    {
    case HotkeyAction::NextWorkspace:
        StepWorkspace(+1, now);
        return;
    case HotkeyAction::PrevWorkspace:
        StepWorkspace(-1, now);
        return;
    case HotkeyAction::BrightnessUp:
    case HotkeyAction::BrightnessDown:
        if(oc)
        {
            const double m = oc->State().master;
            double v = a == HotkeyAction::BrightnessUp ? m + step
                                                       : m - step;
            /* Snap to the step grid's float noise. */
            v = std::round(v * 10000.0) / 10000.0;
            oc->SetMaster(float(std::clamp(v, 0.0, 1.0)));
        }
        return;
    case HotkeyAction::LightsToggle:
        if(oc)
        {
            oc->SetBlackout(!oc->State().blackout);
        }
        return;
    case HotkeyAction::PauseToggle:
        if(oc)
        {
            oc->SetHold(!oc->State().hold);
        }
        return;
    }
}

nlohmann::json Automation::ToJson() const
{
    std::lock_guard<std::mutex> g(mutex_);
    nlohmann::json hk = nlohmann::json::array();
    for(const Live& l : live_)
    {
        nlohmann::json e = {
            { "action", l.binding.action },
            { "chord",  l.binding.chord },
            { "state",  HotkeyStateName(l.state) },
        };
        if(!l.detail.empty())
        {
            e["detail"] = l.detail;
        }
        hk.push_back(std::move(e));
    }
    nlohmann::json actions  = nlohmann::json::array();
    nlohmann::json proposed = nlohmann::json::object();
    for(HotkeyAction a : AllHotkeyActions())
    {
        actions.push_back(HotkeyActionName(a));
        proposed[HotkeyActionName(a)] = ProposedChord(a);
    }
    return nlohmann::json::object({
        { "hotkeys",         std::move(hk) },
        { "actions",         std::move(actions) },
        { "proposed",        std::move(proposed) },
        { "cycle",           config_.cycle },
        { "brightness_step",
          std::round(config_.brightness_step * 10000.0) / 10000.0 },
        { "registrar",       deps_.registrar != nullptr },
        { "max_hotkeys",     kMaxHotkeys },
    });
}

} /* namespace openrgb2 */
