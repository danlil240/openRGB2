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
#include <chrono>
#include <thread>
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

/* Windows renames of a just-written file/dir can fail briefly
   with ACCESS_DENIED / SHARING_VIOLATION while an AV scanner,
   the indexer or a backup agent holds a handle. Those are retried
   a few times with a short backoff (~0.4 s total worst case);
   anything else fails at once. */
constexpr int kRenameRetries = 5;

bool TransientRenameError(unsigned long e)
{
    return e == ERROR_ACCESS_DENIED || e == ERROR_SHARING_VIOLATION ||
           e == ERROR_LOCK_VIOLATION;
}

void RenameBackoff(int attempt)
{
    std::this_thread::sleep_for(
        std::chrono::milliseconds(25 * (attempt + 1)));
}

/* fs::rename with the bounded transient retry above (MSVC
   reports the Win32 code in a system_category error_code). */
void RenameWithRetry(const fs::path& from, const fs::path& to,
                     std::error_code& ec)
{
    for(int attempt = 0;; ++attempt)
    {
        ec.clear();
        fs::rename(from, to, ec);
        if(!ec || attempt >= kRenameRetries ||
           ec.category() != std::system_category() ||
           !TransientRenameError((unsigned long)ec.value()))
        {
            return;
        }
        RenameBackoff(attempt);
    }
}

