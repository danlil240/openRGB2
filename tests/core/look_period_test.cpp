/*---------------------------------------------------------*\
|| look_period_test.cpp                                      |
||                                                           |
||   R-11 step 1: the pure period analysis that decides     |
||   whether a look can be compiled into one seamless       |
||   uploaded loop, and the optional speed snapping.        |
||                                                           |
||   Cases from docs/plans/R-11-compile-looks-to-uploaded-  |
||   loops.md step 1: packaged Wave refused without         |
||   snapping (10.42 s and 6.0 s, no common cycle under     |
||   20 s), noise refused, ripple refused, 2 s + 3 s gives  |
||   a 6 s cycle, static gives a 1-frame loop. Plus the     |
||   snapping rule the plan recommends (period -> T/k for   |
||   the T with the smallest relative speed change, <= 5%). |
||                                                           |
||   SPDX-License-Identifier: GPL-2.0-or-later               |
\*---------------------------------------------------------*/

#include "playback/LookPeriod.h"
#include "effects/Presets.h"
#include "effects/EffectEngine.h"
#include "scene/DefaultDesk.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using namespace studio;

static int checks = 0;
static int failures = 0;
#define CHECK(x) do { ++checks; if(!(x)) { ++failures; \
    std::printf("FAIL line %d: %s\n", __LINE__, #x); } } while(0)

static bool Near(double a, double b, double tol) { return std::fabs(a - b) <= tol; }

/* One Device object with a few emitters along x — enough for
   every untargeted layer to reach it. */
static SceneDocument OneFanDoc()
{
    SceneDocument doc;
    SceneObject fan;
    fan.id       = "fan";
    fan.kind     = ObjectKind::Device;
    fan.geometry = "fan_body";
    for(int i = 0; i < 4; i++)
    {
        Emitter e;
        e.local_pos = { 0.02f * (float)i, 0.0f, 0.0f };
        fan.emitters.push_back(e);
    }
    doc.objects.push_back(fan);
    return doc;
}

static EffectLayer Layer(const char* prim, float speed, float scale)
{
    EffectLayer L;
    L.primitive = prim;
    L.speed     = speed;
    L.scale     = scale;
    L.palette   = MakePalette({ PaletteStop{ 0.0f, ColorF{ 1, 0, 0, 1 } },
                                PaletteStop{ 0.5f, ColorF{ 0, 0, 1, 1 } } });
    return L;
}

static void TestPackagedWaveRefused()
{
    const std::vector<EffectLayer> layers = BuildPreset("wave", 0);
    CHECK(layers.size() == 3);
    const SceneDocument doc = BuildDefaultDesk();

    const LookPeriod p = AnalyzeLookPeriod(layers, doc);
    CHECK(!p.compilable);
    CHECK(p.reason.find("common cycle") != std::string::npos);
    /* Both wave layers reach the desk. The static keyboard layer
       reaches nothing here: the default desk's keyboard is a
       matrix_map object whose emitters are built from the bound
       zone at scene preparation, so it has none offline. */
    CHECK(p.layers.size() == 3);
    CHECK(p.layers[0].motion == LayerMotion::Periodic);
    CHECK(p.layers[1].motion == LayerMotion::Periodic);
    CHECK(p.layers[2].motion == LayerMotion::Unused);
    CHECK(Near(p.layers[0].period_s, 1.25 / 0.12, 1e-3));   /* 10.42 s */
    CHECK(Near(p.layers[1].period_s, 0.9 / 0.15, 1e-3));    /*  6.0 s  */
    CHECK(!LookCycleSeconds(layers, doc).has_value());

    /* The plan's snapping rule does not rescue Wave either: the
       best cycle under 20 s needs a ~7.1% speed change (T ~ 11.2 s,
       one slow cycle and two fast ones). */
    const LookPeriod s = AnalyzeLookPeriod(layers, doc, SnapOptions());
    CHECK(!s.compilable);
    CHECK(Near(s.best_change, 0.0706, 0.001));
    CHECK(Near(s.best_cycle_s, 11.208, 0.01));

    /* Allowed 10%, it snaps to that cycle. */
    LookPeriodOptions loose = SnapOptions();
    loose.max_change = 0.10;
    const LookPeriod w = AnalyzeLookPeriod(layers, doc, loose);
    CHECK(w.compilable);
    CHECK(w.snapped);
    CHECK(Near(w.cycle_s, 11.208, 0.01));
    CHECK(w.layers[0].repeats == 1 && w.layers[1].repeats == 2);
    CHECK(Near(w.max_change, 0.0706, 0.001));
}

static void TestNoiseRefused()
{
    const SceneDocument doc = OneFanDoc();
    EffectLayer n = Layer("noise", 1.0f, 3.0f);
    n.direction = { 0.1f, 0.3f, 0.0f };
    const LookPeriod p = AnalyzeLookPeriod({ n }, doc);
    CHECK(!p.compilable);
    CHECK(p.layers.size() == 1 && p.layers[0].motion == LayerMotion::NotCompilable);
    CHECK(p.reason.find("noise") != std::string::npos);
    /* Snapping cannot help a non-periodic layer. */
    CHECK(!AnalyzeLookPeriod({ n }, doc, SnapOptions()).compilable);

    /* Frozen noise (speed 0) does not move: a still frame. */
    n.speed = 0.0f;
    const LookPeriod still = AnalyzeLookPeriod({ n }, doc);
    CHECK(still.compilable && still.is_static);
}

static void TestRippleRefused()
{
    const SceneDocument doc = OneFanDoc();
    EffectLayer r = Layer("ripple", 0.5f, 0.05f);
    r.source = "key";
    const LookPeriod p = AnalyzeLookPeriod({ Layer("static", 0, 1), r }, doc);
    CHECK(!p.compilable);
    CHECK(p.reason.find("ripple") != std::string::npos);
    CHECK(p.reason.find("live input") != std::string::npos);
    /* screenfield and level read live input too. */
    CHECK(!AnalyzeLookPeriod({ Layer("screenfield", 0, 1) }, doc).compilable);
    CHECK(!AnalyzeLookPeriod({ Layer("level", 0, 1) }, doc).compilable);
}

static void TestCommensurateCycle()
{
    SceneDocument doc = OneFanDoc();
    /* wave: period = scale/|speed| -> 1/0.5 = 2 s;
       gradient: period = 1/|speed| -> 3 s. */
    const std::vector<EffectLayer> layers = {
        Layer("wave", 0.5f, 1.0f), Layer("gradient", 1.0f / 3.0f, 1.0f) };
    const LookPeriod p = AnalyzeLookPeriod(layers, doc);
    CHECK(p.compilable && !p.is_static && !p.snapped);
    CHECK(Near(p.cycle_s, 6.0, 1e-4));
    CHECK(p.layers[0].repeats == 3 && p.layers[1].repeats == 2);
    CHECK(p.max_change < 1e-5);
    CHECK(NominalFrameCount(p) == 360);   /* 6 s at 60 fps */
    const auto t = LookCycleSeconds(layers, doc);
    CHECK(t.has_value() && Near(*t, 6.0, 1e-4));

    /* Global EffectState.speed folds in: twice as fast -> 3 s. */
    doc.effect.speed = 2.0f;
    const LookPeriod fast = AnalyzeLookPeriod(layers, doc);
    CHECK(fast.compilable && Near(fast.cycle_s, 3.0, 1e-4));
    /* Intensity 0 makes every layer invisible: a still frame. */
    doc.effect.speed     = 1.0f;
    doc.effect.intensity = 0.0f;
    CHECK(AnalyzeLookPeriod(layers, doc).is_static);
    doc.effect.intensity = 1.0f;

    /* comet: period = path length / |speed| (a 1 m square loop
       at 0.5 m/s is 8 s); with the 2 s wave the cycle is 8 s. */
    EffectLayer c = Layer("comet", 0.5f, 0.3f);
    c.path = { { 0, 0, 0 }, { 1, 0, 0 }, { 1, 1, 0 }, { 0, 1, 0 } };
    const LookPeriod pc = AnalyzeLookPeriod({ layers[0], c }, doc);
    CHECK(pc.compilable && Near(pc.cycle_s, 8.0, 1e-3));

    /* Within the 0.5% tolerance: 2 s and 3.01 s still close at
       ~6 s, and the runtime stack is nudged to exact periods. */
    const std::vector<EffectLayer> near = {
        Layer("wave", 0.5f, 1.0f), Layer("spin", 1.0f / 3.01f, 2.0f) };
    const LookPeriod pn = AnalyzeLookPeriod(near, doc);
    CHECK(pn.compilable && !pn.snapped);
    CHECK(pn.max_change > 0.0 && pn.max_change <= 0.005);
    CHECK(pn.runtime_layers.size() == 2);
    const double p0 = 1.0 / pn.runtime_layers[0].speed;   /* wave scale 1 */
    const double p1 = 1.0 / pn.runtime_layers[1].speed;
    CHECK(Near(pn.cycle_s / p0, 3.0, 1e-5));
    CHECK(Near(pn.cycle_s / p1, 2.0, 1e-5));

    /* A layer that reaches no emitter is ignored, even noise. */
    EffectLayer off = Layer("noise", 1.0f, 3.0f);
    off.targets = { "keyboard_body" };
    CHECK(AnalyzeLookPeriod({ layers[0], off }, doc).compilable);
    /* So is a disabled one. */
    off.targets.clear();
    off.enabled = false;
    CHECK(AnalyzeLookPeriod({ layers[0], off }, doc).compilable);
}

static void TestStaticOneFrame()
{
    const SceneDocument doc = OneFanDoc();
    const LookPeriod p = AnalyzeLookPeriod({ Layer("static", 0, 1) }, doc);
    CHECK(p.compilable && p.is_static);
    CHECK(p.cycle_s == 0.0);
    CHECK(NominalFrameCount(p) == 1);
    const auto t = LookCycleSeconds({ Layer("static", 0, 1) }, doc);
    CHECK(t.has_value() && *t == 0.0);
    /* A moving primitive at speed 0 is static too. */
    CHECK(AnalyzeLookPeriod({ Layer("wave", 0.0f, 1.0f) }, doc).is_static);
    /* So is an empty stack (painted colors only). */
    CHECK(AnalyzeLookPeriod({}, doc).is_static);
}

static void TestSnapping()
{
    const SceneDocument doc = OneFanDoc();
    /* 2 s and 3.1 s: 3.3% apart, no exact cycle. */
    const std::vector<EffectLayer> layers = {
        Layer("wave", 0.5f, 1.0f), Layer("gradient", 1.0f / 3.1f, 1.0f) };
    CHECK(!AnalyzeLookPeriod(layers, doc).compilable);

    const LookPeriod s = AnalyzeLookPeriod(layers, doc, SnapOptions());
    CHECK(s.compilable && s.snapped);
    /* k = (3, 2): 6.0 and 6.2 -> T = 6.1, each layer moves 1.64%. */
    CHECK(Near(s.cycle_s, 6.1, 1e-3));
    CHECK(s.layers[0].repeats == 3 && s.layers[1].repeats == 2);
    CHECK(Near(s.max_change, 0.1 / 6.1, 1e-4));
    CHECK(Near(s.layers[0].speed_change, -0.1 / 6.1, 1e-4));  /* slower */
    CHECK(Near(s.layers[1].speed_change, +0.1 / 6.1, 1e-4));  /* faster */
    /* The snapped stack loops exactly at T. */
    const double p0 = 1.0 / s.runtime_layers[0].speed;
    const double p1 = 1.0 / s.runtime_layers[1].speed;
    CHECK(Near(s.cycle_s / p0, 3.0, 1e-5));
    CHECK(Near(s.cycle_s / p1, 2.0, 1e-5));

    /* Frames at t = 0 and t = T match bit for bit. */
    EffectEngine engine;
    engine.SetLayers(s.runtime_layers);
    FrameColors a, b;
    engine.Evaluate(doc, 0.0, a);
    engine.Evaluate(doc, s.cycle_s, b);
    CHECK(!a.empty());
    int diff = 0;
    for(size_t i = 0; i < a["fan"].size(); i++)
    {
        const SceneColor x = a["fan"][i], y = b["fan"][i];
        for(int sh = 0; sh < 24; sh += 8)
        {
            diff = std::max(diff, std::abs((int)((x >> sh) & 0xFF) - (int)((y >> sh) & 0xFF)));
        }
    }
    CHECK(diff <= 1);

    /* An exact cycle needs no snap: the option leaves it alone. */
    const LookPeriod e = AnalyzeLookPeriod(
        { Layer("wave", 0.5f, 1.0f), Layer("gradient", 1.0f / 3.0f, 1.0f) },
        doc, SnapOptions());
    CHECK(e.compilable && !e.snapped && Near(e.cycle_s, 6.0, 1e-4));

    /* A single layer slower than the 20 s cap cannot loop... */
    const std::vector<EffectLayer> slow = { Layer("gradient", 1.0f / 25.0f, 1.0f) };
    const LookPeriod sl = AnalyzeLookPeriod(slow, doc, SnapOptions());
    CHECK(!sl.compilable);
    CHECK(sl.reason.find("longer than the 20 s limit") != std::string::npos);
    /* ...but 20.5 s snaps to 20 s at 2.5%. */
    const LookPeriod s2 = AnalyzeLookPeriod({ Layer("gradient", 1.0f / 20.5f, 1.0f) },
                                            doc, SnapOptions());
    CHECK(s2.compilable && s2.snapped && Near(s2.cycle_s, 20.0, 1e-6));
    CHECK(Near(s2.max_change, 0.025, 1e-4));
}

int main()
{
    TestPackagedWaveRefused();
    TestNoiseRefused();
    TestRippleRefused();
    TestCommensurateCycle();
    TestStaticOneFrame();
    TestSnapping();
    std::printf("look_period: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
