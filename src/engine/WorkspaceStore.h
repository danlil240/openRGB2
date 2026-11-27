/*---------------------------------------------------------*\
|||| WorkspaceStore.h                                        ||
||||                                                           |
||||   The managed workspace root (studio-import Task 2,   ||
||||   plan section 3.2): a Qt-free, OpenRGB2-owned        ||
||||   directory tree holding every managed workspace as   ||
||||   a sequence of IMMUTABLE generations plus an         ||
||||   atomically-replaced active-generation marker.       ||
||||                                                           |
||||   Layout:                                             ||
||||     <root>/library/devices/*.device.json   managed    ||
||||     <root>/library/effects/*.effect.json   libraries  ||
||||     <root>/assets/<rel>                    shared       ||
||||                               bundled asset payloads  ||
||||                               (Task 9 — never         ||
||||                               overwritten)            ||
||||     <root>/<id>/active        marker: which           ||
||||                               generation is live +    ||
||||                               the workspace/resource  ||
||||                               revision counters       ||
||||     <root>/<id>/active.lkg    last-valid marker       ||
||||                               backup                  ||
||||     <root>/<id>/active.rejected  a corrupt marker is  ||
||||                               set aside, never        ||
||||                               destroyed               ||
||||     <root>/<id>/staging/      generation assembly —   ||
||||                               orphan staging is       ||
||||                               ignored and cleaned     ||
||||                               INSIDE the verified     ||
||||                               managed root            ||
||||     <root>/<id>/generations/gen-NNNNNN/               ||
||||         document.json         compact v3 workspace    ||
||||         manifest.json         resource manifest:      ||
||||                               content hashes for the  ||
||||                               document, every         ||
||||                               snapshotted resource,   ||
||||                               and the retained        ||
||||                               original                ||
||||         resources/devices/    referenced device-type  │
||||         resources/effects/    look snapshots          │
||||         original.json         verbatim imported       ||
||||                               bytes (when imported)   ||
||||                                                           |
||||   Commit order (the crash contract):                  ||
||||     1. install extracted types/looks into the managed ||
||||        library with collision-aware rename (a local   ||
||||        file is NEVER overwritten — identical content  ||
||||        is reused, different content lands suffixed    ||
||||        with every reference remapped)                 ||
||||     2. assemble the whole generation under staging/   ||
||||        (resources, document, original, manifest LAST) ||
||||     3. rename staging -> generations/gen-NNNNNN       ||
||||     4. back the current marker up to active.lkg (or   ||
||||        set it aside as .rejected when unreadable)     ||
||||     5. atomically replace active via temp +           ||
||||        MoveFileEx write-through rename                ||
||||   No fallible I/O runs after the marker replacement.  ||
||||   A crash anywhere earlier leaves either the old or   ||
||||   the new generation — never a torn workspace — and   ||
||||   Load recovers the last valid generation through     ||
||||   the marker backup.                                  ||
||||                                                           |
||||   SPDX-License-Identifier: GPL-2.0-or-later               ||
\*---------------------------------------------------------*/

#pragma once

