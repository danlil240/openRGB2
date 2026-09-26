/*---------------------------------------------------------*\
||| StudioConfig.h                                            |
|||                                                           |
|||   StudioDocument — the authoritative workspace doc     |
|||   persisted as studio.json. Qt-free: candidate         |
|||   validation and deterministic serialization run under |
|||   plain cl so tests/studio_config_test.cpp covers them.|
|||                                                           |
|||   Layout (schema v3 — compact instances):              |
|||     schema_version, name                               |
|||     ui/camera/controls/render  — editor preferences    |
|||     inputs  — reactive input source settings           |
|||     output  — brightness + live_on_startup             |
|||     devices — instance id -> {type,x,y,z,rx,ry,rz,     |
|||               [parent]}; placements only, NEVER        |
|||               expanded entities or embedded types      |
|||     bindings — binding id -> hardware identity         |
|||     device_settings — instance path -> per-instance    |
|||               state (visible/locked/mirror_of/zones)   |
|||     colors  — {objects, emitters} keyed by stable      |
|||               instance/entity paths                    |
|||     effects — preset/seed/speed/intensity/playing      |
|||               + layers (the user's authored inline     |
|||               layer stack - typed, validated,        |
|||               resolved literals only; empty = the    |
|||               named preset resolves via the registry)|
|||     extensions — third-party data, retained verbatim   |
|||                                                           |
|||   Type definitions live in external files              |
|||   (presets/devices/*.device.json, presets/DevicePreset)|
|||   and the expanded runtime scene is produced by        |
|||   scene/SceneResolver — never serialized here.         |
|||                                                           |
|||   SPDX-License-Identifier: GPL-2.0-or-later               |
\*---------------------------------------------------------*/

#pragma once

#include "../scene/SceneTypes.h"

#include <nlohmann/json.hpp>

#include <map>
#include <string>
#include <vector>

