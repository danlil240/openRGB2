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

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <process.h>
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
    /* Recovering through the backup must not destroy the newer,
       intact generation the damaged marker used to name. */
    CHECK(fs::exists(wsdir / "generations" / "gen-000002" /
                     "manifest.json"));

    /* Both markers corrupt -> honest failure, never a guessed
       blank workspace — and never a deleted one: every complete
       generation stays on disk for recovery. */
    WriteText((wsdir / "active.lkg").string(), "also-bad");
    lr = store.Load("desk");
    CHECK(lr.status == WorkspaceStore::LoadStatus::Failed);
    CHECK(!lr.error.empty());
    CHECK(lr.candidate == nullptr);
    CHECK(fs::exists(wsdir / "generations" / "gen-000001" /
                     "manifest.json"));
    CHECK(fs::exists(wsdir / "generations" / "gen-000002" /
                     "manifest.json"));
}

/*---------------------------------------------------------*\
||| T4b: a single-commit workspace whose only marker is      ||
|||      damaged keeps its only generation on disk          ||
\*---------------------------------------------------------*/
static void TestCorruptOnlyMarkerKeepsGeneration()
{
    const fs::path types = g_tmp / "types-d2";
    fs::create_directories(types);
    WriteText((types / "mini-fan.device.json").string(),
              TypeJson("mini-fan", 8, "base"));

    WorkspaceStore::Deps deps;
    deps.devices = RegistryWith(types.string());
    const fs::path root = g_tmp / "ws4b";
    WorkspaceStore store(root.string(), deps);
    const fs::path wsdir = root / "desk";

    WorkspaceCandidate cand;
    CHECK(MakeCandidate(json::parse(WorkspaceJson("only", "mini-fan")),
                        deps.devices, deps.effects, cand));
    CHECK(store.Commit("desk", cand, std::string(), 0).ok);
    CHECK(!fs::exists(wsdir / "active.lkg"));
    const std::string good_marker = ReadText((wsdir / "active").string());

    WriteText((wsdir / "active").string(), "garbage{{{");
    WorkspaceStore::LoadResult lr = store.Load("desk");
    CHECK(lr.status == WorkspaceStore::LoadStatus::Failed);
    CHECK(fs::exists(wsdir / "generations" / "gen-000001" /
                     "manifest.json"));

    /* Repairing the marker by hand brings the workspace back. */
    WriteText((wsdir / "active").string(), good_marker);
    lr = store.Load("desk");
    CHECK(lr.status == WorkspaceStore::LoadStatus::Loaded);
    CHECK(lr.candidate && lr.candidate->document.meta.name == "only");
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
    const size_t retained = CountGenerationDirs(wsdir);
    CHECK(retained > 2);
    WorkspaceStore::LoadResult lr = store.Load("desk");
    CHECK(lr.status == WorkspaceStore::LoadStatus::Loaded);
    CHECK(lr.generation == (uint64_t)total);
    CHECK(lr.workspace_revision == (uint64_t)total);
    /* Loading is read-mostly: the retained history survives it
       (pruning belongs to commit, not to load). */
    CHECK(CountGenerationDirs(wsdir) == retained);
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
        [&applied](std::shared_ptr<const WorkspaceCandidate> cand,
                   std::string&) -> bool
        {
            applied = cand ? cand->resolved : nullptr;
            return applied != nullptr;
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

/*---------------------------------------------------------*\
||| T11: InstallEffectResource (Task 6 resource_install) — ||
|||      revision-gated, collision-safe single-look        ||
|||      install: identical reuses, differing content      ||
|||      suffixes without overwriting, stale revisions and ||
|||      an unreadable marker refuse, and a failed install ||
|||      leaves the active workspace + library intact      ||
\*---------------------------------------------------------*/
static studio::EffectDocument EffectDoc(const std::string& id,
                                        const std::string& name,
                                        double speed)
{
    const nlohmann::ordered_json ej = nlohmann::ordered_json::object({
        { "schema_version", studio::EFFECT_SCHEMA_VERSION },
        { "id",             id                          },
        { "name",           name                        },
        { "layers", json::array({ json::object({
              { "primitive", "wave" },
              { "speed",     speed  } }) }) },
    });
    studio::EffectDocument d;
    std::vector<std::string> perrs;
    CHECK(studio::EffectDocumentFromJson(ej, d, &perrs));
    if(!perrs.empty())
    {
        std::printf("  (effect parse: %s)\n", perrs.front().c_str());
    }
    return d;
}

static void TestEffectResourceInstall()
{
    const fs::path types = g_tmp / "types-fx";
    fs::create_directories(types);
    WriteText((types / "mini-fan.device.json").string(),
              TypeJson("mini-fan", 8, "base"));

    WorkspaceStore::Deps deps;
    deps.devices = RegistryWith(types.string());
    const fs::path root = g_tmp / "ws11";
    WorkspaceStore store(root.string(), deps);
    const fs::path wsdir = root / "desk";
    const fs::path elib  = root / "library" / "effects";

    WorkspaceCandidate cand;
    CHECK(MakeCandidate(json::parse(WorkspaceJson("desk", "mini-fan")),
                        deps.devices, deps.effects, cand));
    WorkspaceStore::CommitResult cr =
        store.Commit("desk", cand, std::string(), 0);
    CHECK(cr.ok);                       /* ws_rev 1, res_rev 1 */

    /* No workspace at all — refused before touching disk. */
    {
        auto r = store.InstallEffectResource(
            "ghost", EffectDoc("look_a", "A", 1.0), 0, 0);
        CHECK(!r.ok);
        CHECK(!r.error.empty());
    }
    /* A bad id fails validation before any write. */
    {
        studio::EffectDocument bad;
        bad.id = "bad id!";
        auto r = store.InstallEffectResource("desk", bad, 1, 1);
        CHECK(!r.ok);
        CHECK(!fs::exists(elib / "bad id!.effect.json"));
    }
    /* Stale revisions refuse — both gates. */
    {
        auto r = store.InstallEffectResource(
            "desk", EffectDoc("look_a", "A", 1.0), 0, 1);
        CHECK(!r.ok);
        r = store.InstallEffectResource(
            "desk", EffectDoc("look_a", "A", 1.0), 1, 0);
        CHECK(!r.ok);
    }

    /* Clean install — resource_revision bumps, workspace
       revision + active generation untouched. */
    studio::EffectDocument look = EffectDoc("my_look", "Mine", 0.4);
    {
        auto r = store.InstallEffectResource("desk", look, 1, 1);
        if(!r.ok)
        {
            std::printf("  (install: %s)\n", r.error.c_str());
        }
        CHECK(r.ok);
        CHECK(r.landed_id == "my_look");
        CHECK(r.workspace_revision == 1);
        CHECK(r.resource_revision == 2);
        CHECK(!r.reused);
        CHECK(fs::exists(elib / "my_look.effect.json"));
        WorkspaceStore::LoadResult lr = store.Load("desk");
        CHECK(lr.status == WorkspaceStore::LoadStatus::Loaded);
        CHECK(lr.workspace_revision == 1);
        CHECK(lr.resource_revision == 2);
        /* The effective registry now resolves the landed look. */
        const studio::EffectRegistry reg =
            store.EffectiveEffectRegistry();
        CHECK(reg.Contains("my_look"));
    }

    /* Identical content reuses — no file write, no revision
       bump. */
    {
        auto r = store.InstallEffectResource("desk", look, 1, 2);
        CHECK(r.ok && r.reused);
        CHECK(r.landed_id == "my_look");
        CHECK(r.resource_revision == 2);
        CHECK(!fs::exists(elib / "my_look-2.effect.json"));
    }

    /* Different content under a taken id lands suffixed; the
       existing file's bytes are byte-identical afterwards. */
    const std::string original =
        ReadText((elib / "my_look.effect.json").string());
    {
        studio::EffectDocument clash =
            EffectDoc("my_look", "Different", 9.9);
        auto r = store.InstallEffectResource("desk", clash, 1, 2);
        CHECK(r.ok);
        CHECK(!r.reused);
        CHECK(r.landed_id == "my_look-2");
        CHECK(r.resource_revision == 3);
        CHECK(fs::exists(elib / "my_look-2.effect.json"));
        CHECK(ReadText((elib / "my_look.effect.json").string())
                  == original);            /* never overwritten */
        /* The suffixed file carries its own id. */
        std::vector<std::string> verrs;
        studio::EffectDocument landed;
        CHECK(studio::EffectDocumentFromJsonFile(
            (elib / "my_look-2.effect.json").string(),
            landed, &verrs));
        CHECK(landed.id == "my_look-2");
        /* The name the user gave survives the suffix. */
        CHECK(landed.name == "Different");
    }

    /* An unreadable marker refuses the install and leaves the
       library + last-good state intact. */
    WriteText((wsdir / "active").string(), "garbage{{{");
    WriteText((wsdir / "active.lkg").string(), "also-bad");
    {
        auto r = store.InstallEffectResource(
            "desk", EffectDoc("another", "B", 1.0), 1, 3);
        CHECK(!r.ok);
        CHECK(!r.error.empty());
        CHECK(!fs::exists(elib / "another.effect.json"));
        CHECK(ReadText((elib / "my_look.effect.json").string())
                  == original);
        WorkspaceStore::LoadResult lr = store.Load("desk");
        CHECK(lr.status == WorkspaceStore::LoadStatus::Failed);
    }
}

/*---------------------------------------------------------*\
|||| T10a: ExternalScan — the passive active-generation     ||
|||| fingerprint (studio-import Task 10, ported from the    ||
|||| source ConfigStore external-change cases).             ||
|||| Same-content rewrites reproduce the signature          ||
|||| (suppression); replacement changes it; removal sets    ||
|||| `missing`; the scan never mutates.                     ||
\*---------------------------------------------------------*/
static void TestExternalScan()
{
    const fs::path types = g_tmp / "types-t10a";
    fs::create_directories(types);
    WriteText((types / "mini-fan.device.json").string(),
              TypeJson("mini-fan", 8, "base"));

    WorkspaceStore::Deps deps;
    deps.devices = RegistryWith(types.string());
    const fs::path root = g_tmp / "ws-ext";
    WorkspaceStore store(root.string(), deps);
    const fs::path wsdir = root / "desk";

    /* Nothing managed — not watched, not missing. */
    {
        const WorkspaceStore::ExternalFingerprint fp =
            store.ExternalScan("desk");
        CHECK(!fp.watched);
        CHECK(!fp.missing);
        CHECK(!store.ExternalScan("../bad").watched);
    }

    WorkspaceCandidate cand;
    CHECK(MakeCandidate(json::parse(WorkspaceJson("disk-a", "mini-fan")),
                        deps.devices, deps.effects, cand));
    CHECK(store.Commit("desk", cand, std::string(), 0).ok);

    const WorkspaceStore::ExternalFingerprint base =
        store.ExternalScan("desk");
    CHECK(base.watched);
    CHECK(!base.missing);
    CHECK(!base.signature.empty());

    /* Same-content rewrite — identical bytes reproduce the
       identical signature (the source's same-content watcher
       suppression). */
    const fs::path doc = wsdir / "generations" / "gen-000001" /
                         "document.json";
    const std::string orig = ReadText(doc.string());
    WriteText(doc.string(), orig);
    {
        const WorkspaceStore::ExternalFingerprint fp =
            store.ExternalScan("desk");
        CHECK(fp.watched);
        CHECK(fp.signature == base.signature);
        CHECK(!fp.missing);
    }

    /* Content replacement — a different document flips the
       signature; nothing is missing. The scan is READ-ONLY:
       the file's own bytes are untouched afterwards. */
    WriteText(doc.string(), orig + "\n/* outside edit */");
    {
        const WorkspaceStore::ExternalFingerprint fp =
            store.ExternalScan("desk");
        CHECK(fp.signature != base.signature);
        CHECK(!fp.missing);
    }
    WriteText(doc.string(), orig);
    {
        const WorkspaceStore::ExternalFingerprint fp =
            store.ExternalScan("desk");
        CHECK(fp.signature == base.signature);   /* restore suppresses */
    }

    /* Removal sets missing + names what vanished. */
    fs::remove(doc);
    {
        const WorkspaceStore::ExternalFingerprint fp =
            store.ExternalScan("desk");
        CHECK(fp.signature != base.signature);
        CHECK(fp.missing);
        CHECK(fp.detail.find("document.json") !=
              std::string::npos);
    }
    WriteText(doc.string(), orig);

    /* Marker deletion — the top-level "deleted" case. */
    const std::string marker = ReadText((wsdir / "active").string());
    fs::remove(wsdir / "active");
    {
        const WorkspaceStore::ExternalFingerprint fp =
            store.ExternalScan("desk");
        CHECK(fp.missing);
        CHECK(fp.detail.find("marker") != std::string::npos);
    }
    WriteText((wsdir / "active").string(), marker);

    /* Corrupt marker — modified (replaced), not missing. */
    WriteText((wsdir / "active").string(), "garbage{{{");
    {
        const WorkspaceStore::ExternalFingerprint fp =
            store.ExternalScan("desk");
        CHECK(fp.signature != base.signature);
        CHECK(!fp.missing);
        CHECK(fp.detail.find("corrupt") != std::string::npos ||
              fp.detail.find("replaced") != std::string::npos);
    }
    WriteText((wsdir / "active").string(), marker);
    {
        const WorkspaceStore::ExternalFingerprint fp =
            store.ExternalScan("desk");
        CHECK(fp.signature == base.signature);
    }
    /* The scan left everything in place. */
    CHECK(store.Load("desk").status ==
          WorkspaceStore::LoadStatus::Loaded);
}

/*---------------------------------------------------------*\
|||| T10b: LoadBackup — Restore Backup's source. The       ||
|||| marker backup's generation loads honestly; no backup   ||
|||| refuses; a torn primary still reads the backup.        ||
\*---------------------------------------------------------*/
static void TestLoadBackup()
{
    const fs::path types = g_tmp / "types-t10b";
    fs::create_directories(types);
    WriteText((types / "mini-fan.device.json").string(),
              TypeJson("mini-fan", 8, "base"));

    WorkspaceStore::Deps deps;
    deps.devices = RegistryWith(types.string());
    const fs::path root = g_tmp / "ws-backup";
    WorkspaceStore store(root.string(), deps);
    const fs::path wsdir = root / "desk";

    WorkspaceCandidate cand;
    CHECK(MakeCandidate(json::parse(WorkspaceJson("gen-one", "mini-fan")),
                        deps.devices, deps.effects, cand));
    CHECK(store.Commit("desk", cand, std::string(), 0).ok);

    /* One commit — no backup exists yet. */
    CHECK(store.LoadBackup("desk").status ==
          WorkspaceStore::LoadStatus::Failed);

    WorkspaceCandidate cand2;
    CHECK(MakeCandidate(json::parse(WorkspaceJson("gen-two", "mini-fan")),
                        deps.devices, deps.effects, cand2));
    CHECK(store.Commit("desk", cand2, std::string(), 1).ok);

    /* The backup names the PREVIOUS generation — the
       last-known-good the commit boundary saved. */
    WorkspaceStore::LoadResult lr = store.LoadBackup("desk");
    CHECK(lr.status == WorkspaceStore::LoadStatus::Loaded);
    CHECK(lr.generation == 1);
    CHECK(lr.workspace_revision == 1);
    CHECK(lr.candidate != nullptr);
    if(lr.candidate)
    {
        CHECK(lr.candidate->document.meta.name == "gen-one");
    }

    /* A torn primary doesn't disturb the backup read. */
    const std::string marker_bytes =
        ReadText((wsdir / "active").string());
    WriteText((wsdir / "active").string(), "garbage{{{");
    lr = store.LoadBackup("desk");
    CHECK(lr.status == WorkspaceStore::LoadStatus::Loaded);
    CHECK(lr.generation == 1);

    /* Restore via the service commits the backup FORWARD as
       a new generation — never write-in-place. */
    WriteText((wsdir / "active").string(), marker_bytes);
    WorkspaceService::Deps sdeps;
    sdeps.devices = deps.devices;
    WorkspaceService svc(root.string(), sdeps);
    CHECK(svc.Open("desk").ok);
    /* The revision pair is read from the durable marker —
       doc-only commits may leave resource_revision at 1. */
    const WorkspaceStore::Revisions rev =
        store.LoadRevisions("desk");
    CHECK(rev.exists);
    CHECK(rev.workspace_revision == 2);
    WorkspaceService::Result rr =
        svc.RestoreBackup("desk", rev.workspace_revision,
                          rev.resource_revision);
    CHECK(rr.ok);
    CHECK(rr.committed);
    CHECK(rr.recovered);
    CHECK(rr.generation == 3);
    WorkspaceStore::LoadResult after = store.Load("desk");
    CHECK(after.status == WorkspaceStore::LoadStatus::Loaded);
    CHECK(after.generation == 3);
    CHECK(after.candidate->document.meta.name == "gen-one");

    /* A stale caller is refused — the revision gate is the
       durable marker's, not the enqueued snapshot's. */
    rr = svc.RestoreBackup("desk", rev.workspace_revision,
                           rev.resource_revision);
    CHECK(!rr.ok);
    CHECK(rr.detail == "revision_conflict");
    /* ...and nothing managed under an id refuses honestly. */
    rr = svc.RestoreBackup("ghost", 0, 0);
    CHECK(!rr.ok);
}

/*---------------------------------------------------------*\
|||| T10c: future-schema refusal (ported source case): a    ||
|||| marker or document declaring a NEWER schema version    ||
|||| is never guessed/migrated — the store reports the      ||
|||| honest failure (or recovers the last-valid gen).       ||
\*---------------------------------------------------------*/
static void TestFutureSchemaRefusal()
{
    const fs::path types = g_tmp / "types-t10c";
    fs::create_directories(types);
    WriteText((types / "mini-fan.device.json").string(),
              TypeJson("mini-fan", 8, "base"));

    WorkspaceStore::Deps deps;
    deps.devices = RegistryWith(types.string());
    const fs::path root = g_tmp / "ws-future";
    WorkspaceStore store(root.string(), deps);
    const fs::path wsdir = root / "desk";

    WorkspaceCandidate cand;
    CHECK(MakeCandidate(json::parse(WorkspaceJson("v3", "mini-fan")),
                        deps.devices, deps.effects, cand));
    CHECK(store.Commit("desk", cand, std::string(), 0).ok);
    WorkspaceCandidate cand2;
    CHECK(MakeCandidate(json::parse(WorkspaceJson("v3-b", "mini-fan")),
                        deps.devices, deps.effects, cand2));
    CHECK(store.Commit("desk", cand2, std::string(), 1).ok);

    /* A future-version MARKER refuses — the lkg backup
       recovers the last-valid generation. */
    const fs::path marker = wsdir / "active";
    json mj = json::parse(ReadText(marker.string()));
    mj["schema_version"] = 99;
    WriteText(marker.string(), mj.dump(2));
    WorkspaceStore::LoadResult lr = store.Load("desk");
    CHECK(lr.status == WorkspaceStore::LoadStatus::Recovered);
    CHECK(lr.generation == 1);   /* the backup's gen */

    /* Restore a sane marker; then a future-version DOCUMENT
       (manifest hash re-pinned so the refusal is the SCHEMA,
       not the manifest) — the generation is unusable and the
       last-valid generation recovers. */
    WriteText(marker.string(),
              ReadText((wsdir / "active.lkg").string()));
    const fs::path gendir = wsdir / "generations" / "gen-000001";
    const fs::path doc = gendir / "document.json";
    json dj = json::parse(ReadText(doc.string()));
    dj["schema_version"] = 99;
    const std::string dbytes = dj.dump(2);
    WriteText(doc.string(), dbytes);
    /* Re-pin the manifest entry so manifest verification
       passes and the SCHEMA refusal is what fails. */
    const fs::path mpath = gendir / "manifest.json";
    json man = json::parse(ReadText(mpath.string()));
    uint64_t h = 1469598103934665603ull;
    for(unsigned char c : dbytes)
    {
        h ^= c;
        h *= 1099511628211ull;
    }
    char buf[32];
    std::snprintf(buf, sizeof(buf), "fnv1a64:%016llx",
                  (unsigned long long)h);
    for(auto& e : man["files"])
    {
        if(e.value("file", std::string()) == "document.json")
        {
            e["sha"]   = buf;
            e["bytes"] = (uint64_t)dbytes.size();
        }
    }
    WriteText(mpath.string(), man.dump(2));
    lr = store.Load("desk");
    CHECK(lr.status != WorkspaceStore::LoadStatus::Loaded);
    CHECK(lr.candidate == nullptr ||
          lr.candidate->document.meta.name != "");
    /* The corrupt-forward workspace never loads gen-000001's
       document as truth — either honest recovery or an honest
       failure, never a guessed schema. */
    CHECK(!lr.error.empty());
}

/*---------------------------------------------------------*\
|||| T10d: failed save -> retry (ported source case:        ||
|||| "migration retry after failed save"). A denied write   ||
|||| fails the commit and leaves the last generation        ||
|||| intact; the RETRY commits cleanly on the same base.    ||
\*---------------------------------------------------------*/
static void TestFailedCommitRetry()
{
    const fs::path types = g_tmp / "types-t10d";
    fs::create_directories(types);
    WriteText((types / "mini-fan.device.json").string(),
              TypeJson("mini-fan", 8, "base"));

    WorkspaceStore::Deps deps;
    deps.devices = RegistryWith(types.string());
    const fs::path root = g_tmp / "ws-retry";
    WorkspaceStore store(root.string(), deps);
    const fs::path wsdir = root / "desk";

    WorkspaceCandidate cand;
    CHECK(MakeCandidate(json::parse(WorkspaceJson("first", "mini-fan")),
                        deps.devices, deps.effects, cand));
    CHECK(store.Commit("desk", cand, std::string(), 0).ok);

    /* Deny the marker replace — the save fails, prior state
       intact. */
    const fs::path marker = wsdir / "active";
    std::error_code ec;
    fs::permissions(marker, fs::perms::owner_read,
                    fs::perm_options::replace, ec);
    CHECK(!ec);
    WorkspaceCandidate cand2;
    CHECK(MakeCandidate(json::parse(WorkspaceJson("second", "mini-fan")),
                        deps.devices, deps.effects, cand2));
    CHECK(!store.Commit("desk", cand2, std::string(), 1).ok);
    WorkspaceStore::LoadResult lr = store.Load("desk");
    CHECK(lr.status == WorkspaceStore::LoadStatus::Loaded);
    CHECK(lr.candidate->document.meta.name == "first");

    /* The retry — same base revision — lands cleanly. */
    fs::permissions(marker, fs::perms::owner_all,
                    fs::perm_options::replace, ec);
    CHECK(!ec);
    CHECK(store.Commit("desk", cand2, std::string(), 1).ok);
    lr = store.Load("desk");
    CHECK(lr.status == WorkspaceStore::LoadStatus::Loaded);
    /* The failed attempt's published-but-unreferenced generation
       is kept (ids never recycle), so the retry takes the next
       ordinal; the user-visible revision is what must be 2. */
    CHECK(lr.workspace_revision == 2);
    CHECK(lr.generation >= 2);
    CHECK(lr.candidate->document.meta.name == "second");
}

/*---------------------------------------------------------*\
|||| I-11: a wrong-typed manifest field never throws out of  ||
|||| Load. Every damaged-newest-generation variant must fall ||
|||| back to the marker backup (Recovered) — before the fix  ||
|||| nlohmann's value()/type checks threw type_error, and    ||
|||| startup re-adoption had no catch: a crash every launch. ||
\*---------------------------------------------------------*/
static void TestWrongTypedManifestNeverThrows()
{
    const fs::path types = g_tmp / "types-i11";
    fs::create_directories(types);
    WriteText((types / "mini-fan.device.json").string(),
              TypeJson("mini-fan", 8, "base"));

    WorkspaceStore::Deps deps;
    deps.devices = RegistryWith(types.string());
    const fs::path root = g_tmp / "ws-i11";
    WorkspaceStore store(root.string(), deps);
    const fs::path wsdir = root / "desk";

    WorkspaceCandidate cand;
    CHECK(MakeCandidate(json::parse(WorkspaceJson("one", "mini-fan")),
                        deps.devices, deps.effects, cand));
    CHECK(store.Commit("desk", cand, std::string(), 0).ok);
    WorkspaceCandidate cand2;
    CHECK(MakeCandidate(json::parse(WorkspaceJson("two", "mini-fan")),
                        deps.devices, deps.effects, cand2));
    CHECK(store.Commit("desk", cand2, std::string(), 1).ok);

    const fs::path mpath =
        wsdir / "generations" / "gen-000002" / "manifest.json";
    const std::string good = ReadText(mpath.string());
    const json gj = json::parse(good);
    CHECK(gj.is_object());

    struct Variant
    {
        const char* what;
        json        patch;    /* merged over the good manifest */
    };
    const std::vector<Variant> variants = {
        { "kind: number",           {{ "kind", 5 }} },
        { "kind: array",            {{ "kind", json::array() }} },
        { "schema_version: string", {{ "schema_version", "1" }} },
        { "source_form: number",    {{ "source_form", 7 }} },
        { "document: array",        {{ "document", json::array() }} },
        { "document.file: number",  {{ "document", {
              { "file", 5 }, { "sha", "x" } } }} },
        { "document.sha: object",   {{ "document", {
              { "file", "document.json" },
              { "sha", json::object() } } }} },
        { "original.file: array",   {{ "original", {
              { "file", json::array() }, { "sha", "x" } } }} },
        { "resources entry: string", {{ "resources", {
              { "devices", {{ "mini-fan", "not-an-entry" }} } } }} },
        { "resources entry.file: bool", {{ "resources", {
              { "devices", {{ "mini-fan", {
                    { "file", true }, { "sha", "x" } } }} } } }} },
    };
    for(const Variant& v : variants)
    {
        json m = gj;
        m.merge_patch(v.patch);
        WriteText(mpath.string(), m.dump(2));
        bool threw = false;
        WorkspaceStore::LoadResult lr;
        try
        {
            lr = store.Load("desk");
        }
        catch(const std::exception& e)
        {
            threw = true;
            std::printf("  (%s: Load threw %s)\n", v.what, e.what());
        }
        CHECK(!threw);
        CHECK(lr.status == WorkspaceStore::LoadStatus::Recovered);
        CHECK(lr.generation == 1);
        CHECK(!lr.error.empty());
        if(lr.status != WorkspaceStore::LoadStatus::Recovered)
        {
            std::printf("  (%s: status %d, %s)\n", v.what,
                        (int)lr.status, lr.error.c_str());
        }

        /* The service surface (the startup re-adopt path) must
           not throw either. */
        WorkspaceService::Deps sdeps;
        sdeps.devices = deps.devices;
        WorkspaceService svc(root.string(), sdeps);
        threw = false;
        try
        {
            const WorkspaceService::Result r = svc.Open("desk");
            CHECK(r.ok && r.recovered);
        }
        catch(const std::exception& e)
        {
            threw = true;
            std::printf("  (%s: Open threw %s)\n", v.what, e.what());
        }
        CHECK(!threw);
    }
    WriteText(mpath.string(), good);
    const WorkspaceStore::LoadResult lr = store.Load("desk");
    CHECK(lr.status == WorkspaceStore::LoadStatus::Loaded);
    CHECK(lr.generation == 2);
}

/*---------------------------------------------------------*\n|||| I-13: a commit that fails AFTER its library install     ||
|||| leaves no library files behind. Commit installs the     ||
|||| candidate's extracted types into the shared managed     ||
|||| library first; a later failure (here: the generations   ||
|||| dir cannot be created) used to leave them installed,    ||
|||| visible in every catalog though nothing committed.      ||
\*---------------------------------------------------------*/
static void TestFailedCommitLeavesNoLibraryFiles()
{
    const fs::path root = g_tmp / "ws-i13-lib";
    const fs::path lib  = root / "library" / "devices";
    const fs::path wsdir = root / "desk";

    WorkspaceCandidate cand;
    cand.document.meta.name = "gizmo-desk";
    {
        studio::DeviceInstance inst;
        inst.type = "gizmo";
        cand.document.devices["g"] = inst;
    }
    studio::DevicePreset gizmo;
    {
        std::vector<std::string> perrs;
        const json tj = json::parse(TypeJson("gizmo", 4, "NEW"));
        CHECK(studio::DevicePresetFromJson(tj, gizmo, &perrs));
    }
    cand.extracted_types.push_back(gizmo);
    cand.device_registry.Add(gizmo, nullptr);
    {
        auto resolved = std::make_shared<studio::SceneDocument>();
        std::vector<std::string> rerrs;
        CHECK(studio::ResolveScene(cand.document, cand.device_registry,
                                   *resolved, &rerrs));
        cand.resolved = std::move(resolved);
    }

    WorkspaceStore::Deps deps;   /* base registry knows no 'gizmo' */
    WorkspaceStore store(root.string(), deps);

    /* Block the publish step: a FILE where the generations dir
       must be created. */
    fs::create_directories(wsdir);
    WriteText((wsdir / "generations").string(), "not a dir");
    WorkspaceStore::CommitResult cr =
        store.Commit("desk", cand, std::string(), 0);
    CHECK(!cr.ok);
    CHECK(!fs::exists(lib / "gizmo.device.json"));
    if(fs::exists(lib / "gizmo.device.json"))
    {
        std::printf("  (failed commit left library/devices/"
                    "gizmo.device.json: %s)\n", cr.error.c_str());
    }

    /* Unblocked, the same commit lands the type normally. */
    std::error_code ec;
    fs::remove(wsdir / "generations", ec);
    cr = store.Commit("desk", cand, std::string(), 0);
    CHECK(cr.ok);
    CHECK(fs::exists(lib / "gizmo.device.json"));
}

/*---------------------------------------------------------*\n|||| I-13: workspace ids that Windows cannot hold as a      ||
|||| directory (CON, NUL, COM1, ...) or that alias the      ||
|||| store's own dirs (library/assets/transfers, any case)  ||
|||| are refused; an id differing from an existing one only ||
|||| in letter case never writes into (or reads) that       ||
|||| workspace — NTFS would resolve both to one directory.  ||
\*---------------------------------------------------------*/
static void TestWorkspaceIdRules()
{
    for(const char* bad : { "CON", "con", "Nul", "PRN", "aux",
                            "COM1", "com9", "LPT1", "lpt9",
                            "library", "Library", "ASSETS",
                            "transfers", "Transfers" })
    {
        CHECK(!WorkspaceStore::IsWorkspaceId(bad));
        if(WorkspaceStore::IsWorkspaceId(bad))
        {
            std::printf("  (accepted reserved id '%s')\n", bad);
        }
    }
    for(const char* ok : { "desk", "Desk-2", "console", "com10",
                           "lpt", "nul_1", "my-assets" })
    {
        CHECK(WorkspaceStore::IsWorkspaceId(ok));
    }

    const fs::path types = g_tmp / "types-i13";
    fs::create_directories(types);
    WriteText((types / "mini-fan.device.json").string(),
              TypeJson("mini-fan", 8, "base"));
    WorkspaceStore::Deps deps;
    deps.devices = RegistryWith(types.string());
    const fs::path root = g_tmp / "ws-i13";
    WorkspaceStore store(root.string(), deps);

    WorkspaceCandidate cand;
    CHECK(MakeCandidate(json::parse(WorkspaceJson("lower", "mini-fan")),
                        deps.devices, deps.effects, cand));
    CHECK(store.Commit("desk", cand, std::string(), 0).ok);

    /* "Desk" at desk's revision would commit INTO desk. */
    WorkspaceCandidate cand2;
    CHECK(MakeCandidate(json::parse(WorkspaceJson("upper", "mini-fan")),
                        deps.devices, deps.effects, cand2));
    const WorkspaceStore::CommitResult cr =
        store.Commit("Desk", cand2, std::string(), 1);
    CHECK(!cr.ok);
    CHECK(cr.error.find("case") != std::string::npos);
    const WorkspaceStore::LoadResult lr = store.Load("desk");
    CHECK(lr.status == WorkspaceStore::LoadStatus::Loaded);
    CHECK(lr.workspace_revision == 1);
    CHECK(lr.candidate &&
          lr.candidate->document.meta.name == "lower");
    /* Reads under the other case do not alias desk either. */
    const WorkspaceStore::LoadResult la = store.Load("DESK");
    CHECK(la.status != WorkspaceStore::LoadStatus::Loaded);
    CHECK(la.candidate == nullptr);
    CHECK(!store.LoadRevisions("DESK").exists);
    const std::vector<std::string> ids = store.List();
    CHECK(ids.size() == 1 && ids[0] == "desk");
}

/*---------------------------------------------------------*\
|||| I-6: the `get` poll is metadata-only. The published     ||
|||| snapshot carries the current document's bytes (built    ||
|||| by the committing thread, not by the reader), and the   ||
|||| external-change check re-reads + hashes the tracked     ||
|||| files only when their size/write-time stamp moved —    ||
|||| a recently-written ("racy") stamp always rescans.      ||
\*---------------------------------------------------------*/
static void BackdateTree(const fs::path& dir, int seconds)
{
    std::error_code ec;
    const auto t = fs::file_time_type::clock::now() -
                   std::chrono::seconds(seconds);
    for(fs::recursive_directory_iterator it(dir, ec), end;
        !ec && it != end; it.increment(ec))
    {
        if(it->is_regular_file(ec))
        {
            fs::last_write_time(it->path(), t, ec);
        }
    }
}

static void TestPollIsMetadataOnly()
{
    const fs::path types = g_tmp / "types-i6";
    fs::create_directories(types);
    WriteText((types / "mini-fan.device.json").string(),
              TypeJson("mini-fan", 8, "base"));
    WorkspaceStore::Deps deps;
    deps.devices = RegistryWith(types.string());
    const fs::path root = g_tmp / "ws-i6";
    {
        WorkspaceStore store(root.string(), deps);
        WorkspaceCandidate cand;
        CHECK(MakeCandidate(
            json::parse(WorkspaceJson("poll-a", "mini-fan")),
            deps.devices, deps.effects, cand));
        CHECK(store.Commit("desk", cand, std::string(), 0).ok);
    }
    const fs::path wsdir = root / "desk";
    BackdateTree(wsdir, 30);

    WorkspaceService::Deps sdeps;
    sdeps.devices = deps.devices;
    WorkspaceService svc(root.string(), sdeps);
    CHECK(svc.Open("desk").ok);
    BackdateTree(wsdir, 30);

    /* The adopt published the document bytes — the same bytes
       DocumentBytes (download / content_sha256) serves. */
    {
        const WorkspaceService::PublishedState ps = svc.Snapshot();
        CHECK(ps.document_bytes != nullptr);
        std::string direct, derr;
        CHECK(svc.DocumentBytes("desk", direct, derr));
        CHECK(ps.document_bytes && *ps.document_bytes == direct);
        CHECK(!direct.empty() && direct.front() == '{');
    }

    /* Quiet poll: one full scan, then stamp hits. */
    const uint64_t scans0 = svc.ExternalFullScans();
    for(int i = 0; i < 5; ++i)
    {
        CHECK(svc.CheckExternal().state == "none");
    }
    CHECK(svc.ExternalFullScans() - scans0 == 1);

    /* An outside edit moves the stamp: rescanned once, then
       the "modified" answer is served from the stamp. */
    const fs::path doc = wsdir / "generations" / "gen-000001" /
                         "document.json";
    const std::string orig = ReadText(doc.string());
    WriteText(doc.string(), orig + "\n/* outside edit */");
    BackdateTree(wsdir, 20);
    CHECK(svc.CheckExternal().state == "modified");
    CHECK(svc.CheckExternal().state == "modified");
    CHECK(svc.ExternalFullScans() - scans0 == 2);

    /* Restored bytes: the stamp moved again -> rescan ->
       same-content suppression reports "none". */
    WriteText(doc.string(), orig);
    BackdateTree(wsdir, 10);
    CHECK(svc.CheckExternal().state == "none");
    CHECK(svc.ExternalFullScans() - scans0 == 3);

    /* Racy: a same-size rewrite that just happened is never
       trusted to the stamp — every poll rescans until the
       write ages out, so it cannot hide. */
    std::string flipped = orig;
    flipped[0] = flipped[0] == ' ' ? '\t' : ' ';
    WriteText(doc.string(), flipped);
    CHECK(svc.CheckExternal().state == "modified");
    CHECK(svc.CheckExternal().state == "modified");
    CHECK(svc.ExternalFullScans() - scans0 == 5);
    WriteText(doc.string(), orig);
    CHECK(svc.CheckExternal().state == "none");
}

int main()
{
    g_tmp = fs::temp_directory_path() /
            ("openrgb2_workspace_store_test_" + std::to_string(_getpid()));
    std::error_code ec;
    fs::remove_all(g_tmp, ec);
    fs::create_directories(g_tmp);

    TestCommitAndLoad();
    TestResourceSnapshotSurvivesRestart();
    TestInterruptedCommits();
    TestCorruptMarkerRecovery();
    TestCorruptOnlyMarkerKeepsGeneration();
    TestDeniedWrites();
    TestResourceCollision();
    TestIncompleteGeneration();
    TestGenerationBound();
    TestServiceSurface();
    TestFutureGenerationNotRewritten();
    TestEffectResourceInstall();
    /* studio-import Task 10 */
    TestExternalScan();
    TestLoadBackup();
    TestFutureSchemaRefusal();
    TestFailedCommitRetry();
    TestWrongTypedManifestNeverThrows();
    TestFailedCommitLeavesNoLibraryFiles();
    TestWorkspaceIdRules();
    TestPollIsMetadataOnly();

    fs::remove_all(g_tmp, ec);
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
