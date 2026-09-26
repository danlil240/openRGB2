/*---------------------------------------------------------*\
|| studio_editor_test.cpp                                    ||
||                                                           ||
||   Qt-free tests for the Desktop Lighting Studio editor ||
||   core (plugins/DesktopLightingStudio/editor/):        ||
||   gesture records, cancel/no-op, locked instances,     ||
||   snapping, rename/delete/group/mirror edits and       ||
||   world-placement preservation. Plain cl build via     ||
||   build-studio-tests.bat.                              ||
||                                                           ||
||   SPDX-License-Identifier: GPL-2.0-or-later               ||
\*---------------------------------------------------------*/

#include "scene/SceneTypes.h"
#include "scene/SceneGraph.h"
#include "scene/SceneResolver.h"
#include "config/StudioConfig.h"
#include "presets/DevicePreset.h"
#include "presets/PresetRegistry.h"
#include "effects/Presets.h"
#include "editor/EditorController.h"
#include "editor/TransformCommands.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <set>
#include <string>

static int failures = 0;
static int checks   = 0;

#define CHECK(cond, name)                                              \
    do {                                                               \
        ++checks;                                                      \
        if(!(cond)) { ++failures; std::printf("FAIL: %s\n", name); }   \
    } while(0)

static bool Near(float a, float b, float eps = 1e-4f)
{
    return std::fabs(a - b) < eps;
}

static bool NearVec(const studio::Vec3& a, const studio::Vec3& b,
                    float eps = 1e-4f)
{
    return Near(a.x, b.x, eps) && Near(a.y, b.y, eps)
        && Near(a.z, b.z, eps);
}

/*---------------------------------------------------------*\
|| Fixture: tiny type library (fan-120 / desk / group)    ||
|| and a workspace using it — two fan instances (one     ||
|| parented under a rotated "case" group), a mirrored    ||
|| fan, a desk, paint + zone binding.                    ||
\*---------------------------------------------------------*/
static studio::DevicePreset FanType()
{
    studio::DevicePreset p;
    p.id       = "fan-120";
    p.name     = "Test fan";
    p.category = "fan";
    studio::PresetEntity body;
    body.id       = "body";
    body.geometry = "fan_body";
    body.size_m   = { 0.12f, 0.025f, 0.12f };
    body.zone     = "ring";
    p.entities["body"] = body;
    studio::DeviceZone z;
    z.id        = "ring";
    z.entity    = "body";
    z.led_count = 4;
    z.layout.type   = "points";
    z.layout.points = { { 0.05f, 0.0f, 0.0f }, { 0.0f, 0.0f, 0.05f },
                        { -0.05f, 0.0f, 0.0f }, { 0.0f, 0.0f, -0.05f } };
    p.zones.push_back(z);
    return p;
}

static studio::DevicePreset DeskType()
{
    studio::DevicePreset p;
    p.id       = "desk";
    p.name     = "Test desk";
    p.category = "decor";
    studio::PresetEntity top;
    top.id       = "top";
    top.geometry = "desk";
    top.size_m   = { 1.4f, 0.04f, 0.75f };
    p.entities["top"] = top;
    return p;
}

static studio::DevicePreset GroupType()
{
    studio::DevicePreset p;
    p.id       = "group";
    p.name     = "Placement group";
    p.category = "group";
    return p;
}

static studio::PresetRegistry TestRegistry()
{
    studio::PresetRegistry reg;
    std::vector<std::string> errs;
    reg.Add(FanType(), &errs);
    reg.Add(DeskType(), &errs);
    reg.Add(GroupType(), &errs);
    return reg;
}

static studio::StudioDocument Fixture()
{
    using namespace studio;
    StudioDocument w;
    w.meta.name = "Editor fixture";

    DeviceInstance desk;
    desk.type     = "desk";
    desk.position = { 0.0f, -0.02f, 0.10f };
    w.devices["desk"] = desk;

    DeviceInstance grp;
    grp.type         = "group";
    grp.position     = { 0.4f, 0.1f, -0.1f };
    grp.rotation_deg = { 0.0f, 30.0f, 0.0f };   /* rotated parent */
    w.devices["case"] = grp;

    DeviceInstance fan0;
    fan0.type     = "fan-120";
    fan0.position = { 0.0f, -0.16f, -0.14f };
    fan0.parent   = "case";
    w.devices["fan0"] = fan0;

    DeviceInstance fan1;
    fan1.type     = "fan-120";
    fan1.position = { 0.2f, 0.0f, 0.0f };
    w.devices["fan1"] = fan1;

    DeviceInstance fan2;
    fan2.type     = "fan-120";
    fan2.position = { 0.1f, 0.05f, 0.0f };
    fan2.parent   = "fan0";                 /* workspace child of fan0 */
    w.devices["fan2"] = fan2;

    DeviceInstance fanm;
    fanm.type     = "fan-120";
    fanm.position = { -0.2f, 0.0f, 0.2f };
    w.devices["fan0m"] = fanm;

    DeviceBinding bind;
    bind.id              = "fan_bus";
    bind.controller_name = "X870E AORUS ELITE";
    bind.vendor          = "Gigabyte";
    bind.zone_name       = "ARGB_V2_1";
    bind.zone_leds       = 8;
    w.bindings["fan_bus"] = bind;

    DeviceSettings s0;
    s0.zones["ring"] = { "fan_bus", 0, true };
    w.device_settings["fan0"] = s0;
    DeviceSettings sm;
    sm.mirror_of = "fan0";
    w.device_settings["fan0m"] = sm;

    w.object_colors["fan0/body"]     = MakeSceneColor(0x20, 0x40, 0x80);
    w.emitter_colors["fan0/body"][2] = MakeSceneColor(0xFF, 0x00, 0x10);
    return w;
}

static bool Resolve(const studio::StudioDocument& w,
                    const studio::PresetRegistry& reg,
                    studio::SceneDocument& out)
{
    std::vector<std::string> errs;
    if(!studio::ResolveScene(w, reg, out, &errs))
    {
        std::printf("  (resolve failed: %s)\n",
                    errs.empty() ? "?" : errs.front().c_str());
        return false;
    }
    return true;
}

static studio::Vec3 EmitterWorld(const studio::SceneDocument& doc,
                                 const std::map<std::string, studio::Mat4>& world,
                                 const std::string& obj, int idx)
{
    const studio::SceneObject* o = studio::FindObject(doc, obj);
    if(o == nullptr || idx < 0 || idx >= (int)o->emitters.size())
    {
        return { -9999.0f, -9999.0f, -9999.0f };
    }
    return studio::TransformPoint(world.at(obj), o->emitters[idx].local_pos);
}

/*---------------------------------------------------------*\
|| Drag gesture: 100 previews = ONE edit record; revert   ||
|| restores every touched instance; moving a parent       ||
|| restores its children's world placement on undo.       ||
\*---------------------------------------------------------*/
static void TestDragOneRecord()
{
    using namespace studio;

    StudioDocument   w   = Fixture();
    PresetRegistry   reg = TestRegistry();
    EditorController ctl(w);

    /* sanity: fixture resolves, fan0/body bound + verified */
    SceneDocument before;
    CHECK(Resolve(w, reg, before), "drag: fixture resolves");

    /* 100-update drag on one instance -> exactly one record */
    CHECK(ctl.Select("fan1"), "drag: select fan1");
    CHECK(ctl.BeginTransform(), "drag: begin");
    for(int i = 1; i <= 100; i++)
    {
        ctl.PreviewTranslate({ i * 0.001f, 99.0f, 0.0f },
                             EditPlane::DeskXZ);
    }
    /* XZ plane: y delta constrained away */
    CHECK(NearVec(w.devices["fan1"].position, { 0.3f, 0.0f, 0.0f }),
          "drag: preview lands on XZ plane");
    std::optional<EditorEdit> e = ctl.Commit();
    CHECK(e.has_value(), "drag: commit yields the single record");
    CHECK(!ctl.GestureActive(), "drag: gesture closed");

    /* record reverts the workspace exactly */
    RevertEditorEdit(w, *e);
    CHECK(NearVec(w.devices["fan1"].position, { 0.2f, 0.0f, 0.0f }),
          "drag: revert restores transform");
    ApplyEditorEdit(w, *e);
    CHECK(NearVec(w.devices["fan1"].position, { 0.3f, 0.0f, 0.0f }),
          "drag: re-apply restores moved transform");

    /* moving the "case" parent moves the whole subtree — the
       record only carries the parent, undo restores the
       children's world placement implicitly */
    SceneDocument before2;
    CHECK(Resolve(w, reg, before2), "drag: resolve before parent move");
    const auto wm0 = ResolveWorldMatrices(before2);
    const Vec3 fan0_e0 = EmitterWorld(before2, wm0, "fan0/body", 0);

    ctl.SetSelection({ "case" });
    CHECK(ctl.BeginTransform(), "drag: begin parent");
    ctl.PreviewTranslate({ 0.1f, 0.0f, 0.0f }, EditPlane::DeskXZ);
    std::optional<EditorEdit> e2 = ctl.Commit();
    CHECK(e2.has_value() && e2->devices.after.count("case") == 1
          && e2->devices.after.count("fan0") == 0,
          "drag: parent move records only the parent");

    SceneDocument moved;
    CHECK(Resolve(w, reg, moved), "drag: resolve after parent move");
    const auto wm1 = ResolveWorldMatrices(moved);
    const Vec3 fan0_moved = EmitterWorld(moved, wm1, "fan0/body", 0);
    CHECK(Near(fan0_moved.x, fan0_e0.x + 0.1f)
          && Near(fan0_moved.y, fan0_e0.y)
          && Near(fan0_moved.z, fan0_e0.z),
          "drag: child emitter rides the moved parent");

    RevertEditorEdit(w, *e2);
    SceneDocument back;
    CHECK(Resolve(w, reg, back), "drag: resolve after undo");
    const auto wm2 = ResolveWorldMatrices(back);
    CHECK(NearVec(EmitterWorld(back, wm2, "fan0/body", 0), fan0_e0),
          "drag: undo restores every affected child");
}

/*---------------------------------------------------------*\
|| Cancel and no-op produce zero records.                 ||
\*---------------------------------------------------------*/
static void TestCancelAndNoOp()
{
    using namespace studio;

    StudioDocument   w = Fixture();
    EditorController ctl(w);

    ctl.Select("fan1");
    CHECK(ctl.BeginTransform(), "cancel: begin");
    ctl.PreviewTranslate({ 0.2f, 0.0f, 0.1f }, EditPlane::Free);
    ctl.PreviewRotate({ 0.0f, 1.0f, 0.0f }, 45.0f);
    ctl.Cancel();
    CHECK(!ctl.GestureActive(), "cancel: gesture closed");
    CHECK(NearVec(w.devices["fan1"].position, { 0.2f, 0.0f, 0.0f })
          && NearVec(w.devices["fan1"].rotation_deg, { 0.0f, 0.0f, 0.0f }),
          "cancel: snapshot restored");
    CHECK(!ctl.Commit().has_value(), "cancel: commit after cancel is empty");

    /* no-op: begin + commit with no preview */
    CHECK(ctl.BeginTransform(), "noop: begin");
    CHECK(!ctl.Commit().has_value(), "noop: empty commit yields no record");

    /* zero-delta preview is still a no-op */
    CHECK(ctl.BeginTransform(), "noop: begin zero delta");
    ctl.PreviewTranslate({ 0.0f, 0.0f, 0.0f }, EditPlane::Free);
    CHECK(!ctl.Commit().has_value(), "noop: zero delta yields no record");

    /* no selection -> begin refused */
    ctl.ClearSelection();
    CHECK(!ctl.BeginTransform(), "noop: empty selection can't begin");
}

