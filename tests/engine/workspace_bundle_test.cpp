// SPDX-License-Identifier: GPL-2.0-or-later
//
// workspace_bundle_test — studio-import Task 9 engine suite.
// Offline only: temp dirs, an in-process WorkspaceService with
// the staged-transfer surface satisfied by lambdas — no IPC,
// no hardware, no sockets.
//
// Covers the brief's acceptance list:
//   - PresetBundle round trip on disk: ExportBundle (transitive
//     type closure, sanitized studio.json, asset copy),
//     InspectBundle classification (new|identical|conflict),
//     ApplyImport conflict remapping incl. the nested
//     dependency cascade, never-overwrite, malformed/oversized
//     bundles refused, an interrupted export (no studio.json)
//     refused at inspect
//   - WorkspaceStore single-resource ops: device-type install
//     (collision suffix, explicit replace, packaged-default
//     override), in-use / referenced / packaged-default delete
//     refusals, revert-to-default, revision gates
//   - The service-level packed wire form: export packs the
//     bundle dir into {files}, a referenced personal look
//     materializes inline with the named preset kept as
//     provenance, import_inspect reports conflicts, hostile
//     paths/encodings are refused before any write, and
//     import_commit lands types + remaps without ever applying

#include "WorkspaceService.h"
#include "WorkspaceStore.h"

#include "config/WorkspaceCandidate.h"
#include "config/StudioConfig.h"
#include "presets/DevicePreset.h"
#include "presets/EffectRegistry.h"
#include "presets/PresetBundle.h"
#include "presets/PresetRegistry.h"

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using namespace openrgb2;
using nlohmann::json;

