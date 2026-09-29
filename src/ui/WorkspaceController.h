/*---------------------------------------------------------*\
|| WorkspaceController.h                                     ||
||                                                           ||
||   QObject front of the shared workspace draft          ||
||   (studio-import Task 4, plan sections 3.1/3.3): owns  ||
||   the WorkspaceEditSession the editing pages adapt to, ||
||   the WorkspaceDraftStore the unsaved-edit recovery    ||
||   comes from, and every engine correlation for the     ||
||   WORKSPACE_COMMAND verb space.                        ||
||                                                           ||
||   Correlation discipline (plan review focus #2):       ||
||   - REQUEST ids pair a workspaceCommand's OK/ERROR     ||
||     reply (job accepted is NOT a saved workspace)      ||
||   - JOB ids pair the terminal "workspace" EVENT        ||
||     carrying the JobStatus record                      ||
||   - REVISIONS + content_sha256 reconcile the accepted  ||
||     document — incl. across reconnect, where a commit  ||
||     that landed before the drop is adopted by hash and ||
||     a stale echo (older revisions) is ignored, never   ||
||     allowed to clobber newer work or clear a draft     ||
||                                                           ||
||   The controller performs NO file/engine work without  ||
||   an explicit action; a draft record is offered for    ||
||   recovery, never applied to the engine on its own.    ||
||                                                           ||
||   SPDX-License-Identifier: GPL-2.0-or-later               ||
||\*---------------------------------------------------------*/

#pragma once

#include "WorkspaceDraftStore.h"
#include "WorkspaceEditSession.h"

#include <QByteArray>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>

#include <cstdint>
#include <deque>
#include <map>
#include <optional>
#include <set>
#include <string>

namespace openrgb2
{
namespace ui
{

class EngineBridge;
class ProfileLibrary;

class WorkspaceController : public QObject
{
    Q_OBJECT

    /* The negotiated engine advertised studio_workspace_v1 —
       without it every managed action reports unavailable and
       the pages keep their legacy paths. */
    Q_PROPERTY(bool available READ available
                   NOTIFY availabilityChanged)
    /* A managed workspace is active on the engine. */
    Q_PROPERTY(bool managed READ managed NOTIFY workspaceChanged)
    /* The draft document is loaded and editable. */
    Q_PROPERTY(bool ready READ ready NOTIFY draftChanged)
    /* Document fetch/save job round-trip in flight. */
    Q_PROPERTY(bool loading READ loading NOTIFY loadingChanged)
    Q_PROPERTY(QString workspaceId READ workspaceId
                   NOTIFY workspaceChanged)
    Q_PROPERTY(QString workspaceName READ workspaceName
                   NOTIFY workspaceChanged)
    Q_PROPERTY(quint64 workspaceRevision READ workspaceRevision
                   NOTIFY workspaceChanged)
    Q_PROPERTY(quint64 resourceRevision READ resourceRevision
                   NOTIFY workspaceChanged)
    Q_PROPERTY(QVariantList workspaceList READ workspaceList
                   NOTIFY workspaceListChanged)

    Q_PROPERTY(bool dirty READ dirty NOTIFY dirtyChanged)
    /* The engine's accepted revision moved under pending
       edits (or a save lost a revision race) — surfaced for
       the user to resolve; nothing merges silently. */
    Q_PROPERTY(bool conflict READ conflict NOTIFY conflictChanged)
    Q_PROPERTY(bool saving READ saving NOTIFY savingChanged)
    Q_PROPERTY(QString error READ error NOTIFY errorChanged)
    /* "" | "input" | "engine" | "connection" — same honesty
       convention as the other controllers. */
    Q_PROPERTY(QString errorSource READ errorSource
                   NOTIFY errorChanged)
    Q_PROPERTY(bool canUndo READ canUndo NOTIFY historyChanged)
    Q_PROPERTY(bool canRedo READ canRedo NOTIFY historyChanged)
    Q_PROPERTY(QString undoLabel READ undoLabel
                   NOTIFY historyChanged)
    Q_PROPERTY(QString redoLabel READ redoLabel
                   NOTIFY historyChanged)

    /* A durable draft record waits for the user's pick —
       recoveryStale = its baseline revisions predate the
       engine's current ones. Offered, NEVER auto-applied. */
    Q_PROPERTY(bool recoveryOffered READ recoveryOffered
                   NOTIFY recoveryChanged)
    Q_PROPERTY(bool recoveryStale READ recoveryStale
                   NOTIFY recoveryChanged)
    Q_PROPERTY(QString statusText READ statusText
                   NOTIFY statusTextChanged)

public:
    explicit WorkspaceController(EngineBridge* bridge,
                                 QObject* parent = nullptr);

    /* The shared draft — adapters (SceneEditController,
       LayoutController, task-5/6 panels) edit through it. */
    WorkspaceEditSession&       Session()       { return session_; }
    const WorkspaceEditSession& Session() const { return session_; }
    WorkspaceDraftStore&        Drafts()        { return drafts_; }