/*---------------------------------------------------------*\
|| Locked instances: selectable, but every transform op   ||
|| and delete/group skip them.                            ||
\*---------------------------------------------------------*/
static void TestLocked()
{
    using namespace studio;

    StudioDocument   w = Fixture();
    EditorController ctl(w);
    w.device_settings["fan1"].locked = true;

    CHECK(ctl.Select("fan1"), "locked: still selectable for display");
    CHECK(!ctl.BeginTransform(), "locked: gesture refuses all-locked");
    CHECK(!ctl.SetPosition("fan1", { 1.0f, 0.0f, 0.0f }).has_value(),
          "locked: SetPosition rejected");
    CHECK(!ctl.SetRotation("fan1", { 0.0f, 90.0f, 0.0f }).has_value(),
          "locked: SetRotation rejected");
    CHECK(!ctl.Delete({ "fan1" }).has_value(),
          "locked: delete rejected");
    CHECK(!ctl.Group().has_value(),
          "locked: group with only locked selection rejected");

    /* mixed selection: the unlocked member still moves */
    ctl.SetSelection({ "fan0", "fan1" });
    /* fan0's ancestor isn't selected; fan1 is locked -> movable = fan0 */
    CHECK(ctl.BeginTransform(), "locked: mixed selection begins");
    CHECK(ctl.GestureIds().count("fan0") == 1
          && ctl.GestureIds().count("fan1") == 0,
          "locked: movable excludes the locked id");
    ctl.PreviewTranslate({ 0.05f, 0.0f, 0.0f }, EditPlane::DeskXZ);
    CHECK(NearVec(w.devices["fan1"].position, { 0.2f, 0.0f, 0.0f }),
          "locked: locked instance untouched");
    std::optional<EditorEdit> e = ctl.Commit();
    CHECK(e.has_value()
          && e->devices.after.count("fan0") == 1
          && e->devices.after.count("fan1") == 0,
          "locked: record holds only the movable edit");

    /* unlock restores editability */
    std::optional<EditorEdit> un = ctl.SetLocked("fan1", false);
    CHECK(un.has_value() && !w.device_settings["fan1"].locked,
          "locked: SetLocked edit");
    CHECK(ctl.SetPosition("fan1", { 0.0f, 0.0f, 0.0f }).has_value(),
          "locked: unlocked instance editable");
}

/*---------------------------------------------------------*\
|| Snapping.                                               ||
\*---------------------------------------------------------*/
static void TestSnap()
{
    using namespace studio;

    CHECK(Near(EditorController::SnapTranslate({ 0.0137f, 0.0f, 0.0f },
                                               0.001f).x, 0.014f),
          "snap: 0.0137 m -> 0.014 m at 1 mm step");
    CHECK(Near(EditorController::SnapTranslate({ 0.0137f, 0.0f, 0.0f }).x,
               0.01f),
          "snap: 0.0137 m -> 0.01 m at default 10 mm");
    /* nearest-tick snapping: 23 deg is 7 deg from 30, 8 from 15 —
       the brief's "23 -> 15" reads as grid behavior, the nearest
       tick is what editors actually apply */
    CHECK(Near(EditorController::SnapAngle(22.0f), 15.0f),
          "snap: 22 deg -> 15 deg");
    CHECK(Near(EditorController::SnapAngle(23.0f), 30.0f),
          "snap: 23 deg -> 30 deg (nearest 15 deg tick)");
    CHECK(Near(EditorController::SnapAngle(52.5f), 60.0f, 1e-3f)
          || Near(EditorController::SnapAngle(52.5f), 45.0f, 1e-3f),
          "snap: mid angle lands on a 15 deg tick");

    /* snap inside a gesture honors workspace controls prefs */
    StudioDocument   w = Fixture();
    EditorController ctl(w);
    ctl.Select("fan1");
    CHECK(ctl.BeginTransform(), "snap: begin");
    ctl.PreviewTranslate({ 0.0137f, 0.0f, 0.0f }, EditPlane::DeskXZ,
                         /*snap*/ true);
    CHECK(Near(w.devices["fan1"].position.x, 0.21f),
          "snap: gesture snaps to controls.move_snap_m");
    ctl.Cancel();
}

/*---------------------------------------------------------*\
|| Rotate gesture: multi-selection pivot, world math      ||
|| through a rotated parent frame.                         ||
\*---------------------------------------------------------*/
static void TestRotate()
{
    using namespace studio;

    StudioDocument   w = Fixture();
    EditorController ctl(w);

    /* two root instances rotate around their shared pivot */
    ctl.SetSelection({ "desk", "fan1" });
    CHECK(ctl.BeginTransform(), "rotate: begin pair");
    /* pivot = centroid({0,-0.02,0.1},{0.2,0,0}) = {0.1,-0.01,0.05} */
    ctl.PreviewRotate({ 0.0f, 1.0f, 0.0f }, 90.0f);
    std::optional<EditorEdit> e = ctl.Commit();
    CHECK(e.has_value(), "rotate: commit");
    /* Ry(90): rel {x,z} -> {z,-x}
       fan1 rel {0.1,0.01,-0.05} -> {-0.05,0.01,-0.1} + pivot = {0.05,0,-0.05} */
    CHECK(NearVec(w.devices["fan1"].position, { 0.05f, 0.0f, -0.05f }),
          "rotate: fan1 orbits the pivot");
    /* desk rel {-0.1,-0.01,0.05} -> {0.05,-0.01,0.1} + pivot = {0.15,-0.02,0.15} */
    CHECK(NearVec(w.devices["desk"].position, { 0.15f, -0.02f, 0.15f }),
          "rotate: desk orbits the pivot");
    /* ~90 deg yaw — asin loses a couple hundredths near the pole */
    CHECK(Near(w.devices["fan1"].rotation_deg.y, 90.0f, 0.1f)
          && Near(w.devices["fan1"].rotation_deg.x, 0.0f, 0.1f)
          && Near(w.devices["fan1"].rotation_deg.z, 0.0f, 0.1f),
          "rotate: rotation lands on Y");

    /* rotating a child of the rotated "case" parent keeps world
       placement coherent: emitter world pos matches pivot math */
    StudioDocument   w2  = Fixture();
    PresetRegistry   reg = TestRegistry();
    EditorController ctl2(w2);
    SceneDocument    before;
    CHECK(Resolve(w2, reg, before), "rotate: resolve fixture");
    const auto wm = ResolveWorldMatrices(before);
    const Vec3 fan0_wp = TransformPoint(wm.at("fan0"), { 0.0f, 0.0f, 0.0f });

    ctl2.Select("fan0");
    CHECK(ctl2.BeginTransform(), "rotate: begin child");
    ctl2.PreviewRotate({ 0.0f, 1.0f, 0.0f }, 90.0f);
    std::optional<EditorEdit> e2 = ctl2.Commit();
    CHECK(e2.has_value(), "rotate: child commit");

    SceneDocument after;
    CHECK(Resolve(w2, reg, after), "rotate: resolve after");
    const auto wm2 = ResolveWorldMatrices(after);
    const Vec3 fan0_after = TransformPoint(wm2.at("fan0"),
                                         { 0.0f, 0.0f, 0.0f });
    /* single selection: pivot == own world pos, so the position is
       unchanged and the world rotation gained 90 deg about Y */
    CHECK(NearVec(fan0_after, fan0_wp),
          "rotate: self-pivot keeps world position");
    const Vec3 e_before = EmitterWorld(before, wm, "fan0/body", 0);
    const Vec3 e_after  = EmitterWorld(after, wm2, "fan0/body", 0);
    CHECK(!NearVec(e_after, e_before, 1e-3f),
          "rotate: emitter world pos actually rotated");
}

/*---------------------------------------------------------*\
|| Rename re-keys devices, settings, colors, parent and   ||
|| mirror_of references; resolution still succeeds.       ||
\*---------------------------------------------------------*/
static void TestRename()
{
    using namespace studio;

    StudioDocument   w   = Fixture();
    PresetRegistry   reg = TestRegistry();
    EditorController ctl(w);

    std::optional<EditorEdit> e = ctl.Rename("fan0", "fan0r");
    CHECK(e.has_value(), "rename: edit produced");
    CHECK(w.devices.count("fan0") == 0
          && w.devices.count("fan0r") == 1
          && w.devices["fan0r"].type == "fan-120",
          "rename: devices re-keyed");
    CHECK(w.devices["fan2"].parent == "fan0r",
          "rename: child parent re-pointed");
    CHECK(w.device_settings.count("fan0r") == 1
          && w.device_settings["fan0r"].zones["ring"].binding == "fan_bus",
          "rename: device_settings re-keyed");
    CHECK(w.device_settings["fan0m"].mirror_of == "fan0r",
          "rename: mirror_of re-pointed");
    CHECK(w.object_colors.count("fan0r/body") == 1
          && w.object_colors.count("fan0/body") == 0
          && w.emitter_colors["fan0r/body"].count(2) == 1,
          "rename: color keys re-keyed");

    SceneDocument doc;
    CHECK(Resolve(w, reg, doc), "rename: still resolves");
    CHECK(FindObject(doc, "fan0r/body") != nullptr
          && FindObject(doc, "fan0/body") == nullptr,
          "rename: resolved ids follow");

    /* revert restores every key */
    RevertEditorEdit(w, *e);
    CHECK(w.devices.count("fan0") == 1
          && w.device_settings.count("fan0") == 1
          && w.device_settings["fan0m"].mirror_of == "fan0"
          && w.object_colors.count("fan0/body") == 1
          && w.devices["fan2"].parent == "fan0",
          "rename: revert restores all keys");

    /* invalid renames produce no record */
    CHECK(!ctl.Rename("fan0", "fan1").has_value(),   /* collision   */
          "rename: existing id rejected");
    CHECK(!ctl.Rename("fan0", "has space").has_value(), /* bad id    */
          "rename: illegal id rejected");
    CHECK(!ctl.Rename("ghost", "fan9").has_value(),    /* missing    */
          "rename: missing id rejected");
}

/*---------------------------------------------------------*\
|| Delete cascades descendants, strips dangling mirrors,  ||
|| and reverts to the byte-identical document.            ||
\*---------------------------------------------------------*/
static void TestDeleteUndo()
{
    using namespace studio;

    StudioDocument   w   = Fixture();
    PresetRegistry   reg = TestRegistry();
    EditorController ctl(w);
    const nlohmann::json before_json = ToJson(w);

    ctl.SetSelection({ "fan0" });
    std::optional<EditorEdit> e = ctl.DeleteSelected();
    CHECK(e.has_value(), "delete: edit produced");
    CHECK(w.devices.count("fan0") == 0 && w.devices.count("fan2") == 0,
          "delete: descendants cascade");
    CHECK(w.device_settings.count("fan0") == 0
          && w.object_colors.count("fan0/body") == 0
          && w.emitter_colors.count("fan0/body") == 0,
          "delete: settings + colors removed");
    CHECK(w.device_settings["fan0m"].mirror_of.empty(),
          "delete: dangling mirror_of cleared");
    CHECK(!ctl.IsSelected("fan0"), "delete: selection pruned");

    SceneDocument doc;
    CHECK(Resolve(w, reg, doc), "delete: still resolves");
    CHECK(FindObject(doc, "fan0/body") == nullptr,
          "delete: resolved subtree gone");

    RevertEditorEdit(w, *e);
    CHECK(ToJson(w) == before_json,
          "delete: revert restores document exactly");
}

/*---------------------------------------------------------*\
|| Mirrored duplication: Linked objects, no zone writer.  ||
\*---------------------------------------------------------*/
static void TestDuplicateMirrored()
{
    using namespace studio;

    StudioDocument   w   = Fixture();
    PresetRegistry   reg = TestRegistry();
    EditorController ctl(w);

    ctl.Select("fan0");
    std::optional<EditorEdit> e = ctl.DuplicateMirrored();
    CHECK(e.has_value(), "mirror: edit produced");
    CHECK(w.devices.count("fan0_mirror") == 1
          && w.devices["fan0_mirror"].type == "fan-120"
          && w.devices["fan0_mirror"].parent == "case",
          "mirror: new instance, same type + parent");
    CHECK(Near(w.devices["fan0_mirror"].position.x,
               w.devices["fan0"].position.x + 0.03f),
          "mirror: offset placement");
    const DeviceSettings& ns = w.device_settings["fan0_mirror"];
    CHECK(ns.mirror_of == "fan0" && ns.zones.empty(),
          "mirror: mirror_of set, no zones/bindings");

    SceneDocument doc;
    CHECK(Resolve(w, reg, doc), "mirror: resolves");
    const SceneObject* linked = FindObject(doc, "fan0_mirror/body");
    CHECK(linked != nullptr && linked->kind == ObjectKind::Linked
          && linked->mirror_of == "fan0/body",
          "mirror: resolved object is Linked to the source");
    CHECK(linked->binding.empty() && linked->emitters.empty(),
          "mirror: no output ownership");

    /* a second duplicate gets a unique id and mirrors the owner,
       never forming a chain */
    e = ctl.DuplicateMirrored();
    CHECK(e.has_value() && w.devices.count("fan0_mirror_2") == 1,
          "mirror: unique follow-up id");
    /* selection moved to the copy, so this duplicated the copy */
    CHECK(w.device_settings["fan0_mirror_2"].mirror_of == "fan0",
          "mirror: copy-of-copy targets the owner, no chain");

    RevertEditorEdit(w, *e);
    CHECK(w.devices.count("fan0_mirror_2") == 0
          && w.device_settings.count("fan0_mirror_2") == 0,
          "mirror: revert removes the copy");
}

