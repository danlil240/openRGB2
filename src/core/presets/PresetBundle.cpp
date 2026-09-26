/*---------------------------------------------------------*\
||| PresetBundle.cpp                                          |
|||                                                           |
|||   SPDX-License-Identifier: GPL-2.0-or-later               |
\*---------------------------------------------------------*/

#include "PresetBundle.h"
#include "../config/ConfigMigration.h"
#include "../scene/SceneJson.h"
#include "../scene/SceneResolver.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace studio
{

namespace fs = std::filesystem;

static void Err(std::vector<std::string>* errors, const std::string& e)
{
    if(errors != nullptr)
    {
        errors->push_back(e);
    }
}

static void Warn(std::vector<std::string>* warnings, const std::string& w)
{
    if(warnings != nullptr)
    {
        warnings->push_back(w);
    }
}

static std::string JoinErrors(const std::vector<std::string>& errs)
{
    std::string out;
    for(const std::string& e : errs)
    {
        if(!out.empty())
        {
            out += "; ";
        }
        out += e;
    }
    return out;
}

/*---------------------------------------------------------*\
||| Small file helpers — size caps and temp+rename writes. ||
\*---------------------------------------------------------*/
static bool ReadCapped(const fs::path& path, unsigned long long cap,
                       std::string& out, std::string& error)
{
    std::error_code ec;
    if(!fs::is_regular_file(path, ec))
    {
        error = path.string() + ": not a file";
        return false;
    }
    const unsigned long long size = fs::file_size(path, ec);
    if(ec)
    {
        error = path.string() + ": unreadable";
        return false;
    }
    if(size > cap)
    {
        error = path.string() + ": exceeds the "
                + std::to_string(cap / 1024 / 1024) + " MB limit";
        return false;
    }
    std::ifstream f(path, std::ios::binary);
    if(!f)
    {
        error = path.string() + ": open failed";
        return false;
    }
    std::ostringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return true;
}

/* Write tmp + rename. rename() can't replace an existing target
   on Windows, so the replace path is remove+rename — still never
   a truncated target: the old file only goes away once the new
   one is fully written. */
static bool WriteFileAtomic(const fs::path& path, const std::string& bytes,
                            std::string& error)
{
    std::error_code ec;
    const fs::path tmp = path.string() + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if(!f)
        {
            error = tmp.string() + ": open failed";
            return false;
        }
        f.write(bytes.data(), (std::streamsize)bytes.size());
        f.flush();
        if(!f.good())
        {
            f.close();
            fs::remove(tmp, ec);
            error = path.string() + ": write failed";
            return false;
        }
    }
    fs::rename(tmp, path, ec);
    if(ec)
    {
        ec.clear();
        fs::remove(path, ec);
        ec.clear();
        fs::rename(tmp, path, ec);
    }
    if(ec)
    {
        fs::remove(tmp, ec);
        error = path.string() + ": rename failed (" + ec.message() + ")";
        return false;
    }
    return true;
}

static void ApplyRenames(DevicePreset& p,
                         const std::map<std::string, std::string>& renames)
{
    for(auto& kv : p.entities)
    {
        const auto it = renames.find(kv.second.type);
        if(it != renames.end())
        {
            kv.second.type = it->second;
        }
    }
}

/* First free "<base>-import" / "<base>-importN" id — used for
   derived renames of identical types whose dependency renamed. */
static std::string DerivedId(const std::string& base,
                             const std::set<std::string>& taken)
{
    std::string cand = base + "-import";
    for(int n = 2; taken.count(cand); n++)
    {
        cand = base + "-import" + std::to_string(n);
    }
    return cand;
}