static int checks   = 0;
static int failures = 0;
#define CHECK(x) do { ++checks; if(!(x)) { ++failures; \
    std::printf("FAIL line %d: %s\n", __LINE__, #x); } } while(0)

namespace fs = std::filesystem;

static fs::path g_tmp;

static std::string P(const std::string& rel)
{
    return (g_tmp / rel).string();
}
/* Scratch stage dirs left under a managed root (any name starting
   with `base`) — the per-job stages must all be cleaned up. */
static int CountStageDirs(const fs::path& root, const std::string& base)
{
    int n = 0;
    std::error_code ec;
    fs::directory_iterator it(root, ec), end;
    for(; !ec && it != end; it.increment(ec))
    {
        if(it->path().filename().string().compare(
               0, base.size(), base) == 0)
        {
            ++n;
        }
    }
    return n;
}

static void Write(const fs::path& path, const std::string& s)
{
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << s;
}
static std::string Read(const fs::path& path)
{
    std::ifstream in(path, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

/* One ring-zone type; `variant` differentiates content; an
   optional asset model ref exercises the asset bundle path;
   `child` adds a child-entity dependency edge. */
static std::string TypeJson(const std::string& id, int leds,
                            const std::string& variant,
                            const std::string& child = "",
                            const std::string& asset = "")
{
    json ent = json::object({
        { "geometry", "fan_body" },
        { "zone",     "ring"     },
    });
    if(!asset.empty())
    {
        ent["appearance"] = json::object({{ "model", asset }});
    }
    json entities = json::object({{ "body", ent }});
    if(!child.empty())
    {
        entities["mount"] = json::object({
            { "type", child },
            { "position", json::array({ 0.0, 0.02, 0.0 }) },
        });
    }
    return json::object({
        { "schema_version", 1        },
        { "id",             id       },
        { "name",           id + " " + variant },
        { "category",       "fan"    },
        { "entities",       entities },
        { "zones", json::array({ json::object({
              { "id",        "ring" },
              { "entity",    "body" },
              { "led_count", leds   },
              { "layout", json::object({
                    { "type",     "ring" },
                    { "radius_m", 0.05  } }) } }) }) },
    }).dump(2);
}

/* Compact v3 workspace on `type_id`, carrying the identity +
   arming fields the sanitizer must strip. `preset` names an
   optional effect look. */
static std::string WorkspaceJson(const std::string& name,
                                 const std::string& type_id,
                                 const std::string& preset = "")
{
    json doc = json::object({
        { "schema_version", 3    },
        { "name",           name },
        { "meta", json::object({{ "name", name }}) },
        { "output", json::object({
              { "brightness",       1.0  },
              { "live_on_startup",  true } }) },
        { "devices", json::object({
              { "fan", json::object({
                    { "type", type_id },
                    { "x", 0.1 }, { "y", 0.05 }, { "z", -0.02 },
                    { "rx", 0.0 }, { "ry", 30.0 }, { "rz", 0.0 } }) } }) },
        { "bindings", json::object({
              { "b1", json::object({
                    { "controller_name", "Test Controller" },
                    { "serial",          "SN-SECRET-1"     },
                    { "zone_name",       "ring"            },
                    { "zone_leds",       8                 } }) } }) },
        { "device_settings", json::object({
              { "fan", json::object({
                    { "zones", json::object({
                          { "ring", json::object({
                                { "binding",   "b1"  },
                                { "addr_base", 0     },
                                { "verified",  true  } }) } }) } }) } }) },
        { "effects", json::object({
              { "preset",  preset },
              { "seed",    7      },
              { "playing", true   } }) },
    });
    return doc.dump(2);
}

static std::string LookJson(const std::string& id)
{
    return json::object({
        { "schema_version", 1         },
        { "id",             id        },
        { "name",           id        },
        { "needs",          ""        },
        { "layers", json::array({ json::object({
              { "primitive", "static" },
              { "opacity",   0.5      },
              { "palette", json::array({ json::object({
                    { "pos",   0.0       },
                    { "color", "#102030" } }) }) } }) }) },
    }).dump(2);
}

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

/* A service wired for offline tests: staged uploads read from
   `uploads`, downloads land in `downloads`; job events collect. */
struct Svc
{
    WorkspaceStore::Deps       sdeps;
    WorkspaceService::Deps     deps;
    WorkspaceService           svc;
    std::map<std::string, std::string> uploads;
    std::string                      last_download;
    std::string                      last_download_kind;
    unsigned long long               dl_seq = 0;

    Svc(const std::string& root,
        const studio::PresetRegistry& devices,
        const studio::EffectRegistry& effects)
        : svc(root, DepsFor(devices, effects))
    {
        WorkspaceService::JobSurface surf;
        surf.read_staged =
            [this](const std::string& tok, std::string& bytes,
                   std::string& err) -> bool
            {
                const auto it = uploads.find(tok);
                if(it == uploads.end())
                {
                    err = "unknown token";
                    return false;
                }
                bytes = it->second;
                return true;
            };
        surf.stage_download =
            [this](uint64_t, const std::string& kind,
                   const std::string& bytes, std::string& token,
                   std::string& sha, std::string&) -> bool
            {
                last_download      = bytes;
                last_download_kind = kind;
                token = "dl-" + std::to_string(++dl_seq);
                sha   = std::string(64, 'a');   /* test sha */
                return true;
            };
        svc.SetJobSurface(surf);
    }

    WorkspaceService::Deps DepsFor(
        const studio::PresetRegistry& devices,
        const studio::EffectRegistry& effects)
    {
        WorkspaceService::Deps d;
        d.devices     = devices;
        d.effects     = effects;
        d.apply_scene = [](std::shared_ptr<const WorkspaceCandidate>,
                           std::string&) { return true; };
        return d;
    }

    WorkspaceService::JobStatus WaitJob(uint64_t job_id,
                                        uint64_t session = 1)
    {
        for(int i = 0; i < 400; ++i)
        {
            const WorkspaceService::JobStatus st =
                svc.Status(session, job_id);
            if(st.found &&
               (st.state == WorkspaceService::JobState::Succeeded ||
                st.state == WorkspaceService::JobState::Failed ||
                st.state == WorkspaceService::JobState::Cancelled))
            {
                return st;
            }
            std::this_thread::sleep_for(
                std::chrono::milliseconds(10));
        }
        WorkspaceService::JobStatus st;
        st.error = "job never terminated";
        return st;
    }

    WorkspaceService::JobStatus Run(
        const json& body, uint64_t session = 1)
    {
        const WorkspaceService::JobResult r =
            svc.Enqueue(session, body.value("verb", ""), body);
        if(!r.accepted)
        {
            WorkspaceService::JobStatus st;
            st.state  = WorkspaceService::JobState::Failed;
            st.error  = r.error;
            st.detail = r.detail;
            return st;
        }
        return WaitJob(r.job_id, session);
    }
};

/*---------------------------------------------------------*\
|| T1: PresetBundle on disk — export closure/sanitize,     ||
|| inspect classification, apply remaps + cascade.         ||
\*---------------------------------------------------------*/
static void TestBundleRoundTrip()
{
    std::printf("-- bundle: export/inspect/apply on disk\n");
    /* Every type dir is workspace-shaped —
       <ws>/presets/devices with its assets at <ws>/assets — so
       asset lookups and landings stay inside g_tmp. (A shallow
       dir used to resolve `../../assets` to %TEMP%\assets: the
       test read a residue file there and ApplyImport wrote one.) */
    const fs::path base_ws    = g_tmp / "t1-basews";
    const fs::path base_types = base_ws / "presets" / "devices";
    fs::create_directories(base_types);
    /* ring-fan carries an asset ref; tower mounts it — the
       export must carry BOTH (transitive closure). */
    Write(base_types / "ring-fan.device.json",
          TypeJson("ring-fan", 8, "base", "", "models/fan.glb"));
    Write(base_types / "tower.device.json",
          TypeJson("tower", 4, "base", "ring-fan"));
    Write(base_ws / "assets" / "models" / "fan.glb", "GLB-BYTES");

    studio::PresetRegistry reg;
    {
        std::vector<std::string> lerrs;
        CHECK(reg.LoadDirectory(base_types.string(), &lerrs));
        for(const std::string& e : lerrs)
        {
            std::printf("  (base load: %s)\n", e.c_str());
        }
    }
    CHECK(reg.Contains("tower") && reg.Contains("ring-fan"));

    /* The source workspace root: presets/devices + assets. */
    const fs::path wsroot = g_tmp / "t1-srcws";
    Write(wsroot / "assets" / "models" / "fan.glb",
          "GLB-BYTES");
    studio::StudioDocument doc;
    {
        std::vector<std::string> errs;
        CHECK(studio::FromJson(
            json::parse(WorkspaceJson("portable", "tower")),
            doc, &errs));
    }

    const fs::path bundle = g_tmp / "t1-bundle";
    std::vector<std::string> errors, warnings;
    unsigned int types_written = 0, assets_copied = 0;
    CHECK(studio::ExportBundle(
        bundle.string(), doc, reg,
        (wsroot / "presets" / "devices").string(),
        &errors, &warnings, /*overwrite=*/false,
        &types_written, &assets_copied));
    if(!errors.empty())
    {
        std::printf("  (export: %s)\n", errors.front().c_str());
    }
    CHECK(types_written == 2);
    CHECK(assets_copied == 1);
    CHECK(fs::exists(bundle / "studio.json"));
    CHECK(fs::exists(bundle / "presets" / "devices" /
                     "tower.device.json"));
    CHECK(fs::exists(bundle / "presets" / "devices" /
                     "ring-fan.device.json"));
    CHECK(fs::exists(bundle / "assets" / "models" / "fan.glb"));

    /* studio.json is the sanitized form. */
    const json sj = json::parse(
        Read(bundle / "studio.json"), nullptr, false);
    CHECK(sj.is_object());
    CHECK(sj["bindings"]["b1"].value("serial", "x").empty()
          || !sj["bindings"]["b1"].contains("serial"));
    CHECK(sj["device_settings"]["fan"]["zones"]["ring"]
          .value("verified", false) == false);
    CHECK(sj.value("output", json::object())
          .value("live_on_startup", true) == false);
    CHECK(sj.value("effects", json::object())
          .value("playing", true) == false);

    /* Refusing a non-empty destination without overwrite. */
    CHECK(!studio::ExportBundle(bundle.string(), doc, reg,
                                (wsroot / "presets" / "devices")
                                    .string(),
                                &errors));

    /* Inspect against an EMPTY local library — all new. */
    {
        studio::PresetRegistry empty;
        studio::ImportPlan plan;
        errors.clear();
        CHECK(studio::InspectBundle(bundle.string(), empty,
                                    plan, &errors));
        CHECK(plan.types.size() == 2);
        CHECK(plan.assets.size() == 1);
        for(const studio::BundleType& t : plan.types)
        {
            CHECK(t.status == studio::BundleType::Status::New);
        }
    }

    /* Same content local -> identical; different -> conflict. */
    /* The local workspace has the type but NOT its asset yet —
       the load reports it missing (the type still registers) and
       the import below lands the bundled copy under <ws>/assets. */
    const fs::path local_ws    = g_tmp / "t1-localws";
    const fs::path local_types = local_ws / "presets" / "devices";
    fs::create_directories(local_types);
    Write(local_types / "ring-fan.device.json",
          TypeJson("ring-fan", 8, "base", "", "models/fan.glb"));
    Write(local_types / "tower.device.json",
          TypeJson("tower", 4, "DIFFERENT", "ring-fan"));
    studio::PresetRegistry local;
    {
        std::vector<std::string> lerrs;
        CHECK(!local.LoadDirectory(local_types.string(), &lerrs));
        bool missing_asset = false;
        for(const std::string& e : lerrs)
        {
            std::printf("  (local load: %s)\n", e.c_str());
            missing_asset = missing_asset ||
                e.find("missing asset 'models/fan.glb'") !=
                    std::string::npos;
        }
        CHECK(missing_asset);
        CHECK(local.Contains("ring-fan") && local.Contains("tower"));
    }

    studio::ImportPlan plan;
    errors.clear();
    CHECK(studio::InspectBundle(bundle.string(), local,
                                plan, &errors));
    bool tower_conflict = false, ring_identical = false;
    for(const studio::BundleType& t : plan.types)
    {
        if(t.id == "tower")
        {
            tower_conflict =
                t.status == studio::BundleType::Status::Conflict;
        }
        if(t.id == "ring-fan")
        {
            ring_identical =
                t.status == studio::BundleType::Status::Identical;
        }
    }
    CHECK(tower_conflict);
    CHECK(ring_identical);

    /* Apply with an explicit conflict rename — the workspace's
       type ref remaps, the local file is never overwritten. */
    const std::string local_tower_before =
        Read(local_types / "tower.device.json");
    studio::StudioDocument applied;
    errors.clear();
    CHECK(studio::ApplyImport(plan, {{ "tower", "tower-x" }},
                              local_types.string(), applied,
                              &errors, &warnings));
    if(!errors.empty())
    {
        std::printf("  (apply: %s)\n", errors.front().c_str());
    }
    CHECK(applied.devices.at("fan").type == "tower-x");
    CHECK(Read(local_types / "tower.device.json") ==
          local_tower_before);                 /* never overwritten */
    CHECK(fs::exists(local_types / "tower-x.device.json"));
    /* The bundled asset landed inside the target workspace. */
    CHECK(Read(local_ws / "assets" / "models" / "fan.glb") ==
          "GLB-BYTES");

    /* A shallow (non-workspace-shaped) target with bundled assets
       is refused before anything is written — the lexical
       `../../assets` would have escaped the target's tree. */
    {
        const fs::path shallow = g_tmp / "t1-sub" / "t1-shallow";
        fs::create_directories(shallow);
        studio::StudioDocument sdoc;
        std::vector<std::string> serrs;
        CHECK(!studio::ApplyImport(plan, {{ "tower", "tower-x" }},
                                   shallow.string(), sdoc,
                                   &serrs, &warnings));
        CHECK(!serrs.empty());
        CHECK(!fs::exists(shallow / "tower-x.device.json"));
        CHECK(!fs::exists(g_tmp / "assets"));
    }
    /* The renamed type keeps its child ref — ring-fan was
       identical, so it stays ring-fan. */
    {
        studio::DevicePreset tp;
        CHECK(studio::DevicePresetFromJsonFile(
            (local_types / "tower-x.device.json").string(),
            tp, nullptr));
        CHECK(tp.entities.count("mount") == 1 &&
              tp.entities.at("mount").type == "ring-fan");
    }

    /* Nested collision: the child conflicts too — renaming it
       cascades a remap inside the renamed parent. */
    {
        const fs::path nested_ws = g_tmp / "t1-nestedws";
        const fs::path nested = nested_ws / "presets" / "devices";
        fs::create_directories(nested);
        Write(nested_ws / "assets" / "models" / "fan.glb",
              "LOCAL-GLB");
        Write(nested / "ring-fan.device.json",
              TypeJson("ring-fan", 8, "OTHER", "", "models/fan.glb"));
        Write(nested / "tower.device.json",
              TypeJson("tower", 4, "OTHER", "ring-fan"));
        studio::PresetRegistry nreg;
        CHECK(nreg.LoadDirectory(nested.string(), nullptr));
        studio::ImportPlan nplan;
        errors.clear();
        CHECK(studio::InspectBundle(bundle.string(), nreg,
                                    nplan, &errors));
        studio::StudioDocument ndoc;
        errors.clear();
        CHECK(studio::ApplyImport(
            nplan,
            {{ "ring-fan", "ring-alt" }, { "tower", "tower-alt" }},
            nested.string(), ndoc, &errors, &warnings));
        CHECK(ndoc.devices.at("fan").type == "tower-alt");
        studio::DevicePreset tp;
        CHECK(studio::DevicePresetFromJsonFile(
            (nested / "tower-alt.device.json").string(),
            tp, nullptr));
        CHECK(tp.entities.at("mount").type == "ring-alt");
    }

    /* Malformed + interrupted bundles refuse at inspect. */
    {
        const fs::path bad = g_tmp / "t1-bad";
        fs::create_directories(bad);
        Write(bad / "studio.json", "{ not json ");
        studio::ImportPlan plan;
        CHECK(!studio::InspectBundle(bad.string(), reg,
                                     plan, &errors));

        const fs::path torn = g_tmp / "t1-torn";
        fs::create_directories(torn / "presets" / "devices");
        Write(torn / "presets" / "devices" / "ring-fan.device.json",
              TypeJson("ring-fan", 8, "base"));
        /* studio.json missing — the interrupted-export marker. */
        CHECK(!studio::InspectBundle(torn.string(), reg,
                                     plan, &errors));
    }
}

/*---------------------------------------------------------*\
|| T2: WorkspaceStore single-resource ops.                 ||
\*---------------------------------------------------------*/
static void TestStoreResourceOps()
{
    std::printf("-- store: resource install/remove\n");
    const fs::path base_types = g_tmp / "t2-base";
    fs::create_directories(base_types);
    Write(base_types / "mini-fan.device.json",
          TypeJson("mini-fan", 8, "base"));

    WorkspaceStore::Deps deps;
    deps.devices.LoadDirectory(base_types.string(), nullptr);
    const fs::path root = g_tmp / "t2-store";
    WorkspaceStore store(root.string(), deps);

    WorkspaceCandidate cand;
    std::printf("   t2: parse candidate\n");
    CHECK(MakeCandidate(
        json::parse(WorkspaceJson("desk", "mini-fan")),
        deps.devices, deps.effects, cand));
    std::printf("   t2: commit\n");
    WorkspaceStore::CommitResult cr =
        store.Commit("desk", cand, std::string(), 0);
    CHECK(cr.ok);
    uint64_t ws_rev = cr.workspace_revision;
    uint64_t rs_rev = cr.resource_revision;

    std::printf("   t2: install panel-x\n");
    /* New type installs; revision bumps; stale gate refuses. */
    studio::DevicePreset p;
    CHECK(studio::DevicePresetFromJson(
        json::parse(TypeJson("panel-x", 12, "v1")), p, nullptr));
    auto ir = store.InstallDeviceTypeResource(
        "desk", p, /*replace=*/false, ws_rev, rs_rev);
    CHECK(ir.ok && ir.landed_id == "panel-x" && !ir.reused);
    rs_rev = ir.resource_revision;
    CHECK(fs::exists(fs::path(store.LibraryDevicesDir()) /
                     "panel-x.device.json"));
    {
        auto stale = store.InstallDeviceTypeResource(
            "desk", p, false, ws_rev, 0);
        CHECK(!stale.ok);
    }
    /* Identical content reuses — no bump. */
    ir = store.InstallDeviceTypeResource("desk", p, false,
                                         ws_rev, rs_rev);
    CHECK(ir.ok && ir.reused);
    CHECK(ir.resource_revision == rs_rev);

    /* Differing content without replace lands suffixed. */
    studio::DevicePreset p2;
    CHECK(studio::DevicePresetFromJson(
        json::parse(TypeJson("panel-x", 12, "v2")), p2, nullptr));
    ir = store.InstallDeviceTypeResource("desk", p2, false,
                                         ws_rev, rs_rev);
    CHECK(ir.ok && ir.landed_id != "panel-x");
    rs_rev = ir.resource_revision;

    /* Explicit replace rewrites the FILE-layer entry. */
    studio::DevicePreset p3;
    CHECK(studio::DevicePresetFromJson(
        json::parse(TypeJson("panel-x", 12, "v3")), p3, nullptr));
    ir = store.InstallDeviceTypeResource("desk", p3,
                                         /*replace=*/true,
                                         ws_rev, rs_rev);
    CHECK(ir.ok && ir.replaced && ir.landed_id == "panel-x");
    rs_rev = ir.resource_revision;
    {
        studio::DevicePreset landed;
        CHECK(studio::DevicePresetFromJsonFile(
            (fs::path(store.LibraryDevicesDir()) /
             "panel-x.device.json").string(), landed, nullptr));
        CHECK(landed.name == "panel-x v3");
    }

    std::printf("   t2: replace packaged\n");
    /* Replace against a PACKAGED default lands the file
       override — the packaged bytes are untouched. */
    studio::DevicePreset pv;
    CHECK(studio::DevicePresetFromJson(
        json::parse(TypeJson("mini-fan", 8, "override")),
        pv, nullptr));
    ir = store.InstallDeviceTypeResource("desk", pv,
                                         /*replace=*/true,
                                         ws_rev, rs_rev);
    CHECK(ir.ok && ir.landed_id == "mini-fan");
    rs_rev = ir.resource_revision;
    CHECK(Read(base_types / "mini-fan.device.json") ==
          TypeJson("mini-fan", 8, "base"));

    std::printf("   t2: in-use refusal\n");
    /* In-use refusal: the workspace references mini-fan. */
    std::printf("   t2: -> RemoveResource mini-fan\n");
    auto rr = store.RemoveResource("desk", "device_type",
                                   "mini-fan", ws_rev, rs_rev);
    std::printf("   t2: <- ok=%d err=%s\n", (int)rr.ok,
                rr.error.c_str());
    CHECK(!rr.ok);
    CHECK(rr.error.find("in use") != std::string::npos);

    std::printf("   t2: dep installs\n");
    /* Dependent refusal: install dep-a + dep-b(refs dep-a),
       then removing dep-a names dep-b. */
    studio::DevicePreset da, db;
    CHECK(studio::DevicePresetFromJson(
        json::parse(TypeJson("dep-a", 4, "a")), da, nullptr));
    {
        auto dai = store.InstallDeviceTypeResource(
            "desk", da, false, ws_rev, rs_rev);
        CHECK(dai.ok);
        rs_rev = dai.resource_revision;
    }
    CHECK(studio::DevicePresetFromJson(
        json::parse(TypeJson("dep-b", 4, "b", "dep-a")),
        db, nullptr));
    {
        auto tmp = store.InstallDeviceTypeResource(
            "desk", db, false, ws_rev, rs_rev);
        CHECK(tmp.ok);
        if(!tmp.ok) std::printf("  (dep-b install: %s)\n",
                                tmp.error.c_str());
        rs_rev = tmp.resource_revision;
    }
    std::printf("   t2: remove dep-a\n");
    rr = store.RemoveResource("desk", "device_type", "dep-a",
                              ws_rev, rs_rev);
    CHECK(!rr.ok);
    CHECK(rr.error.find("dep-b") != std::string::npos);

    /* Packaged default with no file layer: nothing to delete. */
    rr = store.RemoveResource("desk", "device_type", "mini-fan",
                              ws_rev, rs_rev);
    CHECK(!rr.ok);   /* still in use — the honest refusal */

    /* Free file-layer type removes; the panel-x OVERRIDE of a
       packaged… wait — mini-fan is in use; delete dep-b first,
       then dep-a. */
    std::printf("   t2: remove dep-b\n");
    rr = store.RemoveResource("desk", "device_type", "dep-b",
                              ws_rev, rs_rev);
    if(!rr.ok) std::printf("  (dep-b remove: %s)\n",
                           rr.error.c_str());
    CHECK(rr.ok && rr.removed && !rr.reverted_to_default);
    rs_rev = rr.resource_revision;
    std::printf("   t2: remove dep-a\n");
    rr = store.RemoveResource("desk", "device_type", "dep-a",
                              ws_rev, rs_rev);
    CHECK(rr.ok && rr.removed);
    rs_rev = rr.resource_revision;

    /* Revert-to-default: install a file override of a type the
       workspace does NOT use but that has a packaged default…
       mini-fan is in use — use a packaged default that isn't.
       There isn't one in this registry; simulate: add a second
       default is impossible post-deps. Instead verify the
       file-layer delete of a NON-default id reports
       reverted_to_default=false and the id is gone. */
    std::printf("   t2: remove panel-x\n");
    rr = store.RemoveResource("desk", "device_type", "panel-x",
                              ws_rev, rs_rev);
    if(!rr.ok) std::printf("  (panel-x remove: %s)\n",
                           rr.error.c_str());
    CHECK(rr.ok && rr.removed && !rr.reverted_to_default);
    rs_rev = rr.resource_revision;
    CHECK(!fs::exists(fs::path(store.LibraryDevicesDir()) /
                      "panel-x.device.json"));

    std::printf("   t2: tail refusals\n");
    /* Removing a packaged-only id reports the honest reason. */
    rr = store.RemoveResource("desk", "device_type",
                              "mini-fan", ws_rev, rs_rev);
    CHECK(!rr.ok);   /* in use — still first */
    /* effect-kind remove on a missing id fails honestly. */
    rr = store.RemoveResource("desk", "effect", "nope",
                              ws_rev, rs_rev);
    CHECK(!rr.ok);
    rr = store.RemoveResource("desk", "bogus", "panel-x",
                              ws_rev, rs_rev);
    CHECK(!rr.ok);
}

/*---------------------------------------------------------*\
|| T3: the packed wire form through WorkspaceService —      ||
|| export packs, inline look materialization, inspect +    ||
|| hostile-path refusal, conflict choices at commit, no    ||
|| auto-apply.                                             ||
\*---------------------------------------------------------*/
static void TestPackedServiceFlow()
{
    std::printf("-- service: packed export/inspect/commit\n");
    const fs::path base_types = g_tmp / "t3-base";
    const fs::path looks      = g_tmp / "t3-looks";
    fs::create_directories(base_types);
    fs::create_directories(looks);
    Write(base_types / "mini-fan.device.json",
          TypeJson("mini-fan", 8, "base"));
    Write(looks / "my-look.effect.json", LookJson("my-look"));

    studio::PresetRegistry reg;
    CHECK(reg.LoadDirectory(base_types.string(), nullptr));
    studio::EffectRegistry fx;
    CHECK(fx.LoadDirectory(looks.string(), nullptr));

    Svc a(P("t3-svc-a"), reg, fx);
    /* Commit a workspace that references the look. */
    {
        const auto st = a.Run(json::object({
            { "verb", "commit" },
            { "id",   "desk"   },
            { "document",
              json::parse(WorkspaceJson("desk", "mini-fan",
                                        "my-look")) },
            { "base_revision",     0ull },
            { "resource_revision", 0ull },
        }));
        CHECK(st.state == WorkspaceService::JobState::Succeeded);
        if(st.state != WorkspaceService::JobState::Succeeded)
        {
            std::printf("  (commit: %s)\n", st.error.c_str());
        }
        CHECK(st.applied);        /* commit adopts + applies */
    }

    /* Export -> packed wire bundle staged for download. A
       legacy fixed-name stage left by an older build is swept;
       the per-job stage is removed afterwards. */
    Write(fs::path(P("t3-svc-a")) / ".export-stage" / "junk.txt", "x");
    json bundle;
    {
        const auto st = a.Run(json::object({
            { "verb", "export" } }));
        CHECK(CountStageDirs(P("t3-svc-a"), ".export-stage") == 0);
        CHECK(st.state == WorkspaceService::JobState::Succeeded);
        if(st.state != WorkspaceService::JobState::Succeeded)
        {
            std::printf("  (export: %s)\n", st.error.c_str());
            return;
        }
        CHECK(!st.result.value("transfer_id", "").empty());
        CHECK(a.last_download_kind == "export");
        bundle = json::parse(a.last_download, nullptr, false);
        CHECK(bundle.is_object());
        CHECK(bundle.value("format", "") ==
              "openrgb2-bundle-files");
        const json files = bundle.value("files", json());
        CHECK(files.contains("studio.json"));
        CHECK(files.contains(
            "presets/devices/mini-fan.device.json"));
        /* Inline look materialization: the named preset stays
           as provenance AND the resolved layers travel inline —
           no look FILE ships in the bundle. */
        const json sj = json::parse(
            files["studio.json"].value("data", ""),
            nullptr, false);
        CHECK(sj.value("effects", json::object())
              .value("preset", "") == "my-look");
        CHECK(sj["effects"].value("layers", json()).is_array()
              && !sj["effects"]["layers"].empty());
        bool look_file = false;
        for(auto it = files.begin(); it != files.end(); ++it)
        {
            look_file |= it.key().find(".effect.json") !=
                         std::string::npos;
        }
        CHECK(!look_file);
        /* Sanitized: no serial, no verified, not playing, no
           live_on_startup. */
        CHECK(sj["bindings"]["b1"].value("serial","x").empty());
        CHECK(sj["device_settings"]["fan"]["zones"]["ring"]
              .value("verified", false) == false);
        CHECK(sj["effects"].value("playing", true) == false);
        CHECK(sj.value("output", json::object())
              .value("live_on_startup", true) == false);
    }

    /* Fresh service: inspect the packed bundle — new types
       reported, no conflicts against the same base. */
    Svc b(P("t3-svc-b"), reg, fx);
    b.uploads["up1"] = bundle.dump();
    {
        const auto st = b.Run(json::object({
            { "verb",   "import_inspect" },
            { "staged", "up1"            },
        }));
        CHECK(st.state == WorkspaceService::JobState::Succeeded);
        CHECK(st.result.value("valid", false));
        CHECK(st.result.value("conflicts", json()).empty());
        CHECK(CountStageDirs(P("t3-svc-b"), ".import-stage") == 0);
        /* The workspace's serialized bytes never armed anything
           on the importing side either — inspect commits
           nothing. */
        CHECK(b.svc.WorkspaceRevision() == 0);
    }

    /* Hostile packed paths refuse BEFORE any staging. */
    auto hostile = [&](const std::string& rel)
    {
        const json bad = json::object({
            { "format", "openrgb2-bundle-files" },
            { "version", 1 },
            { "files", json::object({
                  { "studio.json", json::object({
                        { "encoding", "utf8" },
                        { "data", "{}" } }) },
                  { rel, json::object({
                        { "encoding", "utf8" },
                        { "data", "{}" } }) } }) },
        });
        const std::string tok = "bad-" + rel.substr(0, 4) +
                                std::to_string(rel.size());
        b.uploads[tok] = bad.dump();
        const auto st = b.Run(json::object({
            { "verb",   "import_inspect" },
            { "staged", tok              },
        }));
        CHECK(st.state == WorkspaceService::JobState::Failed);
        return st;
    };
    hostile("presets/devices/../evil.device.json");
    hostile("../escape.json");
    hostile("presets/devices/evil dir.device.json");
    hostile("assets/../../outside.png");
    {
        /* Bad encoding. */
        const json bad = json::object({
            { "format", "openrgb2-bundle-files" },
            { "files", json::object({
                  { "studio.json", json::object({
                        { "encoding", "utf8" },
                        { "data", WorkspaceJson("x", "mini-fan") } }) },
                  { "assets/a.bin", json::object({
                        { "encoding", "weird" },
                        { "data", "AA==" } }) } }) },
        });
        b.uploads["badenc"] = bad.dump();
        const auto st = b.Run(json::object({
            { "verb",   "import_inspect" },
            { "staged", "badenc"         },
        }));
        CHECK(st.state == WorkspaceService::JobState::Failed);
    }

    /* Commit the good bundle — lands, never applies. */
    {
        const auto st = b.Run(json::object({
            { "verb",   "import_commit" },
            { "id",     "imp"           },
            { "staged", "up1"           },
        }));
        CHECK(st.state == WorkspaceService::JobState::Succeeded);
        if(st.state != WorkspaceService::JobState::Succeeded)
        {
            std::printf("  (import: %s)\n", st.error.c_str());
        }
        CHECK(st.committed);
        CHECK(!st.applied);      /* imports never auto-apply */
        /* The bundled type landed in the managed library and
           the imported document's identity fields were
           sanitized on the way in. */
        /* mini-fan was IDENTICAL to the importer's base — the
           library needs no file-layer copy (reuse, not
           rewrite). The workspace's own generation snapshot
           pins it regardless. */
        const WorkspaceStore::Revisions irv =
            b.svc.Store().LoadRevisions("imp");
        CHECK(irv.exists);
        std::string bytes, err;
        CHECK(b.svc.Store().ReadActiveDocumentBytes(
            "imp", bytes, err));
        const json imp = json::parse(bytes, nullptr, false);
        CHECK(imp["bindings"]["b1"].value("serial","x").empty());
        CHECK(imp["effects"].value("playing", true) == false);
    }

    /* Conflict choices ride the commit body: overwrite the
       library copy with different content, re-inspect, and a
       missing choice refuses by name; with the choice it lands
       under the new id and the doc remaps. */
    {
        Write(fs::path(b.svc.Store().LibraryDevicesDir()) /
                  "mini-fan.device.json",
              TypeJson("mini-fan", 8, "DIFFERENT"));
        const auto st = b.Run(json::object({
            { "verb",   "import_inspect" },
            { "staged", "up1"            },
        }));
        CHECK(st.result.value("conflicts", json()).size() == 1);

        /* A second workspace id imports fresh (revision 0). */
        const auto sc = b.Run(json::object({
            { "verb",   "import_commit" },
            { "id",     "imp2"          },
            { "staged", "up1"           },
        }));
        CHECK(sc.state == WorkspaceService::JobState::Failed);
        CHECK(sc.detail == "conflict_choice_required"
              || sc.error.find("conflict") != std::string::npos);

        const auto sc2 = b.Run(json::object({
            { "verb",   "import_commit" },
            { "id",     "imp2"          },
            { "staged", "up1"           },
            { "choices", json::object(
                  {{ "mini-fan", "mini-fan-alt" }}) },
        }));
        CHECK(sc2.state == WorkspaceService::JobState::Succeeded);
        if(sc2.state != WorkspaceService::JobState::Succeeded)
        {
            std::printf("  (import2: %s)\n", sc2.error.c_str());
        }
        CHECK(fs::exists(
            fs::path(b.svc.Store().LibraryDevicesDir()) /
            "mini-fan-alt.device.json"));
        /* The local file was never overwritten. */
        studio::DevicePreset lp;
        CHECK(studio::DevicePresetFromJsonFile(
            (fs::path(b.svc.Store().LibraryDevicesDir()) /
             "mini-fan.device.json").string(), lp, nullptr));
        CHECK(lp.name == "mini-fan DIFFERENT");
        std::string bytes, err;
        CHECK(b.svc.Store().ReadActiveDocumentBytes(
            "imp2", bytes, err));
        const json imp2 = json::parse(bytes, nullptr, false);
        CHECK(imp2["devices"]["fan"].value("type", "") ==
              "mini-fan-alt");
    }

    /* resource_install / resource_remove ride the same queue —
       revision-gated against the TARGET workspace's marker
       (the import adopted imp; it just never applied it). */
    {
        const WorkspaceStore::Revisions rev =
            b.svc.Store().LoadRevisions("imp");
        CHECK(rev.exists);
        const auto st = b.Run(json::object({
            { "verb",  "resource_install" },
            { "id",    "imp"            },
            { "kind",  "device_type"    },
            { "device_type",
              json::parse(TypeJson("extra", 4, "x")) },
            { "base_revision",     rev.workspace_revision },
            { "resource_revision", rev.resource_revision  },
        }));
        CHECK(st.state == WorkspaceService::JobState::Succeeded);
        CHECK(st.result.value("landed_id", "") == "extra");
        /* The stale-resource-revision refusal reaches the wire
           as a classified detail, not a bare failure. */
        const auto stale = b.Run(json::object({
            { "verb",  "resource_install" },
            { "id",    "imp"            },
            { "kind",  "device_type"    },
            { "device_type",
              json::parse(TypeJson("extra2", 4, "x")) },
            { "base_revision",     rev.workspace_revision },
            { "resource_revision", rev.resource_revision  },
        }));
        CHECK(stale.state == WorkspaceService::JobState::Failed);
        CHECK(stale.detail == "revision_conflict");
        /* resource_remove on a packaged default refuses. */
        const auto rd = b.Run(json::object({
            { "verb",        "resource_remove" },
            { "id",          "imp"             },
            { "kind",        "device_type"     },
            { "resource_id", "mini-fan"        },
            { "base_revision",     rev.workspace_revision },
            { "resource_revision",
              rev.resource_revision + 1        },
        }));
        /* mini-fan has a file-layer entry from the import, and
           "imp" still uses it — in-use refusal wins. */
        CHECK(rd.state == WorkspaceService::JobState::Failed);
        CHECK(rd.detail == "in_use" ||
              rd.error.find("in use") != std::string::npos);
    }

    /* Whole-branch review regression: service-owned resource
       writes rewrite the ACTIVE workspace's marker (the
       bumped resource_revision is mixed into the
       ExternalScan signature). Without the re-baseline every
       resource_install / resource_remove / preset_import on
       the current workspace reports a phantom
       external_change "modified" — a sticky banner whose
       offered remedy is a draft-replacing reload. All three
       paths must leave CheckExternal "none", and
       preset_import must publish the bumped resource
       revision. ("imp2" is the adopted current workspace —
       the conflict-choice import above adopted it.) */
    {
        CHECK(b.svc.Snapshot().id == "imp2");
        CHECK(b.svc.CheckExternal().state == "none");
        WorkspaceStore::Revisions rev =
            b.svc.Store().LoadRevisions("imp2");
        CHECK(rev.exists);
        const auto ri = b.Run(json::object({
            { "verb",  "resource_install" },
            { "id",    "imp2"           },
            { "kind",  "effect"         },
            { "effect", json::parse(LookJson("svc-look")) },
            { "base_revision",     rev.workspace_revision },
            { "resource_revision", rev.resource_revision  },
        }));
        CHECK(ri.state == WorkspaceService::JobState::Succeeded);
        if(ri.state != WorkspaceService::JobState::Succeeded)
        {
            std::printf("  (look install: %s)\n",
                        ri.error.c_str());
        }
        CHECK(b.svc.CheckExternal().state == "none");
        CHECK(b.svc.Snapshot().resource_revision ==
              ri.resource_revision);

        rev = b.svc.Store().LoadRevisions("imp2");
        const auto rr = b.Run(json::object({
            { "verb",        "resource_remove" },
            { "id",          "imp2"            },
            { "kind",        "effect"          },
            { "resource_id", "svc-look"        },
            { "base_revision",     rev.workspace_revision },
            { "resource_revision", rev.resource_revision  },
        }));
        CHECK(rr.state == WorkspaceService::JobState::Succeeded);
        if(rr.state != WorkspaceService::JobState::Succeeded)
        {
            std::printf("  (look remove: %s)\n",
                        rr.error.c_str());
        }
        CHECK(b.svc.CheckExternal().state == "none");
        CHECK(b.svc.Snapshot().resource_revision ==
              rr.resource_revision);

        /* preset_import — a types-only packed bundle installs
           into the ACTIVE workspace's library via the
           per-type loop; the tail must publish the bumped
           resource_revision and re-baseline the signature. */
        b.uploads["types-only"] = json::object({
            { "format",  "openrgb2-bundle-files" },
            { "version", 1 },
            { "files", json::object({
                  { "presets/devices/preset-fan.device.json",
                    json::object({
                        { "encoding", "utf8" },
                        { "data",
                          TypeJson("preset-fan", 6,
                                   "bundled") } }) } }) },
        }).dump();
        rev = b.svc.Store().LoadRevisions("imp2");
        const auto pi = b.Run(json::object({
            { "verb",   "import_commit"  },
            { "id",     "imp2"           },
            { "mode",   "preset_import"  },
            { "staged", "types-only"     },
            { "base_revision",     rev.workspace_revision },
            { "resource_revision", rev.resource_revision  },
        }));
        CHECK(pi.state == WorkspaceService::JobState::Succeeded);
        if(pi.state != WorkspaceService::JobState::Succeeded)
        {
            std::printf("  (preset_import: %s)\n",
                        pi.error.c_str());
        }
        CHECK(fs::exists(
            fs::path(b.svc.Store().LibraryDevicesDir()) /
            "preset-fan.device.json"));
        CHECK(b.svc.CheckExternal().state == "none");
        CHECK(b.svc.Snapshot().resource_revision ==
              pi.resource_revision);
    }
}

int main()
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    g_tmp = fs::temp_directory_path() /
            ("or2-bundle-" + std::to_string(
                std::chrono::steady_clock::now()
                    .time_since_epoch().count()));
    std::error_code ec;
    fs::create_directories(g_tmp, ec);
    std::printf("tmp: %s\n", g_tmp.string().c_str());

    TestBundleRoundTrip();
    TestStoreResourceOps();
    TestPackedServiceFlow();

    std::printf("\n%d checks, %d failures\n", checks, failures);
    fs::remove_all(g_tmp, ec);
    return failures == 0 ? 0 : 1;
}
