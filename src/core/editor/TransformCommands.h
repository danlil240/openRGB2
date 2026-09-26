/*---------------------------------------------------------*\
|| TransformCommands.h                                       ||
||                                                           ||
||   EditorEdit — the Qt-free undo record for every       ||
||   workspace edit the editor performs. One record       ||
||   holds per-section deltas over the compact authoring  ||
||   document (StudioDocument):                           ||
||     devices          — instance placements (add/remove ||
||                        reparent included)              ||
||     device_settings  — visible/locked/mirror_of/zones  ||
||     bindings         — controller identities a zone    ||
||                        setting references              ||
||     object_colors / emitter_colors — re-keyed or       ||
||                        removed paint                   ||
||   Apply/Revert restore either side of the edit, so a   ||
||   drag's 100 preview updates, a rename, a delete and   ||
||   a group all undo through the same shape. The         ||
||   QUndoCommand adapter lives in SceneBridge.cpp next   ||
||   to the existing color/brightness commands.           ||
||                                                           ||
||   Also carries the small rigid-transform math helpers  ||
||   the controller needs for world<->parent-local        ||
||   conversion (instance transforms are always T*R,      ||
||   scale stays 1).                                      ||
||                                                           ||
||   SPDX-License-Identifier: GPL-2.0-or-later               ||
\*---------------------------------------------------------*/

#pragma once

#include "../config/StudioConfig.h"
#include "../scene/SceneGraph.h"

#include <map>
#include <set>
#include <string>
#include <vector>

namespace studio
{

/*---------------------------------------------------------*\
|| Rigid-transform helpers. Instance transforms carry no  ||
|| scale, so every instance world matrix is T*R and these ||
|| inverses stay exact. Quats are unit quats produced by  ||
|| RotationQuat / AxisAngleQuat.                          ||
\*---------------------------------------------------------*/

/* Hamilton product a * b — applies b's rotation first. */
Quat QuatMul(const Quat& a, const Quat& b);
/* Rotation inverse for a unit quaternion. */
Quat QuatConjugate(const Quat& q);
/* Rotation of `degrees` around `axis` (need not be normalized). */
Quat AxisAngleQuat(const Vec3& axis, float degrees);
/* Pure-rotation matrix for q. */
Mat4 QuatMatrix(const Quat& q);

/* Inverse of a rigid T*R matrix (unit scale — rotation part is
   orthonormal, so the inverse is transpose + back-rotated
   translation). */
Mat4 RigidInverse(const Mat4& m);
/* RigidInverse(m) applied to a point without forming the matrix. */
Vec3 RigidInversePoint(const Mat4& m, const Vec3& p);
Vec3 Mat4Translation(const Mat4& m);

/* Extract stored-convention XYZ degrees (Rz*Ry*Rx) from a rigid
   rotation matrix — the inverse of RotationQuat's mapping, used
   when a world rotation must be re-expressed in a new parent
   frame (grouping, reparenting). */
Vec3 EulerDegFromMat4(const Mat4& r);

/*---------------------------------------------------------*\
|| SectionDelta — keys touched in one StudioDocument      ||
|| section. `before` holds the prior value for every key  ||
|| that existed; `after` the new value for every key that ||
|| exists afterwards. A key in before-only was removed;   ||
|| after-only was added.                                  ||
\*---------------------------------------------------------*/
template<typename T>
struct SectionDelta
{
    std::map<std::string, T> before;
    std::map<std::string, T> after;

    bool Empty() const { return before.empty() && after.empty(); }
};

/* Record key's pre-edit value (call BEFORE mutating src). */
template<typename T>
void SnapshotKey(SectionDelta<T>& d, const std::map<std::string, T>& src,
                 const std::string& key)
{
    const auto it = src.find(key);
    if(it != src.end())
    {
        d.before[key] = it->second;
    }
}

/* Record key's post-edit value (call AFTER mutating src). */
template<typename T>
void CaptureKey(SectionDelta<T>& d, const std::map<std::string, T>& src,
                const std::string& key)
{
    const auto it = src.find(key);
    if(it != src.end())
    {
        d.after[key] = it->second;
    }
}

template<typename T>
void ApplySection(std::map<std::string, T>& dst, const SectionDelta<T>& d)
{
    for(const auto& kv : d.after)
    {
        dst[kv.first] = kv.second;
    }
    for(const auto& kv : d.before)
    {
        if(d.after.find(kv.first) == d.after.end())
        {
            dst.erase(kv.first);
        }
    }
}

template<typename T>
void RevertSection(std::map<std::string, T>& dst, const SectionDelta<T>& d)
{
    for(const auto& kv : d.before)
    {
        dst[kv.first] = kv.second;
    }
    for(const auto& kv : d.after)
    {
        if(d.before.find(kv.first) == d.before.end())
        {
            dst.erase(kv.first);
        }
    }
}

/*---------------------------------------------------------*\
|| EffectDelta — the authored effect state captured in   ||
|| one undo record: the inline layer stack plus the look ||
|| id it was derived from and the global seed. `preset`  ||
|| is provenance — editing the stack never rewrites it,  ||
|| but reset-to-preset clears `layers` so the named look ||
|| resolves again, and adopting a saved look rewrites    ||
|| `preset` + `layers` together. Values are the AUTHORED ||
|| ones — runtime speed/intensity/ripple multipliers are ||
|| applied on the engine copy in rebuildEffect and never ||
|| reach this snapshot.                                  ||
\*---------------------------------------------------------*/
struct EffectDelta
{
    std::string              preset;
    unsigned int             seed  = 0;
    std::vector<EffectLayer> layers;
};

/*---------------------------------------------------------*\
|| EditorEdit — one undoable workspace edit.             ||
\*---------------------------------------------------------*/
struct EditorEdit
{
    std::string                                     label;   /* undo text */
    SectionDelta<DeviceInstance>                    devices;
    SectionDelta<DeviceSettings>                    settings;
    SectionDelta<DeviceBinding>                     bindings;
    SectionDelta<SceneColor>                        object_colors;
    SectionDelta<std::map<int, SceneColor>>         emitter_colors;
    bool                                            has_effect = false;
    EffectDelta                                     effect_before;
    EffectDelta                                     effect_after;

    bool Empty() const;

    /* True when only device position/rotation values changed —
       same instance keys, same type, same parent, no settings or
       color deltas. The presentation model uses this to pick a
       granular dataChanged over a full reset during drags. */
    bool TransformsOnly() const;

    /* Instance ids whose rows need a transform refresh. */
    std::set<std::string> TransformIds() const;
};

void ApplyEditorEdit(StudioDocument& ws, const EditorEdit& e);
void RevertEditorEdit(StudioDocument& ws, const EditorEdit& e);

} /* namespace studio */
