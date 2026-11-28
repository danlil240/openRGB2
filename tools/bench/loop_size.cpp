/*---------------------------------------------------------*\
|| loop_size.cpp                                             |
||                                                           |
||   R-11 step 3: offline measurement. Compiles every        |
||   packaged look for the 120-LED SL Wireless group into    |
||   one uploaded loop and prints its cycle T, frame count   |
||   N, effective fps and tinyuz size against the firmware   |
||   limits (12 KiB compressed, 4096 frames) - once without  |
||   speed snapping and once with the plan's snapping rule   |
||   (<= 5% speed change, cycle <= 20 s).                    |
||                                                           |
||   No hardware, no SDK: the frames come from the same      |
||   EffectEngine the live path uses, over the default desk  |
||   reduced to the group's three fans (slw_fan_0..2, LED    |
||   order = emitter address), and the loop from the shared  |
||   BuildLoopFromRenderer (60 fps start, shrink to fit).    |
||                                                           |
||   Usage: loop_size [--seeds N]                            |
||     N = remix seeds swept per look (default 100; 0 = skip) |
||   Output is Markdown (docs/benchmarks/loop-size.md).      |
||                                                           |
||   SPDX-License-Identifier: GPL-2.0-or-later               |
\*---------------------------------------------------------*/

#include "effects/EffectEngine.h"
#include "effects/Presets.h"
#include "playback/LookPeriod.h"
#include "playback/PlaybackLoop.h"
#include "scene/DefaultDesk.h"
#include "scene/SceneGraph.h"

#include "LianLiWirelessFanType.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

using namespace studio;

namespace
{

const char* const kGroupFans[] = { "slw_fan_0", "slw_fan_1", "slw_fan_2" };
constexpr size_t  kLedsPerFan  = 40;
constexpr size_t  kGroupLeds   = 120;

/* The default desk with only the wireless group's fans as Device
   objects (parents and decor stay for the world transforms). The
   period analysis then sees exactly the layers that reach the
   group, as a per-group compiler would. */
SceneDocument GroupDoc()
{
    SceneDocument full = BuildDefaultDesk();
    SceneDocument doc  = full;
    doc.objects.clear();
    for(const SceneObject& o : full.objects)
    {
        bool fan = false;
        for(const char* id : kGroupFans)
        {
            fan = fan || o.id == id;
        }
        if(fan || (o.kind != ObjectKind::Device && o.kind != ObjectKind::Linked))
        {
            doc.objects.push_back(o);
        }
    }
    return doc;
}

struct Compiled
{
    bool        ok = false;
    std::string error;
    LianLiWireless::PlaybackLoop loop;
    double      render_ms   = 0.0;   /* EffectEngine, all attempts  */
    double      build_ms    = 0.0;   /* render + compress, total    */
    int         attempts    = 0;
    int         seam_delta  = 0;     /* max channel |frame(T) - frame(0)| */
};

/* One group frame at t into rgb[0 .. 360): fan k's emitter at
   address a lands on LED k*40 + a. */
void GroupFrame(const EffectEngine& engine, const SceneDocument& doc,
                const std::map<std::string, Mat4>& world, double t,
                FrameColors& frame, uint8_t* rgb)
{
    engine.Evaluate(doc, world, t, frame);
    for(size_t k = 0; k < 3; k++)
    {
        const SceneObject* obj = FindObject(doc, kGroupFans[k]);
        const auto it = frame.find(kGroupFans[k]);
        for(size_t i = 0; obj && i < obj->emitters.size(); i++)
        {
            /* An untouched fan shows its painted color. */
            SceneColor col = 0;
            if(it != frame.end())
            {
                col = it->second[i];
            }
            else
            {
                const auto base = doc.object_colors.find(obj->id);
                col = base != doc.object_colors.end() ? base->second : 0;
            }
            const int addr = obj->emitters[i].address >= 0
                           ? obj->emitters[i].address : (int)i;
            uint8_t* px = &rgb[(k * kLedsPerFan + addr) * 3];
            px[0] = (uint8_t)(col & 0xFF);
            px[1] = (uint8_t)((col >> 8) & 0xFF);
            px[2] = (uint8_t)((col >> 16) & 0xFF);
        }
    }
}

Compiled Compile(const LookPeriod& p, const SceneDocument& doc)
{
    Compiled c;
    EffectEngine engine;
    engine.SetLayers(p.runtime_layers);
    const std::map<std::string, Mat4> world = ResolveWorldMatrices(doc);
    FrameColors frame;
    const double T = p.is_static ? 0.0 : p.cycle_s;

    auto render = [&](size_t frames) {
        const auto t0 = std::chrono::steady_clock::now();
        ++c.attempts;
        std::vector<uint8_t> rgb(frames * kGroupLeds * 3, 0);
        for(size_t f = 0; f < frames; f++)
        {
            GroupFrame(engine, doc, world, T * (double)f / (double)frames, frame,
                       &rgb[f * kGroupLeds * 3]);
        }
        c.render_ms += std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - t0).count();
        return rgb;
    };

