// SPDX-License-Identifier: GPL-2.0-or-later
//
// Profile-store tests (src/engine/ProfileStore.h + SceneLoader.h +
// the PlaybackController durable-store seams — Task 3.8).
// Offline only: temp dirs, fake snapshots, no hardware/sockets.
//
// Covers the brief's acceptance list:
//   - atomicity: a torn temp file (kill-save-midway) leaves the
//     old file intact; a completed save is never torn
//   - migration: a legacy (schema-less) profile loads, defaults
//     playback to LiveEffects, preserves still-applicable fields
//     (incl. unknown members) — never silently uploaded playback
//   - missing-device binding: the persisted resolved identity is
//     resolvable without the device present and is NEVER
//     reassigned to a same-named different-serial device
//   - corruption: primary corrupt -> last-known-good + reported
//     error; both corrupt -> honest failure, never blank
//   - calibration regression: out-of-range LED addresses rejected
//     at the persistence boundary
//   - durable PlaybackMode: a selection written through the
//     controller's save_modes seam survives an engine "restart"

#include "ProfileStore.h"
#include "SceneLoader.h"
#include "PlaybackController.h"

#include "scene/BindingResolver.h"

#include <nlohmann/json.hpp>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

using namespace openrgb2;

static int checks   = 0;
static int failures = 0;
#define CHECK(x) do { ++checks; if(!(x)) { ++failures; \
    std::printf("FAIL line %d: %s\n", __LINE__, #x); } } while(0)

namespace fs = std::filesystem;

static fs::path g_tmp;

static std::string Path(const std::string& name)
{
    return (g_tmp / name).string();
}

static void WriteText(const std::string& path, const std::string& s)
{
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << s;
}

static EngineState SampleState(const char* key, PlaybackMode mode)
{
    EngineState st;
    PersistedSceneMode m;
    m.scene_key        = key;
    m.mode             = mode;
    m.params.speed     = 150;
    m.params.reverse   = true;
    m.params.color     = RgbColor{ 0x10, 0x20, 0x30 };
    st.scenes.push_back(m);
    studio::DeviceBinding b;
    b.id              = "b-fan";
    b.controller_name = "UNI FAN SL V3 Wireless";
    b.vendor          = "Lian Li";
    b.serial          = "AA:BB:CC:DD:EE:FF";   /* placeholder */
    b.location        = "Wireless: AA:BB:CC:DD:EE:FF";
    b.device_type     = 3;
    b.zone_name       = "Fan 1";
    b.zone_leds       = 40;
    st.bindings[b.id] = b;
    return st;
}

/*---------------------------------------------------------*\
||| T1: atomic writes — torn temp or old-or-new, never a   ||
|||     half-written profile                               ||
\*---------------------------------------------------------*/

static void TestAtomicity()
{
    const std::string p = Path("atomic.json");
    ProfileStore      store(p);
    std::string       err;

    CHECK(store.Save(SampleState("name:a", PlaybackMode::LiveEffects),
                     err));
    CHECK(fs::exists(p));

    /* Kill-save-midway: a torn temp file is left behind; the
       committed file is untouched. */
    WriteText(store.TempPath(), "{\"kind\":\"openrgb2-engine-st");
    ProfileStore::LoadResult lr = store.Load();
    CHECK(lr.status == ProfileStore::LoadStatus::Loaded);
    CHECK(lr.state.scenes.size() == 1);
    CHECK(lr.state.scenes[0].scene_key == "name:a");

    /* A completed second save lands whole; the previous valid
       file became the last-known-good. */
    CHECK(store.Save(SampleState("name:b",
                                 PlaybackMode::AnimationPlayback),
                     err));
    CHECK(fs::exists(store.LkgPath()));
    lr = store.Load();
    CHECK(lr.status == ProfileStore::LoadStatus::Loaded);
    CHECK(lr.state.scenes.size() == 1);
    CHECK(lr.state.scenes[0].scene_key == "name:b");
    CHECK(lr.state.scenes[0].mode == PlaybackMode::AnimationPlayback);
    CHECK(lr.state.scenes[0].params.speed == 150);
    CHECK(lr.state.scenes[0].params.reverse == true);
    CHECK(lr.state.bindings.size() == 1);

    /* The LKG is the previous state. */
    std::ifstream in(store.LkgPath());
    std::stringstream ss;
    ss << in.rdbuf();
    const nlohmann::json lkg = nlohmann::json::parse(ss.str());
    CHECK(lkg["scenes"][0]["scene"].get<std::string>() == "name:a");
}

/*---------------------------------------------------------*\
||| T2: migration — a schema-less legacy profile loads      ||
|||     forward, defaults playback to Live effects, keeps  ||
|||     every field that still applies                      ||
\*---------------------------------------------------------*/

static void TestMigration()
{
    const std::string p = Path("legacy.json");
    WriteText(p, R"json({
        "scenes": {
            "name:uploaded": { "playback": "animation",
                               "speed": 140, "reverse": true },
            "name:plain":    { },
            "name:odd":      "animation"
        },
        "bindings": {
            "b-fan": { "controller_name": "UNI FAN SL V3 Wireless",
                       "serial": "AA:BB:CC:DD:EE:FF",
                       "zone_name": "Fan 1", "zone_leds": 40 }
        },
        "custom_data": { "note": "keep me" }
    })json");
    ProfileStore            store(p);
    ProfileStore::LoadResult lr = store.Load();
    CHECK(lr.status == ProfileStore::LoadStatus::Migrated);
    CHECK(lr.error.empty());
    CHECK(lr.state.scenes.size() == 3);

    /* The scene that never had a playback field migrates to
       LiveEffects — an old profile is preserved as a Live
       effect, NEVER silently uploaded playback. */
    bool saw_live = false, saw_anim = false, saw_shorthand = false;
    for(const PersistedSceneMode& m : lr.state.scenes)
    {
        if(m.scene_key == "name:plain")
        {
            saw_live = m.mode == PlaybackMode::LiveEffects;
        }
        if(m.scene_key == "name:uploaded")
        {
            /* An EXPLICIT old selection is data — it survives. */
            saw_anim = m.mode == PlaybackMode::AnimationPlayback &&
                       m.params.speed == 140 && m.params.reverse;
        }
        if(m.scene_key == "name:odd")
        {
            saw_shorthand =
                m.mode == PlaybackMode::AnimationPlayback;
        }
    }
    CHECK(saw_live);
    CHECK(saw_anim);
    CHECK(saw_shorthand);

    /* Fields that still apply are never silently dropped. */
    CHECK(lr.state.bindings.count("b-fan") == 1);
    CHECK(lr.state.bindings["b-fan"].serial == "AA:BB:CC:DD:EE:FF");
    CHECK(lr.state.extensions["custom_data"]["note"]
              .get<std::string>() == "keep me");

    /* Re-saving the migrated state writes the current schema
       and round-trips the retained members. */
    std::string err;
    CHECK(store.Save(lr.state, err));
    lr = store.Load();
    CHECK(lr.status == ProfileStore::LoadStatus::Loaded);
    CHECK(lr.state.extensions["custom_data"]["note"]
              .get<std::string>() == "keep me");

    /* An unrecognised legacy mode migrates to Live (safe),
       with a warning — never to playback. The warning must
       actually reach the caller (review fix: warnings were
       silently dropped by ParseState). */
    WriteText(Path("odd-mode.json"),
              "{ \"scenes\": [ { \"scene\": \"name:x\", "
              "               \"mode\": \"turbo\" } ] }");
    ProfileStore            odd(Path("odd-mode.json"));
    ProfileStore::LoadResult ol = odd.Load();
    CHECK(ol.status == ProfileStore::LoadStatus::Migrated);
    CHECK(ol.state.scenes.size() == 1);
    CHECK(ol.state.scenes[0].mode == PlaybackMode::LiveEffects);
    CHECK(!ol.warnings.empty());
    if(!ol.warnings.empty())
    {
        CHECK(ol.warnings[0].find("turbo") != std::string::npos);
        CHECK(ol.warnings[0].find("live_effects")
              != std::string::npos);
    }

    /* A newer schema is rejected — no silent downgrade. */
    WriteText(Path("future.json"),
              "{ \"schema_version\": 99, \"scenes\": [] }");
    ProfileStore            fut(Path("future.json"));
    ProfileStore::LoadResult fl = fut.Load();
    CHECK(fl.status == ProfileStore::LoadStatus::Failed);
    CHECK(!fl.error.empty());

    /* Strict current-schema validation: an unknown mode in a
       v1 file is corruption, not a quiet default. */
    WriteText(Path("bad-mode.json"),
              "{ \"schema_version\": 1, \"scenes\": ["
              "  { \"scene\": \"name:x\", \"mode\": \"turbo\" } ] }");
    ProfileStore            bad(Path("bad-mode.json"));
    ProfileStore::LoadResult bl = bad.Load();
    CHECK(bl.status == ProfileStore::LoadStatus::Failed);
}