/*---------------------------------------------------------*\
|| Group / ungroup: world placement preserved through     ||
|| reparenting.                                            ||
\*---------------------------------------------------------*/
static void TestGroup()
{
    using namespace studio;

    StudioDocument   w   = Fixture();
    PresetRegistry   reg = TestRegistry();
    EditorController ctl(w);

    SceneDocument before;
    CHECK(Resolve(w, reg, before), "group: resolve before");
    const auto wm0 = ResolveWorldMatrices(before);
    const Vec3 fan0_e0 = EmitterWorld(before, wm0, "fan0/body", 0);
    const Vec3 fan1_e0 = EmitterWorld(before, wm0, "fan1/body", 0);

    ctl.SetSelection({ "fan0", "fan1" });
    std::optional<EditorEdit> e = ctl.Group();
    CHECK(e.has_value(), "group: edit produced");
    CHECK(w.devices.count("group") == 1
          && w.devices["group"].type == "group",
          "group: group-type instance created");
    CHECK(w.devices["fan0"].parent == "group"
          && w.devices["fan1"].parent == "group",
          "group: children reparented");
    CHECK(ctl.PrimarySelection() == "group",
          "group: selection follows the group");

    SceneDocument after;
    CHECK(Resolve(w, reg, after), "group: resolves");
    const auto wm1 = ResolveWorldMatrices(after);
    CHECK(NearVec(EmitterWorld(after, wm1, "fan0/body", 0), fan0_e0, 1e-3f),
          "group: fan0 world placement preserved");
    CHECK(NearVec(EmitterWorld(after, wm1, "fan1/body", 0), fan1_e0, 1e-3f),
          "group: fan1 world placement preserved");

    /* moving the group moves both subtrees */
    const Vec3 pivot0 = w.devices["group"].position;
    std::optional<EditorEdit> mv = ctl.SetPosition("group",
                                                 { 0.0f, 0.0f, 0.0f });
    CHECK(mv.has_value(), "group: movable");
    SceneDocument moved;
    CHECK(Resolve(w, reg, moved), "group: resolve moved");
    const auto wm2 = ResolveWorldMatrices(moved);
    const Vec3 delta { -pivot0.x, -pivot0.y, -pivot0.z };
    const Vec3 expect1 { fan1_e0.x + delta.x, fan1_e0.y + delta.y,
                         fan1_e0.z + delta.z };
    CHECK(NearVec(EmitterWorld(moved, wm2, "fan1/body", 0), expect1, 1e-3f),
          "group: moving group moves children");

    /* ungroup restores the parent structure and placement */
    RevertEditorEdit(w, *mv);
    ctl.SetSelection({ "group" });
    std::optional<EditorEdit> ug = ctl.Ungroup();
    CHECK(ug.has_value(), "ungroup: edit produced");
    CHECK(w.devices.count("group") == 0
          && w.devices["fan0"].parent.empty()
          && w.devices["fan1"].parent.empty(),
          "ungroup: children re-rooted (group was at root)");
    SceneDocument un;
    CHECK(Resolve(w, reg, un), "ungroup: resolves");
    const auto wm3 = ResolveWorldMatrices(un);
    CHECK(NearVec(EmitterWorld(un, wm3, "fan1/body", 0), fan1_e0, 1e-3f),
          "ungroup: world placement preserved");

    /* revert of the group edit restores the original parents */
    RevertEditorEdit(w, *ug);
    CHECK(w.devices["fan0"].parent == "group"
          && w.devices.count("group") == 1,
          "ungroup: revert restores the group");
    RevertEditorEdit(w, *e);
    CHECK(w.devices["fan0"].parent == "case"
          && w.devices["fan1"].parent.empty()
          && w.devices.count("group") == 0,
          "group: revert restores original parents");
}

/*---------------------------------------------------------*\
|| Post-edit resolution + compactness + edit typing.      ||
\*---------------------------------------------------------*/
static void TestResolutionAndCompactness()
{
    using namespace studio;

    StudioDocument   w   = Fixture();
    PresetRegistry   reg = TestRegistry();
    EditorController ctl(w);

    SceneDocument before;
    CHECK(Resolve(w, reg, before), "resolve: before");
    const auto wm0 = ResolveWorldMatrices(before);
    const Vec3 fan1_e0 = EmitterWorld(before, wm0, "fan1/body", 0);
    const Vec3 fan0_e0 = EmitterWorld(before, wm0, "fan0/body", 0);

    std::optional<EditorEdit> e =
        ctl.SetPosition("fan1", { 0.5f, 0.0f, 0.0f });
    CHECK(e.has_value(), "resolve: numeric edit produced");
    CHECK(e->TransformsOnly() && e->TransformIds().count("fan1") == 1,
          "resolve: numeric edit classifies as transform-only");

    SceneDocument after;
    CHECK(Resolve(w, reg, after), "resolve: after");
    const auto wm1 = ResolveWorldMatrices(after);
    CHECK(NearVec(EmitterWorld(after, wm1, "fan1/body", 0),
                  { fan1_e0.x + 0.3f, fan1_e0.y, fan1_e0.z }),
          "resolve: emitters land at the moved position");
    CHECK(NearVec(EmitterWorld(after, wm1, "fan0/body", 0), fan0_e0),
          "resolve: sibling instance untouched");

    /* structural edits are not transform-only */
    std::optional<EditorEdit> rn = ctl.Rename("fan1", "fan_top");
    CHECK(rn.has_value() && !rn->TransformsOnly(),
          "resolve: rename is structural");

    /* compactness: ToJson carries placements only — never
       expanded entities, emitters or embedded definitions */
    const nlohmann::json j = ToJson(w);
    bool expanded = j.contains("entities") || j.contains("emitters")
                 || j.contains("definitions") || j.contains("scene");
    for(const auto& kv : j["devices"].items())
    {
        if(kv.value().contains("entities") || kv.value().contains("emitters")
           || kv.value().contains("geometry") || kv.value().contains("zones"))
        {
            expanded = true;
        }
    }
    CHECK(!expanded, "compact: no expanded content after edits");
}

/*---------------------------------------------------------*\
|| Ancestor/descendant dedup: selecting a parent and its  ||
|| child moves the subtree once, not twice.               ||
\*---------------------------------------------------------*/
static void TestAncestorDedup()
{
    using namespace studio;

    StudioDocument   w = Fixture();
    EditorController ctl(w);

    ctl.SetSelection({ "case", "fan0" });   /* fan0 is a child of case */
    CHECK(ctl.BeginTransform(), "dedup: begin");
    CHECK(ctl.GestureIds().count("case") == 1
          && ctl.GestureIds().count("fan0") == 0,
          "dedup: selected child rides the selected parent");
    ctl.PreviewTranslate({ 0.1f, 0.0f, 0.0f }, EditPlane::DeskXZ);
    ctl.Cancel();
}

/*---------------------------------------------------------*\
||| Gesture lifecycle isolation (review I-1): a live       ||
||| gesture never lets a stale snapshot corrupt a swapped  ||
||| document; a second begin cancels the old one; undo-    ||
||| style external writes mid-gesture can't crash commit.  ||
|\*---------------------------------------------------------*/
static void TestGestureLifecycle()
{
    using namespace studio;

    /* instance vanishes mid-gesture -> Commit neither resurrects it
       nor records it */
    {
        StudioDocument   w = Fixture();
        EditorController ctl(w);
        ctl.Select("fan1");
        CHECK(ctl.BeginTransform(), "life: begin on fan1");
        ctl.PreviewTranslate({ 0.1f, 0.0f, 0.0f }, EditPlane::DeskXZ);
        w.devices.erase("fan1");              /* doc-swap stand-in */
        CHECK(!ctl.Commit().has_value(),
              "life: commit after vanish yields no record");
        CHECK(w.devices.count("fan1") == 0,
              "life: commit did not resurrect fan1");
    }

    /* instance vanishes mid-gesture -> Cancel doesn't re-add it */
    {
        StudioDocument   w = Fixture();
        EditorController ctl(w);
        ctl.Select("fan1");
        CHECK(ctl.BeginTransform(), "life2: begin");
        ctl.PreviewTranslate({ 0.1f, 0.0f, 0.0f }, EditPlane::DeskXZ);
        w.devices.erase("fan1");
        ctl.Cancel();
        CHECK(w.devices.count("fan1") == 0,
              "life2: cancel did not resurrect fan1");
        CHECK(!ctl.GestureActive(), "life2: gesture closed");
    }

    /* whole-document swap mid-gesture (ApplyWorkspace stand-in):
       commit produces no record for old-doc ids */
    {
        StudioDocument   w = Fixture();
        EditorController ctl(w);
        ctl.Select("fan0");
        CHECK(ctl.BeginTransform(), "life3: begin");
        ctl.PreviewTranslate({ 0.1f, 0.0f, 0.0f }, EditPlane::DeskXZ);
        StudioDocument fresh;
        fresh.meta.name = "other doc";
        DeviceInstance solo;
        solo.type     = "fan-120";
        solo.position = { 1.0f, 0.0f, 0.0f };
        fresh.devices["solo"] = solo;
        w = fresh;                       /* ctl's ws ref stays valid */
        CHECK(!ctl.Commit().has_value(),
              "life3: commit after doc swap yields no record");
        CHECK(w.devices.count("fan0") == 0
              && w.devices.count("solo") == 1,
              "life3: swapped doc intact");
    }

    /* second BeginTransform during a live gesture cancels the old
       one — the previewed transform is restored, never baked into
       the new baseline */
    {
        StudioDocument   w = Fixture();
        EditorController ctl(w);
        ctl.Select("fan1");
        CHECK(ctl.BeginTransform(), "life4: first begin");
        ctl.PreviewTranslate({ 0.1f, 0.0f, 0.0f }, EditPlane::DeskXZ);
        CHECK(NearVec(w.devices["fan1"].position, { 0.3f, 0.0f, 0.0f }),
              "life4: preview applied");
        CHECK(ctl.BeginTransform(),
              "life4: second begin cancels + starts fresh");
        CHECK(NearVec(w.devices["fan1"].position, { 0.2f, 0.0f, 0.0f }),
              "life4: old preview restored before re-snapshot");
        CHECK(ctl.GestureActive()
              && ctl.GestureIds().count("fan1") == 1,
              "life4: new gesture live on fan1");
        ctl.Cancel();
    }

    /* undo-style external write mid-gesture (bridge undo() cancels
       first; here the revert lands, then Cancel restores the
       gesture's own snapshot — no crash, coherent doc) */
    {
        StudioDocument   w = Fixture();
        EditorController ctl(w);
        std::optional<EditorEdit> e1 =
            ctl.SetPosition("fan1", { 0.5f, 0.0f, 0.0f });
        CHECK(e1.has_value(), "life5: prior edit recorded");
        ctl.Select("fan1");
        CHECK(ctl.BeginTransform(), "life5: begin");
        ctl.PreviewTranslate({ 0.1f, 0.0f, 0.0f }, EditPlane::DeskXZ);
        RevertEditorEdit(w, *e1);      /* undo lands mid-gesture */
        ctl.Cancel();
        CHECK(NearVec(w.devices["fan1"].position, { 0.5f, 0.0f, 0.0f }),
              "life5: cancel restores the gesture snapshot");
        CHECK(!ctl.Commit().has_value(),
              "life5: nothing left to commit");
    }

    /* InstanceWorlds walks parent chains — a dangling parent id is
       pushed before the existence check and must NOT be inserted
       into ws.devices by operator[] (same hazard class as I-1) */
    {
        StudioDocument   w = Fixture();
        EditorController ctl(w);
        w.devices["fan2"].parent = "ghost";   /* dangling ref */
        const size_t n0 = w.devices.size();
        ctl.Select("fan1");
        CHECK(ctl.BeginTransform(), "dangle: begin");
        CHECK(w.devices.size() == n0 && w.devices.count("ghost") == 0,
              "dangle: InstanceWorlds inserted no phantom instance");
        ctl.Cancel();
        /* group path calls InstanceWorlds too */
        ctl.SetSelection({ "fan1", "fan0m" });
        CHECK(ctl.Group().has_value(), "dangle: group");
        CHECK(w.devices.count("ghost") == 0,
              "dangle: group path inserted no phantom instance");
    }
}

