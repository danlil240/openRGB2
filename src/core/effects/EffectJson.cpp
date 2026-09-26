/*---------------------------------------------------------*\
|||| EffectJson.cpp                                            |
||||                                                           |
||||   SPDX-License-Identifier: GPL-2.0-or-later               |
\*---------------------------------------------------------*/

#include "EffectJson.h"

#include "Presets.h"                /* RemixRng / RotateYaw — the one draw stream */
#include "../presets/DevicePreset.h"   /* IsPresetId — id == filename */
#include "../scene/JsonFields.h"
#include "../scene/SceneJson.h"     /* SceneColorHex / ParseSceneColor */

#include <cmath>
#include <fstream>
#include <limits>
#include <sstream>

namespace studio
{
namespace
{

/*---------------------------------------------------------*\
|||| Small json-type-agnostic readers — the checked       ||
|||| JsonFields.h readers bind to nlohmann::json, but      ||
|||| effect documents parse as ordered_json so remix      ||
|||| draws keep file order. These mirror them over any    ||
|||| basic_json<...> instantiation.                       |
\*---------------------------------------------------------*/
template<typename J>
bool JStr(const J& j, const char* key, std::string& out,
          const std::string& path, std::vector<std::string>& errs)
{
    const auto it = j.find(key);
    if(it == j.end())
    {
        return true;            /* absent: caller keeps its default */
    }
    if(!it->is_string())
    {
        AddErr(errs, path + "." + key, "expected string");
        return false;
    }
    out = it->template get<std::string>();
    return true;
}

/* IsInt is json-bound; ordered_json needs its own. */
template<typename J>
bool JIsInt(const J& v)
{
    return v.is_number_integer() || v.is_number_unsigned();
}

bool IsSpecKey(const std::string& k)
{
    static const char* const keys[] = {
        "remix", "remix01", "remix_angle", "remix_u32",
        "remix_neg", "remix_pick", "remix_yaw",
    };
    for(const char* s : keys)
    {
        if(k == s)
        {
            return true;
        }
    }
    return false;
}

/* Count the remix-spec keys on a value (spec objects carry
   exactly one). */
template<typename J>
int SpecKeyCount(const J& v)
{
    if(!v.is_object())
    {
        return 0;
    }
    int n = 0;
    for(auto it = v.begin(); it != v.end(); ++it)
    {
        if(IsSpecKey(it.key()))
        {
            ++n;
        }
    }
    return n;
}

template<typename J>
bool IsSpec(const J& v)
{
    return v.is_object() && SpecKeyCount(v) == 1;
}

/* A bounded [lo,hi] pair read from a 2-element array. */
template<typename J>
bool RangeSpec(const J& v, double& lo, double& hi,
               const std::string& path, std::vector<std::string>& errs)
{
    if(!v.is_array() || v.size() != 2 || !v[0].is_number()
       || !v[1].is_number())
    {
        AddErr(errs, path, "expected [lo,hi] numbers");
        return false;
    }
    lo = v[0].template get<double>();
    hi = v[1].template get<double>();
    if(!std::isfinite(lo) || !std::isfinite(hi))
    {
        AddErr(errs, path, "expected finite bounds");
        return false;
    }
    if(lo > hi)
    {
        AddErr(errs, path, "remix bounds: lo > hi");
        return false;
    }
    return true;
}

/* Two finite numbers (alternatives / bounds without ordering). */
template<typename J>
bool NumPair(const J& v, double& a, double& b,
             const std::string& path, std::vector<std::string>& errs)
{
    if(!v.is_array() || v.size() != 2 || !v[0].is_number()
       || !v[1].is_number())
    {
        AddErr(errs, path, "expected [a,b] numbers");
        return false;
    }
    a = v[0].template get<double>();
    b = v[1].template get<double>();
    if(!std::isfinite(a) || !std::isfinite(b))
    {
        AddErr(errs, path, "expected finite numbers");
        return false;
    }
    return true;
}

/* One scalar draw: draws `out` (0 in validate mode — rng may be
   null) and reports the spec's implied [imp_lo,imp_hi] domain so
   field bounds hold for every seed, not just the drawn one. */
template<typename J>
bool ScalarSpec(const J& v, RemixRng* rng, double& out,
                double& imp_lo, double& imp_hi,
                const std::string& path, std::vector<std::string>& errs)
{
    /* Exactly one key AND it must be a spec key: SpecKeyCount
       alone counted only spec keys, so {"remix":[..],"foo":2}
       passed and silently dropped the extra key. */
    if(v.size() != 1 || SpecKeyCount(v) != 1)
    {
        AddErr(errs, path, "expected one remix spec key");
        return false;
    }
    const std::string key = v.begin().key();
    const J& spec = v.begin().value();
    const std::string sp = path + "." + key;

    auto flag = [&]() -> bool
    {
        if(!spec.is_boolean() || !spec.template get<bool>())
        {
            AddErr(errs, sp, "expected true");
            return false;
        }
        return true;
    };

    if(key == "remix")
    {
        double lo, hi;
        if(!RangeSpec(spec, lo, hi, sp, errs))
        {
            return false;
        }
        imp_lo = lo; imp_hi = hi;
        out = rng ? (double)rng->Range((float)lo, (float)hi) : 0.0;
        return true;
    }
    if(key == "remix_neg")
    {
        double lo, hi;
        if(!RangeSpec(spec, lo, hi, sp, errs))
        {
            return false;
        }
        imp_lo = -hi; imp_hi = -lo;
        out = rng ? -(double)rng->Range((float)lo, (float)hi) : 0.0;
        return true;
    }
    if(key == "remix01")
    {
        if(!flag())
        {
            return false;
        }
        imp_lo = 0.0; imp_hi = 1.0;
        out = rng ? (double)rng->Next01() : 0.0;
        return true;
    }
    if(key == "remix_angle")
    {
        if(!flag())
        {
            return false;
        }
        imp_lo = 0.0; imp_hi = 6.2831853;
        out = rng ? (double)rng->Angle() : 0.0;
        return true;
    }
    if(key == "remix_pick")
    {
        double a, b;
        if(!NumPair(spec, a, b, sp, errs))
        {
            return false;
        }
        imp_lo = a < b ? a : b;
        imp_hi = a < b ? b : a;
        /* Next01() < 0.5 ? a : b — reproduces the C++ spokes
           draw exactly. */
        const float u = rng ? rng->Next01() : 0.0f;
        out = u < 0.5f ? a : b;
        return true;
    }
    AddErr(errs, path, "remix spec '" + key + "' not valid for a"
                       " scalar field");
    return false;
}

/* Scalar value: literal number or scalar remix spec -> double
   plus the value's implied domain ([d,d] for literals, the
   spec's full range for remix) so bounds hold for every seed.
   Domain checks stay with the caller. */
template<typename J>
bool NumValue(const J& v, double& d, double& imp_lo, double& imp_hi,
              RemixRng* rng, const std::string& path,
              std::vector<std::string>& errs)
{
    if(v.is_number())
    {
        d = v.template get<double>();
        if(!std::isfinite(d))
        {
            AddErr(errs, path, "expected finite number");
            return false;
        }
        imp_lo = imp_hi = d;
        return true;
    }
    if(v.is_object())
    {
        if(rng == nullptr)
        {
            AddErr(errs, path, "remix spec in resolved layers"
                               " — literals only");
            return false;
        }
        return ScalarSpec(v, rng, d, imp_lo, imp_hi, path, errs);
    }
    AddErr(errs, path, "expected number or remix spec");
    return false;
}

template<typename J>
bool NumField(const J& v, float& out, double lo, double hi,
              RemixRng* rng, const std::string& path,
              std::vector<std::string>& errs)
{
    double d = 0.0, il = 0.0, ih = 0.0;
    if(!NumValue(v, d, il, ih, rng, path, errs))
    {
        return false;
    }
    if(d < lo || d > hi || il < lo || ih > hi)
    {
        AddErr(errs, path, "out of range " + std::to_string(lo)
                           + ".." + std::to_string(hi));
        return false;
    }
    /* Narrow to float THEN check finiteness — a finite double
       inside the declared bounds (speed/phase/density allow up
       to DBL_MAX) still overflows to inf at float width, and a
       remix spec whose implied domain doesn't fit float would
       produce inf for some seed. Non-finite is an error, not a
       clamp — same rule PathField already applies. */
    out = (float)d;
    if(!std::isfinite(out) || !std::isfinite((float)il)
       || !std::isfinite((float)ih))
    {
        AddErr(errs, path, "expected float-finite number");
        return false;
    }
    return true;
}

/* [x,y,z] or, for direction fields, {"remix_yaw":[x,y,z,lo,hi]}.
   Element specs draw in array order. */
template<typename J>
bool VecField(const J& v, Vec3& out, bool allow_yaw, RemixRng* rng,
              const std::string& path, std::vector<std::string>& errs)
{
    if(v.is_object())
    {
        if(rng == nullptr)
        {
            AddErr(errs, path, "remix spec in resolved layers"
                               " — literals only");
            return false;
        }
        if(v.size() != 1 || SpecKeyCount(v) != 1
           || !v.contains("remix_yaw"))
        {
            AddErr(errs, path, "expected one remix spec key"
                               " (vectors support remix_yaw only)");
            return false;
        }
        if(!allow_yaw)
        {
            AddErr(errs, path, "remix_yaw not valid here");
            return false;
        }
        const J& spec = v["remix_yaw"];
        const std::string sp = path + ".remix_yaw";
        if(!spec.is_array() || spec.size() != 5)
        {
            AddErr(errs, sp, "expected [x,y,z,lo,hi]");
            return false;
        }
        double n[5];
        for(int i = 0; i < 5; i++)
        {
            if(!spec[i].is_number()
               || !std::isfinite(n[i] = spec[i].template get<double>()))
            {
                AddErr(errs, sp, "expected finite [x,y,z,lo,hi]");
                return false;
            }
        }
        if(n[3] > n[4])
        {
            AddErr(errs, sp, "remix bounds: lo > hi");
            return false;
        }
        /* Narrow to float and re-check — a finite double (1e300)
           still overflows to inf at float width and would poison
           Normalize/Range with inf/NaN. */
        const float fn[5] = { (float)n[0], (float)n[1], (float)n[2],
                              (float)n[3], (float)n[4] };
        for(int i = 0; i < 5; i++)
        {
            if(!std::isfinite(fn[i]))
            {
                AddErr(errs, sp,
                       "expected float-finite [x,y,z,lo,hi]");
                return false;
            }
        }
        if(rng != nullptr)
        {
            const float a = rng->Range(fn[3], fn[4]);
            out = RotateYaw(Normalize({ fn[0], fn[1], fn[2] }), a);
        }
        return true;
    }
    if(!v.is_array() || v.size() != 3)
    {
        AddErr(errs, path, "expected [x,y,z] array");
        return false;
    }
    Vec3 r;
    float* comps[] = { &r.x, &r.y, &r.z };
    for(int i = 0; i < 3; i++)
    {
        const std::string ep = path + "[" + std::to_string(i) + "]";
        const J& e = v[i];
        if(e.is_number())
        {
            const double d = e.template get<double>();
            const float  f = (float)d;
            if(!std::isfinite(d) || !std::isfinite(f))
            {
                AddErr(errs, ep, "expected finite number");
                return false;
            }
            *comps[i] = f;
            continue;
        }
        if(e.is_object())
        {
            if(rng == nullptr)
            {
                AddErr(errs, ep, "remix spec in resolved layers"
                                   " — literals only");
                return false;
            }
            double d = 0.0, il = 0.0, ih = 0.0;
            if(!ScalarSpec(e, rng, d, il, ih, ep, errs))
            {
                return false;
            }
            /* Same narrowing rule as NumField: the drawn value and
               the spec's implied domain must survive float width. */
            const float f = (float)d;
            if(!std::isfinite(f) || !std::isfinite((float)il)
               || !std::isfinite((float)ih))
            {
                AddErr(errs, ep, "expected float-finite number");
                return false;
            }
            *comps[i] = f;
            continue;
        }
        AddErr(errs, ep, "expected number or remix spec");
        return false;
    }
    out = r;
    return true;
}

template<typename J>
bool PathField(const J& v, std::vector<Vec3>& out,
               const std::string& path, std::vector<std::string>& errs)
{
    if(!v.is_array())
    {
        AddErr(errs, path, "expected array of [x,y,z]");
        return false;
    }
    if(v.size() > EFFECT_MAX_PATH)
    {
        AddErr(errs, path, "count exceeds cap "
               + std::to_string(EFFECT_MAX_PATH));
        return false;
    }
    std::vector<Vec3> pts;
    int i = 0;
    for(const J& jp : v)
    {
        const std::string pp = path + "[" + std::to_string(i++) + "]";
        Vec3 p;
        if(!jp.is_array() || jp.size() != 3)
        {
            AddErr(errs, pp, "expected [x,y,z]");
            return false;
        }
        for(int k = 0; k < 3; k++)
        {
            if(!jp[k].is_number())
            {
                AddErr(errs, pp, "expected numbers");
                return false;
            }
        }
        p.x = (float)jp[0].template get<double>();
        p.y = (float)jp[1].template get<double>();
        p.z = (float)jp[2].template get<double>();
        if(!std::isfinite(p.x) || !std::isfinite(p.y)
           || !std::isfinite(p.z))
        {
            AddErr(errs, pp, "expected finite numbers");
            return false;
        }
        pts.push_back(p);
    }
    out = pts;
    return true;
}

template<typename J>
bool PaletteField(const J& v, Palette& out, RemixRng* rng,
                  const std::string& path, std::vector<std::string>& errs)
{
    if(!v.is_array())
    {
        AddErr(errs, path, "expected array of stops");
        return false;
    }
    if(v.empty())
    {
        /* A present-but-empty palette is an error — a layer that
           never samples color carries no palette key at all. */
        AddErr(errs, path, "empty palette — omit the key instead");
        return false;
    }
    if(v.size() > EFFECT_MAX_STOPS)
    {
        AddErr(errs, path, "count exceeds cap "
               + std::to_string(EFFECT_MAX_STOPS));
        return false;
    }
    Palette p;
    double last = -1.0;
    bool   ordered = true;
    int i = 0;
    for(const J& js : v)
    {
        const std::string sp = path + "[" + std::to_string(i++) + "]";
        if(!js.is_object())
        {
            AddErr(errs, sp, "expected {pos,color} object");
            return false;
        }
        for(auto it = js.begin(); it != js.end(); ++it)
        {
            if(it.key() != "pos" && it.key() != "color")
            {
                AddErr(errs, sp + "." + it.key(), "unknown key");
                return false;
            }
        }
        PaletteStop stop;
        if(!js.contains("pos") || !js.contains("color"))
        {
            AddErr(errs, sp, "needs pos and color");
            return false;
        }
        float pos = 0.0f;
        if(!NumField(js["pos"], pos, 0.0, 1.0, rng, sp + ".pos",
                     errs))
        {
            return false;
        }
        stop.pos = pos;
        const J& jc = js["color"];
        if(!jc.is_string())
        {
            AddErr(errs, sp + ".color", "expected \"#RRGGBB\"");
            return false;
        }
        /* ParseSceneColor binds to nlohmann::json; a wrapped copy
           keeps the ordered_json document working. */
        SceneColor c = 0;
        const nlohmann::json jv = jc.template get<std::string>();
        if(!ParseSceneColor(jv, c))
        {
            AddErr(errs, sp + ".color", "expected \"#RRGGBB\"");
            return false;
        }
        stop.color = ToColorF(c);
        if(stop.pos <= last)
        {
            ordered = false;
        }
        last = stop.pos;
        p.stops.push_back(stop);
    }
    if(!ordered)
    {
        AddErr(errs, path, "stop positions must be strictly"
                           " increasing");
        return false;
    }
    out = p;
    return true;
}

template<typename J>
bool TargetsField(const J& v, std::vector<std::string>& out,
                  const std::string& path, std::vector<std::string>& errs)
{
    if(!v.is_array())
    {
        AddErr(errs, path, "expected array of strings");
        return false;
    }
    if(v.size() > EFFECT_MAX_TARGETS)
    {
        AddErr(errs, path, "count exceeds cap "
               + std::to_string(EFFECT_MAX_TARGETS));
        return false;
    }
    std::vector<std::string> ts;
    int i = 0;
    for(const J& jt : v)
    {
        const std::string tp = path + "[" + std::to_string(i++) + "]";
        if(!jt.is_string() || jt.template get<std::string>().empty())
        {
            AddErr(errs, tp, "expected non-empty string");
            return false;
        }
        ts.push_back(jt.template get<std::string>());
    }
    out = ts;
    return true;
}

template<typename J>
bool SeedField(const J& v, unsigned int& out, RemixRng* rng,
               const std::string& path, std::vector<std::string>& errs)
{
    if(v.is_object())
    {
        if(rng == nullptr)
        {
            AddErr(errs, path, "remix spec in resolved layers"
                               " — literals only");
            return false;
        }
        if(v.size() != 1 || SpecKeyCount(v) != 1
           || !v.contains("remix_u32"))
        {
            AddErr(errs, path, "seed takes remix_u32 only");
            return false;
        }
        const J& spec = v["remix_u32"];
        const std::string sp = path + ".remix_u32";
        if(!spec.is_array() || spec.size() != 2
           || !JIsInt(spec[0]) || !JIsInt(spec[1]))
        {
            AddErr(errs, sp, "expected [lo,hi] integers");
            return false;
        }
        const long long lo = spec[0].template get<long long>();
        const long long hi = spec[1].template get<long long>();
        if(lo < 0 || hi > 4294967295ll || lo > hi)
        {
            AddErr(errs, sp, "expected 0 <= lo <= hi <= 4294967295");
            return false;
        }
        if(rng != nullptr)
        {
            if(lo == 0 && hi == 4294967295ll)
            {
                /* Full-range seed draws reproduce rng.Next()
                   verbatim (no modulo). */
                out = rng->Next();
            }
            else
            {
                out = (unsigned int)(lo
                    + (long long)(rng->Next() % (unsigned int)(hi - lo + 1)));
            }
        }
        return true;
    }
    if(!JIsInt(v))
    {
        AddErr(errs, path, "expected unsigned integer or remix_u32");
        return false;
    }
    if(v.is_number_unsigned())
    {
        const unsigned long long u = v.template get<unsigned long long>();
        if(u > 4294967295ull)
        {
            AddErr(errs, path, "expected unsigned integer"
                               " in 0..4294967295");
            return false;
        }
        out = (unsigned int)u;
        return true;
    }
    const long long s = v.template get<long long>();
    if(s < 0 || s > 4294967295ll)
    {
        AddErr(errs, path, "expected unsigned integer"
                           " in 0..4294967295");
        return false;
    }
    out = (unsigned int)s;
    return true;
}

/*---------------------------------------------------------*\
|||| The shared layer parser. Iterates the layer object   ||
|||| in document order so remix draws land in file order. ||
|||| rng == nullptr -> resolved-literal context (a remix  ||
|||| spec is an error); rng != nullptr -> draw as found.  |
\*---------------------------------------------------------*/
template<typename J>
bool ParseLayer(const J& j, EffectLayer& l, const std::string& path,
                RemixRng* rng, std::vector<std::string>& errs)
{
    if(!j.is_object())
    {
        AddErr(errs, path, "expected object");
        return false;
    }
    EffectLayer out;
    bool ok = true;
    for(auto it = j.begin(); it != j.end(); ++it)
    {
        const std::string& k = it.key();
        const J&           v = it.value();
        const std::string  p = path + "." + k;
        if(k == "primitive")
        {
            if(!v.is_string())
            {
                AddErr(errs, p, "expected string");
                ok = false;
                continue;
            }
            out.primitive = v.template get<std::string>();
            if(!IsPrimitive(out.primitive))
            {
                AddErr(errs, p, "unknown primitive '"
                       + out.primitive + "'");
                ok = false;
            }
        }
        else if(k == "space")
        {
            if(!v.is_string())
            {
                AddErr(errs, p, "expected string");
                ok = false;
                continue;
            }
            const std::string s = v.template get<std::string>();
            if(s == "world")      { out.space = CoordSpace::World; }
            else if(s == "local") { out.space = CoordSpace::Local; }
            else
            {
                AddErr(errs, p, "expected \"world\"|\"local\"");
                ok = false;
            }
        }
        else if(k == "blend")
        {
            if(!v.is_string())
            {
                AddErr(errs, p, "expected string");
                ok = false;
                continue;
            }
            const std::string s = v.template get<std::string>();
            if(s == "replace")    { out.blend = BlendMode::Replace; }
            else if(s == "add")   { out.blend = BlendMode::Add; }
            else if(s == "screen"){ out.blend = BlendMode::Screen; }
            else
            {
                AddErr(errs, p,
                       "expected \"replace\"|\"add\"|\"screen\"");
                ok = false;
            }
        }
        else if(k == "opacity")
        {
            ok &= NumField(v, out.opacity, 0.0, 1.0, rng, p, errs);
        }
        else if(k == "speed")
        {
            ok &= NumField(v, out.speed,
                           -std::numeric_limits<double>::max(),
                           std::numeric_limits<double>::max(),
                           rng, p, errs);
        }
        else if(k == "scale")
        {
            /* >0: primitives divide by it (wavelength, ring
               spacing, tail length, screen width). */
            double d = 0.0, il = 0.0, ih = 0.0;
            if(!NumValue(v, d, il, ih, rng, p, errs))
            {
                ok = false;
            }
            else if(d <= 0.0 || il <= 0.0)
            {
                AddErr(errs, p, "expected number > 0");
                ok = false;
            }
            else
            {
                /* Post-narrowing checks at float width: a finite-
                   positive double (1e-46) underflows to 0.0f and
                   EvalWave/EvalPulse would divide by zero — re-test
                   the >0 contract on the narrowed value AND the
                   spec's implied low bound. Finiteness rule as in
                   NumField. */
                out.scale = (float)d;
                if(out.scale <= 0.0f || (float)il <= 0.0f)
                {
                    AddErr(errs, p, "expected number > 0");
                    ok = false;
                }
                else if(!std::isfinite(out.scale)
                        || !std::isfinite((float)ih))
                {
                    AddErr(errs, p, "expected float-finite number");
                    ok = false;
                }
            }
        }
        else if(k == "phase")
        {
            ok &= NumField(v, out.phase,
                           -std::numeric_limits<double>::max(),
                           std::numeric_limits<double>::max(),
                           rng, p, errs);
        }
        else if(k == "density")
        {
            ok &= NumField(v, out.density, 0.0,
                           std::numeric_limits<double>::max(),
                           rng, p, errs);
        }
        else if(k == "origin")
        {
            ok &= VecField(v, out.origin, false, rng, p, errs);
        }
        else if(k == "direction")
        {
            ok &= VecField(v, out.direction, true, rng, p, errs);
        }
        else if(k == "path")
        {
            ok &= PathField(v, out.path, p, errs);
        }
        else if(k == "palette")
        {
            ok &= PaletteField(v, out.palette, rng, p, errs);
        }
        else if(k == "targets")
        {
            ok &= TargetsField(v, out.targets, p, errs);
        }
        else if(k == "source")
        {
            if(!v.is_string())
            {
                AddErr(errs, p, "expected string");
                ok = false;
                continue;
            }
            const std::string s = v.template get<std::string>();
            if(s.empty() || s == "audio" || s == "key"
               || s == "screen")
            {
                out.source = s;
            }
            else
            {
                AddErr(errs, p,
                       "expected \"audio\"|\"key\"|\"screen\"|\"\"");
                ok = false;
            }
        }
        else if(k == "seed")
        {
            ok &= SeedField(v, out.seed, rng, p, errs);
        }
        else if(k == "enabled")
        {
            if(!v.is_boolean())
            {
                AddErr(errs, p, "expected boolean");
                ok = false;
                continue;
            }
            out.enabled = v.template get<bool>();
        }
        else
        {
            AddErr(errs, p, "unknown key");
            ok = false;
        }
    }
    if(ok)
    {
        l = out;
    }
    return ok;
}

} /* anonymous namespace */

bool IsPrimitive(const std::string& s)
{
    static const char* const prims[] = {
        "static", "gradient", "wave", "pulse", "comet", "noise",
        "spin", "ripple", "screenfield", "level",
    };
    for(const char* p : prims)
    {
        if(s == p)
        {
            return true;
        }
    }
    return false;
}

/*---------------------------------------------------------*\
|||| Resolved-layer serialization (canonical — the        ||
|||| fixture oracle writes the same shape).               |
\*---------------------------------------------------------*/
nlohmann::json EffectLayerToJson(const EffectLayer& l)
{
    nlohmann::json j;
    j["primitive"] = l.primitive;
    j["space"]     = l.space == CoordSpace::Local ? "local" : "world";
    j["blend"]     = l.blend == BlendMode::Add    ? "add"
                   : l.blend == BlendMode::Screen ? "screen"
                                                  : "replace";
    j["opacity"]   = l.opacity;
    j["speed"]     = l.speed;
    j["scale"]     = l.scale;
    j["phase"]     = l.phase;
    j["density"]   = l.density;
    j["origin"]    = nlohmann::json::array(
        { l.origin.x, l.origin.y, l.origin.z });
    j["direction"] = nlohmann::json::array(
        { l.direction.x, l.direction.y, l.direction.z });
    nlohmann::json path = nlohmann::json::array();
    for(const Vec3& v : l.path)
    {
        path.push_back(nlohmann::json::array({ v.x, v.y, v.z }));
    }
    j["path"] = path;
    if(!l.palette.stops.empty())
    {
        nlohmann::json stops = nlohmann::json::array();
        for(const PaletteStop& s : l.palette.stops)
        {
            stops.push_back({
                {"pos",   s.pos},
                {"color", SceneColorHex(ToSceneColor(s.color))},
            });
        }
        j["palette"] = stops;
    }
    nlohmann::json targets = nlohmann::json::array();
    for(const std::string& t : l.targets)
    {
        targets.push_back(t);
    }
    j["targets"] = targets;
    j["source"]  = l.source;
    j["seed"]    = l.seed;
    j["enabled"] = l.enabled;
    return j;
}

nlohmann::json EffectLayersToJson(const std::vector<EffectLayer>& layers)
{
    nlohmann::json out = nlohmann::json::array();
    for(const EffectLayer& l : layers)
    {
        out.push_back(EffectLayerToJson(l));
    }
    return out;
}

bool EffectLayerFromJson(const nlohmann::json& j, EffectLayer& l,
                         const std::string& path,
                         std::vector<std::string>* errors)
{
    std::vector<std::string> local;
    std::vector<std::string>& errs = errors ? *errors : local;
    return ParseLayer(j, l, path, nullptr, errs);
}

bool EffectLayersFromJson(const nlohmann::json& j,
                          std::vector<EffectLayer>& out,
                          std::vector<std::string>* errors,
                          const std::string& base)
{
    /* Sub-parser: callers (SceneJson::FromJson effect.layers,
       StudioConfig effects.layers) embed this inside a larger
       document parse — do NOT clear their error vector or
       already-recorded field errors (e.g. effect.seed) are lost. */
    std::vector<std::string> local;
    std::vector<std::string>& errs = errors ? *errors : local;
    if(!j.is_array())
    {
        AddErr(errs, base, "expected array");
        return false;
    }
    if(j.size() > EFFECT_MAX_LAYERS)
    {
        AddErr(errs, base, "count exceeds cap "
               + std::to_string(EFFECT_MAX_LAYERS));
        return false;
    }
    bool ok = true;
    std::vector<EffectLayer> tmp;
    int i = 0;
    for(const nlohmann::json& jl : j)
    {
        EffectLayer l;
        const std::string p = base + "[" + std::to_string(i++) + "]";
        if(ParseLayer(jl, l, p, nullptr, errs))
        {
            tmp.push_back(l);
        }
        else
        {
            ok = false;
        }
    }
    if(ok)
    {
        out = tmp;
    }
    return ok;
}

/*---------------------------------------------------------*\
|||| Effect document                                       |
\*---------------------------------------------------------*/
nlohmann::ordered_json EffectDocumentToJson(const EffectDocument& d)
{
    nlohmann::ordered_json j;
    j["$schema"]        = "../../schemas/effect.schema.json";
    j["schema_version"] = d.schema_version;
    j["id"]             = d.id;
    j["name"]           = d.name;
    j["description"]    = d.description;
    j["needs"]          = d.needs;
    j["layers"]         = d.layers;
    return j;
}

bool ResolveEffectLayers(const nlohmann::ordered_json& layers,
                         unsigned int seed,
                         std::vector<EffectLayer>& out,
                         std::vector<std::string>* errors)
{
    if(errors)
    {
        errors->clear();
    }
    std::vector<std::string> local;
    std::vector<std::string>& errs = errors ? *errors : local;
    if(!layers.is_array())
    {
        AddErr(errs, "layers", "expected array");
        return false;
    }
    if(layers.size() > EFFECT_MAX_LAYERS)
    {
        AddErr(errs, "layers", "count exceeds cap "
               + std::to_string(EFFECT_MAX_LAYERS));
        return false;
    }
    bool ok = true;
    RemixRng rng(seed);
    std::vector<EffectLayer> tmp;
    int i = 0;
    for(const nlohmann::ordered_json& jl : layers)
    {
        EffectLayer l;
        if(ParseLayer(jl, l, "layers[" + std::to_string(i++) + "]",
                      &rng, errs))
        {
            tmp.push_back(l);
        }
        else
        {
            ok = false;
        }
    }
    if(ok)
    {
        out = tmp;
    }
    return ok;
}

bool ResolveEffectLayers(const EffectDocument& d, unsigned int seed,
                         std::vector<EffectLayer>& out,
                         std::vector<std::string>* errors)
{
    return ResolveEffectLayers(d.layers, seed, out, errors);
}

bool EffectDocumentFromJson(const nlohmann::ordered_json& j,
                            EffectDocument& d,
                            std::vector<std::string>* errors)
{
    if(errors)
    {
        errors->clear();
    }
    std::vector<std::string> local;
    std::vector<std::string>& errs = errors ? *errors : local;

    if(!j.is_object())
    {
        AddErr(errs, "root", "expected object");
        return false;
    }
    if(!j.contains("schema_version") || !JIsInt(j["schema_version"]))
    {
        AddErr(errs, "schema_version", "expected integer");
        return false;
    }
    const long long version = j["schema_version"].get<long long>();
    if(version != EFFECT_SCHEMA_VERSION)
    {
        AddErr(errs, "schema_version", "expected "
               + std::to_string(EFFECT_SCHEMA_VERSION)
               + ", got " + std::to_string(version));
        return false;
    }

    EffectDocument out;
    JStr(j, "id",          out.id,          "root", errs);
    JStr(j, "name",        out.name,        "root", errs);
    JStr(j, "description", out.description, "root", errs);
    JStr(j, "needs",       out.needs,       "root", errs);
    if(!IsPresetId(out.id))
    {
        AddErr(errs, "id", "expected non-empty [A-Za-z0-9_-] id");
    }
    if(!(out.needs.empty() || out.needs == "audio"
         || out.needs == "key" || out.needs == "screen"))
    {
        AddErr(errs, "needs",
               "expected \"audio\"|\"key\"|\"screen\"|\"\"");
    }
    if(!j.contains("layers") || !j["layers"].is_array())
    {
        AddErr(errs, "layers", "expected array");
        return false;
    }
    if(j["layers"].size() > EFFECT_MAX_LAYERS)
    {
        AddErr(errs, "layers", "count exceeds cap "
               + std::to_string(EFFECT_MAX_LAYERS));
        return false;
    }
    out.layers = j["layers"];

    /* Validation = one full resolve against a scratch stream:
       remix specs are checked AND exercised so a spec that can
       never produce a valid literal is rejected at load. */
    RemixRng probe(0);
    std::vector<EffectLayer> scratch;
    int i = 0;
    bool ok = errs.empty();
    for(const nlohmann::ordered_json& jl : out.layers)
    {
        EffectLayer l;
        if(ParseLayer(jl, l, "layers[" + std::to_string(i++) + "]",
                      &probe, errs))
        {
            scratch.push_back(l);
        }
        else
        {
            ok = false;
        }
    }
    if(!ok || !errs.empty())
    {
        return false;
    }
    d = out;
    return true;
}

bool EffectDocumentFromJsonFile(const std::string& path,
                                EffectDocument& d,
                                std::vector<std::string>* errors)
{
    if(errors)
    {
        errors->clear();
    }
    std::ifstream f(path, std::ios::binary);
    if(!f.is_open())
    {
        if(errors)
        {
            errors->push_back(path + ": cannot open");
        }
        return false;
    }
    std::stringstream ss;
    ss << f.rdbuf();
    const std::string text = ss.str();
    const nlohmann::ordered_json j =
        nlohmann::ordered_json::parse(text, nullptr, false);
    if(j.is_discarded())
    {
        if(errors)
        {
            errors->push_back(path + ": malformed JSON");
        }
        return false;
    }
    std::vector<std::string> local;
    if(!EffectDocumentFromJson(j, d, &local))
    {
        if(errors)
        {
            for(const std::string& e : local)
            {
                errors->push_back(path + ": " + e);
            }
        }
        return false;
    }
    return true;
}

} /* namespace studio */