/*---------------------------------------------------------*\
||| T3: offline bindings — a missing device keeps its       ||
|||     identity; a same-named different device never       ||
|||     inherits the binding                                ||
\*---------------------------------------------------------*/

static void TestOfflineBindings()
{
    /* The persisted identity table (as RecordResolvedBindings
       wrote it after a real enumeration). */
    std::map<std::string, studio::DeviceBinding> resolved;
    {
        studio::DeviceBinding b;
        b.id              = "b1";
        b.controller_name = "UNI FAN SL V3 Wireless";
        b.vendor          = "Lian Li";
        b.serial          = "AA:BB:CC:DD:EE:FF";   /* placeholder */
        b.location        = "Wireless: AA:BB:CC:DD:EE:FF";
        b.device_type     = 3;
        b.zone_name       = "Fan 1";
        b.zone_leds       = 40;
        resolved[b.id]    = b;
    }

    /* A committed/identifier-free scene binding (name-only)
       gains the recorded serial/location/zone at apply. */
    studio::SceneDocument doc;
    {
        studio::DeviceBinding b;
        b.id              = "b1";
        b.controller_name = "UNI FAN SL V3 Wireless";
        doc.bindings.push_back(b);
    }
    CHECK(AugmentSceneBindings(doc, resolved));
    const studio::DeviceBinding& aug = doc.bindings[0];
    CHECK(aug.serial   == "AA:BB:CC:DD:EE:FF");
    CHECK(aug.location == "Wireless: AA:BB:CC:DD:EE:FF");
    CHECK(aug.zone_name == "Fan 1");
    CHECK(aug.zone_leds == 40);

    /* Resolution without the device present: empty snapshot
       list -> stays bound to its identity, reported
       unresolved, never reassigned. */
    std::vector<studio::ControllerSnapshot> none;
    studio::ResolvedBinding r =
        studio::ResolveBinding(aug, none);
    CHECK(r.status == studio::BindingStatus::Unresolved);
    CHECK(aug.serial == "AA:BB:CC:DD:EE:FF");   /* identity kept */

    /* A same-named DIFFERENT device (different serial) must
       not claim the binding. */
    studio::ControllerSnapshot impostor;
    impostor.name     = "UNI FAN SL V3 Wireless";
    impostor.vendor   = "Lian Li";
    impostor.serial   = "11:22:33:44:55:66";     /* placeholder */
    impostor.location = "Wireless: 11:22:33:44:55:66";
    impostor.device_type = 3;
    impostor.zones.push_back({ "Fan 1", 40, 0, 0, false });
    r = studio::ResolveBinding(aug, { impostor });
    CHECK(r.status == studio::BindingStatus::Unresolved);

    /* The SAME device returning resolves again. */
    studio::ControllerSnapshot real = impostor;
    real.serial   = "AA:BB:CC:DD:EE:FF";
    real.location = "Wireless: AA:BB:CC:DD:EE:FF";
    r = studio::ResolveBinding(aug, { real });
    CHECK(r.status == studio::BindingStatus::Resolved);
    CHECK(r.zone_index == 0);

    /* A re-purposed binding id (different controller_name) is
       never rewritten by the persisted identity. */
    studio::SceneDocument doc2;
    {
        studio::DeviceBinding b;
        b.id              = "b1";
        b.controller_name = "Some Other Controller";
        doc2.bindings.push_back(b);
    }
    CHECK(!AugmentSceneBindings(doc2, resolved));
    CHECK(doc2.bindings[0].serial.empty());
}

