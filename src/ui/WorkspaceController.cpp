// SPDX-License-Identifier: GPL-2.0-or-later
//
// WorkspaceController — QObject front of the shared workspace
// draft. See the header for the correlation contract; the
// pieces below are:
//
//   bridge feeds   — hasWorkspaceChanged (capability gate),
//                    workspaceChanged (snapshot AND job-status
//                    records share the store), replyReceived
//                    (request-id tags), engineLost.
//   snapshot/doc   — ApplySnapshot moves engine truth; the
//                    document body rides the bounded transfer
//                    download and is adopted per reconcile /
//                    force-adopt / dirty state.
//   jobs           — commit/save_copy/reload/import/export all
//                    resolve through the terminal job record;
//                    request replies only carry {accepted,
//                    job_id}.
//   draft shadow   — NoteDraftEdited stages the debounced
//                    record; recovery is offered, never
//                    applied.

#include "WorkspaceController.h"

#include "EngineBridge.h"
#include "JsonVariant.h"
#include "ProfileLibrary.h"

#include "config/StudioConfig.h"

#include <QCryptographicHash>
#include <QDesktopServices>
#include <QFile>
#include <QMetaObject>
#include <QUrl>

namespace openrgb2
{
namespace ui
{

namespace
{

QString Sha256Hex(const QByteArray& bytes)
{
    return QString::fromLatin1(
        QCryptographicHash::hash(bytes, QCryptographicHash::Sha256)
            .toHex());
}

QString Sha256Hex(const std::string& s)
{
    return Sha256Hex(QByteArray::fromStdString(s));
}

quint64 JU64(const QVariantMap& m, const char* key)
{
    return m.value(QLatin1String(key)).toULongLong();
}

} /* anonymous namespace */

WorkspaceController::WorkspaceController(EngineBridge* bridge,
                                         QObject* parent)
    : QObject(parent)
    , bridge_(bridge)
{
    connect(bridge_, &EngineBridge::hasWorkspaceChanged,
            this, &WorkspaceController::OnAvailabilityChanged);
    connect(bridge_, &EngineBridge::workspaceChanged,
            this, &WorkspaceController::OnWorkspaceChanged);
    connect(bridge_, &EngineBridge::replyReceived,
            this, &WorkspaceController::OnReplyReceived);
    connect(bridge_, &EngineBridge::connectionStateChanged,
            this, &WorkspaceController::OnConnectionStateChanged);
    connect(bridge_, &EngineBridge::engineLost,
            this, &WorkspaceController::OnEngineLost);
    /* Task 10: passive external-change detection — the poll
       issues `get`; the reply's external_change compare is
       read-only both sides. It never loads a document and
       never applies. */
    connect(&ext_poll_, &QTimer::timeout, this, [this] {
        if(available_ && managed_ && bridge_ &&
           bridge_->connected())
        {
            SendCommand(QStringLiteral("get"), {},
                        QStringLiteral("get"));
        }
    });
    /* Draft records from a previous run predate any engine —
       offer recovery once local state exists. */
    QMetaObject::invokeMethod(
        this, [this] { CheckRecoveryOffer(); },
        Qt::QueuedConnection);
    if(bridge_->hasWorkspace())
    {
        OnAvailabilityChanged();
    }
}

/*---------------------------------------------------------*\
|| state getters                                             ||
\*---------------------------------------------------------*/

bool    WorkspaceController::available() const { return available_; }
bool    WorkspaceController::managed() const   { return managed_; }
bool    WorkspaceController::ready() const
                                                { return session_.HasWorking(); }
bool    WorkspaceController::loading() const   { return loading_; }
QString WorkspaceController::workspaceId() const   { return ws_id_; }
QString WorkspaceController::workspaceName() const { return ws_name_; }
quint64 WorkspaceController::workspaceRevision() const { return ws_rev_; }
quint64 WorkspaceController::resourceRevision() const  { return res_rev_; }
QVariantList WorkspaceController::workspaceList() const { return ws_list_; }
bool    WorkspaceController::dirty() const    { return session_.Dirty(); }
bool    WorkspaceController::conflict() const { return conflict_; }
bool    WorkspaceController::saving() const   { return saving_; }
QString WorkspaceController::error() const       { return error_; }
QString WorkspaceController::errorSource() const { return error_source_; }
bool    WorkspaceController::canUndo() const  { return session_.CanUndo(); }
bool    WorkspaceController::canRedo() const  { return session_.CanRedo(); }
QString WorkspaceController::undoLabel() const
    { return QString::fromStdString(session_.UndoLabel()); }
QString WorkspaceController::redoLabel() const
    { return QString::fromStdString(session_.RedoLabel()); }
bool    WorkspaceController::recoveryOffered() const { return recovery_offered_; }
bool    WorkspaceController::recoveryStale() const   { return recovery_stale_; }
QString WorkspaceController::statusText() const      { return status_text_; }
QString WorkspaceController::externalChange() const  { return external_state_; }
QString WorkspaceController::externalDetail() const  { return external_detail_; }
QString WorkspaceController::workspaceDir() const    { return ws_dir_; }
QString WorkspaceController::workspaceRoot() const   { return ws_root_; }
bool    WorkspaceController::replaceOffered() const  { return pending_replace_.has_value(); }
QString WorkspaceController::replaceText() const
{
    return pending_replace_ ? pending_replace_->text : QString();
}

void WorkspaceController::setExternalPollForTesting(int ms)
{
    ext_poll_ms_ = ms;
    UpdatePoll();
}

void WorkspaceController::SetExternal(const QString& state,
                                      const QString& detail)
{
    const QString s =
        (state == QLatin1String("modified") ||
         state == QLatin1String("deleted")) ? state : QString();
    const QString prev = external_state_;
    if(s == prev && detail == external_detail_)
    {
        return;
    }
    external_state_  = s;
    external_detail_ = detail;
    emit externalChanged();
    if(!s.isEmpty() && s != prev)
    {
        /* Reported ONCE per state transition — including an
           escalation (modified -> deleted) which is its own
           event. Detection never applies, never saves, never
           touches hardware; the notice is the whole mechanism
           (Task 10 hard rule). */
        emit notice(QStringLiteral(
            "Workspace '%1' files changed on disk (%2%3) — "
            "nothing was applied. Reload takes the disk copy; "
            "your draft is untouched.")
                .arg(ws_id_)
                .arg(s)
                .arg(detail.isEmpty()
                         ? QString()
                         : QStringLiteral(": ") + detail));
    }
}

void WorkspaceController::UpdatePoll()
{
    if(ext_poll_ms_ > 0 && available_ && managed_ &&
       bridge_ != nullptr && bridge_->connected())
    {
        if(!ext_poll_.isActive())
        {
            ext_poll_.start(ext_poll_ms_);
        }
    }
    else
    {
        ext_poll_.stop();
    }
}

void WorkspaceController::OfferReplace(const QString& action,
                                       const QString& id,
                                       const QString& text)
{
    /* The dirty draft is NEVER replaced silently — the action
       latches here until confirmReplace()/cancelReplace()
       resolves it (Task 10: source/destination + conflict
       info BEFORE replacing). */
    pending_replace_ = PendingReplace{ action, id, text };
    emit replaceChanged();
    emit notice(text);
}

/*---------------------------------------------------------*\
|| small helpers                                             ||
\*---------------------------------------------------------*/

int WorkspaceController::SendCommand(const QString& verb,
                                     const QVariantMap& args,
                                     const QString& tag)
{
    const int id = bridge_->workspaceCommand(verb, args);
    if(id > 0)
    {
        pending_reqs_[id] = tag;
    }
    return id;
}

QString WorkspaceController::BaselineSha()
{
    if(!session_.HasBaseline())
    {
        baseline_sha_.clear();
        return QString();
    }
    /* Cached — the baseline only moves at LoadApplied /
       ApplyRemote / AdoptCommitted (each call site clears the
       cache); a per-edit recompute serializes the whole
       document every keystroke. */
    if(baseline_sha_.isEmpty())
    {
        baseline_sha_ = Sha256Hex(
            studio::ToJson(session_.Baseline()).dump(2));
    }
    return baseline_sha_;
}

QString WorkspaceController::SlugFor(const QString& name)
{
    QString out;
    for(const QChar c : name.toLower())
    {
        if(c.isLetterOrNumber())
        {
            out += c;
        }
        else if(!out.isEmpty() && !out.endsWith('-'))
        {
            out += '-';
        }
    }
    while(out.endsWith('-'))
    {
        out.chop(1);
    }
    return out.isEmpty() ? QStringLiteral("workspace") : out;
}

void WorkspaceController::SetError(const QString& e,
                                   const QString& source)
{
    if(e == error_ && source == error_source_)
    {
        return;
    }
    error_        = e;
    error_source_ = source;
    emit errorChanged();
}

void WorkspaceController::SetStatus(const QString& s)
{
    if(s == status_text_)
    {
        return;
    }
    status_text_ = s;
    emit statusTextChanged();
}

void WorkspaceController::SetConflict(bool on, const QString& why)
{
    if(on == conflict_)
    {
        return;
    }
    conflict_ = on;
    emit conflictChanged();
    if(on)
    {
        SetStatus(QStringLiteral("Workspace changed on the "
                                 "engine — review before "
                                 "saving"));
        emit notice(why.isEmpty()
            ? QStringLiteral("The managed workspace moved "
                             "under pending edits. Reload to "
                             "take the engine copy, or save to "
                             "commit your draft.")
            : why);
    }
}

void WorkspaceController::SetSaving(bool on)
{
    if(on == saving_)
    {
        return;
    }
    saving_ = on;
    emit savingChanged();
}

void WorkspaceController::SetLoading(bool on)
{
    if(on == loading_)
    {
        return;
    }
    loading_ = on;
    emit loadingChanged();
}

/*---------------------------------------------------------*\
|| bridge feeds                                              ||
\*---------------------------------------------------------*/

void WorkspaceController::OnAvailabilityChanged()
{
    const bool now = bridge_->hasWorkspace();
    if(now == available_)
    {
        /* First attach already set it; still re-read the
           snapshot — a reconnect reuses the same flag. */
        if(now)
        {
            SendCommand(QStringLiteral("get"), {},
                        QStringLiteral("get"));
        }
        return;
    }
    available_ = now;
    emit availabilityChanged();
    if(available_)
    {
        SetError(QString(), QString());
        SendCommand(QStringLiteral("get"), {},
                    QStringLiteral("get"));
    }
    else
    {
        /* Legacy engine — no workspace surface. Managed state
           clears honestly; the local draft + recovery are
           untouched (they never needed the engine). */
        if(managed_)
        {
            managed_ = false;
            emit workspaceChanged();
        }
        SetStatus(QStringLiteral(
            "Engine does not expose workspaces — "
            "legacy controls in use"));
    }
    CheckRecoveryOffer();
    UpdatePoll();
}

void WorkspaceController::OnConnectionStateChanged()
{
    /* Reconnect: WELCOME re-advertises the capability but the
       hasWorkspace flag may not toggle — issue get whenever a
       session (re)opens on a capable engine. */
    if(bridge_->connected() && bridge_->hasWorkspace())
    {
        if(!available_)
        {
            OnAvailabilityChanged();
        }
        else
        {
            SendCommand(QStringLiteral("get"), {},
                        QStringLiteral("get"));
        }
    }
    UpdatePoll();
}

void WorkspaceController::OnEngineLost(const QString& reason)
{
    /* Capture the in-flight import's name BEFORE the abort
       clears it — the entry still owes the UI a terminal
       signal. */
    QString inflight_import;
    if(ul_.has_value() &&
       ul_->purpose.startsWith(QLatin1String("import:")))
    {
        inflight_import = ul_->purpose.mid(7);
    }
    AbortTransfers();
    SetLoading(false);
    if(saving_)
    {
        /* The commit MAY have landed before the drop — keep
           the pending doc + hash so the next snapshot can
           reconcile by content_sha256; the draft stays
           exactly as the user left it. */
        reconcile_pending_ = true;
        SetSaving(false);
        pending_job_id_ = 0;
        SetError(QStringLiteral("connection lost during save: %1")
                     .arg(reason), QStringLiteral("connection"));
        emit saveFinished(false);
    }
    pending_reqs_.clear();
    /* Job ids are per-session state — a reconnect starts
       numbering over, so stale tracked ids could collide with
       a NEW job and misattribute its terminal event. Queued
       imports stop with the session; the upload AbortTransfers
       just cancelled can never resume. Every outstanding
       import entry still owes the UI a terminal signal — a
       per-entry waiter must never hang on a dropped queue. */
    const QString lost = QStringLiteral("connection lost: %1")
                             .arg(reason);
    tracked_jobs_.clear();
    if(!inflight_import.isEmpty())
    {
        emit importFinished(inflight_import, false, lost);
    }
    for(const auto& kv : import_req_names_)
    {
        emit importFinished(kv.second, false, lost);
    }
    import_req_names_.clear();
    for(const auto& kv : import_names_)
    {
        emit importFinished(kv.second, false, lost);
    }
    import_names_.clear();
    for(const PendingImport& imp : import_queue_)
    {
        emit importFinished(imp.name, false, lost);
    }
    import_queue_.clear();
    /* Task 10: a pending replace latch and the external
       compare die with the session — both re-derive from the
       next attach's poll. */
    if(pending_replace_)
    {
        pending_replace_.reset();
        emit replaceChanged();
    }
    pending_switch_id_.clear();
    SetExternal(QString(), QString());
    UpdatePoll();
    /* A pending bundle inspect dies with the session — the
       dialog sees the honest failure, never a hang. */
    if(!pending_inspect_token_.isEmpty() ||
       !pending_inspect_name_.isEmpty())
    {
        pending_inspect_token_.clear();
        pending_inspect_name_.clear();
        emit importInspectionReady(QVariantMap({
            { QStringLiteral("valid"),  false },
            { QStringLiteral("errors"), QStringList{ lost } },
        }));
    }
    /* Every import latch is now drained — the banner must not
       survive the session that armed it. */
    SettleImportStatus();
}

void WorkspaceController::OnWorkspaceChanged()
{
    const QVariantMap m = bridge_->workspace();
    if(m.contains(QStringLiteral("job_id")) &&
       m.contains(QStringLiteral("state")))
    {
        OnJobEvent(m);
        return;
    }
    if(m.contains(QStringLiteral("managed")) ||
       m.contains(QStringLiteral("id")))
    {
        ApplySnapshot(m);
        return;
    }
    /* transfer_swept / misc notices — informational only. */
}

/*---------------------------------------------------------*\
|| snapshot + document adoption                              ||
\*---------------------------------------------------------*/

void WorkspaceController::ApplySnapshot(const QVariantMap& snap)
{
    const bool    managed = snap.value(QStringLiteral("managed"))
                                .toBool();
    const QString id      = snap.value(QStringLiteral("id"))
                                .toString();
    const quint64 ws      = JU64(snap, "workspace_revision");
    const quint64 res     = JU64(snap, "resource_revision");
    const QString sha     = snap.value(QStringLiteral("content_sha256"))
                                .toString();

    if(!managed || id.isEmpty())
    {
        if(managed_)
        {
            managed_ = false;
            emit workspaceChanged();
        }
        SetExternal(QString(), QString());
        ws_dir_.clear();
        ws_root_.clear();
        UpdatePoll();
        return;
    }

    /* Task 10: the poll reply also carries the managed paths
       and the engine's passive file compare — LIVE disk truth,
       orthogonal to the document's revision ordering. It must
       land BEFORE the stale-echo guard below: a dropped
       doc/revision field must never freeze the external-change
       latch (a recovered reload publishes a LOWER revision —
       dropping those replies left "modified" latched forever).
       Absent fields (a scripted/older engine) leave prior
       state alone. */
    const QString new_root =
        snap.value(QStringLiteral("workspace_root")).toString();
    const QString new_dir  =
        snap.value(QStringLiteral("workspace_dir")).toString();
    bool identity_changed = false;
    if(!new_root.isEmpty() && new_root != ws_root_)
    {
        ws_root_ = new_root;
        identity_changed = true;
    }
    if(!new_dir.isEmpty() && new_dir != ws_dir_)
    {
        ws_dir_ = new_dir;
        identity_changed = true;
    }
    if(snap.contains(QStringLiteral("external_change")))
    {
        SetExternal(snap.value(QStringLiteral("external_change"))
                        .toString(),
                    snap.value(QStringLiteral("external_detail"))
                        .toString());
    }

    /* Stale-echo guard: a replayed older snapshot carries the
       SAME content hash — drop the doc/revision fields so an
       out-of-order push can never roll the view back or
       clobber newer draft state. A LOWER revision with a
       DIFFERENT hash is a real rollback (the store recovered a
       tampered generation and adopted the last-valid one) — it
       must be fetched+adopted, not ignored, or the UI latches
       stale flags forever. */
    if(id == ws_id_ && managed_ && ws < ws_rev_ &&
       (sha.isEmpty() || sha == ws_sha256_))
    {
        if(identity_changed)
        {
            emit workspaceChanged();
        }
        return;
    }

    const bool ws_changed  = id != ws_id_;
    if(ws_changed || id == pending_switch_id_)
    {
        /* The open-target switch resolved — the ws_changed
           fetch below now downloads the RIGHT id. */
        pending_switch_id_.clear();
    }
    const bool revs_moved  = ws_changed || ws != ws_rev_ ||
                             res != res_rev_;
    const bool sha_changed = sha != ws_sha256_;
    /* A RESOURCE-only move: install/remove jobs advance the
       library revision while the document (revision + content
       hash) stands — the draft's commit base rebases silently
       rather than fetching a byte-identical document and
       flagging a conflict (F07). Covers the poll-before-event
       ordering; the event ordering rebase lives in
       OnJobEvent. */
    const bool res_only_move =
        managed_ && !ws_changed && ws == ws_rev_ &&
        res > res_rev_ && (sha.isEmpty() || sha == ws_sha256_);
    ws_id_   = id;
    ws_name_ = snap.value(QStringLiteral("name")).toString();
    ws_rev_  = ws;
    res_rev_ = res;
    ws_sha256_ = sha;
    if(!managed_ || ws_changed || revs_moved || identity_changed)
    {
        managed_ = true;
        emit workspaceChanged();
    }
    UpdatePoll();

    /* An offer may predate the engine attach — restate its
       staleness against the live revisions, and DROP an offer
       the resolved managed id proves foreign (a pre-attach
       fallback can't know the managed id until now). */
    if(recovery_offered_)
    {
        const bool foreign = managed_ &&
                             offered_ws_id_ != ws_id_;
        const WorkspaceDraftRecord rec =
            drafts_.Read(offered_ws_id_);
        const bool stale = foreign || !rec.exists ||
            !managed_ ||
            rec.baseline_revision != ws_rev_ ||
            rec.resource_revision != res_rev_;
        if(foreign || !rec.exists)
        {
            recovery_offered_ = false;
            recovery_stale_   = false;
            offered_ws_id_.clear();
            emit recoveryChanged();
        }
        else if(stale != recovery_stale_)
        {
            recovery_stale_ = stale;
            emit recoveryChanged();
        }
    }

    if(reconcile_pending_ && !pending_doc_sha_.isEmpty() &&
       !sha.isEmpty() && sha == pending_doc_sha_)
    {
        /* Our in-flight commit LANDED before the drop — adopt
           it by hash rather than resubmitting or rolling the
           draft back. */
        studio::StudioDocument d;
        std::vector<std::string> errs;
        if(studio::FromJson(pending_doc_, d, &errs))
        {
            session_.AdoptCommitted(d, ws, res);
            baseline_sha_.clear();
        }
        pending_doc_      = nullptr;
        pending_doc_sha_  = QString();
        reconcile_pending_ = false;
        SetSaving(false);
        SetConflict(false);
        SetError(QString(), QString());
        emit saveFinished(true);
        emit draftEdited();
        NoteDraftEdited();
        CheckRecoveryOffer();
        return;
    }
    /* The pending commit did NOT land — it either failed or
       never ran; stop reconciling when revisions confirm the
       snapshot is current (not a stale echo). */
    if(reconcile_pending_ && ws >= session_.BaseRevision())
    {
        reconcile_pending_ = false;
        pending_doc_      = nullptr;
        pending_doc_sha_  = QString();
    }

    if(!session_.HasWorking() ||
       (ws_changed && session_.HasWorking()))
    {
        if(ws_changed)
        {
            /* A different managed workspace is active — the
               old draft stays in its own durable record; the
               new document loads fresh. */
            force_adopt_ = true;
        }
        FetchDocument();
        return;
    }
    if(res_only_move)
    {
        session_.RebaseRevisions(ws, res);
        /* The durable record re-stages the rebased pair — a
           recovered draft must not be born stale. */
        NoteDraftEdited();
    }
    else if(revs_moved || sha_changed)
    {
        /* The accepted workspace moved under the draft —
           fetch the new document; the session decides adopt
           (clean) vs preserve (dirty) so nothing merges or
           clobbers silently. */
        FetchDocument();
    }
    CheckRecoveryOffer();
}

void WorkspaceController::FetchDocument()
{
    /* While a foreign open is latched, ws_id_ still names the
       OLD workspace — a revs_moved snapshot for it (a commit
       landing mid-switch) must NOT fetch: the download would
       adopt under force_adopt_ and clean-Discard the staged
       draft record the replace latch promised survives. The
       resolving snapshot's ws_changed path refetches. */
    if(!available_ || ws_id_.isEmpty() || dl_.has_value() ||
       dl_requested_ || !pending_switch_id_.isEmpty())
    {
        return;
    }
    SetLoading(true);
    const int id = SendCommand(
        QStringLiteral("transfer_begin"),
        QVariantMap({
            { QStringLiteral("direction"), QStringLiteral("download") },
            { QStringLiteral("kind"),      QStringLiteral("document") },
            { QStringLiteral("id"),        ws_id_ },
        }),
        QStringLiteral("dl_begin"));
    if(id == 0)
    {
        SetLoading(false);
        SetError(QStringLiteral("could not start document "
                                "fetch"), QStringLiteral("connection"));
    }
    else
    {
        dl_requested_      = true;
        dl_request_ws_     = ws_id_;
        dl_request_epoch_  = fetch_epoch_;
    }
}

void WorkspaceController::AdoptFetchedDocument(
    const QByteArray& bytes)
{
    const nlohmann::json j = nlohmann::json::parse(
        bytes.toStdString(), nullptr, false);
    studio::StudioDocument d;
    std::vector<std::string> errs;
    if(j.is_discarded() || !studio::FromJson(j, d, &errs))
    {
        SetError(QStringLiteral("workspace document failed "
                                "to parse: %1")
                     .arg(errs.empty()
                              ? QStringLiteral("malformed json")
                              : QString::fromStdString(errs.front())),
                 QStringLiteral("engine"));
        return;
    }
    if(reconcile_pending_ && !pending_doc_sha_.isEmpty() &&
       Sha256Hex(bytes) == pending_doc_sha_)
    {
        session_.AdoptCommitted(d, ws_rev_, res_rev_);
        baseline_sha_.clear();
        pending_doc_       = nullptr;
        pending_doc_sha_   = QString();
        reconcile_pending_ = false;
        SetSaving(false);
        SetConflict(false);
        emit saveFinished(true);
    }
    else if(force_adopt_ || !session_.HasWorking())
    {
        force_adopt_ = false;
        session_.LoadApplied(d, ws_rev_, res_rev_);
        baseline_sha_.clear();
        SetConflict(false);
        /* A recovery offer for this workspace stays up until
           the user decides — the engine doc loading clean must
           never silently consume a crash record. */
    }
    else
    {
        session_.ApplyRemote(d, ws_rev_, res_rev_);
        baseline_sha_.clear();
        if(session_.Dirty())
        {
            SetConflict(true);
        }
    }
    /* A successful adoption resolves whatever the error strip
       last reported — take-engine-copy after a failed save
       must not leave the stale "revision" message up (F07). */
    SetError(QString(), QString());
    emit draftEdited();
    const bool r = session_.HasWorking();
    if(r != ready_cached_)
    {
        ready_cached_ = r;
        emit draftChanged();
    }
    /* A freshly applied/recovered document carries its own
       camera + render editor prefs — the viewport re-reads
       them on this pair (Task 8). */
    emit cameraChanged();
    emit renderPrefsChanged();
    NoteDraftEdited();
    CheckRecoveryOffer();
    SetStatus(session_.Dirty()
        ? QStringLiteral("Unsaved changes")
        : QStringLiteral("In sync with engine"));
}

/*---------------------------------------------------------*\
|| recovery offer                                            ||
\*---------------------------------------------------------*/

void WorkspaceController::CheckRecoveryOffer()
{
    if(recovery_offered_ || session_.Dirty())
    {
        /* A live dirty draft's staged record is not a
           "recovery" — it IS the current work. */
        return;
    }
    QString id;
    if(managed_)
    {
        id = ws_id_;
    }
    else if(!available_)
    {
        /* No workspace surface at all (missing/legacy
           engine) — the only honest offer is whatever record
           exists; the draft is workspace-agnostic until the
           user opens a target. */
        const QStringList ids = drafts_.recordedIds();
        if(!ids.isEmpty())
        {
            id = ids.first();
        }
    }
    else
    {
        /* A capable engine whose managed state isn't resolved
           yet (or that manages nothing): never offer a
           workspace's record into the WRONG or empty session —
           a foreign draft staged here would commit as this
           workspace's content on the next save. */
        return;
    }
    WorkspaceDraftRecord rec;
    if(!id.isEmpty())
    {
        rec = drafts_.Read(id);
    }
    if(!rec.exists)
    {
        if(!rec.error.isEmpty())
        {
            SetError(rec.error, QStringLiteral("input"));
        }
        return;
    }
    offered_ws_id_    = id;
    recovery_offered_ = true;
    /* "Stale" when the engine moved past the revisions the
       draft was edited against — or when the engine is absent
       but the record names a baseline it can no longer
       confirm. */
    recovery_stale_ =
        !managed_ ||
        rec.baseline_revision != ws_rev_ ||
        rec.resource_revision != res_rev_;
    emit recoveryChanged();
    emit notice(QStringLiteral("An unsaved draft for "
                               "workspace '%1' was recovered "
                               "from disk").arg(id));
}

void WorkspaceController::recoverDraft()
{
    if(!recovery_offered_)
    {
        return;
    }
    if(managed_ && offered_ws_id_ != ws_id_)
    {
        /* Defense-in-depth: re-check the offer against the
           live managed id — a foreign record must never stage
           into this session (its document would commit as
           this workspace's content). */
        SetError(QStringLiteral("the recovered draft belongs "
                                "to a different workspace"),
                 QStringLiteral("input"));
        return;
    }
    const WorkspaceDraftRecord rec = drafts_.Read(offered_ws_id_);
    studio::StudioDocument d;
    std::vector<std::string> errs;
    if(!rec.exists ||
       !studio::FromJson(rec.document, d, &errs))
    {
        SetError(QStringLiteral("draft record could not be "
                                "restored"), QStringLiteral("input"));
        return;
    }
    /* Stage the draft locally — the engine sees NOTHING until
       the user explicitly saves. */
    session_.LoadDraft(d);
    drafts_.Discard(offered_ws_id_);
    recovery_offered_ = false;
    recovery_stale_   = false;
    emit recoveryChanged();
    emit draftEdited();
    if(!ready_cached_)
    {
        ready_cached_ = true;
        emit draftChanged();
    }
    emit cameraChanged();
    emit renderPrefsChanged();
    NoteDraftEdited();
}

void WorkspaceController::discardDraft()
{
    if(!offered_ws_id_.isEmpty())
    {
        drafts_.Discard(offered_ws_id_);
    }
    recovery_offered_ = false;
    recovery_stale_   = false;
    offered_ws_id_.clear();
    emit recoveryChanged();
}

/*---------------------------------------------------------*\
|| user actions                                              ||
\*---------------------------------------------------------*/

void WorkspaceController::save()
{
    if(!available_ || !bridge_->connected())
    {
        SetError(QStringLiteral("engine unavailable — the "
                                "draft stays local"),
                 QStringLiteral("engine"));
        emit saveFinished(false);
        return;
    }
    if(!managed_)
    {
        /* A LIVE engine that manages nothing is not
           "unavailable" — the draft needs its workspace
           re-opened, not a reconnect. */
        SetError(ws_id_.isEmpty()
            ? QStringLiteral("no workspace is open on the "
                             "engine — open one to continue; "
                             "your draft stays local")
            : QStringLiteral("no workspace is open on the "
                             "engine — reopen '%1' to continue; "
                             "your draft stays local")
                  .arg(ws_id_),
            QStringLiteral("engine"));
        emit saveFinished(false);
        return;
    }
    if(!session_.HasWorking() || saving_ || !session_.Dirty())
    {
        return;
    }
    /* Snapshot what is being committed NOW: adoption later
       uses THESE bytes, and the reconnect reconcile hashes
       them — edits that land during the save are never part
       of this snapshot. */
    pending_doc_       = studio::ToJson(session_.Working());
    pending_doc_sha_   = Sha256Hex(pending_doc_.dump(2));
    SetSaving(true);
    SetError(QString(), QString());
    const int id = SendCommand(
        QStringLiteral("commit"),
        QVariantMap({
            { QStringLiteral("id"),                ws_id_ },
            { QStringLiteral("base_revision"),
              QVariant::fromValue<qulonglong>(
                  session_.BaseRevision()) },
            { QStringLiteral("resource_revision"),
              QVariant::fromValue<qulonglong>(
                  session_.ResourceRevision()) },
            { QStringLiteral("document"),
              JsonToVariant(pending_doc_).toMap() },
        }),
        QStringLiteral("job:commit"));
    if(id == 0)
    {
        SetSaving(false);
        pending_doc_     = nullptr;
        pending_doc_sha_ = QString();
        SetError(QStringLiteral("save request could not be "
                                "sent"), QStringLiteral("connection"));
        emit saveFinished(false);
    }
}

void WorkspaceController::saveCopy(const QString& name)
{
    if(!available_ || !bridge_->connected() || !managed_ ||
       !session_.HasWorking() || saving_)
    {
        return;
    }
    nlohmann::json doc = studio::ToJson(session_.Working());
    doc["name"]        = name.toStdString();
    pending_doc_       = doc;
    pending_doc_sha_   = Sha256Hex(doc.dump(2));
    pending_save_name_ = name;
    SetSaving(true);
    /* save_copy asserts the TARGET does not exist — the
       base revisions the session knows belong to the SOURCE
       workspace; the store's gate compares against the
       target marker, so a copy to a fresh name must say 0
       (an existing id refuses honestly with
       revision_conflict — a copy never silently
       overwrites). */
    const int id = SendCommand(
        QStringLiteral("save_copy"),
        QVariantMap({
            { QStringLiteral("id"),   SlugFor(name) },
            { QStringLiteral("name"), name },
            { QStringLiteral("base_revision"),
              QVariant::fromValue<qulonglong>(0) },
            { QStringLiteral("resource_revision"),
              QVariant::fromValue<qulonglong>(0) },
            { QStringLiteral("document"),
              JsonToVariant(doc).toMap() },
        }),
        QStringLiteral("job:save_copy"));
    if(id == 0)
    {
        SetSaving(false);
        pending_doc_ = nullptr;
        emit saveFinished(false);
    }
}

void WorkspaceController::openWorkspace(const QString& id)
{
    if(!available_ || !bridge_->connected() || id.isEmpty() ||
       id == ws_id_)
    {
        return;
    }
    /* Task 10: a dirty draft is never replaced silently — the
       switch latches for confirmation first (unless the
       conflict strip already presented the choice). The draft
       record stays staged either way — switching never loses
       unsaved work. */
    if(session_.Dirty() && !conflict_)
    {
        OfferReplace(QStringLiteral("reload"), id,
            QStringLiteral(
                "Open workspace '%1'? Source: the engine's "
                "managed copy of '%1'. Destination: this "
                "editing session — your unsaved edits to '%2' "
                "would be replaced (they stay in the draft "
                "store, recoverable until you discard).")
                .arg(id).arg(ws_id_));
        return;
    }
    DoReload(id);
}

void WorkspaceController::reload()
{
    if(!available_ || !bridge_->connected())
    {
        SetError(QStringLiteral("engine unavailable"),
                 QStringLiteral("engine"));
        return;
    }
    if(!managed_)
    {
        /* Connected but unmanaged — a reopen is the remedy,
           not a reconnect. */
        SetError(ws_id_.isEmpty()
            ? QStringLiteral("no workspace is open on the "
                             "engine — open one to continue")
            : QStringLiteral("no workspace is open on the "
                             "engine — reopen '%1' to continue")
                  .arg(ws_id_),
            QStringLiteral("engine"));
        return;
    }
    /* Dirty draft + no latched conflict: confirm before the
       disk copy replaces pending edits (Task 10). */
    if(session_.Dirty() && !conflict_)
    {
        OfferReplace(QStringLiteral("reload"), QString(),
            QStringLiteral(
                "Reload '%1' from disk? Source: the engine's "
                "accepted generation (revision %2)%3. "
                "Destination: this editing session — your "
                "unsaved edits would be replaced (they stay in "
                "the draft store until you discard).")
                .arg(ws_id_)
                .arg(ws_rev_)
                .arg(external_state_.isEmpty()
                         ? QString()
                         : QStringLiteral(" — the on-disk files "
                                          "reported %1")
                               .arg(external_state_)));
        return;
    }
    DoReload(QString());
}

void WorkspaceController::DoReload(const QString& id)
{
    QVariantMap args;
    if(!id.isEmpty())
    {
        args.insert(QStringLiteral("id"), id);
    }
    /* Revision-checked reload: the engine refuses when the
       marker moved past what we last saw (Task 10). For a
       foreign target id the revisions aren't ours to claim —
       the engine resolves that read on its own truth. */
    if(id.isEmpty() || id == ws_id_)
    {
        pending_switch_id_.clear();
        args.insert(QStringLiteral("base_revision"),
                    QVariant::fromValue<qulonglong>(ws_rev_));
        args.insert(QStringLiteral("resource_revision"),
                    QVariant::fromValue<qulonglong>(res_rev_));
    }
    else
    {
        /* Foreign open: the job event can resolve BEFORE the
           workspace snapshot renames ws_id_ — the fetch must
           wait for the snapshot (ApplySnapshot's ws_changed
           path) so the old workspace's document is never
           downloaded, adopted, or mistaken for clean state. */
        pending_switch_id_ = id;
    }
    force_adopt_ = true;
    /* Invalidate any document fetch already in flight — its
       bytes predate the confirmed replace and must never
       adopt (it would Discard the staged draft record). */
    ++fetch_epoch_;
    if(SendCommand(QStringLiteral("reload"), args,
                   QStringLiteral("job:reload")) <= 0)
    {
        /* No job in flight — a latched force_adopt_ would
           wholesale-adopt the next fetch over a dirty draft,
           and a foreign-open pending_switch_id_ would gate
           FetchDocument until disconnect. Clear both. */
        force_adopt_ = false;
        pending_switch_id_.clear();
        SetError(QStringLiteral("reload request could not be "
                                "sent"), QStringLiteral("connection"));
    }
}

void WorkspaceController::restoreBackup()
{
    if(!available_ || !bridge_->connected())
    {
        SetError(QStringLiteral("engine unavailable"),
                 QStringLiteral("engine"));
        return;
    }
    if(!managed_)
    {
        SetError(ws_id_.isEmpty()
            ? QStringLiteral("no workspace is open on the "
                             "engine — open one to continue")
            : QStringLiteral("no workspace is open on the "
                             "engine — reopen '%1' to continue")
                  .arg(ws_id_),
            QStringLiteral("engine"));
        return;
    }
    if(session_.Dirty() && !conflict_)
    {
        OfferReplace(QStringLiteral("restore_backup"), QString(),
            QStringLiteral(
                "Restore Backup for '%1'? Source: the marker "
                "backup's last-valid generation. Destination: a "
                "NEW committed generation — the current one "
                "stays in history; your unsaved edits would be "
                "replaced (they stay in the draft store).")
                .arg(ws_id_));
        return;
    }
    DoRestoreBackup();
}

void WorkspaceController::DoRestoreBackup()
{
    /* Same invalidation as DoReload — a fetch that began
       before the confirmed restore must not adopt. */
    ++fetch_epoch_;
    const int req = SendCommand(
        QStringLiteral("restore_backup"),
        QVariantMap({
            { QStringLiteral("id"),                ws_id_ },
            { QStringLiteral("base_revision"),
              QVariant::fromValue<qulonglong>(ws_rev_) },
            { QStringLiteral("resource_revision"),
              QVariant::fromValue<qulonglong>(res_rev_) },
        }),
        QStringLiteral("job:restore_backup"));
    if(req <= 0)
    {
        SetError(QStringLiteral("restore request could not be "
                                "sent"), QStringLiteral("connection"));
    }
}

void WorkspaceController::confirmReplace()
{
    if(!pending_replace_)
    {
        return;
    }
    const PendingReplace pr = *pending_replace_;
    pending_replace_.reset();
    emit replaceChanged();
    if(pr.action == QLatin1String("restore_backup"))
    {
        DoRestoreBackup();
    }
    else
    {
        DoReload(pr.id);
    }
}

void WorkspaceController::cancelReplace()
{
    if(!pending_replace_)
    {
        return;
    }
    pending_replace_.reset();
    emit replaceChanged();
    SetStatus(QStringLiteral("Kept your draft — nothing was "
                             "replaced"));
}

void WorkspaceController::openWorkspaceFolder()
{
    const QString dir = !ws_dir_.isEmpty() ? ws_dir_ : ws_root_;
    if(dir.isEmpty())
    {
        SetError(QStringLiteral("the engine did not report a "
                                "workspace folder"),
                 QStringLiteral("engine"));
        return;
    }
    /* UI-side only — the engine never spawns a shell. */
    QDesktopServices::openUrl(QUrl::fromLocalFile(dir));
}

void WorkspaceController::resetEdits()
{
    session_.Discard();
    SetConflict(false);
    emit draftEdited();
    NoteDraftEdited();
}

void WorkspaceController::undo()
{
    if(session_.Undo())
    {
        emit draftEdited();
        NoteDraftEdited();
    }
}

void WorkspaceController::redo()
{
    if(session_.Redo())
    {
        emit draftEdited();
        NoteDraftEdited();
    }
}

void WorkspaceController::exportBundle()
{
    /* Task 10 edge: a silent return left the dialog in
       "exporting" forever — refusals surface through error,
       which the dialog's onErrorChanged unlatches. */
    if(!available_ || !managed_ || !bridge_->connected())
    {
        SetError(QStringLiteral("no managed workspace to "
                                "export"), QStringLiteral("engine"));
        return;
    }
    if(SendCommand(QStringLiteral("export"), {},
                   QStringLiteral("job:export")) <= 0)
    {
        SetError(QStringLiteral("export request could not be "
                                "sent"), QStringLiteral("connection"));
    }
}

void WorkspaceController::importDocument(const QString& name,
                                         const QByteArray& bytes)
{
    if(bytes.isEmpty())
    {
        emit importFinished(name, false,
            QStringLiteral("empty import document"));
        return;
    }
    import_queue_.push_back({ name, bytes });
    PumpImportQueue();
}

void WorkspaceController::inspectImport(const QString& name,
                                        const QString& bytesBase64)
{
    auto bad = [this](const QString& why)
    {
        emit importInspectionReady(QVariantMap({
            { QStringLiteral("valid"),  false },
            { QStringLiteral("errors"), QStringList{ why } },
        }));
    };
    /* The dialog hands over readLocalFile's base64 TEXT — the
       wire upload carries the decoded bundle bytes, never the
       twice-encoded form (the engine parses studio.json
       directly). */
    const QByteArray bytes =
        QByteArray::fromBase64(bytesBase64.toLatin1());
    /* Every accepted form is a JSON object (a bare compact
       document or the packed bundle map) — the cheap shape
       check keeps undecodable/garbage input honest before the
       upload round-trip. */
    if(bytes.isEmpty() ||
       bytes.trimmed().isEmpty() ||
       bytes.trimmed().front() != '{')
    {
        bad(QStringLiteral("file did not decode to a workspace"
                           " bundle (base64 JSON expected)"));
        return;
    }
    if(!available_)
    {
        bad(QStringLiteral("engine not connected"));
        return;
    }
    pending_inspect_name_ = name;
    /* A replaced pending token is never consumed — release it
       instead of leaving it to idle-expire. */
    ReleaseStagedToken(pending_inspect_token_);
    pending_inspect_token_.clear();
    if(!BeginUpload(QStringLiteral("inspect:") + name, bytes,
                    QStringLiteral("import_inspect"),
                    QVariantMap()))
    {
        bad(ul_.has_value()
            ? QStringLiteral("another transfer is in flight")
            : QStringLiteral("upload could not begin"));
    }
}

void WorkspaceController::confirmImport(const QVariantMap& choices,
                                        const QString& mode)
{
    if(pending_inspect_token_.isEmpty())
    {
        emit importFinished(pending_inspect_name_, false,
            QStringLiteral("no staged import to confirm"));
        return;
    }
    const QString name = pending_inspect_name_;
    /* mode selects the target/verb semantics:
         ""              → a NEW workspace slugged from the file
                           name (never adopted, never applied);
         "current"       → the imported document commits onto
                           the ACTIVE workspace (the dialog's
                           replace-my-layout) — gated on the
                           live revisions, still never applied;
         "preset_import" → resources only, gated on the active
                           workspace's resource scope (the mode
                           is forwarded to the engine). */
    const bool preset_only = (mode == QLatin1String("preset_import"));
    const bool into_current =
        (mode == QLatin1String("current") && managed_);
    QString id;
    if(into_current || (preset_only && managed_))
    {
        id = ws_id_;
    }
    else
    {
        id = SlugFor(name);
        /* Task 10 edge: empty mode must NEVER collide with an
           existing workspace slug — an accidental hit would
           read as an overwrite of the open id (revision-gated
           but confusing). Suffix until free; the engine's
           revision check stays the authority for anything the
           cached list missed. */
        auto taken = [this](const QString& c) -> bool
        {
            if(c == ws_id_)
            {
                return true;
            }
            for(const QVariant& v : ws_list_)
            {
                if(v.toString() == c)
                {
                    return true;
                }
            }
            return false;
        };
        if(taken(id))
        {
            for(int n = 2; ; ++n)
            {
                const QString cand =
                    id + QLatin1Char('-') + QString::number(n);
                if(!taken(cand))
                {
                    id = cand;
                    break;
                }
            }
        }
    }
    /* The revisions gate on the TARGET workspace — a fresh id
       expects 0/0; importing over/inside the open workspace
       passes its known revisions (a stale pair fails
       revision_conflict — honest, never clobbered). */
    const bool target_is_current = (id == ws_id_);
    QVariantMap args({
        { QStringLiteral("staged"), pending_inspect_token_ },
        { QStringLiteral("id"),     id },
        { QStringLiteral("name"),   name },
        { QStringLiteral("base_revision"),
          QVariant::fromValue<qulonglong>(
              target_is_current ? ws_rev_ : 0) },
        { QStringLiteral("resource_revision"),
          QVariant::fromValue<qulonglong>(
              target_is_current ? res_rev_ : 0) },
    });
    if(!choices.isEmpty())
    {
        args.insert(QStringLiteral("choices"), choices);
    }
    if(preset_only)
    {
        args.insert(QStringLiteral("mode"),
                    QStringLiteral("preset_import"));
    }
    const int req = SendCommand(QStringLiteral("import_commit"),
                                args,
                                QStringLiteral("job:import_commit"));
    if(req <= 0)
    {
        emit importFinished(name, false,
            QStringLiteral("commit could not be sent"));
        /* Queued entries still owe a terminal signal — keep
           the queue moving exactly like a refused accept. */
        PumpImportQueue();
        return;
    }
    import_req_names_[req] = name;
    pending_inspect_token_.clear();
}

void WorkspaceController::cancelImport()
{
    /* The staged bundle will never be consumed — release the
       token now instead of leaving it to idle-expire. */
    ReleaseStagedToken(pending_inspect_token_);
    pending_inspect_token_.clear();
    pending_inspect_name_.clear();
}

QVariantMap WorkspaceController::readLocalFile(const QString& path)
{
    QVariantMap out;
    if(path.isEmpty())
    {
        out["ok"]    = false;
        out["error"] = QStringLiteral("no path given");
        return out;
    }
    QFile f(QUrl(path).isValid() && path.startsWith(QLatin1String("file:"))
                ? QUrl(path).toLocalFile() : path);
    if(!f.open(QIODevice::ReadOnly))
    {
        out["ok"]    = false;
        out["error"] = f.errorString();
        return out;
    }
    /* The bundle ceiling — a bigger file can never be a
       portable workspace anyway. */
    constexpr qint64 kCap = 64ll * 1024 * 1024;
    if(f.size() > kCap)
    {
        out["ok"]    = false;
        out["error"] = QStringLiteral("file exceeds the 64 MiB "
                                      "bundle bound");
        return out;
    }
    const QByteArray bytes = f.readAll();
    out["ok"]     = true;
    out["base64"] = QString::fromLatin1(bytes.toBase64());
    return out;
}

QVariantMap WorkspaceController::writeLocalFile(
    const QString& path, const QString& bytesBase64)
{
    QVariantMap out;
    if(path.isEmpty())
    {
        out["ok"]    = false;
        out["error"] = QStringLiteral("no path given");
        return out;
    }
    const QString local =
        path.startsWith(QLatin1String("file:"))
            ? QUrl(path).toLocalFile() : path;
    if(QFile::exists(local))
    {
        out["ok"]    = false;
        out["error"] = QStringLiteral(
            "file exists — never overwriting");
        return out;
    }
    QFile f(local);
    if(!f.open(QIODevice::WriteOnly | QIODevice::NewOnly))
    {
        out["ok"]    = false;
        out["error"] = f.errorString();
        return out;
    }
    const QByteArray bytes =
        QByteArray::fromBase64(bytesBase64.toLatin1());
    if(f.write(bytes) != bytes.size())
    {
        out["ok"]    = false;
        out["error"] = QStringLiteral("write failed");
        return out;
    }
    out["ok"] = true;
    return out;
}

int WorkspaceController::importProfiles(ProfileLibrary* library)
{
    if(!library)
    {
        return 0;
    }
    int queued = 0;
    const QVariantList entries = library->entries();
    for(const QVariant& e : entries)
    {
        const QString name =
            e.toMap().value(QStringLiteral("name")).toString();
        const QVariantMap doc = library->profileDocument(name);
        if(name.isEmpty() || doc.isEmpty())
        {
            continue;
        }
        import_queue_.push_back({
            name,
            QByteArray::fromStdString(
                VariantToJson(doc).dump(2)) });
        ++queued;
    }
    PumpImportQueue();
    return queued;
}

void WorkspaceController::PumpImportQueue()
{
    while(!import_queue_.empty() && !ul_.has_value())
    {
        PendingImport imp = import_queue_.front();
        import_queue_.pop_front();
        if(BeginUpload(QStringLiteral("import:") + imp.name,
                       imp.bytes,
                       QStringLiteral("import_commit"),
                       QVariantMap({
                           { QStringLiteral("id"),
                             SlugFor(imp.name) },
                           { QStringLiteral("name"), imp.name },
                       })))
        {
            return;
        }
        /* Refused before a byte moved — the entry still owes
           the UI a terminal signal; drain the rest honestly
           instead of dropping it silent. */
        emit importFinished(imp.name, false,
            available_
                ? QStringLiteral("upload could not begin")
                : QStringLiteral("engine not connected"));
    }
    SettleImportStatus();
}

void WorkspaceController::SettleImportStatus()
{
    if(status_text_ != QStringLiteral("Importing workspace…"))
    {
        return;
    }
    if(!import_queue_.empty() || ul_.has_value() ||
       !import_req_names_.empty() || !import_names_.empty())
    {
        return;
    }
    SetStatus(session_.Dirty()
        ? QStringLiteral("Unsaved changes")
        : QStringLiteral("In sync with engine"));
}

void WorkspaceController::ReleaseStagedToken(const QString& token)
{
    if(token.isEmpty())
    {
        return;
    }
    SendCommand(QStringLiteral("transfer_cancel"),
        QVariantMap({{ QStringLiteral("transfer_id"), token }}),
        QStringLiteral("misc"));
}

void WorkspaceController::refreshList()
{
    if(!available_)
    {
        return;
    }
    SendCommand(QStringLiteral("list"),
                QVariantMap({{ QStringLiteral("limit"), 256 }}),
                QStringLiteral("list"));
}

void WorkspaceController::FlushDrafts()
{
    drafts_.Flush();
}

void WorkspaceController::NoteDraftEdited()
{
    const bool d = session_.Dirty();
    if(d != dirty_cached_)
    {
        dirty_cached_ = d;
        emit dirtyChanged();
    }
    emit historyChanged();
    const QString id = ws_id_.isEmpty()
        ? QStringLiteral("local") : ws_id_;
    if(d)
    {
        drafts_.Stage(id, session_.BaseRevision(),
                      session_.ResourceRevision(),
                      BaselineSha(),
                      studio::ToJson(session_.Working()));
        /* The record under `id` is now THIS session's live
           shadow — a clean transition may drop it. A record
           staged by a previous session (or a different id)
           is a recovery candidate: never silently dropped. */
        staged_id_ = id;
        SetStatus(QStringLiteral("Unsaved changes"));
    }
    else if(staged_id_ == id &&
            !(recovery_offered_ && offered_ws_id_ == id))
    {
        /* A clean draft drops its durable shadow — unless the
           record is the outstanding recovery offer, which is
           the user's call, never ours. */
        drafts_.Discard(id);
        staged_id_.clear();
        SetStatus(QStringLiteral("In sync with engine"));
    }
    else if(staged_id_ != id)
    {
        /* Clean, but the record on disk was NOT staged by
           this session — leave it for CheckRecoveryOffer. */
        SetStatus(QStringLiteral("In sync with engine"));
    }
}

/*---------------------------------------------------------*\
|| reply dispatch (request-id correlation)                   ||
\*---------------------------------------------------------*/

void WorkspaceController::OnReplyReceived(int id,
                                          const QString& type,
                                          const QVariantMap& body)
{
    const auto it = pending_reqs_.find(id);
    if(it == pending_reqs_.end())
    {
        return;
    }
    const QString tag = it->second;
    pending_reqs_.erase(it);
    const bool    ok  = type == QLatin1String("OK");
    const QString msg = ok ? QString()
        : body.value(QStringLiteral("message"),
                     body.value(QStringLiteral("error"))
                         .toString()).toString();

    if(tag == QLatin1String("get"))
    {
        if(ok)
        {
            ApplySnapshot(body);
        }
        else
        {
            SetError(msg.isEmpty()
                ? QStringLiteral("workspace get failed") : msg,
                QStringLiteral("engine"));
        }
    }
    else if(tag == QLatin1String("list"))
    {
        if(ok)
        {
            ws_list_ = body.value(QStringLiteral("workspaces"))
                           .toList();
            emit workspaceListChanged();
        }
    }
    else if(tag == QLatin1String("dl_begin") ||
            tag == QLatin1String("dl_chunk") ||
            tag == QLatin1String("dl_finish"))
    {
        OnDownloadReply(tag.mid(3), body, ok, msg);
    }
    else if(tag == QLatin1String("ul_begin") ||
            tag == QLatin1String("ul_chunk") ||
            tag == QLatin1String("ul_finish"))
    {
        OnUploadReply(tag.mid(3), body, ok, msg);
    }
    else if(tag.startsWith(QLatin1String("job:")))
    {
        OnJobReply(id, tag.mid(4), body, ok, msg);
    }
    else if(tag == QLatin1String("job_status"))
    {
        OnJobStatusReply(body, ok);
    }
}

void WorkspaceController::OnJobReply(int request_id,
                                     const QString& verb,
                                     const QVariantMap& body,
                                     bool ok,
                                     const QString& message)
{
    /* An import_commit answer pairs with the name recorded
       under THIS request id — replies are not guaranteed to
       land in submit order, so a FIFO could mislabel which
       entry finished. */
    QString import_name;
    if(verb == QLatin1String("import_commit"))
    {
        const auto it = import_req_names_.find(request_id);
        if(it != import_req_names_.end())
        {
            import_name = it->second;
            import_req_names_.erase(it);
        }
    }
    if(!ok)
    {
        if(verb == QLatin1String("commit") ||
           verb == QLatin1String("save_copy"))
        {
            SetSaving(false);
            pending_doc_     = nullptr;
            pending_doc_sha_ = QString();
            emit saveFinished(false);
        }
        else if(verb == QLatin1String("import_commit"))
        {
            /* A refused acceptance still releases the queue —
               the remaining entries are never abandoned. */
            emit importFinished(import_name, false, message);
            PumpImportQueue();
        }
        else if(verb == QLatin1String("reload") ||
                verb == QLatin1String("restore_backup"))
        {
            /* No job will ever resolve — a latched force_adopt_
               would wholesale-adopt the NEXT fetch over a dirty
               draft; a pending foreign-open target dies with
               its job too. */
            force_adopt_ = false;
            pending_switch_id_.clear();
        }
        SetError(message, QStringLiteral("engine"));
        return;
    }
    if(!body.value(QStringLiteral("accepted")).toBool())
    {
        /* Bounded queue — honest busy, never a silent drop. */
        if(verb == QLatin1String("commit") ||
           verb == QLatin1String("save_copy"))
        {
            SetSaving(false);
            pending_doc_     = nullptr;
            pending_doc_sha_ = QString();
            emit saveFinished(false);
        }
        else if(verb == QLatin1String("import_commit"))
        {
            emit importFinished(import_name, false,
                QStringLiteral("engine job queue is busy"));
            PumpImportQueue();
        }
        else if(verb == QLatin1String("reload") ||
                verb == QLatin1String("restore_backup"))
        {
            force_adopt_ = false;
            pending_switch_id_.clear();
        }
        SetError(QStringLiteral("engine job queue is busy — "
                                "try again"),
                 QStringLiteral("engine"));
        return;
    }
    const quint64 job = JU64(body, "job_id");
    if(verb == QLatin1String("commit") ||
       verb == QLatin1String("save_copy"))
    {
        pending_job_id_ = job;
    }
    else if(verb == QLatin1String("reload") ||
            verb == QLatin1String("restore_backup") ||
            verb == QLatin1String("import_inspect") ||
            verb == QLatin1String("import_commit") ||
            verb == QLatin1String("export"))
    {
        tracked_jobs_[job] = verb;
        if(verb == QLatin1String("import_commit"))
        {
            import_names_[job] = import_name;
            SetStatus(QStringLiteral("Importing workspace…"));
        }
    }
}

void WorkspaceController::OnJobStatusReply(const QVariantMap& body,
                                           bool ok)
{
    if(ok && body.value(QStringLiteral("found")).toBool())
    {
        const QString state =
            body.value(QStringLiteral("state")).toString();
        if(state == QLatin1String("succeeded") ||
           state == QLatin1String("failed") ||
           state == QLatin1String("cancelled"))
        {
            ResolveJob(body);
        }
    }
}

/*---------------------------------------------------------*\
|| job events + resolution                                   ||
\*---------------------------------------------------------*/

void WorkspaceController::OnJobEvent(const QVariantMap& ev)
{
    const quint64 job   = JU64(ev, "job_id");
    const QString state =
        ev.value(QStringLiteral("state")).toString();
    const bool terminal =
        state == QLatin1String("succeeded") ||
        state == QLatin1String("failed")    ||
        state == QLatin1String("cancelled");
    if(!terminal)
    {
        return;
    }
    /* Revision truth updates from terminal records — scoped to
       OUR managed workspace. The record carries the job's
       target id: a job a client (this one or another) ran on
       ANOTHER workspace reports that workspace's revisions —
       adopting them would corrupt the commit base of the
       workspace actually open here. Records without an id
       (older engines) keep the previous behavior. */
    const QString job_ws =
        ev.value(QStringLiteral("id")).toString();
    const bool foreign_ws = !job_ws.isEmpty() && job_ws != ws_id_;
    const quint64 ws  = JU64(ev, "workspace_revision");
    const quint64 res = JU64(ev, "resource_revision");
    const bool revs_moved = !foreign_ws &&
                            (ws > ws_rev_ || res > res_rev_);
    if(!foreign_ws)
    {
        if(ws > ws_rev_)   { ws_rev_ = ws; }
        if(res > res_rev_) { res_rev_ = res; }
    }

    if(pending_job_id_ != 0 && job == pending_job_id_)
    {
        pending_job_id_ = 0;
        ResolveJob(ev);
        return;
    }
    const auto it = tracked_jobs_.find(job);
    if(it != tracked_jobs_.end())
    {
        tracked_jobs_.erase(it);
        ResolveJob(ev);
        return;
    }
    /* A resource-only move (look/type install or remove) bumps
       resource_revision while the document bytes stand —
       rebase the draft's commit base silently instead of
       fetching + flagging a conflict (F07). */
    const QString ev_verb =
        ev.value(QStringLiteral("verb")).toString();
    if(revs_moved && managed_ &&
       (ev_verb == QLatin1String("resource_install") ||
        ev_verb == QLatin1String("resource_remove")))
    {
        session_.RebaseRevisions(ws, res);
        /* Re-stage the durable record with the rebased pair —
           a crash-recovered draft must not be born stale. */
        NoteDraftEdited();
        return;
    }
    /* Foreign job: a commit that is not ours moved the
       workspace — fetch the document; the session decides
       adopt vs preserve. */
    if(revs_moved && managed_ &&
       ev.value(QStringLiteral("committed")).toBool())
    {
        FetchDocument();
    }
}

void WorkspaceController::ResolveJob(const QVariantMap& st)
{
    const QString verb  =
        st.value(QStringLiteral("verb")).toString();
    const QString state =
        st.value(QStringLiteral("state")).toString();
    const quint64 ws  = JU64(st, "workspace_revision");
    const quint64 res = JU64(st, "resource_revision");
    const QString detail =
        st.value(QStringLiteral("detail")).toString();

    if(state != QLatin1String("succeeded"))
    {
        const QString err =
            st.value(QStringLiteral("error")).toString();
        if(verb == QLatin1String("commit") ||
           verb == QLatin1String("save_copy"))
        {
            SetSaving(false);
            pending_doc_     = nullptr;
            pending_doc_sha_ = QString();
            /* The draft is untouched — rejection preserves
               every pending edit. */
            if(detail == QLatin1String("revision_conflict"))
            {
                SetConflict(true,
                    QStringLiteral("The workspace changed on "
                                   "the engine before the save "
                                   "landed."));
            }
            SetError(err.isEmpty()
                ? QStringLiteral("save rejected by engine")
                : err, QStringLiteral("engine"));
            emit saveFinished(false);
        }
        else if(verb == QLatin1String("reload"))
        {
            force_adopt_ = false;
            pending_switch_id_.clear();
            SetLoading(false);
            SetError(err, QStringLiteral("engine"));
        }
        else if(verb == QLatin1String("restore_backup"))
        {
            SetError(err.isEmpty()
                         ? QStringLiteral("backup restore failed")
                         : err, QStringLiteral("engine"));
        }
        else if(verb == QLatin1String("import_inspect"))
        {
            /* The staged upload stays staged — the caller may
               still fix choices or retry; only the report
               carries the failure. */
            QVariantMap report;
            report[QStringLiteral("valid")]  = false;
            report[QStringLiteral("errors")] = QStringList{
                err.isEmpty() ? QStringLiteral(
                    "inspection failed") : err };
            emit importInspectionReady(report);
        }
        else if(verb == QLatin1String("import_commit"))
        {
            const quint64 j = JU64(st, "job_id");
            const QString iname = import_names_.count(j)
                                      ? import_names_[j]
                                      : QString();
            import_names_.erase(j);
            /* Report WHICH entry failed and keep the queue
               moving — one bad import never abandons the
               rest. */
            emit importFinished(iname, false, err);
            SetError(err, QStringLiteral("engine"));
            PumpImportQueue();
        }
        else if(verb == QLatin1String("export"))
        {
            SetLoading(false);
            SetError(err, QStringLiteral("engine"));
        }
        return;
    }

    if(verb == QLatin1String("commit"))
    {
        /* Own commit accepted: adopt the submitted document +
           the reported revisions as the new baseline. Edits
           made during the save stay pending — adoption only
           moves the baseline. */
        studio::StudioDocument d;
        std::vector<std::string> errs;
        if(studio::FromJson(pending_doc_, d, &errs))
        {
            session_.AdoptCommitted(d, ws, res);
            baseline_sha_.clear();
        }
        pending_doc_     = nullptr;
        pending_doc_sha_ = QString();
        SetSaving(false);
        SetConflict(false);
        SetError(QString(), QString());
        emit saveFinished(true);
        emit draftEdited();
        NoteDraftEdited();
    }
    else if(verb == QLatin1String("save_copy"))
    {
        SetSaving(false);
        SetError(QString(), QString());
        emit notice(QStringLiteral("Saved a copy as '%1'")
                        .arg(pending_save_name_));
        pending_doc_     = nullptr;
        pending_doc_sha_ = QString();
        emit saveFinished(true);
    }
    else if(verb == QLatin1String("reload"))
    {
        /* A landed reload resolves whatever the strip last
           showed (a failed save's "stale revision" must not
           survive the fresh document it fetched — F07). */
        SetError(QString(), QString());
        if(!pending_switch_id_.isEmpty() &&
           pending_switch_id_ != ws_id_)
        {
            /* Foreign open: the terminal event beat the
               workspace snapshot — fetch NOTHING under the
               stale id. A `get` forces the snapshot (whose
               ws_changed path fetches the new id); the
               passive poll is the backstop. */
            SendCommand(QStringLiteral("get"), {},
                        QStringLiteral("get"));
        }
        else
        {
            pending_switch_id_.clear();
            /* A reload can move the published revision BACKWARD
               — e.g. the store recovered a tampered active
               generation and adopted the last-valid one. The
               stale-echo guard in ApplySnapshot drops ws <
               ws_rev_ forever, so re-baseline to the job's
               reported truth before the next poll arrives. */
            if(ws > 0 && ws < ws_rev_)
            {
                ws_rev_ = ws;
            }
            if(res > 0 && res < res_rev_)
            {
                res_rev_ = res;
            }
            force_adopt_ = true;
            FetchDocument();
        }
    }
    else if(verb == QLatin1String("restore_backup"))
    {
        /* The backup committed forward — adopt its document
           wholesale (the user confirmed the replace) and let
           the next poll re-baseline the external compare. */
        SetError(QString(), QString());
        force_adopt_ = true;
        FetchDocument();
        emit notice(QStringLiteral(
            "Restored the last-valid backup — committed as "
            "generation %1")
                .arg(JU64(st, "generation")));
    }
    else if(verb == QLatin1String("import_inspect"))
    {
        /* Task 9 — the report drives the import dialog's
           conflict choices; nothing was committed. */
        emit importInspectionReady(
            st.value(QStringLiteral("result")).toMap());
    }
    else if(verb == QLatin1String("import_commit"))
    {
        const quint64 j = JU64(st, "job_id");
        const QString iname = import_names_.count(j)
                                  ? import_names_[j]
                                  : QString();
        import_names_.erase(j);
        emit importFinished(iname, true, QString());
        emit notice(iname.isEmpty()
            ? QStringLiteral("Workspace imported")
            : QStringLiteral("Workspace '%1' imported")
                  .arg(iname));
        refreshList();
        PumpImportQueue();
    }
    else if(verb == QLatin1String("export"))
    {
        /* The bundle is staged for download — result carries
           the transfer token; read it through the bounded
           mechanism. */
        const QVariantMap result =
            st.value(QStringLiteral("result")).toMap();
        const QString token =
            result.value(QStringLiteral("transfer_id"))
                .toString();
        if(token.isEmpty())
        {
            SetError(QStringLiteral("export produced no "
                                    "download"),
                     QStringLiteral("engine"));
            return;
        }
        Download d;
        d.purpose     = QStringLiteral("export");
        d.transfer_id = token;
        d.length      = JU64(result, "bytes");
        d.sha256      = result.value(QStringLiteral("sha256"))
                            .toString();
        dl_ = d;
        SetLoading(true);
        ContinueDownload();
    }
}

/*---------------------------------------------------------*\
|| transfers                                                 ||
\*---------------------------------------------------------*/

bool WorkspaceController::BeginDownload(const QString& kind,
                                        const QString& id,
                                        const QString& purpose)
{
    (void)purpose;
    const int req = SendCommand(
        QStringLiteral("transfer_begin"),
        QVariantMap({
            { QStringLiteral("direction"), QStringLiteral("download") },
            { QStringLiteral("kind"),      kind },
            { QStringLiteral("id"),        id },
        }),
        QStringLiteral("dl_begin"));
    return req > 0;
}

bool WorkspaceController::ContinueDownload()
{
    if(!dl_.has_value())
    {
        return false;
    }
    const int id = SendCommand(
        QStringLiteral("transfer_chunk"),
        QVariantMap({
            { QStringLiteral("transfer_id"), dl_->transfer_id },
            { QStringLiteral("offset"),
              QVariant::fromValue<qulonglong>(dl_->received) },
            { QStringLiteral("max_bytes"),
              QVariant::fromValue<qulonglong>(
                  dl_->max_chunk > 0 ? dl_->max_chunk
                                     : (quint64)65536) },
        }),
        QStringLiteral("dl_chunk"));
    if(id == 0)
    {
        dl_.reset();
        SetLoading(false);
        return false;
    }
    return true;
}

void WorkspaceController::OnDownloadReply(const QString& stage,
                                          const QVariantMap& body,
                                          bool ok,
                                          const QString& message)
{
    if(stage == QLatin1String("begin"))
    {
        dl_requested_ = false;
        if(!ok)
        {
            SetLoading(false);
            SetError(message.isEmpty()
                ? QStringLiteral("download could not begin")
                : message, QStringLiteral("engine"));
            return;
        }
        if(dl_.has_value())
        {
            /* A second begin reply (queued get/snapshot
               duplicate) — the live download owns dl_. */
            return;
        }
        Download d;
        d.purpose     = QStringLiteral("document");
        d.transfer_id =
            body.value(QStringLiteral("transfer_id")).toString();
        d.length    = JU64(body, "length");
        d.sha256    = body.value(QStringLiteral("sha256"))
                          .toString();
        d.max_chunk = JU64(body, "max_chunk_bytes");
        /* The begin reply carries no id — stamp the request's
           workspace + epoch so a pre-switch fetch finishing
           late is dropped instead of adopting the OLD
           workspace's document over a confirmed replace. */
        d.workspace_id = dl_request_ws_;
        d.epoch        = dl_request_epoch_;
        dl_ = d;
        ContinueDownload();
        return;
    }
    if(stage == QLatin1String("chunk"))
    {
        if(!ok)
        {
            SetLoading(false);
            SetError(message.isEmpty()
                ? QStringLiteral("download failed")
                : message, QStringLiteral("engine"));
            dl_.reset();
            return;
        }
        if(!dl_.has_value())
        {
            return;      /* stale chunk after finish/abort */
        }
        const QByteArray chunk = QByteArray::fromBase64(
            body.value(QStringLiteral("data")).toString()
                .toLatin1());
        dl_->bytes.append(chunk);
        dl_->received += (quint64)chunk.size();
        if(body.value(QStringLiteral("eof")).toBool())
        {
            FinishDownload();
        }
        else
        {
            ContinueDownload();
        }
        return;
    }
    /* finish — release acknowledgement; nothing to do. */
    dl_.reset();
}

void WorkspaceController::FinishDownload()
{
    if(!dl_.has_value())
    {
        return;
    }
    Download d = *dl_;
    /* Bounded integrity: the staged byte count and hash must
       match before the document is even parsed. */
    if(d.length > 0 && (quint64)d.bytes.size() != d.length)
    {
        SetError(QStringLiteral("download length mismatch"),
                 QStringLiteral("engine"));
        dl_.reset();
        SetLoading(false);
        return;
    }
    if(!d.sha256.isEmpty() && Sha256Hex(d.bytes) != d.sha256)
    {
        SetError(QStringLiteral("download hash mismatch"),
                 QStringLiteral("engine"));
        dl_.reset();
        SetLoading(false);
        return;
    }
    /* Release the transfer server-side (fire and forget). */
    SendCommand(QStringLiteral("transfer_finish"),
        QVariantMap({{ QStringLiteral("transfer_id"),
                       d.transfer_id }}),
        QStringLiteral("dl_finish"));
    dl_.reset();
    SetLoading(false);
    if(d.purpose == QLatin1String("export"))
    {
        emit exportReady(
            ws_name_ + QStringLiteral(".workspace.json"),
            QString::fromLatin1(d.bytes.toBase64()));
        return;
    }
    /* Stale pre-switch/pre-replace fetch — its bytes are the
       document the user already moved past. Adopting would
       clobber the draft AND clean-Discard the durable record
       it was just staged into; drop it and fetch the doc the
       CURRENT snapshot actually names. */
    if(d.epoch != fetch_epoch_ ||
       (!d.workspace_id.isEmpty() && d.workspace_id != ws_id_))
    {
        FetchDocument();
        return;
    }
    AdoptFetchedDocument(d.bytes);
}

bool WorkspaceController::BeginUpload(const QString& purpose,
                                      const QByteArray& bytes,
                                      const QString& verb,
                                      const QVariantMap& args)
{
    if(ul_.has_value() || !available_)
    {
        return false;
    }
    Upload u;
    u.purpose = purpose;
    u.bytes   = bytes;
    u.verb    = verb;
    u.args    = args;
    ul_       = u;
    const int id = SendCommand(
        QStringLiteral("transfer_begin"),
        QVariantMap({
            { QStringLiteral("direction"), QStringLiteral("upload") },
            { QStringLiteral("kind"),      QStringLiteral("bundle") },
            { QStringLiteral("length"),
              QVariant::fromValue<qulonglong>(
                  (qulonglong)bytes.size()) },
            { QStringLiteral("sha256"),    Sha256Hex(bytes) },
        }),
        QStringLiteral("ul_begin"));
    if(id == 0)
    {
        ul_.reset();
        return false;
    }
    return true;
}

void WorkspaceController::OnUploadReply(const QString& stage,
                                        const QVariantMap& body,
                                        bool ok,
                                        const QString& message)
{
    if(!ul_.has_value())
    {
        return;
    }
    if(!ok)
    {
        const QString purpose = ul_->purpose;
        ul_.reset();
        SetError(message.isEmpty()
            ? QStringLiteral("upload failed")
            : message, QStringLiteral("engine"));
        if(purpose.startsWith(QLatin1String("import:")))
        {
            emit importFinished(purpose.mid(7), false,
                                error_);
            PumpImportQueue();
        }
        else if(purpose.startsWith(QLatin1String("inspect:")))
        {
            emit importInspectionReady(QVariantMap({
                { QStringLiteral("valid"),  false },
                { QStringLiteral("errors"),
                  QStringList{ error_.isEmpty()
                      ? QStringLiteral("upload failed")
                      : error_ } },
            }));
        }
        return;
    }
    if(stage == QLatin1String("begin"))
    {
        ul_->transfer_id =
            body.value(QStringLiteral("transfer_id")).toString();
        ul_->max_chunk = JU64(body, "max_chunk_bytes");
        if(ul_->max_chunk == 0)
        {
            ul_->max_chunk = 65536;
        }
    }
    else if(stage == QLatin1String("chunk"))
    {
        ul_->sent = JU64(body, "received");
        if(ul_->sent < (quint64)ul_->bytes.size())
        {
            /* fall through to the next chunk below */
        }
        else
        {
            SendCommand(QStringLiteral("transfer_finish"),
                QVariantMap({{ QStringLiteral("transfer_id"),
                               ul_->transfer_id }}),
                QStringLiteral("ul_finish"));
            return;
        }
    }
    else if(stage == QLatin1String("finish"))
    {
        const QString token =
            body.value(QStringLiteral("staged")).toString();
        Upload u = *ul_;
        ul_.reset();
        if(!token.isEmpty())
        {
            QVariantMap args = u.args;
            args.insert(QStringLiteral("staged"), token);
            /* Job-accept replies carry only the request id —
               the entry's name rides a request-keyed map so
               out-of-order accepts can never mislabel WHICH
               import finished or failed. */
            if(u.verb == QLatin1String("import_inspect"))
            {
                /* The staged token stays consumable for the
                   SAME session — confirmImport reuses it for
                   the commit (never re-uploads). */
                pending_inspect_token_ = token;
            }
            const int id = SendCommand(u.verb, args,
                QStringLiteral("job:") + u.verb);
            if(id <= 0)
            {
                /* No job will ever consume the staged token —
                   release it instead of letting it hold the
                   slot until idle expiry. */
                ReleaseStagedToken(token);
                if(u.verb == QLatin1String("import_commit"))
                {
                    emit importFinished(
                        u.args.value(QStringLiteral("name"))
                            .toString(),
                        false,
                        QStringLiteral("commit could not be "
                                       "sent"));
                }
                else if(u.verb == QLatin1String("import_inspect"))
                {
                    /* The dead token must not linger as the
                       pending commit source. */
                    pending_inspect_token_.clear();
                    emit importInspectionReady(QVariantMap({
                        { QStringLiteral("valid"),  false },
                        { QStringLiteral("errors"),
                          QStringList{ QStringLiteral(
                              "inspect job could not be sent") } },
                    }));
                }
                SetError(QStringLiteral("staged upload could "
                                        "not be committed"),
                         QStringLiteral("connection"));
            }
            else if(u.verb == QLatin1String("import_commit"))
            {
                import_req_names_[id] =
                    u.args.value(QStringLiteral("name"))
                        .toString();
            }
        }
        PumpImportQueue();
        return;
    }
    /* begin / chunk-in-progress → send the next bounded slice. */
    const quint64 off = ul_->sent;
    const quint64 n   = std::min<quint64>(
        ul_->max_chunk, (quint64)ul_->bytes.size() - off);
    const QByteArray slice = ul_->bytes.mid((qsizetype)off,
                                            (qsizetype)n);
    SendCommand(QStringLiteral("transfer_chunk"),
        QVariantMap({
            { QStringLiteral("transfer_id"), ul_->transfer_id },
            { QStringLiteral("offset"),
              QVariant::fromValue<qulonglong>(off) },
            { QStringLiteral("data"),
              QString::fromLatin1(slice.toBase64()) },
        }),
        QStringLiteral("ul_chunk"));
}

void WorkspaceController::AbortTransfers()
{
    if(dl_.has_value())
    {
        SendCommand(QStringLiteral("transfer_cancel"),
            QVariantMap({{ QStringLiteral("transfer_id"),
                           dl_->transfer_id }}),
            QStringLiteral("misc"));
    }
    if(ul_.has_value())
    {
        SendCommand(QStringLiteral("transfer_cancel"),
            QVariantMap({{ QStringLiteral("transfer_id"),
                           ul_->transfer_id }}),
            QStringLiteral("misc"));
    }
    dl_.reset();
    dl_requested_ = false;
    ul_.reset();
}

/*---------------------------------------------------------*\
|| test hooks                                                ||
\*---------------------------------------------------------*/

void WorkspaceController::setDraftDirForTesting(const QString& dir)
{
    drafts_.setRootForTesting(dir);
}

void WorkspaceController::setDebounceForTesting(int ms)
{
    drafts_.setDebounceForTesting(ms);
}

void WorkspaceController::PollPendingJobForTesting()
{
    if(pending_job_id_ == 0)
    {
        return;
    }
    SendCommand(QStringLiteral("job_status"),
        QVariantMap({{ QStringLiteral("job_id"),
                       QVariant::fromValue<qulonglong>(
                           pending_job_id_) }}),
        QStringLiteral("job_status"));
}

/*---------------------------------------------------------*\
|| viewport editor prefs (studio-import Task 8)              ||
||                                                           ||
||   meta.camera / meta.render / meta.ui are editor prefs   ||
||   inside the shared draft — SetMeta mutates them        ||
||   WITHOUT an undo record (the same rule the source       ||
||   SceneBridge::setCameraState documented); the change    ||
||   still rides the dirty + durable-draft path through      ||
||   NoteDraftEdited. With no draft the reads return the     ||
||   compact-document defaults and writes are refused.       ||
\*---------------------------------------------------------*/

QVariantMap WorkspaceController::cameraState() const
{
    studio::CameraPrefs c;                 /* compact defaults */
    std::string middle = "pan";
    if(session_.HasWorking())
    {
        c      = session_.Working().meta.camera;
        middle = session_.Working().meta.controls.middle_drag;
    }
    QVariantMap m;
    m[QStringLiteral("view")]       = QString::fromStdString(c.view);
    m[QStringLiteral("projection")] =
        QString::fromStdString(c.projection);
    m[QStringLiteral("tx")] = (double)c.target.x;
    m[QStringLiteral("ty")] = (double)c.target.y;
    m[QStringLiteral("tz")] = (double)c.target.z;
    m[QStringLiteral("yaw")]      = (double)c.yaw_deg;
    m[QStringLiteral("pitch")]    = (double)c.pitch_deg;
    m[QStringLiteral("distance")] = (double)c.distance;
    m[QStringLiteral("span")]     = (double)c.span;
    /* Rides along on the prefs read so the viewport's input
       router can honor middle_drag: pan|orbit without a second
       API — same contract as the source bridge. */
    m[QStringLiteral("middle_drag")] =
        QString::fromStdString(middle);
    return m;
}

void WorkspaceController::setCameraState(const QVariantMap& state)
{
    const bool changed = session_.SetMeta(
        [&state](studio::WorkspaceMeta& meta)
        {
            studio::CameraPrefs& c = meta.camera;
            if(state.contains(QStringLiteral("view")))
            {
                const QString v =
                    state[QStringLiteral("view")].toString();
                if(v == QLatin1String("desk")
                   || v == QLatin1String("top")
                   || v == QLatin1String("front")
                   || v == QLatin1String("case")
                   || v == QLatin1String("free"))
                {
                    c.view = v.toStdString();
                }
            }
            if(state.contains(QStringLiteral("projection")))
            {
                const QString v =
                    state[QStringLiteral("projection")].toString();
                if(v == QLatin1String("orthographic")
                   || v == QLatin1String("perspective"))
                {
                    c.projection = v.toStdString();
                }
            }
            const auto num = [&state](const char* k, float& dst)
            {
                const QString key = QString::fromLatin1(k);
                if(state.contains(key))
                {
                    bool ok = false;
                    const double v = state[key].toDouble(&ok);
                    if(ok && std::isfinite(v))
                    {
                        dst = (float)v;
                    }
                }
            };
            num("tx", c.target.x);
            num("ty", c.target.y);
            num("tz", c.target.z);
            num("yaw",      c.yaw_deg);
            num("pitch",    c.pitch_deg);
            num("distance", c.distance);
            num("span",     c.span);
            if(c.distance <= 0.0f) { c.distance = 1.21f; }
            if(c.span     <= 0.0f) { c.span     = 0.9f;  }
            return true;
        });
    if(changed)
    {
        emit cameraChanged();
        NoteDraftEdited();
    }
}

QString WorkspaceController::renderQuality() const
{
    return QString::fromStdString(
        session_.HasWorking()
            ? session_.Working().meta.render.quality
            : std::string("balanced"));
}

void WorkspaceController::setRenderQuality(const QString& q)
{
    if(q != QLatin1String("low") && q != QLatin1String("balanced")
       && q != QLatin1String("high"))
    {
        return;
    }
    const std::string want = q.toStdString();
    const bool changed = session_.SetMeta(
        [&want](studio::WorkspaceMeta& meta)
        {
            meta.render.quality = want;
            return true;
        });
    if(changed)
    {
        emit renderPrefsChanged();
        NoteDraftEdited();
    }
}

bool WorkspaceController::renderBloom() const
{
    return !session_.HasWorking()
           || session_.Working().meta.render.bloom;
}

void WorkspaceController::setRenderBloom(bool on)
{
    const bool changed = session_.SetMeta(
        [on](studio::WorkspaceMeta& meta)
        {
            meta.render.bloom = on;
            return true;
        });
    if(changed)
    {
        emit renderPrefsChanged();
        NoteDraftEdited();
    }
}

bool WorkspaceController::reducedMotion() const
{
    return session_.HasWorking()
           && session_.Working().meta.ui.reduced_motion;
}

void WorkspaceController::setReducedMotion(bool on)
{
    const bool changed = session_.SetMeta(
        [on](studio::WorkspaceMeta& meta)
        {
            meta.ui.reduced_motion = on;
            return true;
        });
    if(changed)
    {
        emit renderPrefsChanged();
        NoteDraftEdited();
    }
}

} /* namespace ui */
} /* namespace openrgb2 */