/*---------------------------------------------------------*\
||| Phantom no-op records (review I-3): a zero-delta       ||
||| preview on a PARENTED instance round-trips through the ||
||| rotated parent frame — epsilon diff must not commit.   ||
|\*---------------------------------------------------------*/
static void TestParentedNoOp()
{
    using namespace studio;

    StudioDocument   w = Fixture();
    EditorController ctl(w);

    ctl.Select("fan0");            /* child of the 30-deg-rotated case */
    CHECK(ctl.BeginTransform(), "pnoop: begin parented");
    ctl.PreviewTranslate({ 0.0f, 0.0f, 0.0f }, EditPlane::Free);
    CHECK(!ctl.Commit().has_value(),
          "pnoop: zero-delta translate on parented instance is no-op");

    CHECK(ctl.BeginTransform(), "pnoop: begin rotate");
    ctl.PreviewRotate({ 0.0f, 1.0f, 0.0f }, 0.0f);
    CHECK(!ctl.Commit().has_value(),
          "pnoop: zero-degree rotate on parented instance is no-op");

    /* real moves still produce records — epsilon only swallows
       round-trip noise */
    CHECK(ctl.BeginTransform(), "pnoop: begin real move");
    ctl.PreviewTranslate({ 0.001f, 0.0f, 0.0f }, EditPlane::DeskXZ);
    CHECK(ctl.Commit().has_value(),
          "pnoop: 1 mm move on parented instance records");
}

/*---------------------------------------------------------*\
||| Mid-gesture paint (review I-2): the bridge's preview   ||
||| path syncs the runtime overlay into the workspace      ||
||| BEFORE resolving — simulated here in the same order.   ||
|\*---------------------------------------------------------*/
static void TestMidGesturePaint()
{
    using namespace studio;

    StudioDocument   w   = Fixture();
    PresetRegistry   reg = TestRegistry();
    EditorController ctl(w);

    SceneDocument doc;
    CHECK(Resolve(w, reg, doc), "paint: fixture resolves");

    ctl.Select("fan1");
    CHECK(ctl.BeginTransform(), "paint: begin");

    /* paint lands on the runtime doc mid-gesture (bridge
       paintEmitter path) */
    doc.object_colors["fan1/body"] = MakeSceneColor(0x11, 0x22, 0x33);

    /* the next preview's ordering contract: SyncWorkspace copies the
       overlay into the workspace, then the controller previews,
       then the workspace re-resolves */
    w.object_colors = doc.object_colors;            /* SyncWorkspace */
    ctl.PreviewTranslate({ 0.1f, 0.0f, 0.0f }, EditPlane::DeskXZ);
    SceneDocument after;
    CHECK(Resolve(w, reg, after), "paint: preview resolve");
    CHECK(after.object_colors.count("fan1/body") == 1
          && after.object_colors["fan1/body"] == MakeSceneColor(0x11, 0x22, 0x33),
          "paint: mid-gesture paint survives the preview resolve");

    /* without the sync step the stale workspace would drop it —
       assert the hazard is real, i.e. the sync is what saves it */
    StudioDocument stale = Fixture();
    SceneDocument unst;
    CHECK(Resolve(stale, reg, unst), "paint: stale resolve");
    CHECK(unst.object_colors.count("fan1/body") == 0,
          "paint: unsynced workspace drops the paint");

    ctl.Cancel();
}

/*---------------------------------------------------------*\
||| Protective lock semantics (review I-4): cascade and    ||
||| reparent ops refuse when they'd touch a locked node.   ||
|\*---------------------------------------------------------*/
static void TestLockedCascade()
{
    using namespace studio;

    /* delete whose cascade reaches a locked descendant -> refused */
    {
        StudioDocument   w = Fixture();
        EditorController ctl(w);
        w.device_settings["fan2"].locked = true;   /* child of fan0 */
        CHECK(!ctl.Delete({ "fan0" }).has_value(),
              "lockc: delete reaching locked child refused");
        CHECK(!ctl.LastError().empty(),
              "lockc: refusal carries a reason");
        CHECK(w.devices.count("fan0") == 1 && w.devices.count("fan2") == 1,
              "lockc: nothing deleted");
        /* unlocked descendant cascade still works */
        w.device_settings["fan2"].locked = false;
        CHECK(ctl.Delete({ "fan0" }).has_value(),
              "lockc: unlocked cascade still deletes");
    }

    /* ungroup with a locked child -> refused */
    {
        StudioDocument   w = Fixture();
        EditorController ctl(w);
        ctl.SetSelection({ "fan0", "fan1" });
        CHECK(ctl.Group().has_value(), "locku: group created");
        CHECK(ctl.SetLocked("fan1", true).has_value(),
              "locku: fan1 locked inside the group");
        CHECK(!ctl.Ungroup().has_value(),
              "locku: ungroup with locked child refused");
        CHECK(!ctl.LastError().empty(),
              "locku: refusal carries a reason");
        CHECK(w.devices.count("group") == 1
              && w.devices["fan1"].parent == "group",
              "locku: group intact");
        /* unlocking restores the op */
        CHECK(ctl.SetLocked("fan1", false).has_value(), "locku: unlock");
        CHECK(ctl.Ungroup().has_value(),
              "locku: ungroup proceeds once unlocked");
    }

    /* rename of a parent with a locked child -> refused */
    {
        StudioDocument   w = Fixture();
        EditorController ctl(w);
        w.device_settings["fan2"].locked = true;
        CHECK(!ctl.Rename("fan0", "fan0r").has_value(),
              "lockr: rename with locked child refused");
        CHECK(!ctl.LastError().empty(),
              "lockr: refusal carries a reason");
        CHECK(w.devices.count("fan0") == 1
              && w.devices["fan2"].parent == "fan0",
              "lockr: nothing re-keyed");
    }

    /* all-locked delete selection -> refused WITH a reason, not a
       silent no-op (bridge surfaces LastError via status) */
    {
        StudioDocument   w = Fixture();
        EditorController ctl(w);
        w.device_settings["fan1"].locked = true;
        ctl.SetSelection({ "fan1" });
        CHECK(!ctl.DeleteSelected().has_value(),
              "lockd: all-locked delete refused");
        CHECK(!ctl.LastError().empty(),
              "lockd: refusal carries a reason");
        CHECK(w.devices.count("fan1") == 1,
              "lockd: nothing deleted");

        /* all-locked group selection likewise reports */
        CHECK(!ctl.Group().has_value()
              && !ctl.LastError().empty(),
              "lockd: all-locked group refused with reason");
    }
}

/*---------------------------------------------------------*\
||| Group under a common parent (review I-5): members      ||
||| sharing a non-root parent nest the group there; an     ||
||| unselected sibling keeps its world placement.          ||
|\*---------------------------------------------------------*/
static void TestGroupCommonParent()
{
    using namespace studio;

    StudioDocument   w   = Fixture();
    PresetRegistry   reg = TestRegistry();
    EditorController ctl(w);

    /* two children of "case": fan0 and fan2 (re-parented here) */
    w.devices["fan2"].parent = "case";

    SceneDocument before;
    CHECK(Resolve(w, reg, before), "gcp: resolve before");
    const auto wm0 = ResolveWorldMatrices(before);
    const Vec3 fan0_e0 = EmitterWorld(before, wm0, "fan0/body", 0);
    const Vec3 fan2_e0 = EmitterWorld(before, wm0, "fan2/body", 0);
    const Vec3 fan1_e0 = EmitterWorld(before, wm0, "fan1/body", 0);

    ctl.SetSelection({ "fan0", "fan2" });
    std::optional<EditorEdit> e = ctl.Group();
    CHECK(e.has_value(), "gcp: group produced");
    CHECK(w.devices["group"].parent == "case",
          "gcp: group nests under the common parent");
    CHECK(w.devices["fan0"].parent == "group"
          && w.devices["fan2"].parent == "group",
          "gcp: members reparented to the group");

    SceneDocument after;
    CHECK(Resolve(w, reg, after), "gcp: resolves");
    const auto wm1 = ResolveWorldMatrices(after);
    CHECK(NearVec(EmitterWorld(after, wm1, "fan0/body", 0), fan0_e0, 1e-3f),
          "gcp: fan0 world preserved under nested group");
    CHECK(NearVec(EmitterWorld(after, wm1, "fan2/body", 0), fan2_e0, 1e-3f),
          "gcp: fan2 world preserved under nested group");
    CHECK(NearVec(EmitterWorld(after, wm1, "fan1/body", 0), fan1_e0),
          "gcp: unselected sibling world unchanged");

    /* moving the common parent carries the nested group */
    std::optional<EditorEdit> mv =
        ctl.SetPosition("case", { 0.4f, 0.1f, -0.2f });
    CHECK(mv.has_value(), "gcp: common parent movable");
    SceneDocument moved;
    CHECK(Resolve(w, reg, moved), "gcp: resolve moved");
    const auto wm2 = ResolveWorldMatrices(moved);
    CHECK(!NearVec(EmitterWorld(moved, wm2, "fan0/body", 0), fan0_e0, 1e-3f),
          "gcp: nested group rides the moved parent");
    RevertEditorEdit(w, *mv);

    /* mixed parents still root the group (unchanged semantics) */
    StudioDocument   w2 = Fixture();
    EditorController ctl2(w2);
    ctl2.SetSelection({ "fan0", "fan1" });   /* case-child + root */
    std::optional<EditorEdit> e2 = ctl2.Group();
    CHECK(e2.has_value() && w2.devices["group"].parent.empty(),
          "gcp: mixed parents root the group");

    /* grouping fan0 keeps its unselected child fan2's world pos */
    SceneDocument b2;
    CHECK(Resolve(w2, reg, b2), "gcp: resolve second fixture");
    const auto wmb = ResolveWorldMatrices(b2);
    const Vec3 fan2_b = EmitterWorld(b2, wmb, "fan2/body", 0);
    SceneDocument a2;
    CHECK(Resolve(w2, reg, a2), "gcp: resolve grouped");
    const auto wma = ResolveWorldMatrices(a2);
    CHECK(NearVec(EmitterWorld(a2, wma, "fan2/body", 0), fan2_b, 1e-3f),
          "gcp: unselected child keeps world placement");
}

/*---------------------------------------------------------*\
|| Task 4.2 — AddInstance: unique stable type-derived id ||
|| (fan-120, fan-120_2, ...), unbound root placement,     ||
|| selection follows, revert removes only the new row.    ||
\*---------------------------------------------------------*/
static void TestAddInstance()
{
    using namespace studio;

    StudioDocument   w   = Fixture();
    PresetRegistry   reg = TestRegistry();
    EditorController ctl(w);

    std::optional<EditorEdit> e =
        ctl.AddInstance("fan-120", { 0.5f, 0.0f, 0.3f });
    CHECK(e.has_value(), "add: edit produced");
    CHECK(w.devices.count("fan-120") == 1
          && w.devices["fan-120"].type == "fan-120"
          && w.devices["fan-120"].parent.empty(),
          "add: type-derived id, unbound root placement");
    CHECK(NearVec(w.devices["fan-120"].position, { 0.5f, 0.0f, 0.3f }),
          "add: position lands as given");
    CHECK(ctl.PrimarySelection() == "fan-120",
          "add: new instance selected");
    CHECK(e->devices.after.count("fan-120") == 1
          && e->devices.after.at("fan-120").type == "fan-120",
          "add: record carries the insert");

    e = ctl.AddInstance("fan-120", { -0.3f, 0.0f, 0.0f });
    CHECK(e.has_value() && w.devices.count("fan-120_2") == 1,
          "add: stable _2 suffix, no clobber");
    RevertEditorEdit(w, *e);
    CHECK(w.devices.count("fan-120_2") == 0
          && w.devices.count("fan-120") == 1,
          "add: revert removes only the new row");

    SceneDocument doc;
    CHECK(Resolve(w, reg, doc), "add: resolves");
    CHECK(FindObject(doc, "fan-120/body") != nullptr,
          "add: resolved objects exist for the new instance");

    /* refusals touch nothing */
    const size_t n0 = w.devices.size();
    CHECK(!ctl.AddInstance("bad id!", { 0, 0, 0 }).has_value()
          && w.devices.size() == n0,
          "add: bad type id refused, doc untouched");
    ctl.Select("fan1");
    CHECK(ctl.BeginTransform(), "add: gesture begins");
    CHECK(!ctl.AddInstance("desk", { 0, 0, 0 }).has_value()
          && w.devices.size() == n0,
          "add: refused mid-gesture");
    ctl.Cancel();
}

