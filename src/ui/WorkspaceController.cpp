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
#include <QMetaObject>

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
    while(!import_submit_names_.empty())
    {
        emit importFinished(import_submit_names_.front(),
                            false, lost);
        import_submit_names_.pop_front();
    }
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
        return;
    }

    /* Stale-echo guard: revisions the engine already passed
       are ignored outright — an out-of-order push can never
       roll the view back or clobber newer draft state. */
    if(id == ws_id_ && managed_ && ws < ws_rev_)
    {
        return;
    }

    const bool ws_changed  = id != ws_id_;
    const bool revs_moved  = ws_changed || ws != ws_rev_ ||
                             res != res_rev_;
    const bool sha_changed = sha != ws_sha256_;
    ws_id_   = id;
    ws_name_ = snap.value(QStringLiteral("name")).toString();
    ws_rev_  = ws;
    res_rev_ = res;
    ws_sha256_ = sha;
    if(!managed_ || ws_changed || revs_moved)
    {
        managed_ = true;
        emit workspaceChanged();
    }

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
    if(revs_moved || sha_changed)
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
    if(!available_ || ws_id_.isEmpty() || dl_.has_value() ||
       dl_requested_)
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
        dl_requested_ = true;
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
    if(!available_ || !bridge_->connected() || !managed_)
    {
        SetError(QStringLiteral("engine unavailable — the "
                                "draft stays local"),
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
    const int id = SendCommand(
        QStringLiteral("save_copy"),
        QVariantMap({
            { QStringLiteral("id"),   SlugFor(name) },
            { QStringLiteral("name"), name },
            { QStringLiteral("base_revision"),
              QVariant::fromValue<qulonglong>(
                  session_.BaseRevision()) },
            { QStringLiteral("resource_revision"),
              QVariant::fromValue<qulonglong>(
                  session_.ResourceRevision()) },
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
    /* The current draft record is staged already (debounced
       shadow) — switching never loses unsaved work. */
    force_adopt_ = true;
    if(SendCommand(QStringLiteral("reload"),
                   QVariantMap({{ QStringLiteral("id"), id }}),
                   QStringLiteral("job:reload")) <= 0)
    {
        /* No job in flight — a latched force_adopt_ would
           wholesale-adopt the next fetch over a dirty draft. */
        force_adopt_ = false;
        SetError(QStringLiteral("reload request could not be "
                                "sent"), QStringLiteral("connection"));
    }
}

void WorkspaceController::reload()
{
    if(!available_ || !bridge_->connected() || !managed_)
    {
        if(!available_)
        {
            SetError(QStringLiteral("engine unavailable"),
                     QStringLiteral("engine"));
        }
        return;
    }
    force_adopt_ = true;
    if(SendCommand(QStringLiteral("reload"), {},
                   QStringLiteral("job:reload")) <= 0)
    {
        force_adopt_ = false;
        SetError(QStringLiteral("reload request could not be "
                                "sent"), QStringLiteral("connection"));
    }
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
    if(!available_ || !managed_)
    {
        return;
    }
    SendCommand(QStringLiteral("export"), {},
                QStringLiteral("job:export"));
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
    if(import_queue_.empty() || ul_.has_value())
    {
        return;
    }
    PendingImport imp = import_queue_.front();
    import_queue_.pop_front();
    BeginUpload(QStringLiteral("import:") + imp.name,
                imp.bytes, QStringLiteral("import_commit"),
                QVariantMap({
                    { QStringLiteral("id"),   SlugFor(imp.name) },
                    { QStringLiteral("name"), imp.name },
                }));
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
        SetStatus(QStringLiteral("Unsaved changes"));
    }
    else if(!(recovery_offered_ && offered_ws_id_ == id))
    {
        /* A clean draft drops its durable shadow — unless the
           record is the outstanding recovery offer, which is
           the user's call, never ours. */
        drafts_.Discard(id);
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
        OnJobReply(tag.mid(4), body, ok, msg);
    }
    else if(tag == QLatin1String("job_status"))
    {
        OnJobStatusReply(body, ok);
    }
}

void WorkspaceController::OnJobReply(const QString& verb,
                                     const QVariantMap& body,
                                     bool ok,
                                     const QString& message)
{
    /* An import_commit answer pairs with the oldest queued
       submit name — the serial queue guarantees order. */
    QString import_name;
    if(verb == QLatin1String("import_commit") &&
       !import_submit_names_.empty())
    {
        import_name = import_submit_names_.front();
        import_submit_names_.pop_front();
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
        else if(verb == QLatin1String("reload"))
        {
            /* No job will ever resolve — a latched force_adopt_
               would wholesale-adopt the NEXT fetch over a dirty
               draft. */
            force_adopt_ = false;
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
        else if(verb == QLatin1String("reload"))
        {
            force_adopt_ = false;
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
    /* Revision truth updates from EVERY terminal record —
       including jobs other clients ran. */
    const quint64 ws  = JU64(ev, "workspace_revision");
    const quint64 res = JU64(ev, "resource_revision");
    const bool revs_moved = ws > ws_rev_ || res > res_rev_;
    if(ws > ws_rev_)   { ws_rev_ = ws; }
    if(res > res_rev_) { res_rev_ = res; }

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
            SetLoading(false);
            SetError(err, QStringLiteral("engine"));
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
        force_adopt_ = true;
        FetchDocument();
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
               the entry's name rides a FIFO keyed to the
               serial queue so the terminal event reports
               WHICH import finished or failed. */
            if(u.verb == QLatin1String("import_commit"))
            {
                import_submit_names_.push_back(
                    u.args.value(QStringLiteral("name"))
                        .toString());
            }
            const int id = SendCommand(u.verb, args,
                QStringLiteral("job:") + u.verb);
            if(id <= 0)
            {
                if(u.verb == QLatin1String("import_commit"))
                {
                    import_submit_names_.pop_back();
                    emit importFinished(
                        u.args.value(QStringLiteral("name"))
                            .toString(),
                        false,
                        QStringLiteral("commit could not be "
                                       "sent"));
                }
                SetError(QStringLiteral("staged upload could "
                                        "not be committed"),
                         QStringLiteral("connection"));
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

} /* namespace ui */
} /* namespace openrgb2 */
