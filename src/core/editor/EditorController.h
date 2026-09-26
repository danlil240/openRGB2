/*---------------------------------------------------------*\
|| EditorController.h                                        ||
||                                                           ||
||   Qt-free editor state: selection, transform gestures  ||
||   and every discrete workspace edit. All ops mutate    ||
||   the compact AUTHORING document (StudioDocument) —    ||
||   never the resolved runtime scene — and return an     ||
||   EditorEdit the caller pushes onto the undo stack.    ||
||   std::nullopt means the op was a no-op or rejected    ||
||   (missing/locked ids) and produces NO history entry.  ||
||                                                           ||
||   Rules this enforces:                                  ||
||   - Ops target ROOT instance ids only. A resolved      ||
||     object id maps to its owning instance through the  ||
||     first '/' segment (InstanceOf). Nested             ||
||     <parent>/<entity> child devices are type-owned     ||
||     internals — type authoring, not placement.         ||
||   - Locked instances (device_settings.locked) stay     ||
||     selectable for display but are excluded from the   ||
||     movable set: transform ops and grouping skip them. ||
||     Locks are protective beyond direct edits: Delete   ||
||     REFUSES when its descendant cascade would reach a  ||
||     locked instance; Ungroup refuses when the group    ||
||     holds a locked child; Rename refuses when the      ||
||     instance has a locked child (re-keying a child's   ||
||     parent is a reparent). LastError carries the       ||
||     refusal reason for the UI status line.             ||
||   - A gesture snapshots the movable instances once at  ||
||     BeginTransform; previews recompute from that       ||
||     snapshot; Commit yields ONE record; Cancel         ||
||     restores the snapshot with zero records. A second  ||
||     BeginTransform while a gesture is live cancels the ||
||     old one first. Commit/Cancel only touch ids still  ||
||     present in the document — a mid-gesture doc swap   ||
||     can never resurrect or corrupt instances.          ||
||   - Hierarchy (parent) and output ownership            ||
||     (mirror_of) stay separate; DuplicateMirrored only  ||
||     ever creates a Linked copy, never a second zone    ||
||     writer.                                            ||
||                                                           ||
||   SPDX-License-Identifier: GPL-2.0-or-later               ||
\*---------------------------------------------------------*/

#pragma once

#include "TransformCommands.h"

