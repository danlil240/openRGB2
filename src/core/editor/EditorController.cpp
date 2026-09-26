/*---------------------------------------------------------*\
|| EditorController.cpp                                      ||
||                                                           ||
||   SPDX-License-Identifier: GPL-2.0-or-later               ||
\*---------------------------------------------------------*/

#include "EditorController.h"

#include "../effects/EffectJson.h"
#include "../presets/DevicePreset.h"
#include "../presets/PresetRegistry.h"
#include "../scene/SceneJson.h"

#include <algorithm>
#include <cmath>
#include <cfloat>

namespace studio
{
namespace
{

bool SameVec(const Vec3& a, const Vec3& b)
{
    return a.x == b.x && a.y == b.y && a.z == b.z;
}

/* Gesture-diff comparators. Previews run positions through a rigid
   round-trip (R^T * (R * p)) and rotations through asin/atan2, so a
   zero-delta drag on a parented or rotated instance lands a few ULPs
   off the authored values — exact == would turn a click without drag
   into a phantom "transform" record. Only the Commit diff uses these;
   the numeric-edit early-outs keep exact SameVec (typing the same
   number back really is equal). */
bool NearVec(const Vec3& a, const Vec3& b, float eps)
{
    return std::fabs(a.x - b.x) < eps
        && std::fabs(a.y - b.y) < eps
        && std::fabs(a.z - b.z) < eps;
}

bool NearInstance(const DeviceInstance& a, const DeviceInstance& b)
{
    return a.type == b.type && a.parent == b.parent
        && NearVec(a.position, b.position, 1e-6f)      /* ~1 um   */
        && NearVec(a.rotation_deg, b.rotation_deg, 1e-4f);
}

Vec3 Add(const Vec3& a, const Vec3& b)
{
    return { a.x + b.x, a.y + b.y, a.z + b.z };
}

/* Rotation-only copy of a rigid world matrix. */
Mat4 RotationOnly(const Mat4& m)
{
    Mat4 r = m;
    r.m[12] = r.m[13] = r.m[14] = 0.0f;
    return r;
}

float AxisComp(const Vec3& v, int axis)
{
    return axis == 0 ? v.x : axis == 1 ? v.y : v.z;
}

void SetAxisComp(Vec3& v, int axis, float c)
{
    if(axis == 0)      { v.x = c; }
    else if(axis == 1) { v.y = c; }
    else               { v.z = c; }
}

const char* AxisName(int axis)
{
    return axis == 0 ? "x" : axis == 1 ? "y" : "z";
}

} /* anonymous namespace */

std::string EditorController::InstanceOf(const std::string& object_id)
{
    const size_t slash = object_id.find('/');
    return slash == std::string::npos ? object_id
                                      : object_id.substr(0, slash);
}

bool EditorController::Exists(const std::string& id) const
{
    return ws.devices.find(id) != ws.devices.end();
}

bool EditorController::IsLocked(const std::string& id) const
{
    const auto it = ws.device_settings.find(id);
    return it != ws.device_settings.end() && it->second.locked;
}

bool EditorController::IsVisible(const std::string& id) const
{
    const auto it = ws.device_settings.find(id);
    return it == ws.device_settings.end() || it->second.visible;
}

/*---------------------------------------------------------*\
|| Selection                                                ||
\*---------------------------------------------------------*/
bool EditorController::Select(const std::string& object_or_instance_id,
                              bool additive)
{
    const std::string id = InstanceOf(object_or_instance_id);
    if(!Exists(id))
    {
        if(!additive)
        {
            ClearSelection();
        }
        return false;
    }
    if(!additive)
    {
        selection = { id };
        return true;
    }
    const auto it = std::find(selection.begin(), selection.end(), id);
    if(it != selection.end())
    {
        /* Ctrl-style additive re-click toggles the instance off. */
        selection.erase(it);
        return true;
    }
    selection.push_back(id);
    return true;
}

void EditorController::SetSelection(const std::vector<std::string>& ids)
{
    selection.clear();
    for(const std::string& raw : ids)
    {
        const std::string id = InstanceOf(raw);
        if(Exists(id) && !IsSelected(id))
        {
            selection.push_back(id);
        }
    }
}

void EditorController::ClearSelection()
{
    selection.clear();
}

std::string EditorController::PrimarySelection() const
{
    return selection.empty() ? std::string() : selection.back();
}

bool EditorController::IsSelected(const std::string& id) const
{
    return std::find(selection.begin(), selection.end(), id)
        != selection.end();
}

bool EditorController::PathBelongsTo(const std::string& key,
                                     const std::string& id)
{
    return key == id
        || (key.size() > id.size()
            && key.compare(0, id.size(), id) == 0
            && key[id.size()] == '/');
}

std::string EditorController::UniqueId(const std::string& base) const
{
    if(!Exists(base))
    {
        return base;
    }
    for(int i = 2; ; i++)
    {
        const std::string cand = base + "_" + std::to_string(i);
        if(!Exists(cand))
        {
            return cand;
        }
    }
}

/*---------------------------------------------------------*\
|| Instance world matrices (devices parent chain, rigid)  ||
\*---------------------------------------------------------*/
std::map<std::string, Mat4> EditorController::InstanceWorlds() const
{
    std::map<std::string, Mat4> world;
    for(const auto& kv : ws.devices)
    {
        std::vector<std::string> chain;
        std::set<std::string>        seen;
        std::string                  cur = kv.first;
        while(!cur.empty() && world.find(cur) == world.end()
              && seen.insert(cur).second)
        {
            chain.push_back(cur);
            const auto it = ws.devices.find(cur);
            cur = (it == ws.devices.end()) ? std::string()
                                           : it->second.parent;
        }
        Mat4 pw = (world.find(cur) != world.end()) ? world[cur]
                                                  : Mat4Identity();
        for(auto it = chain.rbegin(); it != chain.rend(); ++it)
        {
            /* find(), never operator[]: a dangling parent id lands in
               `chain` before the existence check, and inserting a
               phantom empty DeviceInstance into the authoring doc
               from this read-only-looking path is exactly the hazard
               class the gesture lifecycle fix removed. Treat it as
               an identity root frame. */
            const auto dit = ws.devices.find(*it);
            if(dit == ws.devices.end())
            {
                world[*it] = pw;
                continue;
            }
            const DeviceInstance& d = dit->second;
            Transform t;
            t.position     = d.position;
            t.rotation_deg = d.rotation_deg;
            pw = Mat4Mul(pw, LocalMatrix(t));
            world[*it] = pw;
        }
    }
    return world;
}

Mat4 EditorController::ParentFrame(const std::map<std::string, Mat4>& worlds,
                                   const std::string& id) const
{
    const auto it = ws.devices.find(id);
    if(it == ws.devices.end() || it->second.parent.empty())
    {
        return Mat4Identity();
    }
    const auto pw = worlds.find(it->second.parent);
    return pw != worlds.end() ? pw->second : Mat4Identity();
}

std::set<std::string> EditorController::Movable() const
{
    std::set<std::string> sel;
    for(const std::string& id : selection)
    {
        if(Exists(id) && !IsLocked(id))
        {
            sel.insert(id);
        }
    }
    /* Drop ids a selected ancestor already carries — otherwise a
       translate would apply twice (once through the ancestor's
       frame, once through the explicit local write). */
    std::set<std::string> out = sel;
    for(const std::string& id : sel)
    {
        std::set<std::string> seen;
        std::string           p = ws.devices[id].parent;
        while(!p.empty() && seen.insert(p).second)
        {
            if(sel.count(p))
            {
                out.erase(id);
                break;
            }
            const auto it = ws.devices.find(p);
            p = (it == ws.devices.end()) ? std::string()
                                       : it->second.parent;
        }
    }
    return out;
}

/*---------------------------------------------------------*\
|| Transform gesture                                        ||
\*---------------------------------------------------------*/
bool EditorController::BeginTransform()
{
    /* A second begin while a gesture is live cancels the old one —
       the snapshot is restored so a mid-drag state can never become
       the new baseline unrecorded. */
    if(gesture.active)
    {
        Cancel();
    }
    /* A live effect-layer gesture is cancelled the same way — the
       two preview kinds are mutually exclusive. */
    if(layer_gesture.active)
    {
        CancelLayerGesture();
    }
    gesture = Gesture{};
    gesture.movable = Movable();
    if(gesture.movable.empty())
    {
        return false;
    }
    const std::map<std::string, Mat4> worlds = InstanceWorlds();
    Vec3 sum {};
    for(const std::string& id : gesture.movable)
    {
        gesture.snapshot[id]      = ws.devices[id];
        gesture.world[id]         = worlds.at(id);
        gesture.parent_world[id]  = ParentFrame(worlds, id);
        sum = Add(sum, Mat4Translation(worlds.at(id)));
    }
    const float n = (float)gesture.movable.size();
    gesture.pivot  = { sum.x / n, sum.y / n, sum.z / n };
    gesture.active = true;
    return true;
}

void EditorController::PreviewTranslate(const Vec3& world_delta,
                                        EditPlane plane, bool snap)
{
    if(!gesture.active)
    {
        return;
    }
    Vec3 d = world_delta;
    switch(plane)
    {
    case EditPlane::DeskXZ:  d.y = 0.0f; break;
    case EditPlane::FrontXY: d.z = 0.0f; break;
    case EditPlane::SideYZ:  d.x = 0.0f; break;
    case EditPlane::Free:    break;
    }
    const float step = ws.meta.controls.move_snap_m;
    for(const std::string& id : gesture.movable)
    {
        const Vec3 wp  = Add(Mat4Translation(gesture.world[id]), d);
        Vec3       lp  = RigidInversePoint(gesture.parent_world[id], wp);
        if(snap)
        {
            lp = SnapTranslate(lp, step);
        }
        ws.devices[id].position = lp;
    }
}

void EditorController::PreviewRotate(const Vec3& axis, float degrees,
                                     bool snap)
{
    if(!gesture.active)
    {
        return;
    }
    const float deg = snap
        ? SnapAngle(degrees, ws.meta.controls.rotate_snap_deg)
        : degrees;
    const Quat qa = AxisAngleQuat(axis, deg);
    const Mat4 ra = QuatMatrix(qa);
    const Vec3 pv = gesture.pivot;
    for(const std::string& id : gesture.movable)
    {
        /* world pos -> around pivot -> back into the parent frame */
        const Vec3 wp  = Mat4Translation(gesture.world[id]);
        const Vec3 rel { wp.x - pv.x, wp.y - pv.y, wp.z - pv.z };
        const Vec3 rp  = RotateVec(qa, rel);
        const Vec3 nwp { pv.x + rp.x, pv.y + rp.y, pv.z + rp.z };
        ws.devices[id].position =
            RigidInversePoint(gesture.parent_world[id], nwp);

        /* world rot: qa * Rw; local rot: Rp^-1 * (qa * Rw) */
        const Mat4 rw = Mat4Mul(ra, RotationOnly(gesture.world[id]));
        const Mat4 rl = Mat4Mul(RigidInverse(gesture.parent_world[id]), rw);
        ws.devices[id].rotation_deg = EulerDegFromMat4(rl);
    }
}

std::optional<EditorEdit> EditorController::Commit()
{
    if(!gesture.active)
    {
        return std::nullopt;
    }
    EditorEdit e;
    for(const std::string& id : gesture.movable)
    {
        const DeviceInstance& before = gesture.snapshot[id];
        /* find(), never operator[]: if the document changed under the
           gesture (doc swap), an absent id must not be resurrected
           into the record — or inserted into ws.devices at all. */
        const auto it = ws.devices.find(id);
        if(it == ws.devices.end())
        {
            continue;
        }
        if(!NearInstance(before, it->second))
        {
            e.devices.before[id] = before;
            e.devices.after[id]  = it->second;
        }
    }
    const size_t n = e.devices.after.size();
    e.label = "transform " + std::to_string(n)
            + (n == 1 ? " instance" : " instances");
    gesture = Gesture{};
    if(e.Empty())
    {
        return std::nullopt;
    }
    return e;
}

void EditorController::Cancel()
{
    if(!gesture.active)
    {
        return;
    }
    for(const std::string& id : gesture.movable)
    {
        /* Restore only ids the document still has — after a doc swap
           the snapshot's old-doc entries must not resurrect. */
        const auto it = ws.devices.find(id);
        if(it != ws.devices.end())
        {
            it->second = gesture.snapshot[id];
        }
    }
    gesture = Gesture{};
}

/*---------------------------------------------------------*\
|| Numeric edits                                            ||
\*---------------------------------------------------------*/
std::optional<EditorEdit>
EditorController::SetPosition(const std::string& id, const Vec3& pos)
{
    if(gesture.active || !Exists(id) || IsLocked(id))
    {
        return std::nullopt;
    }
    if(SameVec(ws.devices[id].position, pos))
    {
        return std::nullopt;
    }
    EditorEdit e;
    e.label = "set position " + id;
    SnapshotKey(e.devices, ws.devices, id);
    ws.devices[id].position = pos;
    CaptureKey(e.devices, ws.devices, id);
    return e;
}

std::optional<EditorEdit>
EditorController::SetRotation(const std::string& id, const Vec3& rot_deg)
{
    if(gesture.active || !Exists(id) || IsLocked(id))
    {
        return std::nullopt;
    }
    if(SameVec(ws.devices[id].rotation_deg, rot_deg))
    {
        return std::nullopt;
    }
    EditorEdit e;
    e.label = "set rotation " + id;
    SnapshotKey(e.devices, ws.devices, id);
    ws.devices[id].rotation_deg = rot_deg;
    CaptureKey(e.devices, ws.devices, id);
    return e;
}

std::optional<EditorEdit>
EditorController::SetPositionWorld(const std::string& id, const Vec3& pos)
{
    if(gesture.active || !Exists(id) || IsLocked(id))
    {
        return std::nullopt;
    }
    const std::map<std::string, Mat4> worlds = InstanceWorlds();
    const Vec3 lp = RigidInversePoint(ParentFrame(worlds, id), pos);
    return SetPosition(id, lp);
}

/*---------------------------------------------------------*\
|| Align / Distribute — world-axis batch edits. Each     ||
|| member's new world position is converted back into    ||
|| its own parent frame; the whole op is one record.     ||
\*---------------------------------------------------------*/
std::optional<EditorEdit>
EditorController::Align(int axis, int mode)
{
    last_error.clear();
    if(gesture.active || axis < 0 || axis > 2 || mode < 0 || mode > 2)
    {
        return std::nullopt;
    }
    const std::set<std::string> mov = Movable();
    if(mov.size() < 2)
    {
        last_error = selection.empty()
            ? "align: nothing selected"
            : "align needs 2+ movable instances (locked ids are skipped)";
        return std::nullopt;
    }
    const std::map<std::string, Mat4> worlds = InstanceWorlds();

    float target = AxisComp(Mat4Translation(worlds.at(*mov.begin())), axis);
    if(mode == 0 || mode == 2)
    {
        for(const std::string& id : mov)
        {
            const float c = AxisComp(Mat4Translation(worlds.at(id)), axis);
            target = (mode == 0) ? std::min(target, c)
                                 : std::max(target, c);
        }
    }
    else
    {
        float sum = 0.0f;
        for(const std::string& id : mov)
        {
            sum += AxisComp(Mat4Translation(worlds.at(id)), axis);
        }
        target = sum / (float)mov.size();
    }

    EditorEdit e;
    e.label = std::string("align ") + AxisName(axis)
            + (mode == 0 ? " min" : mode == 1 ? " center" : " max");
    for(const std::string& id : mov)
    {
        const Vec3 wp  = Mat4Translation(worlds.at(id));
        Vec3       nwp = wp;
        SetAxisComp(nwp, axis, target);
        if(NearVec(wp, nwp, 1e-6f))
        {
            continue;
        }
        SnapshotKey(e.devices, ws.devices, id);
        ws.devices[id].position =
            RigidInversePoint(ParentFrame(worlds, id), nwp);
        CaptureKey(e.devices, ws.devices, id);
    }
    if(e.Empty())
    {
        return std::nullopt;
    }
    return e;
}

std::optional<EditorEdit> EditorController::Distribute(int axis)
{
    last_error.clear();
    if(gesture.active || axis < 0 || axis > 2)
    {
        return std::nullopt;
    }
    const std::set<std::string> mov = Movable();
    if(mov.size() < 3)
    {
        last_error = selection.empty()
            ? "distribute: nothing selected"
            : "distribute needs 3+ movable instances (locked ids are skipped)";
        return std::nullopt;
    }
    const std::map<std::string, Mat4> worlds = InstanceWorlds();

    /* Sort by the axis coordinate; endpoints hold, interior lands
       at equal intervals between them. */
    std::vector<std::pair<float, std::string>> order;
    for(const std::string& id : mov)
    {
        order.emplace_back(AxisComp(Mat4Translation(worlds.at(id)), axis), id);
    }
    std::sort(order.begin(), order.end());
    const float lo   = order.front().first;
    const float hi   = order.back().first;
    const float step = (hi - lo) / (float)(order.size() - 1);

    EditorEdit e;
    e.label = std::string("distribute ") + AxisName(axis);
    for(size_t i = 1; i + 1 < order.size(); i++)
    {
        const std::string& id = order[i].second;
        const Vec3 wp  = Mat4Translation(worlds.at(id));
        Vec3       nwp = wp;
        SetAxisComp(nwp, axis, lo + step * (float)i);
        if(NearVec(wp, nwp, 1e-6f))
        {
            continue;
        }
        SnapshotKey(e.devices, ws.devices, id);
        ws.devices[id].position =
            RigidInversePoint(ParentFrame(worlds, id), nwp);
        CaptureKey(e.devices, ws.devices, id);
    }
    if(e.Empty())
    {
        return std::nullopt;
    }
    return e;
}

/*---------------------------------------------------------*\
|| Rename — re-keys every section that references the     ||
|| instance subtree.                                       ||
\*---------------------------------------------------------*/
std::optional<EditorEdit>
EditorController::Rename(const std::string& id, const std::string& new_id)
{
    last_error.clear();
    if(gesture.active || !Exists(id) || new_id == id
       || !IsPresetId(new_id) || Exists(new_id))
    {
        return std::nullopt;
    }
    /* Re-keying a child's `parent` field is a reparent — refuse when
       the instance has a locked child rather than rewriting one. */
    for(const auto& kv : ws.devices)
    {
        if(kv.second.parent == id && IsLocked(kv.first))
        {
            last_error = "rename refused: locked child " + kv.first;
            return std::nullopt;
        }
    }
    EditorEdit e;
    e.label = "rename " + id + " -> " + new_id;

    SnapshotKey(e.devices, ws.devices, id);
    const DeviceInstance inst = ws.devices[id];
    ws.devices.erase(id);
    ws.devices[new_id] = inst;
    CaptureKey(e.devices, ws.devices, new_id);

    /* children keep the instance as their parent frame */
    for(auto& kv : ws.devices)
    {
        if(kv.second.parent == id)
        {
            SnapshotKey(e.devices, ws.devices, kv.first);
            kv.second.parent = new_id;
            CaptureKey(e.devices, ws.devices, kv.first);
        }
    }

    /* settings keys under the instance subtree */
    std::vector<std::string> skeys;
    for(const auto& kv : ws.device_settings)
    {
        if(PathBelongsTo(kv.first, id))
        {
            skeys.push_back(kv.first);
        }
    }
    for(const std::string& k : skeys)
    {
        SnapshotKey(e.settings, ws.device_settings, k);
        const DeviceSettings v = ws.device_settings[k];
        ws.device_settings.erase(k);
        ws.device_settings[new_id + k.substr(id.size())] = v;
        CaptureKey(e.settings, ws.device_settings,
                   new_id + k.substr(id.size()));
    }

    /* mirror_of references pointing into the subtree */
    for(auto& kv : ws.device_settings)
    {
        if(!kv.second.mirror_of.empty()
           && PathBelongsTo(kv.second.mirror_of, id))
        {
            SnapshotKey(e.settings, ws.device_settings, kv.first);
            kv.second.mirror_of =
                new_id + kv.second.mirror_of.substr(id.size());
            CaptureKey(e.settings, ws.device_settings, kv.first);
        }
    }

    /* paint keyed by resolved instance/entity path */
    std::vector<std::string> ckeys;
    for(const auto& kv : ws.object_colors)
    {
        if(PathBelongsTo(kv.first, id))
        {
            ckeys.push_back(kv.first);
        }
    }
    for(const std::string& k : ckeys)
    {
        SnapshotKey(e.object_colors, ws.object_colors, k);
        const SceneColor v = ws.object_colors[k];
        ws.object_colors.erase(k);
        ws.object_colors[new_id + k.substr(id.size())] = v;
        CaptureKey(e.object_colors, ws.object_colors,
                   new_id + k.substr(id.size()));
    }
    ckeys.clear();
    for(const auto& kv : ws.emitter_colors)
    {
        if(PathBelongsTo(kv.first, id))
        {
            ckeys.push_back(kv.first);
        }
    }
    for(const std::string& k : ckeys)
    {
        SnapshotKey(e.emitter_colors, ws.emitter_colors, k);
        const auto v = ws.emitter_colors[k];
        ws.emitter_colors.erase(k);
        ws.emitter_colors[new_id + k.substr(id.size())] = v;
        CaptureKey(e.emitter_colors, ws.emitter_colors,
                   new_id + k.substr(id.size()));
    }

    for(std::string& s : selection)
    {
        if(s == id)
        {
            s = new_id;
        }
    }
    return e;
}

/*---------------------------------------------------------*\
|| Visibility / lock                                        ||
\*---------------------------------------------------------*/
std::optional<EditorEdit>
EditorController::SetVisible(const std::string& id, bool on)
{
    last_error.clear();
    if(gesture.active || !Exists(id))
    {
        if(!Exists(id))
        {
            last_error = "visibility refused: unknown instance " + id;
        }
        return std::nullopt;
    }
    const auto it = ws.device_settings.find(id);
    const bool cur = (it == ws.device_settings.end()) || it->second.visible;
    if(cur == on)
    {
        return std::nullopt;
    }
    EditorEdit e;
    e.label = on ? "show " + id : "hide " + id;
    SnapshotKey(e.settings, ws.device_settings, id);
    ws.device_settings[id].visible = on;
    CaptureKey(e.settings, ws.device_settings, id);
    return e;
}

std::optional<EditorEdit>
EditorController::SetLocked(const std::string& id, bool on)
{
    last_error.clear();
    if(gesture.active || !Exists(id))
    {
        if(!Exists(id))
        {
            last_error = "lock refused: unknown instance " + id;
        }
        return std::nullopt;
    }
    const auto it = ws.device_settings.find(id);
    const bool cur = (it != ws.device_settings.end()) && it->second.locked;
    if(cur == on)
    {
        return std::nullopt;
    }
    EditorEdit e;
    e.label = on ? "lock " + id : "unlock " + id;
    SnapshotKey(e.settings, ws.device_settings, id);
    ws.device_settings[id].locked = on;
    CaptureKey(e.settings, ws.device_settings, id);
    return e;
}

/*---------------------------------------------------------*\
|| Delete — instance plus descendants, dangling mirror    ||
|| references cleared. Everything needed for revert is    ||
|| stored in the record.                                  ||
\*---------------------------------------------------------*/
std::set<std::string>
EditorController::DeleteCascade(const std::vector<std::string>& ids) const
{
    std::set<std::string> kill;
    for(const std::string& id : ids)
    {
        const std::string iid = InstanceOf(id);
        if(Exists(iid) && !IsLocked(iid))
        {
            kill.insert(iid);
        }
    }
    /* Cascade: descendants ride along so no dangling parent is
       left behind. */
    bool grew = true;
    while(grew)
    {
        grew = false;
        for(const auto& kv : ws.devices)
        {
            if(!kv.second.parent.empty()
               && kill.count(kv.second.parent)
               && !kill.count(kv.first))
            {
                kill.insert(kv.first);
                grew = true;
            }
        }
    }
    return kill;
}

std::optional<EditorEdit>
EditorController::Delete(const std::vector<std::string>& ids)
{
    last_error.clear();
    if(gesture.active)
    {
        return std::nullopt;
    }
    std::set<std::string> kill = DeleteCascade(ids);
    if(kill.empty())
    {
        /* Nothing survived the filter — report a locked selection
           instead of silently no-op'ing (all-missing ids stay
           silent: there was nothing real to delete). */
        for(const std::string& id : ids)
        {
            const std::string iid = InstanceOf(id);
            if(Exists(iid) && IsLocked(iid))
            {
                last_error = "delete refused: selection is locked";
                break;
            }
        }
        return std::nullopt;
    }
    /* Protective lock semantics: the cascade must never swallow a
       locked descendant — the whole delete is refused, not silently
       narrowed. */
    for(const std::string& id : kill)
    {
        if(IsLocked(id))
        {
            last_error = "delete refused: locked descendant " + id;
            return std::nullopt;
        }
    }

    EditorEdit e;
    e.label = "delete " + std::to_string(kill.size())
            + (kill.size() == 1 ? " instance" : " instances");

    for(const std::string& id : kill)
    {
        SnapshotKey(e.devices, ws.devices, id);
        ws.devices.erase(id);
    }
    for(const std::string& id : kill)
    {
        selection.erase(std::remove(selection.begin(), selection.end(), id),
                        selection.end());
    }

    /* settings + color entries under each removed subtree */
    std::vector<std::string> skeys;
    for(const auto& kv : ws.device_settings)
    {
        for(const std::string& id : kill)
        {
            if(PathBelongsTo(kv.first, id))
            {
                skeys.push_back(kv.first);
                break;
            }
        }
    }
    for(const std::string& k : skeys)
    {
        SnapshotKey(e.settings, ws.device_settings, k);
        ws.device_settings.erase(k);
    }

    /* mirror_of targets that no longer exist would fail
       resolution — clear them, recorded for revert. */
    for(auto& kv : ws.device_settings)
    {
        if(kv.second.mirror_of.empty())
        {
            continue;
        }
        for(const std::string& id : kill)
        {
            if(PathBelongsTo(kv.second.mirror_of, id))
            {
                SnapshotKey(e.settings, ws.device_settings, kv.first);
                kv.second.mirror_of.clear();
                CaptureKey(e.settings, ws.device_settings, kv.first);
                break;
            }
        }
    }

    std::vector<std::string> ckeys;
    for(const auto& kv : ws.object_colors)
    {
        for(const std::string& id : kill)
        {
            if(PathBelongsTo(kv.first, id))
            {
                ckeys.push_back(kv.first);
                break;
            }
        }
    }
    for(const std::string& k : ckeys)
    {
        SnapshotKey(e.object_colors, ws.object_colors, k);
        ws.object_colors.erase(k);
    }
    ckeys.clear();
    for(const auto& kv : ws.emitter_colors)
    {
        for(const std::string& id : kill)
        {
            if(PathBelongsTo(kv.first, id))
            {
                ckeys.push_back(kv.first);
                break;
            }
        }
    }
    for(const std::string& k : ckeys)
    {
        SnapshotKey(e.emitter_colors, ws.emitter_colors, k);
        ws.emitter_colors.erase(k);
    }
    return e;
}

std::optional<EditorEdit> EditorController::DeleteSelected()
{
    return Delete(selection);
}

/*---------------------------------------------------------*\
|| Group — new "group" instance at the shared pivot;      ||
|| children keep world placement.                         ||
\*---------------------------------------------------------*/
std::optional<EditorEdit>
EditorController::Group(const std::string& base_id)
{
    last_error.clear();
    if(gesture.active)
    {
        return std::nullopt;
    }
    const std::set<std::string> mov = Movable();
    if(mov.empty())
    {
        last_error = selection.empty()
            ? "group: nothing selected"
            : "group refused: selection is locked";
        return std::nullopt;
    }
    const std::map<std::string, Mat4> worlds = InstanceWorlds();
    Vec3 sum {};
    for(const std::string& id : mov)
    {
        sum = Add(sum, Mat4Translation(worlds.at(id)));
    }
    const float n = (float)mov.size();
    const Vec3 pivot { sum.x / n, sum.y / n, sum.z / n };
    const std::string gid = UniqueId(base_id);

    /* Nest under the common parent: when every movable member shares
       the same non-empty parent, the group lives there too so a later
       move of that parent still carries them. Mixed or root-level
       parents -> the group roots at the world pivot. */
    std::string common;
    bool        first = true;
    for(const std::string& id : mov)
    {
        const std::string& p = ws.devices[id].parent;
        if(first)
        {
            common = p;
            first  = false;
        }
        else if(p != common)
        {
            common.clear();
            break;
        }
    }
    Mat4 gpf = Mat4Identity();
    if(!common.empty())
    {
        const auto pw = worlds.find(common);
        if(pw != worlds.end())
        {
            gpf = pw->second;
        }
        else
        {
            common.clear();    /* dangling parent ref — root the group */
        }
    }

    EditorEdit e;
    e.label = "group " + std::to_string(mov.size())
            + (mov.size() == 1 ? " instance" : " instances");

    /* The group keeps identity rotation in its parent frame; its
       local position is the world pivot expressed parent-locally. */
    DeviceInstance g;
    g.type     = "group";
    g.parent   = common;
    g.position = RigidInversePoint(gpf, pivot);
    SnapshotKey(e.devices, ws.devices, gid);
    ws.devices[gid] = g;
    CaptureKey(e.devices, ws.devices, gid);

    /* Group world = parent frame * T(group local). Children are
       re-expressed in it: pos = Gw^-1 * pw, rot = Gw^-1 * Rw. */
    Transform gt;
    gt.position   = g.position;
    const Mat4 gw   = Mat4Mul(gpf, LocalMatrix(gt));
    const Mat4 ginv = RigidInverse(gw);

    for(const std::string& id : mov)
    {
        SnapshotKey(e.devices, ws.devices, id);
        DeviceInstance& d = ws.devices[id];
        const Mat4      w = worlds.at(id);
        d.parent        = gid;
        d.position      = RigidInversePoint(gw, Mat4Translation(w));
        d.rotation_deg  = EulerDegFromMat4(Mat4Mul(ginv, RotationOnly(w)));
        CaptureKey(e.devices, ws.devices, id);
    }

    selection = { gid };
    return e;
}

/*---------------------------------------------------------*\
|| Ungroup — dissolve selected type-"group" instances,    ||
|| children reparented to the group's parent with world   ||
|| placement preserved.                                    ||
\*---------------------------------------------------------*/
std::optional<EditorEdit> EditorController::Ungroup()
{
    last_error.clear();
    if(gesture.active)
    {
        return std::nullopt;
    }
    std::vector<std::string> targets;
    for(const std::string& id : selection)
    {
        const auto it = ws.devices.find(id);
        if(it != ws.devices.end() && it->second.type == "group"
           && !IsLocked(id))
        {
            targets.push_back(id);
        }
    }
    if(targets.empty())
    {
        return std::nullopt;
    }
    /* Reparenting a locked child would defeat the lock — refuse the
       ungroup entirely. */
    for(const std::string& gid : targets)
    {
        for(const auto& kv : ws.devices)
        {
            if(kv.second.parent == gid && IsLocked(kv.first))
            {
                last_error = "ungroup refused: locked child " + kv.first;
                return std::nullopt;
            }
        }
    }
    const std::map<std::string, Mat4> worlds = InstanceWorlds();

    EditorEdit e;
    e.label = "ungroup";

    for(const std::string& gid : targets)
    {
        const std::string new_parent = ws.devices[gid].parent;
        Mat4              npf        = Mat4Identity();
        if(!new_parent.empty())
        {
            const auto pw = worlds.find(new_parent);
            if(pw != worlds.end())
            {
                npf = pw->second;
            }
        }

        std::vector<std::string> children;
        for(const auto& kv : ws.devices)
        {
            if(kv.second.parent == gid)
            {
                children.push_back(kv.first);
            }
        }
        for(const std::string& c : children)
        {
            SnapshotKey(e.devices, ws.devices, c);
            DeviceInstance& d = ws.devices[c];
            const Mat4      cw = worlds.at(c);
            d.parent     = new_parent;
            d.position   = RigidInversePoint(npf, Mat4Translation(cw));
            const Mat4 rl = Mat4Mul(RigidInverse(npf), RotationOnly(cw));
            d.rotation_deg = EulerDegFromMat4(rl);
            CaptureKey(e.devices, ws.devices, c);
        }

        SnapshotKey(e.devices, ws.devices, gid);
        ws.devices.erase(gid);

        std::vector<std::string> skeys;
        for(const auto& kv : ws.device_settings)
        {
            if(PathBelongsTo(kv.first, gid))
            {
                skeys.push_back(kv.first);
            }
        }
        for(const std::string& k : skeys)
        {
            SnapshotKey(e.settings, ws.device_settings, k);
            ws.device_settings.erase(k);
        }
        for(auto& kv : ws.device_settings)
        {
            if(!kv.second.mirror_of.empty()
               && PathBelongsTo(kv.second.mirror_of, gid))
            {
                SnapshotKey(e.settings, ws.device_settings, kv.first);
                kv.second.mirror_of.clear();
                CaptureKey(e.settings, ws.device_settings, kv.first);
            }
        }

        std::vector<std::string> ckeys;
        for(const auto& kv : ws.object_colors)
        {
            if(PathBelongsTo(kv.first, gid))
            {
                ckeys.push_back(kv.first);
            }
        }
        for(const std::string& k : ckeys)
        {
            SnapshotKey(e.object_colors, ws.object_colors, k);
            ws.object_colors.erase(k);
        }
        ckeys.clear();
        for(const auto& kv : ws.emitter_colors)
        {
            if(PathBelongsTo(kv.first, gid))
            {
                ckeys.push_back(kv.first);
            }
        }
        for(const std::string& k : ckeys)
        {
            SnapshotKey(e.emitter_colors, ws.emitter_colors, k);
            ws.emitter_colors.erase(k);
        }

        selection.erase(
            std::remove(selection.begin(), selection.end(), gid),
            selection.end());
    }
    return e;
}

/*---------------------------------------------------------*\
|| DuplicateMirrored — Linked visual copy, never a        ||
|| second output writer.                                   ||
\*---------------------------------------------------------*/
std::optional<EditorEdit> EditorController::DuplicateMirrored()
{
    last_error.clear();
    if(gesture.active)
    {
        return std::nullopt;
    }
    std::vector<std::string> sources;
    for(const std::string& id : selection)
    {
        if(Exists(id))
        {
            sources.push_back(id);
        }
    }
    if(sources.empty())
    {
        last_error = "duplicate: nothing selected";
        return std::nullopt;
    }

    EditorEdit e;
    e.label = "mirror " + std::to_string(sources.size())
            + (sources.size() == 1 ? " instance" : " instances");
    std::vector<std::string> created;
    for(const std::string& src : sources)
    {
        const DeviceInstance& s = ws.devices[src];

        /* Output ownership: mirror the source's effective owner
           (mirroring a mirror would form a chain the resolver
           rejects). zones stays empty — no second writer. The new
           id derives from that owner so repeated duplicates read
           fan0_mirror, fan0_mirror_2, ... */
        DeviceSettings ns;
        const auto sit = ws.device_settings.find(src);
        if(sit != ws.device_settings.end())
        {
            ns.visible   = sit->second.visible;
            ns.mirror_of = sit->second.mirror_of.empty()
                        ? src : sit->second.mirror_of;
        }
        else
        {
            ns.mirror_of = src;
        }
        const std::string nid =
            UniqueId(InstanceOf(ns.mirror_of) + "_mirror");

        DeviceInstance n = s;
        n.position.x += 0.03f;      /* offset so the copy is visible */
        SnapshotKey(e.devices, ws.devices, nid);
        ws.devices[nid] = n;
        CaptureKey(e.devices, ws.devices, nid);
        SnapshotKey(e.settings, ws.device_settings, nid);
        ws.device_settings[nid] = ns;
        CaptureKey(e.settings, ws.device_settings, nid);

        created.push_back(nid);
    }
    selection = created;
    return e;
}

/*---------------------------------------------------------*\
|| Device-library ops (task 4.2).                           ||
\*---------------------------------------------------------*/
std::optional<EditorEdit>
EditorController::AddInstance(const std::string& type_id, const Vec3& pos)
{
    last_error.clear();
    if(gesture.active || !IsPresetId(type_id))
    {
        return std::nullopt;
    }
    /* Unique stable id straight off the type: "fan-120", then
       "fan-120_2", "fan-120_3", ... */
    const std::string nid = UniqueId(type_id);

    DeviceInstance d;
    d.type     = type_id;
    d.position = pos;            /* no parent — a root placement */
    EditorEdit e;
    e.label = "add " + nid;
    SnapshotKey(e.devices, ws.devices, nid);
    ws.devices[nid] = d;
    CaptureKey(e.devices, ws.devices, nid);
    selection = { nid };
    return e;
}

std::optional<EditorEdit>
EditorController::Retype(const std::string& id, const std::string& new_type)
{
    last_error.clear();
    if(gesture.active || !Exists(id) || !IsPresetId(new_type))
    {
        return std::nullopt;
    }
    if(ws.devices[id].type == new_type)
    {
        return std::nullopt;
    }
    EditorEdit e;
    e.label = "repoint " + id + " -> " + new_type;
    SnapshotKey(e.devices, ws.devices, id);
    ws.devices[id].type = new_type;
    CaptureKey(e.devices, ws.devices, id);
    return e;
}

bool EditorController::BuildPresetFromInstances(
    const std::vector<std::string>& ids, const std::string& new_id,
    const std::string& name, const PresetRegistry& reg,
    DevicePreset& out)
{
    last_error.clear();
    if(gesture.active)
    {
        last_error = "create preset refused: finish the drag first";
        return false;
    }
    /* Dedupe while keeping first-seen order — a double-passed id
       would collide on the entity key. */
    std::vector<std::string> insts;
    for(const std::string& id : ids)
    {
        /* Root instances only — a resolved path like
           "case/case_fans" names a nested child, not a placement.
           InstanceOf() must NOT run here: coercing the path to its
           root would silently build a different type than asked. */
        if(id.find('/') != std::string::npos)
        {
            last_error = "create preset refused: '" + id
                       + "' is not a root instance id "
                         "(nested paths are not accepted)";
            return false;
        }
        if(ws.devices.find(id) == ws.devices.end())
        {
            last_error = "create preset refused: '" + id
                       + "' is not a desk instance";
            return false;
        }
        if(!IsPresetId(id))
        {
            /* The entity id is reused verbatim — an instance id that
               can't be an entity id would produce an unwritable file. */
            last_error = "create preset refused: instance id '" + id
                       + "' is not a valid entity id";
            return false;
        }
        if(reg.Find(ws.devices[id].type) == nullptr)
        {
            last_error = "create preset refused: '" + id
                       + "' has unknown type '" + ws.devices[id].type + "'";
            return false;
        }
        if(std::find(insts.begin(), insts.end(), id) == insts.end())
        {
            insts.push_back(id);
        }
    }
    if(insts.empty())
    {
        last_error = "create preset refused: nothing selected";
        return false;
    }

    /* Shared origin: centroid of the instances' world positions on
       the desk plane (x,z), y = the lowest instance's world y. The
       new type's origin then sits under the middle of the
       arrangement at desk height, and each child ref keeps the
       instance's relative world placement + world rotation. */
    const std::map<std::string, Mat4> worlds = InstanceWorlds();
    Vec3  sum {};
    float min_y = 0.0f;
    bool  first = true;
    for(const std::string& id : insts)
    {
        const Vec3 wp = Mat4Translation(worlds.at(id));
        sum = Add(sum, wp);
        if(first || wp.y < min_y)
        {
            min_y = wp.y;
            first = false;
        }
    }
    const float n = (float)insts.size();
    const Vec3 origin { sum.x / n, min_y, sum.z / n };

    DevicePreset p;
    p.id       = new_id;
    p.name     = name;
    p.category = "custom";
    for(const std::string& id : insts)
    {
        const Mat4& w = worlds.at(id);
        const Vec3  wp = Mat4Translation(w);
        PresetEntity e;
        e.id   = id;
        e.type = ws.devices[id].type;
        e.position = { wp.x - origin.x, wp.y - origin.y,
                       wp.z - origin.z };
        e.rotation_deg = EulerDegFromMat4(RotationOnly(w));
        p.entities[e.id] = e;
    }
    out = p;
    return true;
}

unsigned int EditorController::BakePaintedColors(
    const std::string& iid, DevicePreset& variant) const
{
    /* object_colors keys are resolved paths "<inst>/<entity>".
       Stripping the "<iid>/" prefix leaves the entity id for a
       direct child — or "<child>/<sub>" for paint inside a nested
       child type, which can never be an entity id here ('/' isn't
       in the id charset) so the plain map lookup filters both. */
    unsigned int   painted = 0;
    const std::string prefix = iid + "/";
    for(const auto& kv : ws.object_colors)
    {
        if(kv.first.compare(0, prefix.size(), prefix) != 0)
        {
            continue;
        }
        const auto eit = variant.entities.find(kv.first.substr(
            prefix.size()));
        if(eit == variant.entities.end())
        {
            continue;
        }
        if(!eit->second.appearance.is_object())
        {
            eit->second.appearance = nlohmann::json::object();
        }
        eit->second.appearance["body_color"] = SceneColorHex(kv.second);
        painted++;
    }
    return painted;
}

/*---------------------------------------------------------*\
|| Zone binding (task 4.3) — per-instance settings rows   ||
|| plus the bindings entry they reference. Never writes   ||
|| a preset file; never resizes hardware.                 ||
\*---------------------------------------------------------*/
static bool SameBindingIdentity(const DeviceBinding& a,
                                const DeviceBinding& b)
{
    return a.controller_name == b.controller_name
        && a.vendor         == b.vendor
        && a.serial         == b.serial
        && a.location       == b.location
        && a.device_type    == b.device_type
        && a.zone_name      == b.zone_name
        && a.zone_leds      == b.zone_leds;
}

std::optional<EditorEdit>
EditorController::BindZone(const std::string& iid,
                           const std::string& zone_id,
                           const DeviceBinding& binding,
                           int addr_base, bool verified)
{
    last_error.clear();
    if(gesture.active)
    {
        last_error = "bind refused: finish the drag first";
        return std::nullopt;
    }
    if(!Exists(iid))
    {
        last_error = "bind refused: unknown instance " + iid;
        return std::nullopt;
    }
    if(IsLocked(iid))
    {
        last_error = "bind refused: " + iid + " is locked";
        return std::nullopt;
    }
    if(zone_id.empty() || binding.id.empty()
       || binding.controller_name.empty() || binding.zone_name.empty())
    {
        last_error = "bind refused: incomplete binding identity";
        return std::nullopt;
    }
    if(addr_base < 0)
    {
        addr_base = 0;
    }

    const auto bit = ws.bindings.find(binding.id);
    if(bit != ws.bindings.end() && !SameBindingIdentity(bit->second, binding))
    {
        last_error = "bind refused: binding id '" + binding.id
                   + "' already names different hardware";
        return std::nullopt;
    }

    /* True no-op: the binding already exists identically and the
       zone row already says what was asked. */
    const auto sit = ws.device_settings.find(iid);
    if(bit != ws.bindings.end() && sit != ws.device_settings.end())
    {
        const auto zit = sit->second.zones.find(zone_id);
        if(zit != sit->second.zones.end()
           && zit->second.binding   == binding.id
           && zit->second.addr_base == addr_base
           && zit->second.verified  == verified)
        {
            return std::nullopt;
        }
    }

    EditorEdit e;
    e.label = "bind " + iid + "/" + zone_id;
    if(bit == ws.bindings.end())
    {
        SnapshotKey(e.bindings, ws.bindings, binding.id);
        ws.bindings[binding.id] = binding;
        CaptureKey(e.bindings, ws.bindings, binding.id);
    }
    SnapshotKey(e.settings, ws.device_settings, iid);
    ws.device_settings[iid].zones[zone_id] =
        { binding.id, addr_base, verified };
    CaptureKey(e.settings, ws.device_settings, iid);
    return e;
}

std::optional<EditorEdit>
EditorController::UnbindZone(const std::string& iid,
                             const std::string& zone_id)
{
    last_error.clear();
    if(gesture.active)
    {
        last_error = "unbind refused: finish the drag first";
        return std::nullopt;
    }
    if(!Exists(iid))
    {
        last_error = "unbind refused: unknown instance " + iid;
        return std::nullopt;
    }
    if(IsLocked(iid))
    {
        last_error = "unbind refused: " + iid + " is locked";
        return std::nullopt;
    }
    const auto sit = ws.device_settings.find(iid);
    if(sit == ws.device_settings.end())
    {
        return std::nullopt;
    }
    const auto zit = sit->second.zones.find(zone_id);
    if(zit == sit->second.zones.end()
       || (zit->second.binding.empty() && zit->second.addr_base == 0
           && !zit->second.verified))
    {
        return std::nullopt;
    }
    EditorEdit e;
    e.label = "unbind " + iid + "/" + zone_id;
    SnapshotKey(e.settings, ws.device_settings, iid);
    ws.device_settings[iid].zones.erase(zone_id);
    CaptureKey(e.settings, ws.device_settings, iid);
    return e;
}

std::optional<EditorEdit>
EditorController::SetZoneParams(const std::string& iid,
                                const std::string& zone_id,
                                int addr_base, bool verified)
{
    last_error.clear();
    if(gesture.active)
    {
        last_error = "zone params refused: finish the drag first";
        return std::nullopt;
    }
    if(!Exists(iid))
    {
        last_error = "zone params refused: unknown instance " + iid;
        return std::nullopt;
    }
    if(IsLocked(iid))
    {
        last_error = "zone params refused: " + iid + " is locked";
        return std::nullopt;
    }
    if(addr_base < 0)
    {
        addr_base = 0;
    }
    const auto sit = ws.device_settings.find(iid);
    if(sit == ws.device_settings.end())
    {
        last_error = "zone params refused: " + iid + " has no zones";
        return std::nullopt;
    }
    const auto zit = sit->second.zones.find(zone_id);
    if(zit == sit->second.zones.end() || zit->second.binding.empty())
    {
        last_error = "zone params refused: " + iid + "/" + zone_id
                   + " is not bound";
        return std::nullopt;
    }
    if(zit->second.addr_base == addr_base
       && zit->second.verified == verified)
    {
        return std::nullopt;
    }
    EditorEdit e;
    e.label = "zone params " + iid + "/" + zone_id;
    SnapshotKey(e.settings, ws.device_settings, iid);
    DeviceSettings& s = ws.device_settings[iid];
    s.zones[zone_id].addr_base = addr_base;
    s.zones[zone_id].verified  = verified;
    CaptureKey(e.settings, ws.device_settings, iid);
    return e;
}

/*---------------------------------------------------------*\
|| Effect layers (task 5.2) — authored inline stack         ||
\*---------------------------------------------------------*/
namespace
{

bool SameColorF(const ColorF& a, const ColorF& b)
{
    return a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a;
}

bool SameLayer(const EffectLayer& a, const EffectLayer& b)
{
    if(a.primitive != b.primitive || a.space != b.space
       || a.blend != b.blend || a.enabled != b.enabled
       || a.opacity != b.opacity || a.speed != b.speed
       || a.scale != b.scale || a.phase != b.phase
       || a.density != b.density || a.seed != b.seed
       || a.source != b.source || a.targets != b.targets
       || !SameVec(a.origin, b.origin)
       || !SameVec(a.direction, b.direction)
       || a.path.size() != b.path.size()
       || a.palette.stops.size() != b.palette.stops.size())
    {
        return false;
    }
    for(size_t i = 0; i < a.path.size(); i++)
    {
        if(!SameVec(a.path[i], b.path[i]))
        {
            return false;
        }
    }
    for(size_t i = 0; i < a.palette.stops.size(); i++)
    {
        if(a.palette.stops[i].pos != b.palette.stops[i].pos
           || !SameColorF(a.palette.stops[i].color,
                          b.palette.stops[i].color))
        {
            return false;
        }
    }
    return true;
}

bool SameStack(const std::vector<EffectLayer>& a,
               const std::vector<EffectLayer>& b)
{
    if(a.size() != b.size())
    {
        return false;
    }
    for(size_t i = 0; i < a.size(); i++)
    {
        if(!SameLayer(a[i], b[i]))
        {
            return false;
        }
    }
    return true;
}

} /* anonymous namespace */

void EditorController::SetLayerResolver(LayerResolver resolver, void* ctx)
{
    layer_resolver     = resolver;
    layer_resolver_ctx = ctx;
}

EffectDelta EditorController::SnapEffect() const
{
    EffectDelta d;
    d.preset = ws.effect.preset;
    d.seed   = ws.effect.seed;
    d.layers = ws.effect.layers;
    return d;
}

bool EditorController::EffectDeltaEqual(const EffectDelta& a,
                                        const EffectDelta& b) const
{
    return a.preset == b.preset && a.seed == b.seed
        && SameStack(a.layers, b.layers);
}

void EditorController::RestoreEffect(const EffectDelta& d)
{
    ws.effect.preset = d.preset;
    ws.effect.seed   = d.seed;
    ws.effect.layers = d.layers;
}

bool EditorController::EnsureLayers()
{
    if(!ws.effect.layers.empty())
    {
        return true;                            /* already inline   */
    }
    if(ws.effect.preset.empty())
    {
        return true;                            /* no preset to     */
    }                                           /* materialize      */
    if(layer_resolver == nullptr)
    {
        last_error = "effect edits need a look resolver";
        return false;
    }
    std::vector<EffectLayer> out;
    if(!layer_resolver(ws.effect.preset, ws.effect.seed, out,
                       layer_resolver_ctx))
    {
        last_error = "effect look '" + ws.effect.preset
                   + "' does not resolve";
        return false;
    }
    ws.effect.layers = out;
    return true;
}

EffectLayer* EditorController::LayerAt(size_t i)
{
    if(!EnsureLayers() || i >= ws.effect.layers.size())
    {
        return nullptr;
    }
    return &ws.effect.layers[i];
}

/* Restore `before` after a refused/no-op write so a materialization
   performed for the op doesn't leak into the document unrecorded. */
std::optional<EditorEdit>
EditorController::UndoEffectOp(const EffectDelta& before)
{
    RestoreEffect(before);
    return std::nullopt;
}

std::optional<EditorEdit>
EditorController::FinishEffectEdit(const EffectDelta& before,
                                   const std::string& label)
{
    /* Inside a layer gesture the write already landed in ws — the
       Commit folds the whole gesture into one record. */
    if(layer_gesture.active)
    {
        return std::nullopt;
    }
    const EffectDelta after = SnapEffect();
    if(EffectDeltaEqual(before, after))
    {
        /* No net change (incl. a materialize-only op) — restore so
           an untouched preset stays registry-resolved. */
        RestoreEffect(before);
        return std::nullopt;
    }
    EditorEdit e;
    e.label         = label;
    e.has_effect    = true;
    e.effect_before = before;
    e.effect_after  = after;
    return e;
}

bool EditorController::BeginLayerGesture()
{
    /* Same exclusivity rules as the transform gesture: a live one
       is cancelled (snapshot restored) before the new begin. */
    if(layer_gesture.active)
    {
        CancelLayerGesture();
    }
    if(gesture.active)
    {
        Cancel();
    }
    layer_gesture        = LayerGesture{};
    layer_gesture.begin  = SnapEffect();
    /* Materialize up front so preview ops edit the inline stack;
       a preset that doesn't resolve still allows AddLayer. */
    EnsureLayers();
    layer_gesture.base   = SnapEffect();
    layer_gesture.active = true;
    return true;
}

std::optional<EditorEdit>
EditorController::CommitLayerGesture(const std::string& label)
{
    if(!layer_gesture.active)
    {
        return std::nullopt;
    }
    const EffectDelta after  = SnapEffect();
    const EffectDelta begin  = layer_gesture.begin;
    const EffectDelta base   = layer_gesture.base;
    layer_gesture            = LayerGesture{};
    if(EffectDeltaEqual(after, base)
       || EffectDeltaEqual(after, begin))
    {
        /* No net change against EITHER baseline — restore the
           pre-gesture snapshot (undoes a materialize-only begin,
           or a scrub that wandered back to its start value) and
           push no record. */
        RestoreEffect(begin);
        return std::nullopt;
    }
    EditorEdit e;
    e.label         = label;
    e.has_effect    = true;
    e.effect_before = begin;
    e.effect_after  = after;
    return e;
}

void EditorController::CancelLayerGesture()
{
    if(!layer_gesture.active)
    {
        return;
    }
    RestoreEffect(layer_gesture.begin);
    layer_gesture = LayerGesture{};
}

/*---------------------------------------------------------*\
|| Stack-level ops                                          ||
\*---------------------------------------------------------*/
std::optional<EditorEdit>
EditorController::MoveLayer(size_t from, size_t to)
{
    if(gesture.active)
    {
        return std::nullopt;
    }
    last_error.clear();
    const EffectDelta before = SnapEffect();
    if(!EnsureLayers())
    {
        return std::nullopt;
    }
    const size_t n = ws.effect.layers.size();
    if(from >= n || to >= n || from == to)
    {
        return UndoEffectOp(before);
    }
    EffectLayer tmp = ws.effect.layers[from];
    ws.effect.layers.erase(ws.effect.layers.begin() + (ptrdiff_t)from);
    ws.effect.layers.insert(ws.effect.layers.begin() + (ptrdiff_t)to,
                            tmp);
    return FinishEffectEdit(before, "reorder effect layer");
}

std::optional<EditorEdit>
EditorController::AddLayer(const std::string& primitive)
{
    if(gesture.active)
    {
        return std::nullopt;
    }
    last_error.clear();
    if(!IsPrimitive(primitive))
    {
        last_error = "unknown primitive '" + primitive + "'";
        return std::nullopt;
    }
    const EffectDelta before = SnapEffect();
    /* Best-effort materialize: a preset that fails to resolve
       (deleted file, unknown id, no resolver) must NOT dead-end
       "+ add layer" — the new layer simply starts a fresh inline
       stack and the dead id stays as provenance (the reload path
       already reports the loss). Per-layer ops still refuse a
       failed materialize — there is nothing to index into. */
    EnsureLayers();
    last_error.clear();
    if(ws.effect.layers.size() >= EFFECT_MAX_LAYERS)
    {
        last_error = "layer count exceeds cap "
                   + std::to_string(EFFECT_MAX_LAYERS);
        return UndoEffectOp(before);
    }
    /* Sensible defaults: the struct's own defaults, plus a two-stop
       palette so gradient/wave/spin render something out of the
       box and static shows a color. */
    EffectLayer l;
    l.primitive = primitive;
    l.palette   = MakePalette({ MakeSceneColor(255, 255, 255),
                                MakeSceneColor(40, 120, 255) });
    if(primitive == "screenfield")
    {
        l.source = "screen";
    }
    ws.effect.layers.push_back(l);
    return FinishEffectEdit(before, "add " + primitive + " layer");
}

std::optional<EditorEdit>
EditorController::RemoveLayer(size_t i)
{
    if(gesture.active)
    {
        return std::nullopt;
    }
    last_error.clear();
    const EffectDelta before = SnapEffect();
    if(!EnsureLayers() || i >= ws.effect.layers.size())
    {
        return UndoEffectOp(before);
    }
    ws.effect.layers.erase(ws.effect.layers.begin() + (ptrdiff_t)i);
    return FinishEffectEdit(before, "remove effect layer");
}

std::optional<EditorEdit>
EditorController::SetLayers(const std::vector<EffectLayer>& stack,
                            const std::string& new_preset,
                            const char* label)
{
    if(gesture.active || layer_gesture.active)
    {
        return std::nullopt;
    }
    last_error.clear();
    if(stack.size() > EFFECT_MAX_LAYERS)
    {
        last_error = "layer count exceeds cap "
                   + std::to_string(EFFECT_MAX_LAYERS);
        return std::nullopt;
    }
    const EffectDelta before = SnapEffect();
    ws.effect.layers = stack;
    ws.effect.preset = new_preset;
    return FinishEffectEdit(before,
            label != nullptr ? label
            : stack.empty() ? "reset effect to preset"
                            : "replace effect layers");
}

std::optional<EditorEdit>
EditorController::SelectPreset(const std::string& preset_id)
{
    if(gesture.active || layer_gesture.active)
    {
        return std::nullopt;
    }
    last_error.clear();
    const EffectDelta before = SnapEffect();
    /* A different look starts on seed 0 (the pick is a fresh roll);
       re-selecting the same look keeps the remix seed — matches
       the pre-undoable playPreset contract exactly. */
    if(preset_id != ws.effect.preset)
    {
        ws.effect.seed = 0;
    }
    ws.effect.preset = preset_id;
    ws.effect.layers.clear();
    return FinishEffectEdit(before, "select look");
}

/*---------------------------------------------------------*\
|| Per-layer ops                                            ||
\*---------------------------------------------------------*/
std::optional<EditorEdit>
EditorController::SetLayerEnabled(size_t i, bool on)
{
    if(gesture.active)
    {
        return std::nullopt;
    }
    last_error.clear();
    const EffectDelta before = SnapEffect();
    EffectLayer* l = LayerAt(i);
    if(l == nullptr)
    {
        return UndoEffectOp(before);
    }
    if(l->enabled == on)
    {
        return UndoEffectOp(before);
    }
    l->enabled = on;
    return FinishEffectEdit(before, on ? "enable layer"
                                       : "disable layer");
}

std::optional<EditorEdit>
EditorController::SetLayerBlend(size_t i, BlendMode mode)
{
    if(gesture.active)
    {
        return std::nullopt;
    }
    last_error.clear();
    const EffectDelta before = SnapEffect();
    EffectLayer* l = LayerAt(i);
    if(l == nullptr)
    {
        return UndoEffectOp(before);
    }
    if(l->blend == mode)
    {
        return UndoEffectOp(before);
    }
    l->blend = mode;
    return FinishEffectEdit(before, "set layer blend");
}

std::optional<EditorEdit>
EditorController::SetLayerOpacity(size_t i, float v)
{
    if(gesture.active)
    {
        return std::nullopt;
    }
    last_error.clear();
    if(!std::isfinite(v))
    {
        last_error = "opacity must be finite";
        return std::nullopt;
    }
    const EffectDelta before = SnapEffect();
    EffectLayer* l = LayerAt(i);
    if(l == nullptr)
    {
        return UndoEffectOp(before);
    }
    const float nv = std::max(0.0f, std::min(1.0f, v));
    if(nv == l->opacity)
    {
        return UndoEffectOp(before);
    }
    l->opacity = nv;
    return FinishEffectEdit(before, "set layer opacity");
}

std::optional<EditorEdit>
EditorController::SetLayerField(size_t i, LayerField f, double v)
{
    if(gesture.active)
    {
        return std::nullopt;
    }
    last_error.clear();
    if(!std::isfinite(v))
    {
        last_error = "value must be finite";
        return std::nullopt;
    }
    const EffectDelta before = SnapEffect();
    EffectLayer* l = LayerAt(i);
    if(l == nullptr)
    {
        return UndoEffectOp(before);
    }
    const char* name = "parameter";
    switch(f)
    {
    case LayerField::Speed:
        name = "speed";
        if((float)v == l->speed)
        {
            return UndoEffectOp(before);
        }
        l->speed = (float)v;
        if(!std::isfinite(l->speed))
        {
            last_error = "speed overflows float";
            return UndoEffectOp(before);
        }
        break;
    case LayerField::Scale:
        name = "scale";
        if(v <= 0.0)
        {
            last_error = "scale must be > 0";
            return UndoEffectOp(before);
        }
        if((float)v == l->scale)
        {
            return UndoEffectOp(before);
        }
        l->scale = (float)v;
        if(!std::isfinite(l->scale))
        {
            last_error = "scale overflows float";
            return UndoEffectOp(before);
        }
        break;
    case LayerField::Phase:
        name = "phase";
        if((float)v == l->phase)
        {
            return UndoEffectOp(before);
        }
        l->phase = (float)v;
        if(!std::isfinite(l->phase))
        {
            last_error = "phase overflows float";
            return UndoEffectOp(before);
        }
        break;
    case LayerField::Density:
        name = "density";
        v = std::max(0.0, v);
        if((float)v == l->density)
        {
            return UndoEffectOp(before);
        }
        l->density = (float)v;
        if(!std::isfinite(l->density))
        {
            last_error = "density overflows float";
            return UndoEffectOp(before);
        }
        break;
    case LayerField::Seed:
        name = "seed";
        v = std::max(0.0, std::min(4294967295.0, std::floor(v)));
        if((unsigned int)v == l->seed)
        {
            return UndoEffectOp(before);
        }
        l->seed = (unsigned int)v;
        break;
    }
    return FinishEffectEdit(before,
                            std::string("set layer ") + name);
}

std::optional<EditorEdit>
EditorController::SetLayerSpace(size_t i, CoordSpace space)
{
    if(gesture.active)
    {
        return std::nullopt;
    }
    last_error.clear();
    const EffectDelta before = SnapEffect();
    EffectLayer* l = LayerAt(i);
    if(l == nullptr)
    {
        return UndoEffectOp(before);
    }
    if(l->space == space)
    {
        return UndoEffectOp(before);
    }
    l->space = space;
    return FinishEffectEdit(before, "set layer space");
}

std::optional<EditorEdit>
EditorController::SetLayerOrigin(size_t i, const Vec3& v)
{
    if(gesture.active)
    {
        return std::nullopt;
    }
    last_error.clear();
    if(!std::isfinite(v.x) || !std::isfinite(v.y)
       || !std::isfinite(v.z))
    {
        last_error = "origin must be finite";
        return std::nullopt;
    }
    const EffectDelta before = SnapEffect();
    EffectLayer* l = LayerAt(i);
    if(l == nullptr)
    {
        return UndoEffectOp(before);
    }
    if(SameVec(l->origin, v))
    {
        return UndoEffectOp(before);
    }
    l->origin = v;
    return FinishEffectEdit(before, "set layer origin");
}

std::optional<EditorEdit>
EditorController::SetLayerDirection(size_t i, const Vec3& v)
{
    if(gesture.active)
    {
        return std::nullopt;
    }
    last_error.clear();
    if(!std::isfinite(v.x) || !std::isfinite(v.y)
       || !std::isfinite(v.z))
    {
        last_error = "direction must be finite";
        return std::nullopt;
    }
    const EffectDelta before = SnapEffect();
    EffectLayer* l = LayerAt(i);
    if(l == nullptr)
    {
        return UndoEffectOp(before);
    }
    if(SameVec(l->direction, v))
    {
        return UndoEffectOp(before);
    }
    l->direction = v;
    return FinishEffectEdit(before, "set layer direction");
}

std::optional<EditorEdit>
EditorController::SetLayerSource(size_t i, const std::string& src)
{
    if(gesture.active)
    {
        return std::nullopt;
    }
    last_error.clear();
    if(!src.empty() && src != "audio" && src != "key"
       && src != "screen")
    {
        last_error = "source must be \"\", \"audio\", \"key\""
                     " or \"screen\"";
        return std::nullopt;
    }
    const EffectDelta before = SnapEffect();
    EffectLayer* l = LayerAt(i);
    if(l == nullptr)
    {
        return UndoEffectOp(before);
    }
    if(l->source == src)
    {
        return UndoEffectOp(before);
    }
    l->source = src;
    return FinishEffectEdit(before, "set layer source");
}

std::optional<EditorEdit>
EditorController::SetLayerTargets(size_t i,
                                  const std::vector<std::string>& targets)
{
    if(gesture.active)
    {
        return std::nullopt;
    }
    last_error.clear();
    if(targets.size() > EFFECT_MAX_TARGETS)
    {
        last_error = "target count exceeds cap "
                   + std::to_string(EFFECT_MAX_TARGETS);
        return std::nullopt;
    }
    for(const std::string& t : targets)
    {
        if(t.empty())
        {
            last_error = "target ids must be non-empty";
            return std::nullopt;
        }
    }
    const EffectDelta before = SnapEffect();
    EffectLayer* l = LayerAt(i);
    if(l == nullptr)
    {
        return UndoEffectOp(before);
    }
    if(l->targets == targets)
    {
        return UndoEffectOp(before);
    }
    l->targets = targets;
    return FinishEffectEdit(before, "set layer targets");
}

/*---------------------------------------------------------*\
|| Palette-stop ops — positions strictly increasing in 0..1 ||
\*---------------------------------------------------------*/
bool EditorController::SortLayerStops(EffectLayer& l,
                                      size_t track_stop,
                                      size_t& new_index) const
{
    /* Stable sort keeps same-position order deterministic; the
       moved stop's new slot is reported for selection tracking. */
    struct Tracked { PaletteStop s; bool tracked; };
    std::vector<Tracked> ts;
    ts.reserve(l.palette.stops.size());
    for(size_t i = 0; i < l.palette.stops.size(); i++)
    {
        ts.push_back({ l.palette.stops[i], i == track_stop });
    }
    std::stable_sort(ts.begin(), ts.end(),
                     [](const Tracked& a, const Tracked& b) {
                         return a.s.pos < b.s.pos;
                     });
    new_index = 0;
    for(size_t i = 0; i < ts.size(); i++)
    {
        l.palette.stops[i] = ts[i].s;
        if(ts[i].tracked)
        {
            new_index = i;
        }
    }
    return true;
}

std::optional<EditorEdit>
EditorController::AddLayerStop(size_t i, float pos, const ColorF& color)
{
    if(gesture.active)
    {
        return std::nullopt;
    }
    last_error.clear();
    if(!std::isfinite(pos) || pos < 0.0f || pos > 1.0f)
    {
        last_error = "stop position must be in 0..1";
        return std::nullopt;
    }
    if(!std::isfinite(color.r) || !std::isfinite(color.g)
       || !std::isfinite(color.b) || !std::isfinite(color.a))
    {
        last_error = "stop color must be finite";
        return std::nullopt;
    }
    const EffectDelta before = SnapEffect();
    EffectLayer* l = LayerAt(i);
    if(l == nullptr)
    {
        return UndoEffectOp(before);
    }
    if(l->palette.stops.size() >= EFFECT_MAX_STOPS)
    {
        last_error = "stop count exceeds cap "
                   + std::to_string(EFFECT_MAX_STOPS);
        return UndoEffectOp(before);
    }
    for(const PaletteStop& s : l->palette.stops)
    {
        if(s.pos == pos)
        {
            last_error = "stop positions must be unique";
            return UndoEffectOp(before);
        }
    }
    PaletteStop st;
    st.pos   = pos;
    st.color = color;
    l->palette.stops.push_back(st);
    size_t nidx = 0;
    SortLayerStops(*l, l->palette.stops.size() - 1, nidx);
    return FinishEffectEdit(before, "add palette stop");
}

std::optional<EditorEdit>
EditorController::RemoveLayerStop(size_t i, size_t stop)
{
    if(gesture.active)
    {
        return std::nullopt;
    }
    last_error.clear();
    const EffectDelta before = SnapEffect();
    EffectLayer* l = LayerAt(i);
    if(l == nullptr)
    {
        return UndoEffectOp(before);
    }
    if(stop >= l->palette.stops.size())
    {
        return UndoEffectOp(before);
    }
    l->palette.stops.erase(l->palette.stops.begin()
                           + (ptrdiff_t)stop);
    return FinishEffectEdit(before, "remove palette stop");
}

std::optional<EditorEdit>
EditorController::MoveLayerStop(size_t i, size_t stop, float pos)
{
    if(gesture.active)
    {
        return std::nullopt;
    }
    last_error.clear();
    if(!std::isfinite(pos) || pos < 0.0f || pos > 1.0f)
    {
        last_error = "stop position must be in 0..1";
        return std::nullopt;
    }
    const EffectDelta before = SnapEffect();
    EffectLayer* l = LayerAt(i);
    if(l == nullptr)
    {
        return UndoEffectOp(before);
    }
    if(stop >= l->palette.stops.size())
    {
        return UndoEffectOp(before);
    }
    for(size_t k = 0; k < l->palette.stops.size(); k++)
    {
        if(k != stop && l->palette.stops[k].pos == pos)
        {
            last_error = "stop positions must be unique";
            return UndoEffectOp(before);
        }
    }
    if(l->palette.stops[stop].pos == pos)
    {
        return UndoEffectOp(before);
    }
    l->palette.stops[stop].pos = pos;
    size_t nidx = 0;
    SortLayerStops(*l, stop, nidx);
    return FinishEffectEdit(before, "move palette stop");
}

std::optional<EditorEdit>
EditorController::SetLayerStopColor(size_t i, size_t stop,
                                    const ColorF& color)
{
    if(gesture.active)
    {
        return std::nullopt;
    }
    last_error.clear();
    if(!std::isfinite(color.r) || !std::isfinite(color.g)
       || !std::isfinite(color.b) || !std::isfinite(color.a))
    {
        last_error = "stop color must be finite";
        return std::nullopt;
    }
    const EffectDelta before = SnapEffect();
    EffectLayer* l = LayerAt(i);
    if(l == nullptr)
    {
        return UndoEffectOp(before);
    }
    if(stop >= l->palette.stops.size())
    {
        return UndoEffectOp(before);
    }
    if(SameColorF(l->palette.stops[stop].color, color))
    {
        return UndoEffectOp(before);
    }
    l->palette.stops[stop].color = color;
    return FinishEffectEdit(before, "recolor palette stop");
}

/*---------------------------------------------------------*\
|| Path-point ops (comet waypoints)                         ||
\*---------------------------------------------------------*/
std::optional<EditorEdit>
EditorController::AddLayerPathPoint(size_t i, const Vec3& p)
{
    if(gesture.active)
    {
        return std::nullopt;
    }
    last_error.clear();
    if(!std::isfinite(p.x) || !std::isfinite(p.y)
       || !std::isfinite(p.z))
    {
        last_error = "path point must be finite";
        return std::nullopt;
    }
    const EffectDelta before = SnapEffect();
    EffectLayer* l = LayerAt(i);
    if(l == nullptr)
    {
        return UndoEffectOp(before);
    }
    if(l->path.size() >= EFFECT_MAX_PATH)
    {
        last_error = "path point count exceeds cap "
                   + std::to_string(EFFECT_MAX_PATH);
        return UndoEffectOp(before);
    }
    l->path.push_back(p);
    return FinishEffectEdit(before, "add path point");
}

std::optional<EditorEdit>
EditorController::SetLayerPathPoint(size_t i, size_t pt,
                                    const Vec3& p)
{
    if(gesture.active)
    {
        return std::nullopt;
    }
    last_error.clear();
    if(!std::isfinite(p.x) || !std::isfinite(p.y)
       || !std::isfinite(p.z))
    {
        last_error = "path point must be finite";
        return std::nullopt;
    }
    const EffectDelta before = SnapEffect();
    EffectLayer* l = LayerAt(i);
    if(l == nullptr)
    {
        return UndoEffectOp(before);
    }
    if(pt >= l->path.size())
    {
        return UndoEffectOp(before);
    }
    if(SameVec(l->path[pt], p))
    {
        return UndoEffectOp(before);
    }
    l->path[pt] = p;
    return FinishEffectEdit(before, "move path point");
}

std::optional<EditorEdit>
EditorController::RemoveLayerPathPoint(size_t i, size_t pt)
{
    if(gesture.active)
    {
        return std::nullopt;
    }
    last_error.clear();
    const EffectDelta before = SnapEffect();
    EffectLayer* l = LayerAt(i);
    if(l == nullptr)
    {
        return UndoEffectOp(before);
    }
    if(pt >= l->path.size())
    {
        return UndoEffectOp(before);
    }
    l->path.erase(l->path.begin() + (ptrdiff_t)pt);
    return FinishEffectEdit(before, "remove path point");
}

/*---------------------------------------------------------*\
|| Snapping                                                 ||
\*---------------------------------------------------------*/
Vec3 EditorController::SnapTranslate(const Vec3& v, float step_m)
{
    if(step_m <= 0.0f)
    {
        return v;
    }
    return {
        std::round(v.x / step_m) * step_m,
        std::round(v.y / step_m) * step_m,
        std::round(v.z / step_m) * step_m,
    };
}

float EditorController::SnapAngle(float deg, float step_deg)
{
    if(step_deg <= 0.0f)
    {
        return deg;
    }
    return std::round(deg / step_deg) * step_deg;
}

} /* namespace studio */
