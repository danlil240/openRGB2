/*---------------------------------------------------------*\
|||| WorkspaceService.h                                      ||
||||                                                           |
||||   The engine-internal open/save/import surface over   ||
||||   the managed workspace store (studio-import Task 2,  ||
||||   plan section 3.2). THE ONLY COMPONENT that commits  ||
||||   accepted generations — every workspace-changing     ||
||||   operation funnels through here so revision checks,  ||
||||   sanitizing, and the serialized commit boundary are  ||
||||   impossible to skip.                                 ||
||||                                                           |
||||   Surface:                                             ||
||||   - Open(id):   load the workspace's active           ||
||||     generation and make it current; recovers the      ||
||||     last-valid generation honestly (a recovery is an  ||
||||     ok result with the failure reported in warnings)  ||
||||   - Save(id, doc, expected_rev):  author/save — the   ||
||||     document is committed verbatim (its authored      ||
||||     arming/verified state is the user's own), then    ||
||||     the resolved scene is published through the       ||
||||     injected apply_scene seam                         ||
||||   - Import(id, doc, expected_rev): untrusted-source   ||
||||     path — the candidate is sanitized FIRST (no      ||
||||     live-on-startup, no input capture, no verified    ||
||||     ownership), committed, and NEVER auto-applied     ||
||||   - ImportFile(id, path, expected_rev): Import with   ||
||||     the file's original bytes retained verbatim in    ||
||||     the generation, plus sibling presets/devices and  ||
||||     presets/effects directories consulted as scratch  ||
||||     registries (and installed into the managed        ||
||||     library when the workspace actually references    ||
||||     them)                                             ||
||||                                                           |
||||   All operations are revision-checked against the     ||
||||   durable marker — a stale writer is refused before   ||
||||   anything is staged. Everything is serialized on a   ||
||||   mutex; the store's crash contract does the rest.    ||
||||                                                           |
||||   SPDX-License-Identifier: GPL-2.0-or-later               ||
\*---------------------------------------------------------*/

#pragma once

#include "WorkspaceStore.h"

#include "config/WorkspaceCandidate.h"
#include "scene/SceneTypes.h"

#include <nlohmann/json.hpp>

#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace openrgb2
{

class WorkspaceService
{
public:
    struct Deps
    {
        /* Base libraries the managed root overlays. */
        studio::PresetRegistry devices;
        studio::EffectRegistry effects;
        /* Post-commit publish seam — nullptr is legal (the save
           still commits; `applied` reports false). Runs on the
           caller's thread AFTER the marker moved, holding the
           service mutex; a failure is reported honestly — the
           committed generation stays durable. */
        std::function<bool(
            std::shared_ptr<const studio::SceneDocument>,
            std::string&)> apply_scene;
    };

    WorkspaceService(std::string root, Deps deps);

    struct Result
    {
        bool        ok                 = false;
        bool        committed          = false; /* generation landed */
        bool        applied            = false; /* resolved published */
        bool        recovered          = false; /* LKG path taken     */
        uint64_t    generation         = 0;
        uint64_t    workspace_revision = 0;
        uint64_t    resource_revision  = 0;
        std::string error;
        std::vector<std::string> warnings;
    };

    Result Open(const std::string& id);
    Result Save(const std::string& id,
                const nlohmann::json& document,
                uint64_t expected_workspace_revision);
    Result Import(const std::string& id,
                  const nlohmann::json& document,
                  uint64_t expected_workspace_revision);
    Result ImportFile(const std::string& id,
                      const std::string& path,
                      uint64_t expected_workspace_revision);

    /* Current-workspace introspection (empty until an
       Open/Save/Import adopts one). */
    const WorkspaceStore& Store() const { return store_; }
    std::shared_ptr<const WorkspaceCandidate> CurrentCandidate() const;
    std::string  CurrentId()          const;
    uint64_t     WorkspaceRevision()  const;
    uint64_t     ResourceRevision()   const;

private:
    /* The serialized commit boundary — every mutation runs
       under this mutex. */
    struct Current
    {
        std::string                               id;
        std::shared_ptr<const WorkspaceCandidate> candidate;
        uint64_t workspace_revision = 0;
        uint64_t resource_revision  = 0;
    };
    Result CommitCommon(const std::string& id,
                        WorkspaceCandidate& cand,
                        const std::string& original_bytes,
                        uint64_t expected_rev,
                        bool apply_after);

    WorkspaceStore  store_;
    Deps            deps_;
    mutable std::mutex mutex_;
    Current            current_;
};

} /* namespace openrgb2 */