    const auto t0 = std::chrono::steady_clock::now();
    try
    {
        c.loop = LianLiWireless::BuildLoopFromRenderer(T * 1000.0, kGroupLeds, render);
        c.ok = true;
    }
    catch(const std::exception& e)
    {
        c.error = e.what();
    }
    c.build_ms = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - t0).count();

    /* Seamless? The frame one cycle on must equal frame 0. */
    std::vector<uint8_t> a(kGroupLeds * 3, 0), b(kGroupLeds * 3, 0);
    GroupFrame(engine, doc, world, 0.0, frame, a.data());
    GroupFrame(engine, doc, world, T, frame, b.data());
    for(size_t i = 0; i < a.size(); i++)
    {
        c.seam_delta = std::max(c.seam_delta, std::abs((int)a[i] - (int)b[i]));
    }
    return c;
}

std::string Pct(double v)
{
    char b[32];
    std::snprintf(b, sizeof(b), "%+.2f%%", v * 100.0);
    return b;
}

/* "T s | N | fps | bytes" cells for one analysis. */
std::string Cells(const LookPeriod& p, const Compiled& c)
{
    char b[256];
    if(!p.compilable)
    {
        return "refused | - | - | -";
    }
    if(!c.ok)
    {
        std::snprintf(b, sizeof(b), "%.3f | build failed: %s | - | -",
                      p.cycle_s, c.error.c_str());
        return b;
    }
    if(p.is_static)
    {
        std::snprintf(b, sizeof(b), "static | 1 | held | %zu",
                      c.loop.compressed.size());
        return b;
    }
    std::snprintf(b, sizeof(b), "%.3f | %u | %.1f | %zu", p.cycle_s,
                  c.loop.frame_count, c.loop.nominal_fps, c.loop.compressed.size());
    return b;
}

std::string Changes(const LookPeriod& p)
{
    std::string s;
    for(size_t i = 0; i < p.layers.size(); i++)
    {
        const LayerPeriod& l = p.layers[i];
        if(l.motion != LayerMotion::Periodic)
        {
            continue;
        }
        char b[96];
        std::snprintf(b, sizeof(b), "%sL%zu %s %.2fs x%d %s", s.empty() ? "" : "; ",
                      i, l.primitive.c_str(), l.period_s, l.repeats,
                      Pct(l.speed_change).c_str());
        s += b;
    }
    return s.empty() ? "-" : s;
}

std::string Periods(const LookPeriod& p)
{
    std::string s;
    for(size_t i = 0; i < p.layers.size(); i++)
    {
        const LayerPeriod& l = p.layers[i];
        char b[96];
        switch(l.motion)
        {
        case LayerMotion::Periodic:
            std::snprintf(b, sizeof(b), "%s %.3f s", l.primitive.c_str(), l.period_s);
            break;
        case LayerMotion::Static:
            std::snprintf(b, sizeof(b), "%s (still)", l.primitive.c_str());
            break;
        case LayerMotion::NotCompilable:
            std::snprintf(b, sizeof(b), "%s (refused)", l.primitive.c_str());
            break;
        default:
            continue;   /* does not reach the group */
        }
        s += (s.empty() ? "" : ", ");
        s += b;
    }
    return s.empty() ? "(nothing reaches the group)" : s;
}