/*---------------------------------------------------------*\
||| Collect + sanitize                                     ||
\*---------------------------------------------------------*/
bool CollectUsedTypes(const StudioDocument& ws,
                      const PresetRegistry& registry,
                      std::vector<std::string>& ids,
                      std::vector<std::string>* errors)
{
    std::vector<std::string> errs;
    std::set<std::string> seen;
    std::vector<std::string> stack;
    for(const auto& kv : ws.devices)
    {
        if(kv.second.type.empty())
        {
            errs.push_back("devices." + kv.first + ": empty type");
        }
        else if(seen.insert(kv.second.type).second)
        {
            stack.push_back(kv.second.type);
        }
    }
    /* `seen` doubles as the visited set — a reference cycle can
       never enqueue forever (the registry rejects cycles at load,
       this is belt-and-suspenders for hand-built registries). */
    while(!stack.empty())
    {
        const std::string id = stack.back();
        stack.pop_back();
        const DevicePreset* p = registry.Find(id);
        if(p == nullptr)
        {
            errs.push_back("devices: type '" + id + "' is not"
                           " resolvable in the local library");
            continue;
        }
        for(const std::string& dep : PresetDependencies(*p))
        {
            if(seen.insert(dep).second)
            {
                stack.push_back(dep);
            }
        }
    }
    ids.assign(seen.begin(), seen.end());
    if(errors != nullptr)
    {
        *errors = errs;
    }
    return errs.empty();
}

StudioDocument SanitizeForExport(const StudioDocument& w)
{
    StudioDocument out = w;
    for(auto& kv : out.bindings)
    {
        kv.second.serial.clear();
        kv.second.location.clear();
    }
    for(auto& kv : out.device_settings)
    {
        for(auto& zkv : kv.second.zones)
        {
            zkv.second.verified = false;
        }
    }
    out.meta.live_on_startup = false;
    out.effect.playing       = false;
    return out;
}

