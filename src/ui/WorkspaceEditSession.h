/*---------------------------------------------------------*\
|| WorkspaceEditSession.h                                    ||
||                                                           ||
||   THE ONE shared workspace draft (studio-import Task   ||
||   4, plan section 3.1): Lighting, Layout, device/type  ||
||   panels and layer panels all edit the SAME compact    ||
||   StudioDocument through this object instead of        ||
||   staging their own working copies. Draft and the      ||
||   engine's accepted snapshot are separate by design —  ||
||   nothing here reaches the engine; Save is a           ||
||   WorkspaceController concern.                         ||
||                                                           ||
||   Semantics:                                           ||
||   - baseline_ is the ACCEPTED document the session     ||
||     last loaded/adopted (with the durable revisions)   ||
||   - working_  is the user's draft; Dirty() compares    ||
||     the deterministic serialization                    ||
||   - studio::EditorController ops mutate working_ and   ||
||     hand back an EditorEdit — CommitEdit pushes ONE    ||
||     undo record per committed gesture                  ||
||   - fields EditorEdit does not cover (meta.name,       ||
||     brightness, inputs) commit through CommitDocEdit's ||
||     whole-document snapshot record — same one-record-  ||
||     per-gesture rule                                   ||
||   - history is bounded by 32 entries AND 32 MiB        ||
||     (evicting the oldest undo entries first)           ||
||   - a gesture Cancel restores the begin snapshot and   ||
||     leaves NO history record                           ||
||   - ApplyRemote moves only the baseline: a dirty draft ||
||     is preserved verbatim (the conflict is the         ||
||     controller's flag, not a merge)                    ||
||                                                           ||
||   Qt-free like SceneEditModel/EngineWireClient — the   ||
||   offline suite drives it without an event loop.       ||
||                                                           ||
||   SPDX-License-Identifier: GPL-2.0-or-later               ||
||\*---------------------------------------------------------*/

#pragma once

#include "editor/EditorController.h"

#include <cstdint>
#include <deque>
#include <functional>
#include <optional>
#include <string>

namespace openrgb2
{
namespace ui
{

class WorkspaceEditSession
{
public:
    /* The plan's history bounds (section 3.1): a gesture flood
       can never grow memory without limit. */
    static constexpr size_t kHistoryMaxEntries = 32;
    static constexpr size_t kHistoryMaxBytes   = 32ull * 1024 * 1024;

    WorkspaceEditSession();
    WorkspaceEditSession(const WorkspaceEditSession&)            = delete;
    WorkspaceEditSession& operator=(const WorkspaceEditSession&) = delete;

    /*---- applied/draft state -------------------------------*/
    /* Fresh load of the accepted workspace at the given
       durable revisions (open, first attach, reload): draft =
       baseline = doc, history cleared. */
    void LoadApplied(const studio::StudioDocument& doc,
                     uint64_t workspace_revision,
                     uint64_t resource_revision);
    /* The accepted workspace moved UNDER the draft (another
       client committed, or the engine re-opened it): a CLEAN
       draft adopts the new document; a DIRTY draft is
       preserved verbatim — baseline moves underneath, Dirty()
       recomputes, and the conflict surfaces in
       WorkspaceController, never a silent merge. History is
       kept: its records still describe real edits. */
    void ApplyRemote(const studio::StudioDocument& doc,
                     uint64_t workspace_revision,
                     uint64_t resource_revision);
    /* Our OWN commit was accepted: adopt the committed
       document + reported revisions as the new baseline.
       Edits made during the save stay pending. */
    void AdoptCommitted(const studio::StudioDocument& committed_doc,
                        uint64_t workspace_revision,
                        uint64_t resource_revision);
    /* Recovery staging: `draft` becomes the working copy —
       baseline + revisions are unchanged so the record never
       reads as engine state. Nothing is sent anywhere. */
    void LoadDraft(const studio::StudioDocument& draft);
    /* Wholesale replacement staging (profile/library load):
       one snapshot undo record, labeled. */
    bool StageWorking(const studio::StudioDocument& doc,
                      const std::string& label);

    bool HasBaseline() const { return has_baseline_; }
    bool HasWorking()  const { return has_working_; }
    const studio::StudioDocument& Working()  const { return working_; }
    const studio::StudioDocument& Baseline() const { return baseline_; }
    /* Deterministic-serialization comparison — the same bytes
       the store persists (ToJson never serializes the resolved
       runtime scene). */
    bool Dirty() const;
    uint64_t BaseRevision()     const { return base_rev_; }
    uint64_t ResourceRevision() const { return res_rev_; }

    /* The editor bound to working_ — selection, transform
       gestures (BeginTransform, Preview, Commit, Cancel) and
       every discrete op live here; commit their returned
       EditorEdit through CommitEdit. */
    studio::EditorController& Editor() { return editor_; }

    /*---- undo records --------------------------------------*/
    /* Push one committed edit. False on an empty/rejected edit
       — those produce NO history (the EditorController
       contract). Pushing clears the redo timeline. */
    bool CommitEdit(const studio::EditorEdit& edit);
    /* Whole-document record for fields outside EditorEdit's
       coverage: `mutate` edits Working() in place; a no-op
       mutation (returns false / no change) leaves no record. */
    bool CommitDocEdit(
        const std::string& label,
        const std::function<bool(studio::StudioDocument&)>& mutate);
    bool CommitDocEdit(const std::string& label,
                       const studio::StudioDocument& new_doc);

    bool Undo();
    bool Redo();
    /* working = baseline; clears history + any live gesture —
       the user walked away from the draft edits. */
    void Discard();
    void ClearHistory();

    bool        CanUndo()   const { return !undo_.empty(); }
    bool        CanRedo()   const { return !redo_.empty(); }
    std::string UndoLabel() const;
    std::string RedoLabel() const;
    size_t      UndoCount() const { return undo_.size(); }
    size_t      RedoCount() const { return redo_.size(); }
    /* Live bound accounting (test-visible). */
    size_t      HistoryBytes() const { return history_bytes_; }

    /* Bound injection for the offline suite — production keeps
       the defaults. */
    void SetHistoryBoundsForTesting(size_t max_entries,
                                    size_t max_bytes);

private:
    struct HistoryEntry
    {
        std::string                        label;
        /* Delta record (EditorEdit) — invertible in place. */
        std::optional<studio::EditorEdit>  edit;
        /* Snapshot record — restores whole documents (fields
           the delta shape cannot express). */
        std::optional<studio::StudioDocument> before;
        std::optional<studio::StudioDocument> after;
        size_t                             bytes = 0;
    };
    static size_t EntryCost(const HistoryEntry& e);
    void Push(std::deque<HistoryEntry>& stack, HistoryEntry e);
    void Apply(const HistoryEntry& e);   /* after side */
    void Revert(const HistoryEntry& e);  /* before side */

    studio::StudioDocument   baseline_;
    studio::StudioDocument   working_;
    bool                     has_baseline_ = false;
    bool                     has_working_  = false;
    uint64_t                 base_rev_ = 0;
    uint64_t                 res_rev_  = 0;
    /* Bound to working_ for life — document assignment keeps
       the reference valid (the editor only ever sees the same
       object). */
    studio::EditorController editor_;
    std::deque<HistoryEntry> undo_;
    std::deque<HistoryEntry> redo_;
    size_t                   history_bytes_ = 0;
    size_t                   max_entries_ = kHistoryMaxEntries;
    size_t                   max_bytes_   = kHistoryMaxBytes;
};

} /* namespace ui */
} /* namespace openrgb2 */
