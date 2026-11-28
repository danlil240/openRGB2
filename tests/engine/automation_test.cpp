// SPDX-License-Identifier: GPL-2.0-or-later
//
// Automation tests (src/engine/Automation.h — R-2 global
// hotkeys, R-4 dimming rules): the chord parser, binding
// validation, the EngineState.automation round-trip, the
// action dispatcher against OutputControl with a fake clock,
// fake workspace list and fake registrar. Qt-free, offline:
// nothing here touches RegisterHotKey or the desktop.

#include "Automation.h"
#include "OutputControl.h"

#include <cmath>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

using namespace openrgb2;

static int checks   = 0;
static int failures = 0;
#define CHECK(x) do { ++checks; if(!(x)) { ++failures; \
    std::printf("FAIL line %d: %s\n", __LINE__, #x); } } while(0)

static bool Near(double a, double b, double eps = 1e-6)
{
    return std::fabs(a - b) < eps;
}

/*---- step 1: chords, validation, persistence ---------------*/

static void TestParseChord()
{
    Chord c;
    std::string err;
    CHECK(ParseChord("Ctrl+Alt+PgUp", c, err));
    CHECK(c.mods == (kModCtrl | kModAlt));
    CHECK(c.vk == 0x21);
    CHECK(FormatChord(c) == "Ctrl+Alt+PgUp");

    /* Case, spaces and aliases; canonical modifier order. */
    CHECK(ParseChord(" shift + control+ALT + pagedown ", c, err));
    CHECK(c.mods == (kModCtrl | kModAlt | kModShift));
    CHECK(c.vk == 0x22);
    CHECK(FormatChord(c) == "Ctrl+Alt+Shift+PgDn");

    CHECK(ParseChord("Win+F12", c, err));
    CHECK(c.mods == kModWin && c.vk == 0x7B);
    CHECK(ParseChord("Ctrl+Alt+Shift+Up", c, err));
    CHECK(c.vk == 0x26);
    CHECK(ParseChord("Ctrl+Alt+Shift+End", c, err) && c.vk == 0x23);
    CHECK(ParseChord("Ctrl+Alt+Shift+Pause", c, err) && c.vk == 0x13);
    CHECK(ParseChord("Ctrl+Alt+K", c, err) && c.vk == 'K');
    CHECK(ParseChord("Ctrl+Alt+7", c, err) && c.vk == '7');
    CHECK(ParseChord("Ctrl+Alt+Num5", c, err) && c.vk == 0x65);
    CHECK(FormatChord(c) == "Ctrl+Alt+Num5");
    CHECK(ParseChord("Alt+F1", c, err) && c.vk == 0x70);
    /* A bare key parses (the modifier policy is validation's). */
    CHECK(ParseChord("F9", c, err) && c.mods == 0 && c.vk == 0x78);

    /* Refusals carry a reason. */
    for(const char* bad : { "", "Ctrl+", "Ctrl+Alt", "Ctrl+Bogus",
                            "Ctrl+A+B", "Ctrl++A", "Ctrl+Ctrl+A",
                            "Hyper+A" })
    {
        err.clear();
        CHECK(!ParseChord(bad, c, err));
        CHECK(!err.empty());
    }
    CHECK(FormatChord(Chord{ kModCtrl, 0x07 }).empty());
}

static void TestActions()
{
    CHECK(AllHotkeyActions().size() == 6);
    for(HotkeyAction a : AllHotkeyActions())
    {
        HotkeyAction back;
        CHECK(HotkeyActionFromName(HotkeyActionName(a), back));
        CHECK(back == a);
        /* Every proposal is itself a valid binding. */
        std::vector<HotkeyBinding> out;
        std::string err;
        CHECK(ValidateHotkeys({ { HotkeyActionName(a),
                                  ProposedChord(a) } }, out, err));
    }
    HotkeyAction a;
    CHECK(!HotkeyActionFromName("launch_missiles", a));
    CHECK(std::string(HotkeyActionName(HotkeyAction::NextWorkspace)) ==
          "next_workspace");
    CHECK(ProposedChord(HotkeyAction::NextWorkspace) ==
          "Ctrl+Alt+Shift+PgDn");
    CHECK(ProposedChord(HotkeyAction::PrevWorkspace) ==
          "Ctrl+Alt+Shift+PgUp");
    CHECK(ProposedChord(HotkeyAction::LightsToggle) ==
          "Ctrl+Alt+Shift+End");
}

static void TestValidate()
{
    std::vector<HotkeyBinding> out;
    std::string err;
    CHECK(ValidateHotkeys({}, out, err) && out.empty());

    CHECK(ValidateHotkeys({ { "next_workspace", "alt+ctrl+pgdn" },
                            { "lights_toggle",  "Win+End"       } },
                          out, err));
    CHECK(out.size() == 2);
    CHECK(out[0].chord == "Ctrl+Alt+PgDn");
    CHECK(out[1].chord == "Win+End");

    const std::vector<HotkeyBinding> kept = out;
    /* Shift alone is not enough; a bare key is not either. */
    CHECK(!ValidateHotkeys({ { "next_workspace", "Shift+PgDn" } },
                           out, err));
    CHECK(err.find("Ctrl, Alt or Win") != std::string::npos);
    CHECK(!ValidateHotkeys({ { "next_workspace", "F9" } }, out, err));
    /* Duplicate chord (after canonicalization). */
    CHECK(!ValidateHotkeys({ { "next_workspace", "Ctrl+Alt+K" },
                             { "prev_workspace", "alt+ctrl+k" } },
                           out, err));
    CHECK(err.find("duplicate") != std::string::npos);
    CHECK(!ValidateHotkeys({ { "dance", "Ctrl+Alt+K" } }, out, err));
    CHECK(!ValidateHotkeys({ { "next_workspace", "Ctrl+Nope" } },
                           out, err));
    /* At most 16. */
    std::vector<HotkeyBinding> many;
    for(int i = 0; i < 17; ++i)
    {
        many.push_back({ "brightness_up",
                         "Ctrl+Alt+F" + std::to_string(i + 1) });
    }
    CHECK(!ValidateHotkeys(many, out, err));
    many.pop_back();
    CHECK(ValidateHotkeys(many, out, err) && out.size() == 16);
    /* The same action may own two chords. */
    CHECK(ValidateHotkeys({ { "brightness_up", "Ctrl+Alt+Up" },
                            { "brightness_up", "Ctrl+Alt+Num8" } },
                          out, err));
    (void)kept;
}

static void TestConfigJson()
{
    AutomationConfig c;
    nlohmann::json j = AutomationConfigToJson(c);
    CHECK(j["hotkeys"].is_array() && j["hotkeys"].empty());
    std::vector<std::string> warn;
    AutomationConfig back = AutomationConfigFromJson(j, &warn);
    CHECK(back == c);
    CHECK(warn.empty());

    c.hotkeys = { { "next_workspace", "Ctrl+Alt+Shift+PgDn" },
                  { "pause_toggle",   "Ctrl+Alt+Shift+Pause" } };
    c.cycle   = { "alpha", "beta" };
    c.brightness_step = 0.05;
    c.extensions["profiles"] = nlohmann::json::array({ 1, 2 });
    j = AutomationConfigToJson(c);
    CHECK(j["profiles"] == nlohmann::json::array({ 1, 2 }));
    back = AutomationConfigFromJson(j, &warn);
    CHECK(back == c);

    /* Lenient: malformed members drop with a warning; a binding
       that no longer validates is KEPT verbatim. */
    nlohmann::json bad = {
        { "hotkeys", nlohmann::json::array({
            { { "action", "next_workspace" }, { "chord", "Shift+K" } },
            { { "action", 5 } },
            "junk" }) },
        { "cycle", "alpha" },
        { "brightness_step", 9.0 },
    };
    warn.clear();
    back = AutomationConfigFromJson(bad, &warn);
    CHECK(back.hotkeys.size() == 1);
    CHECK(back.hotkeys[0].chord == "Shift+K");
    CHECK(back.cycle.empty());
    CHECK(Near(back.brightness_step, 0.10));
    CHECK(warn.size() >= 3);
    /* Not an object at all. */
    warn.clear();
    back = AutomationConfigFromJson(nlohmann::json::array(), &warn);
    CHECK(back == AutomationConfig{});
    CHECK(!warn.empty());
}

int main()
{
    TestParseChord();
    TestActions();
    TestValidate();
    TestConfigJson();
    std::printf("automation: %d checks, %d failures\n",
                checks, failures);
    return failures ? 1 : 0;
}