#include "config/WorkspaceCandidate.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace openrgb2
{

class WorkspaceStore
{
public:
    struct Deps
    {
        /* The base libraries generation contents resolve
           against: packaged defaults plus any engine-supplied
           file layers. The managed library and the generation's
           own resource snapshot overlay them in that order. */
        studio::PresetRegistry devices;
        studio::EffectRegistry effects;
    };

    /* Generations retained per workspace beyond the active +
       last-known-good pair — history is bounded so repeated
       saves cannot grow the root without limit. */
    static constexpr unsigned int kMaxGenerations = 8;

    /* Workspace document byte bound — same order as the bundle
       JSON cap; a workspace is one JSON document. */
    static constexpr uint64_t kMaxDocumentBytes = 8ull * 1024 * 1024;

    WorkspaceStore(std::string root, Deps deps);

    /* Development default (plan section 3.2); packaged launchers
       pass a writable app-data root instead. */
    static std::string DefaultRoot();
    static bool        IsWorkspaceId(const std::string& id);

    const std::string& Root()              const { return root_; }
    std::string WorkspaceDir(const std::string& id) const;
    std::string LibraryDevicesDir() const;
    std::string LibraryEffectsDir() const;
    /* Shared bundled-asset pool (<root>/assets/<rel>) — the
       same <workspace>/assets convention the bundle format
       keeps, with the managed root as the workspace. */
    std::string LibraryAssetsDir() const;

    /* Base registries overlaid with the managed library — the
       libraries new candidates parse against. Computed fresh so
       newly installed types/looks are visible immediately. */
    studio::PresetRegistry EffectiveDeviceRegistry() const;
    studio::EffectRegistry EffectiveEffectRegistry() const;

    enum class LoadStatus
    {
        Empty,      /* no managed workspace under this id          */
        Loaded,     /* the marker's generation loaded              */
        Recovered,  /* marker/pointed generation unreadable — the
                       last-valid generation restored; `error`
                       reports the primary failure honestly       */
        Failed,     /* marker and backup both unusable — `error`
                       reports both; candidate is null, never a
                       guessed state                              */
    };
    struct LoadResult
    {
        LoadStatus                               status = LoadStatus::Empty;
        std::shared_ptr<const WorkspaceCandidate> candidate;
        uint64_t            generation         = 0;   /* ordinal */
        uint64_t            workspace_revision = 0;
        uint64_t            resource_revision  = 0;
        std::string         error;
        std::vector<std::string> warnings;
    };
    LoadResult Load(const std::string& id) const;

    /* The durable revision counters for `id` without parsing a
       generation — the IPC commit path re-checks the client's
       base + resource revisions against these at commit time
       (plan 3.3). exists=false with empty error means no
       managed workspace lives under the id (revisions are 0);
       an unreadable marker falls back to the marker backup and
       reports `error` honestly. */
    struct Revisions
    {
        bool        exists             = false;
        uint64_t    workspace_revision = 0;
        uint64_t    resource_revision  = 0;
        std::string error;
    };
    Revisions LoadRevisions(const std::string& id) const;

    /* The ACTIVE generation's document.json bytes verbatim —
       the same bytes Load's candidate re-serializes to
       (ToJson(document).dump(2)), read straight off disk
       WITHOUT the manifest verify / registry overlay /
       candidate parse a full Load performs. The IPC document
       download + content hash use this so a metadata read
       never re-resolves a workspace on a reader thread; the
       marker (primary, backup on a torn primary) selects the
       generation honestly. */
    bool ReadActiveDocumentBytes(const std::string& id,
                                 std::string& bytes,
                                 std::string& err) const;

    /* Managed workspace ids (dirs with a marker or generations —
       the shared "library" dir is never listed). */
    std::vector<std::string> List() const;

    /*---- External-change detection (studio-import Task 10) --*\
    |  ExternalScan is a PASSIVE fingerprint of the active      |
    |  generation's tracked files — the marker pair (contents + |
    |  the generation/revisions they name), the pointed         |
    |  generation's document.json/manifest.json/original.json,  |
    |  and a name:size stamp of its resources/ tree. The        |
    |  service captures `signature` at every adopt/commit and   |
    |  compares later scans against it:                         |
    |    - identical bytes produce the SAME signature — a       |
    |      same-content rewrite never reports a change;         |
    |    - `missing` means a tracked file vanished (the notice  |
    |      reads "deleted"); any other difference reads         |
    |      "modified";                                          |
    |    - `detail` names what moved, for the UI's notice.      |
    |  READ-ONLY — it never cleans orphans, never mutates,      |
    |  never rewrites a marker. watched=false means the id has  |
    |  no managed dir at all (nothing to compare).              |
    \*---------------------------------------------------------*/
    struct ExternalFingerprint
    {
        bool        watched   = false;
        bool        missing   = false;
        std::string signature;
        std::string detail;
    };
    ExternalFingerprint ExternalScan(const std::string& id) const;

    /* Metadata-only companion of ExternalScan (audit I-6): the
       same tracked set described by existence, size and last
       write time — the marker pair is the only thing READ (a
       few bytes, to find the pointed generation); nothing is
       hashed. Equal stamps mean ExternalScan would reproduce
       its last answer, so the 2 s `get` poll rescans only
       when this moves. `racy` is set when any stamped entry
       was written in the last few seconds: a same-size
       rewrite inside one file-time tick cannot be told apart
       by metadata (the racy-git rule), so the caller must
       rescan rather than trust the stamp. */
    struct ExternalStampResult
    {
        std::string stamp;
        bool        racy = true;
    };
    ExternalStampResult ExternalStamp(const std::string& id) const;

    /* The marker BACKUP's generation (active.lkg) — the
       last-known-good load Restore Backup commits forward.
       Empty/Failed statuses carry the honest reason; the
       store itself never repoints the marker here. */
    LoadResult LoadBackup(const std::string& id) const;

    struct CommitResult
    {
        bool        ok                 = false;
        uint64_t    generation         = 0;
        uint64_t    workspace_revision = 0;
        uint64_t    resource_revision  = 0;
        /* The candidate as committed — post-install the document
           may carry renamed type/look references. */
        std::shared_ptr<const WorkspaceCandidate> candidate;
        std::string              error;
        std::vector<std::string> warnings;
    };

    /* Commit `candidate` as a new immutable generation of
       `id` and atomically move the marker.
       - `expected_workspace_revision` must equal the current
         marker's workspace_revision (0 for a new workspace): a
         stale writer is rejected before anything is staged.
       - `original_bytes`, when non-empty, are retained verbatim
         inside the generation (imported-source recovery).
       - The candidate's extracted_types / extracted_effects are
         installed into the managed library first; renames remap
         the committed document's references.
       - extracted_assets (rel -> staged source inside the managed
         root) land under <root>/assets/ — never overwriting;
         byte-equal is a reuse hit, different content keeps the
         local file and warns.
       On failure the previous generation stays active and `error`
       says what happened. */
    CommitResult Commit(const std::string& id,
                        const WorkspaceCandidate& candidate,
                        const std::string& original_bytes,
                        uint64_t expected_workspace_revision);

    /*---- Personal effect looks (studio-import Task 6) ------*\
    |  InstallEffectResource: collision-safe install of ONE   |
    |  validated effect document into the managed effect      |
    |  library — identical existing content is reused,        |
    |  differing content is NEVER overwritten (a suffixed id  |
    |  lands instead). The marker's resource_revision bumps   |
    |  only when a file was actually written; workspace       |
    |  revision and the active generation are untouched.      |
    |  Both expected revisions are checked against the        |
    |  durable marker BEFORE any write — a stale client is    |
    |  rejected ("revision_conflict" discipline), and the     |
    |  resource install is refused entirely when the          |
    |  workspace has no readable marker.                      |
    \*-------------------------------------------------------*/
    struct ResourceInstallResult
    {
        bool        ok                 = false;
        bool        reused             = false; /* identical existed */
        bool        replaced           = false; /* file overwritten
                                                   (editor replace)  */
        std::string landed_id;                  /* final effect id   */
        uint64_t    workspace_revision = 0;
        uint64_t    resource_revision  = 0;
        std::string error;
        std::vector<std::string> warnings;
    };
    ResourceInstallResult InstallEffectResource(
        const std::string& id,
        const studio::EffectDocument& effect,
        uint64_t expected_workspace_revision,
        uint64_t expected_resource_revision);

    /*---- Device types (studio-import Task 9) --------------*\
    |  InstallDeviceTypeResource: one validated device type   |
    |  into the managed library. Same collision discipline as |
    |  the effect install — identical content reuses,         |
    |  differing content lands suffixed — PLUS the editor's   |
    |  explicit `replace` intent: when set, an existing FILE  |
    |  layer entry for the id is rewritten atomically         |
    |  (validate -> temp+rename -> read-back verify); a       |
    |  packaged default is still never touched — replace      |
    |  lands a file override. A "replace" of identical        |
    |  content is a no-op reuse. Without `replace` differing  |
    |  content lands under a suffixed id like an import.      |
    |  `landed_id` reports the final id; `replaced` says an   |
    |  existing file was rewritten.                           |
    \*-------------------------------------------------------*/
    ResourceInstallResult InstallDeviceTypeResource(
        const std::string& id,
        const studio::DevicePreset& preset,
        bool allow_replace,
        uint64_t expected_workspace_revision,
        uint64_t expected_resource_revision);

    /*---- Resource removal (studio-import Task 9) ----------*\
    |  RemoveResource deletes ONE file-layer entry from the   |
    |  managed library — never the packaged defaults.         |
    |  Refusals are honest and named:                         |
    |    - the resource does not exist as a file-layer entry  |
    |      (a packaged default reports "packaged default —    |
    |      nothing to delete");                               |
    |    - `in_use`: the active generation's document of ANY  |
    |      managed workspace references the id (device type   |
    |      in devices.*.type / favorites, or look in          |
    |      effects.preset);                                   |
    |    - `referenced`: another managed type declares the    |
    |      id as a child-entity ref.                          |
    |  Deleting a file that shadows a packaged default        |
    |  REVERTS to it — `reverted_to_default` reports the      |
    |  shadow cleared. A successful remove bumps              |
    |  resource_revision; workspace_revision and the active   |
    |  generation are untouched (the generation's own         |
    |  snapshot still resolves the pinned copy).              |
    \*-------------------------------------------------------*/
    struct ResourceRemoveResult
    {
        bool        ok                 = false;
        bool        removed            = false;
        bool        reverted_to_default = false;
        uint64_t    workspace_revision = 0;
        uint64_t    resource_revision  = 0;
        std::string error;
        std::vector<std::string> warnings;
    };
    /* kind: "device_type" | "effect". */
    ResourceRemoveResult RemoveResource(
        const std::string& id,
        const std::string& kind,
        const std::string& resource_id,
        uint64_t expected_workspace_revision,
        uint64_t expected_resource_revision);

private:
    std::string root_;
    Deps        deps_;
};

} /* namespace openrgb2 */