/*---------------------------------------------------------*\
||| T4: corruption -> last-known-good, honestly reported   ||
\*---------------------------------------------------------*/

static void TestRecovery()
{
    const std::string p = Path("recover.json");
    ProfileStore      store(p);
    std::string       err;

    CHECK(store.Save(SampleState("name:old",
                                 PlaybackMode::LiveEffects), err));
    CHECK(store.Save(SampleState("name:new",
                                 PlaybackMode::AnimationPlayback), err));

    /* Torn primary: recover the last-known-good + report. */
    WriteText(p, "{\"kind\":\"openrgb2-engine-state\",\"schema_");
    ProfileStore::LoadResult lr = store.Load();
    CHECK(lr.status == ProfileStore::LoadStatus::Recovered);
    CHECK(!lr.error.empty());
    CHECK(lr.state.scenes.size() == 1);
    CHECK(lr.state.scenes[0].scene_key == "name:old");

    /* Both corrupt -> honest failure, never a blank state
       presented as success. */
    WriteText(store.LkgPath(), "garbage{{{");
    lr = store.Load();
    CHECK(lr.status == ProfileStore::LoadStatus::Failed);
    CHECK(!lr.error.empty());
    CHECK(lr.state.scenes.empty());

    /* Saving over a corrupt primary preserves it as .rejected
       rather than destroying it. */
    WriteText(Path("preserve.json"), "{{{corrupt");
    ProfileStore keep(Path("preserve.json"));
    CHECK(keep.Save(SampleState("name:k",
                                PlaybackMode::LiveEffects), err));
    CHECK(fs::exists(keep.RejectedPath()));
    std::ifstream in(keep.RejectedPath());
    std::stringstream ss;
    ss << in.rdbuf();
    CHECK(ss.str().find("corrupt") != std::string::npos);
    CHECK(keep.Load().status == ProfileStore::LoadStatus::Loaded);

    /* A missing profile is a fresh start — distinct from
       corruption. */
    ProfileStore none(Path("absent.json"));
    lr = none.Load();
    CHECK(lr.status == ProfileStore::LoadStatus::Empty);
    CHECK(lr.error.empty());
}