/*---------------------------------------------------------*\
||| Export                                                  ||
\*---------------------------------------------------------*/
bool ExportBundle(const std::string& dest_dir,
                  const StudioDocument& workspace,
                  const PresetRegistry& registry,
                  const std::string& source_preset_dir,
                  std::vector<std::string>* errors,
                  std::vector<std::string>* warnings,
                  bool overwrite,
                  unsigned int* types_written,
                  unsigned int* assets_copied)
{
    std::error_code ec;
    const fs::path dest = fs::path(dest_dir).lexically_normal();
    const fs::path doc_path = dest / "studio.json";

    if(fs::exists(doc_path, ec) && !overwrite)
    {
        Err(errors, doc_path.string() + ": destination already"
                    " contains a bundle — pass overwrite to replace it");
        return false;
    }
    /* A non-empty dest is refused outright — bundles mix files
       with foreign content only on an explicit overwrite. */
    if(!overwrite && fs::is_directory(dest, ec))
    {
        fs::directory_iterator it(dest, ec), end;
        if(!ec && it != end)
        {
            Err(errors, dest.string() + ": destination is not"
                        " empty — pass overwrite to export here");
            return false;
        }
    }

    /*------------------------------*\
    | Collect + validate BEFORE any |
    | write — a workspace with      |
    | dangling type refs exports    |
    | nothing.                       |
    \*------------------------------*/
    std::vector<std::string> ids;
    if(!CollectUsedTypes(workspace, registry, ids, errors))
    {
        return false;
    }

    /* The sanitized candidate still goes through the normal
       document validation — a bundle never ships a doc the loader
       would reject. */
    const StudioDocument clean = SanitizeForExport(workspace);
    const nlohmann::json doc_json = ToJson(clean);
    {
        StudioDocument check;
        std::vector<std::string> verrs;
        if(!FromJson(doc_json, check, &verrs))
        {
            Err(errors, "studio.json: sanitized document failed"
                        " validation: " + JoinErrors(verrs));
            return false;
        }
    }

    /* Assets the used types reference, resolved under
       <workspace>/assets/ — missing files warn, never fail. */
    const fs::path assets_src =
        (fs::path(source_preset_dir) / ".." / ".." / "assets")
            .lexically_normal();
    struct AssetCopy { fs::path src; std::string rel; };
    std::vector<AssetCopy> copies;
    {
        std::set<std::string> seen;
        for(const std::string& id : ids)
        {
            const DevicePreset* p = registry.Find(id);
            if(p == nullptr)
            {
                continue;    /* CollectUsedTypes already errored */
            }
            for(const std::string& ref : PresetAssetRefs(*p))
            {
                if(!seen.insert(ref).second)
                {
                    continue;
                }
                if(!IsPresetAssetPath(ref))
                {
                    Warn(warnings, "asset '" + ref + "' fails the"
                                   " portable-path rules — skipped");
                    continue;
                }
                const fs::path src =
                    (assets_src / fs::path(ref)).lexically_normal();
                std::error_code fec;
                if(!fs::is_regular_file(src, fec))
                {
                    Warn(warnings, "asset '" + ref + "' not found"
                                   " under " + assets_src.string());
                    continue;
                }
                const unsigned long long sz = fs::file_size(src, fec);
                if(fec || sz > BUNDLE_MAX_ASSET_BYTES)
                {
                    Warn(warnings, "asset '" + ref + "' exceeds the"
                                   " per-asset limit — skipped");
                    continue;
                }
                copies.push_back({ src, ref });
            }
        }
    }

    /*------------------------------*\
    | Write. Anything that lands is |
    | tracked so a mid-export       |
    | failure cleans up after       |
    | itself.                        |
    \*------------------------------*/
    std::vector<fs::path> written;
    auto fail = [&](const std::string& e) {
        for(const fs::path& p : written)
        {
            fs::remove(p, ec);
        }
        Err(errors, e);
        return false;
    };

    const fs::path types_dir = dest / "presets" / "devices";
    if(!fs::create_directories(types_dir, ec) && ec)
    {
        Err(errors, types_dir.string() + ": cannot create ("
                    + ec.message() + ")");
        return false;
    }
    if(!copies.empty()
       && !fs::create_directories(dest / "assets", ec) && ec)
    {
        Err(errors, (dest / "assets").string() + ": cannot create ("
                    + ec.message() + ")");
        return false;
    }

    for(const std::string& id : ids)
    {
        const DevicePreset* p = registry.Find(id);
        if(p == nullptr)
        {
            continue;
        }
        const fs::path target =
            types_dir / (id + ".device.json");
        std::string werr;
        if(!WriteFileAtomic(target, ToJson(*p).dump(2) + "\n", werr))
        {
            return fail(werr);
        }
        written.push_back(target);
    }
    if(types_written != nullptr)
    {
        *types_written = (unsigned int)ids.size();
    }

    unsigned int copied = 0;
    for(const AssetCopy& a : copies)
    {
        const fs::path dst = dest / "assets" / fs::path(a.rel);
        std::error_code dec;
        if(!fs::create_directories(dst.parent_path(), dec) && dec)
        {
            return fail(dst.parent_path().string() + ": cannot create ("
                        + dec.message() + ")");
        }
        fs::copy_file(a.src, dst, fs::copy_options::overwrite_existing, dec);
        if(dec)
        {
            return fail(dst.string() + ": copy failed ("
                        + dec.message() + ")");
        }
        written.push_back(dst);
        copied++;
    }
    if(assets_copied != nullptr)
    {
        *assets_copied = copied;
    }

    /* Overwrite export: prune type files a previous export left
       behind — a stale bundled type would otherwise install into
       the importer's library. Only *.device.json inside
       <dest>/presets/devices/ — never outside the bundle. */
    if(overwrite && fs::is_directory(types_dir, ec))
    {
        const std::set<std::string> keep(ids.begin(), ids.end());
        for(const auto& entry : fs::directory_iterator(types_dir, ec))
        {
            if(!entry.is_regular_file())
            {
                continue;
            }
            const std::string name = entry.path().filename().string();
            const std::string suffix = ".device.json";
            if(name.size() <= suffix.size()
               || name.compare(name.size() - suffix.size(),
                               suffix.size(), suffix) != 0)
            {
                continue;
            }
            const std::string base =
                name.substr(0, name.size() - suffix.size());
            if(keep.count(base))
            {
                continue;
            }
            std::error_code rec;
            fs::remove(entry.path(), rec);
            if(rec)
            {
                Warn(warnings, "stale bundled type '" + name
                               + "' could not be removed ("
                               + rec.message() + ")");
            }
        }
    }

    /* studio.json LAST — a partial bundle without it is rejected
       by InspectBundle instead of looking complete. */
    std::string werr;
    if(!WriteFileAtomic(doc_path, doc_json.dump(2) + "\n", werr))
    {
        return fail(werr);
    }
    return true;
}

/*---------------------------------------------------------*\
||| Inspect                                                 ||
\*---------------------------------------------------------*/
/* The expanded-doc migration ConfigStore::LoadParsed runs on
   v1/v2 studio.json — same steps, Qt-free: parse the embedded
   scene, extract compact doc + types, carry the file's own
   preference sections over, re-validate as a v3 candidate. */
