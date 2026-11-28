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

/*---- step 2: registration + action dispatcher -------------*/

/* Records every Apply; reports in_use for one chosen chord. */
struct FakeRegistrar : IHotkeyRegistrar
{
    std::vector<std::vector<HotkeyRequest>> applies;
    Chord busy{ kModCtrl | kModAlt, 'B' };
    std::vector<HotkeyResult>
        Apply(const std::vector<HotkeyRequest>& set) override
    {
        applies.push_back(set);
        std::vector<HotkeyResult> out;
        for(const HotkeyRequest& r : set)
        {
            HotkeyResult res;
            res.id = r.id;
            if(r.mods == busy.mods && r.vk == busy.vk)
            {
                res.state  = HotkeyState::InUse;
                res.detail = "hotkey already registered";
            }
            else
            {
                res.state = HotkeyState::Ok;
            }
            out.push_back(res);
        }
        return out;
    }
};

struct Rig
{
    OutputControl                  oc;
    std::shared_ptr<FakeRegistrar> reg = std::make_shared<FakeRegistrar>();
    std::vector<std::string>       ids  = { "alpha", "beta", "gamma" };
    std::string                    active = "beta";
    std::vector<std::string>       reloads;
    std::vector<AutomationConfig>  saved;
    bool                           fail_save = false;
    int                            changes = 0;

    Automation::Deps Deps(bool with_registrar = true)
    {
        Automation::Deps d;
        d.output = &oc;
        if(with_registrar)
        {
            d.registrar = reg;
        }
        d.list_workspaces  = [this] { return ids; };
        d.active_workspace = [this] { return active; };
        d.enqueue_reload =
            [this](const std::string& id, std::string&)
            {
                reloads.push_back(id);
                return true;
            };
        d.persist =
            [this](const AutomationConfig& c, std::string& err)
            {
                if(fail_save)
                {
                    err = "disk full";
                    return false;
                }
                saved.push_back(c);
                return true;
            };
        d.changed = [this] { ++changes; };
        return d;
    }
};

/* The id the registrar was handed for `chord` in its last set. */
static int IdOf(const FakeRegistrar& r, const std::string& chord)
{
    Chord c;
    std::string err;
    ParseChord(chord, c, err);
    if(r.applies.empty())
    {
        return 0;
    }
    for(const HotkeyRequest& q : r.applies.back())
    {
        if(q.mods == c.mods && q.vk == c.vk)
        {
            return q.id;
        }
    }
    return 0;
}

static const nlohmann::json* Entry(const nlohmann::json& body,
                                   const std::string& chord)
{
    for(const nlohmann::json& e : body["hotkeys"])
    {
        if(e["chord"] == chord)
        {
            return &e;
        }
    }
    return nullptr;
}

