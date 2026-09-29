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

    /* Managed workspace ids (dirs with a marker or generations —
       the shared "library" dir is never listed). */
    std::vector<std::string> List() const;

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
       On failure the previous generation stays active and `error`
       says what happened. */
    CommitResult Commit(const std::string& id,
                        const WorkspaceCandidate& candidate,
                        const std::string& original_bytes,
                        uint64_t expected_workspace_revision);

private:
    std::string root_;
    Deps        deps_;
};

} /* namespace openrgb2 */