    /*---- workspace actions (plan section 4) -----------------*/
    /* Commit the draft to the managed workspace — validate +
       persist + publish are engine-side job steps. */
    Q_INVOKABLE void save();
    /* Commit the draft as ANOTHER workspace (id slugged from
       `name`); the open workspace is untouched. */
    Q_INVOKABLE void saveCopy(const QString& name);
    /* Open a different managed workspace (reload {id}). The
       current draft is recorded to the draft store first so
       switching away never loses unsaved work. */
    Q_INVOKABLE void openWorkspace(const QString& id);
    /* Re-open the active workspace from disk (reload {}), then
       re-fetch the document — abandons draft edits + record. */
    Q_INVOKABLE void reload();
    /* Reset draft edits back to the accepted baseline. */
    Q_INVOKABLE void resetEdits();
    /* Adopt / drop the offered recovery record. Recovery only
       ever stages a LOCAL draft — nothing reaches the engine
       without an explicit save. */
    Q_INVOKABLE void recoverDraft();
    Q_INVOKABLE void discardDraft();
    Q_INVOKABLE void undo();
    Q_INVOKABLE void redo();

    /*---- jobs beyond the open workspace ----------------------*/
    /* Export the managed workspace's portable bundle —
       download lands in exportReady(name, base64). */
    Q_INVOKABLE void exportBundle();
    /* Import `bytes` (a compact document or a bundle) as a NEW
       managed workspace named `name` — the upload rides the
       bounded transfer path; never auto-applied (engine-side
       sanitize + no adopt). */
    Q_INVOKABLE void importDocument(const QString& name,
                                    const QByteArray& bytes);
    /* Every named entry of the UI profile library, imported as
       its own managed workspace — originals are retained
       (nothing is deleted or renamed). */
    Q_INVOKABLE int  importProfiles(ProfileLibrary* library);
    /* Refresh the managed workspace list (list verb). */
    Q_INVOKABLE void refreshList();

    /* Normal-close hook: flush debounced draft records. */
    void FlushDrafts();

    /* Adapters call this after mutating Session() — updates
       dirty/history notifications and stages the debounced
       durable record. */
    void NoteDraftEdited();

    /*---- state ------------------------------------------------*/
    bool        available() const;
    bool        managed() const;
    bool        ready() const;
    bool        loading() const;
    QString     workspaceId() const;
    QString     workspaceName() const;
    quint64     workspaceRevision() const;
    quint64     resourceRevision() const;
    QVariantList workspaceList() const;
    bool        dirty() const;
    bool        conflict() const;
    bool        saving() const;
    QString     error() const;
    QString     errorSource() const;
    bool        canUndo() const;
    bool        canRedo() const;
    QString     undoLabel() const;
    QString     redoLabel() const;
    bool        recoveryOffered() const;
    bool        recoveryStale() const;
    QString     statusText() const;

    /*---- test hooks --------------------------------------------*/
    void setDraftDirForTesting(const QString& dir);
    void setDebounceForTesting(int ms);
    int  PendingJobIdForTesting() const { return pending_job_id_; }
    /* Poll job_status for the tracked pending job — the
       lost-event reconcile path. */
    void PollPendingJobForTesting();

signals:
    void availabilityChanged();
    void workspaceChanged();
    void workspaceListChanged();
    void loadingChanged();
    /* Draft content mutated (edit/undo/redo/discard/recover) —
       adapters repaint from Session(). */
    void draftEdited();
    /* ready transition — the document draft exists / cleared. */
    void draftChanged();
    void dirtyChanged();
    void conflictChanged();
    void savingChanged();
    void errorChanged();
    void historyChanged();
    void recoveryChanged();
    void statusTextChanged();
    /* The engine's copy moved under us (another client or a
       reopen); the banner text explains the resolution set. */
    void notice(const QString& text);
    void exportReady(const QString& suggestedName,
                     const QString& bytesBase64);
    void importFinished(const QString& name, bool ok,
                        const QString& error);
    /* save() completed — `ok` distinguishes committed from
       refused so pages can surface honestly. */
    void saveFinished(bool ok);

private:
    /*---- bridge feeds ---------------------------------------*/
    void OnAvailabilityChanged();
    void OnWorkspaceChanged();   /* bridge workspace store moved */
    void OnReplyReceived(int id, const QString& type,
                         const QVariantMap& body);
    void OnEngineLost(const QString& reason);
    void OnConnectionStateChanged();

    /*---- snapshot/state application -------------------------*/
    void ApplySnapshot(const QVariantMap& snap);
    void ReconcileAfterReconnect(const QVariantMap& snap);
    void FetchDocument();
    void AdoptSnapshotDoc(const studio::StudioDocument& doc,
                          uint64_t ws_rev, uint64_t res_rev);
    void CheckRecoveryOffer();
    void PumpImportQueue();