#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace studio
{
class PresetRegistry;
struct DevicePreset;

/* Plane a translate gesture is constrained to; Free keeps all
   axes. DeskXZ is the desk top (the default drag plane). */
enum class EditPlane
{
    Free,
    DeskXZ,     /* Y locked — the desk surface            */
    FrontXY,    /* Z locked                               */
    SideYZ,     /* X locked                               */
};

class EditorController
{
public:
    /* `ws` is the authoring document this controller edits; it
       must outlive the controller. */
    explicit EditorController(StudioDocument& ws) : ws(ws) {}

    /* Owning root instance of a resolved object id: the first
       '/' segment ("fan0/ring" -> "fan0", "fan0" -> "fan0"). */
    static std::string InstanceOf(const std::string& object_id);

    bool Exists(const std::string& id) const;
    bool IsLocked(const std::string& id) const;
    bool IsVisible(const std::string& id) const;

    /* Human-readable reason of the most recent refusal that set one
       (delete reaching a locked descendant, ungroup/rename touching a
       locked child, ...). Empty when the last op did not refuse with a
       message. */
    const std::string& LastError() const { return last_error; }

    /*------------------------------------------------*\
    || Selection — ordered instance ids, back() is the ||
    || primary (most recent). Locked instances may be  ||
    || selected for display; Movable() filters them    ||
    || out of edit ops.                                ||
    \*------------------------------------------------*/
    bool Select(const std::string& object_or_instance_id,
                bool additive = false);
    void SetSelection(const std::vector<std::string>& ids);
    void ClearSelection();
    const std::vector<std::string>& Selection() const { return selection; }
    std::string PrimarySelection() const;
    bool IsSelected(const std::string& id) const;

    /*------------------------------------------------*\
    || Transform gesture: BeginTransform snapshots the ||
    || movable selection; Preview* recompute absolute  ||
    || transforms FROM that snapshot (the delta param  ||
    || is the total gesture delta, not an increment);  ||
    || Commit returns the single edit record; Cancel   ||
    || restores the snapshot.                          ||
    \*------------------------------------------------*/
    bool BeginTransform();
    bool GestureActive() const { return gesture.active; }
    void PreviewTranslate(const Vec3& world_delta,
                          EditPlane plane = EditPlane::DeskXZ,
                          bool snap = false);
    /* Rotate the movable selection `degrees` around `axis`
       through the shared pivot (world space; desk default is
       the Y axis). */
    void PreviewRotate(const Vec3& axis, float degrees, bool snap = false);
    std::optional<EditorEdit> Commit();
    void Cancel();
    const std::set<std::string>& GestureIds() const { return gesture.movable; }

    /*------------------------------------------------*\
    || Discrete ops — each applies to ws and returns   ||
    || the record (nullopt = rejected/no-op).          ||
    \*------------------------------------------------*/
    std::optional<EditorEdit> SetPosition(const std::string& id, const Vec3& pos);
    std::optional<EditorEdit> SetRotation(const std::string& id, const Vec3& rot_deg);
    /* World-space numeric edit — converted into the instance's
       parent frame. */
    std::optional<EditorEdit> SetPositionWorld(const std::string& id, const Vec3& pos);
    std::optional<EditorEdit> Rename(const std::string& id, const std::string& new_id);
    std::optional<EditorEdit> SetVisible(const std::string& id, bool on);
    std::optional<EditorEdit> SetLocked(const std::string& id, bool on);
    /* Cascade-deletes instances and their descendants; strips
       dangling mirror_of references. Locked ids are skipped. */
    std::optional<EditorEdit> Delete(const std::vector<std::string>& ids);
    std::optional<EditorEdit> DeleteSelected();
    /* Pure read of Delete's kill set: the ids' root instances
       (existing + unlocked) plus every descendant that would ride
       along. The UI lists this before confirming (spec §4); the
       real Delete still performs its own locked-descendant
       refusal. */
    std::set<std::string> DeleteCascade(const std::vector<std::string>& ids) const;
    /* New "group"-type instance at the selection's world pivot;
       children keep world placement via parent-local
       conversion. When every member shares the same non-empty
       parent the group nests under it (so a later move of that
       parent carries the members); mixed or root-level parents
       root the group. */
    /* Align the movable selection on a world axis: mode 0 = min,
       1 = center (mean), 2 = max. Each member keeps its other axes;
       the new world position is converted into its parent frame.
       One record for the whole op. */
    std::optional<EditorEdit> Align(int axis /*0=X,1=Y,2=Z*/, int mode);
    /* Evenly space the movable selection along a world axis — the
       two extremes keep their positions, interior members land at
       equal intervals in sorted order. Needs >= 3 movable. */
    std::optional<EditorEdit> Distribute(int axis);

    std::optional<EditorEdit> Group(const std::string& base_id = "group");
    /* Dissolves selected type-"group" instances: children move to
       the group's parent with world placement preserved.
       Non-group selections are skipped. */
    std::optional<EditorEdit> Ungroup();
    /* Mirrored visual copy per selected instance: new unique id,
       same type + parent, small offset, device_settings.mirror_of
       wired to the output owner — never zones/bindings (a second
       writer for the same LEDs is never created). */
    std::optional<EditorEdit> DuplicateMirrored();

    /*------------------------------------------------*\
    || Device-library ops (task 4.2). AddInstance drops  ||
    || a new UNBOUND root instance of `type_id` at pos   ||
    || (the caller resolves/clamps placement; the type   ||
    || id is validated against the registry up in the    ||
    || bridge). The generated id is UniqueId(type_id)    ||
    || and becomes the selection. Retype repoints an     ||
    || instance's type reference — the Save-variant      ||
    || follow-up; same-type and unknown ids are no-ops.  ||
    \*------------------------------------------------*/
    std::optional<EditorEdit> AddInstance(const std::string& type_id,
                                          const Vec3& pos);
    std::optional<EditorEdit> Retype(const std::string& id,
                                     const std::string& new_type);

    /* Read-only builder for "Create preset from selection": fills
       `out` with one CHILD-DEVICE REFERENCE entity per id — the
       entity carries only `type` + the instance's world transform
       re-expressed relative to the shared origin (centroid of the
       instances' world positions on the desk plane, y = the lowest
       instance's y — documented choice: the new type's origin sits
       under the arrangement's center at desk height). No entities
       are copied; `out.zones` stays empty (child types carry their
       own). `ids` must be ROOT instance ids (devices map keys) —
       resolved paths like "case/case_fans" are refused outright
       (InstanceOf is NOT applied: silently coercing a nested path to
       its root would build a type the user didn't ask for). Refuses
       (false + LastError) mid-gesture, on empty input, on non-root/
       unknown ids, and on instances whose type doesn't resolve in
       `reg`. */
    bool BuildPresetFromInstances(const std::vector<std::string>& ids,
                                  const std::string& new_id,
                                  const std::string& name,
                                  const PresetRegistry& reg,
                                  DevicePreset& out);

    /* Bake instance `iid`'s painted base colors into a type variant:
       object_colors keys "<iid>/<entity>" copy onto
       entities[<entity>].appearance["body_color"] ("#RRGGBB") when
       the variant has that entity. Keys that don't name a variant
       entity are ignored — including deeper nested paths
       ("<iid>/<child>/<sub>", which belong to the child type, not
       this variant). Emitter-level paint (emitter_colors) stays
       workspace data and is NEVER baked into the type. Returns the
       number of entities painted. */
    unsigned int BakePaintedColors(const std::string& iid,
                                   DevicePreset& variant) const;

    /*------------------------------------------------*\
    || Zone binding (task 4.3) — writes the           ||
    || per-instance device_settings.zones.<zone_id>   ||
    || row plus the bindings entry it references.     ||
    || Preset files and hardware zone sizes are never ||
    || touched. BindZone ensures `binding` exists in  ||
    || ws.bindings — an existing entry whose identity ||
    || differs is a collision and refuses. UnbindZone ||
    || drops the row; SetZoneParams adjusts           ||
    || addr_base/verified on an existing bound row.   ||
    || One undo record each.                          ||
    \*------------------------------------------------*/
    std::optional<EditorEdit> BindZone(const std::string& iid,
                                       const std::string& zone_id,
                                       const DeviceBinding& binding,
                                       int addr_base, bool verified);
    std::optional<EditorEdit> UnbindZone(const std::string& iid,
                                         const std::string& zone_id);
    std::optional<EditorEdit> SetZoneParams(const std::string& iid,
                                            const std::string& zone_id,
                                            int addr_base, bool verified);

    /*------------------------------------------------*\
    || Effect-layer ops (task 5.2). Every op edits    ||
    || ws.effect.layers — the AUTHORED inline stack — ||
    || and returns the record (nullopt = rejected or  ||
    || no-op or gesture-preview). When the inline     ||
    || stack is empty, the FIRST mutating op          ||
    || materializes it by resolving `preset` through  ||
    || the installed resolver (SetLayerResolver) with ||
    || the persisted seed — the resolved stack lands  ||
    || INSIDE the same undo record, so one undo       ||
    || returns the workspace to registry resolution.  ||
    || `preset` is never rewritten by layer edits —   ||
    || it stays as provenance.                        ||
    ||                                                ||
    || Continuous gestures (slider scrub, stop drag,  ||
    || viewport origin/path drag) run through         ||
    || BeginLayerGesture / preview ops /              ||
    || CommitLayerGesture — previews mutate ws but    ||
    || return no record; Commit folds the whole       ||
    || gesture into ONE record; CancelLayerGesture    ||
    || restores the begin snapshot. The transform     ||
    || gesture above and the layer gesture are        ||
    || mutually exclusive — beginning one cancels     ||
    || the other.                                     ||
    \*------------------------------------------------*/
    typedef bool (*LayerResolver)(const std::string& preset_id,
                                  unsigned int seed,
                                  std::vector<EffectLayer>& out,
                                  void* ctx);
    void SetLayerResolver(LayerResolver resolver, void* ctx);

    /* Scalar fields a numeric edit can target. */
    enum class LayerField
    {
        Speed,
        Scale,
        Phase,
        Density,
        Seed,
    };

    bool BeginLayerGesture();
    bool LayerGestureActive() const { return layer_gesture.active; }
    std::optional<EditorEdit> CommitLayerGesture(
        const std::string& label = "edit layers");
    void CancelLayerGesture();

    /* Stack-level ops. `primitive` must be one of the JSON grammar
       names ("static" | "gradient" | "wave" | "pulse" | "comet" |
       "noise" | "spin" | "ripple" | "screenfield" | "level"). */
    std::optional<EditorEdit> MoveLayer(size_t from, size_t to);
    std::optional<EditorEdit> AddLayer(const std::string& primitive);
    std::optional<EditorEdit> RemoveLayer(size_t i);
    /* Whole-stack replacement — reset-to-preset (pass an empty
       stack + the preset id to restore registry semantics) and
       saved-look adoption both go through here. `new_preset`
       rewrites provenance; pass ws.effect.preset to keep it.
       `label` overrides the undo label (adoption says "save as
       look", not "reset to preset"). */
    std::optional<EditorEdit> SetLayers(
        const std::vector<EffectLayer>& stack,
        const std::string& new_preset,
        const char* label = nullptr);
    /* Look-shelf pick: adopts the named preset and clears the
       authored inline stack as ONE record — the whole point of the
       pick is the named look. A DIFFERENT id also takes a fresh
       seed (the old playPreset contract); re-picking the same id
       keeps the remix seed. Routing the pick through here makes
       stack destruction undoable — undo restores the previous
       stack, preset id AND seed together. */
    std::optional<EditorEdit> SelectPreset(const std::string& preset_id);

    /* Per-layer ops — `i` indexes ws.effect.layers. */
    std::optional<EditorEdit> SetLayerEnabled(size_t i, bool on);
    std::optional<EditorEdit> SetLayerBlend(size_t i, BlendMode mode);
    std::optional<EditorEdit> SetLayerOpacity(size_t i, float v);
    std::optional<EditorEdit> SetLayerField(size_t i, LayerField f,
                                            double v);
    std::optional<EditorEdit> SetLayerSpace(size_t i, CoordSpace space);
    std::optional<EditorEdit> SetLayerOrigin(size_t i, const Vec3& v);
    std::optional<EditorEdit> SetLayerDirection(size_t i, const Vec3& v);
    std::optional<EditorEdit> SetLayerSource(size_t i,
                                           const std::string& src);
    std::optional<EditorEdit> SetLayerTargets(
        size_t i, const std::vector<std::string>& targets);

    /* Palette-stop ops — positions stay strictly increasing in
       0..1; equal/adjacent-colliding positions are refused. */
    std::optional<EditorEdit> AddLayerStop(size_t i, float pos,
                                           const ColorF& color);
    std::optional<EditorEdit> RemoveLayerStop(size_t i, size_t stop);
    std::optional<EditorEdit> MoveLayerStop(size_t i, size_t stop,
                                            float pos);
    std::optional<EditorEdit> SetLayerStopColor(size_t i, size_t stop,
                                              const ColorF& color);

    /* Path-point ops (comet). */
    std::optional<EditorEdit> AddLayerPathPoint(size_t i,
                                                const Vec3& p);
    std::optional<EditorEdit> SetLayerPathPoint(size_t i, size_t pt,
                                                const Vec3& p);
    std::optional<EditorEdit> RemoveLayerPathPoint(size_t i,
                                                   size_t pt);

    /* Snapping — defaults come from ControlsPrefs (10 mm / 15 deg). */
    static Vec3  SnapTranslate(const Vec3& v, float step_m = 0.01f);
    static float SnapAngle(float deg, float step_deg = 15.0f);

private:
    struct Gesture
    {
        bool                          active = false;
        /* movable selection at begin (unlocked, topmost ancestors) */
        std::set<std::string>         movable;
        std::map<std::string, DeviceInstance> snapshot; /* id -> begin state */
        std::map<std::string, Mat4>   world;            /* id -> begin world */
        std::map<std::string, Mat4>   parent_world;     /* id -> parent's    */
        Vec3                          pivot;            /* shared rotate pivot */
    };

    /* Selection filtered to editable instances: existing, unlocked,
       and no selected ancestor (a selected ancestor already moves
       the subtree — applying the delta twice would double-move). */
    std::set<std::string> Movable() const;

    /* Instance id -> world matrix over the devices parent chain
       (all instance transforms are rigid T*R). */
    std::map<std::string, Mat4> InstanceWorlds() const;
    Mat4 ParentFrame(const std::map<std::string, Mat4>& worlds,
                     const std::string& id) const;

    /* Unique id: base, then base_2, base_3, ... */
    std::string UniqueId(const std::string& base) const;

    /* Shared re-key for rename/delete/ungroup: every settings or
       color key equal to `id` or under `id/` belongs to the
       instance subtree. */
    static bool PathBelongsTo(const std::string& key, const std::string& id);

    /* Effect-layer plumbing — SnapEffect copies the authored
       preset/seed/layers triple; EnsureLayers materializes the
       resolved preset stack into ws.effect.layers on first edit
       (resolver-injected so the Qt-free core never links the
       registry); FinishEffectEdit assembles the before/after
       record — inside a layer gesture it returns nullopt and
       leaves the preview state in ws. */
    struct LayerGesture
    {
        bool        active = false;
        EffectDelta begin;      /* snapshot at gesture start        */
        EffectDelta base;       /* post-materialize baseline        */
    };

    EffectDelta SnapEffect() const;
    bool        EnsureLayers();
    void        RestoreEffect(const EffectDelta& d);
    std::optional<EditorEdit> UndoEffectOp(const EffectDelta& before);
    std::optional<EditorEdit> FinishEffectEdit(
        const EffectDelta& before, const std::string& label);
    bool        EffectDeltaEqual(const EffectDelta& a,
                                 const EffectDelta& b) const;
    EffectLayer* LayerAt(size_t i);
    bool        SortLayerStops(EffectLayer& l, size_t track_stop,
                               size_t& new_index) const;

    StudioDocument&         ws;
    std::vector<std::string> selection;
    Gesture                  gesture;
    LayerGesture             layer_gesture;
    LayerResolver            layer_resolver = nullptr;
    void*                    layer_resolver_ctx = nullptr;
    std::string              last_error;
};

} /* namespace studio */
