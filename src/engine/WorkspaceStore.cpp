/*---------------------------------------------------------*\
|||| WorkspaceStore.cpp                                      ||
||||                                                           |
||||   SPDX-License-Identifier: GPL-2.0-or-later               ||
\*---------------------------------------------------------*/

#include "WorkspaceStore.h"

#include "config/StudioConfig.h"
#include "presets/DevicePreset.h"
#include "presets/PresetBundle.h"   /* studio::CollectUsedTypes */
#include "scene/SceneResolver.h"

#include <windows.h>               /* MoveFileExA              */
#include <io.h>                    /* _commit                  */

#include <algorithm>
#include <cctype>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <utility>

namespace openrgb2
{

namespace
{

namespace fs = std::filesystem;

constexpr const char* kMarkerKind   = "openrgb2-workspace-active";
constexpr const char* kManifestKind = "openrgb2-workspace-generation";
constexpr int         kRecordSchema = 1;

/*---------------------------------------------------------*\
|||| Small file primitives — the same atomicity contract  ||
|||| ProfileStore keeps: write temp, flush, _commit to     ||
|||| stable storage, then rename with write-through.      ||
\*---------------------------------------------------------*/

bool ReadFileBytes(const fs::path& path, std::string& out)
{
    std::ifstream in(path, std::ios::binary);
    if(!in)
    {
        return false;
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    out = ss.str();
    return true;
}

/* Plain committed write (no rename) — used for files staged
   inside a generation directory; the directory rename is the
   atomic publish. */
bool WriteFileCommitted(const fs::path& path, const std::string& bytes,
                        std::string& err)
{
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    if(ec)
    {
        err = "cannot create " + path.parent_path().string() +
              ": " + ec.message();
        return false;
    }
    FILE* f = nullptr;
    if(fopen_s(&f, path.string().c_str(), "wb") != 0 || f == nullptr)
    {
        err = "cannot open " + path.string() + " for writing";
        return false;
    }
    bool ok = true;
    if(!bytes.empty() &&
       fwrite(bytes.data(), 1, bytes.size(), f) != bytes.size())
    {
        err = "short write to " + path.string();
        ok  = false;
    }
    if(ok && fflush(f) != 0)
    {
        err = "flush failed for " + path.string();
        ok  = false;
    }
    if(ok && _commit(_fileno(f)) != 0)
    {
        err = "commit failed for " + path.string();
        ok  = false;
    }
    fclose(f);
    if(!ok)
    {
        fs::remove(path, ec);
    }
    return ok;
}

/* temp + commit + rename — the marker's atomic replace. */
bool WriteAtomicFile(const fs::path& tmp, const fs::path& dst,
                     const std::string& bytes, std::string& err)
{
    if(!WriteFileCommitted(tmp, bytes, err))
    {
        return false;
    }
    if(!MoveFileExA(tmp.string().c_str(), dst.string().c_str(),
                    MOVEFILE_REPLACE_EXISTING |
                    MOVEFILE_WRITE_THROUGH))
    {
        err = "rename of " + tmp.string() + " over " +
              dst.string() + " failed (win32 " +
              std::to_string(GetLastError()) + ")";
        std::error_code ec;
        fs::remove(tmp, ec);
        return false;
    }
    return true;
}

/*---------------------------------------------------------*\
|||| Content hashing — the manifest pins every staged     ||
|||| file so an interrupted install is detectable.        ||
\*---------------------------------------------------------*/

std::string HashBytes(const std::string& bytes)
{
    uint64_t h = 1469598103934665603ull;   /* FNV-1a 64 */
    for(unsigned char c : bytes)
    {
        h ^= c;
        h *= 1099511628211ull;
    }
    char buf[32];
    std::snprintf(buf, sizeof(buf), "fnv1a64:%016" PRIx64, h);
    return buf;
}

nlohmann::json FileEntry(const std::string& rel, const std::string& bytes)
{
    return nlohmann::json::object({
        { "file",  rel                     },
        { "sha",   HashBytes(bytes)        },
        { "bytes", (uint64_t)bytes.size()  },
    });
}

/* Verify one manifest file entry exists on disk with matching
   size + hash. */
bool VerifyEntry(const fs::path& dir, const nlohmann::json& entry,
                 const std::string& what, std::string& err)
{
    const std::string rel = entry.value("file", std::string());
    const std::string sha = entry.value("sha",  std::string());
    if(rel.empty() || sha.empty())
    {
        err = what + ": manifest entry missing file/sha";
        return false;
    }
    std::string bytes;
    if(!ReadFileBytes(dir / fs::path(rel), bytes))
    {
        err = what + ": '" + rel + "' referenced by the manifest "
              "is missing";
        return false;
    }
    if(HashBytes(bytes) != sha)
    {
        err = what + ": '" + rel + "' content does not match the "
              "manifest hash";
        return false;
    }
    return true;
}

/*---------------------------------------------------------*\
|||| The active-generation marker                          ||
\*---------------------------------------------------------*/

struct Marker
{
    bool        exists = false;
    bool        valid  = false;
    std::string bytes;
    std::string generation;                 /* "gen-NNNNNN" */
    uint64_t    gen_num            = 0;
    uint64_t    workspace_revision = 0;
    uint64_t    resource_revision  = 0;
    std::string error;
};

std::string GenerationName(uint64_t n)
{
    char buf[32];
    std::snprintf(buf, sizeof(buf), "gen-%06llu",
                  (unsigned long long)n);
    return buf;
}

bool GenerationOrdinal(const std::string& name, uint64_t& out)
{
    if(name.rfind("gen-", 0) != 0)
    {
        return false;
    }
    const std::string num = name.substr(4);
    if(num.empty())
    {
        return false;
    }
    for(char c : num)
    {
        if(c < '0' || c > '9')
        {
            return false;
        }
    }
    out = (uint64_t)std::strtoull(num.c_str(), nullptr, 10);
    return out > 0;
}

Marker ReadMarker(const fs::path& path)
{
    Marker m;
    std::error_code ec;
    if(!fs::exists(path, ec))
    {
        return m;
    }
    m.exists = true;
    if(!ReadFileBytes(path, m.bytes))
    {
        m.error = "unreadable";
        return m;
    }
    const nlohmann::json j =
        nlohmann::json::parse(m.bytes, nullptr, false);
    if(j.is_discarded() || !j.is_object())
    {
        m.error = "not valid JSON";
        return m;
    }
    const auto get_u64 = [&](const char* k, uint64_t& out) -> bool
    {
        const auto it = j.find(k);
        if(it == j.end() || !it->is_number_unsigned())
        {
            return false;
        }
        out = it->get<uint64_t>();
        return true;
    };
    const auto kit = j.find("kind");
    const auto vit = j.find("schema_version");
    const auto git = j.find("generation");
    if(kit == j.end() || !kit->is_string() ||
       kit->get<std::string>() != kMarkerKind ||
       vit == j.end() || !vit->is_number_integer() ||
       vit->get<long long>() != kRecordSchema ||
       git == j.end() || !git->is_string() ||
       !GenerationOrdinal(git->get<std::string>(), m.gen_num) ||
       !get_u64("workspace_revision", m.workspace_revision) ||
       !get_u64("resource_revision",  m.resource_revision))
    {
        m.error = "malformed marker record";
        return m;
    }
    m.generation = git->get<std::string>();
    m.valid = true;
    return m;
}

/*---------------------------------------------------------*\
|||| Managed-root containment — cleanup ever only runs    ||
|||| INSIDE the verified workspace root.                  ||
\*---------------------------------------------------------*/

std::string Canon(const fs::path& p)
{
    std::error_code ec;
    fs::path c = fs::weakly_canonical(p, ec);
    if(ec)
    {
        c = p.lexically_normal();
    }
    std::string s = c.string();
    std::transform(s.begin(), s.end(), s.begin(),
        [](unsigned char ch) { return (char)std::tolower(ch); });
    std::replace(s.begin(), s.end(), '/', '\\');
    return s;
}

bool InsideRoot(const fs::path& root, const fs::path& p)
{
    const std::string r = Canon(root);
    const std::string c = Canon(p);
    return c.size() > r.size() &&
           c.compare(0, r.size(), r) == 0 &&
           (c[r.size()] == '\\' || c[r.size()] == '/');
}

/*---------------------------------------------------------*\
|||| Generation scan / orphan cleanup                      ||
\*---------------------------------------------------------*/

std::vector<std::pair<uint64_t, std::string>> GenerationDirs(
    const fs::path& wsdir)
{
    std::vector<std::pair<uint64_t, std::string>> out;
    std::error_code ec;
    const fs::path gens = wsdir / "generations";
    if(!fs::is_directory(gens, ec))
    {
        return out;
    }
    for(const auto& e : fs::directory_iterator(gens, ec))
    {
        uint64_t n = 0;
        if(e.is_directory() &&
           GenerationOrdinal(e.path().filename().string(), n))
        {
            out.push_back({ n, e.path().filename().string() });
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

/* Remove orphaned state inside the verified workspace dir:
   a torn staging dir, marker temps, and generation dirs no
   marker references. Never reaches outside `wsdir`. */
void CleanupOrphans(const fs::path& wsdir,
                    const std::set<std::string>& keep)
{
    std::error_code ec;
    fs::remove_all(wsdir / "staging", ec);
    for(const char* tmp : { "active.tmp", "active.lkg.tmp",
                            "active.rejected.tmp" })
    {
        fs::remove(wsdir / tmp, ec);
    }
    for(const auto& kv : GenerationDirs(wsdir))
    {
        if(keep.find(kv.second) == keep.end())
        {
            fs::remove_all(wsdir / "generations" / kv.second, ec);
        }
    }
}

/* Bounded retention: marker-referenced generations are never
   pruned; the whole retained window — references included —
   stays within `bound`, newest first. (References are always
   the newest pair, so in practice unreferenced history fills
   the remaining slots.) */
void PruneGenerations(const fs::path& wsdir,
                      const std::set<std::string>& keep,
                      unsigned int bound)
{
    auto gens = GenerationDirs(wsdir);
    std::error_code ec;
    unsigned int kept = 0;
    for(auto it = gens.rbegin(); it != gens.rend(); ++it)
    {
        if(keep.find(it->second) != keep.end() || kept < bound)
        {
            ++kept;
            continue;
        }
        fs::remove_all(wsdir / "generations" / it->second, ec);
    }
}

/*---------------------------------------------------------*\
|||| Manifest                                              ||
\*---------------------------------------------------------*/

struct Manifest
{
    uint64_t       gen_num = 0;
    std::string    source_form;
    nlohmann::json resources;      /* {"devices":{},"effects":{}} */
};

bool ReadManifest(const fs::path& gendir, Manifest& out,
                  std::string& err)
{
    std::string bytes;
    if(!ReadFileBytes(gendir / "manifest.json", bytes))
    {
        err = "manifest.json missing or unreadable";
        return false;
    }
    const nlohmann::json j =
        nlohmann::json::parse(bytes, nullptr, false);
    if(j.is_discarded() || !j.is_object())
    {
        err = "manifest is not valid JSON";
        return false;
    }
    if(j.value("kind", std::string()) != kManifestKind ||
       j.value("schema_version", -1) != kRecordSchema)
    {
        err = "unrecognised manifest record";
        return false;
    }
    if(!j.count("generation") || !j["generation"].is_string() ||
       !GenerationOrdinal(j["generation"].get<std::string>(),
                          out.gen_num))
    {
        err = "manifest carries no valid generation id";
        return false;
    }
    out.source_form = j.value("source_form", std::string());
    out.resources   = j.count("resources") && j["resources"].is_object()
        ? j["resources"] : nlohmann::json::object();
    return true;
}

/* A generation is usable only when its manifest exists and
   every referenced file verifies — an interrupted install is
   never selected. */
bool VerifyGeneration(const fs::path& gendir, Manifest& m,
                      std::string& err)
{
    std::string mbytes;
    if(!ReadFileBytes(gendir / "manifest.json", mbytes))
    {
        err = "manifest.json missing or unreadable";
        return false;
    }
    const nlohmann::json j =
        nlohmann::json::parse(mbytes, nullptr, false);
    if(!ReadManifest(gendir, m, err))
    {
        return false;
    }
    if(!j.count("document") || !j["document"].is_object() ||
       !VerifyEntry(gendir, j["document"], "document", err))
    {
        if(err.empty())
        {
            err = "manifest lacks a document entry";
        }
        return false;
    }
    if(j.count("original") && j["original"].is_object() &&
       !VerifyEntry(gendir, j["original"], "original", err))
    {
        return false;
    }
    for(const char* sect : { "devices", "effects" })
    {
        if(!m.resources.count(sect) ||
           !m.resources[sect].is_object())
        {
            continue;
        }
        for(auto it = m.resources[sect].begin();
            it != m.resources[sect].end(); ++it)
        {
            if(!VerifyEntry(gendir, it.value(),
                            std::string("resources.") + sect + "." +
                            it.key(), err))
            {
                return false;
            }
        }
    }
    return true;
}

/*---------------------------------------------------------*\
|||| Collision-aware library install — the                 ||
|||| ConfigStore::InstallTypes discipline: an existing     ||
|||| local file is NEVER overwritten; identical content is ||
|||| a reuse hit; different content lands under a suffixed||
|||| id with every reference remapped.                    ||
\*---------------------------------------------------------*/

void RemapTypeReferences(studio::StudioDocument& doc,
    const std::map<std::string, std::string>& rename)
{
    if(rename.empty())
    {
        return;
    }
    for(auto& kv : doc.devices)
    {
        const auto it = rename.find(kv.second.type);
        if(it != rename.end())
        {
            kv.second.type = it->second;
        }
    }
    for(std::string& fav : doc.meta.ui.favorites)
    {
        const auto it = rename.find(fav);
        if(it != rename.end())
        {
            fav = it->second;
        }
    }
}

/* Remap `type` refs inside not-yet-installed extracted types —
   an extracted child-entity ref to a renamed type must follow
   the rename (the cascade). */
void RemapTypeBody(std::vector<studio::DevicePreset>& types,
    const std::map<std::string, std::string>& rename)
{
    for(studio::DevicePreset& p : types)
    {
        for(auto& kv : p.entities)
        {
            const auto it = rename.find(kv.second.type);
            if(it != rename.end() && !kv.second.type.empty())
            {
                kv.second.type = it->second;
            }
        }
    }
}

std::string FreeTypeId(const fs::path& dir,
                       const studio::PresetRegistry& base,
                       const std::set<std::string>& claimed,
                       const std::string& base_id)
{
    for(unsigned int n = 2;; ++n)
    {
        const std::string id = base_id + "-" + std::to_string(n);
        std::error_code ec;
        if(claimed.find(id) == claimed.end() &&
           !fs::exists(dir / (id + ".device.json"), ec) &&
           !base.Contains(id))
        {
            return id;
        }
    }
}

/* Install extracted types into the managed device library.
   `doc` type references are remapped onto whatever id each
   type actually landed under. */
bool InstallDeviceTypes(const fs::path& dir,
                        const studio::PresetRegistry& base,
                        studio::StudioDocument& doc,
                        std::vector<studio::DevicePreset>& extracted,
                        std::vector<std::string>& warnings,
                        std::string& err)
{
    if(extracted.empty())
    {
        return true;
    }
    std::error_code ec;
    fs::create_directories(dir, ec);
    if(ec)
    {
        err = "cannot create " + dir.string() + ": " + ec.message();
        return false;
    }

    /* Pass 1 — resolve the landing id for every extracted type. */
    std::map<std::string, std::string> rename;
    std::set<std::string> claimed;
    std::vector<studio::DevicePreset> to_write;
    for(studio::DevicePreset t : extracted)
    {
        const std::string want = t.id;
        if(!studio::IsPresetId(want))
        {
            warnings.push_back("extracted type id '" + want +
                               "' is not a valid identifier — "
                               "dropped");
            continue;
        }
        const bool first_claim = claimed.insert(want).second;
        const fs::path target = dir / (want + ".device.json");
        const studio::DevicePreset* local = nullptr;
        studio::DevicePreset file_preset;
        bool have_local = false;
        if(fs::exists(target, ec))
        {
            if(studio::DevicePresetFromJsonFile(target.string(),
                                              file_preset, nullptr))
            {
                local      = &file_preset;
                have_local = true;
            }
            else
            {
                /* An unparseable local file counts as different
                   content — never overwritten. */
                have_local = true;
            }
        }
        else if(const studio::DevicePreset* p = base.Find(want))
        {
            file_preset = *p;
            local       = &file_preset;
            have_local  = true;
        }

        if(have_local && local != nullptr &&
           studio::ToJson(*local) == studio::ToJson(t))
        {
            continue;   /* identical — reuse, nothing to write */
        }
        if(!have_local && first_claim)
        {
            to_write.push_back(t);
            continue;
        }
        /* Same id, different content (or a duplicate inside the
           same import batch) — suffixed landing. */
        const std::string landed =
            FreeTypeId(dir, base, claimed, want);
        claimed.insert(landed);
        rename[want] = landed;
        warnings.push_back("type '" + want +
                           "' collides with local content — "
                           "imported as '" + landed + "'");
        t.id = landed;
        to_write.push_back(t);
    }

    /* Cascades + document references follow the renames. */
    RemapTypeBody(to_write, rename);
    RemapTypeReferences(doc, rename);

    /* Pass 2 — serialize, re-validate, atomic write, verify the
       landed file (same discipline ApplyImport keeps). */
    for(const studio::DevicePreset& p : to_write)
    {
        const nlohmann::json pj = studio::ToJson(p);
        studio::DevicePreset check;
        std::vector<std::string> verrs;
        if(!studio::DevicePresetFromJson(pj, check, &verrs))
        {
            err = "extracted type '" + p.id +
                  "' failed re-validation: " +
                  (verrs.empty() ? std::string("invalid")
                                 : verrs.front());
            return false;
        }
        const std::string bytes = pj.dump(2);
        const fs::path target = dir / (p.id + ".device.json");
        if(fs::exists(target, ec))
        {
            /* Another writer landed a same-id file mid-commit —
               still never overwritten. */
            err = "type file '" + p.id +
                  ".device.json' appeared during install";
            return false;
        }
        if(!WriteAtomicFile(target.string() + ".tmp", target,
                            bytes, err))
        {
            return false;
        }
        studio::DevicePreset landed;
        if(!studio::DevicePresetFromJsonFile(target.string(),
                                           landed, &verrs) ||
           studio::ToJson(landed) != pj)
        {
            err = "installed type '" + p.id +
                  "' failed read-back verification";
            return false;
        }
    }
    return true;
}

std::string FreeEffectId(const fs::path& dir,
                         const studio::EffectRegistry& base,
                         const std::set<std::string>& claimed,
                         const std::string& base_id)
{
    for(unsigned int n = 2;; ++n)
    {
        const std::string id = base_id + "-" + std::to_string(n);
        std::error_code ec;
        if(claimed.find(id) == claimed.end() &&
           !fs::exists(dir / (id + ".effect.json"), ec) &&
           !base.Contains(id))
        {
            return id;
        }
    }
}

bool InstallEffectLooks(const fs::path& dir,
                        const studio::EffectRegistry& base,
                        studio::StudioDocument& doc,
                        std::vector<studio::EffectDocument>& extracted,
                        std::vector<std::string>& warnings,
                        std::string& err)
{
    if(extracted.empty())
    {
        return true;
    }
    std::error_code ec;
    fs::create_directories(dir, ec);
    if(ec)
    {
        err = "cannot create " + dir.string() + ": " + ec.message();
        return false;
    }
    std::set<std::string> claimed;
    for(studio::EffectDocument d : extracted)
    {
        const std::string want = d.id;
        if(!studio::IsPresetId(want))
        {
            warnings.push_back("effect look id '" + want +
                               "' is not a valid identifier — "
                               "dropped");
            continue;
        }
        const fs::path target = dir / (want + ".effect.json");
        bool        have_local = false;
        bool        identical  = false;
        std::string bytes      = studio::EffectDocumentToJson(d).dump(2);
        if(fs::exists(target, ec))
        {
            have_local = true;
            std::string existing;
            if(ReadFileBytes(target, existing))
            {
                const nlohmann::json ej =
                    nlohmann::json::parse(existing, nullptr, false);
                studio::EffectDocument ed;
                if(!ej.is_discarded() &&
                   studio::EffectDocumentFromJson(ej, ed, nullptr))
                {
                    identical = studio::EffectDocumentToJson(ed) ==
                                studio::EffectDocumentToJson(d);
                }
            }
        }
        else if(const studio::EffectDocument* p = base.Find(want))
        {
            have_local = true;
            identical  = studio::EffectDocumentToJson(*p) ==
                         studio::EffectDocumentToJson(d);
        }
        if(have_local && identical)
        {
            continue;   /* reuse */
        }
        std::string landed = want;
        if(have_local)
        {
            landed = FreeEffectId(dir, base, claimed, want);
            warnings.push_back("effect look '" + want +
                               "' collides with local content — "
                               "imported as '" + landed + "'");
            d.id = landed;
            bytes = studio::EffectDocumentToJson(d).dump(2);
            if(doc.effect.preset == want)
            {
                doc.effect.preset = landed;
            }
        }
        claimed.insert(landed);
        if(!WriteAtomicFile(target.parent_path() /
                                (landed + ".effect.json.tmp"),
                            dir / (landed + ".effect.json"),
                            bytes, err))
        {
            return false;
        }
    }
    return true;
}

} /* anonymous namespace */

/*---------------------------------------------------------*\
|||| WorkspaceStore                                          ||
\*---------------------------------------------------------*/

WorkspaceStore::WorkspaceStore(std::string root, Deps deps)
    : root_(std::move(root)), deps_(std::move(deps))
{
}

std::string WorkspaceStore::DefaultRoot()
{
    /* Development default (plan section 3.2): packaged
       launchers pass a writable app-data root instead. */
    return "profiles/local/workspaces";
}

bool WorkspaceStore::IsWorkspaceId(const std::string& id)
{
    /* Portable single-segment charset — the same rule the
       resource ids follow; "library" is the shared managed
       library dir, never a workspace. */
    return studio::IsPresetId(id) && id != "library";
}

std::string WorkspaceStore::WorkspaceDir(const std::string& id) const
{
    return (fs::path(root_) / id).string();
}

std::string WorkspaceStore::LibraryDevicesDir() const
{
    return (fs::path(root_) / "library" / "devices").string();
}

std::string WorkspaceStore::LibraryEffectsDir() const
{
    return (fs::path(root_) / "library" / "effects").string();
}

studio::PresetRegistry WorkspaceStore::EffectiveDeviceRegistry() const
{
    studio::PresetRegistry reg = deps_.devices;
    std::error_code ec;
    if(fs::is_directory(LibraryDevicesDir(), ec))
    {
        reg.LoadDirectory(LibraryDevicesDir(), nullptr);
    }
    return reg;
}

studio::EffectRegistry WorkspaceStore::EffectiveEffectRegistry() const
{
    studio::EffectRegistry reg = deps_.effects;
    std::error_code ec;
    if(fs::is_directory(LibraryEffectsDir(), ec))
    {
        reg.LoadDirectory(LibraryEffectsDir(), nullptr);
    }
    return reg;
}

std::vector<std::string> WorkspaceStore::List() const
{
    std::vector<std::string> out;
    std::error_code ec;
    if(!fs::is_directory(root_, ec))
    {
        return out;
    }
    for(const auto& e : fs::directory_iterator(root_, ec))
    {
        if(!e.is_directory())
        {
            continue;
        }
        const std::string id = e.path().filename().string();
        if(!IsWorkspaceId(id))
        {
            continue;
        }
        if(fs::exists(e.path() / "active", ec) ||
           fs::is_directory(e.path() / "generations", ec))
        {
            out.push_back(id);
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

namespace
{

/* Load one generation: manifest verify -> resource-overlay
   registries -> candidate parse. */
bool LoadGeneration(const fs::path& root,
                    const WorkspaceStore::Deps& deps,
                    const fs::path& gendir,
                    WorkspaceCandidate& cand,
                    std::vector<std::string>& errors,
                    std::vector<std::string>& warnings)
{
    Manifest m;
    std::string err;
    if(!VerifyGeneration(gendir, m, err))
    {
        errors.push_back("generation '" +
                         gendir.filename().string() + "': " + err);
        return false;
    }

    /* Resource overlay order: base deps -> managed library ->
       this generation's own snapshot. The snapshot wins, so a
       restart with an empty base still resolves the committed
       resource set. */
    studio::PresetRegistry devreg = deps.devices;
    studio::EffectRegistry effreg = deps.effects;
    std::vector<std::string> derrs;
    std::error_code ec;
    const fs::path lib_dev = root / "library" / "devices";
    if(fs::is_directory(lib_dev, ec))
    {
        devreg.LoadDirectory(lib_dev.string(), &derrs);
    }
    const fs::path lib_fx = root / "library" / "effects";
    if(fs::is_directory(lib_fx, ec))
    {
        effreg.LoadDirectory(lib_fx.string(), &derrs);
    }
    const fs::path res_dev = gendir / "resources" / "devices";
    if(fs::is_directory(res_dev, ec))
    {
        devreg.LoadDirectory(res_dev.string(), &derrs);
    }
    const fs::path res_fx = gendir / "resources" / "effects";
    if(fs::is_directory(res_fx, ec))
    {
        effreg.LoadDirectory(res_fx.string(), &derrs);
    }
    for(const std::string& e : derrs)
    {
        warnings.push_back("resource load: " + e);
    }

    std::string docbytes;
    if(!ReadFileBytes(gendir / "document.json", docbytes))
    {
        errors.push_back("generation '" +
                         gendir.filename().string() +
                         "': document.json unreadable");
        return false;
    }
    const nlohmann::json j =
        nlohmann::json::parse(docbytes, nullptr, false);
    if(j.is_discarded())
    {
        errors.push_back("generation '" +
                         gendir.filename().string() +
                         "': document is not valid JSON");
        return false;
    }
    WorkspaceCandidate parsed;
    std::vector<std::string> perrs;
    if(!ParseWorkspaceCandidate(j, devreg, effreg, parsed,
                                perrs, warnings))
    {
        for(const std::string& e : perrs)
        {
            errors.push_back("generation '" +
                             gendir.filename().string() +
                             "': " + e);
        }
        return false;
    }
    cand = std::move(parsed);
    return true;
}

} /* anonymous namespace */

WorkspaceStore::LoadResult WorkspaceStore::Load(
    const std::string& id) const
{
    LoadResult res;
    if(!IsWorkspaceId(id))
    {
        res.status = LoadStatus::Failed;
        res.error  = "invalid workspace id '" + id + "'";
        return res;
    }
    const fs::path wsdir = WorkspaceDir(id);
    std::error_code ec;
    if(!fs::is_directory(wsdir, ec))
    {
        return res;    /* Empty — nothing managed under this id */
    }
    if(!InsideRoot(root_, wsdir))
    {
        res.status = LoadStatus::Failed;
        res.error  = "workspace dir '" + wsdir.string() +
                     "' is outside the managed root — refused";
        return res;
    }

    const Marker primary = ReadMarker(wsdir / "active");
    const Marker backup  = ReadMarker(wsdir / "active.lkg");

    /* Orphan cleanup BEFORE selection, inside the verified
       root: torn staging, marker temps, unreferenced gens. */
    std::set<std::string> keep;
    if(primary.valid)
    {
        keep.insert(primary.generation);
    }
    if(backup.valid)
    {
        keep.insert(backup.generation);
    }
    const auto existing = GenerationDirs(wsdir);
    CleanupOrphans(wsdir, keep);

    auto try_gen = [&](const Marker& m) -> bool
    {
        WorkspaceCandidate cand;
        std::vector<std::string> errs;
        const fs::path gendir =
            wsdir / "generations" / m.generation;
        if(!LoadGeneration(root_, deps_, gendir, cand, errs,
                           res.warnings))
        {
            res.error.clear();
            for(const std::string& e : errs)
            {
                if(!res.error.empty())
                {
                    res.error += "; ";
                }
                res.error += e;
            }
            return false;
        }
        res.candidate           =
            std::make_shared<WorkspaceCandidate>(std::move(cand));
        res.generation          = m.gen_num;
        res.workspace_revision  = m.workspace_revision;
        res.resource_revision   = m.resource_revision;
        return true;
    };

    if(primary.valid)
    {
        if(try_gen(primary))
        {
            res.status = LoadStatus::Loaded;
            return res;
        }
        const std::string perr = "active generation '" +
            primary.generation + "' unusable: " + res.error;
        res.error.clear();
        if(backup.valid && backup.generation != primary.generation)
        {
            if(try_gen(backup))
            {
                res.status = LoadStatus::Recovered;
                res.error  = perr + " — restored the last-valid "
                             "generation from the marker backup";
                return res;
            }
            res.error = perr + "; marker backup '" +
                        backup.generation + "' also unusable: " +
                        res.error;
        }
        else
        {
            res.error = perr + " (no usable marker backup)";
        }
        res.status = LoadStatus::Failed;
        return res;
    }

    if(primary.exists)
    {
        /* A corrupt marker is never guessed at. */
        const std::string perr = "active marker unreadable (" +
                                 primary.error + ")";
        if(backup.valid && try_gen(backup))
        {
            res.status = LoadStatus::Recovered;
            res.error  = perr + " — restored the last-valid "
                         "generation from the marker backup";
            return res;
        }
        res.status = LoadStatus::Failed;
        res.error  = perr + (backup.exists
            ? "; marker backup also unreadable (" + backup.error + ")"
            : "; no marker backup");
        return res;
    }

    /* No marker. A valid backup still recovers; orphans were
       already cleaned above. */
    if(backup.valid)
    {
        if(try_gen(backup))
        {
            res.status = LoadStatus::Recovered;
            res.error  = "active marker missing — restored the "
                         "last-valid generation from the marker "
                         "backup";
            return res;
        }
        res.status = LoadStatus::Failed;
        return res;
    }
    if(!existing.empty() || backup.exists)
    {
        /* Generation dirs / a corrupt backup existed but no
           marker pointed anywhere — orphans cleaned; nothing
           selectable. */
        res.status = LoadStatus::Failed;
        res.error  = "no active marker and no usable backup";
        return res;
    }
    return res;    /* dir existed but held nothing — Empty */
}

WorkspaceStore::CommitResult WorkspaceStore::Commit(
    const std::string& id,
    const WorkspaceCandidate& candidate,
    const std::string& original_bytes,
    uint64_t expected_workspace_revision)
{
    CommitResult res;
    auto fail = [&](const std::string& e) -> CommitResult
    {
        res.error = e;
        return res;
    };

    if(!IsWorkspaceId(id))
    {
        return fail("invalid workspace id '" + id + "'");
    }
    if(!candidate.resolved)
    {
        return fail("candidate carries no resolved scene — "
                    "only fully resolved candidates commit");
    }
    if(original_bytes.size() > kMaxDocumentBytes)
    {
        return fail("original document exceeds the byte bound");
    }

    /* The store never writes a document its own validation
       rejects — serialize + re-validate FIRST (all fallible
       prep happens before the marker move). */
    studio::StudioDocument work_doc = candidate.document;
    {
        const nlohmann::json wj = studio::ToJson(work_doc);
        std::vector<std::string> verrs;
        studio::StudioDocument check;
        if(!studio::FromJson(wj, check, &verrs))
        {
            std::string e = "candidate document failed "
                            "re-validation:";
            for(const std::string& v : verrs)
            {
                e += " " + v;
            }
            return fail(e);
        }
        work_doc = std::move(check);
    }

    std::error_code ec;
    fs::create_directories(root_, ec);
    if(ec)
    {
        return fail("cannot create workspace root '" + root_ +
                    "': " + ec.message());
    }
    const fs::path wsdir = WorkspaceDir(id);
    if(fs::exists(wsdir, ec) && !InsideRoot(root_, wsdir))
    {
        return fail("workspace dir '" + wsdir.string() +
                    "' escapes the managed root — refused");
    }

    /*---- Revision gate: a stale writer is rejected before
    ||   anything is staged. A corrupt primary marker is not
    ||   a commitment blocker when a valid backup carries the
    ||   revisions — it is set aside as .rejected below
    ||   (preserved, never silently destroyed).
    \*---------------------------------------------------------*/
    const Marker primary = ReadMarker(wsdir / "active");
    const Marker backup  = ReadMarker(wsdir / "active.lkg");
    uint64_t cur_ws_rev  = 0;
    uint64_t cur_res_rev = 0;
    uint64_t cur_gen_num = 0;
    if(primary.valid)
    {
        cur_ws_rev  = primary.workspace_revision;
        cur_res_rev = primary.resource_revision;
        cur_gen_num = primary.gen_num;
    }
    else if(primary.exists && backup.valid)
    {
        cur_ws_rev  = backup.workspace_revision;
        cur_res_rev = backup.resource_revision;
        cur_gen_num = backup.gen_num;
    }
    else if(primary.exists)
    {
        return fail("active marker is unreadable (" +
                    primary.error +
                    ") — recover the workspace before "
                    "committing");
    }
    if(expected_workspace_revision != cur_ws_rev)
    {
        return fail("stale workspace revision: expected " +
                    std::to_string(expected_workspace_revision) +
                    " but '" + id + "' is at " +
                    std::to_string(cur_ws_rev));
    }

    /*---- Library install: extracted types/looks land in the
    ||   managed library with collision-aware renames; doc
    ||   references remap. A failed install leaves the prior
    ||   generation untouched (marker not yet written).
    \*---------------------------------------------------------*/
    std::vector<studio::DevicePreset>   ex_types =
        candidate.extracted_types;
    std::vector<studio::EffectDocument> ex_fx    =
        candidate.extracted_effects;
    {
        std::string ierr;
        if(!InstallDeviceTypes(LibraryDevicesDir(), deps_.devices,
                               work_doc, ex_types,
                               res.warnings, ierr) ||
           !InstallEffectLooks(LibraryEffectsDir(), deps_.effects,
                               work_doc, ex_fx,
                               res.warnings, ierr))
        {
            return fail("resource install failed: " + ierr);
        }
    }

    /*---- Effective registries + post-remap resolution.       ||
    ||   The committed document resolves against base +       ||
    ||   library (extracted types included); an expanded-     ||
    ||   scene source keeps its verbatim resolved scene.      ||
    \*---------------------------------------------------------*/
    const studio::PresetRegistry devreg = EffectiveDeviceRegistry();
    const studio::EffectRegistry effreg = EffectiveEffectRegistry();
    {
        const nlohmann::json wj = studio::ToJson(work_doc);
        std::vector<std::string> verrs;
        studio::StudioDocument check;
        if(!studio::FromJson(wj, check, &verrs, &res.warnings))
        {
            return fail("post-install document failed "
                        "re-validation");
        }
        work_doc = std::move(check);
    }
    std::shared_ptr<const studio::SceneDocument> resolved =
        candidate.resolved;
    if(candidate.source_form !=
       WorkspaceCandidate::SourceForm::ExpandedScene)
    {
        auto rs = std::make_shared<studio::SceneDocument>();
        std::vector<std::string> rerrs;
        if(!studio::ResolveScene(work_doc, devreg, *rs, &rerrs,
                                 &res.warnings))
        {
            std::string e = "candidate failed to resolve "
                            "post-install:";
            for(const std::string& r : rerrs)
            {
                e += " " + r;
            }
            return fail(e);
        }
        resolved = std::move(rs);
    }

    /*---- The generation's resource snapshot: every used      ||
    ||   type (transitively) + the referenced look — the      ||
    ||   generation resolves after a restart even with an     ||
    ||   empty base registry.                                 ||
    \*---------------------------------------------------------*/
    std::vector<std::string> used_types;
    {
        std::vector<std::string> cerrs;
        if(!studio::CollectUsedTypes(work_doc, devreg,
                                     used_types, &cerrs))
        {
            std::string e = "type collection failed:";
            for(const std::string& c : cerrs)
            {
                e += " " + c;
            }
            return fail(e);
        }
    }
    std::vector<std::string> used_fx;
    if(!work_doc.effect.preset.empty() &&
       effreg.Contains(work_doc.effect.preset))
    {
        used_fx.push_back(work_doc.effect.preset);
    }

    /*---- Next generation ordinal: strictly above every       ||
    ||   existing dir AND above the marker's generation —     ||
    ||   generation dirs are immutable, ids never recycle.    ||
    \*---------------------------------------------------------*/
    uint64_t next_num = cur_gen_num + 1;
    for(const auto& kv : GenerationDirs(wsdir))
    {
        if(kv.first >= next_num)
        {
            next_num = kv.first + 1;
        }
    }
    const std::string gen_name = GenerationName(next_num);

    /*---- Stage the whole generation under staging/,          ||
    ||   manifest LAST, then one directory rename publishes   ||
    ||   it. A crash before the rename leaves a torn          ||
    ||   staging dir (cleaned on next load); a crash after    ||
    ||   leaves a completed-but-unreferenced generation.      ||
    \*---------------------------------------------------------*/
    const fs::path staging = wsdir / "staging";
    fs::remove_all(staging, ec);   /* inside the verified root */

    nlohmann::json res_dev = nlohmann::json::object();
    for(const std::string& tid : used_types)
    {
        const studio::DevicePreset* p = devreg.Find(tid);
        if(p == nullptr)
        {
            continue;   /* CollectUsedTypes already errored */
        }
        const std::string rel =
            "resources/devices/" + tid + ".device.json";
        const std::string bytes =
            studio::ToJson(*p).dump(2);
        if(!WriteFileCommitted(staging / rel, bytes, res.error))
        {
            return res;
        }
        res_dev[tid] = FileEntry(rel, bytes);
    }
    nlohmann::json res_fx = nlohmann::json::object();
    for(const std::string& fid : used_fx)
    {
        const studio::EffectDocument* d = effreg.Find(fid);
        if(d == nullptr)
        {
            continue;
        }
        const std::string rel =
            "resources/effects/" + fid + ".effect.json";
        const std::string bytes =
            studio::EffectDocumentToJson(*d).dump(2);
        if(!WriteFileCommitted(staging / rel, bytes, res.error))
        {
            return res;
        }
        res_fx[fid] = FileEntry(rel, bytes);
    }
    const std::string doc_bytes = studio::ToJson(work_doc).dump(2);
    if(!WriteFileCommitted(staging / "document.json", doc_bytes,
                           res.error))
    {
        return res;
    }
    nlohmann::json manifest = nlohmann::json::object({
        { "kind",           kManifestKind   },
        { "schema_version", kRecordSchema   },
        { "generation",     gen_name        },
        { "source_form",
          WorkspaceSourceFormName(candidate.source_form) },
        { "document",       FileEntry("document.json", doc_bytes) },
        { "resources",      nlohmann::json::object({
              { "devices", std::move(res_dev) },
              { "effects", std::move(res_fx)  },
          }) },
    });
    if(!original_bytes.empty())
    {
        if(!WriteFileCommitted(staging / "original.json",
                               original_bytes, res.error))
        {
            return res;
        }
        manifest["original"] =
            FileEntry("original.json", original_bytes);
    }
    if(!WriteFileCommitted(staging / "manifest.json",
                           manifest.dump(2), res.error))
    {
        return res;
    }

    fs::create_directories(wsdir / "generations", ec);
    if(ec)
    {
        return fail("cannot create generations dir: " +
                    ec.message());
    }
    fs::rename(staging, wsdir / "generations" / gen_name, ec);
    if(ec)
    {
        return fail("generation publish failed: " + ec.message());
    }

    /*---- The last fallible step: marker backup + replace.    ||
    ||   resource_revision bumps only when the snapshot set   ||
    ||   changed. After the marker lands, NO fallible I/O     ||
    ||   runs — orphan/prune cleanup is best-effort.          ||
    \*---------------------------------------------------------*/
    uint64_t next_res_rev = cur_res_rev == 0 ? 1 : cur_res_rev;
    {
        Manifest prev;
        std::string perr;
        const fs::path pdir = wsdir / "generations" /
                              GenerationName(cur_gen_num);
        if(cur_gen_num == 0 ||
           !VerifyGeneration(pdir, prev, perr) ||
           prev.resources != manifest["resources"])
        {
            next_res_rev = cur_res_rev + 1;
        }
    }

    const fs::path marker_path = wsdir / "active";
    if(primary.exists)
    {
        /* Preserve the outgoing marker: a VALID one whose
           generation still verifies becomes the last-known-good
           backup; an unreadable one is set aside as .rejected. */
        Manifest pm;
        std::string perr;
        if(primary.valid &&
           VerifyGeneration(wsdir / "generations" /
                            primary.generation, pm, perr))
        {
            if(!WriteAtomicFile(wsdir / "active.lkg.tmp",
                                wsdir / "active.lkg",
                                primary.bytes, res.error))
            {
                return res;
            }
        }
        else if(!WriteAtomicFile(wsdir / "active.rejected.tmp",
                                 wsdir / "active.rejected",
                                 primary.bytes, res.error))
        {
            return res;
        }
    }

    const nlohmann::json marker = nlohmann::json::object({
        { "kind",               kMarkerKind    },
        { "schema_version",     kRecordSchema  },
        { "generation",         gen_name       },
        { "workspace_revision", cur_ws_rev + 1 },
        { "resource_revision",  next_res_rev   },
    });
    if(!WriteAtomicFile(wsdir / "active.tmp", marker_path,
                        marker.dump(2), res.error))
    {
        return res;
    }

    /*---- Post-marker: best-effort hygiene only. Orphans      ||
    ||   (a staged dir that lost the rename, stale temps,     ||
    ||   generations past the retention bound) are removed    ||
    ||   inside the verified root; failures cannot fail the   ||
    ||   committed state.                                     ||
    \*---------------------------------------------------------*/
    std::set<std::string> keep = { gen_name };
    {
        const Marker lkg = ReadMarker(wsdir / "active.lkg");
        if(lkg.valid)
        {
            keep.insert(lkg.generation);
        }
    }
    fs::remove_all(staging, ec);
    PruneGenerations(wsdir, keep, kMaxGenerations);

    auto committed          = std::make_shared<WorkspaceCandidate>();
    committed->source_form  = candidate.source_form;
    committed->migrated     = candidate.migrated;
    committed->document     = std::move(work_doc);
    committed->extracted_types   = std::move(ex_types);
    committed->extracted_effects = std::move(ex_fx);
    committed->device_registry   = devreg;
    committed->effect_registry   = effreg;
    committed->resolved          = std::move(resolved);

    res.ok                 = true;
    res.generation         = next_num;
    res.workspace_revision = cur_ws_rev + 1;
    res.resource_revision  = next_res_rev;
    res.candidate          = std::move(committed);
    return res;
}

} /* namespace openrgb2 */