static void TestRegistration()
{
    Rig rig;
    Automation a(rig.Deps());
    std::string err;
    /* Nothing bound by default: the registrar sees no set. */
    CHECK(a.Config().hotkeys.empty());
    nlohmann::json body = a.ToJson();
    CHECK(body["hotkeys"].is_array() && body["hotkeys"].empty());
    CHECK(body["registrar"] == true);
    CHECK(body["actions"].size() == 6);
    CHECK(body["proposed"]["next_workspace"] == "Ctrl+Alt+Shift+PgDn");

    CHECK(a.SetHotkeys({ { "brightness_up", "alt+ctrl+up" },
                         { "lights_toggle", "Ctrl+Alt+B" } },
                       std::nullopt, std::nullopt, err));
    CHECK(rig.saved.size() == 1);
    CHECK(rig.saved.back().hotkeys[0].chord == "Ctrl+Alt+Up");
    CHECK(rig.reg->applies.size() == 1 &&
          rig.reg->applies.back().size() == 2);
    CHECK(rig.changes >= 1);
    body = a.ToJson();
    const nlohmann::json* up = Entry(body, "Ctrl+Alt+Up");
    const nlohmann::json* bb = Entry(body, "Ctrl+Alt+B");
    CHECK(up && (*up)["state"] == "ok" && (*up)["action"] == "brightness_up");
    CHECK(bb && (*bb)["state"] == "in_use");
    CHECK(bb && (*bb)["detail"].is_string());

    /* Validation failure: refused, nothing saved or re-registered. */
    CHECK(!a.SetHotkeys({ { "brightness_up", "Shift+Up" } },
                        std::nullopt, std::nullopt, err));
    CHECK(rig.saved.size() == 1 && rig.reg->applies.size() == 1);
    /* Save failure: refused, platform untouched, state kept. */
    rig.fail_save = true;
    CHECK(!a.SetHotkeys({}, std::nullopt, std::nullopt, err));
    CHECK(err.find("disk full") != std::string::npos);
    CHECK(rig.reg->applies.size() == 1);
    CHECK(a.Config().hotkeys.size() == 2);
    rig.fail_save = false;

    /* cycle / step replace only when given; bad step refused. */
    CHECK(a.SetHotkeys(a.Config().hotkeys,
                       std::vector<std::string>{ "gamma", "alpha" },
                       0.25, err));
    CHECK(a.Config().cycle.size() == 2 &&
          Near(a.Config().brightness_step, 0.25));
    CHECK(!a.SetHotkeys(a.Config().hotkeys, std::nullopt, 0.9, err));
    CHECK(a.SetHotkeys(a.Config().hotkeys, std::nullopt,
                       std::nullopt, err));
    CHECK(a.Config().cycle.size() == 2 &&
          Near(a.Config().brightness_step, 0.25));

    /* Clearing everything unregisters everything. */
    CHECK(a.SetHotkeys({}, std::nullopt, std::nullopt, err));
    CHECK(rig.reg->applies.back().empty());
    CHECK(a.ToJson()["hotkeys"].empty());

    /* No registrar: bindings persist, report unavailable. */
    Rig bare;
    Automation b(bare.Deps(false));
    CHECK(b.SetHotkeys({ { "pause_toggle", "Ctrl+Alt+P" } },
                       std::nullopt, std::nullopt, err));
    CHECK(b.ToJson()["registrar"] == false);
    CHECK(b.ToJson()["hotkeys"][0]["state"] == "unavailable");

    /* Restore: persisted bindings register without a save; one
       that no longer validates is kept and reports invalid. */
    Rig boot;
    Automation c(boot.Deps());
    AutomationConfig persisted;
    persisted.hotkeys = { { "next_workspace", "Ctrl+Alt+N" },
                          { "prev_workspace", "Shift+P" } };
    c.Restore(persisted);
    CHECK(boot.saved.empty());
    CHECK(boot.reg->applies.size() == 1 &&
          boot.reg->applies.back().size() == 1);
    body = c.ToJson();
    CHECK(Entry(body, "Ctrl+Alt+N") &&
          (*Entry(body, "Ctrl+Alt+N"))["state"] == "ok");
    CHECK(Entry(body, "Shift+P") &&
          (*Entry(body, "Shift+P"))["state"] == "invalid");
}

static void TestOutputActions()
{
    Rig rig;
    Automation a(rig.Deps());
    std::string err;
    CHECK(a.SetHotkeys({ { "brightness_up",   "Ctrl+Alt+Up"   },
                         { "brightness_down", "Ctrl+Alt+Down" },
                         { "lights_toggle",   "Ctrl+Alt+End"  },
                         { "pause_toggle",    "Ctrl+Alt+P"    } },
                       std::nullopt, std::nullopt, err));
    const TimePoint t0{};
    const int up   = IdOf(*rig.reg, "Ctrl+Alt+Up");
    const int down = IdOf(*rig.reg, "Ctrl+Alt+Down");
    const int end  = IdOf(*rig.reg, "Ctrl+Alt+End");
    const int p    = IdOf(*rig.reg, "Ctrl+Alt+P");
    CHECK(up != 0 && down != 0 && end != 0 && p != 0);

    /* The registrar thread only queues; Poll runs the action. */
    a.PostHotkey(down);
    CHECK(Near(rig.oc.State().master, 1.0));
    a.Poll(t0);
    CHECK(Near(rig.oc.State().master, 0.9));
    a.PostHotkey(down);
    a.PostHotkey(down);
    a.Poll(t0);
    CHECK(Near(rig.oc.State().master, 0.7, 1e-5));
    /* Clamped at both ends. */
    for(int i = 0; i < 12; ++i)
    {
        a.PostHotkey(down);
    }
    a.Poll(t0);
    CHECK(Near(rig.oc.State().master, 0.0));
    for(int i = 0; i < 15; ++i)
    {
        a.PostHotkey(up);
    }
    a.Poll(t0);
    CHECK(Near(rig.oc.State().master, 1.0));

    a.PostHotkey(end);
    a.Poll(t0);
    CHECK(rig.oc.State().blackout);
    a.PostHotkey(end);
    a.Poll(t0);
    CHECK(!rig.oc.State().blackout);
    a.PostHotkey(p);
    a.Poll(t0);
    CHECK(rig.oc.State().hold);
    a.RunAction(HotkeyAction::PauseToggle, t0);
    CHECK(!rig.oc.State().hold);

    /* A stale id (the set was replaced) and an unknown id are
       ignored. */
    CHECK(a.SetHotkeys({ { "lights_toggle", "Ctrl+Alt+End" } },
                       std::nullopt, std::nullopt, err));
    a.PostHotkey(down);
    a.PostHotkey(99999);
    a.Poll(t0);
    CHECK(Near(rig.oc.State().master, 1.0));
    CHECK(!rig.oc.State().blackout);
    /* Step from the config. */
    CHECK(a.SetHotkeys(a.Config().hotkeys, std::nullopt, 0.05, err));
    a.RunAction(HotkeyAction::BrightnessDown, t0);
    CHECK(Near(rig.oc.State().master, 0.95, 1e-5));
}