static bool MigrateBundledDoc(const nlohmann::json& j,
                              const std::string& source,
                              StudioDocument& doc,
                              std::vector<DevicePreset>& types,
                              std::vector<std::string>* errors,
                              std::vector<std::string>* warnings)
{
    SceneDocument scene;
    std::vector<std::string> serrs;
    if(!FromJson(j["scene"], scene, &serrs))
    {
        Err(errors, source + ": scene: " + JoinErrors(serrs));
        return false;
    }
    StudioDocument mig;
    std::vector<DevicePreset> t;
    std::vector<std::string> merrs, mwarns;
    if(!MigrateExpandedScene(scene, mig, t, &merrs, &mwarns))
    {
        Err(errors, source + ": migration: " + JoinErrors(merrs));
        return false;
    }
    for(const std::string& w : mwarns)
    {
        Warn(warnings, w);
    }
    nlohmann::json wj = ToJson(mig);
    static const char* carried[] = {
        "ui", "camera", "controls", "render", "inputs", "extensions",
    };
    for(const char* k : carried)
    {
        if(j.contains(k))
        {
            wj[k] = j[k];
        }
    }
    if(j.contains("name"))
    {
        wj["name"] = j["name"];
    }
    if(j.contains("output") && j["output"].is_object()
       && j["output"].contains("brightness"))
    {
        wj["output"]["brightness"] = j["output"]["brightness"];
    }
    if(j.contains("effects") && j["effects"].is_object())
    {
        for(auto it = j["effects"].begin(); it != j["effects"].end(); ++it)
        {
            wj["effects"][it.key()] = it.value();
        }
    }
    /* Migration never arms live output. */
    wj["output"]["live_on_startup"] = false;

    std::vector<std::string> verrs, vwarns;
    if(!FromJson(wj, doc, &verrs, &vwarns))
    {
        Err(errors, source + ": migrated document: " + JoinErrors(verrs));
        return false;
    }
    for(const std::string& w : vwarns)
    {
        Warn(warnings, w);
    }
    types = t;
    return true;
}