/*---------------------------------------------------------*\
||| T5: persistence-boundary LED-address validation         ||
|||     (calibration-report regression)                     ||
\*---------------------------------------------------------*/

static void TestBoundaryValidation()
{
    studio::SceneDocument doc;
    {
        studio::DeviceBinding b;
        b.id        = "b1";
        b.zone_name = "ring";
        b.zone_leds = 8;
        doc.bindings.push_back(b);
    }
    studio::SceneObject o;
    o.id       = "fan";
    o.kind     = studio::ObjectKind::Device;
    o.binding  = "b1";
    o.verified = true;
    for(int a = 0; a < 8; ++a)
    {
        studio::Emitter e;
        e.address = a;
        o.emitters.push_back(e);
    }
    doc.objects.push_back(o);

    /* In-range: clean. */
    std::vector<std::string> errs;
    CHECK(ValidateSceneLedAddresses(doc, &errs));
    CHECK(errs.empty());

    /* The calibration regression: address 8 on an 8-LED zone —
       out of range, rejected at the boundary. */
    doc.objects[0].emitters.push_back(studio::Emitter{ {}, "", 8 });
    CHECK(!ValidateSceneLedAddresses(doc, &errs));
    CHECK(!errs.empty());
    CHECK(errs.front().find("out of range") != std::string::npos);

    /* Below the render-only sentinel is invalid too. */
    doc.objects[0].emitters.back().address = -2;
    errs.clear();
    CHECK(!ValidateSceneLedAddresses(doc, &errs));
    CHECK(errs.front().find("invalid") != std::string::npos);

    /* -1 (render-only) is legal; zone_leds 0 means unchecked. */
    doc.objects[0].emitters.back().address = -1;
    doc.bindings[0].zone_leds = 0;
    errs.clear();
    CHECK(ValidateSceneLedAddresses(doc, &errs));
    CHECK(errs.empty());
}

