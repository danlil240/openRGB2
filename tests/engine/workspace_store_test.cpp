// SPDX-License-Identifier: GPL-2.0-or-later
//
// Workspace-store tests (src/engine/WorkspaceStore.h +
// WorkspaceService.h — studio-import Task 2). Offline only:
// temp dirs, no hardware/sockets.
//
// Covers the brief's acceptance list:
//   - immutable generations + atomic active-generation marker:
//     a crash before/after marker replacement, a torn staging
//     dir, or an incomplete generation can never displace the
//     last valid generation
//   - invalid primary marker with a valid last-known-good
//     backup recovers honestly
//   - denied writes fail the commit and leave prior state usable
//   - resource collision: an imported type colliding with a
//     different-content managed library file lands under a
//     suffixed id with remapped references — a local type is
//     never overwritten
//   - interrupted resource installation: a generation whose
//     manifest references missing/corrupt resource files is
//     never selected
//   - resource_snapshot_survives_restart: the generation's own
//     resource snapshot resolves the workspace after a restart
//     with an empty base registry
//   - imports never arm output/input/verified ownership, and the
//     original imported bytes are retained verbatim
//   - revision-checked commits reject stale writers

#include "WorkspaceStore.h"
#include "WorkspaceService.h"

#include "config/WorkspaceCandidate.h"
#include "config/StudioConfig.h"
#include "presets/DevicePreset.h"
#include "presets/EffectRegistry.h"
#include "presets/PresetRegistry.h"
#include "scene/SceneResolver.h"
#include "scene/SceneTypes.h"

#include <nlohmann/json.hpp>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using namespace openrgb2;
using nlohmann::json;

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