static void TestWorkspaceCycle()
{
    using std::chrono::milliseconds;
    Rig rig;
    Automation a(rig.Deps());
    const TimePoint t0 = TimePoint{} + std::chrono::hours(1);

    /* First press reloads at once (beta -> gamma). */
    a.RunAction(HotkeyAction::NextWorkspace, t0);
    CHECK(rig.reloads.size() == 1 && rig.reloads[0] == "gamma");
    /* A burst inside the debounce only moves the target. */
    a.RunAction(HotkeyAction::NextWorkspace, t0 + milliseconds(200));
    a.RunAction(HotkeyAction::NextWorkspace, t0 + milliseconds(400));
    a.RunAction(HotkeyAction::NextWorkspace, t0 + milliseconds(600));
    a.Poll(t0 + milliseconds(900));
    CHECK(rig.reloads.size() == 1);
    /* gamma -> alpha -> beta -> gamma: the burst lands back on
       what is already loading — nothing more is queued. */
    a.Poll(t0 + milliseconds(1700));
    CHECK(rig.reloads.size() == 1);

    /* Burst that ends elsewhere: one trailing reload of the
       latest target. */
    rig.active = "gamma";
    const TimePoint t1 = t0 + std::chrono::seconds(10);
    a.RunAction(HotkeyAction::NextWorkspace, t1);          /* alpha */
    a.RunAction(HotkeyAction::NextWorkspace, t1 + milliseconds(100));
    a.RunAction(HotkeyAction::NextWorkspace, t1 + milliseconds(200));
    a.RunAction(HotkeyAction::NextWorkspace, t1 + milliseconds(300));
    a.RunAction(HotkeyAction::NextWorkspace, t1 + milliseconds(400));
    CHECK(rig.reloads.size() == 2 && rig.reloads[1] == "alpha");
    a.Poll(t1 + milliseconds(1300));
    CHECK(rig.reloads.size() == 2);
    a.Poll(t1 + milliseconds(1400));
    /* alpha, beta, gamma, alpha, beta -> beta */
    CHECK(rig.reloads.size() == 3 && rig.reloads[2] == "beta");
    a.Poll(t1 + milliseconds(5000));
    CHECK(rig.reloads.size() == 3);

    /* prev wraps; an unknown active id starts at an end. */
    rig.active = "alpha";
    const TimePoint t2 = t1 + std::chrono::seconds(10);
    a.RunAction(HotkeyAction::PrevWorkspace, t2);
    CHECK(rig.reloads.back() == "gamma");
    rig.active = "zzz";
    a.RunAction(HotkeyAction::NextWorkspace, t2 + std::chrono::seconds(5));
    CHECK(rig.reloads.back() == "alpha");
    a.RunAction(HotkeyAction::PrevWorkspace, t2 + std::chrono::seconds(10));
    CHECK(rig.reloads.back() == "gamma");

    /* A curated cycle (ids that no longer exist are skipped). */
    std::string err;
    CHECK(a.SetHotkeys({}, std::vector<std::string>{ "gamma", "gone",
                                                     "alpha" },
                       std::nullopt, err));
    rig.active = "alpha";
    const size_t before = rig.reloads.size();
    a.RunAction(HotkeyAction::NextWorkspace, t2 + std::chrono::seconds(20));
    CHECK(rig.reloads.size() == before + 1 &&
          rig.reloads.back() == "gamma");

    /* Nothing to cycle: no reload, no crash. */
    rig.ids.clear();
    a.RunAction(HotkeyAction::NextWorkspace, t2 + std::chrono::seconds(30));
    a.Poll(t2 + std::chrono::seconds(40));
    CHECK(rig.reloads.size() == before + 1);
}

int main()
{
    TestParseChord();
    TestActions();
    TestValidate();
    TestConfigJson();
    TestRegistration();
    TestOutputActions();
    TestWorkspaceCycle();
    std::printf("automation: %d checks, %d failures\n",
                checks, failures);
    return failures ? 1 : 0;
}