/*---------------------------------------------------------*\
||| T6: durable PlaybackMode — the per-scene selection     ||
|||     survives an engine "restart"                        ||
\*---------------------------------------------------------*/

struct FakeBackend
{
    std::vector<DeviceSnapshot> snaps;
    BackendModeReport           report;
    std::vector<std::string>    selects;
};

static DeviceSnapshot WirelessSnap()
{
    DeviceSnapshot s;
    s.key = DeviceKey{ "Lian Li: SL Wireless", "AABBCCDDEEFF",
                       "Wireless: AA:BB:CC:DD:EE:FF", "" };
    s.topology_generation = 7;
    s.zones = { { "Fan 1", 40 }, { "Fan 2", 40 }, { "Fan 3", 40 } };
    s.modes = { "Direct", "Static", "Spectrum Cycle",
                "Rainbow Wave", "Breathing" };
    s.connection = ConnectionState::Connected;
    return s;
}

struct ControllerHarness
{
    FakeBackend backend;
    TimePoint   now{};
    std::shared_ptr<const studio::SceneDocument> scene;

    static PlaybackController::Deps MakeDeps(FakeBackend& b,
        std::shared_ptr<const studio::SceneDocument>& scene,
        TimePoint& now)
    {
        PlaybackController::Deps d;
        d.scene     = [&scene] { return scene; };
        d.snapshots = [&b] { return b.snaps; };
        d.select_mode =
            [&b](const DeviceKey&, const std::string& name,
                 const PlaybackModePatch&, std::string&)
            {
                b.selects.push_back(name);
                return true;
            };
        d.active_mode =
            [&b](const DeviceKey&, BackendModeReport& rep)
            {
                rep = b.report;
                return true;
            };
        d.refresh_status = [](std::string&) { return true; };
        d.set_suppressed = [](const DeviceKey&, bool,
                              const std::string&) {};
        d.drop_pending   = [](const DeviceKey&) { return size_t(0); };
        d.wait_idle      = [](const DeviceKey&, Clock::duration)
            { return true; };
        d.now = [&now] { return now; };
        return d;
    }
};

static std::shared_ptr<studio::SceneDocument>
MakeScene(const std::string& name, const std::string& preset)
{
    auto d = std::make_shared<studio::SceneDocument>();
    d->name          = name;
    d->effect.preset = preset;
    return d;
}

static void TestDurablePlaybackMode()
{
    const std::string p = Path("modes.json");

    /*---- "Run" 1: scene A selects animation; the selection is
       written atomically through the profile store. */
    {
        FakeBackend backend;
        backend.snaps = { WirelessSnap() };
        backend.report.valid = true;
        backend.report.name  = "Direct";
        std::shared_ptr<const studio::SceneDocument> scene =
            MakeScene("sceneA", "rainbow_wave");
        TimePoint now{};

        ProfileStore store(p);
        PlaybackController::Deps d =
            ControllerHarness::MakeDeps(backend, scene, now);
        d.save_modes =
            [&store](const std::vector<PersistedSceneMode>& modes,
                     std::string& err)
            {
                EngineState st;
                st.scenes = modes;
                return store.Save(st, err);
            };
        PlaybackController ctl(d);

        PlaybackParams params;
        params.speed = 150;
        const PlaybackController::Result r =
            ctl.Request(PlaybackMode::AnimationPlayback,
                        PlaybackControl::Playing, params);
        CHECK(r.accepted);
        CHECK(backend.selects.size() == 1);

        /* The selection is on disk — durable. */
        CHECK(fs::exists(p));
        std::ifstream in(p);
        std::stringstream ss;
        ss << in.rdbuf();
        const nlohmann::json j = nlohmann::json::parse(ss.str());
        CHECK(j["scenes"].size() == 1);
        CHECK(j["scenes"][0]["mode"].get<std::string>()
              == "animation_playback");
        CHECK(j["scenes"][0]["speed"].get<int>() == 150);
        CHECK(j["scenes"][0]["scene"].get<std::string>()
              == "name:sceneA");
    }

    /*---- "Run" 2 (engine restart): the profile restores the
       per-scene store; re-applying scene A re-issues its
       animation selection — no re-pick needed. */
    {
        ProfileStore            store(p);
        ProfileStore::LoadResult lr = store.Load();
        CHECK(lr.status == ProfileStore::LoadStatus::Loaded);
        CHECK(lr.state.scenes.size() == 1);
        CHECK(lr.state.scenes[0].mode ==
              PlaybackMode::AnimationPlayback);

        FakeBackend backend;
        backend.snaps = { WirelessSnap() };
        backend.report.valid = true;
        backend.report.name  = "Direct";
        std::shared_ptr<const studio::SceneDocument> scene =
            MakeScene("sceneA", "rainbow_wave");
        TimePoint now{};

        PlaybackController::Deps d =
            ControllerHarness::MakeDeps(backend, scene, now);
        PlaybackController ctl(d);
        CHECK(ctl.RestoreSceneModes(lr.state.scenes) == 1);

        ctl.OnSceneApplied(scene);
        /* The persisted selection re-applied: a fresh upload
           select was issued without any user re-request. */
        CHECK(backend.selects.size() == 1);
        CHECK(backend.selects[0] == "Rainbow Wave");
    }
}

