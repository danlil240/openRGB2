// SPDX-License-Identifier: GPL-2.0-or-later
//
// WorkspaceEditSession — the ONE shared workspace draft. See
// the header for the contract; the implementation notes below
// only cover the non-obvious mechanics.
//
// Undo history has two record shapes:
//   - delta records (studio::EditorEdit) — what
//     EditorController hands back; Undo/Redo invert them in
//     place with ApplyEditorEdit/RevertEditorEdit
//   - snapshot records — whole-document before/after for the
//     fields a delta shape cannot express (meta, inputs,
//     brightness)
//
// Byte accounting is deliberately conservative: a delta
// record is priced at the serialized size of the document it
// produced, a snapshot record at before+after. The real
// retained state is smaller — the bound stays honest without
// needing a serializer for EditorEdit internals.

#include "WorkspaceEditSession.h"

#include "config/StudioConfig.h"
#include "editor/TransformCommands.h"

namespace openrgb2
{
namespace ui
{

WorkspaceEditSession::WorkspaceEditSession()
    : editor_(working_)   /* bound once — working_ never moves */
{
}

/*---------------------------------------------------------*\
|| applied / draft state                                     ||
\*---------------------------------------------------------*/

void WorkspaceEditSession::LoadApplied(
    const studio::StudioDocument& doc,
    uint64_t workspace_revision, uint64_t resource_revision)
{
    if(editor_.GestureActive())
    {
        editor_.Cancel();
    }
    baseline_     = doc;
    working_      = doc;
    has_baseline_ = true;
    has_working_  = true;
    base_rev_     = workspace_revision;
    res_rev_      = resource_revision;
    editor_.ClearSelection();
    ClearHistory();
}

void WorkspaceEditSession::ApplyRemote(
    const studio::StudioDocument& doc,
    uint64_t workspace_revision, uint64_t resource_revision)
{
    if(editor_.GestureActive())
    {
        editor_.Cancel();
    }
    /* Dirtiness is measured BEFORE the baseline moves — a
       clean draft tracks the new document; a dirty draft is
       preserved verbatim against the moved baseline. */
    const bool was_dirty = Dirty();
    baseline_ = doc;
    if(!was_dirty)
    {
        working_ = doc;
    }
    base_rev_ = workspace_revision;
    res_rev_  = resource_revision;
}

void WorkspaceEditSession::AdoptCommitted(
    const studio::StudioDocument& committed_doc,
    uint64_t workspace_revision, uint64_t resource_revision)
{
    baseline_ = committed_doc;
    base_rev_ = workspace_revision;
    res_rev_  = resource_revision;
    /* working_ untouched — edits that landed during the save
       stay pending against the new baseline. */
}

void WorkspaceEditSession::LoadDraft(
    const studio::StudioDocument& draft)
{
    if(editor_.GestureActive())
    {
        editor_.Cancel();
    }
    working_     = draft;
    has_working_ = true;
    /* Baseline + revisions unchanged — a recovered record is
       draft work, never engine state. */
}

bool WorkspaceEditSession::StageWorking(
    const studio::StudioDocument& doc, const std::string& label)
{
    return CommitDocEdit(label, doc);
}

bool WorkspaceEditSession::Dirty() const
{
    if(!has_working_)
    {
        return false;
    }
    if(!has_baseline_)
    {
        return true;
    }
    return studio::ToJson(working_) != studio::ToJson(baseline_);
}

/*---------------------------------------------------------*\
|| undo records                                              ||
\*---------------------------------------------------------*/

bool WorkspaceEditSession::CommitEdit(const studio::EditorEdit& edit)
{
    if(edit.Empty())
    {
        return false;
    }
    HistoryEntry e;
    e.label = edit.label;
    e.edit  = edit;
    /* Conservative cost: the serialized document the edit
       produced bounds the record's real retained state. */
    e.bytes = edit.label.size() + 64 +
              studio::ToJson(working_).dump().size();
    Push(undo_, std::move(e));
    /* A new edit closes the redo timeline. */
    for(const HistoryEntry& r : redo_)
    {
        history_bytes_ -= r.bytes;
    }
    redo_.clear();
    return true;
}

bool WorkspaceEditSession::CommitDocEdit(
    const std::string& label,
    const std::function<bool(studio::StudioDocument&)>& mutate)
{
    if(!has_working_)
    {
        return false;
    }
    if(editor_.GestureActive())
    {
        editor_.Cancel();
    }
    const studio::StudioDocument before = working_;
    if(!mutate(working_))
    {
        return false;
    }
    if(studio::ToJson(before) == studio::ToJson(working_))
    {
        /* A mutation that changed nothing leaves no record. */
        working_ = before;
        return false;
    }
    HistoryEntry e;
    e.label  = label;
    e.before = before;
    e.after  = working_;
    e.bytes  = label.size() + 64 +
               studio::ToJson(*e.before).dump().size() +
               studio::ToJson(*e.after).dump().size();
    Push(undo_, std::move(e));
    for(const HistoryEntry& r : redo_)
    {
        history_bytes_ -= r.bytes;
    }
    redo_.clear();
    return true;
}

bool WorkspaceEditSession::CommitDocEdit(
    const std::string& label, const studio::StudioDocument& new_doc)
{
    return CommitDocEdit(label,
        [&new_doc](studio::StudioDocument& d)
        {
            d = new_doc;
            return true;
        });
}

bool WorkspaceEditSession::SetMeta(
    const std::function<bool(studio::WorkspaceMeta&)>& mutate)
{
    if(!has_working_ || mutate == nullptr)
    {
        return false;
    }
    /* Snapshot the serialized draft so a mutation that changed
       nothing leaves no trace — and no UNDO record is ever
       created: editor prefs are not scene history. Dirty()
       picks a real change up against the baseline on its own. */
    const nlohmann::json before = studio::ToJson(working_);
    if(!mutate(working_.meta))
    {
        return false;
    }
    return studio::ToJson(working_) != before;
}

bool WorkspaceEditSession::Undo()
{
    if(undo_.empty())
    {
        return false;
    }
    if(editor_.GestureActive())
    {
        editor_.Cancel();
    }
    HistoryEntry e = std::move(undo_.back());
    undo_.pop_back();
    history_bytes_ -= e.bytes;
    Revert(e);
    Push(redo_, std::move(e));
    return true;
}

bool WorkspaceEditSession::Redo()
{
    if(redo_.empty())
    {
        return false;
    }
    if(editor_.GestureActive())
    {
        editor_.Cancel();
    }
    HistoryEntry e = std::move(redo_.back());
    redo_.pop_back();
    history_bytes_ -= e.bytes;
    Apply(e);
    Push(undo_, std::move(e));
    return true;
}

void WorkspaceEditSession::Discard()
{
    if(editor_.GestureActive())
    {
        editor_.Cancel();
    }
    if(has_baseline_)
    {
        working_    = baseline_;
        has_working_ = true;
    }
    ClearHistory();
}

void WorkspaceEditSession::ClearHistory()
{
    undo_.clear();
    redo_.clear();
    history_bytes_ = 0;
}

std::string WorkspaceEditSession::UndoLabel() const
{
    return undo_.empty() ? std::string() : undo_.back().label;
}

std::string WorkspaceEditSession::RedoLabel() const
{
    return redo_.empty() ? std::string() : redo_.back().label;
}

void WorkspaceEditSession::SetHistoryBoundsForTesting(
    size_t max_entries, size_t max_bytes)
{
    max_entries_ = max_entries;
    max_bytes_   = max_bytes;
    /* Re-evict under the tighter bound. */
    while(undo_.size() > max_entries_ ||
          (history_bytes_ > max_bytes_ && undo_.size() > 1))
    {
        history_bytes_ -= undo_.front().bytes;
        undo_.pop_front();
    }
}

size_t WorkspaceEditSession::EntryCost(const HistoryEntry& e)
{
    return e.bytes;
}

void WorkspaceEditSession::Push(std::deque<HistoryEntry>& stack,
                                HistoryEntry e)
{
    history_bytes_ += e.bytes;
    stack.push_back(std::move(e));
    while(stack.size() > max_entries_ ||
          (history_bytes_ > max_bytes_ && stack.size() > 1))
    {
        history_bytes_ -= stack.front().bytes;
        stack.pop_front();
    }
}

void WorkspaceEditSession::Apply(const HistoryEntry& e)
{
    if(e.edit.has_value())
    {
        studio::ApplyEditorEdit(working_, *e.edit);
    }
    else if(e.after.has_value())
    {
        working_ = *e.after;
    }
}

void WorkspaceEditSession::Revert(const HistoryEntry& e)
{
    if(e.edit.has_value())
    {
        studio::RevertEditorEdit(working_, *e.edit);
    }
    else if(e.before.has_value())
    {
        working_ = *e.before;
    }
}

} /* namespace ui */
} /* namespace openrgb2 */