bool InspectBundle(const std::string& src_dir,
                   const PresetRegistry& local_registry,
                   ImportPlan& plan,
                   std::vector<std::string>* errors)
{
    std::error_code ec;
    const fs::path src = fs::path(src_dir).lexically_normal();
    const fs::path doc_path = src / "studio.json";
    if(!fs::is_directory(src, ec))
    {
        Err(errors, src_dir + ": not a directory");
        return false;
    }

    /* Everything lands in a LOCAL plan until the bundle proves
       valid — on failure `plan` is untouched. A nullptr `errors`
       is redirected to a scratch list so a malformed bundle can
       never silently pass. */
    std::vector<std::string> errs_local;
    if(errors == nullptr)
    {
        errors = &errs_local;
    }
    ImportPlan p;
    unsigned long long total = 0;
    std::string bytes, rerr;
    if(!ReadCapped(doc_path, BUNDLE_MAX_JSON_BYTES, bytes, rerr))
    {
        Err(errors, rerr);
        return false;
    }
    total += bytes.size();

    nlohmann::json j;
    try
    {
        j = nlohmann::json::parse(bytes);
    }
    catch(const std::exception& e)
    {
        Err(errors, doc_path.string() + ": invalid JSON ("
                    + std::string(e.what()) + ")");
        return false;
    }

    /*------------------------------*\
    | schema_version gate: newer    |
    | docs are rejected; older      |
    | expanded docs migrate through |
    | the same path a file load     |
    | uses.                          |
    \*------------------------------*/
    const long long version = (j.is_object()
                               && j.contains("schema_version")
                               && j["schema_version"].is_number())
        ? j["schema_version"].get<long long>()
        : -1;
    if(version > STUDIO_SCHEMA_VERSION)
    {
        Err(errors, doc_path.string() + ": schema_version "
                    + std::to_string(version) + " is newer than"
                    " supported " + std::to_string(STUDIO_SCHEMA_VERSION));
        return false;
    }

    StudioDocument           candidate;
    std::vector<DevicePreset> migrated_types;
    if(version >= 1 && version < STUDIO_SCHEMA_VERSION
       && j.is_object() && j.contains("scene"))
    {
        if(!MigrateBundledDoc(j, doc_path.string(), candidate,
                              migrated_types, errors, &p.warnings))
        {
            return false;
        }
    }
    else
    {
        std::vector<std::string> verrs, vwarns;
        if(!FromJson(j, candidate, &verrs, &vwarns))
        {
            Err(errors, doc_path.string() + ": " + JoinErrors(verrs));
            return false;
        }
        for(const std::string& w : vwarns)
        {
            Warn(&p.warnings, w);
        }
    }
    /* Re-sanitize no matter what the file claims — imported docs
       never arm live output, never mark a zone verified, and never
       carry foreign serials/locations. */
    p.workspace  = SanitizeForExport(candidate);
    p.source_dir = src.string();

    /*------------------------------*\
    | Bundled type files — each     |
    | fully validated, id ==        |
    | basename, size-capped.         |
    \*------------------------------*/
    std::map<std::string, DevicePreset> bundled;   /* sorted by id */
    const fs::path types_dir = src / "presets" / "devices";
    if(fs::is_directory(types_dir, ec))
    {
        for(const auto& entry : fs::directory_iterator(types_dir, ec))
        {
            if(!entry.is_regular_file())
            {
                continue;
            }
            const std::string name = entry.path().filename().string();
            const std::string suffix = ".device.json";
            if(name.size() <= suffix.size()
               || name.compare(name.size() - suffix.size(),
                               suffix.size(), suffix) != 0)
            {
                continue;
            }
            const std::string base =
                name.substr(0, name.size() - suffix.size());
            const fs::path path = entry.path();
            std::string tbytes, terr;
            if(!ReadCapped(path, BUNDLE_MAX_JSON_BYTES, tbytes, terr))
            {
                Err(errors, terr);
                continue;
            }
            total += tbytes.size();
            nlohmann::json tj;
            try
            {
                tj = nlohmann::json::parse(tbytes);
            }
            catch(const std::exception& e)
            {
                Err(errors, path.string() + ": invalid JSON ("
                            + std::string(e.what()) + ")");
                continue;
            }
            DevicePreset p;
            std::vector<std::string> perrs;
            if(!DevicePresetFromJson(tj, p, &perrs))
            {
                Err(errors, path.string() + ": " + JoinErrors(perrs));
                continue;
            }
            if(p.id != base)
            {
                Err(errors, path.string() + ": id '" + p.id
                            + "' does not match file name '" + base + "'");
                continue;
            }
            bundled[p.id] = p;
        }
    }
    /* Types a migrated expanded doc extracted are bundled content
       too — a bundled file wins on identical content, a divergent
       pair makes the bundle malformed. */
    for(const DevicePreset& p : migrated_types)
    {
        const auto it = bundled.find(p.id);
        if(it == bundled.end())
        {
            bundled[p.id] = p;
        }
        else if(ToJson(it->second) != ToJson(p))
        {
            Err(errors, "type '" + p.id + "' differs between the"
                        " migrated document and its bundled file");
        }
    }
    if(errors != nullptr && !errors->empty())
    {
        return false;
    }

    /*------------------------------*\
    | Dependency closure: every     |
    | bundled type's refs and every |
    | workspace devices.*.type must |
    | resolve inside the bundle or  |
    | the local library — after any |
    | renames ApplyImport makes.    |
    \*------------------------------*/
    const std::vector<std::string> lids = local_registry.Ids();
    p.local_ids = std::set<std::string>(lids.begin(), lids.end());
    auto resolvable = [&](const std::string& id) {
        return bundled.count(id) != 0 || p.local_ids.count(id) != 0;
    };
    for(const auto& kv : bundled)
    {
        for(const std::string& dep : PresetDependencies(kv.second))
        {
            if(!resolvable(dep))
            {
                Err(errors, "presets/devices/" + kv.first
                            + ".device.json: missing dependency '"
                            + dep + "'");
            }
        }
    }
    for(const auto& kv : p.workspace.devices)
    {
        if(!resolvable(kv.second.type))
        {
            Err(errors, "devices." + kv.first + ": type '"
                        + kv.second.type + "' is neither bundled"
                        " nor available locally");
        }
    }
    if(errors != nullptr && !errors->empty())
    {
        return false;
    }

    /*------------------------------*\
    | Inspect-time resolution: the  |
    | candidate must actually       |
    | resolve against a scratch     |
    | registry — local types with   |
    | the bundle overlaid — BEFORE  |
    | apply ever runs. This catches |
    | failure modes the closure     |
    | check can't (resolve-depth    |
    | problems, object/emitter cap  |
    | breaches, reference cycles)   |
    | and is the same resolver the  |
    | post-import apply path uses,  |
    | so "malformed never reaches   |
    | apply" holds at inspect time. |
    \*------------------------------*/
    {
        PresetRegistry scratch = local_registry;
        /* Topo-add bundled types: Add re-validates the whole
           registry, so only attempt a type once its bundled deps
           have landed. Types left pending after a no-progress pass
           close a reference cycle — a malformed-bundle error.
           (A bundled def overlaid on a same-id local type is the
           post-import content for resolution purposes: conflicts
           import under a new id with all refs remapped to it.) */
        std::set<std::string> pending;
        for(const auto& kv : bundled)
        {
            pending.insert(kv.first);
        }
        for(bool progress = true; progress && !pending.empty();)
        {
            progress = false;
            for(auto it = pending.begin(); it != pending.end();)
            {
                bool ready = true;
                for(const std::string& dep :
                    PresetDependencies(bundled[*it]))
                {
                    if(pending.count(dep))
                    {
                        ready = false;
                        break;
                    }
                }
                if(!ready)
                {
                    ++it;
                    continue;
                }
                std::vector<std::string> aerrs;
                if(!scratch.Add(bundled[*it], &aerrs))
                {
                    Err(errors, "presets/devices/" + *it
                                + ".device.json: "
                                + JoinErrors(aerrs));
                }
                it = pending.erase(it);
                progress = true;
            }
        }
        for(const std::string& id : pending)
        {
            Err(errors, "presets/devices/" + id + ".device.json:"
                        " unresolvable type-reference cycle");
        }
        if(errors != nullptr && !errors->empty())
        {
            return false;
        }
        SceneDocument resolved;
        std::vector<std::string> rerrs, rwarns;
        if(!ResolveScene(p.workspace, scratch, resolved,
                         &rerrs, &rwarns))
        {
            Err(errors, src_dir + ": workspace does not resolve"
                        " against bundled+local types: "
                        + JoinErrors(rerrs));
            return false;
        }
        for(const std::string& w : rwarns)
        {
            Warn(&p.warnings, w);
        }
    }

    /*------------------------------*\
    | Classify vs the local library.|
    \*------------------------------*/
    for(const auto& kv : bundled)
    {
        BundleType bt;
        bt.id     = kv.first;
        bt.preset = kv.second;
        const DevicePreset* local = local_registry.Find(kv.first);
        if(local == nullptr)
        {
            bt.status = BundleType::Status::New;
        }
        else
        {
            bt.has_local = true;
            bt.local     = *local;
            bt.status    = (ToJson(*local) == ToJson(kv.second))
                ? BundleType::Status::Identical
                : BundleType::Status::Conflict;
        }
        p.types.push_back(bt);
    }

    /*------------------------------*\
    | Bundled assets: local rel     |
    | paths only, per-file + total  |
    | caps.                          |
    \*------------------------------*/
    const fs::path assets_dir = src / "assets";
    if(fs::is_directory(assets_dir, ec))
    {
        for(const auto& entry :
            fs::recursive_directory_iterator(assets_dir, ec))
        {
            if(!entry.is_regular_file())
            {
                continue;
            }
            const std::string rel =
                fs::relative(entry.path(), assets_dir, ec)
                    .generic_string();
            if(ec || !IsPresetAssetPath(rel))
            {
                Err(errors, entry.path().string() + ": not a portable"
                            " asset path");
                continue;
            }
            const unsigned long long sz =
                fs::file_size(entry.path(), ec);
            if(ec || sz > BUNDLE_MAX_ASSET_BYTES)
            {
                Err(errors, entry.path().string() + ": exceeds the"
                            " per-asset limit");
                continue;
            }
            total += sz;
            p.assets.push_back(rel);
        }
        std::sort(p.assets.begin(), p.assets.end());
    }
    if(total > BUNDLE_MAX_TOTAL_BYTES)
    {
        Err(errors, src_dir + ": bundle exceeds the "
                    + std::to_string(BUNDLE_MAX_TOTAL_BYTES / 1024 / 1024)
                    + " MB total limit");
        return false;
    }
    if(errors != nullptr && !errors->empty())
    {
        return false;
    }

    /* Referenced assets that aren't in the bundle must exist
       locally — surface that expectation now rather than at
       resolve. */
    {
        std::set<std::string> have(p.assets.begin(), p.assets.end());
        std::set<std::string> seen;
        for(const BundleType& bt : p.types)
        {
            for(const std::string& ref : PresetAssetRefs(bt.preset))
            {
                if(seen.insert(ref).second && !have.count(ref))
                {
                    Warn(&p.warnings, "asset '" + ref + "' referenced by"
                                         " '" + bt.id + "' is not in the"
                                         " bundle — a local copy is needed");
                }
            }
        }
    }
    plan = p;
    return true;
}

