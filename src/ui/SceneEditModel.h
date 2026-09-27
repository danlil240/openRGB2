/*---------------------------------------------------------*\
|||| SceneEditModel.h                                          ||
||||                                                           ||
||||   Qt-free edit model for the Lighting view's scene     ||
||||   document (Task 4.2, plan section 5): the choose ->   ||
||||   select -> adjust -> save flow edits a WORKING copy   ||
||||   of the engine-applied SceneDocument JSON and         ||
||||   serializes it into the SET_SCENE body                ||
||||   (docs/ipc-protocol.md §5). Edits are explicit and    ||
||||   reversible — nothing reaches the engine until the    ||
||||   caller asks for the body; a remotely-applied scene   ||
||||   never silently overwrites uncommitted edits.         ||
||||                                                           ||
||||   Deliberately Qt-free like EngineWireClient: the      ||
||||   offline suite drives the model without an event      ||
||||   loop (tests/ui). The QObject wrapper lives in        ||
||||   SceneEditController.                                 ||
||||                                                           ||
||||   SPDX-License-Identifier: GPL-2.0-or-later               ||
||\*---------------------------------------------------------*/

#pragma once

#include <nlohmann/json.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace openrgb2
{
namespace ui
{

/*---------------------------------------------------------*\
||| Playback-preset capability table — the wire contract   ||
||| itself (docs/ipc-protocol.md §5): the three uploaded   ||
||| presets, the params each accepts through               ||
||| SET_PLAYBACK_MODE {params:{speed,reverse,color}}, and  ||
||| the names a scene may put in effect.preset. The        ||
||| engine's copy lives in src/engine/PlaybackPresets.cpp;||
||| the UI mirrors the DOCUMENTED table — controls the    ||
||| table doesn't grant are never emitted.               ||
\*---------------------------------------------------------*/
struct PlaybackPresetInfo
{
    std::string id;        /* snake_case scene-preset id          */
    std::string sdk_name;  /* display/SDK mode name               */
    bool        speed     = true;
    bool        direction = false;   /* reverse param             */
    bool        color     = false;   /* "#RRGGBB" param           */
};

/* The three uploaded presets in contract order. */
const std::vector<PlaybackPresetInfo>& PlaybackPresetCatalog();

/* Case/space/underscore/dash-insensitive normalization —
   "Rainbow Wave", "rainbow_wave", "rainbow-wave" are one id
   (same convention the engine applies). */
std::string NormalizePresetId(const std::string& name);

/* Match a scene-preset name (snake_case id OR SDK display
   name) against the catalog; false when not a playback
   preset. */
bool PlaybackPresetFromName(const std::string& name,
                            PlaybackPresetInfo& out);

/* Uploaded-preset speed domain on the wire: 10..200. */
int ClampPlaybackSpeed(int speed);

/*---------------------------------------------------------*\
||| DeviceGroup — one selectable output group derived     ||
||| from the scene document's objects + bindings (the     ||
||| engine's real binding data, never a fabricated list).||
||| Connection/LED truth is joined by the controller;     ||
||| this model reports what the DOCUMENT declares.       ||
\*---------------------------------------------------------*/
struct DeviceGroup
{
    std::string id;               /* scene object id            */
    std::string label;            /* display label              */
    std::string binding;          /* DeviceBinding id ("" none) */
    std::string controller_name;  /* from the binding           */
    std::string vendor;           /* from the binding           */
    std::string serial;           /* from the binding           */
    std::string location;         /* from the binding           */
    int         device_type = -1; /* from the binding (-1 any)  */
    std::string zone_name;        /* from the binding           */
    uint32_t    zone_leds   = 0;  /* declared by the binding    */
    size_t      emitter_count = 0;
    bool        verified    = false; /* unverified = never written */
};

/*---------------------------------------------------------*\
||| SceneEditModel                                         ||
||||                                                          ||
||||   baseline_ — the last document the engine APPLIED     ||
||||   (GET_SCENE / scene-event document).                  ||
||||   working_  — the user's editable copy; every op       ||
||||   mutates it. Dirty = working differs from baseline.   ||
||||   Conflict = the applied document moved while edits    ||
||||   were pending (another client, or the engine          ||
||||   re-applied) — surfaced, never silently merged.       ||
\*---------------------------------------------------------*/
class SceneEditModel
{
public:
    /* A minimal valid v2 document for scenes authored from
       scratch (no baseline — the engine has no scene loaded).
       Bindings stay empty: output topology is authored by the
       Layout view, never invented here. */
    static nlohmann::json NewDocument();

    /*---- baseline / working state -------------------------*/
    void LoadApplied(const nlohmann::json& doc_or_null);
    /* Stage an arbitrary document as the working copy (a
       library profile load) — dirty recomputes against the
       baseline so the staged doc is an explicit pending edit,
       not a silent apply. */
    void LoadWorking(const nlohmann::json& doc);
    /* The applied document just changed under pending edits —
       or was saved through this model and echoed augmented. */
    void Commit();            /* baseline = working (save OK)  */
    void DiscardEdits();      /* working  = baseline           */
    /* The applied-scene push OUR OWN save produced (the
       caller correlates by scene revision — the SET_SCENE OK
       and the scene event carry the same counter): the engine
       reserializes the document it applied (float narrowing,
       defaulted fields filled in), so the echo differs from
       the submitted working copy while being semantically the
       same apply. Adopt it as the new baseline — and as the
       working copy when nothing was edited since the commit —
       without flagging a conflict: nothing foreign moved.
       Edits made after the save stay pending; dirty simply
       recomputes against the echoed baseline. */
    void AdoptApplied(const nlohmann::json& doc);

    bool                    HasBaseline() const;
    bool                    HasWorking()  const;
    bool                    Dirty()       const;
    bool                    Conflict()    const { return conflict_; }
    const nlohmann::json&   Working()     const { return working_; }
    const nlohmann::json&   Baseline()    const { return baseline_; }
    /* True when the baseline itself had inline effect layers
       and a preset pick replaced them (surfaces in the UI). */
    bool                    PresetReplacedLayers() const
    { return preset_replaced_layers_; }

    /*---- edits (false + err text on invalid input) --------*/
    bool SetPreset(const std::string& id, std::string& err);
    bool SetName(const std::string& name, std::string& err);
    bool SetBrightness(double v);            /* clamps 0..1    */
    bool SetEffectSpeed(double v);           /* clamps bounds  */
    bool SetEffectIntensity(double v);       /* clamps 0..1    */
    bool SetObjectColor(const std::string& object_id,
                        const std::string& hex, std::string& err);
    bool ClearObjectColor(const std::string& object_id);

    /*---- group selection ----------------------------------
       Selected object ids scope palette writes always, and
       scope effect `targets` when the document carries an
       inline layer stack (named presets resolve engine-side —
       there is no wire way to target them; GroupsTargetable
       reports which case the working doc is in).         */
    void SetSelectedGroups(const std::vector<std::string>& ids);
    const std::vector<std::string>& SelectedGroups() const
    { return selected_; }
    bool GroupsTargetable() const;
    /* Write selection into every inline layer's `targets`
       (empty selection -> empty targets = every device). */
    void ApplyTargetsToLayers();

    /*---- views ---------------------------------------------*/
    std::vector<DeviceGroup> DeviceGroups() const;
    std::string              PresetId() const;
    nlohmann::json           BuildSetSceneBody() const;

    /*---- field helpers (shared with the controller/QML) ---*/
    static bool IsHexColor(const std::string& s);
    static std::string NormalizeHexColor(const std::string& s);

private:
    void EnsureWorking();

    nlohmann::json          baseline_ = nullptr;
    nlohmann::json          working_  = nullptr;
    bool                    conflict_ = false;
    bool                    preset_replaced_layers_ = false;
    std::vector<std::string> selected_;
};

} /* namespace ui */
} /* namespace openrgb2 */