static std::string ReadText(const std::string& path)
{
    std::ifstream in(path, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

/* A minimal device type — one ring zone of `leds` emitters on a
   single "body" entity. `id` must equal the intended file id. */
static std::string TypeJson(const std::string& id, int leds,
                            const std::string& variant)
{
    json e = json::object({
        { "geometry", "fan_body" },
        { "zone",     "ring"     },
        { "tag",      variant    },   /* content discriminator */
    });
    /* `tag` is not a real entity field — keep content distinct via
       the type NAME instead so the document stays schema-valid. */
    e.erase("tag");
    return json::object({
        { "schema_version", 1        },
        { "id",             id       },
        { "name",           id + " " + variant },
        { "category",       "fan"    },
        { "entities",       json::object({{ "body", e }}) },
        { "zones", json::array({ json::object({
              { "id",        "ring" },
              { "entity",    "body" },
              { "led_count", leds   },
              { "layout", json::object({
                    { "type",     "ring" },
                    { "radius_m", 0.05  } }) } }) }) },
    }).dump(2);
}

/* Compact v3 workspace referencing one device type, bound + verified. */
static std::string WorkspaceJson(const std::string& name,
                                 const std::string& type_id)
{
    return json::object({
        { "schema_version", 3    },
        { "name",           name },
        { "devices", json::object({
              { "fan", json::object({
                    { "type", type_id },
                    { "x", 0.1 }, { "y", 0.05 }, { "z", -0.02 },
                    { "rx", 0.0 }, { "ry", 30.0 }, { "rz", 0.0 } }) } }) },
        { "bindings", json::object({
              { "b1", json::object({
                    { "controller_name", "Test Controller" },
                    { "zone_name",       "ring"            },
                    { "zone_leds",       8                 } }) } }) },
        { "device_settings", json::object({
              { "fan", json::object({
                    { "zones", json::object({
                          { "ring", json::object({
                                { "binding",   "b1"  },
                                { "addr_base", 0     },
                                { "verified",  true  } }) } }) } }) } }) },
    }).dump(2);
}

/* const-safe lookup (std::map::operator[] is non-const). */
static const studio::DeviceInstance* DevAt(
    const studio::StudioDocument& d, const std::string& id)
{
    const auto it = d.devices.find(id);
    return it == d.devices.end() ? nullptr : &it->second;
}

static studio::PresetRegistry RegistryWith(const std::string& dir)
{
    studio::PresetRegistry reg;
    reg.LoadDirectory(dir, nullptr);
    return reg;
}

/* Build a candidate the way the loader does. */
static bool MakeCandidate(const json& j,
                          const studio::PresetRegistry& devices,
                          const studio::EffectRegistry& effects,
                          WorkspaceCandidate& out)
{
    std::vector<std::string> errors, warnings;
    if(!ParseWorkspaceCandidate(j, devices, effects, out,
                                errors, warnings))
    {
        if(!errors.empty())
        {
            std::printf("  (candidate parse: %s)\n",
                        errors.front().c_str());
        }
        return false;
    }
    return true;
}

static size_t CountGenerationDirs(const fs::path& ws)
{
    size_t n = 0;
    std::error_code ec;
    const fs::path gens = ws / "generations";
    if(!fs::is_directory(gens, ec))
    {
        return 0;
    }
    for(const auto& e : fs::directory_iterator(gens, ec))
    {
        if(e.is_directory() &&
           e.path().filename().string().rfind("gen-", 0) == 0)
        {
            ++n;
        }
    }
    return n;
}

/*---------------------------------------------------------*\
||| T1: commit -> immutable generation -> marker -> load;  ||
|||     a second commit bumps the revision and retains the ||
|||     prior generation                                   ||
\*---------------------------------------------------------*/
static void TestCommitAndLoad()
{
    const fs::path types = g_tmp / "types-a";
    fs::create_directories(types);
    WriteText((types / "mini-fan.device.json").string(),
              TypeJson("mini-fan", 8, "base"));

    WorkspaceStore::Deps deps;
    deps.devices = RegistryWith(types.string());
    CHECK(deps.devices.Contains("mini-fan"));

    const fs::path root = g_tmp / "ws1";
    WorkspaceStore store(root.string(), deps);

    /* Missing workspace: Empty, not an error. */
    WorkspaceStore::LoadResult lr = store.Load("desk");
    CHECK(lr.status == WorkspaceStore::LoadStatus::Empty);

    WorkspaceCandidate cand;
    CHECK(MakeCandidate(json::parse(WorkspaceJson("desk", "mini-fan")),
                        deps.devices, deps.effects, cand));

    WorkspaceStore::CommitResult cr =
        store.Commit("desk", cand, std::string(), 0);
    CHECK(cr.ok);
    if(!cr.ok)
    {
        std::printf("  (commit: %s)\n", cr.error.c_str());
    }
    CHECK(cr.generation == 1);
    CHECK(cr.workspace_revision == 1);
    CHECK(cr.resource_revision == 1);

    /* On-disk shape: immutable generation + marker. */
    const fs::path wsdir = root / "desk";
    CHECK(fs::exists(wsdir / "generations" / "gen-000001" /
                     "document.json"));
    CHECK(fs::exists(wsdir / "generations" / "gen-000001" /
                     "manifest.json"));
    CHECK(fs::exists(wsdir / "generations" / "gen-000001" /
                     "resources" / "devices" / "mini-fan.device.json"));
    CHECK(fs::exists(wsdir / "active"));

    lr = store.Load("desk");
    CHECK(lr.status == WorkspaceStore::LoadStatus::Loaded);
    CHECK(lr.candidate != nullptr);
    if(lr.candidate)
    {
        CHECK(lr.candidate->document.meta.name == "desk");
        const studio::DeviceInstance* fan =
            DevAt(lr.candidate->document, "fan");
        CHECK(fan != nullptr && fan->type == "mini-fan");
        CHECK(lr.candidate->resolved != nullptr);
        if(lr.candidate->resolved)
        {
            const studio::SceneObject* o = studio::FindObject(
                *lr.candidate->resolved, "fan/body");
            CHECK(o != nullptr);
            if(o)
            {
                CHECK(o->kind == studio::ObjectKind::Device);
                CHECK(o->binding == "b1");
                CHECK(o->verified);
                CHECK(o->emitters.size() == 8);
            }
        }
    }
    CHECK(lr.workspace_revision == 1);

    /* Second commit: revision-checked, generation 2 retained
       alongside generation 1. */
    WorkspaceCandidate cand2;
    CHECK(MakeCandidate(json::parse(WorkspaceJson("desk v2", "mini-fan")),
                        deps.devices, deps.effects, cand2));
    cr = store.Commit("desk", cand2, std::string(), 1);
    CHECK(cr.ok);
    CHECK(cr.generation == 2);
    CHECK(cr.workspace_revision == 2);

    lr = store.Load("desk");
    CHECK(lr.status == WorkspaceStore::LoadStatus::Loaded);
    CHECK(lr.generation == 2);
    CHECK(lr.workspace_revision == 2);
    CHECK(lr.candidate->document.meta.name == "desk v2");
    CHECK(CountGenerationDirs(wsdir) == 2);

    /* Stale writer rejected: expected 1 while current is 2. */
    WorkspaceStore::CommitResult stale =
        store.Commit("desk", cand2, std::string(), 1);
    CHECK(!stale.ok);
    CHECK(stale.error.find("revision") != std::string::npos);
    lr = store.Load("desk");
    CHECK(lr.generation == 2);

    /* Ids are a portable single-segment charset — path escapes
       are refused before any filesystem touch. */
    CHECK(!WorkspaceStore::IsWorkspaceId("../up"));
    CHECK(!WorkspaceStore::IsWorkspaceId("a/b"));
    CHECK(!WorkspaceStore::IsWorkspaceId(""));
    CHECK(WorkspaceStore::IsWorkspaceId("my-desk_01"));
}

/*---------------------------------------------------------*\
||| T2: resource_snapshot_survives_restart — the           ||
|||     generation's own resource snapshot resolves the    ||
|||     workspace after a restart whose base registry is   ||
|||     EMPTY (the library copy was never the only source) ||
\*---------------------------------------------------------*/
static void TestResourceSnapshotSurvivesRestart()
{
    const fs::path types = g_tmp / "types-b";
    fs::create_directories(types);
    WriteText((types / "widget.device.json").string(),
              TypeJson("widget", 4, "v1"));

    const fs::path root = g_tmp / "ws2";
    {
        WorkspaceStore::Deps deps;
        deps.devices = RegistryWith(types.string());
        WorkspaceStore store(root.string(), deps);

        WorkspaceCandidate cand;
        CHECK(MakeCandidate(
            json::parse(WorkspaceJson("widget desk", "widget")),
            deps.devices, deps.effects, cand));
        WorkspaceStore::CommitResult cr =
            store.Commit("desk", cand, std::string(), 0);
        CHECK(cr.ok);
    }

    /* "Restart": a fresh store over the same root with an EMPTY
       base registry — only the generation's resource snapshot can
       resolve the workspace. */
    {
        WorkspaceStore::Deps deps;   /* deliberately empty */
        WorkspaceStore store(root.string(), deps);
        WorkspaceStore::LoadResult lr = store.Load("desk");
        CHECK(lr.status == WorkspaceStore::LoadStatus::Loaded);
        CHECK(lr.candidate != nullptr && lr.candidate->resolved);
        if(lr.candidate)
        {
            const studio::DeviceInstance* fan =
                DevAt(lr.candidate->document, "fan");
            CHECK(fan != nullptr && fan->type == "widget");
            CHECK(lr.candidate->device_registry.Contains("widget"));
            const studio::SceneObject* o = studio::FindObject(
                *lr.candidate->resolved, "fan/body");
            CHECK(o != nullptr && o->emitters.size() == 4);
        }
    }
}

/*---------------------------------------------------------*\
||| T3: interruptions around the marker boundary           ||
|||     - torn staging + unmarked generation dirs are      ||
|||       orphans: ignored, and cleaned inside the root    ||
|||     - a completed generation survives a crash right    ||
|||       after the marker landed                          ||
\*---------------------------------------------------------*/
static void TestInterruptedCommits()
{
    const fs::path types = g_tmp / "types-c";
    fs::create_directories(types);
    WriteText((types / "mini-fan.device.json").string(),
              TypeJson("mini-fan", 8, "base"));

    WorkspaceStore::Deps deps;
    deps.devices = RegistryWith(types.string());
    const fs::path root = g_tmp / "ws3";
    WorkspaceStore store(root.string(), deps);
    const fs::path wsdir = root / "desk";

    WorkspaceCandidate cand;
    CHECK(MakeCandidate(json::parse(WorkspaceJson("one", "mini-fan")),
                        deps.devices, deps.effects, cand));
    CHECK(store.Commit("desk", cand, std::string(), 0).ok);

    /* Crash BEFORE marker replacement: a torn staging dir and a
       never-referenced generation dir are left behind. */
    fs::create_directories(wsdir / "staging" / "resources");
    WriteText((wsdir / "staging" / "document.json").string(),
              "{\"schema_version\":3,\"name\":\"torn\"");
    fs::create_directories(wsdir / "generations" / "gen-000007");
    WriteText((wsdir / "generations" / "gen-000007" /
               "document.json").string(),
              WorkspaceJson("orphan", "mini-fan"));
    /* No manifest, no marker update — the orphan must never be
       selected and is cleaned inside the managed root. */

    WorkspaceStore::LoadResult lr = store.Load("desk");
    CHECK(lr.status == WorkspaceStore::LoadStatus::Loaded);
    CHECK(lr.generation == 1);
    CHECK(lr.candidate->document.meta.name == "one");
    CHECK(!fs::exists(wsdir / "staging"));
    CHECK(!fs::exists(wsdir / "generations" / "gen-000007"));

    /* Crash AFTER marker replacement: commit generation 2, then
       drop a torn marker temp — the committed state is whole. */
    WorkspaceCandidate cand2;
    CHECK(MakeCandidate(json::parse(WorkspaceJson("two", "mini-fan")),
                        deps.devices, deps.effects, cand2));
    CHECK(store.Commit("desk", cand2, std::string(), 1).ok);
    WriteText((wsdir / "active.tmp").string(), "{\"partial\":");
    lr = store.Load("desk");
    CHECK(lr.status == WorkspaceStore::LoadStatus::Loaded);
    CHECK(lr.generation == 2);
    CHECK(lr.candidate->document.meta.name == "two");
    CHECK(!fs::exists(wsdir / "active.tmp"));

    /* The active generation's directory vanishing outright (or
       being incomplete) falls back to the last-valid marker. */
    fs::remove_all(wsdir / "generations" / "gen-000002");
    lr = store.Load("desk");
    CHECK(lr.status == WorkspaceStore::LoadStatus::Recovered);
    CHECK(lr.generation == 1);
    CHECK(!lr.error.empty());
}

/*---------------------------------------------------------*\
||| T4: invalid primary marker, valid last-known-good      ||
\*---------------------------------------------------------*/
static void TestCorruptMarkerRecovery()
{
    const fs::path types = g_tmp / "types-d";
    fs::create_directories(types);
    WriteText((types / "mini-fan.device.json").string(),
              TypeJson("mini-fan", 8, "base"));

    WorkspaceStore::Deps deps;
    deps.devices = RegistryWith(types.string());
    const fs::path root = g_tmp / "ws4";
    WorkspaceStore store(root.string(), deps);
    const fs::path wsdir = root / "desk";

    WorkspaceCandidate cand;
    CHECK(MakeCandidate(json::parse(WorkspaceJson("first", "mini-fan")),
                        deps.devices, deps.effects, cand));
    CHECK(store.Commit("desk", cand, std::string(), 0).ok);
    WorkspaceCandidate cand2;
    CHECK(MakeCandidate(json::parse(WorkspaceJson("second", "mini-fan")),
                        deps.devices, deps.effects, cand2));
    CHECK(store.Commit("desk", cand2, std::string(), 1).ok);
    CHECK(fs::exists(wsdir / "active.lkg"));

    /* Garbage primary marker -> recover the recorded last-valid
       generation, reported honestly. */
    WriteText((wsdir / "active").string(), "garbage{{{");
    WorkspaceStore::LoadResult lr = store.Load("desk");
    CHECK(lr.status == WorkspaceStore::LoadStatus::Recovered);
    CHECK(!lr.error.empty());
    CHECK(lr.generation == 1);
    CHECK(lr.candidate->document.meta.name == "first");

    /* Both markers corrupt -> honest failure, never a guessed
       blank workspace. */
    WriteText((wsdir / "active.lkg").string(), "also-bad");
    lr = store.Load("desk");
    CHECK(lr.status == WorkspaceStore::LoadStatus::Failed);
    CHECK(!lr.error.empty());
    CHECK(lr.candidate == nullptr);
}

/*---------------------------------------------------------*\
||| T5: denied writes — a read-only marker rejects the     ||
|||     commit and the prior generation stays usable       ||
\*---------------------------------------------------------*/
static void TestDeniedWrites()
{
    const fs::path types = g_tmp / "types-e";
    fs::create_directories(types);
    WriteText((types / "mini-fan.device.json").string(),
              TypeJson("mini-fan", 8, "base"));

    WorkspaceStore::Deps deps;
    deps.devices = RegistryWith(types.string());
    const fs::path root = g_tmp / "ws5";
    WorkspaceStore store(root.string(), deps);
    const fs::path wsdir = root / "desk";

    WorkspaceCandidate cand;
    CHECK(MakeCandidate(json::parse(WorkspaceJson("safe", "mini-fan")),
                        deps.devices, deps.effects, cand));
    CHECK(store.Commit("desk", cand, std::string(), 0).ok);

    /* Deny the marker replacement: the temp write may succeed but
       the rename over a read-only file must fail — and the
       previously committed generation must remain untouched. */
    const fs::path marker = wsdir / "active";
    std::error_code ec;
    fs::permissions(marker, fs::perms::owner_read,
                    fs::perm_options::replace, ec);
    CHECK(!ec);

    WorkspaceCandidate cand2;
    CHECK(MakeCandidate(json::parse(WorkspaceJson("blocked", "mini-fan")),
                        deps.devices, deps.effects, cand2));
    WorkspaceStore::CommitResult cr =
        store.Commit("desk", cand2, std::string(), 1);
    CHECK(!cr.ok);
    CHECK(!cr.error.empty());

    fs::permissions(marker, fs::perms::owner_all,
                    fs::perm_options::replace, ec);

    WorkspaceStore::LoadResult lr = store.Load("desk");
    CHECK(lr.status == WorkspaceStore::LoadStatus::Loaded);
    CHECK(lr.generation == 1);
    CHECK(lr.candidate->document.meta.name == "safe");

    /* The failed commit left no half-generation selected. */
    CHECK(CountGenerationDirs(wsdir) <= 2);
}

/*---------------------------------------------------------*\
||| T6: resource collision — an imported type colliding    ||
|||     with a different-content managed library file      ||
|||     lands under a suffixed id; references remap; the   ||
|||     local file is never overwritten                    ||
\*---------------------------------------------------------*/
static void TestResourceCollision()
{
    const fs::path root = g_tmp / "ws6";
    const fs::path lib  = root / "library" / "devices";
    fs::create_directories(lib);
    const std::string local_a = TypeJson("widget", 4, "LOCAL");
    WriteText((lib / "widget.device.json").string(), local_a);

    /* A candidate carrying an extracted same-id/different-content
       type — the shape a legacy/import parse produces. */
    WorkspaceCandidate cand;
    studio::StudioDocument& doc = cand.document;
    doc.meta.name = "imported";
    {
        studio::DeviceInstance inst;
        inst.type = "widget";
        doc.devices["w"] = inst;
    }
    studio::DevicePreset imported;
    {
        std::vector<std::string> perrs;
        const json tj = json::parse(TypeJson("widget", 4, "IMPORTED"));
        CHECK(studio::DevicePresetFromJson(tj, imported, &perrs));
    }
    cand.extracted_types.push_back(imported);
    cand.device_registry.Add(imported, nullptr);   /* scratch overlay */
    {
        auto resolved = std::make_shared<studio::SceneDocument>();
        std::vector<std::string> rerrs;
        CHECK(studio::ResolveScene(cand.document, cand.device_registry,
                                   *resolved, &rerrs));
        cand.resolved = std::move(resolved);
    }

    WorkspaceStore::Deps deps;   /* base registry knows no 'widget' */
    WorkspaceStore store(root.string(), deps);
    WorkspaceStore::CommitResult cr =
        store.Commit("desk", cand, std::string(), 0);
    CHECK(cr.ok);
    if(!cr.ok)
    {
        std::printf("  (collision commit: %s)\n", cr.error.c_str());
    }

    /* The local file is untouched; the import landed suffixed. */
    CHECK(ReadText((lib / "widget.device.json").string()) == local_a);
    CHECK(fs::exists(lib / "widget-2.device.json"));

    /* The committed document's reference was remapped. */
    CHECK(cr.candidate != nullptr);
    if(cr.candidate)
    {
        const studio::DeviceInstance* w =
            DevAt(cr.candidate->document, "w");
        CHECK(w != nullptr && w->type == "widget-2");
    }
    WorkspaceStore::LoadResult lr = store.Load("desk");
    CHECK(lr.status == WorkspaceStore::LoadStatus::Loaded);
    {
        const studio::DeviceInstance* w =
            DevAt(lr.candidate->document, "w");
        CHECK(w != nullptr && w->type == "widget-2");
    }
}

/*---------------------------------------------------------*\
||| T7: interrupted resource installation — a generation   ||
|||     whose manifest references a missing file is never  ||
|||     selected; the last-valid generation recovers       ||
\*---------------------------------------------------------*/
static void TestIncompleteGeneration()
{
    const fs::path types = g_tmp / "types-g";
    fs::create_directories(types);
    WriteText((types / "mini-fan.device.json").string(),
              TypeJson("mini-fan", 8, "base"));

    WorkspaceStore::Deps deps;
    deps.devices = RegistryWith(types.string());
    const fs::path root = g_tmp / "ws7";
    WorkspaceStore store(root.string(), deps);
    const fs::path wsdir = root / "desk";

    WorkspaceCandidate cand;
    CHECK(MakeCandidate(json::parse(WorkspaceJson("good", "mini-fan")),
                        deps.devices, deps.effects, cand));
    CHECK(store.Commit("desk", cand, std::string(), 0).ok);
    WorkspaceCandidate cand2;
    CHECK(MakeCandidate(json::parse(WorkspaceJson("better", "mini-fan")),
                        deps.devices, deps.effects, cand2));
    CHECK(store.Commit("desk", cand2, std::string(), 1).ok);

    /* Simulate an interrupted resource installation whose marker
       somehow advanced: remove a resource the gen-000002 manifest
       still references. The store must refuse it and recover the
       last-valid generation. */
    const fs::path res =
        wsdir / "generations" / "gen-000002" / "resources" /
        "devices" / "mini-fan.device.json";
    CHECK(fs::exists(res));
    fs::remove(res);

    WorkspaceStore::LoadResult lr = store.Load("desk");
    CHECK(lr.status == WorkspaceStore::LoadStatus::Recovered);
    CHECK(!lr.error.empty());
    CHECK(lr.generation == 1);
    CHECK(lr.candidate->document.meta.name == "good");
}

/*---------------------------------------------------------*\
||| T8: bounded generations — commits are retained to a    ||
|||     bound; the active + last-known-good generations    ||
|||     are never pruned                                   ||
\*---------------------------------------------------------*/
static void TestGenerationBound()
{
    const fs::path types = g_tmp / "types-h";
    fs::create_directories(types);
    WriteText((types / "mini-fan.device.json").string(),
              TypeJson("mini-fan", 8, "base"));

    WorkspaceStore::Deps deps;
    deps.devices = RegistryWith(types.string());
    const fs::path root = g_tmp / "ws8";
    WorkspaceStore store(root.string(), deps);
    const fs::path wsdir = root / "desk";

    const int total = (int)WorkspaceStore::kMaxGenerations + 4;
    for(int i = 0; i < total; ++i)
    {
        WorkspaceCandidate cand;
        CHECK(MakeCandidate(
            json::parse(WorkspaceJson("rev" + std::to_string(i),
                                      "mini-fan")),
            deps.devices, deps.effects, cand));
        CHECK(store.Commit("desk", cand, std::string(),
                           (uint64_t)i).ok);
    }

    CHECK(CountGenerationDirs(wsdir) <= WorkspaceStore::kMaxGenerations);
    WorkspaceStore::LoadResult lr = store.Load("desk");
    CHECK(lr.status == WorkspaceStore::LoadStatus::Loaded);
    CHECK(lr.generation == (uint64_t)total);
    CHECK(lr.workspace_revision == (uint64_t)total);
}

/*---------------------------------------------------------*\
||| T9: WorkspaceService — revision-checked jobs, imports  ||
|||     never arm, original bytes retained, publish seam   ||
\*---------------------------------------------------------*/
static void TestServiceSurface()
{
    const fs::path types = g_tmp / "types-i";
    fs::create_directories(types);
    WriteText((types / "mini-fan.device.json").string(),
              TypeJson("mini-fan", 8, "base"));

    WorkspaceService::Deps deps;
    deps.devices = RegistryWith(types.string());
    std::shared_ptr<const studio::SceneDocument> applied;
    deps.apply_scene =
        [&applied](std::shared_ptr<const studio::SceneDocument> doc,
                   std::string&) -> bool
        {
            applied = std::move(doc);
            return true;
        };

    const fs::path root = g_tmp / "ws9";
    WorkspaceService svc(root.string(), deps);

    /* Opening an absent workspace is a structured failure. */
    WorkspaceService::Result r = svc.Open("nope");
    CHECK(!r.ok);
    CHECK(!r.error.empty());

    /* Import a workspace that tries to arm everything — all of it
       must be stripped while identity hints survive review. */
    const json armed = json::object({
        { "schema_version", 3       },
        { "name",           "armed" },
        { "devices", json::object({
              { "fan", json::object({ { "type", "mini-fan" } }) } }) },
        { "bindings", json::object({
              { "b1", json::object({
                    { "controller_name", "Test Controller" },
                    { "serial",          "AA:BB:CC"        },
                    { "location",        "Wireless: AA"    },
                    { "zone_name",       "ring"            },
                    { "zone_leds",       8                 } }) } }) },
        { "device_settings", json::object({
              { "fan", json::object({
                    { "zones", json::object({
                          { "ring", json::object({
                                { "binding",  "b1"  },
                                { "verified", true  } }) } }) } }) } }) },
        { "inputs", json::object({
              { "audio",  true },
              { "keys",   true },
              { "screen", true } }) },
        { "output", json::object({
              { "live_on_startup", true } }) },
        { "effects", json::object({ { "playing", true } }) },
    });
    r = svc.Import("desk", armed, 0);
    CHECK(r.ok);
    if(!r.ok)
    {
        std::printf("  (import: %s)\n", r.error.c_str());
    }
    CHECK(r.committed);
    CHECK(!r.applied);   /* imports never auto-apply */
    CHECK(r.workspace_revision == 1);

    /* Sanitized at rest: output/inputs/ownership unarmed; the
       serial/location identity hints are retained for review. */
    r = svc.Open("desk");
    CHECK(r.ok);
    CHECK(svc.CurrentCandidate() != nullptr);
    if(svc.CurrentCandidate())
    {
        const studio::StudioDocument& d =
            svc.CurrentCandidate()->document;
        CHECK(!d.meta.live_on_startup);
        CHECK(!d.inputs.audio && !d.inputs.keys && !d.inputs.screen);
        const auto dsit = d.device_settings.find("fan");
        CHECK(dsit != d.device_settings.end());
        if(dsit != d.device_settings.end())
        {
            const auto zit = dsit->second.zones.find("ring");
            CHECK(zit != dsit->second.zones.end() &&
                  !zit->second.verified);
        }
        const auto bit = d.bindings.find("b1");
        CHECK(bit != d.bindings.end());
        if(bit != d.bindings.end())
        {
            CHECK(bit->second.serial == "AA:BB:CC");
            CHECK(bit->second.location == "Wireless: AA");
        }
        CHECK(d.effect.playing == false);
        /* The resolved scene is sanitized too — no verified
           emitters can reach hardware. */
        if(svc.CurrentCandidate()->resolved)
        {
            for(const studio::SceneObject& o :
                    svc.CurrentCandidate()->resolved->objects)
            {
                CHECK(!o.verified);
            }
        }
    }

    /* The original imported bytes are retained verbatim inside the
       generation for recovery. */
    const fs::path orig =
        root / "desk" / "generations" / "gen-000001" / "original.json";
    CHECK(fs::exists(orig));
    CHECK(ReadText(orig.string()) == armed.dump(2));

    /* Revision-checked save: the right revision commits and
       publishes the resolved scene; a stale one is refused. */
    WorkspaceCandidate save_cand;
    const json save_doc =
        json::parse(WorkspaceJson("saved", "mini-fan"));
    r = svc.Save("desk", save_doc, 999);   /* stale */
    CHECK(!r.ok);
    CHECK(r.error.find("revision") != std::string::npos
          || r.error.find("stale") != std::string::npos);
    CHECK(!r.committed);

    r = svc.Save("desk", save_doc, 1);
    CHECK(r.ok);
    CHECK(r.committed);
    CHECK(r.applied);
    CHECK(r.workspace_revision == 2);
    CHECK(applied != nullptr && applied->name == "saved");
    CHECK(svc.WorkspaceRevision() == 2);

    /* A stale revision still refuses after the commit. */
    r = svc.Save("desk", save_doc, 1);
    CHECK(!r.ok);
    CHECK(!r.committed);

    /* The on-disk active generation is the save. */
    WorkspaceStore::LoadResult lr = svc.Store().Load("desk");
    CHECK(lr.generation == 2);
    CHECK(lr.candidate->document.meta.name == "saved");
}

/*---------------------------------------------------------*\
||| T10: future generation documents are not rewritten —   ||
|||      a hand-edited newer schema_version in a stored    ||
|||      generation fails honestly and recovers, never a   ||
|||      silent downgrade                                  ||
\*---------------------------------------------------------*/
static void TestFutureGenerationNotRewritten()
{
    const fs::path types = g_tmp / "types-j";
    fs::create_directories(types);
    WriteText((types / "mini-fan.device.json").string(),
              TypeJson("mini-fan", 8, "base"));

    WorkspaceStore::Deps deps;
    deps.devices = RegistryWith(types.string());
    const fs::path root = g_tmp / "ws10";
    WorkspaceStore store(root.string(), deps);
    const fs::path wsdir = root / "desk";

    WorkspaceCandidate cand;
    CHECK(MakeCandidate(json::parse(WorkspaceJson("v1", "mini-fan")),
                        deps.devices, deps.effects, cand));
    CHECK(store.Commit("desk", cand, std::string(), 0).ok);

    /* Corrupt the active generation's document to a newer schema
       version — it must fail to select, recover gen 1 via the
       last-known-good marker... commit a second generation first
       so the marker/LKG pair is exercised. */
    WorkspaceCandidate cand2;
    CHECK(MakeCandidate(json::parse(WorkspaceJson("v2", "mini-fan")),
                        deps.devices, deps.effects, cand2));
    CHECK(store.Commit("desk", cand2, std::string(), 1).ok);

    const fs::path doc_path =
        wsdir / "generations" / "gen-000002" / "document.json";
    WriteText(doc_path.string(),
              "{\"schema_version\":99,\"name\":\"future\"}");
    WorkspaceStore::LoadResult lr = store.Load("desk");
    CHECK(lr.status == WorkspaceStore::LoadStatus::Recovered);
    CHECK(lr.generation == 1);
    CHECK(!lr.error.empty());
    /* The corrupt document was not rewritten in place. */
    CHECK(ReadText(doc_path.string()).find("\"schema_version\":99")
              != std::string::npos);
}

int main()
{
    g_tmp = fs::temp_directory_path() /
            "openrgb2_workspace_store_test";
    std::error_code ec;
    fs::remove_all(g_tmp, ec);
    fs::create_directories(g_tmp);

    TestCommitAndLoad();
    TestResourceSnapshotSurvivesRestart();
    TestInterruptedCommits();
    TestCorruptMarkerRecovery();
    TestDeniedWrites();
    TestResourceCollision();
    TestIncompleteGeneration();
    TestGenerationBound();
    TestServiceSurface();
    TestFutureGenerationNotRewritten();

    fs::remove_all(g_tmp, ec);
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