/*---------------------------------------------------------*\
||| T7: --scene workspace-doc resolution — the profile/    ||
|||     workspace layer resolves compact and legacy forms  ||
|||     into the same SceneDocument                        ||
\*---------------------------------------------------------*/

static void TestSceneLoader()
{
    /* A minimal device type the workspace expands against —
       id == file name (the registry contract). */
    const fs::path types_dir = g_tmp / "types";
    fs::create_directories(types_dir);
    WriteText((types_dir / "mini-fan.device.json").string(), R"json({
        "schema_version": 1,
        "id": "mini-fan",
        "name": "Mini Fan",
        "category": "fan",
        "entities": {
            "body": { "geometry": "fan_body", "zone": "ring" }
        },
        "zones": [
            { "id": "ring", "entity": "body", "led_count": 8,
              "layout": { "type": "ring", "radius_m": 0.05 } }
        ]
    })json");

    studio::PresetRegistry reg;
    std::vector<std::string> rerrs;
    CHECK(reg.LoadDirectory(types_dir.string(), &rerrs));
    CHECK(reg.Contains("mini-fan"));

    /* Compact workspace -> resolved SceneDocument with the
       expanded bound object. */
    const std::string ws_path = Path("studio.json");
    WriteText(ws_path, R"json({
        "schema_version": 3,
        "name": "desk",
        "devices": {
            "fan": { "type": "mini-fan",
                     "x": 0.0, "y": 0.1, "z": 0.0,
                     "rx": 0.0, "ry": 0.0, "rz": 0.0 }
        },
        "device_settings": {
            "fan": { "zones": {
                "ring": { "binding": "b1", "addr_base": 0,
                          "verified": true } } }
        },
        "bindings": {
            "b1": { "controller_name": "Mini Fan Controller",
                    "zone_name": "ring", "zone_leds": 8 }
        }
    })json");
    std::shared_ptr<studio::SceneDocument> doc;
    std::string err;
    std::vector<std::string> warnings;
    CHECK(LoadSceneDocument(ws_path, reg, doc, err, &warnings));
    CHECK(doc != nullptr);
    if(doc)
    {
        CHECK(!doc->bindings.empty());
        CHECK(doc->bindings[0].id == "b1");
        bool found_device = false;
        for(const studio::SceneObject& o : doc->objects)
        {
            if(o.kind == studio::ObjectKind::Device &&
               o.binding == "b1" && o.verified)
            {
                found_device = true;
                CHECK(o.emitters.size() == 8);
            }
        }
        CHECK(found_device);
    }

    /* Expanded scene JSON still loads directly. */
    const std::string exp_path = Path("expanded.json");
    WriteText(exp_path, R"json({
        "version": 2,
        "name": "expanded",
        "objects": [ { "id": "o1", "kind": "decor" } ]
    })json");
    doc.reset();
    CHECK(LoadSceneDocument(exp_path, reg, doc, err));
    CHECK(doc != nullptr && doc->name == "expanded");

    /* Legacy host-settings blob -> migrated forward through
       the workspace path (never silently to playback — the
       migrated scene has no playback entry at all). */
    const std::string leg_path = Path("legacy-settings.json");
    WriteText(leg_path, R"json({
        "scene": {
            "version": 2,
            "name": "legacy-scene",
            "objects": [
                { "id": "lamp", "kind": "decor",
                  "emitters": [] }
            ]
        },
        "inputs": { "audio": false }
    })json");
    doc.reset();
    CHECK(LoadSceneDocument(leg_path, reg, doc, err, &warnings));
    CHECK(doc != nullptr);

    /* The on-disk v1/v2 expanded workspace — schema_version < 3
       with a "scene" member + "definitions" (the real format
       written by the old project's ConfigStore, e.g.
       studio.v2.backup.json). StudioConfig::FromJson rejects
       this outright, so detection must route it to the
       migration path (review fix). */
    const std::string v2_path = Path("studio.v2.json");
    WriteText(v2_path, R"json({
        "schema_version": 2,
        "name": "v2 desk",
        "definitions": { "devices": [], "effects": [] },
        "inputs": { "audio": false, "keys": false,
                    "screen": false, "screen_index": 0,
                    "sens_pct": 100, "decay_pct": 100 },
        "output": { "brightness": 1.0, "live_on_startup": false },
        "scene": {
            "version": 2,
            "name": "v2-scene",
            "bindings": [
                { "id": "b1", "controller_name": "Mini Fan",
                  "vendor": "Test", "zone_name": "ring",
                  "zone_leds": 1 }
            ],
            "objects": [
                { "id": "fan0", "kind": "device", "binding": "b1",
                  "geometry": "fan_ring", "label": "Fan",
                  "verified": true,
                  "emitters": [
                      { "address": 0, "group": "ring",
                        "pos": { "x": 0.0, "y": 0.0, "z": 0.0 } }
                  ] }
            ]
        }
    })json");
    doc.reset();
    warnings.clear();
    CHECK(LoadSceneDocument(v2_path, reg, doc, err, &warnings));
    CHECK(doc != nullptr);
    if(doc)
    {
        /* Migration + re-resolution produced the device object
           bound to b1 — live_only stays false (never silently
           armed). */
        bool found = false;
        for(const studio::SceneObject& o : doc->objects)
        {
            if(o.kind == studio::ObjectKind::Device &&
               o.binding == "b1")
            {
                found = true;
            }
        }
        CHECK(found);
    }

    /* Garbage is rejected honestly, not blanked. */
    const std::string bad_path = Path("bad.json");
    WriteText(bad_path, "{ not json");
    doc.reset();
    CHECK(!LoadSceneDocument(bad_path, reg, doc, err));
    CHECK(!err.empty());
}