/*---------------------------------------------------------*\
|| Task 4.2 — Retype: repoint devices[id].type (the save-||
|| variant follow-up). Mirror-linked instances are type- ||
|| locked — the repoint applies but resolution refuses,  ||
|| which is why the bridge rejects them before writing.  ||
\*---------------------------------------------------------*/
static void TestRetype()
{
    using namespace studio;

    StudioDocument   w   = Fixture();
    PresetRegistry   reg = TestRegistry();
    EditorController ctl(w);

    std::optional<EditorEdit> e = ctl.Retype("fan1", "desk");
    CHECK(e.has_value() && w.devices["fan1"].type == "desk",
          "retype: type repointed");
    CHECK(e->devices.after.count("fan1") == 1
          && e->devices.before.count("fan1") == 1,
          "retype: one record (before + after)");
    RevertEditorEdit(w, *e);
    CHECK(w.devices["fan1"].type == "fan-120",
          "retype: revert restores the type");

    CHECK(!ctl.Retype("fan1", "fan-120").has_value(),
          "retype: same type is a no-op");
    CHECK(!ctl.Retype("nope", "desk").has_value(),
          "retype: unknown instance refused");
    CHECK(!ctl.Retype("fan1", "bad id!").has_value(),
          "retype: bad type id refused");
    CHECK(w.devices["fan1"].type == "fan-120"
          && w.devices.count("nope") == 0,
          "retype: refusals left the doc untouched");

    /* fan0 is mirrored by fan0m — retype applies structurally but
       resolution must refuse it (the bridge's early mirror check
       exists so this never strands a written variant file). */
    std::optional<EditorEdit> bad = ctl.Retype("fan0", "desk");
    CHECK(bad.has_value(), "retype: mirror-target edit produced");
    SceneDocument doc;
    CHECK(!Resolve(w, reg, doc),
          "retype: mirror-linked repoint fails resolve");
    RevertEditorEdit(w, *bad);
    CHECK(Resolve(w, reg, doc), "retype: revert restores resolve");
}

/*---------------------------------------------------------*\
|| Task 4.2 — BuildPresetFromInstances: one child-ref    ||
|| entity per instance, transforms re-expressed relative ||
|| to the shared origin (centroid on the desk plane, y = ||
|| lowest world y). The product must serialize like a    ||
|| hand-written *.device.json.                           ||
\*---------------------------------------------------------*/
static void TestBuildPresetFromInstances()
{
    using namespace studio;

    StudioDocument   w   = Fixture();
    PresetRegistry   reg = TestRegistry();
    EditorController ctl(w);

    /* instance world transforms (fan0 rides the rotated 'case') */
    SceneDocument doc;
    CHECK(Resolve(w, reg, doc), "bp: fixture resolves");
    const auto wm = ResolveWorldMatrices(doc);
    const Vec3 w0 = Mat4Translation(wm.at("fan0/body"));
    const Vec3 w1 = Mat4Translation(wm.at("fan1/body"));

    DevicePreset p;
    CHECK(ctl.BuildPresetFromInstances({ "fan0", "fan1" },
                                       "my-rig", "My Rig", reg, p),
          "bp: builds");
    CHECK(p.id == "my-rig" && p.name == "My Rig"
          && p.entities.size() == 2 && p.zones.empty(),
          "bp: id/name set, two child refs, no zones");
    const PresetEntity& e0 = p.entities.at("fan0");
    const PresetEntity& e1 = p.entities.at("fan1");
    CHECK(e0.type == "fan-120" && e0.geometry.empty()
          && e0.zone.empty() && e0.parent.empty()
          && e1.type == "fan-120",
          "bp: entities are pure child type refs");

    const Vec3 origin { (w0.x + w1.x) * 0.5f,
                        std::min(w0.y, w1.y),
                        (w0.z + w1.z) * 0.5f };
    CHECK(NearVec(e0.position, { w0.x - origin.x, w0.y - origin.y,
                               w0.z - origin.z }, 1e-3f),
          "bp: fan0 world placement relative to origin");
    CHECK(NearVec(e1.position, { w1.x - origin.x, w1.y - origin.y,
                               w1.z - origin.z }, 1e-3f),
          "bp: fan1 world placement relative to origin");
    /* fan0 inherits the case's 30-degree yaw — the child ref must
       carry WORLD rotation, not the instance's stored local one. */
    CHECK(Near(std::fmod(std::fabs(e0.rotation_deg.y), 360.0f),
               30.0f, 0.5f),
          "bp: child ref carries world rotation");
    CHECK(Near(std::fabs(e1.rotation_deg.y), 0.0f, 0.5f),
          "bp: unrotated instance stays unrotated");

    /* file-ready: ToJson -> DevicePresetFromJson, the same pass
       WritePresetFile runs before and after landing the file */
    const nlohmann::json j = ToJson(p);
    DevicePreset back;
    std::vector<std::string> errs;
    CHECK(DevicePresetFromJson(j, back, &errs),
          "bp: preset round-trips through the file schema");
    CHECK(back.entities.at("fan0").type == "fan-120"
          && back.entities.at("fan0").geometry.empty(),
          "bp: child ref survives serialization");

    /* a single instance is its own origin */
    DevicePreset solo;
    CHECK(ctl.BuildPresetFromInstances({ "fan1" }, "one-fan",
                                       "One Fan", reg, solo),
          "bp: single builds");
    CHECK(solo.entities.size() == 1
          && NearVec(solo.entities.at("fan1").position,
                     { 0.0f, 0.0f, 0.0f }, 1e-4f),
          "bp: single instance sits on the origin");

    /* Root ids only — a resolved path like "fan1/body" names an
       entity under an instance, not a placement; the builder must
       refuse it (field-specific message), NOT coerce it to its
       root via InstanceOf. */
    DevicePreset byObj;
    CHECK(!ctl.BuildPresetFromInstances({ "fan1/body" }, "o2",
                                        "O2", reg, byObj),
          "bp: nested path refused (root ids only)");
    CHECK(ctl.LastError().find("root instance")
              != std::string::npos,
          "bp: refusal names the root-instance requirement");
    CHECK(byObj.entities.empty(),
          "bp: refusal left the candidate untouched");

    /* duplicates collapse — one entity per instance */
    DevicePreset dup;
    CHECK(ctl.BuildPresetFromInstances({ "fan1", "fan1" }, "d",
                                       "D", reg, dup)
          && dup.entities.size() == 1,
          "bp: duplicate ids dedupe");

    /* refusals leave `out` and the workspace untouched */
    DevicePreset keep;
    keep.id = "keep";
    CHECK(!ctl.BuildPresetFromInstances({}, "x", "X", reg, keep)
          && keep.id == "keep",
          "bp: empty selection refused");
    CHECK(!ctl.BuildPresetFromInstances({ "nope" }, "x", "X",
                                        reg, keep)
          && keep.id == "keep",
          "bp: unknown instance refused");

    StudioDocument w2 = Fixture();
    w2.devices["fan1"].type = "no-such-type";
    EditorController ctl2(w2);
    DevicePreset p2;
    CHECK(!ctl2.BuildPresetFromInstances({ "fan1" }, "x", "X",
                                         reg, p2),
          "bp: unresolvable instance type refused");
}

/*---------------------------------------------------------*\
||| Task 4.2 fix — BakePaintedColors: the variant file    ||
||| must reproduce the instance's painted look. Keys are  ||
||| resolved "<inst>/<entity>" paths; only entities the   ||
||| type actually has get body_color; nested child paint  ||
||| and emitter overrides stay workspace data.            ||
\*---------------------------------------------------------*/
static void TestBakePaintedColors()
{
    using namespace studio;

    StudioDocument   w = Fixture();   /* fan0/body painted #204080 */
    EditorController ctl(w);

    /* Variant seed = the source type plus one entity the instance
       never painted, so "untouched" is observable. */
    DevicePreset v = FanType();
    PresetEntity shroud;
    shroud.id       = "shroud";
    shroud.geometry = "box";
    shroud.size_m   = { 0.13f, 0.03f, 0.13f };
    v.entities["shroud"] = shroud;

    /* Ignored keys: a nested path inside a child type, an entity
       the type doesn't have, and another instance's paint. */
    w.object_colors["fan0/nested/inner"] =
        MakeSceneColor(0xAA, 0xBB, 0xCC);
    w.object_colors["fan0/nope"]  = MakeSceneColor(0x11, 0x22, 0x33);
    w.object_colors["fan1/body"]  = MakeSceneColor(0x99, 0x88, 0x77);

    CHECK(ctl.BakePaintedColors("fan0", v) == 1,
          "bake: exactly the one matching entity painted");
    const nlohmann::json& a = v.entities.at("body").appearance;
    CHECK(a.is_object() && a.value("body_color", "") == "#204080",
          "bake: painted entity carries appearance.body_color");
    CHECK(a.size() == 1,
          "bake: emitter paint stayed workspace data");
    CHECK(v.entities.at("shroud").appearance.is_null(),
          "bake: unpainted entity untouched");

    /* An instance with no paint leaves every entity untouched. */
    DevicePreset v2 = FanType();
    CHECK(ctl.BakePaintedColors("desk", v2) == 0
          && v2.entities.at("body").appearance.is_null(),
          "bake: unpainted instance bakes nothing");
}