namespace studio
{

/* Workspace schema version. Older documents are migrated on load
   (v2 expanded workspaces by the file store); newer ones are
   rejected without rewriting (no silent downgrade). */
constexpr int STUDIO_SCHEMA_VERSION = 3;

/* Content caps — malformed community content must not be able to
   destabilize the host. Generous vs. the current ~20-instance desk. */
constexpr unsigned int STUDIO_MAX_DEVICES     = 2048;
constexpr unsigned int STUDIO_MAX_BINDINGS    = 512;
constexpr unsigned int STUDIO_MAX_SETTINGS    = 8192;
constexpr unsigned int STUDIO_MAX_ZONE_SET    = 64;
constexpr unsigned int STUDIO_MAX_COLOR_KEYS  = 8192;
/* Expansion caps — enforced by SceneResolver while instances
   unfold into the runtime scene (many devices x fat types must not
   allocate unboundedly). Same bounds the v2 scene validator used. */
constexpr unsigned int STUDIO_MAX_OBJECTS     = 2048;
constexpr unsigned int STUDIO_MAX_EMITTERS    = 65536;

struct UiPrefs
{
    std::string theme = "graphite";
    bool        reduced_motion = false;
    /* ui.favorites — pinned device-type ids, display order kept.
       UI state only: not undoable, never part of the scene.
       Non-string entries and invalid identifiers are dropped on
       load with a warning; duplicates collapse to first seen. */
    std::vector<std::string> favorites;
};

struct CameraPrefs
{
    std::string view       = "desk";          /* desk|top|front|case|free */
    std::string projection = "orthographic";  /* orthographic|perspective */
    /* Final pose of the last camera gesture — editor prefs, never
       scene-object history. All fields optional in studio.json;
       missing keys keep these defaults. */
    Vec3        target     = { 0.1f, 0.18f, 0.05f }; /* pan/orbit focus (m) */
    float       yaw_deg    = 0.0f;    /* free-view orbit yaw             */
    float       pitch_deg  = -38.0f;  /* free-view orbit pitch           */
    float       distance   = 1.21f;   /* perspective dolly distance (m)  */
    float       span       = 0.9f;    /* ortho vertical extent (m)       */
};

struct ControlsPrefs
{
    std::string middle_drag      = "pan";     /* pan|orbit */
    float       move_snap_m      = 0.01f;
    float       rotate_snap_deg  = 15.0f;
};

struct RenderPrefs
{
    std::string quality = "balanced";         /* low|balanced|high */
    bool        bloom   = true;
};

/* Reactive input-source settings — persisted beside the scene so a
   saved workspace restores the sources it was using. */
struct InputSettings
{
    bool audio        = false;
    bool keys         = false;
    bool screen       = false;
    int  screen_index = 0;
    int  sens_pct     = 100;    /* audio sensitivity, 25..200   */
    int  decay_pct    = 100;    /* ripple decay scale, 50..300  */
};

/* Non-scene, non-input workspace state. Kept whole so preference
   sections the editor doesn't use yet still round-trip. */
struct WorkspaceMeta
{
    std::string    name;
    UiPrefs        ui;
    CameraPrefs    camera;
    ControlsPrefs  controls;
    RenderPrefs    render;
    /* output.live_on_startup — defaults false; migration and
       imported documents may never enable it. */
    bool           live_on_startup = false;
    /* Third-party extension data, retained verbatim. */
    nlohmann::json extensions;
};

/*---------------------------------------------------------*\
||| Compact authoring sections.                            ||
|||                                                           |
|||   Instance paths: a root device instance is its        ||
|||   devices key ("case"); a nested child-device expands  ||
|||   as "<parent>/<entity>" ("case/case_fans"). All       ||
|||   device_settings and colors keys are such paths or    ||
|||   "<path>/<entity>" for a specific expanded entity.    |
\*---------------------------------------------------------*/
struct DeviceInstance
{
    std::string type;                   /* device-type id (file id)  */
    Vec3        position;               /* meters                    */
    Vec3        rotation_deg;           /* XYZ degrees, Rz*Ry*Rx     */
    std::string parent;                 /* optional parent instance  */
};

struct ZoneSetting
{
    std::string binding;                /* bindings key; "" = unbound */
    int         addr_base = 0;          /* first LED index in zone    */
    bool        verified = false;       /* verified => writable       */
};

struct DeviceSettings
{
    bool        visible = true;
    bool        locked  = false;
    /* Instance-level mirror: this instance shares another
       instance's output. Resolution marks every expanded object
       under the source path Linked -> the corresponding object
       under the target path (same type required, no chains). */
    std::string mirror_of;
    /* zone_id -> binding attachment + address base. */
    std::map<std::string, ZoneSetting> zones;
    /* Unknown fields, retained verbatim (forward compatibility). */
    nlohmann::json extra;
};

struct StudioDocument
{
    int            schema_version = STUDIO_SCHEMA_VERSION;
    WorkspaceMeta  meta;
    InputSettings  inputs;
    /* devices map: instance id -> placement. */
    std::map<std::string, DeviceInstance>  devices;
    /* bindings map: binding id -> hardware identity. */
    std::map<std::string, DeviceBinding>   bindings;
    /* instance path -> per-instance state. */
    std::map<std::string, DeviceSettings>  device_settings;
    /* instance/entity path -> base color. */
    std::map<std::string, SceneColor>      object_colors;
    /* instance/entity path -> emitter index -> color. */
    std::map<std::string, std::map<int, SceneColor>> emitter_colors;
    float                                brightness = 1.0f;
    EffectState                          effect;
    /* Resolved runtime scene — filled by scene/SceneResolver on
       load, NEVER serialized into the workspace. */
    SceneDocument                        scene;
};

/* Deterministic serialization (nlohmann orders keys; the store
   writes dump(2)). Colors serialize as "#RRGGBB". */
nlohmann::json ToJson(const StudioDocument& doc);

/* Whole-document validation into a candidate. On success `doc`
   receives the candidate and the function returns true; on failure
   `doc` is untouched and `errors` holds field-path messages.
   `warnings`, when given, collects non-fatal notes (unknown fields,
   unknown effect presets). Validates the compact sections only —
   resolution against the type registry is a separate step
   (SceneResolver), driven by the file store. Never throws. */
bool FromJson(const nlohmann::json& j, StudioDocument& doc,
              std::vector<std::string>* errors   = nullptr,
              std::vector<std::string>* warnings = nullptr);

} /* namespace studio */