int main()
{
    g_tmp = fs::temp_directory_path() /
            "openrgb2_profile_store_test";
    std::error_code ec;
    fs::remove_all(g_tmp, ec);
    fs::create_directories(g_tmp);

    TestAtomicity();
    TestMigration();
    TestOfflineBindings();
    TestRecovery();
    TestBoundaryValidation();
    TestDurablePlaybackMode();
    TestSceneLoader();

    /* Opt-in manual check: point OPENRGB2_SCENE_FILE at a real
       scene/workspace file (e.g. a studio.v2.backup.json) to
       verify LoadSceneDocument against it. No-op when unset —
       the suite itself stays hermetic. */
    if(const char* extra = std::getenv("OPENRGB2_SCENE_FILE"))
    {
        if(*extra)
        {
            studio::PresetRegistry reg;
            std::shared_ptr<studio::SceneDocument> d;
            std::string err;
            std::vector<std::string> warns;
            const bool ok =
                LoadSceneDocument(extra, reg, d, err, &warns);
            std::printf("scene-file %s: %s (%u objects, %u warnings%s%s)\n",
                extra, ok ? "ok" : "FAILED",
                d ? (unsigned int)d->objects.size() : 0u,
                (unsigned int)warns.size(),
                ok ? "" : ", err: ", ok ? "" : err.c_str());
            CHECK(ok);
        }
    }

    fs::remove_all(g_tmp, ec);
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