/* temp + commit + rename — the marker's atomic replace. */
bool WriteAtomicFile(const fs::path& tmp, const fs::path& dst,
                     const std::string& bytes, std::string& err)
{
    if(!WriteFileCommitted(tmp, bytes, err))
    {
        return false;
    }
    bool moved = false;
    DWORD last = 0;
    for(int attempt = 0;; ++attempt)
    {
        if(MoveFileExA(tmp.string().c_str(), dst.string().c_str(),
                       MOVEFILE_REPLACE_EXISTING |
                       MOVEFILE_WRITE_THROUGH))
        {
            moved = true;
            break;
        }
        last = GetLastError();
        if(attempt >= kRenameRetries || !TransientRenameError(last))
        {
            break;
        }
        RenameBackoff(attempt);
    }
    if(!moved)
    {
        err = "rename of " + tmp.string() + " over " +
              dst.string() + " failed (win32 " +
              std::to_string(last) + ")";
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

/* A string member, or "" when `obj` is not an object, lacks
   `key`, or holds another type. Record files are read from
   disk and may be damaged: json::value() THROWS type_error on
   a wrong-typed member (or a non-object), and a throw out of
   Load reaches startup re-adoption (audit I-11). Every
   record read here type-checks the way ReadMarker does. */
std::string StringMember(const nlohmann::json& obj, const char* key)
{
    if(!obj.is_object())
    {
        return std::string();
    }
    const auto it = obj.find(key);
    return it != obj.end() && it->is_string()
        ? it->get<std::string>() : std::string();
}

/* Verify one manifest file entry exists on disk with matching
   size + hash. */
bool VerifyEntry(const fs::path& dir, const nlohmann::json& entry,
                 const std::string& what, std::string& err)
{
    const std::string rel = StringMember(entry, "file");
    const std::string sha = StringMember(entry, "sha");
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
    /* Explicit increment(ec): a range-for's operator++ throws on
       a transient FS error. A failed scan yields what it saw. */
    fs::directory_iterator it(gens, ec), end;
    for(; !ec && it != end; it.increment(ec))
    {
        uint64_t n = 0;
        std::error_code dec;
        if(it->is_directory(dec) &&
           GenerationOrdinal(it->path().filename().string(), n))
        {
            out.push_back({ n, it->path().filename().string() });
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

/* Remove torn state inside the verified workspace dir: the
   staging dir, marker temps, and generation dirs that no marker
   references AND that were never published. Commit writes the
   manifest last and publishes with one directory rename, so a
   generation without manifest.json is torn by construction.
   A complete generation is never removed here, even when no
   readable marker names it: a damaged marker must not destroy
   the data it pointed at, and the retained history is pruned
   by Commit (PruneGenerations), not by Load. Never reaches
   outside `wsdir`. */
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
        const fs::path gendir = wsdir / "generations" / kv.second;
        std::error_code mec;
        if(keep.find(kv.second) == keep.end() &&
           !fs::exists(gendir / "manifest.json", mec) && !mec)
        {
            fs::remove_all(gendir, ec);
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
|||| Library-install rollback (audit I-13)                 ||
\*---------------------------------------------------------*/

/* Every regular file under the given dirs (recursive). Never
   throws: a missing dir contributes nothing. */
std::set<std::string> RegularFilesUnder(
    const std::vector<fs::path>& dirs)
{
    std::set<std::string> out;
    for(const fs::path& d : dirs)
    {
        std::error_code ec;
        fs::recursive_directory_iterator it(d, ec), end;
        for(; !ec && it != end; it.increment(ec))
        {
            std::error_code fec;
            if(it->is_regular_file(fec))
            {
                out.insert(it->path().string());
            }
        }
    }
    return out;
}

/* Commit installs extracted types/looks/assets into the SHARED
   managed library before its later fallible steps (re-
   validation, resolve, staging writes, publish rename, marker).
   The installers never overwrite — they only ADD files — so a
   commit that fails afterwards removes exactly the files its
   install added, and the library is as it was. Armed with the
   pre-install listing; disarmed when the commit succeeds. */
class LibraryInstallRollback
{
public:
    LibraryInstallRollback(std::vector<fs::path> dirs,
                           const bool& committed)
        : dirs_(std::move(dirs))
        , before_(RegularFilesUnder(dirs_))
        , committed_(committed)
    {
    }
    ~LibraryInstallRollback()
    {
        if(committed_)
        {
            return;
        }
        for(const std::string& f : RegularFilesUnder(dirs_))
        {
            if(before_.find(f) == before_.end())
            {
                std::error_code ec;
                fs::remove(f, ec);
            }
        }
    }
    LibraryInstallRollback(const LibraryInstallRollback&) = delete;
    LibraryInstallRollback& operator=(
        const LibraryInstallRollback&) = delete;

private:
    std::vector<fs::path>  dirs_;
    std::set<std::string>  before_;
    const bool&            committed_;
};

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
    const auto vit = j.find("schema_version");
    if(StringMember(j, "kind") != kManifestKind ||
       vit == j.end() || !vit->is_number_integer() ||
       vit->get<long long>() != kRecordSchema)
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
    if(j.contains("source_form") && !j["source_form"].is_string())
    {
        err = "manifest source_form is not a string";
        return false;
    }
    out.source_form = StringMember(j, "source_form");
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

/* Copy staged bundle assets into the shared <root>/assets/
   pool — never overwriting an existing file: byte-equal is a
   silent reuse hit, different content keeps the local file and
   warns. rel paths are re-validated (bundle-relative only) and
   every staged source must sit INSIDE the managed root — the
   import flow stages there, so a caller-supplied absolute path
   outside the root is refused rather than followed. */
bool InstallAssets(const std::string& root,
    const std::vector<std::pair<std::string, std::string>>& assets,
                   std::vector<std::string>& warnings,
                   std::string& err)
{
    if(assets.empty())
    {
        return true;
    }
    std::error_code ec;
    const fs::path dst_root =
        (fs::path(root) / "assets").lexically_normal();
    fs::create_directories(dst_root, ec);
    if(ec)
    {
        err = "cannot create " + dst_root.string() + ": " +
              ec.message();
        return false;
    }
    std::set<std::string> seen;
    for(const auto& kv : assets)
    {
        const std::string& rel = kv.first;
        if(!seen.insert(rel).second)
        {
            continue;
        }
        if(!studio::IsPresetAssetPath(rel))
        {
            warnings.push_back("asset '" + rel +
                               "' fails the portable-path rules — "
                               "skipped");
            continue;
        }
        const fs::path src = fs::path(kv.second).lexically_normal();
        const fs::path dst =
            (dst_root / fs::path(rel)).lexically_normal();
        if(!InsideRoot(root, src))
        {
            err = "asset '" + rel + "' source '" + kv.second +
                  "' is outside the managed root — refused";
            return false;
        }
        if(!InsideRoot(dst_root.string(), dst) ||
           dst.parent_path().empty())
        {
            err = "asset '" + rel +
                  "' escapes the assets root — refused";
            return false;
        }
        if(!fs::is_regular_file(src, ec))
        {
            err = "asset '" + rel + "' staged source '" +
                  kv.second + "' is missing";
            return false;
        }
        if(fs::exists(dst, ec))
        {
            /* Byte-compare: equal content is a reuse hit;
               different content keeps the local file. */
            std::string have, inc;
            if(ReadFileBytes(dst, have) && ReadFileBytes(src, inc)
               && have == inc)
            {
                continue;
            }
            warnings.push_back("asset '" + rel +
                               "' exists locally — kept");
            continue;
        }
        fs::create_directories(dst.parent_path(), ec);
        if(ec)
        {
            err = "cannot create asset dir for '" + rel + "': " +
                  ec.message();
            return false;
        }
        fs::copy_file(src, dst, fs::copy_options::none, ec);
        if(ec)
        {
            err = "asset '" + rel + "' copy failed: " +
                  ec.message();
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

namespace
{

std::string AsciiLower(std::string s)
{
    for(char& c : s)
    {
        if(c >= 'A' && c <= 'Z')
        {
            c = (char)(c - 'A' + 'a');
        }
    }
    return s;
}

/* An existing entry under `root` whose name equals `id` only
   case-insensitively ("" when none, or when the exact name
   exists). NTFS resolves both spellings to ONE directory, so
   such an id would read and write another workspace. */
std::string CaseAliasOf(const fs::path& root, const std::string& id)
{
    const std::string want = AsciiLower(id);
    std::string alias;
    std::error_code iec;
    fs::directory_iterator it(root, iec), end;
    for(; !iec && it != end; it.increment(iec))
    {
        const std::string n = it->path().filename().string();
        if(n == id)
        {
            return std::string();
        }
        if(alias.empty() && AsciiLower(n) == want)
        {
            alias = n;
        }
    }
    return alias;
}

std::string CaseAliasError(const std::string& id,
                           const std::string& alias)
{
    return "workspace id '" + id + "' differs only in letter "
           "case from existing '" + alias + "' — ids are "
           "case-insensitive on disk";
}

} /* anonymous namespace */

bool WorkspaceStore::IsWorkspaceId(const std::string& id)
{
    /* Portable single-segment charset — the same rule the
       resource ids follow — minus every name that cannot be a
       workspace directory (audit I-13), compared
       case-insensitively because the filesystem is:
         - the store's own root-level dirs (the shared managed
           library, its assets, the transfer staging root);
         - the Windows reserved device names, which never name
           a directory (CON, PRN, AUX, NUL, COM1-9, LPT1-9). */
    if(!studio::IsPresetId(id))
    {
        return false;
    }
    const std::string l = AsciiLower(id);
    if(l == "library" || l == "assets" || l == "transfers" ||
       l == "con" || l == "prn" || l == "aux" || l == "nul")
    {
        return false;
    }
    if(l.size() == 4 &&
       (l.compare(0, 3, "com") == 0 || l.compare(0, 3, "lpt") == 0) &&
       l[3] >= '1' && l[3] <= '9')
    {
        return false;
    }
    return true;
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

std::string WorkspaceStore::LibraryAssetsDir() const
{
    return (fs::path(root_) / "assets").string();
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
    /* Explicit increment — never throws mid-scan (see
       GenerationDirs). */
    std::error_code iec;
    fs::directory_iterator it(root_, iec), end;
    for(; !iec && it != end; it.increment(iec))
    {
        const fs::directory_entry& e = *it;
        std::error_code dec;
        if(!e.is_directory(dec))
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

WorkspaceStore::Revisions WorkspaceStore::LoadRevisions(
    const std::string& id) const
{
    Revisions r;
    if(!IsWorkspaceId(id))
    {
        r.error = "invalid workspace id '" + id + "'";
        return r;
    }
    if(const std::string alias = CaseAliasOf(root_, id);
       !alias.empty())
    {
        r.error = CaseAliasError(id, alias);
        return r;    /* exists:false — not THIS id's workspace */
    }
    const fs::path wsdir = WorkspaceDir(id);
    std::error_code ec;
    if(!fs::is_directory(wsdir, ec))
    {
        return r;    /* no managed workspace under this id */
    }
    const Marker primary = ReadMarker(wsdir / "active");
    if(primary.valid)
    {
        r.exists             = true;
        r.workspace_revision = primary.workspace_revision;
        r.resource_revision  = primary.resource_revision;
        return r;
    }
    /* Marker backup carries the last-known-good counters — a
       torn primary never silently reads as revision 0. */
    const Marker backup = ReadMarker(wsdir / "active.lkg");
    if(backup.valid)
    {
        r.exists             = true;
        r.workspace_revision = backup.workspace_revision;
        r.resource_revision  = backup.resource_revision;
        if(primary.exists)
        {
            r.error = "primary marker unreadable — revisions are "
                      "the last-known-good marker's";
        }
        return r;
    }
    if(primary.exists || backup.exists)
    {
        r.exists = true;
        r.error  = "active marker unreadable";
    }
    return r;
}

bool WorkspaceStore::ReadActiveDocumentBytes(
    const std::string& id, std::string& bytes,
    std::string& err) const
{
    if(!IsWorkspaceId(id))
    {
        err = "invalid workspace id '" + id + "'";
        return false;
    }
    if(const std::string alias = CaseAliasOf(root_, id);
       !alias.empty())
    {
        err = CaseAliasError(id, alias);
        return false;
    }
    const fs::path wsdir = WorkspaceDir(id);
    std::error_code ec;
    if(!fs::is_directory(wsdir, ec))
    {
        err = "no managed workspace '" + id + "'";
        return false;
    }
    /* The marker selects honestly — a torn primary falls back
       to the last-known-good backup, never a guessed gen. */
    const Marker primary = ReadMarker(wsdir / "active");
    const Marker backup  = ReadMarker(wsdir / "active.lkg");
    const Marker* sel = primary.valid  ? &primary
                      : backup.valid   ? &backup
                      : nullptr;
    if(sel == nullptr)
    {
        err = "workspace '" + id + "' has no readable marker";
        return false;
    }
    const fs::path doc = wsdir / "generations" / sel->generation /
                         "document.json";
    if(!ReadFileBytes(doc, bytes))
    {
        err = "workspace '" + id + "' document.json is "
              "unreadable under generation '" + sel->generation +
              "'";
        return false;
    }
    if(bytes.size() > kMaxDocumentBytes)
    {
        err = "workspace '" + id + "' document exceeds the "
              "byte bound";
        bytes.clear();
        return false;
    }
    return true;
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
    if(const std::string alias = CaseAliasOf(root_, id);
       !alias.empty())
    {
        res.status = LoadStatus::Failed;
        res.error  = CaseAliasError(id, alias);
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

    /* Torn-state cleanup BEFORE selection, inside the verified
       root: staging, marker temps, unpublished gens. Complete
       generations survive whatever the markers say. */
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

WorkspaceStore::LoadResult WorkspaceStore::LoadBackup(
    const std::string& id) const
{
    LoadResult res;
    if(!IsWorkspaceId(id))
    {
        res.status = LoadStatus::Failed;
        res.error  = "invalid workspace id '" + id + "'";
        return res;
    }
    if(const std::string alias = CaseAliasOf(root_, id);
       !alias.empty())
    {
        res.status = LoadStatus::Failed;
        res.error  = CaseAliasError(id, alias);
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
    /* Restore Backup loads the generation the MARKER BACKUP
       names — the last-valid pointer the commit boundary
       maintains. It is never guessed: a missing/corrupt
       backup reports honestly instead of picking a dir. */
    const Marker backup = ReadMarker(wsdir / "active.lkg");
    if(!backup.valid)
    {
        res.status = LoadStatus::Failed;
        res.error  = backup.exists
            ? "workspace '" + id + "' marker backup is unreadable "
              "(" + backup.error + ")"
            : "workspace '" + id + "' has no marker backup — "
              "nothing to restore";
        return res;
    }
    WorkspaceCandidate cand;
    std::vector<std::string> errs;
    const fs::path gendir =
        wsdir / "generations" / backup.generation;
    if(!LoadGeneration(root_, deps_, gendir, cand, errs,
                       res.warnings))
    {
        res.status = LoadStatus::Failed;
        for(const std::string& e : errs)
        {
            if(!res.error.empty())
            {
                res.error += "; ";
            }
            res.error += e;
        }
        if(res.error.empty())
        {
            res.error = "backup generation '" + backup.generation +
                        "' unusable";
        }
        return res;
    }
    res.status            = LoadStatus::Loaded;
    res.candidate         =
        std::make_shared<WorkspaceCandidate>(std::move(cand));
    res.generation        = backup.gen_num;
    res.workspace_revision = backup.workspace_revision;
    res.resource_revision  = backup.resource_revision;
    return res;
}

WorkspaceStore::ExternalFingerprint WorkspaceStore::ExternalScan(
    const std::string& id) const
{
    ExternalFingerprint fp;
    if(!IsWorkspaceId(id))
    {
        fp.detail = "invalid workspace id '" + id + "'";
        return fp;
    }
    const fs::path wsdir = WorkspaceDir(id);
    std::error_code ec;
    if(!fs::is_directory(wsdir, ec))
    {
        /* Nothing managed under this id — not an external
           change, just nothing to watch. */
        return fp;
    }
    fp.watched = true;

    /* Signature mixer — every tracked byte feeds one ordered
       token; identical content reproduces it exactly. */
    std::string sig;
    auto mix = [&sig](const char* tag, const std::string& v)
    {
        sig += tag;
        sig += '=';
        sig += v;
        sig += ';';
    };
    auto mix_marker = [&](const char* tag, const Marker& m)
    {
        if(!m.exists)
        {
            mix(tag, "absent");
        }
        else if(!m.valid)
        {
            /* Content-pinned: rewriting the marker with the
               same corrupt bytes stays suppressed. */
            mix(tag, "corrupt:" + HashBytes(m.bytes));
        }
        else
        {
            mix(tag, m.generation + "/" +
                     std::to_string(m.workspace_revision) + "/" +
                     std::to_string(m.resource_revision));
        }
    };
    const Marker primary = ReadMarker(wsdir / "active");
    const Marker backup  = ReadMarker(wsdir / "active.lkg");
    mix_marker("m", primary);
    mix_marker("b", backup);

    if(!primary.exists)
    {
        fp.missing = true;
        fp.detail  = "active marker removed";
    }
    else if(!primary.valid)
    {
        fp.detail  = "active marker replaced or corrupt (" +
                     primary.error + ")";
    }
    else
    {
        const fs::path gendir =
            wsdir / "generations" / primary.generation;
        if(!fs::is_directory(gendir, ec))
        {
            fp.missing = true;
            fp.detail  = "generation '" + primary.generation +
                         "' removed";
            mix("g", "absent");
        }
        else
        {
            mix("g", primary.generation);
            auto mix_file = [&](const char* tag,
                                const char* rel)
            {
                std::string bytes;
                const fs::path p = gendir / rel;
                if(ReadFileBytes(p, bytes))
                {
                    mix(tag, HashBytes(bytes));
                }
                else
                {
                    mix(tag, "absent");
                    if(fs::exists(p, ec))
                    {
                        if(fp.detail.empty())
                        {
                            fp.detail = std::string(rel) +
                                        " unreadable";
                        }
                    }
                    else
                    {
                        fp.missing = true;
                        if(fp.detail.empty())
                        {
                            fp.detail = std::string(rel) +
                                        " removed";
                        }
                    }
                }
            };
            mix_file("doc",    "document.json");
            mix_file("man",    "manifest.json");
            /* original.json is OPTIONAL — absent is a state,
               never a "missing" finding. Mix it as a plain
               present/absent+hash so its arrival or churn
               still registers. */
            {
                std::string bytes;
                const fs::path op = gendir / "original.json";
                if(ReadFileBytes(op, bytes))
                {
                    mix("orig", HashBytes(bytes));
                }
                else
                {
                    mix("orig", "absent");
                    if(fs::exists(op, ec) && fp.detail.empty())
                    {
                        fp.detail = "original.json unreadable";
                    }
                }
            }
            /* Resource snapshot stamp — sorted rel:size pairs
               catch add/remove/replace without hashing every
               byte (content churn inside the generation's own
               snapshot is engine-owned anyway). */
            const fs::path rdir = gendir / "resources";
            std::string stamp;
            if(fs::is_directory(rdir, ec))
            {
                std::vector<std::string> entries;
                for(fs::recursive_directory_iterator
                        it(rdir, ec), end;
                    !ec && it != end; it.increment(ec))
                {
                    if(!it->is_regular_file(ec))
                    {
                        continue;
                    }
                    entries.push_back(
                        fs::relative(it->path(), rdir, ec)
                            .generic_string() + ":" +
                        std::to_string(it->file_size(ec)));
                }
                std::sort(entries.begin(), entries.end());
                for(const std::string& e : entries)
                {
                    stamp += e;
                    stamp += ';';
                }
            }
            mix("res", stamp);
        }
    }
    fp.signature = std::move(sig);
    return fp;
}

WorkspaceStore::ExternalStampResult WorkspaceStore::ExternalStamp(
    const std::string& id) const
{
    ExternalStampResult out;
    if(!IsWorkspaceId(id))
    {
        return out;            /* racy: the caller just scans */
    }
    const fs::path wsdir = WorkspaceDir(id);
    /* Anything written within this window is "racy" — far
       wider than any file-time granularity. */
    const auto now     = fs::file_time_type::clock::now();
    const auto horizon = now - std::chrono::seconds(3);
    bool racy = false;
    std::string st;
    auto stamp_file = [&](const char* tag, const fs::path& p)
    {
        std::error_code ec;
        const fs::file_status fs_st = fs::status(p, ec);
        st += tag;
        st += '=';
        if(ec || !fs::exists(fs_st))
        {
            st += "absent;";
            return;
        }
        const uintmax_t   sz = fs::is_regular_file(fs_st)
                                   ? fs::file_size(p, ec) : 0;
        const fs::file_time_type t = fs::last_write_time(p, ec);
        if(ec || t > horizon)
        {
            racy = true;
        }
        st += std::to_string((uint64_t)sz);
        st += '@';
        st += std::to_string(
            (long long)t.time_since_epoch().count());
        st += ';';
    };

    std::error_code ec;
    if(!fs::is_directory(wsdir, ec))
    {
        out.stamp = "unwatched";
        out.racy  = false;
        return out;
    }
    stamp_file("m", wsdir / "active");
    stamp_file("b", wsdir / "active.lkg");
    /* The pointed generation comes from the marker itself —
       a tiny read; a corrupt/absent marker leaves the
       generation part out (the marker's own stamp moved). */
    const Marker primary = ReadMarker(wsdir / "active");
    if(primary.exists && primary.valid)
    {
        const fs::path gendir =
            wsdir / "generations" / primary.generation;
        st += "g=" + primary.generation + ";";
        stamp_file("doc",  gendir / "document.json");
        stamp_file("man",  gendir / "manifest.json");
        stamp_file("orig", gendir / "original.json");
        const fs::path rdir = gendir / "resources";
        if(fs::is_directory(rdir, ec))
        {
            std::vector<std::string> entries;
            for(fs::recursive_directory_iterator it(rdir, ec), end;
                !ec && it != end; it.increment(ec))
            {
                /* Regular files only — the set ExternalScan
                   stamps (a directory's own write time moves
                   with its entries, which are listed here). */
                std::error_code fec;
                if(!it->is_regular_file(fec))
                {
                    continue;
                }
                const fs::file_time_type t =
                    fs::last_write_time(it->path(), fec);
                if(fec || t > horizon)
                {
                    racy = true;
                }
                entries.push_back(
                    fs::relative(it->path(), rdir, fec)
                        .generic_string() + ":" +
                    std::to_string(it->file_size(fec)) + "@" +
                    std::to_string((long long)
                        t.time_since_epoch().count()));
            }
            std::sort(entries.begin(), entries.end());
            st += "res=";
            for(const std::string& e : entries)
            {
                st += e;
                st += ',';
            }
            st += ';';
        }
    }
    out.stamp = std::move(st);
    out.racy  = racy;
    return out;
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
    /* "Desk" must never write into "desk" (audit I-13). */
    if(const std::string alias = CaseAliasOf(root_, id);
       !alias.empty())
    {
        return fail(CaseAliasError(id, alias));
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
    /* Any failure from here until res.ok removes what the
       install added (audit I-13); res outlives the guard. A
       candidate that extracts nothing installs nothing — no
       listing then. */
    const bool installs = !ex_types.empty() || !ex_fx.empty() ||
                          !candidate.extracted_assets.empty();
    LibraryInstallRollback install_rollback(
        installs ? std::vector<fs::path>{
                       fs::path(LibraryDevicesDir()),
                       fs::path(LibraryEffectsDir()),
                       fs::path(LibraryAssetsDir()) }
                 : std::vector<fs::path>{},
        res.ok);
    {
        std::string ierr;
        if(!InstallDeviceTypes(LibraryDevicesDir(), deps_.devices,
                               work_doc, ex_types,
                               res.warnings, ierr) ||
           !InstallEffectLooks(LibraryEffectsDir(), deps_.effects,
                               work_doc, ex_fx,
                               res.warnings, ierr) ||
           !InstallAssets(root_, candidate.extracted_assets,
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
    RenameWithRetry(staging, wsdir / "generations" / gen_name, ec);
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

/*---------------------------------------------------------*\
||| InstallEffectResource — Task 6 personal-look install.   ||
||| Same collision discipline as InstallEffectLooks but for ||
||| ONE validated document, revision-gated and WITHOUT a    ||
||| generation: the marker's resource_revision bumps in     ||
||| place only when a file was written.                     ||
\*---------------------------------------------------------*/
WorkspaceStore::ResourceInstallResult WorkspaceStore::InstallEffectResource(
    const std::string& id,
    const studio::EffectDocument& effect,
    uint64_t expected_workspace_revision,
    uint64_t expected_resource_revision)
{
    ResourceInstallResult res;
    auto fail = [&](std::string e) -> ResourceInstallResult
    {
        res.error = std::move(e);
        return res;
    };
    if(!IsWorkspaceId(id))
    {
        return fail("invalid workspace id '" + id + "'");
    }
    if(!studio::IsPresetId(effect.id))
    {
        return fail("effect id '" + effect.id +
                    "' is not a valid identifier");
    }
    /* Re-validate the serialized form — the file that lands
       must re-parse to the same document (bounded, canonical). */
    const nlohmann::ordered_json ej =
        studio::EffectDocumentToJson(effect);
    {
        studio::EffectDocument check;
        std::vector<std::string> verrs;
        if(!studio::EffectDocumentFromJson(ej, check, &verrs))
        {
            return fail("effect '" + effect.id +
                        "' failed validation: " +
                        (verrs.empty() ? std::string("invalid")
                                       : verrs.front()));
        }
    }
    const std::string bytes = ej.dump(2);
    if(bytes.size() > 1024 * 1024)
    {
        return fail("effect '" + effect.id +
                    "' exceeds the 1 MiB resource bound");
    }

    std::error_code ec;
    const fs::path wsdir = WorkspaceDir(id);
    if(fs::exists(wsdir, ec) && !InsideRoot(root_, wsdir))
    {
        return fail("workspace dir '" + wsdir.string() +
                    "' escapes the managed root — refused");
    }
    /* "Desk" must never write into "desk" (audit I-13). */
    if(const std::string alias = CaseAliasOf(root_, id);
       !alias.empty())
    {
        return fail(CaseAliasError(id, alias));
    }

    /*---- Revision gate — same primary/backup discipline as
    ||   Commit: an unreadable marker is never guessed at.
    \*---------------------------------------------------------*/
    const Marker primary = ReadMarker(wsdir / "active");
    const Marker backup  = ReadMarker(wsdir / "active.lkg");
    Marker       current;
    if(primary.valid)
    {
        current = primary;
    }
    else if(primary.exists && backup.valid)
    {
        current = backup;
    }
    else if(primary.exists)
    {
        return fail("active marker is unreadable (" +
                    primary.error +
                    ") — recover the workspace before "
                    "installing resources");
    }
    else if(backup.valid)
    {
        current = backup;
    }
    else
    {
        return fail("workspace '" + id +
                    "' has no readable marker — nothing to "
                    "install against");
    }
    if(expected_workspace_revision != current.workspace_revision)
    {
        return fail("stale workspace revision: expected " +
                    std::to_string(expected_workspace_revision) +
                    " but '" + id + "' is at " +
                    std::to_string(current.workspace_revision));
    }
    if(expected_resource_revision != current.resource_revision)
    {
        return fail("stale resource revision: expected " +
                    std::to_string(expected_resource_revision) +
                    " but '" + id + "' is at " +
                    std::to_string(current.resource_revision));
    }

    /*---- Collision-safe install — identical content reuses,  ||
    ||   differing content lands under a suffixed id, never   ||
    ||   an overwrite (InstallEffectLooks discipline).        ||
    \*-------------------------------------------------------*/
    const fs::path dir = fs::path(LibraryEffectsDir());
    fs::create_directories(dir, ec);
    if(ec)
    {
        return fail("cannot create " + dir.string() + ": " +
                    ec.message());
    }
    std::string landed = effect.id;
    const fs::path target = dir / (landed + ".effect.json");
    bool have_local = false;
    bool identical  = false;
    if(fs::exists(target, ec))
    {
        have_local = true;
        std::string existing;
        if(ReadFileBytes(target, existing))
        {
            const nlohmann::json parsed =
                nlohmann::json::parse(existing, nullptr, false);
            studio::EffectDocument ed;
            if(!parsed.is_discarded() &&
               studio::EffectDocumentFromJson(parsed, ed, nullptr))
            {
                identical =
                    studio::EffectDocumentToJson(ed) == ej;
            }
        }
    }
    else if(const studio::EffectDocument* p =
                deps_.effects.Find(landed))
    {
        /* A packaged/default look with the same id counts as
           existing content — identical reuses, differing gets
           the file-layer override name. */
        have_local = true;
        identical  = studio::EffectDocumentToJson(*p) == ej;
    }
    bool wrote = false;
    if(have_local && identical)
    {
        res.reused = true;      /* nothing written */
    }
    else
    {
        if(have_local)
        {
            landed = FreeEffectId(dir, deps_.effects, {},
                                  effect.id);
            res.warnings.push_back(
                "effect look '" + effect.id +
                "' collides with existing content — "
                "installed as '" + landed + "'");
        }
        studio::EffectDocument to_write = effect;
        to_write.id = landed;
        const std::string wbytes =
            studio::EffectDocumentToJson(to_write).dump(2);
        const fs::path wtarget = dir / (landed + ".effect.json");
        if(fs::exists(wtarget, ec))
        {
            /* Another writer landed the id mid-install —
               still never overwritten. */
            return fail("effect file '" + landed +
                        ".effect.json' appeared during install");
        }
        if(!WriteAtomicFile(wtarget.string() + ".tmp", wtarget,
                            wbytes, res.error))
        {
            return res;
        }
        /* Read-back verification — the landed file must parse
           to the document written. */
        studio::EffectDocument landed_doc;
        if(!studio::EffectDocumentFromJsonFile(wtarget.string(),
                                               landed_doc, nullptr) ||
           studio::EffectDocumentToJson(landed_doc) !=
               studio::EffectDocumentToJson(to_write))
        {
            return fail("installed effect '" + landed +
                        "' failed read-back verification");
        }
        wrote = true;
    }

    /*---- Marker bump: resource_revision advances only when  ||
    ||   the library actually changed. Generation + workspace||
    ||   revision pass through untouched; the marker replace  ||
    ||   is the same atomic write the commit path uses.       ||
    \*-------------------------------------------------------*/
    uint64_t next_res_rev = current.resource_revision;
    if(wrote)
    {
        ++next_res_rev;
        const nlohmann::json marker = nlohmann::json::object({
            { "kind",               kMarkerKind    },
            { "schema_version",     kRecordSchema  },
            { "generation",         current.generation },
            { "workspace_revision", current.workspace_revision },
            { "resource_revision",  next_res_rev   },
        });
        if(!WriteAtomicFile(wsdir / "active.tmp",
                            wsdir / "active",
                            marker.dump(2), res.error))
        {
            return res;
        }
    }

    res.ok                 = true;
    res.landed_id          = landed;
    res.workspace_revision = current.workspace_revision;
    res.resource_revision  = next_res_rev;
    return res;
}

/*---------------------------------------------------------*\
|||| InstallDeviceTypeResource — studio-import Task 9.     ||
|||| One validated device type into the managed library:   ||
|||| same revision gate + collision discipline as the      ||
|||| effect install, plus the editor's explicit `replace`  ||
|||| (the file layer is the authored override — rewriting  ||
|||| it IS the save). A packaged default is never touched;||
|||| replace simply lands the file override.               ||
\*---------------------------------------------------------*/

namespace
{

/* Shared marker gate for the single-resource ops — the same
   primary/backup/unreadable discipline InstallEffectResource
   keeps. */
bool LoadResourceGate(const WorkspaceStore& store,
                      const std::string& id,
                      uint64_t expected_workspace_revision,
                      uint64_t expected_resource_revision,
                      Marker& current,
                      fs::path& wsdir,
                      std::string& err)
{
    std::error_code ec;
    wsdir = fs::path(store.WorkspaceDir(id));
    if(fs::exists(wsdir, ec) && !InsideRoot(store.Root(), wsdir))
    {
        err = "workspace dir '" + wsdir.string() +
              "' escapes the managed root — refused";
        return false;
    }
    if(const std::string alias = CaseAliasOf(store.Root(), id);
       !alias.empty())
    {
        err = CaseAliasError(id, alias);
        return false;
    }
    const Marker primary = ReadMarker(wsdir / "active");
    const Marker backup  = ReadMarker(wsdir / "active.lkg");
    if(primary.valid)
    {
        current = primary;
    }
    else if(primary.exists && backup.valid)
    {
        current = backup;
    }
    else if(primary.exists)
    {
        err = "active marker is unreadable (" + primary.error +
              ") — recover the workspace before touching resources";
        return false;
    }
    else if(backup.valid)
    {
        current = backup;
    }
    else
    {
        err = "workspace '" + id +
              "' has no readable marker — nothing to act on";
        return false;
    }
    if(expected_workspace_revision != current.workspace_revision)
    {
        err = "stale workspace revision: expected " +
              std::to_string(expected_workspace_revision) +
              " but '" + id + "' is at " +
              std::to_string(current.workspace_revision);
        return false;
    }
    if(expected_resource_revision != current.resource_revision)
    {
        err = "stale resource revision: expected " +
              std::to_string(expected_resource_revision) +
              " but '" + id + "' is at " +
              std::to_string(current.resource_revision);
        return false;
    }
    return true;
}

bool BumpResourceRevision(const fs::path& wsdir, const Marker& current,
                          uint64_t& next_res_rev, std::string& err)
{
    next_res_rev = current.resource_revision + 1;
    const nlohmann::json marker = nlohmann::json::object({
        { "kind",               kMarkerKind    },
        { "schema_version",     kRecordSchema  },
        { "generation",         current.generation },
        { "workspace_revision", current.workspace_revision },
        { "resource_revision",  next_res_rev   },
    });
    return WriteAtomicFile(wsdir / "active.tmp",
                           wsdir / "active",
                           marker.dump(2), err);
}

/* Ids a REMOVE would strand: every workspace's ACTIVE document
   parsed for `devices.*.type` (+ ui.favorites) or
   `effects.preset`. Cheap bounded reads — one document.json per
   managed id via the marker path. */
bool ResourceInUse(const WorkspaceStore& store,
                   const std::string& kind,
                   const std::string& res_id,
                   std::vector<std::string>& users)
{
    for(const std::string& wid : store.List())
    {
        std::string bytes, derr;
        if(!store.ReadActiveDocumentBytes(wid, bytes, derr))
        {
            continue;    /* unreadable generation — no doc */
        }
        const nlohmann::json j =
            nlohmann::json::parse(bytes, nullptr, false);
        if(j.is_discarded())
        {
            continue;
        }
        bool hit = false;
        if(kind == "device_type")
        {
            /* j.value() returns a temporary — bind it before
               .items() or the iteration proxy dangles. */
            const nlohmann::json devices =
                j.value("devices", nlohmann::json::object());
            for(const auto& kv : devices.items())
            {
                hit |= kv.value().value("type", std::string())
                       == res_id;
            }
            if(!hit && j.contains("meta") &&
               j["meta"].contains("ui") &&
               j["meta"]["ui"].contains("favorites") &&
               j["meta"]["ui"]["favorites"].is_array())
            {
                for(const nlohmann::json& f :
                        j["meta"]["ui"]["favorites"])
                {
                    hit |= f.is_string() &&
                           f.get<std::string>() == res_id;
                }
            }
        }
        else if(kind == "effect")
        {
            hit = j.contains("effects") &&
                  j["effects"].value("preset", std::string())
                      == res_id;
        }
        if(hit)
        {
            users.push_back(wid);
        }
    }
    return !users.empty();
}

} /* anonymous namespace */

WorkspaceStore::ResourceInstallResult
WorkspaceStore::InstallDeviceTypeResource(
    const std::string& id,
    const studio::DevicePreset& preset,
    bool allow_replace,
    uint64_t expected_workspace_revision,
    uint64_t expected_resource_revision)
{
    ResourceInstallResult res;
    auto fail = [&](std::string e) -> ResourceInstallResult
    {
        res.error = std::move(e);
        return res;
    };
    if(!IsWorkspaceId(id))
    {
        return fail("invalid workspace id '" + id + "'");
    }
    if(!studio::IsPresetId(preset.id))
    {
        return fail("type id '" + preset.id +
                    "' is not a valid identifier");
    }
    /* Re-validate the serialized form — the file that lands
       must re-parse to the same preset (bounded, canonical). */
    const nlohmann::json pj = studio::ToJson(preset);
    {
        studio::DevicePreset check;
        std::vector<std::string> verrs;
        if(!studio::DevicePresetFromJson(pj, check, &verrs))
        {
            return fail("type '" + preset.id +
                        "' failed validation: " +
                        (verrs.empty() ? std::string("invalid")
                                       : verrs.front()));
        }
    }
    /* Dependency graph check against the EFFECTIVE registry
       with the candidate layered on — missing/cyclic child refs
       refuse before any write. */
    {
        studio::PresetRegistry tmp = EffectiveDeviceRegistry();
        std::vector<std::string> derrs;
        if(!tmp.Add(preset, &derrs))
        {
            return fail("type '" + preset.id +
                        "' dependency check failed: " +
                        (derrs.empty() ? std::string("invalid")
                                       : derrs.front()));
        }
    }
    const std::string bytes = pj.dump(2);
    if(bytes.size() > 1024 * 1024)
    {
        return fail("type '" + preset.id +
                    "' exceeds the 1 MiB resource bound");
    }

    fs::path wsdir;
    Marker   current;
    if(!LoadResourceGate(*this, id,
                         expected_workspace_revision,
                         expected_resource_revision,
                         current, wsdir, res.error))
    {
        return res;
    }

    std::error_code ec;
    const fs::path dir = fs::path(LibraryDevicesDir());
    fs::create_directories(dir, ec);
    if(ec)
    {
        return fail("cannot create " + dir.string() + ": " +
                    ec.message());
    }

    const fs::path target = dir / (preset.id + ".device.json");
    bool        have_file = fs::exists(target, ec);
    bool        identical = false;
    bool        have_default = deps_.devices.HasDefault(preset.id);
    if(have_file)
    {
        studio::DevicePreset existing;
        if(studio::DevicePresetFromJsonFile(target.string(),
                                          existing, nullptr))
        {
            identical = studio::ToJson(existing) == pj;
        }
    }
    else if(const studio::DevicePreset* p =
                deps_.devices.Find(preset.id))
    {
        have_default = true;
        identical    = studio::ToJson(*p) == pj;
    }

    bool wrote = false;
    std::string landed = preset.id;
    if((have_file || have_default) && identical)
    {
        res.reused = true;      /* nothing to write */
    }
    else if((have_file || have_default) && !allow_replace)
    {
        /* Collision-safe import semantics: differing content
           never overwrites — a suffixed id lands instead. */
        std::set<std::string> claimed;
        landed = FreeTypeId(dir, deps_.devices, claimed,
                            preset.id);
        res.warnings.push_back(
            "type '" + preset.id +
            "' collides with existing content — installed as '" +
            landed + "'");
    }
    if(!res.reused)
    {
        studio::DevicePreset to_write = preset;
        to_write.id = landed;
        const std::string wbytes =
            studio::ToJson(to_write).dump(2);
        const fs::path wtarget = dir / (landed + ".device.json");
        const bool overwrite = allow_replace && have_file;
        if(!overwrite && fs::exists(wtarget, ec))
        {
            /* Another writer landed the id mid-install —
               still never overwritten. */
            return fail("type file '" + landed +
                        ".device.json' appeared during install");
        }
        if(!WriteAtomicFile(wtarget.string() + ".tmp", wtarget,
                            wbytes, res.error))
        {
            return res;
        }
        /* Read-back verification — the landed file must parse
           to the preset written. */
        studio::DevicePreset landed_p;
        std::vector<std::string> verrs;
        if(!studio::DevicePresetFromJsonFile(wtarget.string(),
                                           landed_p, &verrs) ||
           studio::ToJson(landed_p) != studio::ToJson(to_write))
        {
            return fail("installed type '" + landed +
                        "' failed read-back verification");
        }
        wrote = true;
        if(overwrite)
        {
            res.replaced = true;
        }
    }

    uint64_t next_res_rev = current.resource_revision;
    if(wrote)
    {
        std::string werr;
        if(!BumpResourceRevision(wsdir, current, next_res_rev,
                                 werr))
        {
            return fail(werr);
        }
    }
    res.ok                 = true;
    res.landed_id          = landed;
    res.workspace_revision = current.workspace_revision;
    res.resource_revision  = next_res_rev;
    return res;
}

/*---------------------------------------------------------*\
|||| RemoveResource — file-layer delete with honest        ||
|||| refusal (Task 9): packaged defaults have nothing on  ||
|||| disk; in-use ids and child-ref'd ids are refused by   ||
|||| name. A deleted override of a packaged default        ||
|||| reverts to it.                                        ||
\*---------------------------------------------------------*/

WorkspaceStore::ResourceRemoveResult WorkspaceStore::RemoveResource(
    const std::string& id,
    const std::string& kind,
    const std::string& resource_id,
    uint64_t expected_workspace_revision,
    uint64_t expected_resource_revision)
{
    ResourceRemoveResult res;
    auto fail = [&](std::string e) -> ResourceRemoveResult
    {
        res.error = std::move(e);
        return res;
    };
    if(!IsWorkspaceId(id))
    {
        return fail("invalid workspace id '" + id + "'");
    }
    const bool is_type = kind == "device_type";
    const bool is_fx   = kind == "effect";
    if(!is_type && !is_fx)
    {
        return fail("resource kind '" + kind +
                    "' must be device_type|effect");
    }
    if(!studio::IsPresetId(resource_id))
    {
        return fail("resource id '" + resource_id +
                    "' is not a valid identifier");
    }
    fs::path wsdir;
    Marker   current;
    if(!LoadResourceGate(*this, id,
                         expected_workspace_revision,
                         expected_resource_revision,
                         current, wsdir, res.error))
    {
        return res;
    }

    /* File-layer existence first — the packaged default has
       nothing on disk to remove. */
    const fs::path dir = is_type
        ? fs::path(LibraryDevicesDir())
        : fs::path(LibraryEffectsDir());
    const fs::path target = dir / (resource_id +
        (is_type ? ".device.json" : ".effect.json"));
    std::error_code ec;
    if(!fs::exists(target, ec))
    {
        const bool packaged = is_type
            ? deps_.devices.HasDefault(resource_id)
            : deps_.effects.HasDefault(resource_id);
        return fail("'" + resource_id + "'" +
                    (packaged
                        ? " is a packaged default — nothing to delete"
                        : " has no file-layer entry"));
    }

    /* In-use across EVERY managed workspace's active document —
       removing it would strand that workspace's next resolve. */
    std::vector<std::string> users;
    if(ResourceInUse(*this, kind, resource_id, users))
    {
        std::string e = "'" + resource_id + "' is in use by ";
        for(size_t i = 0; i < users.size(); ++i)
        {
            e += (i ? ", " : "") + users[i];
        }
        return fail(e);
    }

    /* Referenced-as-child: another type's entity `type` ref
       (file layer AND packaged defaults — a revert must not
       strand a default either). */
    if(is_type)
    {
        std::vector<std::string> dependents;
        studio::PresetRegistry all = EffectiveDeviceRegistry();
        for(const std::string& oid : all.Ids())
        {
            if(oid == resource_id)
            {
                continue;
            }
            const studio::DevicePreset* op = all.Find(oid);
            if(op == nullptr)
            {
                continue;
            }
            for(const std::string& dep :
                    studio::PresetDependencies(*op))
            {
                if(dep == resource_id)
                {
                    dependents.push_back(oid);
                    break;
                }
            }
        }
        if(!dependents.empty())
        {
            std::string e = "'" + resource_id +
                            "' is referenced by ";
            for(size_t i = 0; i < dependents.size(); ++i)
            {
                e += (i ? ", " : "") + dependents[i];
            }
            return fail(e);
        }
    }

    /* Marker bump BEFORE the delete: a failed bump leaves the
       file in place and the caller's stale revision can simply
       retry — the reverse order could strand a deleted file
       with an un-bumped (lying) marker. A bump without the
       remove only wastes a revision number, never content. */
    uint64_t next_res_rev = current.resource_revision;
    std::string werr;
    if(!BumpResourceRevision(wsdir, current, next_res_rev, werr))
    {
        return fail(werr);
    }

    if(!fs::remove(target, ec) || ec)
    {
        return fail("cannot remove '" + target.string() + "'" +
                    (ec ? ": " + ec.message() : ""));
    }
    res.removed = true;
    res.reverted_to_default = is_type
        ? deps_.devices.HasDefault(resource_id)
        : deps_.effects.HasDefault(resource_id);
    res.ok                 = true;
    res.workspace_revision = current.workspace_revision;
    res.resource_revision  = next_res_rev;
    return res;
}

} /* namespace openrgb2 */
