/*---------------------------------------------------------*\
|| DefaultDesk.cpp                                           |
||                                                           |
||   SPDX-License-Identifier: GPL-2.0-or-later               |
\*---------------------------------------------------------*/

#include "DefaultDesk.h"
#include "EmitterLayout.h"

namespace studio
{

/*---------------------------------------------------------*\
|| v3 default content. The bundled                         |
|| presets/devices/*.device.json files are the             |
|| AUTHORITATIVE type library — SceneBridge loads them     |
|| from the qrc. This file only keeps the minimal C++      |
|| fallback set (desk + group) used when no packaged file  |
|| is readable, plus the compact workspace referencing     |
|| the packaged ids.                                       |
\*---------------------------------------------------------*/
namespace
{

const float DEF_FAN_FACE  = 0.016f;

/*---------------------------------------------------------*\
|| UNI FAN SL Wireless (SL V3) — 40 LEDs per fan, but NOT ||
|| a ring. The frame carries two identical LED strips on   ||
|| opposite edges; each strip is a 12-LED angular run along|
|| the front-face diffuser plus an 8-LED bar on the side   ||
|| face. Strip B is strip A rotated 180 deg about the fan  ||
|| axis, so address order walks the physical chain:        ||
||   [0..11]  front run A  (-x -> +x, top edge)            ||
||   [12..19] side bar A   (+x -> -x on the +z side face)  ||
||   [20..31] front run B  (+x -> -x, bottom edge)         ||
||   [32..39] side bar B   (-x -> +x on the -z side face)  ||
|| The packaged preset and the expanded default desk share ||
|| this table — TestDefaultWorkspaceParity compares the    ||
|| resolved world positions of both.                       ||
\*---------------------------------------------------------*/
const Vec3 SLW_LED_POINTS[40] = {
    /* strip A — front-face run */
    { -0.0540f, DEF_FAN_FACE,  0.0160f },
    { -0.0478f, DEF_FAN_FACE,  0.0284f },
    { -0.0416f, DEF_FAN_FACE,  0.0408f },
    { -0.0347f, DEF_FAN_FACE,  0.0520f },
    { -0.0208f, DEF_FAN_FACE,  0.0520f },
    { -0.0069f, DEF_FAN_FACE,  0.0520f },
    {  0.0069f, DEF_FAN_FACE,  0.0520f },
    {  0.0208f, DEF_FAN_FACE,  0.0520f },
    {  0.0347f, DEF_FAN_FACE,  0.0520f },
    {  0.0416f, DEF_FAN_FACE,  0.0408f },
    {  0.0478f, DEF_FAN_FACE,  0.0284f },
    {  0.0540f, DEF_FAN_FACE,  0.0160f },
    /* strip A — side-edge bar. The physical strip is on the side
       face, but mid-plane dots (|y| <= 0.014) are sealed inside the
       opaque body mesh and can't render — so it sits on the light
       face at the frame edge instead. */
    {  0.0490f, DEF_FAN_FACE,  0.0580f },
    {  0.0350f, DEF_FAN_FACE,  0.0580f },
    {  0.0210f, DEF_FAN_FACE,  0.0580f },
    {  0.0070f, DEF_FAN_FACE,  0.0580f },
    { -0.0070f, DEF_FAN_FACE,  0.0580f },
    { -0.0210f, DEF_FAN_FACE,  0.0580f },
    { -0.0350f, DEF_FAN_FACE,  0.0580f },
    { -0.0490f, DEF_FAN_FACE,  0.0580f },
    /* strip B — front-face run (strip A rotated 180 deg) */
    {  0.0540f, DEF_FAN_FACE, -0.0160f },
    {  0.0478f, DEF_FAN_FACE, -0.0284f },
    {  0.0416f, DEF_FAN_FACE, -0.0408f },
    {  0.0347f, DEF_FAN_FACE, -0.0520f },
    {  0.0208f, DEF_FAN_FACE, -0.0520f },
    {  0.0069f, DEF_FAN_FACE, -0.0520f },
    { -0.0069f, DEF_FAN_FACE, -0.0520f },
    { -0.0208f, DEF_FAN_FACE, -0.0520f },
    { -0.0347f, DEF_FAN_FACE, -0.0520f },
    { -0.0416f, DEF_FAN_FACE, -0.0408f },
    { -0.0478f, DEF_FAN_FACE, -0.0284f },
    { -0.0540f, DEF_FAN_FACE, -0.0160f },
    /* strip B — side-edge bar */
    { -0.0490f, DEF_FAN_FACE, -0.0580f },
    { -0.0350f, DEF_FAN_FACE, -0.0580f },
    { -0.0210f, DEF_FAN_FACE, -0.0580f },
    { -0.0070f, DEF_FAN_FACE, -0.0580f },
    {  0.0070f, DEF_FAN_FACE, -0.0580f },
    {  0.0210f, DEF_FAN_FACE, -0.0580f },
    {  0.0350f, DEF_FAN_FACE, -0.0580f },
    {  0.0490f, DEF_FAN_FACE, -0.0580f },
};

std::vector<Emitter> SlwEmitters(const std::string& group)
{
    std::vector<Emitter> out;
    out.reserve(40);
    for(int i = 0; i < 40; i++)
    {
        Emitter e;
        e.local_pos = SLW_LED_POINTS[i];
        e.group     = group;
        e.address   = i;
        out.push_back(e);
    }
    return out;
}

PresetEntity Part(const std::string& id, const std::string& geometry,
                  const Vec3& size, const Vec3& pos, const Vec3& rot,
                  const std::string& zone = std::string())
{
    PresetEntity e;
    e.id           = id;
    e.geometry     = geometry;
    e.size_m       = size;
    e.position     = pos;
    e.rotation_deg = rot;
    e.zone         = zone;
    return e;
}

DevicePreset Preset(const std::string& id, const std::string& name,
                    const std::string& category)
{
    DevicePreset p;
    p.id       = id;
    p.name     = name;
    p.category = category;
    return p;
}

DeviceInstance Inst(const std::string& type, const Vec3& pos,
                    const Vec3& rot, const std::string& parent = std::string())
{
    DeviceInstance d;
    d.type         = type;
    d.position     = pos;
    d.rotation_deg = rot;
    d.parent       = parent;
    return d;
}

ZoneSetting ZoneBound(const std::string& binding, int addr_base,
                      bool verified)
{
    ZoneSetting z;
    z.binding   = binding;
    z.addr_base = addr_base;
    z.verified  = verified;
    return z;
}

} /* anonymous namespace */

std::vector<DevicePreset> DefaultDevicePresets()
{
    /* Minimal recoverable fallback — used ONLY when no packaged
       presets/devices/*.device.json is readable (missing qrc or
       every file failing validation). The full type library lives
       in the JSON files; duplicating it in C++ just made the two
       drift. Keep exactly what BuildFallbackWorkspace() needs
       (desk) plus "group", the structural type v2-migrated
       workspaces may reference. */
    std::vector<DevicePreset> out;
    {
        DevicePreset p = Preset("desk", "Desk surface", "furniture");
        p.entities["body"] = Part("body", "desk",
            { 1.4f, 0.04f, 0.75f }, { 0, 0, 0 }, { 0, 0, 0 });
        out.push_back(p);
    }
    {
        /* Placement-only type for v2 group nodes produced by
           migration — no entities, no zones. */
        DevicePreset p = Preset("group", "Placement group", "group");
        out.push_back(p);
    }
    return out;
}

StudioDocument BuildFallbackWorkspace()
{
    /* The desk a user gets when every packaged type file is
       unreadable — resolvable on the minimal fallback set alone. */
    StudioDocument w;
    w.meta.name  = "Recovery desk";
    w.brightness = 1.0f;
    w.devices["desk"] = Inst("desk", { 0.0f, -0.02f, 0.10f }, { 0, 0, 0 });
    return w;
}

StudioDocument BuildDefaultWorkspace()
{
    StudioDocument w;
    w.meta.name   = "Default desk";
    w.brightness  = 1.0f;

    /* Same persistent identities as the expanded builder. */
    for(const DeviceBinding& b : BuildDefaultDesk().bindings)
    {
        w.bindings[b.id] = b;
    }

    /*-----------------------------------------------------*\
    | Root placements (world coords, meters/degrees).       |
    \*-----------------------------------------------------*/
    w.devices["desk"]     = Inst("desk",         { 0.0f, -0.02f, 0.10f }, { 0, 0, 0 });
    w.devices["monitor"]  = Inst("monitor",      { 0.0f, 0.32f, -0.22f }, { 0, 0, 0 });
    w.devices["keyboard"] = Inst("keyboard-104", { -0.06f, 0.02f, 0.26f }, { 0, 0, 0 });
    w.devices["mouse"]    = Inst("mouse-3zone",  { 0.26f, 0.02f, 0.27f }, { 0, 0, 0 });
    w.devices["case"]     = Inst("pc-case",      { 0.47f, 0.235f, -0.10f }, { 0, 0, 0 });

    /*-----------------------------------------------------*\
    | Inside the case — same case-local coords as the       |
    | expanded desk; parent "case" supplies the world       |
    | anchor.                                               |
    \*-----------------------------------------------------*/
    const std::string CASE = "case";

    /* ARGB_V2_1 — four case fans on one mirrored 8-LED ring. */
    w.devices["case_fans"]     = Inst("fan-120", { 0.0f, -0.16f, -0.14f }, { 0, 0, 0 }, CASE);
    w.devices["case_fan_b1"]   = Inst("fan-120", { 0.0f, -0.16f, -0.01f }, { 0, 0, 0 }, CASE);
    w.devices["case_fan_b2"]   = Inst("fan-120", { 0.0f, -0.205f, 0.14f }, { 0, 0, 0 }, CASE);
    w.devices["case_fan_rear"] = Inst("fan-120", { 0.0f, 0.065f, -0.20f }, { 90, 0, 0 }, CASE);
    w.device_settings["case_fans"].zones["ring"] = ZoneBound("argb_v2_1", 0, true);
    w.device_settings["case_fan_b1"].mirror_of   = "case_fans";
    w.device_settings["case_fan_b2"].mirror_of   = "case_fans";
    w.device_settings["case_fan_rear"].mirror_of = "case_fans";

    /* ARGB_V2_2 — 3 mirrored radiator fans (addrs 0-7) + pump (8-15). */
    w.devices["rad_fans"]   = Inst("fan-120", { 0.0f, 0.185f, -0.14f }, { 0, -90, 0 }, CASE);
    w.devices["rad_fan_m1"] = Inst("fan-120", { 0.0f, 0.185f, -0.01f }, { 0, -90, 0 }, CASE);
    w.devices["rad_fan_m2"] = Inst("fan-120", { 0.0f, 0.185f, 0.12f }, { 0, -90, 0 }, CASE);
    w.device_settings["rad_fans"].zones["ring"] = ZoneBound("argb_v2_2", 0, true);
    w.device_settings["rad_fan_m1"].mirror_of   = "rad_fans";
    w.device_settings["rad_fan_m2"].mirror_of   = "rad_fans";

    w.devices["pump"] = Inst("pump-360", { 0.05f, 0.05f, -0.11f }, { 0, 0, 90 }, CASE);
    w.device_settings["pump"].zones["ring"] = ZoneBound("argb_v2_2", 8, true);

    /* ARGB_V2_3 — single fan above the GPU's top edge. */
    w.devices["gpu_top_fan"] = Inst("fan-120", { 0.03f, 0.008f, 0.10f }, { 0, 0, 0 }, CASE);
    w.device_settings["gpu_top_fan"].zones["ring"] = ZoneBound("argb_v2_3", 0, true);

    /* 3x Lian Li SL Wireless — vertical side-intake stack. */
    for(int i = 0; i < 3; i++)
    {
        const std::string id = "slw_fan_" + std::to_string(i);
        w.devices[id] = Inst("fan-slw",
            { 0.08f, -0.135f + 0.125f * (float)i, 0.15f }, { 0, 0, 90 }, CASE);
        w.device_settings[id].zones["ring"] = ZoneBound(id, 0, true);
    }

    /* Corsair DIMMs — two 10-LED strips; bindings disambiguate the
       identical controllers by I2C location. */
    for(int i = 0; i < 2; i++)
    {
        const std::string id = "dimm_" + std::to_string(i);
        w.devices[id] = Inst("ram-stick",
            { 0.05f, 0.09f, -0.07f + 0.035f * (float)i }, { 0, 0, 90 }, CASE);
        w.device_settings[id].zones["strip"] = ZoneBound(id, 0, true);
    }

    /* GPU — body is decor; side logo + 3 unverified fan rings. */
    w.devices["gpu_body"] = Inst("gpu-card", { 0.03f, -0.035f, -0.03f }, { 0, -90, 0 }, CASE);
    w.devices["gpu_logo"] = Inst("gpu-logo", { -0.04f, -0.035f, -0.02f }, { 0, 90, 0 }, CASE);
    w.device_settings["gpu_logo"].zones["strip"] = ZoneBound("gpu_logo", 0, true);

    const char* fans[3] = { "gpu_fan_r", "gpu_fan_m", "gpu_fan_l" };
    for(int i = 0; i < 3; i++)
    {
        w.devices[fans[i]] = Inst("gpu-fan",
            { 0.03f, -0.075f, -0.13f + 0.10f * (float)i }, { 0, 0, 0 }, CASE);
        /* zero-RPM lighting unconfirmed — bound but never written */
        w.device_settings[fans[i]].zones["ring"] =
            ZoneBound(fans[i], 0, false);
    }

    /*-----------------------------------------------------*\
    | Zone attachments outside the case.                    |
    \*-----------------------------------------------------*/
    w.device_settings["keyboard"].zones["matrix"] = ZoneBound("kbd_g512", 0, true);
    w.device_settings["mouse"].zones["wheel"] = ZoneBound("mouse_wheel", 0, true);
    w.device_settings["mouse"].zones["logo"]  = ZoneBound("mouse_logo", 0, true);
    w.device_settings["mouse"].zones["strip"] = ZoneBound("mouse_strip", 0, true);

    /*-----------------------------------------------------*\
    | Base colors — same values as the expanded desk, keyed |
    | at the resolved <instance>/<entity> paths.            |
    \*-----------------------------------------------------*/
    w.object_colors["keyboard/body"]    = MakeSceneColor(40, 60, 200);
    w.object_colors["mouse/wheel"]      = MakeSceneColor(200, 30, 30);
    w.object_colors["mouse/logo"]       = MakeSceneColor(200, 30, 30);
    w.object_colors["mouse/strip"]      = MakeSceneColor(200, 30, 30);
    w.object_colors["case_fans/body"]   = MakeSceneColor(0, 180, 160);
    w.object_colors["rad_fans/body"]    = MakeSceneColor(0, 140, 200);
    w.object_colors["pump/body"]        = MakeSceneColor(200, 200, 210);
    w.object_colors["gpu_top_fan/body"] = MakeSceneColor(0, 180, 160);
    for(int i = 0; i < 3; i++)
    {
        w.object_colors["slw_fan_" + std::to_string(i) + "/body"] =
            MakeSceneColor(0, 180, 160);
    }
    for(int i = 0; i < 2; i++)
    {
        w.object_colors["dimm_" + std::to_string(i) + "/body"] =
            MakeSceneColor(220, 140, 30);
    }
    w.object_colors["gpu_logo/body"] = MakeSceneColor(230, 230, 240);

    return w;
}


static SceneObject Group(const std::string& id, const std::string& label,
                         const Vec3& pos)
{
    SceneObject o;
    o.id                 = id;
    o.label              = label;
    o.kind               = ObjectKind::Group;
    o.transform.position = pos;
    return o;
}

static SceneObject Decor(const std::string& id, const std::string& label,
                         const std::string& geometry,
                         const Vec3& pos, const Vec3& rot, const Vec3& size_m)
{
    SceneObject o;
    o.id                   = id;
    o.label                = label;
    o.kind                 = ObjectKind::Decor;
    o.geometry             = geometry;
    o.transform.position   = pos;
    o.transform.rotation_deg = rot;
    o.size_m               = size_m;
    return o;
}

static SceneObject Device(const std::string& id, const std::string& label,
                          const std::string& geometry, const std::string& binding,
                          const Vec3& pos, const Vec3& rot)
{
    SceneObject o;
    o.id                   = id;
    o.label                = label;
    o.kind                 = ObjectKind::Device;
    o.geometry             = geometry;
    o.binding              = binding;
    o.transform.position   = pos;
    o.transform.rotation_deg = rot;
    o.verified             = true;
    return o;
}

static SceneObject Linked(const std::string& id, const std::string& label,
                          const std::string& geometry, const std::string& mirror_of,
                          const Vec3& pos, const Vec3& rot)
{
    SceneObject o;
    o.id                   = id;
    o.label                = label;
    o.kind                 = ObjectKind::Linked;
    o.geometry             = geometry;
    o.mirror_of            = mirror_of;
    o.transform.position   = pos;
    o.transform.rotation_deg = rot;
    o.verified             = false;   /* mirror copies own no addresses */
    return o;
}

SceneDocument BuildDefaultDesk()
{
    SceneDocument doc;
    doc.name = "Default desk";

    /*-----------------------------------------------------*\
    | Persistent bindings — identities, never list indices. |
    | Zone names match the inventory / zone imports.        |
    \*-----------------------------------------------------*/
    doc.bindings = {
        { "kbd_g512",    "G512",              "Logitech",  "", "", -1, "Keyboard",        0 },
        { "mouse_wheel", "Basilisk V3 Pro",   "Razer",     "", "", -1, "Scroll Wheel",    0 },
        { "mouse_logo",  "Basilisk V3 Pro",   "Razer",     "", "", -1, "Logo",            0 },
        { "mouse_strip", "Basilisk V3 Pro",   "Razer",     "", "", -1, "LED Strip",       0 },
        { "argb_v2_1",   "X870E AORUS ELITE", "Gigabyte",  "", "", -1, "ARGB_V2_1",       8 },
        { "argb_v2_2",   "X870E AORUS ELITE", "Gigabyte",  "", "", -1, "ARGB_V2_2",      16 },
        { "argb_v2_3",   "X870E AORUS ELITE", "Gigabyte",  "", "", -1, "ARGB_V2_3",       8 },
        { "gpu_logo",    "MASTER",            "",          "", "",  2, "Side Logo",       0 },
        { "gpu_fan_r",   "MASTER",            "",          "", "",  2, "Right Fan",       0 },
        { "gpu_fan_m",   "MASTER",            "",          "", "",  2, "Middle Fan",      0 },
        { "gpu_fan_l",   "MASTER",            "",          "", "",  2, "Left Fan",        0 },
        { "dimm_0",      "Corsair",           "Corsair",   "",
          "I2C: PawnIO SMBus PIIX4 0, address 0x19",       1, "",   0 },
        { "dimm_1",      "Corsair",           "Corsair",   "",
          "I2C: PawnIO SMBus PIIX4 0, address 0x1B",       1, "",   0 },
        { "slw_fan_0",   "UNI FAN",           "Lian Li",   "", "", -1, "Fan 1",          40 },
        { "slw_fan_1",   "UNI FAN",           "Lian Li",   "", "", -1, "Fan 2",          40 },
        { "slw_fan_2",   "UNI FAN",           "Lian Li",   "", "", -1, "Fan 3",          40 },
    };

    /*-----------------------------------------------------*\
    | Decor + groups. Groups are placement-only parents:    |
    | a group's position equals the old world anchor, so a  |
    | child's local transform is (old world - anchor).      |
    \*-----------------------------------------------------*/
    doc.objects.push_back(Decor("desk", "Desk", "desk",
        { 0.0f, -0.02f, 0.10f }, { 0, 0, 0 }, { 1.4f, 0.04f, 0.75f }));
    doc.objects.push_back(Decor("monitor", "Monitor", "monitor",
        { 0.0f, 0.32f, -0.22f }, { 0, 0, 0 }, { 0.62f, 0.36f, 0.02f }));

    /* The mouse moves as one device — body, wheel, logo, and the
       underglow strip are children of this group. */
    doc.objects.push_back(Group("mouse", "Mouse", { 0.26f, 0.02f, 0.27f }));
    {
        SceneObject body = Decor("mouse_body", "Mouse body", "mouse_body",
            { 0, 0, 0 }, { 0, 0, 0 }, { 0.066f, 0.04f, 0.117f });
        body.parent_id = "mouse";
        doc.objects.push_back(body);
    }

    /* The case and every component mounted inside it form one group
       centered on the shell. */
    doc.objects.push_back(Group("case", "Case", { 0.47f, 0.235f, -0.10f }));
    {
        SceneObject shell = Decor("case_shell", "Case shell", "case_shell",
            { 0, 0, 0 }, { 0, 0, 0 }, { 0.21f, 0.47f, 0.46f });
        shell.parent_id = "case";
        doc.objects.push_back(shell);
    }

    /*-----------------------------------------------------*\
    | Peripherals                                           |
    \*-----------------------------------------------------*/
    {
        /* G512 — emitters are rebuilt from the zone matrix map
           when the binding resolves (layout = "matrix_map").   */
        SceneObject kbd = Device("keyboard", "G512 Keyboard", "keyboard_body",
                                 "kbd_g512", { -0.06f, 0.02f, 0.26f }, { 0, 0, 0 });
        kbd.layout = "matrix_map";
        doc.objects.push_back(kbd);
        doc.object_colors["keyboard"] = MakeSceneColor(40, 60, 200);
    }
    {
        /* Mouse children — local offsets are (old world - group). */
        SceneObject wheel = Device("mouse_wheel", "Mouse wheel", "mouse_zone",
                                   "mouse_wheel", { 0, 0.025f, -0.035f }, { 0, 0, 0 });
        wheel.parent_id = "mouse";
        Emitter e; e.local_pos = { 0, 0, 0 }; e.group = "mouse_wheel"; e.address = 0;
        wheel.emitters.push_back(e);
        doc.objects.push_back(wheel);

        SceneObject logo = Device("mouse_logo", "Mouse logo", "mouse_zone",
                                  "mouse_logo", { 0, 0.025f, 0.02f }, { 0, 0, 0 });
        logo.parent_id = "mouse";
        logo.emitters.push_back(e);
        logo.emitters[0].group = "mouse_logo";
        doc.objects.push_back(logo);

        SceneObject strip = Device("mouse_strip", "Mouse underglow", "mouse_zone",
                                   "mouse_strip", { 0, -0.008f, 0.005f }, { 0, 0, 0 });
        strip.parent_id = "mouse";
        strip.emitters = layout::Ring(11, 0.028f, 90.0f, false, "mouse_strip");
        doc.objects.push_back(strip);

        doc.object_colors["mouse_wheel"] = MakeSceneColor(200, 30, 30);
        doc.object_colors["mouse_logo"]  = MakeSceneColor(200, 30, 30);
        doc.object_colors["mouse_strip"] = MakeSceneColor(200, 30, 30);
    }

    /*-----------------------------------------------------*\
    | Inside the case — all positions are case-local        |
    | (case group anchor: world {0.47, 0.235, -0.10}).      |
    \*-----------------------------------------------------*/
    const float RING_R = 0.052f;

    /* Ring emitters sit on the light-bearing face, not the
       mid-plane: a dot at local y=0 is sealed inside the opaque
       body mesh (fan half-thickness 14 mm, pump 22.5 mm — both
       exceed the 5.5 mm dot radius) and can never be seen.    */
    const float FAN_FACE_Y  = 0.016f;
    const float PUMP_FACE_Y = 0.024f;

    /* ARGB_V2_1 — four case fans on one mirrored 8-LED ring.
       Logical owner + 3 linked copies: bottom intake row x3 running
       front-to-back (back + middle sit at PSU-shroud height, front
       one on the case floor ahead of the shroud) and a rear exhaust
       above the GPU. */
    {
        SceneObject f0 = Device("case_fans", "Case fans (x4 shared)", "fan_body",
                                "argb_v2_1", { 0.0f, -0.16f, -0.14f }, { 0, 0, 0 });
        f0.parent_id = "case";
        f0.emitters = layout::Ring(8, RING_R, 0.0f, false, "case_fans", 0, FAN_FACE_Y);
        doc.objects.push_back(f0);
        SceneObject b1 = Linked("case_fan_b1", "Case fan (mirror)", "fan_body",
                                "case_fans", { 0.0f, -0.16f, -0.01f }, { 0, 0, 0 });
        b1.parent_id = "case";
        doc.objects.push_back(b1);
        SceneObject b2 = Linked("case_fan_b2", "Case fan (mirror)", "fan_body",
                                "case_fans", { 0.0f, -0.205f, 0.14f }, { 0, 0, 0 });
        b2.parent_id = "case";
        doc.objects.push_back(b2);
        SceneObject rear = Linked("case_fan_rear", "Rear exhaust (mirror)", "fan_body",
                                  "case_fans", { 0.0f, 0.065f, -0.20f }, { 90, 0, 0 });
        rear.parent_id = "case";
        doc.objects.push_back(rear);
        doc.object_colors["case_fans"] = MakeSceneColor(0, 180, 160);
    }

    /* ARGB_V2_2 — GAMING 360 ICE: 3 mirrored radiator fans on
       addresses 0-7, pump on 8-15. */
    {
        /* Top-mounted 360 rad — the row runs front-to-back (z); an
           x row overflows the 0.21 m-wide case. 90 deg clockwise. */
        SceneObject rad = Device("rad_fans", "Radiator fans (x3 shared)", "fan_body",
                                 "argb_v2_2", { 0.0f, 0.185f, -0.14f }, { 0, -90, 0 });
        rad.parent_id = "case";
        rad.emitters = layout::Ring(8, RING_R, 0.0f, false, "rad_fans", 0, FAN_FACE_Y);
        doc.objects.push_back(rad);
        SceneObject m1 = Linked("rad_fan_m1", "Radiator fan (mirror)", "fan_body",
                                "rad_fans", { 0.0f, 0.185f, -0.01f }, { 0, -90, 0 });
        m1.parent_id = "case";
        doc.objects.push_back(m1);
        SceneObject m2 = Linked("rad_fan_m2", "Radiator fan (mirror)", "fan_body",
                                "rad_fans", { 0.0f, 0.185f, 0.12f }, { 0, -90, 0 });
        m2.parent_id = "case";
        doc.objects.push_back(m2);

        /* Pump cap faces -x like the Lian Li stack — RGB out from
           the motherboard toward the glass. Sits on the CPU socket,
           -z of the DIMMs (left of them in the scene view). */
        SceneObject pump = Device("pump", "Cooler pump", "pump_body",
                                  "argb_v2_2", { 0.05f, 0.05f, -0.11f }, { 0, 0, 90 });
        pump.parent_id = "case";
        pump.emitters = layout::Ring(8, 0.020f, 0.0f, false, "pump", 8, PUMP_FACE_Y);
        doc.objects.push_back(pump);

        doc.object_colors["rad_fans"] = MakeSceneColor(0, 140, 200);
        doc.object_colors["pump"]     = MakeSceneColor(200, 200, 210);
    }

    /* ARGB_V2_3 — single fan just above the GPU's top edge, near
       the card's front end. */
    {
        SceneObject top = Device("gpu_top_fan", "Top GPU fan", "fan_body",
                                 "argb_v2_3", { 0.03f, 0.008f, 0.10f }, { 0, 0, 0 });
        top.parent_id = "case";
        top.emitters = layout::Ring(8, RING_R, 0.0f, false, "gpu_top_fan", 0, FAN_FACE_Y);
        doc.objects.push_back(top);
        doc.object_colors["gpu_top_fan"] = MakeSceneColor(0, 180, 160);
    }

    /* 3x Lian Li SL Wireless — vertical side-intake stack on the
       right wall near the front; rz=+90 stands the fan up so its
       light face points -x into the case (toward the glass). The UNI
       FAN controller exposes one zone per fan ("Fan 1-3", 40 LEDs
       each), so each scene fan maps 1:1 onto a physical zone. The
       40 LEDs are two edge strips, not a ring (SLW_LED_POINTS). */
    for(int i = 0; i < 3; i++)
    {
        const std::string id = "slw_fan_" + std::to_string(i);
        SceneObject fan = Device(id, "SL Wireless fan " + std::to_string(i + 1),
                                 "fan_body", id,
                                 { 0.08f, -0.135f + 0.125f * (float)i, 0.15f }, { 0, 0, 90 });
        fan.parent_id = "case";
        fan.emitters = SlwEmitters(id);
        doc.objects.push_back(fan);
        doc.object_colors[id] = MakeSceneColor(0, 180, 160);
    }

    /* Corsair DIMMs — two 10-LED strips. rz=+90 stands the stick up:
       long edge vertical, thin faces toward +/-z (edge-on to the
       glass), and the LED strip ends up running up the glass-facing
       edge like the Dominator light bar. Slots spread along z.
       Identical controllers, so the bindings disambiguate by I2C
       location (0x19/0x1B). Which address is which physical slot needs
       a visual flash check; swap the locations in the bindings if
       dimm_0/dimm_1 are reversed. */
    for(int i = 0; i < 2; i++)
    {
        const std::string id = "dimm_" + std::to_string(i);
        SceneObject dimm = Device(id, "RAM stick " + std::to_string(i + 1),
                                  "ram_body", id,
                                  { 0.05f, 0.09f, -0.07f + 0.035f * (float)i },
                                  { 0, 0, 90 });
        dimm.parent_id = "case";
        dimm.emitters = layout::Strip(10, 0.012f, { -0.054f, 0.03f, 0 }, id);
        doc.objects.push_back(dimm);
        doc.object_colors[id] = MakeSceneColor(220, 140, 30);
    }

    /* GPU — normal (horizontal) mount: ry=-90 turns the card's
       0.30 m length onto z (bracket at the back), so it is a flat
       slab 0.05 m thick in y reaching 0.13 m out from the board
       toward the glass. The glass sees the card's -x edge, which
       carries the side logo. Fan rings rendered but unverified
       (zero-RPM lighting unconfirmed — plan forbids writes). */
    {
        SceneObject gpu = Decor("gpu_body", "GPU", "gpu_body",
            { 0.03f, -0.035f, -0.03f }, { 0, -90, 0 }, { 0.30f, 0.05f, 0.13f });
        gpu.parent_id = "case";
        doc.objects.push_back(gpu);

        /* Side logo on the glass-facing (-x) edge; ry=90 lays the
           strip along the card's z length. */
        SceneObject logo = Device("gpu_logo", "GPU side logo", "gpu_logo",
                                  "gpu_logo", { -0.04f, -0.035f, -0.02f }, { 0, 90, 0 });
        logo.parent_id = "case";
        logo.emitters = layout::Strip(4, 0.02f, { -0.03f, 0, 0 }, "gpu_logo");
        doc.objects.push_back(logo);
        doc.object_colors["gpu_logo"] = MakeSceneColor(230, 230, 240);

        const char* fans[3]  = { "gpu_fan_r", "gpu_fan_m", "gpu_fan_l" };
        const char* names[3] = { "Right", "Middle", "Left" };
        for(int i = 0; i < 3; i++)
        {
            /* Fans hang just below the card's bottom face (card
               bottom y=-0.06), spread along its z length. Emitters
               face down like the hardware. */
            SceneObject fan = Device(fans[i], std::string("GPU ") + names[i] + " fan",
                                     "fan_body", fans[i],
                                     { 0.03f, -0.075f, -0.13f + 0.10f * (float)i },
                                     { 0, 0, 0 });
            fan.parent_id = "case";
            fan.emitters = layout::Ring(8, 0.04f, 0.0f, false, fans[i], 0, -FAN_FACE_Y);
            fan.verified = false;
            doc.objects.push_back(fan);
        }
    }

    return doc;
}

} /* namespace studio */