double Median(std::vector<double> v)
{
    if(v.empty())
    {
        return 0.0;
    }
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
}

} /* namespace */

int main(int argc, char** argv)
{
    int seeds = 100;
    for(int i = 1; i < argc; i++)
    {
        if(std::strcmp(argv[i], "--seeds") == 0 && i + 1 < argc)
        {
            seeds = std::atoi(argv[++i]);
        }
    }
#ifdef OPENRGB2_EFFECT_DIR
    /* The packaged looks of this source tree unless overridden. */
    char*  existing = nullptr;
    size_t len      = 0;
    if(_dupenv_s(&existing, &len, "DLS_EFFECT_DIR") != 0 || existing == nullptr)
    {
        _putenv_s("DLS_EFFECT_DIR", OPENRGB2_EFFECT_DIR);
    }
    std::free(existing);
#endif

    const SceneDocument doc = GroupDoc();
    const std::vector<PresetInfo> looks = PresetList();

    std::printf("## Packaged looks, seed 0, 120-LED group\n\n");
    std::printf("Limits: %zu B compressed, %zu frames, 60 fps nominal "
                "(BuildLoopFromRenderer). Snapping: <= %.0f%% speed change, "
                "cycle <= %.0f s.\n\n",
                LianLiWireless::MAX_COMPRESSED_BYTES, LianLiWireless::MAX_FRAME_COUNT,
                kLookSnapMaxChange * 100.0, kLookMaxCycleSeconds);
    std::printf("| Look | Layers reaching the group | No snap: T s | N | fps | bytes "
                "| Snap: T s | N | fps | bytes | Snap speed changes | B/frame (snap) "
                "| Seam delta | Build ms (snap) |\n");
    std::printf("|---|---|---|---|---|---|---|---|---|---|---|---|---|---|\n");

    std::vector<std::string> notes;
    for(const PresetInfo& info : looks)
    {
        const std::vector<EffectLayer> layers = BuildPreset(info.id, 0);
        const LookPeriod plain = AnalyzeLookPeriod(layers, doc);
        const LookPeriod snap  = AnalyzeLookPeriod(layers, doc, SnapOptions());
        const Compiled cp = plain.compilable ? Compile(plain, doc) : Compiled{};
        const Compiled cs = snap.compilable
                          ? (plain.compilable ? cp : Compile(snap, doc)) : Compiled{};
        char bpf[64] = "-", seam[32] = "-", ms[64] = "-";
        if(cs.ok)
        {
            std::snprintf(bpf, sizeof(bpf), "%.0f",
                          (double)cs.loop.compressed.size() / cs.loop.frame_count);
            std::snprintf(seam, sizeof(seam), "%d", cs.seam_delta);
            std::snprintf(ms, sizeof(ms), "%.0f (%d %s)", cs.build_ms, cs.attempts,
                          cs.attempts == 1 ? "pass" : "passes");
        }
        std::printf("| %s | %s | %s | %s | %s | %s | %s | %s |\n", info.id.c_str(),
                    Periods(plain).c_str(), Cells(plain, cp).c_str(),
                    Cells(snap, cs).c_str(),
                    snap.snapped ? Changes(snap).c_str() : "-", bpf, seam, ms);
        if(!snap.compilable)
        {
            char b[320];
            std::snprintf(b, sizeof(b), "- `%s`: %s", info.id.c_str(), snap.reason.c_str());
            notes.push_back(b);
        }
        else if(!plain.compilable)
        {
            char b[320];
            std::snprintf(b, sizeof(b), "- `%s`: without snapping: %s", info.id.c_str(),
                          plain.reason.c_str());
            notes.push_back(b);
        }
    }
    std::printf("\nRefusals:\n\n");
    for(const std::string& n : notes)
    {
        std::printf("%s\n", n.c_str());
    }

    /*---- What would it take: periodic looks refused at 20 s / 5% ----*/
    std::printf("\n## Sensitivity: looks with only periodic layers that snapping "
                "at 5%% / 20 s still refuses\n\n");
    std::printf("| Look | Max cycle s | Max change | T s | change | N | fps | bytes "
                "| Build ms |\n");
    std::printf("|---|---|---|---|---|---|---|---|---|\n");
    for(const PresetInfo& info : looks)
    {
        const std::vector<EffectLayer> layers = BuildPreset(info.id, 0);
        const LookPeriod snap = AnalyzeLookPeriod(layers, doc, SnapOptions());
        bool periodic_only = !snap.compilable;
        for(const LayerPeriod& l : snap.layers)
        {
            periodic_only = periodic_only && l.motion != LayerMotion::NotCompilable;
        }
        if(!periodic_only)
        {
            continue;
        }
        for(double max_cycle : { 20.0, 30.0, 45.0, 60.0 })
        {
            for(double max_change : { 0.05, 0.10 })
            {
                LookPeriodOptions o;
                o.max_cycle_s = max_cycle;
                o.max_change  = max_change;
                const LookPeriod p = AnalyzeLookPeriod(layers, doc, o);
                if(!p.compilable)
                {
                    std::printf("| %s | %.0f | %.0f%% | refused (best %.2f%% at %.2f s) "
                                "| - | - | - | - | - |\n", info.id.c_str(), max_cycle,
                                max_change * 100.0, p.best_change * 100.0, p.best_cycle_s);
                    continue;
                }
                const Compiled c = Compile(p, doc);
                std::printf("| %s | %.0f | %.0f%% | %.3f | %.2f%% | %u | %.1f | %zu "
                            "| %.0f |\n",
                            info.id.c_str(), max_cycle, max_change * 100.0, p.cycle_s,
                            p.max_change * 100.0, c.ok ? c.loop.frame_count : 0,
                            c.ok ? c.loop.nominal_fps : 0.0,
                            c.ok ? c.loop.compressed.size() : (size_t)0, c.build_ms);
            }
        }
    }

    /*---- Cycle length vs fps: the compilable looks slowed down ----*/
    std::printf("\n## Cycle length vs fps: compilable looks at other global speeds "
                "(seed 0)\n\n");
    std::printf("EffectState.speed scales every layer, so T scales by 1/speed "
                "and the content per cycle is unchanged.\n\n");
    std::printf("| Look | Global speed | T s | N | fps | bytes | B/frame | Build ms |\n");
    std::printf("|---|---|---|---|---|---|---|---|\n");
    for(const PresetInfo& info : looks)
    {
        const std::vector<EffectLayer> layers = BuildPreset(info.id, 0);
        if(!AnalyzeLookPeriod(layers, doc).compilable)
        {
            continue;
        }
        for(float speed : { 2.0f, 1.0f, 0.5f, 0.25f, 0.125f })
        {
            SceneDocument slowed = doc;
            slowed.effect.speed = speed;
            const LookPeriod p = AnalyzeLookPeriod(layers, slowed);
            if(!p.compilable)
            {
                std::printf("| %s | %.3f | refused: %s | - | - | - | - | - |\n",
                            info.id.c_str(), speed, p.reason.c_str());
                continue;
            }
            const Compiled c = Compile(p, slowed);
            std::printf("| %s | %.3f | %.3f | %u | %.1f | %zu | %.0f | %.0f |\n",
                        info.id.c_str(), speed, p.cycle_s, c.loop.frame_count,
                        c.loop.nominal_fps, c.loop.compressed.size(),
                        (double)c.loop.compressed.size() / c.loop.frame_count, c.build_ms);
        }
    }

    /*---- Baseline: today's nine presets on the same group ----*/
    std::printf("\n## Baseline: the nine backend presets on the SL V3 group "
                "(BuildPlaybackLoop, 40 LEDs/fan, SLV3 tables, white)\n\n");
    std::printf("| Preset | speed 100 (2.4 s): N | fps | bytes | speed 25 (9.6 s): N "
                "| fps | bytes | speed 10 (24 s): N | fps | bytes |\n");
    std::printf("|---|---|---|---|---|---|---|---|---|---|\n");
    using LianLiWireless::PlaybackPreset;
    using LianLiWireless::WirelessFanType;
    static const struct { PlaybackPreset p; const char* name; } presets[] = {
        { PlaybackPreset::SpectrumCycle,     "SpectrumCycle" },
        { PlaybackPreset::RainbowWave,       "RainbowWave" },
        { PlaybackPreset::Breathing,         "Breathing" },
        { PlaybackPreset::PerimeterWave,     "PerimeterWave" },
        { PlaybackPreset::PerimeterComet,    "PerimeterComet" },
        { PlaybackPreset::PerimeterChase,    "PerimeterChase" },
        { PlaybackPreset::PerimeterPingPong, "PerimeterPingPong" },
        { PlaybackPreset::StackWave,         "StackWave" },
        { PlaybackPreset::StackRainbow,      "StackRainbow" },
    };
    for(const auto& pr : presets)
    {
        std::printf("| %s", pr.name);
        for(unsigned speed : { 100u, 25u, 10u })
        {
            LianLiWireless::PlaybackRequest r;
            r.preset       = pr.p;
            r.led_count    = kGroupLeds;
            r.speed        = speed;
            r.leds_per_fan = kLedsPerFan;
            r.led_order    = LianLiWireless::FanLedOrder(WirelessFanType::Slv3Led);
            r.stack_pos    = LianLiWireless::FanStackPos(WirelessFanType::Slv3Led);
            r.led_height   = LianLiWireless::FanLedHeight(WirelessFanType::Slv3Led);
            r.red = 255; r.green = 255; r.blue = 255;
            const LianLiWireless::PlaybackLoop loop = LianLiWireless::BuildPlaybackLoop(r);
            std::printf(" | %u | %.1f | %zu", loop.frame_count, loop.nominal_fps,
                        loop.compressed.size());
        }
        std::printf(" |\n");
    }

    /*---- Remix seed sweep ----*/
    if(seeds > 0)
    {
        std::printf("\n## Remix seed sweep: seeds 0..%d, 120-LED group\n\n", seeds - 1);
        std::printf("| Look | Compilable no snap | Compilable snap | T s min / median / max "
                    "(snap) | fps min / median / max | bytes min / median / max "
                    "| 60 fps share |\n");
        std::printf("|---|---|---|---|---|---|---|\n");
        for(const PresetInfo& info : looks)
        {
            int plain_ok = 0, snap_ok = 0, at60 = 0;
            std::vector<double> T, fps, bytes;
            for(int s = 0; s < seeds; s++)
            {
                const std::vector<EffectLayer> layers = BuildPreset(info.id, (unsigned)s);
                const LookPeriod plain = AnalyzeLookPeriod(layers, doc);
                const LookPeriod snap  = AnalyzeLookPeriod(layers, doc, SnapOptions());
                plain_ok += plain.compilable ? 1 : 0;
                if(!snap.compilable)
                {
                    continue;
                }
                ++snap_ok;
                const Compiled c = Compile(snap, doc);
                if(!c.ok)
                {
                    continue;
                }
                T.push_back(snap.is_static ? 0.0 : snap.cycle_s);
                fps.push_back(snap.is_static ? 60.0 : c.loop.nominal_fps);
                bytes.push_back((double)c.loop.compressed.size());
                at60 += (snap.is_static || c.loop.nominal_fps >= 59.0) ? 1 : 0;
            }
            if(T.empty())
            {
                std::printf("| %s | %d/%d | %d/%d | - | - | - | - |\n", info.id.c_str(),
                            plain_ok, seeds, snap_ok, seeds);
                continue;
            }
            std::printf("| %s | %d/%d | %d/%d | %.2f / %.2f / %.2f | %.1f / %.1f / %.1f "
                        "| %.0f / %.0f / %.0f | %d/%zu |\n",
                        info.id.c_str(), plain_ok, seeds, snap_ok, seeds,
                        *std::min_element(T.begin(), T.end()), Median(T),
                        *std::max_element(T.begin(), T.end()),
                        *std::min_element(fps.begin(), fps.end()), Median(fps),
                        *std::max_element(fps.begin(), fps.end()),
                        *std::min_element(bytes.begin(), bytes.end()), Median(bytes),
                        *std::max_element(bytes.begin(), bytes.end()), at60, T.size());
        }
    }
    return 0;
}
