/*---------------------------------------------------------*\
|| SceneTypes.h                                              |
||                                                           |
||   Desktop Lighting Studio scene model — Qt-free core.     |
||   Right-handed coordinates, Y up, meters.                 |
||                                                           |
||   The scene document is the single source of truth for    |
||   object placement, emitter positions, bindings, and      |
||   static colors. QML renders from it; the output          |
||   adapter writes hardware from the same document.         |
||                                                           |
||   SPDX-License-Identifier: GPL-2.0-or-later               |
\*---------------------------------------------------------*/

#pragma once

#include <initializer_list>
#include <map>
#include <string>
#include <vector>

namespace studio
{

/* Colors use OpenRGB's packed 0x00BBGGRR format so scene colors feed
   controller writes without conversion. */
typedef unsigned int SceneColor;

inline SceneColor MakeSceneColor(unsigned int r, unsigned int g, unsigned int b)
{
    return ((b & 0xFF) << 16) | ((g & 0xFF) << 8) | (r & 0xFF);
}

struct Vec3
{
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

struct Transform
{
    Vec3 position;
    Vec3 rotation_deg;      /* euler XYZ, degrees; the ONLY rotation  */
                            /* convention — converted once to a       */
                            /* quaternion (SceneGraph) for both the   */
                            /* renderer and the effects engine.       */
    Vec3 scale { 1.0f, 1.0f, 1.0f };  /* dimensionless, must be > 0    */
};

/* world = T * Rxyz * S applied to local point */
Vec3 TransformPoint(const Transform& t, const Vec3& p);

/*---------------------------------------------------------*\
|| Emitter — one addressable LED inside a scene object.    |
||                                                         |
||   group    — logical emitter group; mirrored copies     |
||              share a group so they show identical state.|
||   address  — LED index inside the bound zone, or -1     |
||              for render-only emitters (linked copies,   |
||              unverified lights).                        |
\*---------------------------------------------------------*/
struct Emitter
{
    Vec3        local_pos;
    std::string group;
    int         address = -1;
};

enum class ObjectKind
{
    Decor,      /* render only — desk, case shell                  */
    Device,     /* bound to a real controller zone                 */
    Linked,     /* mirror copy of another object's emitter group   */
    Group,      /* placement-only parent node — no body, no output */
};

struct SceneObject
{
    std::string             id;
    std::string             label;
    ObjectKind              kind = ObjectKind::Decor;
    std::string             parent_id;      /* placement frame owner; "" */
                                            /* = scene root. Placement   */
                                            /* only — output ownership   */
                                            /* is mirror_of, below.      */
    Transform               transform;      /* local to parent frame     */
    Vec3                    size_m;         /* body geometry dimensions  */
                                            /* in meters (pre-scale); a  */
                                            /* 0 component falls back to */
                                            /* the geometry's canonical  */
                                            /* axis (ResolvedBodySize).  */
                                            /* Separate from transform.  */
                                            /* scale.                    */
    std::string             binding;        /* DeviceBinding id          */
    std::string             mirror_of;      /* source object id (Linked) */
    std::string             geometry;       /* decor/body mesh hint      */
    std::string             layout;         /* "" or "matrix_map" —      */
                                            /* rebuild emitters from     */
                                            /* the bound zone's matrix   */
    std::vector<Emitter>    emitters;
    bool                    verified = false; /* false => never written  */
    bool                    visible  = true;
};

/*---------------------------------------------------------*\
|| DeviceBinding — persistent device identity.             |
||   Matches controllers by name/vendor/serial/location    |
||   and zones by name. NEVER a controller-list index.     |
\*---------------------------------------------------------*/
struct DeviceBinding
{
    std::string id;
    std::string controller_name;
    std::string vendor;
    std::string serial;
    std::string location;
    int         device_type = -1;       /* device_type enum, -1 = any   */
    std::string zone_name;
    unsigned int zone_leds = 0;         /* expected count, 0 = unchecked */
};

/*---------------------------------------------------------*\
||| Effect layer model — declared here (not in            |
||| effects/EffectTypes.h) because the persisted inline   |
||| layer stack lives on EffectState below, and           |
||| EffectTypes.h already depends on this header. The     |
||| definitions still live in effects/EffectTypes.cpp.    |
|||                                                         |
|||   ColorF — 0..1 float working space for compositing. |
|||   SceneColor (packed 0x00BBGGRR) converts at the      |
|||   edges.                                              |
\*---------------------------------------------------------*/
struct ColorF
{
    float r = 0.0f;
    float g = 0.0f;
    float b = 0.0f;
    float a = 1.0f;         /* coverage: how much this result owns the pixel */
};

ColorF     ToColorF(SceneColor c);
SceneColor ToSceneColor(const ColorF& c);

enum class BlendMode
{
    Replace,    /* lerp toward the layer color by coverage            */
    Add,        /* dst + src * coverage                               */
    Screen,     /* 1 - (1-dst)(1-src*coverage) — soft additive light  */
};

enum class CoordSpace
{
    World,      /* shared desk frame — effects cross devices          */
    Local,      /* object-local frame — per-fan/per-keyboard fields   */
};

/*---------------------------------------------------------*\
|||| Palette — interpolated color stops. Sample(u) wraps;|
|||| the last stop blends back into the first so cyclic  |
|||| effects (spin, wave) stay seamless.                 |
\*---------------------------------------------------------*/
struct PaletteStop
{
    float     pos = 0.0f;   /* 0..1 */
    ColorF    color;
};

struct Palette
{
    std::vector<PaletteStop> stops;
    ColorF Sample(float u) const;   /* u wraps via frac */
};

Palette MakePalette(std::initializer_list<SceneColor> colors); /* even stops */
Palette MakePalette(std::initializer_list<PaletteStop> stops);

/*---------------------------------------------------------*\
|||| EffectLayer — one field + how it composites.         |
||||                                                         |
||||   primitive — "static" | "gradient" | "wave" | "pulse" ||
||||               "comet" | "noise" | "spin" | "ripple" |  |
||||               "screenfield" | "level"                  |
||||   space     — World (shared desk) or Local (per object)||
||||   speed     — primitive motion rate (m/s, rev/s, ...)  |
||||   scale     — wavelength / ring spacing / noise freq / ||
||||               spoke count / comet tail length / ripple ||
||||               band half-width / screenfield width (m)  |
||||   phase     — 0..1 starting offset                     |
||||   density   — band sharpness / noise contrast / ripple ||
||||               decay rate (1/s)                         |
||||   origin    — pulse/comet/spin center (world or local);|
||||               ripple spawn for position-less events;   ||
||||               screenfield screen center                |
||||   direction — wave/gradient axis; noise wind           |
||||   path      — comet waypoints (polyline, closed auto)  |
||||   targets   — object ids, geometry tags, emitter       |
||||               groups; empty = every device object      |
||||   source    — which input events feed a ripple         |
||||               ("audio" | "key" | "" = all)             |
||||   seed      — per-layer deterministic variation        |
||||   enabled   — false = the layer composites nothing;    |
||||               persisted on/off switch for the layer    |
||||               stack editor (task 5.2)                  |
\*---------------------------------------------------------*/
struct EffectLayer
{
    std::string              primitive = "static";
    CoordSpace               space     = CoordSpace::World;
    BlendMode                blend     = BlendMode::Replace;
    float                    opacity   = 1.0f;
    float                    speed     = 1.0f;
    float                    scale     = 0.5f;
    float                    phase     = 0.0f;
    float                    density   = 1.0f;
    Vec3                     origin;
    Vec3                     direction { 1.0f, 0.0f, 0.0f };
    std::vector<Vec3>        path;
    Palette                  palette;
    std::vector<std::string> targets;
    std::string              source;         /* "audio" | "key" | "" = all */
    unsigned int             seed    = 0;
    bool                     enabled = true;
};

/*---------------------------------------------------------*\
||| EffectState — which preset drives the scene and how.   |
||| Persisted with the document so a saved scene is also   |
||| the startup scene.                                      |
\*---------------------------------------------------------*/
struct EffectState
{
    std::string              preset;                    /* "" = none             */
    unsigned int             seed = 0;                  /* remix seed            */
    float                    speed = 1.0f;              /* global rate           */
    float                    intensity = 1.0f;
    bool                     playing = false;
    /* The user's edited inline layer stack (resolved literals only).
       Empty = resolve `preset` through the effect-look registry.
       Serialized as workspace effects.layers; never a dump of the
       engine's resolved output. */
    std::vector<EffectLayer> layers;
};

/* One evaluated frame: owner object id -> per-emitter colors.
   Produced by EffectEngine; consumed by the preview and the
   output adapter so both show the same colors. */
typedef std::map<std::string, std::vector<SceneColor>> FrameColors;

struct SceneDocument
{
    int                                 version = 2;
    std::string                         name;
    std::vector<DeviceBinding>          bindings;
    std::vector<SceneObject>            objects;
    /* object id -> base color (emitters without an override)         */
    std::map<std::string, SceneColor>   object_colors;
    /* object id -> emitter index -> color (painted overrides)        */
    std::map<std::string, std::map<int, SceneColor>> emitter_colors;
    float                               brightness = 1.0f;
    EffectState                         effect;
};

const SceneObject* FindObject(const SceneDocument& doc, const std::string& id);
SceneObject*       FindObject(SceneDocument& doc, const std::string& id);

/*---------------------------------------------------------*\
||| Body size contract — the ONE place it is decided:     ||
|||   size_m is authored body dimensions in meters        ||
|||   (pre-scale); a component <= 0 falls back to the     ||
|||   geometry's canonical axis size, so {0,0,0} means    ||
|||   "canonical size" for every known geometry — decor   ||
|||   bodies included. Validation (size_m >= 0, finite),  ||
|||   the schema doc, and the renderer all share this     ||
|||   rule; the bridge publishes ResolvedBodySize to QML. ||
\*---------------------------------------------------------*/

/* Canonical body dimensions per geometry (meters); {0,0,0} for
   geometry the renderer doesn't know (such objects render no
   body mesh). */
Vec3 CanonicalBodySize(const std::string& geometry);

/* Effective body size: authored size_m with per-axis canonical
   fallback. */
Vec3 ResolvedBodySize(const SceneObject& o);

/* The object that owns the output addresses for id — follows
   mirror_of on Linked objects. Returns nullptr if unresolvable. */
const SceneObject* OutputOwner(const SceneDocument& doc, const std::string& id);

/* Effective color of an emitter: painted override, else object base
   color, else 0. Linked objects read through to their source. */
SceneColor EmitterColor(const SceneDocument& doc, const std::string& object_id,
                        int emitter_index);

} /* namespace studio */
