// SPDX-License-Identifier: GPL-2.0-or-later
//
// Audit E-12b exit/idle output policy (src/engine/OutputPolicy.h).
// Deterministic, no backend: the model, controller matching, the
// JSON validator shared by the profile load and set_policy, and
// the per-key executor's decision table over injected deps.

#include "OutputPolicy.h"

#include <chrono>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

using namespace openrgb2;

static int checks   = 0;
static int failures = 0;
#define CHECK(x) do { ++checks; if(!(x)) { ++failures; \
    std::printf("FAIL line %d: %s\n", __LINE__, #x); } } while(0)

static DeviceKey Key(const char* device, const char* serial,
                     const char* location)
{
    DeviceKey k;
    k.device   = device;
    k.serial   = serial;
    k.location = location;
    return k;
}

/*---------------------------------------------------------*\
|| Model: defaults, names, matching, resolution             ||
\*---------------------------------------------------------*/
static void TestModel()
{
    OutputPolicy p;
    CHECK(p.on_exit == OutputAction::Hold);
    CHECK(p.on_idle == OutputAction::Hold);
    CHECK(p.IsDefault());

    CHECK(std::string(OutputActionName(OutputAction::Hold))    == "hold");
    CHECK(std::string(OutputActionName(OutputAction::Black))   == "black");
    CHECK(std::string(OutputActionName(OutputAction::Restore)) == "restore");
    OutputAction a = OutputAction::Hold;
    CHECK(ParseOutputAction("black", a) && a == OutputAction::Black);
    CHECK(ParseOutputAction("restore", a) && a == OutputAction::Restore);
    CHECK(!ParseOutputAction("Black", a));
    CHECK(!ParseOutputAction("off", a));

    const DeviceKey kb  = Key("Logitech: G512", "KB-1", "HID: 1-2");
    const DeviceKey gpu = Key("NVIDIA: RTX", "", "I2C: /dev/i2c-3");
    const DeviceKey mb  = Key("GIGABYTE: X870E", "MB-1", "smbus:0");

    OutputPolicyMatch m;
    CHECK(m.Empty());
    CHECK(!m.Matches(kb));                 /* empty selects nothing */
    m.device = "Logitech: G512";
    CHECK(m.Matches(kb));
    CHECK(!m.Matches(gpu));
    m.serial = "OTHER";
    CHECK(!m.Matches(kb));                 /* every given field     */
    m = OutputPolicyMatch{};
    m.location_prefix = "I2C:";
    CHECK(m.Matches(gpu));
    CHECK(!m.Matches(kb));
    m = OutputPolicyMatch{};
    m.serial = "MB-1";
    CHECK(m.Matches(mb));

    /* First rule that matches AND sets the field wins. */
    p.on_exit = OutputAction::Black;
    OutputPolicyRule r1;
    r1.match.location_prefix = "I2C:";
    r1.on_idle = OutputAction::Restore;          /* exit not set  */
    OutputPolicyRule r2;
    r2.match.location_prefix = "I2C:";
    r2.on_exit = OutputAction::Hold;
    OutputPolicyRule r3;
    r3.match.device = "Logitech: G512";
    r3.on_exit = OutputAction::Restore;
    p.per_controller = { r1, r2, r3 };
    CHECK(!p.IsDefault());
    CHECK(p.ExitFor(gpu) == OutputAction::Hold);       /* r2      */
    CHECK(p.IdleFor(gpu) == OutputAction::Restore);    /* r1      */
    CHECK(p.ExitFor(kb)  == OutputAction::Restore);    /* r3      */
    CHECK(p.IdleFor(kb)  == OutputAction::Hold);       /* global  */
    CHECK(p.ExitFor(mb)  == OutputAction::Black);      /* global  */

    CHECK(IsDirectModeName("Direct"));
    CHECK(IsDirectModeName("direct"));
    CHECK(IsDirectModeName("Custom"));
    CHECK(!IsDirectModeName("Rainbow"));
    CHECK(!IsDirectModeName("Static"));
}

/*---------------------------------------------------------*\
|| JSON: round trip + validation                            ||
\*---------------------------------------------------------*/
static void TestJson()
{
    OutputPolicy p;
    p.on_exit = OutputAction::Black;
    p.on_idle = OutputAction::Restore;
    OutputPolicyRule r;
    r.match.device          = "Logitech: G512";
    r.match.location_prefix = "HID:";
    r.match.serial          = "KB-1";
    r.on_exit               = OutputAction::Hold;
    p.per_controller.push_back(r);

    const nlohmann::json j = OutputPolicyToJson(p);
    CHECK(j["on_exit"] == "black");
    CHECK(j["on_idle"] == "restore");
    CHECK(j["per_controller"].size() == 1);
    CHECK(j["per_controller"][0]["on_exit"] == "hold");
    CHECK(!j["per_controller"][0].contains("on_idle"));
    CHECK(j["per_controller"][0]["match"]["serial"] == "KB-1");

    OutputPolicy back;
    std::string err;
    CHECK(OutputPolicyFromJson(j, back, err));
    CHECK(err.empty());
    CHECK(back == p);

    /* Defaults: an empty object is the default policy; absent
       match fields are omitted on the wire. */
    OutputPolicy d;
    CHECK(OutputPolicyFromJson(nlohmann::json::object(), d, err));
    CHECK(d.IsDefault());
    const nlohmann::json dj = OutputPolicyToJson(OutputPolicy{});
    CHECK(dj["on_exit"] == "hold");
    CHECK(dj["on_idle"] == "hold");
    CHECK(dj["per_controller"].is_array() &&
          dj["per_controller"].empty());

    /* Rejections leave `out` untouched and name the path. */
    auto rejects = [](const nlohmann::json& bad, const char* path)
    {
        OutputPolicy o;
        o.on_exit = OutputAction::Restore;
        std::string e;
        const bool ok = OutputPolicyFromJson(bad, o, e);
        const bool untouched = o.on_exit == OutputAction::Restore &&
                               o.per_controller.empty();
        if(ok || !untouched || e.find(path) == std::string::npos)
        {
            std::printf("  rejects(%s) -> ok=%d err='%s'\n",
                        bad.dump().c_str(), (int)ok, e.c_str());
            return false;
        }
        return true;
    };
    const auto J = [](const char* text)
    { return nlohmann::json::parse(text); };
    CHECK(rejects(nlohmann::json::array(), "output_policy"));
    CHECK(rejects(J(R"({"on_exit":"off"})"), "on_exit"));
    CHECK(rejects(J(R"({"on_idle":3})"), "on_idle"));
    CHECK(rejects(J(R"({"per_controller":"x"})"), "per_controller"));
    CHECK(rejects(J(R"({"per_controller":[{"on_exit":"black"}]})"),
                  "per_controller[0].match"));
    CHECK(rejects(J(R"({"per_controller":[{"match":{},"on_exit":"black"}]})"),
                  "per_controller[0].match"));
    CHECK(rejects(J(R"({"per_controller":[{"match":{"serial":7},"on_exit":"black"}]})"),
                  "per_controller[0].match.serial"));
    CHECK(rejects(J(R"({"per_controller":[{"match":{"serial":"S"},"on_idle":"dim"}]})"),
                  "per_controller[0].on_idle"));
    {
        nlohmann::json many = nlohmann::json::array();
        for(size_t i = 0; i <= kMaxOutputPolicyRules; ++i)
        {
            many.push_back(J(R"({"match":{"serial":"S"},"on_exit":"black"})"));
        }
        nlohmann::json body = nlohmann::json::object();
        body["per_controller"] = many;
        CHECK(rejects(body, "per_controller"));
    }
}

/*---------------------------------------------------------*\
|| Executor: decision table over recorded fake deps         ||
\*---------------------------------------------------------*/
struct FakeOps
{
    bool                     paused = false;
    std::vector<DeviceKey>   calibration;
    std::vector<DeviceKey>   owned;
    bool                     stop_black_ok = true;
    bool                     submit_ok     = true;
    bool                     idle_ok       = true;
    bool                     select_ok     = true;
    std::vector<std::string> calls;      /* "op:serial" in order */

    static bool Has(const std::vector<DeviceKey>& v, const DeviceKey& k)
    {
        for(const DeviceKey& x : v)
        {
            if(x == k) return true;
        }
        return false;
    }

    OutputPolicyDeps Deps()
    {
        OutputPolicyDeps d;
        d.output_paused = [this] { return paused; };
        d.calibration_owns = [this](const DeviceKey& k)
        { return Has(calibration, k); };
        d.playback_owns = [this](const DeviceKey& k)
        { return Has(owned, k); };
        d.playback_stop_black = [this](std::string& err)
        {
            calls.push_back("stop_black");
            if(!stop_black_ok) err = "no acknowledged upload";
            return stop_black_ok;
        };
        d.submit_black = [this](const DeviceSnapshot& s, std::string& err)
        {
            calls.push_back("black:" + s.key.serial);
            if(!submit_ok) err = "lane rejected";
            return submit_ok;
        };
        d.wait_idle = [this](const DeviceKey& k, Clock::duration t)
        {
            calls.push_back("idle:" + k.serial + ":" +
                std::to_string(std::chrono::duration_cast<
                    std::chrono::milliseconds>(t).count()));
            return idle_ok;
        };
        d.select_mode = [this](const DeviceKey& k, const PriorMode& m,
                               std::string& err)
        {
            calls.push_back("select:" + k.serial + ":" + m.name);
            if(!select_ok) err = "send failed";
            return select_ok;
        };
        d.drop_pending = [this](const DeviceKey& k)
        { calls.push_back("drop:" + k.serial); };
        return d;
    }
};

static DeviceSnapshot Snap(const char* serial, const char* location)
{
    DeviceSnapshot s;
    s.key        = Key((std::string("dev-") + serial).c_str(), serial,
                       location);
    s.connection = ConnectionState::Connected;
    s.topology_generation = 1;
    s.zones      = { ZoneSnapshot{ "Z", 4 } };
    return s;
}

static const OutputPolicyOutcome* Find(
    const std::vector<OutputPolicyOutcome>& v, const char* serial)
{
    for(const OutputPolicyOutcome& o : v)
    {
        if(o.key.serial == serial) return &o;
    }
    return nullptr;
}

static size_t IndexOfCall(const std::vector<std::string>& calls,
                          const std::string& c)
{
    for(size_t i = 0; i < calls.size(); ++i)
    {
        if(calls[i] == c) return i;
    }
    return (size_t)-1;
}

static void TestExecutor()
{
    const DeviceSnapshot kb   = Snap("KB", "HID: 1");
    const DeviceSnapshot mb   = Snap("MB", "smbus:0");
    const DeviceSnapshot gpu  = Snap("GPU", "I2C: 3");
    const DeviceSnapshot fans = Snap("FANS", "Wireless: AA");
    std::map<DeviceKey, PriorMode> priors;
    PriorMode rainbow;
    rainbow.name = "Rainbow";
    priors[kb.key] = rainbow;
    PriorMode direct;
    direct.name = "Direct";
    priors[gpu.key] = direct;              /* left by an earlier run */
    priors[fans.key] = rainbow;

    /* hold (the default): nothing is touched. */
    {
        FakeOps ops;
        const auto out = RunOutputPolicy({ kb, mb }, OutputPhase::Exit,
                                         OutputPolicy{}, priors,
                                         ops.Deps());
        CHECK(out.size() == 2);
        CHECK(out[0].outcome == "held" && out[1].outcome == "held");
        CHECK(ops.calls.empty());
    }

    /* black: every lane gets its black queued (pending frames
       dropped first) BEFORE any wait — lanes drain in parallel. */
    {
        FakeOps ops;
        OutputPolicy p;
        p.on_exit = OutputAction::Black;
        const auto out = RunOutputPolicy({ kb, mb }, OutputPhase::Exit,
                                         p, priors, ops.Deps());
        CHECK(out.size() == 2);
        CHECK(Find(out, "KB") && Find(out, "KB")->outcome == "black");
        CHECK(Find(out, "MB") && Find(out, "MB")->outcome == "black");
        CHECK(Find(out, "MB")->policy == OutputAction::Black);
        const size_t b1 = IndexOfCall(ops.calls, "black:KB");
        const size_t b2 = IndexOfCall(ops.calls, "black:MB");
        const size_t w1 = IndexOfCall(ops.calls, "idle:KB:500");
        const size_t w2 = IndexOfCall(ops.calls, "idle:MB:500");
        CHECK(IndexOfCall(ops.calls, "drop:KB") < b1);
        CHECK(b1 != (size_t)-1 && b2 != (size_t)-1);
        CHECK(w1 != (size_t)-1 && w2 != (size_t)-1);
        CHECK(b1 < w1 && b2 < w1 && b1 < w2 && b2 < w2);
        CHECK(IndexOfCall(ops.calls, "stop_black") == (size_t)-1);
    }

    /* restore: wait for the lane, then UPDATEMODE back. No
       record or a Direct record falls back to black; the
       wireless group always falls back to black. */
    {
        FakeOps ops;
        OutputPolicy p;
        p.on_exit = OutputAction::Restore;
        const auto out = RunOutputPolicy({ kb, mb, gpu, fans },
                                         OutputPhase::Exit, p, priors,
                                         ops.Deps());
        CHECK(out.size() == 4);
        CHECK(Find(out, "KB")->outcome == "restored");
        CHECK(Find(out, "KB")->detail == "Rainbow");
        CHECK(Find(out, "MB")->outcome == "black_fallback");
        CHECK(Find(out, "GPU")->outcome == "black_fallback");
        CHECK(Find(out, "FANS")->outcome == "black_fallback");
        CHECK(Find(out, "FANS")->detail.find("wireless") !=
              std::string::npos);
        const size_t w = IndexOfCall(ops.calls, "idle:KB:500");
        const size_t s = IndexOfCall(ops.calls, "select:KB:Rainbow");
        CHECK(w != (size_t)-1 && s != (size_t)-1 && w < s);
        CHECK(IndexOfCall(ops.calls, "black:MB") != (size_t)-1);
        CHECK(IndexOfCall(ops.calls, "black:GPU") != (size_t)-1);
        CHECK(IndexOfCall(ops.calls, "black:FANS") != (size_t)-1);
        CHECK(IndexOfCall(ops.calls, "select:FANS:Rainbow") ==
              (size_t)-1);
        CHECK(IndexOfCall(ops.calls, "black:KB") == (size_t)-1);
    }

    /* Per-controller override beats the global value; the idle
       phase reads on_idle. */
    {
        FakeOps ops;
        OutputPolicy p;
        p.on_exit = OutputAction::Black;
        p.on_idle = OutputAction::Hold;
        OutputPolicyRule r;
        r.match.serial = "KB";
        r.on_exit      = OutputAction::Hold;
        r.on_idle      = OutputAction::Restore;
        p.per_controller.push_back(r);
        auto out = RunOutputPolicy({ kb, mb }, OutputPhase::Exit, p,
                                   priors, ops.Deps());
        CHECK(Find(out, "KB")->outcome == "held");
        CHECK(Find(out, "MB")->outcome == "black");
        ops.calls.clear();
        out = RunOutputPolicy({ kb, mb }, OutputPhase::Idle, p, priors,
                              ops.Deps());
        CHECK(Find(out, "KB")->outcome == "restored");
        CHECK(Find(out, "MB")->outcome == "held");
        CHECK(IndexOfCall(ops.calls, "black:MB") == (size_t)-1);
    }

    /* Loop-owned group: black = the playback owner's stop-black
       (once); restore/hold keep the loop (it IS a hardware
       mode). A hold key then drops the black stop-black queued
       for wired peers. */
    {
        FakeOps ops;
        ops.owned = { fans.key };
        OutputPolicy p;
        p.on_exit = OutputAction::Black;
        OutputPolicyRule r;
        r.match.serial = "MB";
        r.on_exit      = OutputAction::Hold;
        p.per_controller.push_back(r);
        auto out = RunOutputPolicy({ fans, kb, mb }, OutputPhase::Exit,
                                   p, priors, ops.Deps());
        CHECK(Find(out, "FANS")->outcome == "loop_stopped");
        CHECK(Find(out, "KB")->outcome == "black");
        CHECK(Find(out, "MB")->outcome == "held");
        size_t stops = 0;
        for(const std::string& c : ops.calls)
        {
            stops += c == "stop_black" ? 1 : 0;
        }
        CHECK(stops == 1);
        CHECK(IndexOfCall(ops.calls, "black:FANS") == (size_t)-1);
        CHECK(IndexOfCall(ops.calls, "drop:MB") != (size_t)-1);
        CHECK(IndexOfCall(ops.calls, "stop_black") <
              IndexOfCall(ops.calls, "black:KB"));

        FakeOps ops2;
        ops2.owned = { fans.key };
        p.on_exit  = OutputAction::Restore;
        p.per_controller.clear();
        out = RunOutputPolicy({ fans }, OutputPhase::Exit, p, priors,
                              ops2.Deps());
        CHECK(Find(out, "FANS")->outcome == "loop_kept");
        CHECK(ops2.calls.empty());

        FakeOps ops3;
        ops3.owned         = { fans.key };
        ops3.stop_black_ok = false;
        p.on_exit = OutputAction::Black;
        out = RunOutputPolicy({ fans }, OutputPhase::Exit, p, priors,
                              ops3.Deps());
        CHECK(Find(out, "FANS")->outcome == "failed");
        CHECK(Find(out, "FANS")->detail == "no acknowledged upload");
    }

    /* Skips: the power gate skips every key with no call at all;
       calibration skips its key only. */
    {
        FakeOps ops;
        ops.paused = true;
        OutputPolicy p;
        p.on_exit = OutputAction::Black;
        auto out = RunOutputPolicy({ kb, fans }, OutputPhase::Exit, p,
                                   priors, ops.Deps());
        CHECK(Find(out, "KB")->outcome == "skipped_power_gate");
        CHECK(Find(out, "FANS")->outcome == "skipped_power_gate");
        CHECK(ops.calls.empty());

        FakeOps ops2;
        ops2.calibration = { kb.key };
        out = RunOutputPolicy({ kb, mb }, OutputPhase::Exit, p, priors,
                              ops2.Deps());
        CHECK(Find(out, "KB")->outcome == "skipped_calibration");
        CHECK(Find(out, "MB")->outcome == "black");
        CHECK(IndexOfCall(ops2.calls, "black:KB") == (size_t)-1);
    }

    /* Failures are reported per key, never thrown. */
    {
        FakeOps ops;
        ops.submit_ok = false;
        ops.select_ok = false;
        OutputPolicy p;
        p.on_exit = OutputAction::Restore;
        auto out = RunOutputPolicy({ kb, mb }, OutputPhase::Exit, p,
                                   priors, ops.Deps());
        CHECK(Find(out, "KB")->outcome == "failed");
        CHECK(Find(out, "KB")->detail == "send failed");
        CHECK(Find(out, "MB")->outcome == "failed");
        CHECK(Find(out, "MB")->detail == "lane rejected");

        FakeOps ops2;
        ops2.idle_ok = false;
        p.on_exit = OutputAction::Black;
        out = RunOutputPolicy({ mb }, OutputPhase::Exit, p, priors,
                              ops2.Deps());
        CHECK(Find(out, "MB")->outcome == "black");
        CHECK(!Find(out, "MB")->detail.empty());   /* unconfirmed */
    }

    /* Report shape. */
    {
        OutputPolicyOutcome o;
        o.key     = kb.key;
        o.policy  = OutputAction::Restore;
        o.outcome = "restored";
        o.detail  = "Rainbow";
        const nlohmann::json j = OutputPolicyOutcomesToJson({ o });
        CHECK(j.is_array() && j.size() == 1);
        CHECK(j[0]["policy"] == "restore");
        CHECK(j[0]["outcome"] == "restored");
        CHECK(j[0]["detail"] == "Rainbow");
        CHECK(j[0]["key"]["device"] == "dev-KB");
        CHECK(j[0]["key"]["location"] == "HID: 1");
    }
}

int main()
{
    TestModel();
    TestJson();
    TestExecutor();
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