    /*---- job bookkeeping ------------------------------------*/
    void OnJobEvent(const QVariantMap& ev);
    void ResolveJob(const QVariantMap& st);
    void SetError(const QString& e, const QString& source);
    void SetStatus(const QString& s);
    void SetConflict(bool on, const QString& why = QString());
    void SetSaving(bool on);
    void SetLoading(bool on);

    /*---- transfers -------------------------------------------*/
    /* Staged upload: `bytes` up, then `verb` with `args` +
       {staged:token}. purpose labels the flow for errors. */
    bool BeginUpload(const QString& purpose,
                     const QByteArray& bytes,
                     const QString& verb,
                     const QVariantMap& args);
    /* Document download for the active workspace. */
    bool BeginDownload(const QString& kind, const QString& id,
                       const QString& purpose);
    bool ContinueDownload();
    void FinishDownload();
    void AbortTransfers();

    EngineBridge*          bridge_;
    WorkspaceEditSession   session_;
    WorkspaceDraftStore    drafts_;

    /*---- current managed snapshot (engine truth) -------------*/
    bool        available_  = false;
    bool        managed_    = false;
    QString     ws_id_;
    QString     ws_name_;
    quint64     ws_rev_     = 0;   /* last seen durable revision */
    quint64     res_rev_    = 0;
    QString     ws_sha256_;        /* last seen content hash     */
    QVariantList ws_list_;

    /*---- pending engine correlations -------------------------*/
    /* request-id -> tag so one replyReceived dispatch serves
       get/list/commit/transfer flows without collisions. */
    std::map<int, QString> pending_reqs_;
    /* The in-flight save: the document snapshot + base
       revisions submitted — adoption uses THESE bytes, and
       reconnect reconcile hashes them against content_sha256. */
    int         pending_save_req_ = 0;
    uint64_t    pending_job_id_   = 0;
    QString     pending_verb_;     /* "commit" | "save_copy" |
                                      "reload" | "import_commit" |
                                      "export" */
    nlohmann::json pending_doc_ = nullptr;
    QString     pending_doc_sha_;
    QString     pending_save_name_;
    bool        pending_doc_dirty_ = false; /* commit was ours   */
    /* job_id -> purpose for terminal workspace events not part
       of the save flow (import/export/reload). */
    std::map<uint64_t, QString> tracked_jobs_;

    /*---- transfer state machines (bounded: one each) ---------*/
    struct Download
    {
        QString    purpose;        /* "document" | "export"     */
        QString    transfer_id;
        QByteArray bytes;
        quint64    received  = 0;
        quint64    length    = 0;
        quint64    max_chunk = 0;
        QString    sha256;
        QString    workspace_id;   /* document fetch only       */
    };
    std::optional<Download> dl_;
    /* transfer_begin round-trip in flight — snapshot pushes
       and get replies can both ask for the document before
       the first begin reply lands; never issue a second
       begin (duplicate offset-0 chunks corrupt the stream). */
    bool        dl_requested_ = false;
    struct Upload
    {
        QString    purpose;        /* "commit"|"save_copy"|"import" */
        QString    transfer_id;
        QByteArray bytes;
        quint64    sent      = 0;
        quint64    max_chunk = 0;
        QString    verb;
        QVariantMap args;
    };
    std::optional<Upload> ul_;

    /*---- surfaced state ---------------------------------------*/
    bool        loading_         = false;
    bool        saving_          = false;
    bool        conflict_        = false;
    QString     error_;
    QString     error_source_;
    QString     status_text_;
    bool        recovery_offered_ = false;
    bool        recovery_stale_   = false;
    QString     offered_ws_id_;       /* record being offered   */
    /* Cached values for NOTIFY discipline — Dirty() compares
       serialized trees, so transitions are computed once at
       mutation points. */
    bool        dirty_cached_    = false;
    bool        ready_cached_    = false;
    /* reload/openWorkspace: the next document fetch adopts
       wholesale (draft edits abandoned — the draft record is
       already staged so nothing is lost). */
    bool        force_adopt_     = false;
    /* Reconnect reconcile: a commit interrupted mid-flight
       keeps its doc + hash until the next snapshot resolves
       it — never resubmitted blindly, never rolled back. */
    bool        reconcile_pending_ = false;
    /* Serial import queue — uploads/transfers are bounded one
       each, so library imports stage one entry at a time. */
    struct PendingImport { QString name; QByteArray bytes; };
    std::deque<PendingImport> import_queue_;
    int SendCommand(const QString& verb, const QVariantMap& args,
                    const QString& tag);
    QString BaselineSha() const;
    static QString SlugFor(const QString& name);
    void OnJobReply(const QString& verb, const QVariantMap& body,
                    bool ok, const QString& message);
    void OnJobStatusReply(const QVariantMap& body, bool ok);
    void OnDownloadReply(const QString& stage,
                         const QVariantMap& body, bool ok,
                         const QString& message);
    void OnUploadReply(const QString& stage,
                       const QVariantMap& body, bool ok,
                       const QString& message);
    void AdoptFetchedDocument(const QByteArray& bytes);
};

} /* namespace ui */
} /* namespace openrgb2 */