/*---------------------------------------------------------*\
||| Apply                                                   ||
\*---------------------------------------------------------*/
bool ApplyImport(const ImportPlan& plan,
                 const std::map<std::string, std::string>& choices,
                 const std::string& target_preset_dir,
                 StudioDocument& out,
                 std::vector<std::string>* errors,
                 std::vector<std::string>* warnings)
{
    namespace fs = std::filesystem;
    std::error_code ec;

    /*------------------------------*\
    | Resolve the rename map:       |
    | every Conflict id needs an    |
    | import_as choice; nothing     |
    | else may appear in choices.   |
    \*------------------------------*/
    std::map<std::string, std::string> renames;
    std::set<std::string>              taken = plan.local_ids;
    for(const BundleType& bt : plan.types)
    {
        taken.insert(bt.id);   /* bundle ids are taken too */
    }
    for(const auto& kv : choices)
    {
        const BundleType* bt = nullptr;
        for(const BundleType& t : plan.types)
        {
            if(t.id == kv.first) { bt = &t; }
        }
        if(bt == nullptr || bt->status != BundleType::Status::Conflict)
        {
            Err(errors, "choices: '" + kv.first + "' is not a"
                        " same-id conflict in this bundle");
            return false;
        }
    }
    for(const BundleType& bt : plan.types)
    {
        if(bt.status != BundleType::Status::Conflict)
        {
            continue;
        }
        const auto it = choices.find(bt.id);
        if(it == choices.end() || it->second.empty())
        {
            Err(errors, "type '" + bt.id + "' differs from the local"
                        " definition — an import id is required");
            return false;
        }
        const std::string& nid = it->second;
        if(!IsPresetId(nid))
        {
            Err(errors, "choices." + bt.id + ": '" + nid
                        + "' is not a valid type id");
            return false;
        }
        if(nid == bt.id || taken.count(nid))
        {
            Err(errors, "choices." + bt.id + ": '" + nid
                        + "' collides with an existing type id");
            return false;
        }
        renames[bt.id] = nid;
        taken.insert(nid);
    }

    /* Derived renames: an "identical" type whose child refs point
       at a renamed type is no longer identical once remapped — it
       imports under a derived free id and its own referrers must
       remap too. Iterate to a fixpoint; each pass adds at least
       one rename, so it terminates. */
    for(;;)
    {
        bool progress = false;
        for(const BundleType& bt : plan.types)
        {
            if(bt.status != BundleType::Status::Identical
               || renames.count(bt.id))
            {
                continue;
            }
            DevicePreset remapped = bt.preset;
            ApplyRenames(remapped, renames);
            if(ToJson(remapped) != ToJson(bt.local))
            {
                const std::string nid = DerivedId(bt.id, taken);
                taken.insert(nid);
                renames[bt.id] = nid;
                Warn(warnings, "type '" + bt.id + "' imported as '" + nid
                               + "' (its dependency was renamed)");
                progress = true;
            }
        }
        if(!progress)
        {
            break;
        }
    }

    /*------------------------------*\
    | Final content per type: refs  |
    | remapped, id set, revalidated.|
    | Identical-and-unchanged types |
    | are pure reuse — no write.    |
    \*------------------------------*/
    struct Write { DevicePreset p; fs::path target; };
    std::vector<Write> writes;
    const fs::path dir = fs::path(target_preset_dir).lexically_normal();
    for(const BundleType& bt : plan.types)
    {
        DevicePreset p = bt.preset;
        ApplyRenames(p, renames);
        const auto rit = renames.find(bt.id);
        const bool renamed  = rit != renames.end();
        const bool reusable = !renamed
            && bt.status == BundleType::Status::Identical
            && ToJson(p) == ToJson(bt.local);
        if(reusable)
        {
            continue;
        }
        p.id = renamed ? rit->second : bt.id;
        /* The file layer is validate-then-write: the exact bytes
           headed to disk must parse back. */
        const nlohmann::json pj = ToJson(p);
        DevicePreset check;
        std::vector<std::string> verrs;
        if(!DevicePresetFromJson(pj, check, &verrs) || check.id != p.id)
        {
            Err(errors, "type '" + p.id + "': remapped definition"
                        " failed validation: " + JoinErrors(verrs));
            return false;
        }
        writes.push_back({ p, dir / (p.id + ".device.json") });
    }

    /* Pre-flight: never overwrite a local type file. A same-content
       file already on disk is a reuse hit (idempotent re-import);
       different content is an error — someone changed the library
       between inspect and apply. */
    for(Write& w : writes)
    {
        if(!fs::exists(w.target, ec))
        {
            continue;
        }
        DevicePreset existing;
        if(DevicePresetFromJsonFile(w.target.string(), existing, nullptr)
           && ToJson(existing) == ToJson(w.p))
        {
            w.target.clear();    /* already correct — reuse */
            continue;
        }
        Err(errors, w.target.string() + ": a different local type"
                    " already occupies this file — not overwriting");
        return false;
    }

    /*------------------------------*\
    | Write the new-id type files.  |
    \*------------------------------*/
    if(!fs::create_directories(dir, ec) && ec)
    {
        Err(errors, dir.string() + ": cannot create (" + ec.message() + ")");
        return false;
    }
    for(const Write& w : writes)
    {
        if(w.target.empty())
        {
            continue;
        }
        std::string werr;
        if(!WriteFileAtomic(w.target, ToJson(w.p).dump(2) + "\n", werr))
        {
            Err(errors, werr);
            return false;
        }
        /* Validate the file that actually landed. */
        DevicePreset landed;
        std::vector<std::string> verrs;
        if(!DevicePresetFromJsonFile(w.target.string(), landed, &verrs)
           || landed.id != w.p.id)
        {
            fs::remove(w.target, ec);
            Err(errors, w.target.string() + ": written type failed"
                        " validation: "
                        + (verrs.empty() ? "id mismatch" : verrs.front()));
            return false;
        }
    }

    /*------------------------------*\
    | Bundled assets land under     |
    | <workspace>/assets/ — never  |
    | overwriting a local file.     |
    \*------------------------------*/
    const fs::path assets_dst =
        (dir / ".." / ".." / "assets").lexically_normal();
    const fs::path assets_src = fs::path(plan.source_dir) / "assets";
    for(const std::string& rel : plan.assets)
    {
        const fs::path dst = assets_dst / fs::path(rel);
        if(fs::exists(dst, ec))
        {
            Warn(warnings, "asset '" + rel + "' exists locally — kept");
            continue;
        }
        std::error_code cec;
        if(!fs::create_directories(dst.parent_path(), cec) && cec)
        {
            Err(errors, dst.parent_path().string() + ": cannot create ("
                        + cec.message() + ")");
            return false;
        }
        fs::copy_file(assets_src / fs::path(rel), dst,
                      fs::copy_options::none, cec);
        if(cec)
        {
            Err(errors, dst.string() + ": copy failed (" + cec.message() + ")");
            return false;
        }
    }

    /*------------------------------*\
    | The workspace candidate: refs |
    | remapped, sanitize re-applied |
    | (idempotent) — still NOT      |
    | activated; the caller runs    |
    | the normal resolve path.      |
    \*------------------------------*/
    StudioDocument candidate = SanitizeForExport(plan.workspace);
    for(auto& kv : candidate.devices)
    {
        const auto it = renames.find(kv.second.type);
        if(it != renames.end())
        {
            kv.second.type = it->second;
        }
    }
    out = candidate;
    return true;
}

} /* namespace studio */
