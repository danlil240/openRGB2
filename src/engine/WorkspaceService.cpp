/*---------------------------------------------------------*\
|||| WorkspaceService.cpp                                    ||
||||                                                           |
||||   SPDX-License-Identifier: GPL-2.0-or-later               ||
\*---------------------------------------------------------*/

#include "WorkspaceService.h"

#include "config/StudioConfig.h"
#include "presets/DevicePreset.h"
#include "presets/PresetBundle.h"
#include "effects/EffectJson.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <utility>

namespace openrgb2
{

namespace
{

namespace fs = std::filesystem;

bool ReadBytes(const std::string& path, std::string& out,
               uint64_t cap)
{
    std::ifstream in(path, std::ios::binary);
    if(!in)
    {
        return false;
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    out = ss.str();
    return out.size() <= cap;
}

/* Job-result report arrays ride the 1 MiB reply envelope — a
   pathological bundle (thousands of bundled presets or parse
   errors) must never push a result over it. Reports keep the
   first entries and carry the dropped count honestly. */
constexpr size_t kMaxReportItems = 256;

void Err(std::vector<std::string>& errors, const std::string& e)
{
    errors.push_back(e);
}

void CapReportArray(nlohmann::json& report, const char* key)
{
    const auto it = report.find(key);
    if(it == report.end() || !it->is_array() ||
       it->size() <= kMaxReportItems)
    {
        return;
    }
    const size_t dropped = it->size() - kMaxReportItems;
    it->erase(it->begin() + (ptrdiff_t)kMaxReportItems,
              it->end());
    report[std::string(key) + "_truncated"] = dropped;
}

/* Layer the types/looks a folder-of-files import carries under
   <dir>/presets/{devices,effects} — a scratch overlay only; the
   caller's registries are never mutated (matching the
   candidate's snapshot discipline). */
void LoadSiblingPresets(const fs::path& file_dir,
                        studio::PresetRegistry& devices,
                        studio::EffectRegistry& effects,
                        std::vector<std::string>& warnings)
{
    const fs::path pdev = file_dir / "presets" / "devices";
    const fs::path pfx  = file_dir / "presets" / "effects";
    std::error_code ec;
    std::vector<std::string> lerrs;
    if(fs::is_directory(pdev, ec))
    {
        devices.LoadDirectory(pdev.string(), &lerrs);
    }
    if(fs::is_directory(pfx, ec))
    {
        effects.LoadDirectory(pfx.string(), &lerrs);
    }
    for(std::string& e : lerrs)
    {
        warnings.push_back("sibling presets: " + e);
    }
}

/* Pull the sibling-file types/looks the document actually
   references into the candidate's extracted sets so the commit
   installs them into the managed library — only what is used,
   only under a free id. */
void HarvestUsedSiblings(const fs::path& file_dir,
                         WorkspaceCandidate& cand)
{
    const fs::path pdev = file_dir / "presets" / "devices";
    const fs::path pfx  = file_dir / "presets" / "effects";
    std::error_code ec;

    if(fs::is_directory(pdev, ec))
    {
        std::set<std::string> used;
        for(const auto& kv : cand.document.devices)
        {
            used.insert(kv.second.type);
        }
        /* Transitive deps inside sibling types — walk to fixpoint
           over the sibling registry. */
        studio::PresetRegistry sreg;
        sreg.LoadDirectory(pdev.string(), nullptr);
        bool grew = true;
        while(grew)
        {
            grew = false;
            std::vector<std::string> snapshot(used.begin(),
                                              used.end());
            for(const std::string& id : snapshot)
            {
                const studio::DevicePreset* p = sreg.Find(id);
                if(p == nullptr)
                {
                    continue;
                }
                for(const std::string& dep :
                        studio::PresetDependencies(*p))
                {
                    if(used.insert(dep).second)
                    {
                        grew = true;
                    }
                }
            }
        }
        std::set<std::string> have;
        for(const studio::DevicePreset& p : cand.extracted_types)
        {
            have.insert(p.id);
        }
        for(const std::string& id : used)
        {
            if(have.count(id))
            {
                continue;
            }
            if(const studio::DevicePreset* p = sreg.Find(id))
            {
                cand.extracted_types.push_back(*p);
                have.insert(id);
            }
        }
    }

    if(fs::is_directory(pfx, ec) &&
       !cand.document.effect.preset.empty())
    {
        studio::EffectRegistry sreg;
        sreg.LoadDirectory(pfx.string(), nullptr);
        const std::string& want = cand.document.effect.preset;
        bool have = false;
        for(const studio::EffectDocument& d : cand.extracted_effects)
        {
            have |= d.id == want;
        }
        if(!have)
        {
            if(const studio::EffectDocument* d = sreg.Find(want))
            {
                cand.extracted_effects.push_back(*d);
            }
        }
    }
}

/*---------------------------------------------------------*\
|||| Packed bundle codec (studio-import Task 9).           ||
|||| The wire form of the directory bundle — one JSON      ||
|||| object whose `files` map carries every bundle file:   ||
||||   { "format": "openrgb2-bundle-files", "version": 1,  ||
||||     "files": { "studio.json":            {"encoding": ||
||||                  "utf8","data":"..."},                ||
||||                "presets/devices/x.device.json": {...},||
||||                "assets/<rel>": {"encoding":"base64",  ||
||||                                 "data":"..."} } }     ||
|||| Unpack re-validates every path (bundle-relative only, ||
|||| the known prefixes) and enforces the same size caps   ||
|||| InspectBundle applies — the staged bytes are the      ||
|||| retained original either way.                         ||
\*---------------------------------------------------------*/

bool IsBundleRelPath(const std::string& rel)
{
    if(rel == "studio.json")
    {
        return true;
    }
    auto tail_id = [&](const char* prefix, const char* suffix)
        -> bool
    {
        const std::string pre(prefix), suf(suffix);
        if(rel.size() <= pre.size() + suf.size() ||
           rel.compare(0, pre.size(), pre) != 0 ||
           rel.compare(rel.size() - suf.size(), suf.size(), suf) != 0)
        {
            return false;
        }
        /* The basename between prefix and suffix must be a
           preset id — same charset rule as the file layer. */
        return studio::IsPresetId(
            rel.substr(pre.size(),
                       rel.size() - pre.size() - suf.size()));
    };
    if(tail_id("presets/devices/", ".device.json") ||
       tail_id("presets/effects/", ".effect.json"))
    {
        return true;
    }
    const std::string apre("assets/");
    if(rel.size() > apre.size() &&
       rel.compare(0, apre.size(), apre) == 0)
    {
        return studio::IsPresetAssetPath(rel.substr(apre.size()));
    }
    return false;
}

constexpr char kB64[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string B64Encode(const std::string& in)
{
    std::string out;
    out.reserve((in.size() + 2) / 3 * 4);
    for(size_t i = 0; i < in.size(); i += 3)
    {
        const unsigned int a = (unsigned char)in[i];
        const unsigned int b = i + 1 < in.size()
            ? (unsigned char)in[i + 1] : 0;
        const unsigned int c = i + 2 < in.size()
            ? (unsigned char)in[i + 2] : 0;
        out += kB64[a >> 2];
        out += kB64[((a & 3) << 4) | (b >> 4)];
        out += (i + 1 < in.size()) ? kB64[((b & 15) << 2) | (c >> 6)]
                                   : '=';
        out += (i + 2 < in.size()) ? kB64[c & 63] : '=';
    }
    return out;
}

bool B64Decode(const std::string& in, std::string& out)
{
    if(in.size() % 4 != 0)
    {
        return false;
    }
    auto val = [](char c) -> int
    {
        if(c >= 'A' && c <= 'Z') return c - 'A';
        if(c >= 'a' && c <= 'z') return c - 'a' + 26;
        if(c >= '0' && c <= '9') return c - '0' + 52;
        if(c == '+') return 62;
        if(c == '/') return 63;
        return -1;
    };
    out.clear();
    out.reserve(in.size() / 4 * 3);
    for(size_t i = 0; i < in.size(); i += 4)
    {
        int v[4];
        int pad = 0;
        for(int k = 0; k < 4; ++k)
        {
            const char c = in[i + (size_t)k];
            if(c == '=')
            {
                v[k] = 0;
                ++pad;
            }
            else
            {
                v[k] = val(c);
                if(v[k] < 0 || pad > 0)
                {
                    return false;   /* '=' only at the tail */
                }
            }
        }
        if(pad > 2 || (pad > 0 && i + 4 != in.size()))
        {
            return false;
        }
        const unsigned int trip =
            ((unsigned)v[0] << 18) | ((unsigned)v[1] << 12) |
            ((unsigned)v[2] << 6)  | (unsigned)v[3];
        out += (char)(trip >> 16);
        if(pad < 2) out += (char)(trip >> 8);
        if(pad < 1) out += (char)trip;
    }
    return true;
}

/* Unpack a packed-bundle `files` map into `dest` — a staging
   dir the caller owns (inside the managed root). Bounds:
   BUNDLE_MAX_JSON_BYTES per .json, BUNDLE_MAX_ASSET_BYTES per
   decoded asset, BUNDLE_MAX_TOTAL_BYTES total, and a file-count
   cap — a zip-bomb-shaped map of tiny entries is as hostile as
   a huge one (directory entry + metadata cost per file). Every
   path must satisfy IsBundleRelPath. Returns the file count. */
constexpr unsigned int kBundleMaxFiles = 512;
bool UnpackBundleFiles(const nlohmann::json& files,
                       const fs::path& dest,
                       std::vector<std::string>& errors,
                       unsigned int* count = nullptr)
{
    unsigned long long total = 0;
    unsigned int n = 0;
    for(auto it = files.begin(); it != files.end(); ++it)
    {
        if(n >= kBundleMaxFiles)
        {
            Err(errors, "bundle exceeds the " +
                        std::to_string(kBundleMaxFiles) +
                        "-file entry bound");
            return false;
        }
        const std::string rel = it.key();
        if(!IsBundleRelPath(rel))
        {
            Err(errors, "files." + rel + ": not a bundle-relative"
                        " path (studio.json, presets/{devices,"
                        "effects}/<id>.*.json or assets/<rel>)");
            return false;
        }
        const nlohmann::json& ent = it.value();
        const std::string enc =
            ent.value("encoding", std::string());
        std::string bytes;
        const bool is_json = rel.size() >= 5 &&
            rel.compare(rel.size() - 5, 5, ".json") == 0;
        if(enc == "utf8")
        {
            if(!ent.value("data", nlohmann::json()).is_string())
            {
                Err(errors, "files." + rel + ": utf8 data must be"
                            " a string");
                return false;
            }
            bytes = ent["data"].get<std::string>();
        }
        else if(enc == "base64")
        {
            if(!ent.value("data", nlohmann::json()).is_string() ||
               !B64Decode(ent["data"].get<std::string>(), bytes))
            {
                Err(errors, "files." + rel + ": bad base64 data");
                return false;
            }
        }
        else
        {
            Err(errors, "files." + rel +
                        ": encoding must be utf8|base64");
            return false;
        }
        const unsigned long long cap = is_json
            ? studio::BUNDLE_MAX_JSON_BYTES
            : studio::BUNDLE_MAX_ASSET_BYTES;
        if(bytes.size() > cap)
        {
            Err(errors, "files." + rel + ": exceeds the " +
                        std::to_string(cap / 1024 / 1024) +
                        " MiB file bound");
            return false;
        }
        total += bytes.size();
        if(total > studio::BUNDLE_MAX_TOTAL_BYTES)
        {
            Err(errors, "bundle exceeds the " +
                std::to_string(
                    studio::BUNDLE_MAX_TOTAL_BYTES / 1024 / 1024) +
                " MiB total bound");
            return false;
        }
        const fs::path dst =
            (dest / fs::path(rel)).lexically_normal();
        /* Defense in depth — the rel-path check above is the
           gate; the normalized join must still resolve under
           dest. */
        const std::string dstr = dst.generic_string();
        const std::string rstr = dest.generic_string() + "/";
        if(dstr.compare(0, rstr.size(), rstr) != 0)
        {
            Err(errors, "files." + rel +
                        ": escapes the unpack dir — refused");
            return false;
        }
        std::error_code ec;
        fs::create_directories(dst.parent_path(), ec);
        if(ec)
        {
            Err(errors, "files." + rel + ": cannot create dir: " +
                        ec.message());
            return false;
        }
        std::ofstream out(dst, std::ios::binary | std::ios::trunc);
        out << bytes;
        if(!out)
        {
            Err(errors, "files." + rel + ": write failed");
            return false;
        }
        ++n;
    }
    if(count != nullptr)
    {
        *count = n;
    }
    return true;
}

/* Pack a bundle directory into the wire `files` map — every
   regular file under `dir`, path-forwarded, utf8 for .json and
   base64 otherwise. Total stays under BUNDLE_MAX_TOTAL_BYTES. */
bool PackBundleDir(const fs::path& dir, nlohmann::json& files,
                   std::vector<std::string>& errors)
{
    std::error_code ec;
    if(!fs::is_directory(dir, ec))
    {
        return true;    /* empty bundle — nothing to pack */
    }
    unsigned long long total = 0;
    for(fs::recursive_directory_iterator it(dir, ec), end;
        !ec && it != end; it.increment(ec))
    {
        if(!it->is_regular_file(ec))
        {
            continue;
        }
        const std::string rel =
            fs::relative(it->path(), dir, ec).generic_string();
        std::string bytes;
        if(!ReadBytes(it->path().string(), bytes,
                      studio::BUNDLE_MAX_TOTAL_BYTES + 1))
        {
            Err(errors, rel + ": unreadable or oversized");
            return false;
        }
        total += bytes.size();
        if(total > studio::BUNDLE_MAX_TOTAL_BYTES)
        {
            Err(errors, "bundle exceeds the " +
                std::to_string(
                    studio::BUNDLE_MAX_TOTAL_BYTES / 1024 / 1024) +
                " MiB total bound");
            return false;
        }
        const bool is_json = rel.size() >= 5 &&
            rel.compare(rel.size() - 5, 5, ".json") == 0;
        files[rel] = nlohmann::json::object({
            { "encoding", is_json ? "utf8" : "base64" },
            { "data",     is_json ? bytes : B64Encode(bytes) },
        });
    }
    return true;
}

/* The conflict-choice suggestion ApplyImport will accept — a
   free id derived from the conflict id (the "-imported" family
   the file layer's suffixing uses). */
std::string SuggestImportId(const std::string& base,
                            const std::set<std::string>& taken)
{
    for(unsigned int n = 1;; ++n)
    {
        const std::string id = base + "-imported" +
            (n == 1 ? std::string() : "-" + std::to_string(n));
        if(studio::IsPresetId(id) && !taken.count(id))
        {
            return id;
        }
    }
}

} /* anonymous namespace */

WorkspaceService::WorkspaceService(std::string root, Deps deps)
    : store_(std::move(root),
             WorkspaceStore::Deps{ deps.devices, deps.effects }),
      deps_(std::move(deps))
{
    /* One bounded worker — IPC handlers only validate, enqueue,
       or read snapshots; every file-touching workspace op runs
       here so PING and the legacy control surface never block
       on a slow commit. */
    worker_ = std::thread([this] { WorkerMain(); });
}

WorkspaceService::~WorkspaceService()
{
    Shutdown();
}

void WorkspaceService::Shutdown()
{
    {
        std::lock_guard<std::mutex> g(jobs_mutex_);
        jobs_stop_.store(true);
    }
    jobs_cv_.notify_all();
    if(worker_.joinable())
    {
        worker_.join();
    }
}

WorkspaceService::Result WorkspaceService::Open(const std::string& id)
{
    std::lock_guard<std::mutex> g(mutex_);
    Result res;
    if(!WorkspaceStore::IsWorkspaceId(id))
    {
        res.error = "invalid workspace id '" + id + "'";
        return res;
    }
    const WorkspaceStore::LoadResult lr = store_.Load(id);
    res.warnings = lr.warnings;
    switch(lr.status)
    {
    case WorkspaceStore::LoadStatus::Empty:
        res.error = "no managed workspace '" + id +
                    "' under " + store_.Root();
        return res;
    case WorkspaceStore::LoadStatus::Failed:
        res.error = lr.error;
        return res;
    case WorkspaceStore::LoadStatus::Recovered:
        /* Honest recovery: the last-valid generation adopted,
           the primary failure reported as a warning. */
        res.recovered = true;
        res.warnings.push_back("recovered: " + lr.error);
        break;
    case WorkspaceStore::LoadStatus::Loaded:
        break;
    }
    current_.id                = id;
    current_.candidate         = lr.candidate;
    current_.workspace_revision = lr.workspace_revision;
    current_.resource_revision  = lr.resource_revision;
    current_.generation         = lr.generation;
    current_.recovered          =
        lr.status == WorkspaceStore::LoadStatus::Recovered;
    /* Re-baseline the external-change signature at every
       adopt — files the service itself just selected are the
       new truth, never a "change" (Task 10). */
    current_.external_sig      = store_.ExternalScan(id).signature;
    PublishLocked();
    res.ok                 = true;
    res.generation         = lr.generation;
    res.workspace_revision = lr.workspace_revision;
    res.resource_revision  = lr.resource_revision;
    return res;
}

std::pair<uint64_t, uint64_t>
WorkspaceService::StoreExpectedRevs(const std::string& id,
                                    uint64_t ws_rev,
                                    uint64_t res_rev)
{
    /* After a generation recovery the published revisions name
       the last-valid generation while the durable marker still
       points at the corrupt one — their revisions legitimately
       differ. A caller whose expectations equal the PUBLISHED
       revisions owns the live state; translate to the real
       marker's. Any other base is a genuinely stale writer the
       store gate still rejects. */
    if(current_.id == id && current_.recovered)
    {
        const WorkspaceStore::Revisions mk =
            store_.LoadRevisions(id);
        if(mk.exists && mk.error.empty())
        {
            if(ws_rev == current_.workspace_revision)
            {
                ws_rev = mk.workspace_revision;
            }
            if(res_rev == current_.resource_revision)
            {
                res_rev = mk.resource_revision;
            }
        }
    }
    return {ws_rev, res_rev};
}

WorkspaceService::Result WorkspaceService::CommitCommon(
    const std::string& id,
    WorkspaceCandidate& cand,
    const std::string& original_bytes,
    uint64_t expected_rev,
    bool apply_after,
    std::shared_ptr<const WorkspaceCandidate>* apply_cand,
    std::vector<std::string> prior_warnings)
{
    Result res;
    res.warnings = std::move(prior_warnings);
    /* Service-level stale-writer fast path for the workspace
       currently open — the store's marker check below is the
       authoritative gate either way. */
    if(!current_.id.empty() && current_.id == id &&
       expected_rev != current_.workspace_revision)
    {
        res.error  = "stale workspace revision: expected " +
                     std::to_string(expected_rev) + " but '" + id +
                     "' is at " +
                     std::to_string(current_.workspace_revision);
        res.detail = "revision_conflict";
        return res;
    }

    /* The durable gate expects the MARKER revision; after a
       recovery that differs from the published revision the
       client legitimately holds — translate, and say so. */
    uint64_t store_expected_rev =
        StoreExpectedRevs(id, expected_rev, 0).first;
    if(store_expected_rev != expected_rev)
    {
        res.warnings.push_back(
            "commit after generation recovery: durable marker "
            "re-based from revision " +
            std::to_string(store_expected_rev));
    }

    const WorkspaceStore::CommitResult cr =
        store_.Commit(id, cand, original_bytes,
                      store_expected_rev);
    /* Append — the recovery notice and the caller's prior
       warnings already sit in res.warnings. */
    res.warnings.insert(res.warnings.end(),
                        cr.warnings.begin(), cr.warnings.end());
    if(!cr.ok)
    {
        res.error = cr.error;
        if(cr.error.find("revision") != std::string::npos ||
           cr.error.find("stale")    != std::string::npos)
        {
            res.detail = "revision_conflict";
        }
        return res;
    }
    res.committed          = true;
    res.generation         = cr.generation;
    res.workspace_revision = cr.workspace_revision;
    res.resource_revision  = cr.resource_revision;

    current_.id                = id;
    current_.candidate         = cr.candidate;
    current_.workspace_revision = cr.workspace_revision;
    current_.resource_revision  = cr.resource_revision;
    current_.generation         = cr.generation;
    current_.recovered          = false;   /* marker healed by
                                              this commit */
    /* Our own commit is the new baseline — the files the
       store just wrote are never an external "change". */
    current_.external_sig      = store_.ExternalScan(id).signature;
    PublishLocked();

    /* Publish AFTER the durable commit — and AFTER the caller
       releases the service mutex. The apply emits the scene
       event whose payload re-reads this service (the managed
       flag), so running it here under the lock is a same-
       thread self-deadlock; a failed apply never rolls the
       generation back, it is reported honestly. */
    if(apply_after && apply_cand && deps_.apply_scene &&
       cr.candidate && cr.candidate->resolved)
    {
        *apply_cand = cr.candidate;
    }
    res.ok = true;
    return res;
}

void WorkspaceService::ApplyAfterCommit(
    const std::shared_ptr<const WorkspaceCandidate>& cand,
    uint64_t generation, Result& res)
{
    if(!cand || !deps_.apply_scene)
    {
        return;
    }
    std::string aerr;
    bool applied_ok = false;
    try
    {
        applied_ok = deps_.apply_scene(cand, aerr);
    }
    catch(const std::exception& e)
    {
        aerr = std::string("apply threw: ") + e.what();
    }
    catch(...)
    {
        aerr = "apply threw";
    }
    if(applied_ok)
    {
        res.applied = true;
    }
    else
    {
        res.error = "committed generation " +
                    std::to_string(generation) +
                    " but the scene apply failed: " + aerr;
    }
}

WorkspaceService::ExternalCheck WorkspaceService::CheckExternal()
{
    ExternalCheck out;
    const PublishedState snap = Snapshot();
    if(!snap.managed)
    {
        return out;
    }
    out.managed = true;
    /* Capture the baseline under the lock, scan OFF it (file
       I/O on a reader thread — the DocumentBytes discipline),
       then re-verify the baseline did not move. A commit
       landing mid-scan reports "none" this round; the next
       poll compares against the new baseline. */
    std::string baseline;
    {
        std::lock_guard<std::mutex> g(mutex_);
        if(current_.id != snap.id || current_.external_sig.empty())
        {
            out.state = "none";
            return out;
        }
        baseline = current_.external_sig;
    }
    const WorkspaceStore::ExternalFingerprint fp =
        store_.ExternalScan(snap.id);
    {
        std::lock_guard<std::mutex> g(mutex_);
        if(current_.id != snap.id ||
           current_.external_sig != baseline)
        {
            out.state = "none";
            return out;
        }
        if(!fp.watched)
        {
            /* Managed id, but its workspace dir is gone —
               that IS a deletion. */
            out.state  = "deleted";
            out.detail = "workspace directory removed";
            return out;
        }
        if(fp.signature == baseline)
        {
            out.state = "none";
            return out;
        }
        out.state  = fp.missing ? "deleted" : "modified";
        out.detail = fp.detail.empty()
            ? (fp.missing ? "workspace files were removed"
                          : "workspace files changed on disk")
            : fp.detail;
        return out;
    }
}

WorkspaceService::Result WorkspaceService::RestoreBackup(
    const std::string& id,
    uint64_t expected_workspace_revision,
    uint64_t expected_resource_revision)
{
    std::shared_ptr<const WorkspaceCandidate> apply_cand;
    Result res;
    {
        std::lock_guard<std::mutex> g(mutex_);
        if(!WorkspaceStore::IsWorkspaceId(id))
        {
            res.error  = "invalid workspace id '" + id + "'";
            res.detail = "bad_request";
            return res;
        }
        /* Same commit-time gate as CommitJson — the durable
           marker decides, never the enqueue snapshot. */
        const WorkspaceStore::Revisions rev =
            store_.LoadRevisions(id);
        if(rev.exists && !rev.error.empty())
        {
            res.error  = "workspace marker unreadable: " +
                         rev.error;
            res.detail = "marker_unreadable";
            return res;
        }
        if(!rev.exists)
        {
            res.error  = "no managed workspace '" + id +
                         "' under " + store_.Root();
            res.detail = "bad_request";
            return res;
        }
        /* Recovery translation — same as CommitJson: the
           caller legitimately holds the PUBLISHED (recovered)
           revisions while the marker still names the corrupt
           generation. */
        const bool recovered_base =
            current_.id == id && current_.recovered &&
            expected_workspace_revision ==
                current_.workspace_revision &&
            expected_resource_revision ==
                current_.resource_revision;
        if(!recovered_base &&
           (rev.workspace_revision != expected_workspace_revision ||
            rev.resource_revision  != expected_resource_revision))
        {
            res.error = "stale revision: expected {" +
                std::to_string(expected_workspace_revision) + "," +
                std::to_string(expected_resource_revision) +
                "} but '" + id + "' is at {" +
                std::to_string(rev.workspace_revision) + "," +
                std::to_string(rev.resource_revision) + "}";
            res.detail = "revision_conflict";
            return res;
        }
        const WorkspaceStore::LoadResult lr =
            store_.LoadBackup(id);
        res.warnings = lr.warnings;
        if(lr.status != WorkspaceStore::LoadStatus::Loaded ||
           !lr.candidate)
        {
            res.error = lr.error.empty()
                ? "no usable backup generation for '" + id + "'"
                : lr.error;
            return res;
        }
        /* The backup's candidate commits FORWARD as a new
           immutable generation — the store's own atomic
           contract; never write-in-place over the active
           generation (Task 10 hard rule). */
        WorkspaceCandidate cand = *lr.candidate;
        res = CommitCommon(id, cand, std::string(),
                           expected_workspace_revision,
                           /*apply_after=*/true, &apply_cand,
                           /*prior_warnings=*/lr.warnings);
        if(res.ok)
        {
            res.recovered = true;
            res.warnings.push_back(
                "restored generation " + std::to_string(lr.generation) +
                " (the marker backup's last-valid generation)");
        }
    }
    ApplyAfterCommit(apply_cand, res.generation, res);
    return res;
}

WorkspaceService::Result WorkspaceService::Save(
    const std::string& id,
    const nlohmann::json& document,
    uint64_t expected_workspace_revision)
{
    std::shared_ptr<const WorkspaceCandidate> apply_cand;
    Result res;
    {
        std::lock_guard<std::mutex> g(mutex_);
        if(!WorkspaceStore::IsWorkspaceId(id))
        {
            res.error = "invalid workspace id '" + id + "'";
            return res;
        }
        /* The saved document parses as a candidate against the
           effective libraries — the same candidate contract the
           store and CLI loader share. */
        WorkspaceCandidate cand;
        std::vector<std::string> errors;
        if(!ParseWorkspaceCandidate(document,
                                store_.EffectiveDeviceRegistry(),
                                store_.EffectiveEffectRegistry(),
                                cand, errors, res.warnings))
        {
            for(const std::string& e : errors)
            {
                if(!res.error.empty())
                {
                    res.error += "; ";
                }
                res.error += e;
            }
            if(res.error.empty())
            {
                res.error = "document failed to parse as a "
                            "workspace candidate";
            }
            return res;
        }
        res = CommitCommon(id, cand, document.dump(2),
                           expected_workspace_revision,
                           /*apply_after=*/true, &apply_cand,
                           /*prior_warnings=*/res.warnings);
    }
    /* The apply runs after the lock is released — a scene
       publish emits events that re-enter this service. */
    ApplyAfterCommit(apply_cand, res.generation, res);
    return res;
}

WorkspaceService::Result WorkspaceService::Import(
    const std::string& id,
    const nlohmann::json& document,
    uint64_t expected_workspace_revision)
{
    std::lock_guard<std::mutex> g(mutex_);
    Result res;
    if(!WorkspaceStore::IsWorkspaceId(id))
    {
        res.error = "invalid workspace id '" + id + "'";
        return res;
    }
    WorkspaceCandidate cand;
    std::vector<std::string> errors;
    if(!ParseWorkspaceCandidate(document,
                                store_.EffectiveDeviceRegistry(),
                                store_.EffectiveEffectRegistry(),
                                cand, errors, res.warnings))
    {
        for(const std::string& e : errors)
        {
            if(!res.error.empty())
            {
                res.error += "; ";
            }
            res.error += e;
        }
        if(res.error.empty())
        {
            res.error = "document failed to parse as a workspace "
                        "candidate";
        }
        return res;
    }

    /* Import-boundary hygiene FIRST: an imported workspace never
       automatically enables output, input capture, or verified
       hardware ownership — the stored document and its resolved
       scene are both sanitized before anything commits. */
    SanitizeImportedWorkspace(cand.document);
    if(cand.resolved)
    {
        auto rs = std::make_shared<studio::SceneDocument>(
            *cand.resolved);
        SanitizeImportedScene(*rs);
        cand.resolved = std::move(rs);
    }
    res.warnings.push_back(
        "import sanitized: output/inputs/ownership unarmed; "
        "identity hints kept for review");

    /* Imports NEVER auto-apply — the committed generation waits
       for an explicit open/apply decision. */
    return CommitCommon(id, cand, document.dump(2),
                        expected_workspace_revision,
                        /*apply_after=*/false, nullptr,
                        /*prior_warnings=*/res.warnings);
}

WorkspaceService::Result WorkspaceService::ImportFile(
    const std::string& id,
    const std::string& path,
    uint64_t expected_workspace_revision)
{
    std::lock_guard<std::mutex> g(mutex_);
    Result res;
    if(!WorkspaceStore::IsWorkspaceId(id))
    {
        res.error = "invalid workspace id '" + id + "'";
        return res;
    }
    std::string bytes;
    if(!ReadBytes(path, bytes, WorkspaceStore::kMaxDocumentBytes))
    {
        res.error = "cannot read workspace file '" + path +
                    "' (missing or oversized)";
        return res;
    }
    const nlohmann::json j =
        nlohmann::json::parse(bytes, nullptr, false);
    if(j.is_discarded() || !j.is_object())
    {
        res.error = path + " is not a valid workspace document";
        return res;
    }

    /* Scratch registries: base + managed library + the file's
       sibling presets/ dirs — the folder-of-files import the
       source ConfigStore's File->Open flow served. */
    studio::PresetRegistry devreg = store_.EffectiveDeviceRegistry();
    studio::EffectRegistry effreg = store_.EffectiveEffectRegistry();
    const fs::path dir = fs::path(path).parent_path();
    LoadSiblingPresets(dir, devreg, effreg, res.warnings);

    WorkspaceCandidate cand;
    std::vector<std::string> errors;
    if(!ParseWorkspaceCandidate(j, devreg, effreg, cand,
                                errors, res.warnings))
    {
        for(const std::string& e : errors)
        {
            if(!res.error.empty())
            {
                res.error += "; ";
            }
            res.error += e;
        }
        if(res.error.empty())
        {
            res.error = path +
                        " failed to parse as a workspace candidate";
        }
        return res;
    }
    /* A compact source carries no extracted types of its own —
       pull the sibling files it actually references so the
       commit installs them (collision-aware) and the
       generation snapshots them. */
    HarvestUsedSiblings(dir, cand);

    SanitizeImportedWorkspace(cand.document);
    if(cand.resolved)
    {
        auto rs = std::make_shared<studio::SceneDocument>(
            *cand.resolved);
        SanitizeImportedScene(*rs);
        cand.resolved = std::move(rs);
    }
    res.warnings.push_back(
        "import sanitized: output/inputs/ownership unarmed; "
        "identity hints kept for review");

    /* The original imported bytes are retained verbatim in the
       generation — recovery never needs to re-derive them. */
    return CommitCommon(id, cand, bytes,
                        expected_workspace_revision,
                        /*apply_after=*/false, nullptr,
                        /*prior_warnings=*/res.warnings);
}

std::shared_ptr<const WorkspaceCandidate>
WorkspaceService::CurrentCandidate() const
{
    std::lock_guard<std::mutex> g(mutex_);
    return current_.candidate;
}

void WorkspaceService::PublishLocked()
{
    /* current_ -> snap_ copy; snap_mutex_ is held for the
       copy only — readers never queue behind commit I/O. */
    PublishedState s;
    s.managed            = !current_.id.empty();
    s.id                 = current_.id;
    s.workspace_revision = current_.workspace_revision;
    s.resource_revision  = current_.resource_revision;
    s.generation         = current_.generation;
    if(current_.candidate)
    {
        s.name        = current_.candidate->document.meta.name;
        s.source_form =
            WorkspaceSourceFormName(current_.candidate->source_form);
    }
    std::lock_guard<std::mutex> g(snap_mutex_);
    snap_ = std::move(s);
}

WorkspaceService::PublishedState
WorkspaceService::Snapshot() const
{
    std::lock_guard<std::mutex> g(snap_mutex_);
    return snap_;
}

std::string WorkspaceService::CurrentId() const
{
    return Snapshot().id;
}

uint64_t WorkspaceService::WorkspaceRevision() const
{
    return Snapshot().workspace_revision;
}

uint64_t WorkspaceService::ResourceRevision() const
{
    return Snapshot().resource_revision;
}

uint64_t WorkspaceService::CurrentGeneration() const
{
    return Snapshot().generation;
}

bool WorkspaceService::ManagedActive() const
{
    return Snapshot().managed;
}

/*---------------------------------------------------------*\
||| IPC job machinery (Task 3) — one bounded worker; the      ||
||| handler layer enqueues only.                              ||
\*---------------------------------------------------------*/

void WorkspaceService::SetJobSurface(JobSurface surface)
{
    std::lock_guard<std::mutex> g(jobs_mutex_);
    surface_ = std::move(surface);
}

void WorkspaceService::SetJobEventHook(
    std::function<void(const JobStatus&)> hook)
{
    std::lock_guard<std::mutex> g(jobs_mutex_);
    surface_.job_event = std::move(hook);
}

void WorkspaceService::SetPreJobHookForTesting(
    std::function<void(const std::string&)> hook)
{
    std::lock_guard<std::mutex> g(jobs_mutex_);
    pre_job_hook_ = std::move(hook);
}

namespace
{

bool JsonU64(const nlohmann::json& j, const char* key,
             uint64_t& out)
{
    const auto it = j.find(key);
    if(it == j.end() || !it->is_number_unsigned())
    {
        return false;
    }
    out = it->get<uint64_t>();
    return true;
}

bool IsJobVerb(const std::string& v)
{
    return v == "validate"       || v == "commit"       ||
           v == "save_copy"    || v == "import_inspect" ||
           v == "import_commit" || v == "export"      ||
           v == "resource_install" ||
           v == "resource_remove" ||
           v == "reload"      || v == "restore_backup";
}

} /* anonymous namespace */

WorkspaceService::JobResult WorkspaceService::Enqueue(
    uint64_t session_id, const std::string& verb,
    const nlohmann::json& body)
{
    JobResult r;
    if(!IsJobVerb(verb))
    {
        r.error  = "unknown job verb '" + verb + "'";
        r.detail = "bad_request";
        return r;
    }

    /* Cheap synchronous validation — the worker re-checks the
       durable revisions; enqueue never does file I/O. `reload`
       and `restore_backup` may omit id: the worker falls back
       to the current workspace. */
    const bool needs_id = verb != "export" &&
                          verb != "import_inspect" &&
                          verb != "reload" &&
                          verb != "restore_backup";
    const std::string id = body.value("id", std::string());
    if(needs_id && id.empty())
    {
        r.error  = "'" + verb + "' requires an id";
        r.detail = "bad_request";
        return r;
    }
    if(!id.empty() && !WorkspaceStore::IsWorkspaceId(id))
    {
        r.error  = "invalid workspace id '" + id + "'";
        r.detail = "bad_request";
        return r;
    }
    const bool needs_revisions =
        verb == "commit" || verb == "save_copy" ||
        verb == "validate" || verb == "resource_install" ||
        verb == "resource_remove" || verb == "restore_backup";
    if(needs_revisions)
    {
        uint64_t unused = 0;
        if(!JsonU64(body, "base_revision", unused) ||
           !JsonU64(body, "resource_revision", unused))
        {
            r.error  = "'" + verb + "' requires base_revision and "
                       "resource_revision";
            r.detail = "bad_request";
            return r;
        }
    }
    /* resource_install carries the resource document inline —
       an effect object, or kind:"device_type" + a device_type
       object; bounded by the job-body cap like any verb. */
    if(verb == "resource_install")
    {
        const std::string kind =
            body.value("kind", std::string("effect"));
        const bool ok = (kind == "effect" &&
                         body.contains("effect") &&
                         body["effect"].is_object()) ||
                        (kind == "device_type" &&
                         body.contains("device_type") &&
                         body["device_type"].is_object());
        if(!ok)
        {
            r.error  = "'resource_install' requires an effect "
                       "object or kind:'device_type' + a "
                       "device_type object";
            r.detail = "bad_request";
            return r;
        }
    }
    if(verb == "resource_remove" &&
       (!body.value("kind", nlohmann::json()).is_string() ||
        !body.value("resource_id", nlohmann::json()).is_string()))
    {
        r.error  = "'resource_remove' requires kind + resource_id";
        r.detail = "bad_request";
        return r;
    }
    /* resource_remove is a library op on an EXISTING workspace
       (the marker gate) — an id is required. */
    if(verb == "resource_remove" &&
       body.value("id", std::string()).empty())
    {
        r.error  = "'resource_remove' requires an id";
        r.detail = "bad_request";
        return r;
    }
    const bool needs_source = verb == "commit" || verb == "save_copy" ||
                              verb == "validate";
    if(needs_source && !body.contains("document") &&
       !body.contains("staged"))
    {
        r.error  = "'" + verb + "' requires document or staged";
        r.detail = "bad_request";
        return r;
    }
    if((verb == "import_inspect" || verb == "import_commit") &&
       !body.value("staged", std::string()).size())
    {
        r.error  = "'" + verb + "' requires a staged token";
        r.detail = "bad_request";
        return r;
    }

    {
        std::lock_guard<std::mutex> g(jobs_mutex_);
        if(jobs_pending_.size() >= deps_.max_pending_jobs)
        {
            r.error  = "too many pending workspace jobs";
            r.detail = "busy";
            return r;
        }
        Job job;
        job.st.found      = true;
        job.st.job_id     = next_job_id_++;
        job.st.session_id = session_id;
        job.st.verb       = verb;
        job.st.state      = JobState::Queued;
        job.body          = body;
        job.seq           = next_job_seq_++;
        const uint64_t jid = job.st.job_id;
        jobs_[jid] = std::move(job);
        jobs_pending_.push_back(jid);
        r.accepted = true;
        r.job_id   = jid;
    }
    jobs_cv_.notify_one();
    return r;
}

WorkspaceService::JobStatus WorkspaceService::Status(
    uint64_t session_id, uint64_t job_id) const
{
    std::lock_guard<std::mutex> g(jobs_mutex_);
    JobStatus st;
    const auto it = jobs_.find(job_id);
    if(it == jobs_.end() || it->second.st.session_id != session_id)
    {
        return st;     /* found:false — foreign/unknown job */
    }
    return it->second.st;
}

WorkspaceService::JobResult WorkspaceService::Cancel(
    uint64_t session_id, uint64_t job_id)
{
    JobResult r;
    std::lock_guard<std::mutex> g(jobs_mutex_);
    auto it = jobs_.find(job_id);
    if(it == jobs_.end() || it->second.st.session_id != session_id)
    {
        r.error  = "unknown job";
        r.detail = "bad_request";
        return r;
    }
    Job& job = it->second;
    if(job.st.state != JobState::Queued)
    {
        /* a running commit past the durable marker is never
           rolled back; terminal jobs are already done */
        r.error  = "job is not queued";
        r.detail = "bad_request";
        return r;
    }
    job.st.state = JobState::Cancelled;
    jobs_pending_.erase(
        std::remove(jobs_pending_.begin(), jobs_pending_.end(),
                    job_id),
        jobs_pending_.end());
    r.accepted = true;
    r.job_id   = job_id;
    return r;
}

void WorkspaceService::CancelSession(uint64_t session_id)
{
    std::vector<JobStatus> finished;
    {
        std::lock_guard<std::mutex> g(jobs_mutex_);
        for(auto& kv : jobs_)
        {
            Job& job = kv.second;
            if(job.st.session_id == session_id &&
               job.st.state == JobState::Queued)
            {
                job.st.state = JobState::Cancelled;
                job.st.error = "session ended";
                finished.push_back(job.st);
            }
        }
        jobs_pending_.erase(
            std::remove_if(jobs_pending_.begin(),
                           jobs_pending_.end(),
                           [&](uint64_t jid)
                           {
                               const auto it = jobs_.find(jid);
                               return it == jobs_.end() ||
                                      it->second.st.state !=
                                          JobState::Queued;
                           }),
            jobs_pending_.end());
        /* terminal records of a dead session are useless —
           reconcile is by revision, not job id */
        for(auto it = jobs_.begin(); it != jobs_.end();)
        {
            if(it->second.st.session_id == session_id &&
               it->second.st.state != JobState::Queued &&
               it->second.st.state != JobState::Running)
            {
                it = jobs_.erase(it);
            }
            else
            {
                ++it;
            }
        }
    }
    for(const JobStatus& st : finished)
    {
        JobSurface surface;
        {
            std::lock_guard<std::mutex> g(jobs_mutex_);
            surface = surface_;
        }
        if(surface.job_event)
        {
            surface.job_event(st);
        }
    }
}

void WorkspaceService::WorkerMain()
{
    for(;;)
    {
        uint64_t jid = 0;
        {
            std::unique_lock<std::mutex> l(jobs_mutex_);
            jobs_cv_.wait(l, [&] {
                return jobs_stop_.load() || !jobs_pending_.empty();
            });
            if(jobs_stop_.load())
            {
                return;
            }
            jid = jobs_pending_.front();
            jobs_pending_.pop_front();
            jobs_[jid].st.state = JobState::Running;
        }

        std::function<void(const std::string&)> hook;
        Job  snapshot;
        {
            std::lock_guard<std::mutex> g(jobs_mutex_);
            snapshot = jobs_[jid];
            hook     = pre_job_hook_;
        }
        JobStatus done;
        try
        {
            if(hook)
            {
                hook(snapshot.st.verb);
            }
            done = RunJob(snapshot);
        }
        catch(const std::exception& e)
        {
            /* a throwing job fails honestly — the worker must
               never take the process down */
            done                = snapshot.st;
            done.state          = JobState::Failed;
            done.error          = "job threw";
            done.detail         = e.what();
        }
        catch(...)
        {
            done                = snapshot.st;
            done.state          = JobState::Failed;
            done.error          = "job threw";
            done.detail         = "internal";
        }

        JobSurface surface;
        {
            std::lock_guard<std::mutex> g(jobs_mutex_);
            auto it = jobs_.find(jid);
            if(it != jobs_.end())
            {
                it->second.st = done;
                /* Terminal records stay queryable for
                   reconcile — the request body (a full staged
                   or inline document, up to ~1 MiB) does not:
                   retention keeps status, never payload. */
                it->second.body = nlohmann::json();
            }
            /* bounded retention — oldest terminal records evict */
            while(jobs_.size() > kMaxJobRecords)
            {
                auto oldest = jobs_.end();
                for(auto it = jobs_.begin(); it != jobs_.end(); ++it)
                {
                    const JobState s = it->second.st.state;
                    const bool terminal =
                        s == JobState::Succeeded ||
                        s == JobState::Failed    ||
                        s == JobState::Cancelled;
                    if(terminal &&
                       (oldest == jobs_.end() ||
                        it->second.seq < oldest->second.seq))
                    {
                        oldest = it;
                    }
                }
                if(oldest == jobs_.end())
                {
                    break;
                }
                jobs_.erase(oldest);
            }
            surface = surface_;
        }
        /* the completion EVENT — emitted OFF the job mutex so a
           slow subscriber never stalls the worker */
        if(surface.job_event)
        {
            try { surface.job_event(done); } catch(...) {}
        }
    }
}

WorkspaceService::JobStatus WorkspaceService::RunJob(
    const Job& job)
{
    JobStatus st = job.st;      /* carries id/session/verb    */
    const nlohmann::json& body = job.body;
    const std::string&    verb = job.st.verb;

    auto fail = [&](std::string err, std::string detail)
        -> JobStatus
    {
        st.state  = JobState::Failed;
        st.error  = std::move(err);
        st.detail = std::move(detail);
        return st;
    };
    auto done = [&](const Result& res) -> JobStatus
    {
        st.state              = res.ok ? JobState::Succeeded
                                       : JobState::Failed;
        st.committed          = res.committed;
        st.applied            = res.applied;
        st.recovered          = res.recovered;
        st.generation         = res.generation;
        st.workspace_revision = res.workspace_revision;
        st.resource_revision  = res.resource_revision;
        st.error              = res.error;
        st.detail             = res.detail;
        if(!res.warnings.empty())
        {
            st.result["warnings"] = res.warnings;
            CapReportArray(st.result, "warnings");
        }
        return st;
    };

    JobSurface surface;
    {
        std::lock_guard<std::mutex> g(jobs_mutex_);
        surface = surface_;
    }

    /* The staged payload the job consumes — read on the worker
       thread, never inside the service mutex. */
    auto staged_doc = [&](nlohmann::json& doc, std::string& raw)
        -> std::string
    {
        const auto sit = body.find("staged");
        if(sit == body.end())
        {
            doc = body.value("document", nlohmann::json());
            raw = doc.is_discarded() ? std::string() : doc.dump(2);
            return {};
        }
        if(!sit->is_string())
        {
            return "bad_request";
        }
        if(!surface.read_staged)
        {
            return "staged_unknown";
        }
        std::string err;
        if(!surface.read_staged(sit->get<std::string>(), raw, err))
        {
            return err.empty() ? "staged_unknown" : err;
        }
        doc = nlohmann::json::parse(raw, nullptr, false);
        if(doc.is_discarded() || !doc.is_object())
        {
            return "invalid_document";
        }
        return {};
    };

    if(verb == "validate" || verb == "commit" || verb == "save_copy")
    {
        nlohmann::json doc;
        std::string    raw;
        if(const std::string d = staged_doc(doc, raw); !d.empty())
        {
            return fail(d == "invalid_document"
                            ? "staged bytes are not a JSON object"
                            : d == "staged_unknown"
                                  ? "staged token is not consumable"
                                  : d,
                        d);
        }
        const std::string id = body.value("id", std::string());
        uint64_t base_rev = 0, res_rev = 0;
        JsonU64(body, "base_revision",     base_rev);
        JsonU64(body, "resource_revision", res_rev);

        if(verb == "validate")
        {
            /* Bounded introspection — never commits. The
               candidate form the commit would store is the
               reported validity. */
            WorkspaceCandidate cand;
            std::vector<std::string> errors, warnings;
            {
                std::lock_guard<std::mutex> g(mutex_);
                ParseWorkspaceCandidate(
                    doc, store_.EffectiveDeviceRegistry(),
                    store_.EffectiveEffectRegistry(),
                    cand, errors, warnings);
            }
            st.state = JobState::Succeeded;
            st.result["valid"]    = errors.empty();
            st.result["errors"]   = errors;
            st.result["warnings"] = warnings;
            /* Same 1 MiB reply bound as the import report. */
            CapReportArray(st.result, "errors");
            CapReportArray(st.result, "warnings");
            return st;
        }
        /* commit applies + adopts; save_copy only commits — the
           open workspace is untouched. */
        const Result res = CommitJson(
            id, doc, raw, base_rev, res_rev,
            /*apply_after=*/verb == "commit",
            /*adopt=*/      verb == "commit");
        return done(res);
    }

    if(verb == "import_inspect")
    {
        nlohmann::json doc;
        std::string    raw;
        if(const std::string d = staged_doc(doc, raw); !d.empty())
        {
            return fail("staged bundle is not readable", d);
        }
        std::vector<std::string> errors, warnings;
        nlohmann::json report;
        {
            std::lock_guard<std::mutex> g(mutex_);
            if(doc.is_object() && doc.contains("files") &&
               doc["files"].is_object())
            {
                /* Packed bundle — unpack to a scratch dir inside
                   the managed root and run InspectBundle. */
                report = InspectPackedBundle(doc, errors,
                                             warnings);
            }
            else
            {
                report = InspectCandidateJson(
                    doc, store_.EffectiveDeviceRegistry(),
                    store_.EffectiveEffectRegistry(),
                    errors, warnings);
            }
        }
        st.state   = errors.empty() ? JobState::Succeeded
                                    : JobState::Failed;
        st.error   = errors.empty() ? std::string()
                                    : errors.front();
        st.result  = std::move(report);
        return st;
    }

    if(verb == "import_commit")
    {
        nlohmann::json doc;
        std::string    raw;
        if(const std::string d = staged_doc(doc, raw); !d.empty())
        {
            return fail("staged bundle is not readable", d);
        }
        const std::string id = body.value("id", std::string());
        uint64_t base_rev = 0, res_rev = 0;
        JsonU64(body, "base_revision",     base_rev);
        JsonU64(body, "resource_revision", res_rev);
        /* Conflict choices — {conflict_id: import_as_id} — the
           same map InspectBundle's report offered. */
        std::map<std::string, std::string> choices;
        if(body.contains("choices") && body["choices"].is_object())
        {
            for(auto it = body["choices"].begin();
                it != body["choices"].end(); ++it)
            {
                if(it.value().is_string())
                {
                    choices[it.key()] =
                        it.value().get<std::string>();
                }
            }
        }
        const std::string mode =
            body.value("mode", std::string());
        const Result res =
            ImportStagedJson(id, doc, raw, choices, mode,
                             base_rev, res_rev);
        return done(res);
    }

    if(verb == "resource_install")
    {
        const std::string kind =
            body.value("kind", std::string("effect"));
        const std::string id = body.value("id", std::string());
        uint64_t base_rev = 0, res_rev = 0;
        JsonU64(body, "base_revision",     base_rev);
        JsonU64(body, "resource_revision", res_rev);

        if(kind == "device_type")
        {
            /* Task 9 device-type install: ONE validated type —
               collision-safe by default (differing content lands
               suffixed), `replace:true` rewrites the managed
               file-layer entry (the editor's save). */
            studio::DevicePreset preset;
            {
                std::vector<std::string> errs;
                if(!studio::DevicePresetFromJson(
                       body["device_type"], preset, &errs))
                {
                    return fail(
                        "device type invalid: " +
                        (errs.empty() ? std::string("bad shape")
                                      : errs.front()),
                        "invalid_document");
                }
            }
            const bool allow_replace =
                body.value("replace", false);
            WorkspaceStore::ResourceInstallResult ir;
            {
                std::lock_guard<std::mutex> g(mutex_);
                const auto exp =
                    StoreExpectedRevs(id, base_rev, res_rev);
                ir = store_.InstallDeviceTypeResource(
                    id, preset, allow_replace,
                    exp.first, exp.second);
                if(ir.ok && id == current_.id)
                {
                    current_.resource_revision =
                        ir.resource_revision;
                    PublishLocked();
                }
            }
            if(!ir.ok)
            {
                Result res;
                res.error = ir.error;
                if(ir.error.find("stale") != std::string::npos ||
                   ir.error.find("revision") != std::string::npos)
                {
                    res.detail = "revision_conflict";
                }
                res.warnings = ir.warnings;
                return done(res);
            }
            Result res;
            res.ok                 = true;
            res.workspace_revision = ir.workspace_revision;
            res.resource_revision  = ir.resource_revision;
            res.warnings           = ir.warnings;
            st = done(res);
            st.result["landed_id"] = ir.landed_id;
            st.result["reused"]    = ir.reused;
            st.result["replaced"]  = ir.replaced;
            return st;
        }

        /* Task 6 personal-look install: ONE validated effect
           document into the managed library — collision-safe
           (never overwrites), revision-gated on BOTH the
           workspace and resource revisions, and returning the
           landing id + refreshed revisions. */
        studio::EffectDocument fx;
        {
            std::vector<std::string> errs;
            if(!studio::EffectDocumentFromJson(
                   body["effect"], fx, &errs))
            {
                return fail(
                    "effect document invalid: " +
                    (errs.empty() ? std::string("bad shape")
                                  : errs.front()),
                    "invalid_document");
            }
        }

        WorkspaceStore::ResourceInstallResult ir;
        {
            std::lock_guard<std::mutex> g(mutex_);
            const auto exp =
                StoreExpectedRevs(id, base_rev, res_rev);
            ir = store_.InstallEffectResource(id, fx,
                                              exp.first,
                                              exp.second);
            if(ir.ok && id == current_.id)
            {
                /* Keep the published snapshot coherent — a
                   landed resource is part of the current
                   workspace's resource set immediately. */
                current_.resource_revision = ir.resource_revision;
                PublishLocked();
            }
        }
        if(!ir.ok)
        {
            Result res;
            res.error = ir.error;
            if(ir.error.find("stale") != std::string::npos ||
               ir.error.find("revision") != std::string::npos)
            {
                res.detail = "revision_conflict";
            }
            res.warnings = ir.warnings;
            return done(res);
        }
        Result res;
        res.ok                = true;
        /* No generation commits — the marker's resource
           revision bumped in place; `committed` stays false
           and the result carries the landing id. */
        res.workspace_revision = ir.workspace_revision;
        res.resource_revision  = ir.resource_revision;
        res.warnings          = ir.warnings;
        st = done(res);
        st.result["landed_id"] = ir.landed_id;
        st.result["reused"]    = ir.reused;
        return st;
    }

    if(verb == "resource_remove")
    {
        /* Task 9 library delete — file-layer entries only;
           packaged defaults, in-use ids and child-referenced
           ids refuse by name. Revision-gated like installs. */
        const std::string id   = body.value("id", std::string());
        const std::string kind = body.value("kind", std::string());
        const std::string rid  =
            body.value("resource_id", std::string());
        uint64_t base_rev = 0, res_rev = 0;
        JsonU64(body, "base_revision",     base_rev);
        JsonU64(body, "resource_revision", res_rev);
        WorkspaceStore::ResourceRemoveResult rr;
        {
            std::lock_guard<std::mutex> g(mutex_);
            const auto exp =
                StoreExpectedRevs(id, base_rev, res_rev);
            rr = store_.RemoveResource(id, kind, rid,
                                       exp.first, exp.second);
            if(rr.ok && id == current_.id)
            {
                current_.resource_revision = rr.resource_revision;
                PublishLocked();
            }
        }
        if(!rr.ok)
        {
            Result res;
            res.error = rr.error;
            if(rr.error.find("stale") != std::string::npos ||
               rr.error.find("revision") != std::string::npos)
            {
                res.detail = "revision_conflict";
            }
            else if(rr.error.find("in use") != std::string::npos ||
                    rr.error.find("referenced") != std::string::npos)
            {
                res.detail = "in_use";
            }
            res.warnings = rr.warnings;
            return done(res);
        }
        Result res;
        res.ok                 = true;
        res.workspace_revision = rr.workspace_revision;
        res.resource_revision  = rr.resource_revision;
        res.warnings           = rr.warnings;
        st = done(res);
        st.result["removed"]             = rr.removed;
        st.result["reverted_to_default"] = rr.reverted_to_default;
        return st;
    }

    if(verb == "export")
    {
        /* The managed workspace's portable form — sanitized,
           bundled with the resources it actually uses, staged
           for download on the OWNING session. */
        std::shared_ptr<const WorkspaceCandidate> cand;
        {
            std::lock_guard<std::mutex> g(mutex_);
            if(body.contains("id"))
            {
                const WorkspaceStore::LoadResult lr =
                    store_.Load(body.value("id", std::string()));
                if(lr.status == WorkspaceStore::LoadStatus::Loaded ||
                   lr.status == WorkspaceStore::LoadStatus::Recovered)
                {
                    cand = lr.candidate;
                }
            }
            else
            {
                cand = current_.candidate;
            }
        }
        if(!cand)
        {
            return fail("no managed workspace to export",
                        "bad_request");
        }
        /* Referenced personal looks materialize INSIDE the
           exported document (studio-import Task 9): the named
           preset stays on `effect.preset` as provenance while
           its resolved layers flatten into `effect.layers` —
           an importer that lacks the look file still gets the
           literal stack. Look files are NOT transported in the
           bundle. */
        studio::StudioDocument exdoc =
            studio::SanitizeForExport(cand->document);
        std::vector<std::string> warnings;
        if(exdoc.effect.layers.empty() &&
           !exdoc.effect.preset.empty())
        {
            std::vector<studio::EffectLayer> layers;
            std::vector<std::string> lerrs;
            if(cand->effect_registry.Build(exdoc.effect.preset,
                                           exdoc.effect.seed,
                                           layers, &lerrs))
            {
                exdoc.effect.layers = std::move(layers);
            }
            else
            {
                warnings.push_back(
                    "effect look '" + exdoc.effect.preset +
                    "' did not resolve — the bundle keeps the "
                    "named reference only");
            }
        }
        /* ExportBundle writes the portable directory —
           studio.json LAST, the transitively-referenced types
           alongside, assets copied from <root>/assets — into a
           scratch dir under the managed root, then the whole
           tree packs into the wire `files` map. */
        const fs::path edir =
            (fs::path(store_.Root()) / ".export-stage")
                .lexically_normal();
        std::error_code ec;
        fs::remove_all(edir, ec);
        fs::create_directories(edir, ec);
        std::vector<std::string> errors;
        unsigned int types_written = 0, assets_copied = 0;
        const bool ok =
            !ec &&
            studio::ExportBundle(edir.string(), exdoc,
                                 cand->device_registry,
                                 store_.LibraryDevicesDir(),
                                 &errors, &warnings,
                                 /*overwrite=*/false,
                                 &types_written, &assets_copied);
        nlohmann::json files = nlohmann::json::object();
        if(ok && !PackBundleDir(edir, files, errors))
        {
            errors.push_back("bundle pack failed");
        }
        {
            std::error_code x;
            fs::remove_all(edir, x);
        }
        if(!ok || !errors.empty())
        {
            return fail(errors.empty()
                            ? "export failed"
                            : errors.front(),
                        "internal");
        }
        const nlohmann::json bundle = nlohmann::json::object({
            { "format",  "openrgb2-bundle-files" },
            { "version", 1                       },
            { "files",   std::move(files)        },
        });
        const std::string bytes = bundle.dump(2);
        if(!surface.stage_download)
        {
            return fail("download staging is not wired",
                        "internal");
        }
        std::string token, sha, err;
        if(!surface.stage_download(job.st.session_id, "export",
                                   bytes, token, sha, err))
        {
            return fail("export staging failed: " + err,
                        "internal");
        }
        st.state = JobState::Succeeded;
        st.result = nlohmann::json::object({
            { "transfer_id",    token                  },
            { "bytes",          (uint64_t)bytes.size() },
            { "sha256",         sha                    },
            { "types_written",  types_written          },
            { "assets_copied",  assets_copied          },
            { "warnings",       warnings               },
        });
        CapReportArray(st.result, "warnings");
        return st;
    }

    if(verb == "reload")
    {
        std::string id = body.value("id", std::string());
        if(id.empty())
        {
            std::lock_guard<std::mutex> g(mutex_);
            id = current_.id;
        }
        if(id.empty())
        {
            return fail("no managed workspace is active",
                        "bad_request");
        }
        /* Task 10: when the caller passes its last-seen
           revisions, a reload issued against state the engine
           already moved past refuses honestly rather than
           silently adopting — the explicit, revision-checked
           re-read the brief requires. */
        if(body.contains("base_revision"))
        {
            uint64_t base_rev = 0, res_rev = 0;
            JsonU64(body, "base_revision",     base_rev);
            JsonU64(body, "resource_revision", res_rev);
            const WorkspaceStore::Revisions rev =
                store_.LoadRevisions(id);
            if(rev.exists && !rev.error.empty())
            {
                return fail("workspace marker unreadable: " +
                            rev.error, "marker_unreadable");
            }
            bool recovered_base = false;
            {
                std::lock_guard<std::mutex> g(mutex_);
                recovered_base =
                    current_.id == id && current_.recovered &&
                    base_rev == current_.workspace_revision &&
                    res_rev  == current_.resource_revision;
            }
            if(!recovered_base &&
               (rev.workspace_revision != base_rev ||
                rev.resource_revision  != res_rev))
            {
                return fail("stale revision for '" + id + "'",
                            "revision_conflict");
            }
        }
        Result res = Open(id);
        if(res.ok && deps_.apply_scene)
        {
            std::shared_ptr<const WorkspaceCandidate> cand;
            {
                std::lock_guard<std::mutex> g(mutex_);
                cand = current_.candidate;
            }
            std::string aerr;
            if(cand && cand->resolved &&
               deps_.apply_scene(cand, aerr))
            {
                res.applied = true;
            }
        }
        return done(res);
    }

    if(verb == "restore_backup")
    {
        /* Task 10 Restore Backup — the marker backup's
           generation commits forward as a NEW generation,
           revision-gated at commit time. An explicit user
           action, so the restored scene applies. */
        std::string id = body.value("id", std::string());
        if(id.empty())
        {
            std::lock_guard<std::mutex> g(mutex_);
            id = current_.id;
        }
        if(id.empty())
        {
            return fail("no managed workspace is active",
                        "bad_request");
        }
        uint64_t base_rev = 0, res_rev = 0;
        JsonU64(body, "base_revision",     base_rev);
        JsonU64(body, "resource_revision", res_rev);
        return done(RestoreBackup(id, base_rev, res_rev));
    }

    return fail("unknown job verb '" + verb + "'", "bad_request");
}

WorkspaceService::Result WorkspaceService::CommitJson(
    const std::string& id, const nlohmann::json& document,
    const std::string& original_bytes,
    uint64_t expected_workspace_revision,
    uint64_t expected_resource_revision,
    bool apply_after, bool adopt)
{
    Result res;
    std::shared_ptr<const WorkspaceCandidate> apply_cand;
    {
        std::lock_guard<std::mutex> g(mutex_);
        if(!WorkspaceStore::IsWorkspaceId(id))
        {
            res.error  = "invalid workspace id '" + id + "'";
            res.detail = "bad_request";
            return res;
        }
        /* Authoring + resource revisions re-checked against the
           durable marker at COMMIT time — a base accepted at
           enqueue must lose when an intervening commit landed. */
        const WorkspaceStore::Revisions rev =
            store_.LoadRevisions(id);
        if(rev.exists && !rev.error.empty())
        {
            res.error  = "workspace marker unreadable: " +
                         rev.error;
            res.detail = "marker_unreadable";
            return res;
        }
        /* After a generation recovery the marker still names
           the corrupt generation — its revisions legitimately
           differ from the PUBLISHED ones the client holds.
           A base matching the live recovered state is the
           owner of that state, not a stale writer; the store
           gate is fed the marker revision by CommitCommon's
           StoreExpectedRevs translation. */
        const bool recovered_base =
            current_.id == id && current_.recovered &&
            expected_workspace_revision ==
                current_.workspace_revision &&
            expected_resource_revision ==
                current_.resource_revision;
        if(!recovered_base &&
           (rev.workspace_revision != expected_workspace_revision ||
            rev.resource_revision  != expected_resource_revision))
        {
            res.error = "stale revision: expected {" +
                std::to_string(expected_workspace_revision) + "," +
                std::to_string(expected_resource_revision) +
                "} but '" + id + "' is at {" +
                std::to_string(rev.workspace_revision) + "," +
                std::to_string(rev.resource_revision) + "}";
            res.detail = "revision_conflict";
            return res;
        }
        WorkspaceCandidate cand;
        std::vector<std::string> errors;
        if(!ParseWorkspaceCandidate(document,
                                store_.EffectiveDeviceRegistry(),
                                store_.EffectiveEffectRegistry(),
                                cand, errors, res.warnings))
        {
            for(const std::string& e : errors)
            {
                if(!res.error.empty())
                {
                    res.error += "; ";
                }
                res.error += e;
            }
            if(res.error.empty())
            {
                res.error = "document failed to parse as a "
                            "workspace candidate";
            }
            res.detail = "invalid_document";
            return res;
        }
        if(adopt)
        {
            res = CommitCommon(id, cand, original_bytes,
                               expected_workspace_revision,
                               apply_after, &apply_cand,
                               /*prior_warnings=*/res.warnings);
        }
        else
        {
            /* save_copy — commit under the OTHER id without
               adopting: the current workspace is untouched.
               The store gate still needs the marker revision
               when the target is the recovered current. */
            const WorkspaceStore::CommitResult cr =
                store_.Commit(id, cand, original_bytes,
                              StoreExpectedRevs(
                                  id, expected_workspace_revision,
                                  0).first);
            /* Append — res.warnings already holds the parse
               warnings accumulated above. */
            res.warnings.insert(res.warnings.end(),
                                cr.warnings.begin(),
                                cr.warnings.end());
            if(!cr.ok)
            {
                res.error = cr.error;
                if(cr.error.find("revision") != std::string::npos ||
                   cr.error.find("stale")    != std::string::npos)
                {
                    res.detail = "revision_conflict";
                }
            }
            else
            {
                res.ok                 = true;
                res.committed          = true;
                res.generation         = cr.generation;
                res.workspace_revision = cr.workspace_revision;
                res.resource_revision  = cr.resource_revision;
            }
        }
    }
    /* The scene publish runs AFTER the commit lock releases —
       the emitted event's payload re-reads this service (the
       managed flag), so applying under the lock is a same-
       thread self-deadlock. */
    ApplyAfterCommit(apply_cand, res.generation, res);
    return res;
}

WorkspaceService::Result WorkspaceService::ImportStagedJson(
    const std::string& id, const nlohmann::json& staged,
    const std::string& original_bytes,
    const std::map<std::string, std::string>& choices,
    const std::string& mode,
    uint64_t expected_workspace_revision,
    uint64_t expected_resource_revision)
{
    std::lock_guard<std::mutex> g(mutex_);
    Result res;
    if(!WorkspaceStore::IsWorkspaceId(id))
    {
        res.error  = "invalid workspace id '" + id + "'";
        res.detail = "bad_request";
        return res;
    }
    const WorkspaceStore::Revisions rev = store_.LoadRevisions(id);
    if(rev.exists && !rev.error.empty())
    {
        res.error  = "workspace marker unreadable: " + rev.error;
        res.detail = "marker_unreadable";
        return res;
    }
    /* Same recovery translation as CommitJson: a client
       holding the published (recovered) revisions owns the
       live state even though the marker still names the
       corrupt generation. */
    const bool recovered_base =
        current_.id == id && current_.recovered &&
        expected_workspace_revision ==
            current_.workspace_revision &&
        expected_resource_revision ==
            current_.resource_revision;
    if(!recovered_base &&
       (rev.workspace_revision != expected_workspace_revision ||
        rev.resource_revision  != expected_resource_revision))
    {
        res.error  = "stale revision for '" + id + "'";
        res.detail = "revision_conflict";
        return res;
    }
    /* Task 10 edge: types-only installs land in a managed
       workspace's library — with nothing managed under the
       id the per-type install used to die on the cryptic
       "no readable marker" marker gate. Refuse up front. */
    if(mode == "preset_import" && !rev.exists)
    {
        res.error  = "'preset_import' installs bundle types "
                     "into a managed workspace's library — '" +
                     id + "' has no managed workspace";
        res.detail = "bad_request";
        return res;
    }

    /*------------------------------------------------------*\
    || Packed bundle (studio-import Task 9): the staged      |
    || `files` map unpacks into <root>/.import-stage and     |
    || the whole bundle flow — InspectBundle, explicit       |
    || conflict choices, ApplyImport's cascaded remap — runs |
    || INSIDE staging (target <stage>/out/...), so the real  |
    || library is never touched until the store's own commit |
    || machinery installs the staged types/assets through    |
    || the candidate's extracted-resource path.              |
    \*------------------------------------------------------*/
    const bool packed = staged.is_object() &&
                        staged.contains("files") &&
                        staged["files"].is_object();
    if(packed)
    {
        const fs::path sdir =
            (fs::path(store_.Root()) / ".import-stage")
                .lexically_normal();
        const fs::path sin  = sdir / "in";
        const fs::path sout = sdir / "out";
        std::error_code ec;
        fs::remove_all(sdir, ec);
        fs::create_directories(sin, ec);
        auto wipe = [&]
        {
            std::error_code x;
            fs::remove_all(sdir, x);
        };
        std::vector<std::string> errors;
        if(ec || !UnpackBundleFiles(staged["files"], sin, errors))
        {
            wipe();
            res.error = errors.empty()
                ? "cannot stage the bundle: " + ec.message()
                : errors.front();
            res.detail = "invalid_document";
            for(size_t i = 1; i < errors.size(); ++i)
            {
                res.warnings.push_back(errors[i]);
            }
            return res;
        }
        const bool preset_only = mode == "preset_import";
        /* Types-only bundles carry no studio.json — a
           synthesized empty document satisfies the inspect
           contract; its document commit is skipped below. */
        if(preset_only && !fs::exists(sin / "studio.json", ec))
        {
            std::ofstream ef(sin / "studio.json",
                             std::ios::binary | std::ios::trunc);
            ef << studio::ToJson(studio::StudioDocument()).dump(2)
               << "\n";
            if(!ef)
            {
                wipe();
                res.error  = "cannot stage the synthesized "
                             "studio.json";
                res.detail = "internal";
                return res;
            }
        }
        studio::ImportPlan plan;
        if(!studio::InspectBundle(
               sin.string(), store_.EffectiveDeviceRegistry(),
               plan, &errors))
        {
            wipe();
            res.error = errors.empty()
                ? "bundle inspection failed"
                : errors.front();
            res.detail = "invalid_document";
            for(size_t i = 1; i < errors.size(); ++i)
            {
                res.warnings.push_back(errors[i]);
            }
            return res;
        }
        res.warnings.insert(res.warnings.end(),
                            plan.warnings.begin(),
                            plan.warnings.end());
        /* Every same-id Conflict needs an explicit choice —
           ApplyImport enforces this again; failing early gives
           the cleaner detail. */
        for(const studio::BundleType& t : plan.types)
        {
            if(t.status == studio::BundleType::Status::Conflict &&
               !choices.count(t.id))
            {
                wipe();
                res.error  = "conflicting type '" + t.id +
                             "' requires an explicit import id";
                res.detail = "conflict_choice_required";
                return res;
            }
        }
        /* Apply inside staging: types land under
           out/library/devices, assets under out/assets — the
           real library stays untouched. */
        const fs::path stage_lib = sout / "library" / "devices";
        studio::StudioDocument out_doc;
        if(!studio::ApplyImport(plan, choices, stage_lib.string(),
                                out_doc, &errors, &res.warnings))
        {
            wipe();
            res.error = errors.empty()
                ? "bundle apply failed"
                : errors.front();
            res.detail = "invalid_document";
            return res;
        }
        /* Harvest what ApplyImport produced inside staging. */
        std::vector<studio::DevicePreset> staged_types;
        {
            std::error_code tec;
            if(fs::is_directory(stage_lib, tec))
            {
                for(const auto& ent :
                        fs::directory_iterator(stage_lib, tec))
                {
                    studio::DevicePreset p;
                    if(ent.path().extension() == ".json" &&
                       studio::DevicePresetFromJsonFile(
                           ent.path().string(), p, nullptr))
                    {
                        staged_types.push_back(p);
                    }
                }
            }
        }
        std::vector<std::pair<std::string, std::string>>
            staged_assets;
        {
            const fs::path adir = sout / "assets";
            std::error_code aec;
            if(fs::is_directory(adir, aec))
            {
                for(fs::recursive_directory_iterator it(adir, aec),
                        end;
                    !aec && it != end; it.increment(aec))
                {
                    if(!it->is_regular_file(aec))
                    {
                        continue;
                    }
                    const std::string rel =
                        fs::relative(it->path(), adir, aec)
                            .generic_string();
                    staged_assets.emplace_back(
                        rel, it->path().string());
                }
            }
        }

        if(preset_only)
        {
            /* Library-only install: land each staged type via
               the single-resource path in dependency order —
               the active document is untouched. Each install
               re-checks revisions; the resource revision moves
               under us. */
            uint64_t ws_rev = expected_workspace_revision;
            uint64_t rs_rev = expected_resource_revision;
            std::vector<studio::DevicePreset> todo =
                staged_types;
            bool grew = true;
            while(grew && !todo.empty())
            {
                grew = false;
                for(auto it = todo.begin(); it != todo.end();)
                {
                    bool ready = true;
                    const studio::PresetRegistry eff =
                        store_.EffectiveDeviceRegistry();
                    for(const std::string& dep :
                            studio::PresetDependencies(*it))
                    {
                        ready &= eff.Contains(dep);
                    }
                    if(!ready)
                    {
                        ++it;
                        continue;
                    }
                    const WorkspaceStore::ResourceInstallResult ir =
                        store_.InstallDeviceTypeResource(
                            id, *it, /*allow_replace=*/false,
                            ws_rev, rs_rev);
                    if(!ir.ok)
                    {
                        wipe();
                        res.error  = ir.error;
                        res.detail =
                            ir.error.find("stale") !=
                                    std::string::npos
                                ? "revision_conflict"
                                : "resource_install";
                        return res;
                    }
                    ws_rev = ir.workspace_revision;
                    rs_rev = ir.resource_revision;
                    res.warnings.insert(res.warnings.end(),
                                        ir.warnings.begin(),
                                        ir.warnings.end());
                    it   = todo.erase(it);
                    grew = true;
                }
            }
            if(!todo.empty())
            {
                wipe();
                res.error  = "bundled types have unresolvable "
                             "dependencies";
                res.detail = "invalid_document";
                return res;
            }
            /* Bundled assets ride along with the same
               never-overwrite discipline — there is no
               document commit to carry extracted_assets. */
            {
                const fs::path dst_root =
                    fs::path(store_.LibraryAssetsDir());
                for(const auto& kv : staged_assets)
                {
                    const fs::path dst = dst_root / fs::path(kv.first);
                    std::error_code cec;
                    if(fs::exists(dst, cec))
                    {
                        continue;   /* local file kept */
                    }
                    fs::create_directories(dst.parent_path(), cec);
                    fs::copy_file(fs::path(kv.second), dst,
                                  fs::copy_options::none, cec);
                    if(cec)
                    {
                        res.warnings.push_back(
                            "asset '" + kv.first +
                            "' copy failed: " + cec.message());
                    }
                }
            }
            res.ok                 = true;
            res.detail             = "preset_import";
            res.workspace_revision = ws_rev;
            res.resource_revision  = rs_rev;
            wipe();
            return res;
        }

        /* Re-resolve the remapped document against the
           effective registry + staged-type overlay, then let
           Commit install the staged types/assets into the real
           library via the extracted-resource machinery. */
        studio::PresetRegistry devreg =
            store_.EffectiveDeviceRegistry();
        {
            std::vector<studio::DevicePreset> todo = staged_types;
            bool grew = true;
            while(grew && !todo.empty())
            {
                grew = false;
                for(auto it = todo.begin(); it != todo.end();)
                {
                    if(devreg.Add(*it, nullptr))
                    {
                        it   = todo.erase(it);
                        grew = true;
                    }
                    else
                    {
                        ++it;
                    }
                }
            }
        }
        WorkspaceCandidate cand;
        if(!ParseWorkspaceCandidate(
               studio::ToJson(out_doc), devreg,
               store_.EffectiveEffectRegistry(),
               cand, errors, res.warnings))
        {
            wipe();
            for(const std::string& e : errors)
            {
                if(!res.error.empty())
                {
                    res.error += "; ";
                }
                res.error += e;
            }
            if(res.error.empty())
            {
                res.error = "applied bundle failed to re-resolve";
            }
            res.detail = "invalid_document";
            return res;
        }
        for(const studio::DevicePreset& p : staged_types)
        {
            cand.extracted_types.push_back(p);
        }
        cand.extracted_assets = std::move(staged_assets);
        SanitizeImportedWorkspace(cand.document);
        if(cand.resolved)
        {
            auto rs = std::make_shared<studio::SceneDocument>(
                *cand.resolved);
            SanitizeImportedScene(*rs);
            cand.resolved = std::move(rs);
        }
        res.warnings.push_back(
            "import sanitized: output/inputs/ownership unarmed; "
            "identity hints kept for review");
        wipe();
        /* The generation's retained original is the remapped
           workspace document, not the (up to 64 MiB) packed
           wire form. */
        const std::string retained =
            studio::ToJson(cand.document).dump(2);
        return CommitCommon(id, cand, retained,
                            expected_workspace_revision,
                            /*apply_after=*/false, nullptr,
                            /*prior_warnings=*/res.warnings);
    }

    /* Flat bundle form: {format, workspace:{...},
       resources:{devices, effects}} — a bare compact document
       is equally valid. */
    nlohmann::json doc = staged;
    std::map<std::string, studio::DevicePreset>   bundled_dev;
    std::map<std::string, studio::EffectDocument> bundled_fx;
    studio::PresetRegistry devreg = store_.EffectiveDeviceRegistry();
    studio::EffectRegistry effreg = store_.EffectiveEffectRegistry();
    const bool bundle = staged.is_object() &&
                        staged.contains("workspace") &&
                        staged["workspace"].is_object();
    if(bundle)
    {
        doc = staged["workspace"];
        const nlohmann::json resj =
            staged.value("resources", nlohmann::json::object());
        for(const nlohmann::json& pj :
                resj.value("devices", nlohmann::json::array()))
        {
            studio::DevicePreset p;
            std::vector<std::string> perrs;
            if(!studio::DevicePresetFromJson(pj, p, &perrs))
            {
                res.error  = "bundled device preset invalid: " +
                             (perrs.empty() ? "?" : perrs.front());
                res.detail = "invalid_document";
                return res;
            }
            bundled_dev[p.id] = p;
        }
        for(const nlohmann::json& ej :
                resj.value("effects", nlohmann::json::array()))
        {
            studio::EffectDocument d;
            std::vector<std::string> eerrs;
            if(!studio::EffectDocumentFromJson(ej, d, &eerrs))
            {
                res.error  = "bundled effect look invalid: " +
                             (eerrs.empty() ? "?" : eerrs.front());
                res.detail = "invalid_document";
                return res;
            }
            bundled_fx[d.id] = d;
        }
        /* Scratch overlay — bundled types resolve inter-type
           refs the same way the file layer does. */
        std::vector<studio::DevicePreset> todo;
        for(const auto& kv : bundled_dev)
        {
            todo.push_back(kv.second);
        }
        bool grew = true;
        while(grew && !todo.empty())
        {
            grew = false;
            for(auto it = todo.begin(); it != todo.end();)
            {
                std::vector<std::string> aerrs;
                if(devreg.Add(*it, &aerrs))
                {
                    it   = todo.erase(it);
                    grew = true;
                }
                else
                {
                    ++it;
                }
            }
        }
        if(!todo.empty())
        {
            res.error  = "bundled device presets have "
                         "unresolvable dependencies";
            res.detail = "invalid_document";
            return res;
        }
        for(const auto& kv : bundled_fx)
        {
            effreg.Add(kv.second, nullptr);
        }
    }

    WorkspaceCandidate cand;
    std::vector<std::string> errors;
    if(!ParseWorkspaceCandidate(doc, devreg, effreg, cand,
                                errors, res.warnings))
    {
        for(const std::string& e : errors)
        {
            if(!res.error.empty())
            {
                res.error += "; ";
            }
            res.error += e;
        }
        if(res.error.empty())
        {
            res.error = "staged content failed to parse as a "
                        "workspace candidate";
        }
        res.detail = "invalid_document";
        return res;
    }

    /* Bundled types/looks the document actually uses become
       extracted resources — the commit installs them with
       collision-aware rename + reference remap. */
    {
        std::set<std::string> have;
        for(const studio::DevicePreset& p : cand.extracted_types)
        {
            have.insert(p.id);
        }
        std::vector<std::string> used, uerrs;
        studio::CollectUsedTypes(cand.document, cand.device_registry,
                                 used, &uerrs);
        for(const std::string& tid : used)
        {
            if(have.count(tid) || !bundled_dev.count(tid))
            {
                continue;
            }
            cand.extracted_types.push_back(bundled_dev[tid]);
            have.insert(tid);
        }
        if(bundled_fx.count(cand.document.effect.preset))
        {
            bool have_fx = false;
            for(const studio::EffectDocument& d :
                    cand.extracted_effects)
            {
                have_fx |= d.id == cand.document.effect.preset;
            }
            if(!have_fx)
            {
                cand.extracted_effects.push_back(
                    bundled_fx[cand.document.effect.preset]);
            }
        }
    }

    SanitizeImportedWorkspace(cand.document);
    if(cand.resolved)
    {
        auto rs = std::make_shared<studio::SceneDocument>(
            *cand.resolved);
        SanitizeImportedScene(*rs);
        cand.resolved = std::move(rs);
    }
    res.warnings.push_back(
        "import sanitized: output/inputs/ownership unarmed; "
        "identity hints kept for review");

    /* Imports NEVER auto-apply — the committed generation waits
       for an explicit open/apply decision. */
    return CommitCommon(id, cand, original_bytes,
                        expected_workspace_revision,
                        /*apply_after=*/false, nullptr,
                        /*prior_warnings=*/res.warnings);
}

nlohmann::json WorkspaceService::InspectPackedBundle(
    const nlohmann::json& bundle_doc,
    std::vector<std::string>& errors,
    std::vector<std::string>& warnings)
{
    nlohmann::json out = nlohmann::json::object({
        { "valid",         false                   },
        { "types",         nlohmann::json::array() },
        { "conflicts",     nlohmann::json::array() },
        { "assets",        nlohmann::json::array() },
        { "warnings",      nlohmann::json::array() },
        { "errors",        nlohmann::json::array() },
        { "has_workspace", true                    },
    });
    /* Scratch dir inside the managed root — nothing escapes
       it; a crash orphan is removed on the next run. */
    const fs::path sdir =
        (fs::path(store_.Root()) / ".inspect-stage")
            .lexically_normal();
    std::error_code ec;
    fs::remove_all(sdir, ec);
    fs::create_directories(sdir, ec);
    auto wipe = [&]
    {
        std::error_code x;
        fs::remove_all(sdir, x);
    };
    if(ec ||
       !UnpackBundleFiles(bundle_doc["files"], sdir, errors))
    {
        wipe();
        if(errors.empty())
        {
            errors.push_back("cannot stage the bundle: " +
                             ec.message());
        }
        out["errors"] = errors;
        return out;
    }
    /* Types-only bundles carry no studio.json — synthesize the
       empty document so InspectBundle's plan parses (the
       preset_import mode commits no document anyway). The
       report flags the missing workspace document so the
       client picks a mode commit can actually satisfy —
       `has_workspace:false` means only preset_import lands. */
    const bool has_workspace =
        fs::exists(sdir / "studio.json", ec);
    if(!has_workspace)
    {
        std::ofstream ef(sdir / "studio.json",
                         std::ios::binary | std::ios::trunc);
        ef << studio::ToJson(studio::StudioDocument()).dump(2)
           << "\n";
    }

    studio::ImportPlan plan;
    if(!studio::InspectBundle(
           sdir.string(), store_.EffectiveDeviceRegistry(),
           plan, &errors))
    {
        wipe();
        out["errors"] = errors;
        return out;
    }
    wipe();

    /* Suggested import ids for every conflict — free against
       the local library AND the other bundled ids. */
    std::set<std::string> taken(plan.local_ids.begin(),
                                plan.local_ids.end());
    for(const studio::BundleType& t : plan.types)
    {
        taken.insert(t.id);
    }
    nlohmann::json types = nlohmann::json::array();
    nlohmann::json confl = nlohmann::json::array();
    for(const studio::BundleType& t : plan.types)
    {
        const char* status =
            t.status == studio::BundleType::Status::New
                ? "new"
                : t.status == studio::BundleType::Status::Identical
                      ? "identical" : "conflict";
        types.push_back(nlohmann::json::object({
            { "id",     t.id           },
            { "name",   t.preset.name  },
            { "status", status         },
        }));
        if(t.status == studio::BundleType::Status::Conflict)
        {
            const std::string sug = SuggestImportId(t.id, taken);
            taken.insert(sug);
            confl.push_back(nlohmann::json::object({
                { "id",             t.id           },
                { "local_name",     t.local.name   },
                { "bundled_name",   t.preset.name  },
                { "suggested_id",   sug            },
            }));
        }
    }
    out["types"]      = std::move(types);
    out["conflicts"]  = std::move(confl);
    out["assets"]     = nlohmann::json(plan.assets);
    out["has_workspace"] = has_workspace;
    out["workspace"]  = nlohmann::json::object({
        { "name", has_workspace ? plan.workspace.meta.name
                                : std::string() } });
    warnings.insert(warnings.end(), plan.warnings.begin(),
                    plan.warnings.end());
    out["warnings"]   = warnings;
    out["valid"]      = errors.empty();
    out["errors"]     = errors;
    return out;
}

nlohmann::json WorkspaceService::InspectCandidateJson(
    const nlohmann::json& document_or_bundle,
    const studio::PresetRegistry& devices,
    const studio::EffectRegistry& effects,
    std::vector<std::string>& errors,
    std::vector<std::string>& warnings)
{
    nlohmann::json out = nlohmann::json::object({
        { "valid",     false                 },
        { "conflicts", nlohmann::json::array() },
        { "missing",   nlohmann::json::array() },
        { "warnings",  nlohmann::json::array() },
        { "errors",    nlohmann::json::array() },
    });

    nlohmann::json doc = document_or_bundle;
    studio::PresetRegistry devreg = devices;
    studio::EffectRegistry effreg = effects;
    const bool bundle = document_or_bundle.is_object() &&
                        document_or_bundle.contains("workspace") &&
                        document_or_bundle["workspace"].is_object();
    if(bundle)
    {
        doc = document_or_bundle["workspace"];
        const nlohmann::json resj =
            document_or_bundle.value("resources",
                                     nlohmann::json::object());
        for(const nlohmann::json& pj :
                resj.value("devices", nlohmann::json::array()))
        {
            studio::DevicePreset p;
            std::vector<std::string> perrs;
            if(!studio::DevicePresetFromJson(pj, p, &perrs))
            {
                errors.push_back("bundled device preset invalid: " +
                                 (perrs.empty() ? "?" : perrs.front()));
                continue;
            }
            /* new | identical | conflict — the inspect contract
               the client resolves BEFORE committing. */
            const studio::DevicePreset* local = devreg.Find(p.id);
            std::string status = "new";
            if(local)
            {
                status = studio::ToJson(*local) == studio::ToJson(p)
                             ? "identical" : "conflict";
            }
            out["conflicts"].push_back(nlohmann::json::object({
                { "id",     p.id   },
                { "status", status },
            }));
            devreg.Add(p, nullptr);
        }
        for(const nlohmann::json& ej :
                resj.value("effects", nlohmann::json::array()))
        {
            studio::EffectDocument d;
            if(studio::EffectDocumentFromJson(ej, d, nullptr))
            {
                effreg.Add(d, nullptr);
            }
        }
    }

    WorkspaceCandidate cand;
    std::vector<std::string> perrs;
    if(!ParseWorkspaceCandidate(doc, devreg, effreg, cand,
                                perrs, warnings))
    {
        for(const std::string& e : perrs)
        {
            errors.push_back(e);
        }
    }
    else
    {
        out["valid"] = true;
        out["name"]  = cand.document.meta.name;
        /* types the document uses but no layer resolves */
        std::vector<std::string> used, uerrs;
        studio::CollectUsedTypes(cand.document, cand.device_registry,
                                 used, &uerrs);
        for(const std::string& e : uerrs)
        {
            out["missing"].push_back(e);
        }
    }
    out["errors"]   = errors;
    out["warnings"] = warnings;
    /* Every array field is capped — the report rides the 1 MiB
       job-result envelope; a pathological bundle truncates with
       an honest dropped count, never an oversized reply. */
    for(const char* key :
            { "conflicts", "missing", "warnings", "errors" })
    {
        CapReportArray(out, key);
    }
    return out;
}

bool WorkspaceService::DocumentBytes(const std::string& id,
                                     std::string& bytes,
                                     std::string& err) const
{
    /* Raw document.json — byte-identical to re-serializing a
       loaded candidate, but a marker + single file read rather
       than a full manifest-verify + registry-overlay + parse.
       Reader-thread metadata stays bounded and cheap; the IPC
       layer caches per revision on top of this. */
    return store_.ReadActiveDocumentBytes(id, bytes, err);
}

/*---------------------------------------------------------*\
||| Job serialization (the wire shares one shape)             ||
\*---------------------------------------------------------*/

const char* JobStateName(WorkspaceService::JobState s)
{
    switch(s)
    {
    case WorkspaceService::JobState::Queued:    return "queued";
    case WorkspaceService::JobState::Running:   return "running";
    case WorkspaceService::JobState::Succeeded: return "succeeded";
    case WorkspaceService::JobState::Failed:    return "failed";
    case WorkspaceService::JobState::Cancelled: return "cancelled";
    }
    return "?";
}

nlohmann::json JobStatusToJson(const WorkspaceService::JobStatus& st)
{
    nlohmann::json out = nlohmann::json::object({
        { "found",              st.found                  },
        { "job_id",             st.job_id                 },
        { "session_id",         st.session_id             },
        { "verb",               st.verb                   },
        { "state",              JobStateName(st.state)    },
        { "committed",          st.committed              },
        { "applied",            st.applied                },
        { "recovered",          st.recovered              },
        { "generation",         st.generation             },
        { "workspace_revision", st.workspace_revision     },
        { "resource_revision",  st.resource_revision      },
        { "result",             st.result                 },
    });
    if(!st.error.empty())
    {
        out["error"] = st.error;
    }
    if(!st.detail.empty())
    {
        out["detail"] = st.detail;
    }
    return out;
}

} /* namespace openrgb2 */