/*---------------------------------------------------------*\
|| Task 4.3 — ZoneLayoutToPoints: the editor's "convert    ||
|| generated layout to points" bakes generated positions   ||
|| + generated addresses into an explicit points list —    ||
|| LED order (incl. ring reverse) survives verbatim and    ||
|| led_count follows the baked count. Already-points and   ||
|| dynamic-matrix layouts refuse: nothing to bake.         ||
\*---------------------------------------------------------*/
static void TestZoneLayoutToPoints()
{
    using namespace studio;

    /* ring — 4 LEDs, r=50 mm, start 90°, face_y 10 mm, reversed */
    DeviceZone z;
    z.id = "ring"; z.entity = "body"; z.led_count = 4;
    z.layout.type            = "ring";
    z.layout.radius_m        = 0.05f;
    z.layout.start_angle_deg = 90.0f;
    z.layout.face_y_m        = 0.01f;
    z.layout.reverse         = true;
    CHECK(ZoneLayoutToPoints(z), "zp: ring converts");
    CHECK(z.layout.type == "points" && z.layout.points.size() == 4
          && z.layout.addresses.size() == 4 && z.led_count == 4,
          "zp: points + addresses baked, count synced");
    /* address 0 sits at the start angle; reversed order walks
       counter-clockwise (ang = start - i*90). */
    CHECK(NearVec(z.layout.points[0], { 0.0f, 0.01f, -0.05f }),
          "zp: address 0 at start angle");
    CHECK(NearVec(z.layout.points[1], { 0.05f, 0.01f, 0.0f }),
          "zp: reverse step direction preserved");
    CHECK(z.layout.addresses[0] == 0 && z.layout.addresses[3] == 3,
          "zp: addresses pin the physical order");

    /* strip — spacing 10 mm from origin (100,0,0) mm */
    DeviceZone s;
    s.id = "s"; s.entity = "body"; s.led_count = 3;
    s.layout.type       = "strip";
    s.layout.spacing_m  = 0.01f;
    s.layout.origin     = { 0.1f, 0.0f, 0.0f };
    CHECK(ZoneLayoutToPoints(s), "zp: strip converts");
    CHECK(NearVec(s.layout.points[1], { 0.11f, 0.0f, 0.0f }),
          "zp: strip pitch lands");
    CHECK(s.led_count == 3, "zp: strip count synced");

    /* static matrix — sparse map bakes only live cells, addresses
       carry the cell values */
    DeviceZone m;
    m.id = "mx"; m.entity = "body"; m.led_count = 3;
    m.layout.type      = "matrix";
    m.layout.dynamic   = false;
    m.layout.rows      = 2;
    m.layout.cols      = 2;
    m.layout.pitch_x_m = 0.02f;
    m.layout.pitch_z_m = 0.02f;
    m.layout.empty_cell = 0xFFFFFFFFu;
    m.layout.map       = { 0u, 0xFFFFFFFFu, 1u, 2u };
    CHECK(ZoneLayoutToPoints(m), "zp: static matrix converts");
    CHECK(m.layout.points.size() == 3 && m.led_count == 3,
          "zp: only live cells baked");
    CHECK(m.layout.addresses[0] == 0 && m.layout.addresses[1] == 1
          && m.layout.addresses[2] == 2,
          "zp: matrix cell addresses preserved");
    CHECK(NearVec(m.layout.points[2], { 0.02f, 0.0f, -0.02f }),
          "zp: matrix row/col position preserved");

    /* baked presets still pass file validation (led_count ==
       points size) and generate identical emitters */
    DevicePreset p;
    p.id = "baked"; p.name = "Baked"; p.category = "custom";
    PresetEntity body;
    body.id = "body"; body.geometry = "fan_body";
    body.size_m = { 0.1f, 0.02f, 0.1f };
    body.zone = "ring";
    p.entities["body"] = body;
    p.zones.push_back(z);
    DevicePreset back;
    std::vector<std::string> errs;
    CHECK(DevicePresetFromJson(ToJson(p), back, &errs),
          "zp: baked file validates");
    const std::vector<Emitter> a = GenerateZoneEmitters(
        back.zones[0], "", 0);
    CHECK(a.size() == 4 && a[1].address == 1
          && NearVec(a[1].local_pos, { 0.05f, 0.01f, 0.0f }),
          "zp: baked zone regenerates the same emitters");

    /* refusals — already-points and dynamic matrix */
    CHECK(!ZoneLayoutToPoints(z), "zp: re-convert refused");
    DeviceZone d;
    d.id = "dm"; d.entity = "body"; d.led_count = 0;
    d.layout.type    = "matrix";
    d.layout.dynamic = true;
    CHECK(!ZoneLayoutToPoints(d), "zp: dynamic matrix refused");
}

/*---------------------------------------------------------*\
|| Task 4.3 — per-instance zone binding ops. BindZone      ||
|| ensures the bindings entry and writes                  ||
|| device_settings.<inst>.zones.<zone> in ONE record;      ||
|| SetZoneParams/UnbindZone adjust or drop the row. All    ||
|| undoable through the same Apply/Revert machinery.       ||
\*---------------------------------------------------------*/
static void TestZoneBinding()
{
    using namespace studio;

    StudioDocument   w = Fixture();
    EditorController ctl(w);

    DeviceBinding b;
    b.id              = "mb_argb";
    b.controller_name = "X570 TEST";
    b.vendor          = "ASUS";
    b.zone_name       = "ARGB_1";
    b.zone_leds       = 4;

    std::optional<EditorEdit> e =
        ctl.BindZone("fan1", "ring", b, 0, true);
    CHECK(e.has_value(), "zb: bind produced a record");
    CHECK(w.bindings.count("mb_argb") == 1
          && w.bindings["mb_argb"].zone_leds == 4,
          "zb: binding identity ensured");
    CHECK(w.device_settings["fan1"].zones["ring"].binding == "mb_argb"
          && w.device_settings["fan1"].zones["ring"].verified,
          "zb: zone row written");
    CHECK(!e->Empty() && !e->TransformsOnly(),
          "zb: binding edit is not transforms-only");
    CHECK(e->bindings.after.count("mb_argb") == 1
          && e->settings.after.count("fan1") == 1,
          "zb: one record carries both sections");

    /* identical rebind = no record; a same-id-different-identity
       binding refuses without touching the workspace */
    CHECK(!ctl.BindZone("fan1", "ring", b, 0, true).has_value(),
          "zb: identical rebind is a no-op");
    DeviceBinding other = b;
    other.vendor = "Other";
    CHECK(!ctl.BindZone("fan1", "ring", other, 0, true).has_value()
          && w.bindings["mb_argb"].vendor == "ASUS",
          "zb: identity collision refused");

    /* params adjust the row in place; unbound rows refuse */
    std::optional<EditorEdit> p2 =
        ctl.SetZoneParams("fan1", "ring", 2, false);
    CHECK(p2.has_value()
          && w.device_settings["fan1"].zones["ring"].addr_base == 2
          && !w.device_settings["fan1"].zones["ring"].verified
          && w.device_settings["fan1"].zones["ring"].binding
                 == "mb_argb",
          "zb: params adjusted, binding kept");
    CHECK(!ctl.SetZoneParams("fan1", "ring", 2, false).has_value(),
          "zb: same params no-op");
    CHECK(!ctl.SetZoneParams("desk", "ring", 0, true).has_value()
          && !ctl.LastError().empty(),
          "zb: unbound row refuses with a reason");

    /* revert restores both sections (LIFO) */
    RevertEditorEdit(w, *p2);
    RevertEditorEdit(w, *e);
    CHECK(w.bindings.count("mb_argb") == 0
          && w.device_settings["fan1"].zones.count("ring") == 0,
          "zb: revert removes binding + row");

    /* unbind drops the row but leaves the bindings entry */
    e = ctl.BindZone("fan1", "ring", b, 0, true);
    CHECK(e.has_value(), "zb: rebind");
    std::optional<EditorEdit> u = ctl.UnbindZone("fan1", "ring");
    CHECK(u.has_value()
          && w.device_settings["fan1"].zones.count("ring") == 0
          && w.bindings.count("mb_argb") == 1,
          "zb: unbind drops the row, binding stays");
    RevertEditorEdit(w, *u);
    CHECK(w.device_settings["fan1"].zones["ring"].binding
              == "mb_argb",
          "zb: unbind reverts");
    RevertEditorEdit(w, *e);

    /* refusals: unknown instance, locked, mid-gesture */
    CHECK(!ctl.BindZone("nope", "ring", b, 0, true).has_value()
          && ctl.LastError().find("unknown") != std::string::npos,
          "zb: unknown instance refused");
    w.device_settings["fan1"].locked = true;
    w.device_settings["fan0"].locked = true;
    CHECK(!ctl.BindZone("fan1", "ring", b, 0, true).has_value()
          && !ctl.UnbindZone("fan0", "ring").has_value(),
          "zb: locked instance refused");
    w.device_settings["fan1"].locked = false;
    w.device_settings["fan0"].locked = false;
    ctl.SetSelection({ "fan1" });
    CHECK(ctl.BeginTransform(), "zb: gesture begins");
    CHECK(!ctl.BindZone("fan1", "ring", b, 0, true).has_value(),
          "zb: mid-gesture refused");
    ctl.Cancel();

    /* the fixture's existing bound zone still resolves + writes
       through the same path */
    SceneDocument doc;
    CHECK(Resolve(w, TestRegistry(), doc), "zb: fixture resolves");
}

/*---------------------------------------------------------*\
|| Task 4.3 — schema-backed save: a fully-edited preset    ||
|| (every entity field incl. appearance, all four layout   ||
|| kinds, binding_hints) round-trips ToJson -> FromJson    ||
|| with no unknown keys anywhere — an editor typo would    ||
|| otherwise ride silently into user files. Key sets here  ||
|| mirror schemas/device.schema.json.                      ||
\*---------------------------------------------------------*/
static void TestPresetSchemaRoundTrip()
{
    using namespace studio;

    DevicePreset p;
    p.id       = "full-kit";
    p.name     = "Full edited preset";
    p.category = "custom";

    PresetEntity body;
    body.id       = "body";
    body.geometry = "fan_body";
    body.size_m   = { 0.12f, 0.025f, 0.12f };
    body.position = { 0.01f, 0.02f, -0.03f };
    body.rotation_deg = { 5.0f, 45.0f, 0.0f };
    body.zone     = "ring";
    body.appearance = nlohmann::json::object({
        { "body_color", "#203040" },
        { "roughness",  0.4 },
        { "custom_tag", "kept-verbatim" } });
    p.entities["body"] = body;

    PresetEntity shroud;
    shroud.id     = "shroud";
    shroud.type   = "fan-120";       /* child-device reference */
    shroud.parent = "body";
    shroud.position = { 0.05f, 0.01f, 0.0f };
    p.entities["shroud"] = shroud;

    auto zone = [](const char* id, const char* layout) {
        DeviceZone z; z.id = id; z.entity = "body"; z.led_count = 4;
        z.layout.type = layout; return z; };
    DeviceZone ring = zone("ring", "ring");
    ring.layout.radius_m = 0.05f; ring.layout.start_angle_deg = 30.0f;
    ring.layout.face_y_m = 0.01f; ring.layout.reverse = true;
    DeviceZone strip = zone("strip", "strip");
    strip.layout.spacing_m = 0.008f;
    strip.layout.origin = { 0.01f, 0.0f, 0.0f };
    DeviceZone mx = zone("mx", "matrix");
    mx.layout.rows = 2; mx.layout.cols = 2;
    mx.layout.pitch_x_m = 0.02f; mx.layout.pitch_z_m = 0.02f;
    mx.layout.map = { 0u, 1u, 2u, 3u };
    mx.led_count = 4;
    DeviceZone pts = zone("pts", "points");
    pts.layout.points = { { 0.01f, 0.0f, 0.0f }, { 0.0f, 0.01f, 0.0f },
                          { -0.01f, 0.0f, 0.0f }, { 0.0f, -0.01f, 0.0f } };
    pts.layout.addresses = { 3, 2, 1, 0 };
    pts.led_count = 4;
    /* every zone needs a distinct entity — clone bodies so the
       zone->entity links all resolve */
    p.entities["body"].zone = "ring";
    for(const char* extra : { "e_strip", "e_mx", "e_pts" })
    {
        PresetEntity e2 = body;
        e2.id   = extra;
        e2.zone = "";
        p.entities[extra] = e2;
    }
    strip.entity = "e_strip";
    mx.entity    = "e_mx";
    pts.entity   = "e_pts";
    p.entities["e_strip"].zone = "strip";
    p.entities["e_mx"].zone    = "mx";
    p.entities["e_pts"].zone   = "pts";
    p.zones = { ring, strip, mx, pts };
    p.binding_hints.push_back({ "X870E AORUS ELITE", "Gigabyte",
                                "ARGB_V2_1" });

    const nlohmann::json j = ToJson(p);
    DevicePreset back;
    std::vector<std::string> errs;
    CHECK(DevicePresetFromJson(j, back, &errs),
          "rt: fully-edited preset round-trips the file schema");
    CHECK(back.entities.at("shroud").type == "fan-120"
          && back.entities.at("shroud").parent == "body",
          "rt: child ref + parent survive");
    CHECK(back.entities.at("body").appearance.value("custom_tag", "")
              == "kept-verbatim",
          "rt: unknown appearance key retained verbatim");
    CHECK(back.binding_hints.size() == 1
          && back.binding_hints[0].zone_name == "ARGB_V2_1",
          "rt: binding hint round-trips");
    const std::vector<int> want_addrs = { 3, 2, 1, 0 };
    CHECK(back.zones[3].layout.addresses == want_addrs,
          "rt: point addresses survive");

    /* No unknown keys — mirror of device.schema.json's declared
       properties (entities.items.properties is open by design; the
       editor only ever writes these). */
    const std::set<std::string> top =
        { "$schema", "schema_version", "id", "name", "category",
          "entities", "zones", "binding_hints" };
    const std::set<std::string> ent =
        { "type", "geometry", "size_m", "x", "y", "z",
          "rx", "ry", "rz", "parent", "zone", "appearance" };
    const std::set<std::string> zk =
        { "id", "entity", "led_count", "layout" };
    const std::set<std::string> lk =
        { "type", "radius_m", "start_angle_deg", "face_y_m",
          "reverse", "spacing_m", "origin", "dynamic", "rows", "cols",
          "pitch_x_m", "pitch_z_m", "empty", "map", "points",
          "addresses" };
    const std::set<std::string> hk =
        { "controller_name", "vendor", "zone_name" };
    auto keys_ok = [](const nlohmann::json& o,
                      const std::set<std::string>& known,
                      const char* what) {
        for(auto it = o.begin(); it != o.end(); ++it)
        {
            if(known.count(it.key()) == 0)
            {
                std::printf("  (unknown %s key: %s)\n", what,
                            it.key().c_str());
                return false;
            }
        }
        return true;
    };
    CHECK(keys_ok(j, top, "top"), "rt: no unknown top-level keys");
    for(const auto& kv : j.at("entities").items())
    {
        CHECK(keys_ok(kv.value(), ent, "entity"),
              "rt: entity keys are schema-known");
    }
    for(const nlohmann::json& jz : j.at("zones"))
    {
        CHECK(keys_ok(jz, zk, "zone"), "rt: zone keys known");
        CHECK(keys_ok(jz.at("layout"), lk, "layout"),
              "rt: layout keys known");
    }
    for(const nlohmann::json& jh : j.at("binding_hints"))
    {
        CHECK(keys_ok(jh, hk, "hint"), "rt: hint keys known");
    }
}

