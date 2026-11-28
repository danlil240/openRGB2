/*---------------------------------------------------------*\
||| LookPeriod.cpp                                            |
|||                                                           |
|||   SPDX-License-Identifier: GPL-2.0-or-later               |
\*---------------------------------------------------------*/

#include "LookPeriod.h"

#include "../effects/Presets.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace studio
{

/* A period this short (faster than 1000 cycles/s) is not a look,
   and it would make the switching-point scan below unbounded. */
static constexpr double kMinPeriodSeconds = 1e-3;

static bool ReachesDocument(const EffectLayer& L, const SceneDocument& doc)
{
    /* EffectEngine::Evaluate's reach: Device owners with emitters
       (Linked copies display their owner's frame). */
    for(const SceneObject& obj : doc.objects)
    {
        if(obj.kind != ObjectKind::Device)
        {
            continue;
        }
        for(const Emitter& e : obj.emitters)
        {
            if(LayerMatches(L, obj, e))
            {
                return true;
            }
        }
    }
    return false;
}

static LayerPeriod ClassifyLayer(const EffectLayer& L, const SceneDocument& doc)
{
    LayerPeriod out;
    out.primitive = L.primitive;
    if(!L.enabled || !ReachesDocument(L, doc))
    {
        out.motion = LayerMotion::Unused;
        return out;
    }
    /* An invisible layer (opacity 0, e.g. intensity 0) composites
       as the identity in every blend mode. */
    if(!(L.opacity > 0.0f))
    {
        out.motion = LayerMotion::Static;
        return out;
    }

    const PreparedLayer P = PrepareLayer(L);
    switch(P.prim)
    {
    case Primitive::Ripple:
    case Primitive::ScreenField:
    case Primitive::Level:
        out.motion = LayerMotion::NotCompilable;
        out.reason = L.primitive + " layer reads live input";
        return out;
    case Primitive::Static:
        out.motion = LayerMotion::Static;
        return out;
    default:
        break;
    }

    if(!std::isfinite(L.speed))
    {
        out.motion = LayerMotion::NotCompilable;
        out.reason = L.primitive + " layer has a non-finite speed";
        return out;
    }
    if(L.speed == 0.0f)
    {
        out.motion = LayerMotion::Static;
        return out;
    }
    const double speed = std::fabs((double)L.speed);

    double period = 0.0;
    switch(P.prim)
    {
    case Primitive::Noise:
        out.motion = LayerMotion::NotCompilable;
        out.reason = "noise layer drifts without repeating";
        return out;
    case Primitive::Wave:
    case Primitive::Pulse:
        /* phase = t * speed / scale, 1-periodic */
        if(!(std::fabs(L.scale) > 0.0f) || !std::isfinite(L.scale))
        {
            out.motion = LayerMotion::NotCompilable;
            out.reason = L.primitive + " layer has a zero or non-finite scale";
            return out;
        }
        period = std::fabs((double)L.scale) / speed;
        break;
    case Primitive::Gradient:
    case Primitive::Spin:
        /* phase = t * speed, 1-periodic */
        period = 1.0 / speed;
        break;
    case Primitive::Comet:
        /* head = t * speed mod path length (the float length the
           evaluator itself uses); a degenerate path draws nothing */
        if(P.path_len < 1e-6f)
        {
            out.motion = LayerMotion::Static;
            return out;
        }
        period = (double)P.path_len / speed;
        break;
    default:
        out.motion = LayerMotion::Static;
        return out;
    }

    if(!std::isfinite(period) || period < kMinPeriodSeconds)
    {
        out.motion = LayerMotion::NotCompilable;
        out.reason = L.primitive + " layer moves too fast to sample";
        return out;
    }
    out.motion   = LayerMotion::Periodic;
    out.period_s = period;
    return out;
}

/*---------------------------------------------------------*\
||| Common-cycle search                                     |
|||                                                          |
||| For a cycle T, layer j best uses k_j = round(T / p_j)    |
||| (>= 1) repeats; its relative speed change is             |
||| k_j p_j / T - 1. The worst change over the layers,       |
||| f(T), only switches k-sets at T = (k + 1/2) p_j. Between |
||| two switching points the k-set is fixed and f is         |
||| quasi-convex in T, minimised at T = (a_min + a_max)/2    |
||| (a_j = k_j p_j; the slowest-needing and fastest-needing  |
||| layers change by the same amount), clamped to the        |
||| interval. Scanning every interval is exact.              |
\*---------------------------------------------------------*/
namespace
{
struct Candidate
{
    double           cycle;
    double           change;
    std::vector<int> repeats;
};
}

static double ChangeAt(const std::vector<double>& p, const std::vector<int>& k,
                       double T)
{
    double worst = 0.0;
    for(size_t j = 0; j < p.size(); j++)
    {
        worst = std::max(worst, std::fabs(k[j] * p[j] / T - 1.0));
    }
    return worst;
}

CycleFit FitCommonCycle(const std::vector<double>& p, const LookPeriodOptions& opts)
{
    CycleFit fit;
    if(p.empty() || !(opts.max_cycle_s > 0.0))
    {
        return fit;
    }
    for(double v : p)
    {
        if(!(v >= kMinPeriodSeconds) || !std::isfinite(v))
        {
            return fit;
        }
    }

    const double p_max = *std::max_element(p.begin(), p.end());
    const double hi_T  = opts.max_cycle_s;
    /* Below p_max / 2 the slowest layer would need >= 100%. */
    const double lo_T  = std::min(p_max / 2.0, hi_T);

    std::vector<double> cuts;
    cuts.push_back(lo_T);
    for(double pj : p)
    {
        for(long k = 1;; k++)
        {
            const double s = (k + 0.5) * pj;
            if(s >= hi_T)
            {
                break;
            }
            if(s > lo_T)
            {
                cuts.push_back(s);
            }
        }
    }
    cuts.push_back(hi_T);
    std::sort(cuts.begin(), cuts.end());
    cuts.erase(std::unique(cuts.begin(), cuts.end()), cuts.end());

    std::vector<Candidate> cands;
    const size_t intervals = cuts.size() > 1 ? cuts.size() - 1 : 1;
    cands.reserve(intervals);
    for(size_t i = 0; i < intervals; i++)
    {
        const double lo  = cuts[i];
        const double hi  = cuts.size() > 1 ? cuts[i + 1] : cuts[i];
        const double mid = 0.5 * (lo + hi);
        Candidate c;
        c.repeats.resize(p.size());
        double a_min = 1e300, a_max = 0.0;
        for(size_t j = 0; j < p.size(); j++)
        {
            c.repeats[j] = (int)std::max(1L, std::lround(mid / p[j]));
            const double a = c.repeats[j] * p[j];
            a_min = std::min(a_min, a);
            a_max = std::max(a_max, a);
        }
        c.cycle  = std::min(hi, std::max(lo, 0.5 * (a_min + a_max)));
        c.change = ChangeAt(p, c.repeats, c.cycle);
        cands.push_back(std::move(c));
    }

    const Candidate* best = &cands.front();
    for(const Candidate& c : cands)
    {
        if(c.change < best->change)
        {
            best = &c;
        }
    }
    fit.best_change  = best->change;
    fit.best_cycle_s = best->cycle;

    /* Pass 1 (no snapping): the shortest cycle within tolerance. */
    const Candidate* pick = nullptr;
    const double eps = 1e-12;
    for(const Candidate& c : cands)
    {
        if(c.change <= opts.tolerance + eps)
        {
            pick = &c;
            break;
        }
    }
    /* Pass 2 (snapping, opt-in): the smallest change; cycles whose
       change is within `tolerance` of it count as equally good, and
       the shortest of those wins (more fps, smaller payload). */
    if(pick == nullptr && opts.max_change > opts.tolerance
       && best->change <= opts.max_change + eps)
    {
        const double bar = std::min(opts.max_change, best->change + opts.tolerance);
        for(const Candidate& c : cands)
        {
            if(c.change <= bar + eps)
            {
                pick = &c;
                break;
            }
        }
    }
    if(pick == nullptr)
    {
        return fit;
    }

    fit.found      = true;
    fit.cycle_s    = pick->cycle;
    fit.max_change = pick->change;
    fit.repeats    = pick->repeats;
    fit.speed_change.resize(p.size());
    for(size_t j = 0; j < p.size(); j++)
    {
        fit.speed_change[j] = pick->repeats[j] * p[j] / pick->cycle - 1.0;
    }
    return fit;
}

/*---------------------------------------------------------*\
||| Look analysis                                           |
\*---------------------------------------------------------*/
LookPeriod AnalyzeLookPeriod(const std::vector<EffectLayer>& layers,
                             const SceneDocument& doc,
                             const LookPeriodOptions& opts)
{
    LookPeriod out;
    std::vector<EffectLayer> folded = layers;
    ApplyGlobalParams(folded, doc.effect.speed, doc.effect.intensity);

    std::vector<double> periods;
    std::vector<size_t> periodic_index;
    for(size_t i = 0; i < folded.size(); i++)
    {
        out.layers.push_back(ClassifyLayer(folded[i], doc));
        const LayerPeriod& lp = out.layers.back();
        if(lp.motion == LayerMotion::NotCompilable && out.reason.empty())
        {
            out.reason = lp.reason;
        }
        if(lp.motion == LayerMotion::Periodic)
        {
            periods.push_back(lp.period_s);
            periodic_index.push_back(i);
        }
    }
    if(!out.reason.empty())
    {
        return out;
    }
    if(periods.empty())
    {
        out.compilable     = true;
        out.is_static      = true;
        out.runtime_layers = std::move(folded);
        return out;
    }

    const CycleFit fit = FitCommonCycle(periods, opts);
    out.best_change  = fit.best_change;
    out.best_cycle_s = fit.best_cycle_s;
    if(!fit.found)
    {
        char buf[160];
        std::snprintf(buf, sizeof(buf),
                      "layer speeds do not share a short common cycle "
                      "(<= %g s; the closest needs a %.1f%% speed change)",
                      opts.max_cycle_s, fit.best_change * 100.0);
        out.reason = buf;
        return out;
    }

    out.compilable = true;
    out.cycle_s    = fit.cycle_s;
    out.max_change = fit.max_change;
    out.snapped    = fit.max_change > opts.tolerance + 1e-12;
    for(size_t j = 0; j < periodic_index.size(); j++)
    {
        const size_t i = periodic_index[j];
        out.layers[i].repeats      = fit.repeats[j];
        out.layers[i].speed_change = fit.speed_change[j];
        /* period' = T / k  <=>  speed' = speed * (k p / T) */
        folded[i].speed = (float)((double)folded[i].speed * (1.0 + fit.speed_change[j]));
    }
    out.runtime_layers = std::move(folded);
    return out;
}

std::optional<double> LookCycleSeconds(const std::vector<EffectLayer>& layers,
                                       const SceneDocument& doc)
{
    const LookPeriod p = AnalyzeLookPeriod(layers, doc);
    if(!p.compilable)
    {
        return std::nullopt;
    }
    return p.cycle_s;
}

size_t NominalFrameCount(const LookPeriod& period)
{
    if(!period.compilable)
    {
        return 0;
    }
    if(period.is_static)
    {
        return 1;
    }
    const long n = std::lround(period.cycle_s * kLookNominalFps);
    return (size_t)std::min(4096L, std::max(2L, n));
}

} /* namespace studio */
