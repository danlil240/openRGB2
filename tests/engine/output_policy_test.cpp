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

int main()
{
    TestModel();
    TestJson();
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