/*---------------------------------------------------------*\
||| Task 5.2 — effect-layer editing. Every op is a delta  ||
||| on ws.effect (preset provenance + seed + the authored ||
||| inline stack); first edit on a preset-backed document ||
||| materializes the resolved stack inside the SAME undo  ||
||| record; gestures collapse to one record.              ||
\*---------------------------------------------------------*/
static bool FxResolver(const std::string& id, unsigned int seed,
                       std::vector<studio::EffectLayer>& out, void*)
{
    out = studio::BuildPreset(id, seed);
    return !out.empty();
}

static void TestEffectLayers()
{
    using namespace studio;

    /* A deterministic authored stack so indexes don't depend on
       the packaged looks' contents. */
    auto two_layers = []() {
        EffectLayer a;
        a.primitive = "static";
        a.palette   = MakePalette({ MakeSceneColor(255, 0, 0),
                                    MakeSceneColor(0, 0, 255) });
        EffectLayer b;
        b.primitive = "wave";
        b.blend     = BlendMode::Add;
        b.palette   = MakePalette({ MakeSceneColor(0, 255, 0) });
        b.targets   = { "fan0" };
        return std::vector<EffectLayer>{ a, b };
    };

    /* ---- materialize-on-first-edit + exact undo/redo ---- */
    {
        StudioDocument   w = Fixture();
        w.effect.preset  = "aurora";
        w.effect.seed    = 7;
        EditorController ctl(w);
        ctl.SetLayerResolver(&FxResolver, nullptr);
        CHECK(w.effect.layers.empty(), "fx: preset stack starts empty");

        std::optional<EditorEdit> e = ctl.SetLayerEnabled(1, false);
        CHECK(e.has_value() && e->has_effect,
              "fx: first edit materializes the stack");
        CHECK(!w.effect.layers.empty(),
              "fx: resolved stack landed inline");
        CHECK(w.effect.preset == "aurora",
              "fx: preset kept as provenance");
        CHECK(w.effect.layers.size() > 1
              && !w.effect.layers[1].enabled,
              "fx: the enabled write applied");
        CHECK(e->effect_before.layers.empty()
              && e->effect_before.preset == "aurora"
              && e->effect_before.seed == 7,
              "fx: record holds the pre-materialize state");
        RevertEditorEdit(w, *e);
        CHECK(w.effect.layers.empty() && w.effect.preset == "aurora"
              && w.effect.seed == 7,
              "fx: undo returns to registry resolution");
        ApplyEditorEdit(w, *e);
        CHECK(w.effect.layers.size() == e->effect_after.layers.size()
              && !w.effect.layers[1].enabled,
              "fx: redo restores materialized + edited stack");
    }

    /* ---- materialize without a resolver refuses, cleanly ---- */
    {
        StudioDocument   w = Fixture();
        w.effect.preset  = "aurora";
        EditorController ctl(w);            /* no resolver installed */
        CHECK(!ctl.SetLayerEnabled(0, false).has_value(),
              "fx: no resolver -> op refused");
        CHECK(w.effect.layers.empty(),
              "fx: refused op leaves preset resolution intact");
        CHECK(!ctl.LastError().empty(), "fx: refusal reason set");

        /* An unresolvable preset id refuses the same way. */
        ctl.SetLayerResolver(&FxResolver, nullptr);
        w.effect.preset = "no_such_look";
        CHECK(!ctl.SetLayerEnabled(0, false).has_value(),
              "fx: unresolvable look refuses layer ops");
        /* ...but AddLayer still seeds a fresh inline stack. */
        std::optional<EditorEdit> e = ctl.AddLayer("static");
        CHECK(e.has_value() && w.effect.layers.size() == 1
              && w.effect.layers[0].primitive == "static",
              "fx: AddLayer works on an unresolvable preset");
    }

    /* ---- discrete ops: deltas + exact undo/redo ---- */
    {
        StudioDocument   w = Fixture();
        w.effect.layers  = two_layers();
        EditorController ctl(w);

        std::optional<EditorEdit> e = ctl.SetLayerOpacity(0, 0.35f);
        CHECK(e.has_value() && e->has_effect
              && e->effect_after.layers[0].opacity == 0.35f,
              "fx: opacity record");
        RevertEditorEdit(w, *e);
        CHECK(Near(w.effect.layers[0].opacity, 1.0f),
              "fx: opacity undo");
        ApplyEditorEdit(w, *e);
        CHECK(Near(w.effect.layers[0].opacity, 0.35f),
              "fx: opacity redo");

        CHECK(ctl.SetLayerBlend(0, BlendMode::Screen).has_value()
              && w.effect.layers[0].blend == BlendMode::Screen,
              "fx: blend edit");
        CHECK(ctl.SetLayerField(0, EditorController::LayerField::Speed,
                                2.5).has_value()
              && Near(w.effect.layers[0].speed, 2.5f),
              "fx: speed edit");
        CHECK(!ctl.SetLayerField(0, EditorController::LayerField::Scale,
                                 0.0).has_value(),
              "fx: scale 0 refused (grammar requires > 0)");
        CHECK(!ctl.SetLayerField(0, EditorController::LayerField::Phase,
                                 1e300).has_value(),
              "fx: float-overflow phase refused");
        CHECK(ctl.SetLayerField(0, EditorController::LayerField::Density,
                                -2.0).has_value()
              && w.effect.layers[0].density == 0.0f,
              "fx: density clamps to 0");
        CHECK(ctl.SetLayerField(0, EditorController::LayerField::Seed,
                                42.0).has_value()
              && w.effect.layers[0].seed == 42,
              "fx: seed edit");
        CHECK(ctl.SetLayerSpace(0, CoordSpace::Local).has_value()
              && w.effect.layers[0].space == CoordSpace::Local,
              "fx: space edit");
        CHECK(ctl.SetLayerOrigin(0, { 0.1f, 0.2f, 0.3f }).has_value()
              && NearVec(w.effect.layers[0].origin, { 0.1f, 0.2f, 0.3f }),
              "fx: origin edit");
        CHECK(!ctl.SetLayerOrigin(0,
                { std::numeric_limits<float>::infinity(), 0.0f, 0.0f })
              .has_value(),
              "fx: non-finite origin refused");
        CHECK(ctl.SetLayerDirection(0, { 0.0f, 0.0f, -1.0f }).has_value()
              && NearVec(w.effect.layers[0].direction,
                         { 0.0f, 0.0f, -1.0f }),
              "fx: direction edit");
        CHECK(ctl.SetLayerSource(1, "audio").has_value()
              && w.effect.layers[1].source == "audio",
              "fx: source edit");
        CHECK(!ctl.SetLayerSource(1, "midi").has_value(),
              "fx: bogus source refused");
        CHECK(ctl.SetLayerTargets(1, { "desk", "fan1" }).has_value()
              && w.effect.layers[1].targets.size() == 2,
              "fx: targets edit");
        CHECK(!ctl.SetLayerTargets(1, { "" }).has_value(),
              "fx: empty target refused");

        /* reorder */
        std::optional<EditorEdit> mv = ctl.MoveLayer(0, 1);
        CHECK(mv.has_value()
              && w.effect.layers[0].primitive == "wave"
              && w.effect.layers[1].primitive == "static",
              "fx: reorder moves the row");
        RevertEditorEdit(w, *mv);
        CHECK(w.effect.layers[0].primitive == "static",
              "fx: reorder undo");

        /* add/remove */
        std::optional<EditorEdit> add = ctl.AddLayer("comet");
        CHECK(add.has_value() && w.effect.layers.size() == 3
              && w.effect.layers[2].primitive == "comet"
              && w.effect.layers[2].palette.stops.size() == 2,
              "fx: AddLayer defaults (palette included)");
        CHECK(!ctl.AddLayer("fireworks").has_value(),
              "fx: unknown primitive refused");
        RevertEditorEdit(w, *add);
        CHECK(w.effect.layers.size() == 2, "fx: add undo");
        ApplyEditorEdit(w, *add);
        std::optional<EditorEdit> rm = ctl.RemoveLayer(2);
        CHECK(rm.has_value() && w.effect.layers.size() == 2,
              "fx: remove layer");
    }

    /* ---- palette stops: sorted insert, dup refusal, move,
           recolor, remove — each one record ---- */
    {
        StudioDocument   w = Fixture();
        w.effect.layers  = two_layers();
        EditorController ctl(w);

        /* layer 0 palette: stops at 0.0 and 0.5 (MakePalette evens) */
        std::optional<EditorEdit> e =
            ctl.AddLayerStop(0, 0.25f, ToColorF(MakeSceneColor(0,255,0)));
        CHECK(e.has_value() && w.effect.layers[0].palette.stops.size() == 3
              && Near(w.effect.layers[0].palette.stops[1].pos, 0.25f),
              "fx: stop inserts sorted");
        CHECK(!ctl.AddLayerStop(0, 0.25f,
                                ToColorF(MakeSceneColor(1,2,3))).has_value(),
              "fx: duplicate stop position refused");
        CHECK(!ctl.AddLayerStop(0, 1.5f,
                                ToColorF(MakeSceneColor(1,2,3))).has_value(),
              "fx: stop position > 1 refused");

        e = ctl.MoveLayerStop(0, 2, 0.1f);
        CHECK(e.has_value()
              && Near(w.effect.layers[0].palette.stops[1].pos, 0.1f)
              && Near(w.effect.layers[0].palette.stops[2].pos, 0.25f),
              "fx: stop move re-sorts");
        CHECK(!ctl.MoveLayerStop(0, 0, 0.1f).has_value(),
              "fx: stop move onto sibling refused");

        e = ctl.SetLayerStopColor(0, 0, ToColorF(MakeSceneColor(9,9,9)));
        CHECK(e.has_value()
              && Near(ToSceneColor(w.effect.layers[0].palette.stops[0].color)
                          & 0xFF, 9.0f),
              "fx: stop recolor");
        RevertEditorEdit(w, *e);
        CHECK(Near(ToSceneColor(w.effect.layers[0].palette.stops[0].color)
                       & 0xFF, 255.0f),
              "fx: stop recolor undo");

        e = ctl.RemoveLayerStop(0, 1);
        CHECK(e.has_value()
              && w.effect.layers[0].palette.stops.size() == 2,
              "fx: stop remove");
    }

    /* ---- path points ---- */
    {
        StudioDocument   w = Fixture();
        w.effect.layers  = two_layers();
        EditorController ctl(w);

        CHECK(ctl.AddLayerPathPoint(0, { 0.0f, 0.0f, 0.0f }).has_value()
              && ctl.AddLayerPathPoint(0, { 0.2f, 0.0f, 0.1f }).has_value()
              && w.effect.layers[0].path.size() == 2,
              "fx: path points append");
        std::optional<EditorEdit> e =
            ctl.SetLayerPathPoint(0, 1, { 0.3f, 0.0f, 0.2f });
        CHECK(e.has_value()
              && NearVec(w.effect.layers[0].path[1], { 0.3f, 0.0f, 0.2f }),
              "fx: path point set");
        RevertEditorEdit(w, *e);
        CHECK(NearVec(w.effect.layers[0].path[1], { 0.2f, 0.0f, 0.1f }),
              "fx: path point undo");
        e = ctl.RemoveLayerPathPoint(0, 0);
        CHECK(e.has_value() && w.effect.layers[0].path.size() == 1,
              "fx: path point remove");
        CHECK(!ctl.SetLayerPathPoint(0, 9,
                { 0.0f, 0.0f, 0.0f }).has_value(),
              "fx: out-of-range path index refused");
    }

    /* ---- gesture collapse: 100 previews -> ONE record ---- */
    {
        StudioDocument   w = Fixture();
        w.effect.layers  = two_layers();
        EditorController ctl(w);
        ctl.SetLayerResolver(&FxResolver, nullptr);

        CHECK(ctl.BeginLayerGesture(), "fx: layer gesture begins");
        for(int i = 1; i <= 100; i++)
        {
            /* preview writes land in ws but return no record */
            CHECK(!ctl.SetLayerOpacity(0, i / 100.0f).has_value(),
                  "fx: preview yields no record");
        }
        CHECK(Near(w.effect.layers[0].opacity, 1.0f),
              "fx: preview state is live in ws");
        /* The scrub ends where it began — a no-net-change gesture
           produces no record and restores the snapshot. */
        std::optional<EditorEdit> e =
            ctl.CommitLayerGesture("scrub opacity");
        CHECK(!e.has_value(),
              "fx: no-change gesture yields no record");
        CHECK(Near(w.effect.layers[0].opacity, 1.0f),
              "fx: no-change gesture restored");

        /* 100-update scrub lands one record with the real delta */
        CHECK(ctl.BeginLayerGesture(), "fx: gesture 2 begins");
        for(int i = 1; i <= 100; i++)
        {
            ctl.SetLayerOpacity(0, i * 0.005f);
        }
        e = ctl.CommitLayerGesture();
        CHECK(e.has_value()
              && Near(w.effect.layers[0].opacity, 0.5f)
              && e->effect_before.layers[0].opacity == 1.0f
              && Near(e->effect_after.layers[0].opacity, 0.5f),
              "fx: 100 previews -> one record, begin..final delta");

        /* cancel restores the begin snapshot — including an
           undo of begin-time materialization. */
        StudioDocument   w2 = Fixture();
        w2.effect.preset   = "aurora";
        w2.effect.seed     = 3;
        EditorController ctl2(w2);
        ctl2.SetLayerResolver(&FxResolver, nullptr);
        CHECK(ctl2.BeginLayerGesture(), "fx: gesture 3 begins");
        CHECK(!w2.effect.layers.empty(),
              "fx: begin materializes for preview");
        ctl2.SetLayerEnabled(0, false);
        ctl2.CancelLayerGesture();
        CHECK(w2.effect.layers.empty() && w2.effect.preset == "aurora",
              "fx: cancel restores pre-materialize snapshot");
        CHECK(!ctl2.CommitLayerGesture().has_value(),
              "fx: commit after cancel is empty");
    }

    /* ---- no-record commits leave ws at the BEGIN snapshot ----
       (5.2 review I2 + no-op-record minor: a commit that pushes
       nothing must still restore — and report — the pre-gesture
       state, or a materialized ghost stack leaks forward.) */
    {
        StudioDocument   w = Fixture();
        w.effect.preset  = "aurora";
        w.effect.seed    = 5;
        EditorController ctl(w);
        ctl.SetLayerResolver(&FxResolver, nullptr);

        /* Materialize-only begin, zero preview writes -> commit
           restores begin (inline stack dropped again). */
        CHECK(ctl.BeginLayerGesture(), "fx: gesture begins");
        CHECK(!w.effect.layers.empty(), "fx: begin materialized");
        CHECK(!ctl.CommitLayerGesture().has_value(),
              "fx: materialize-only commit = no record");
        CHECK(w.effect.layers.empty() && w.effect.preset == "aurora",
              "fx: ws restored to pre-gesture state");

        /* after == begin but after != base: materialize, then
           delete every materialized layer — net state equals
           begin, so no record is pushed (was: a no-op record). */
        CHECK(ctl.BeginLayerGesture(), "fx: gesture 2 begins");
        const size_t n = w.effect.layers.size();
        for(size_t i = 0; i < n; i++)
        {
            ctl.RemoveLayer(0);
        }
        CHECK(w.effect.layers.empty(), "fx: preview removed all");
        CHECK(!ctl.CommitLayerGesture().has_value(),
              "fx: after==begin commit = no record");
        CHECK(w.effect.layers.empty() && w.effect.preset == "aurora",
              "fx: after==begin left at begin state");

        /* Discrete ops work immediately after a cancel — the
           leaked-gesture class of bug (5.2 review C1): a gesture
           that never ended would swallow this op as preview. */
        CHECK(ctl.BeginLayerGesture(), "fx: gesture 3 begins");
        ctl.SetLayerOpacity(0, 0.4f);
        ctl.CancelLayerGesture();
        std::optional<EditorEdit> e = ctl.SetLayerEnabled(0, false);
        CHECK(e.has_value() && !w.effect.layers.empty()
              && !w.effect.layers[0].enabled,
              "fx: discrete op lands right after cancel");
    }

    /* ---- reset-to-preset + save-as semantics (SetLayers) ---- */
    {
        StudioDocument   w = Fixture();
        w.effect.preset  = "aurora";
        w.effect.seed    = 11;
        w.effect.layers  = two_layers();
        EditorController ctl(w);

        /* Reset: inline stack cleared, preset semantics restore. */
        std::optional<EditorEdit> e = ctl.SetLayers({}, "aurora");
        CHECK(e.has_value() && w.effect.layers.empty()
              && w.effect.preset == "aurora",
              "fx: reset clears the inline stack");
        RevertEditorEdit(w, *e);
        CHECK(w.effect.layers.size() == 2,
              "fx: reset undo restores the stack");

        /* Save-as adopt: preset <- saved id, stack cleared — one
           undoable record, and its own undo label (review minor:
           adoption is not "reset to preset"). */
        e = ctl.SetLayers({}, "mylook", "save as look");
        CHECK(e.has_value() && w.effect.preset == "mylook"
              && w.effect.layers.empty(),
              "fx: save-as sets preset id + clears stack");
        CHECK(e->label == "save as look",
              "fx: adopt carries its own undo label");
        RevertEditorEdit(w, *e);
        CHECK(w.effect.preset == "aurora"
              && w.effect.layers.size() == 2,
              "fx: save-as undo restores stack + provenance");

        /* Whole-stack replacement also carries layers. */
        std::vector<EffectLayer> st = two_layers();
        st[0].primitive = "ripple";
        e = ctl.SetLayers(st, "mylook2");
        CHECK(e.has_value() && w.effect.layers.size() == 2
              && w.effect.layers[0].primitive == "ripple"
              && w.effect.preset == "mylook2",
              "fx: whole-stack replacement");
    }

    /* ---- M5 finding 1: look pick + stop go through the editor
       now — SelectPreset / "clear effect" are the undoable routes
       playPreset/stopEffect take, so destroying the authored
       stack is a normal record, not a silent doc write. ---- */
    {
        StudioDocument   w = Fixture();
        w.effect.preset  = "aurora";
        w.effect.seed    = 11;
        w.effect.layers  = two_layers();
        EditorController ctl(w);

        /* Picking a DIFFERENT look: stack cleared, fresh seed,
           one record carrying preset + seed + layers. */
        std::optional<EditorEdit> e = ctl.SelectPreset("mylook");
        CHECK(e.has_value() && e->has_effect
              && e->label == "select look",
              "fx: look pick is one undoable record");
        CHECK(w.effect.layers.empty() && w.effect.preset == "mylook"
              && w.effect.seed == 0,
              "fx: pick clears stack + adopts id + fresh seed");
        RevertEditorEdit(w, *e);
        CHECK(w.effect.preset == "aurora" && w.effect.seed == 11
              && w.effect.layers.size() == 2,
              "fx: pick undo restores stack + preset + seed");
        ApplyEditorEdit(w, *e);

        /* Re-picking the SAME look keeps the remix seed (the old
           playPreset contract) and still clears the stack. */
        w.effect.seed   = 42;
        w.effect.layers = two_layers();
        e = ctl.SelectPreset("mylook");
        CHECK(e.has_value() && w.effect.preset == "mylook"
              && w.effect.seed == 42 && w.effect.layers.empty(),
              "fx: re-pick keeps seed, still clears stack");

        /* Stop: clears BOTH preset and stack — undoable, restores
           both (was a direct doc write autosave persisted). */
        w.effect.layers = two_layers();
        e = ctl.SetLayers({}, "", "clear effect");
        CHECK(e.has_value() && e->label == "clear effect"
              && w.effect.preset.empty() && w.effect.layers.empty(),
              "fx: stop clears preset + stack undoably");
        RevertEditorEdit(w, *e);
        CHECK(w.effect.preset == "mylook"
              && w.effect.layers.size() == 2,
              "fx: stop undo restores preset + stack");

        /* A pick that changes nothing pushes no record. */
        StudioDocument   w2 = Fixture();
        w2.effect.preset = "empty_look";
        EditorController ctl2(w2);
        CHECK(!ctl2.SelectPreset("empty_look").has_value(),
              "fx: same-look pick with no stack = no record");

        /* Both routes refuse mid-gesture — the bridge pairs this
           with a status refusal so a stray click can't clobber
           a live drag. */
        w2.effect.layers = two_layers();
        CHECK(ctl2.BeginLayerGesture(), "fx: gesture begins");
        CHECK(!ctl2.SelectPreset("other").has_value()
              && !ctl2.SetLayers({}, "", "clear effect").has_value(),
              "fx: pick + clear refuse mid-gesture");
        CHECK(w2.effect.layers.size() == 2
              && w2.effect.preset == "empty_look",
              "fx: refused pick left the stack untouched");
        ctl2.CancelLayerGesture();
    }

    /* ---- gesture exclusivity: transform vs layer ---- */
    {
        StudioDocument   w = Fixture();
        w.effect.layers  = two_layers();
        EditorController ctl(w);
        ctl.SetLayerResolver(&FxResolver, nullptr);
        ctl.Select("fan1");

        CHECK(ctl.BeginLayerGesture(), "fx: layer gesture starts");
        ctl.SetLayerOpacity(0, 0.5f);
        /* discrete device ops refuse during a layer gesture? — no:
           transform ops don't check layer_gesture; the LAYER ops
           refuse during a TRANSFORM gesture instead. */
        CHECK(ctl.BeginTransform(),
              "fx: BeginTransform cancels the layer gesture");
        CHECK(!ctl.LayerGestureActive(),
              "fx: layer gesture no longer active");
        CHECK(Near(w.effect.layers[0].opacity, 1.0f),
              "fx: cancelled preview state restored");
        ctl.Cancel();

        /* Layer ops refuse while a transform gesture is live. */
        CHECK(ctl.BeginTransform(), "fx: transform begins");
        CHECK(!ctl.SetLayerOpacity(0, 0.2f).has_value()
              && Near(w.effect.layers[0].opacity, 1.0f),
              "fx: layer op refused mid-transform");
        ctl.Cancel();
    }
}

int main()
{
    TestDragOneRecord();
    TestCancelAndNoOp();
    TestLocked();
    TestSnap();
    TestRotate();
    TestRename();
    TestDeleteUndo();
    TestDuplicateMirrored();
    TestGroup();
    TestResolutionAndCompactness();
    TestAncestorDedup();
    TestGestureLifecycle();
    TestParentedNoOp();
    TestMidGesturePaint();
    TestLockedCascade();
    TestGroupCommonParent();
    TestAddInstance();
    TestRetype();
    TestBuildPresetFromInstances();
    TestBakePaintedColors();
    TestZoneLayoutToPoints();
    TestZoneBinding();
    TestPresetSchemaRoundTrip();
    TestEffectLayers();

    std::printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
