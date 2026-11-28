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
#include <QTimer>
#include <QVariantList>
#include <QVariantMap>

#include <cstdint>
#include <deque>
#include <functional>
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

    /*---- studio-import Task 10: external change + actions ----\
    |  The engine polls its own files (the get reply carries    |
    |  the compare). "" | "modified" | "deleted" — surfaced for  |
    |  the user to resolve; detection NEVER applies, saves, or   |
    |  touches hardware. workspaceDir/Root are the engine-       |
    |  reported managed paths (Open Workspace Folder target).    |
    |  replaceOffered: a draft-replacing action (reload / open   |
    |  / restore backup) is latched for an explicit confirm —    |
    |  replaceText carries the source/destination summary.       |
    \*---------------------------------------------------------*/
    Q_PROPERTY(QString externalChange READ externalChange
                   NOTIFY externalChanged)
    Q_PROPERTY(QString externalDetail READ externalDetail
                   NOTIFY externalChanged)
    Q_PROPERTY(QString workspaceDir READ workspaceDir
                   NOTIFY workspaceChanged)
    Q_PROPERTY(QString workspaceRoot READ workspaceRoot
                   NOTIFY workspaceChanged)
    Q_PROPERTY(bool replaceOffered READ replaceOffered
                   NOTIFY replaceChanged)
    Q_PROPERTY(QString replaceText READ replaceText
                   NOTIFY replaceChanged)

    /*---- studio-import Task 8: viewport editor prefs ----------
       meta.camera / meta.render / meta.ui are document editor
       preferences — they ride the draft (dirty + draft-store
       record) but NEVER the undo stack, the same rule the
       source SceneBridge honored for setCameraState /
       setRenderQuality. Reads return the compact defaults when
       no draft is loaded. */
    Q_PROPERTY(QVariantMap cameraState READ cameraState
                   NOTIFY cameraChanged)
    Q_PROPERTY(QString renderQuality READ renderQuality
                   WRITE setRenderQuality NOTIFY renderPrefsChanged)
    Q_PROPERTY(bool renderBloom READ renderBloom
                   WRITE setRenderBloom NOTIFY renderPrefsChanged)
    /* meta.ui.reduced_motion — display-only temporal-AA gate;
       shares renderPrefsChanged (it is a render surface pref). */
    Q_PROPERTY(bool reducedMotion READ reducedMotion
                   WRITE setReducedMotion NOTIFY renderPrefsChanged)

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
       re-fetch the document — abandons draft edits. With a
       dirty draft and no already-latched conflict the action
       LATCHES for confirmation (replaceOffered) instead of
       running — confirmReplace() executes it. */
    Q_INVOKABLE void reload();
    /* Restore Backup (Task 10): the marker backup's last-valid
       generation commits forward as a new generation —
       revision-gated engine-side. Dirty drafts get the same
       replace latch as reload. */
    Q_INVOKABLE void restoreBackup();
    /* Confirm / drop a latched draft-replacing action. */
    Q_INVOKABLE void confirmReplace();
    Q_INVOKABLE void cancelReplace();
    /* Open the engine-reported managed workspace dir in the
       OS shell — UI-side, the engine never spawns anything.
       Only an existing absolute local directory, opened with
       the system explorer.exe (launch::ValidateShellFolder,
       audit U-6); anything else sets error. */
    Q_INVOKABLE void openWorkspaceFolder();
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
    /*---- Task 9 bundle import (inspect -> choose -> commit) ---*\
    |  inspectImport stages the bundle — `bytesBase64` is the   |
    |  base64 text readLocalFile returns (it is decoded here,   |
    |  never uploaded twice-encoded) — and runs import_inspect; |
    |  the report (validity, type conflicts + suggested import  |
    |  ids, bundled assets, warnings) arrives in                 |
    |  importInspectionReady. confirmImport commits the SAME    |
    |  staged upload with the caller's {conflict_id: import_id} |
    |  choices; cancelImport drops the pending inspect (the     |
    |  staged upload expires engine-side). mode selects the     |
    |  target/mode: "" imports as a NEW workspace slugged from  |
    |  the file name, "current" commits the imported document   |
    |  onto the ACTIVE workspace (replace-my-layout; never      |
    |  applied without a reload), "preset_import" installs only |
    |  the bundled resources into the active workspace's        |
    |  resource scope (types/assets, no document commit — the   |
    |  mode is forwarded to the engine).                        |
    \*----------------------------------------------------------*/
    Q_INVOKABLE void inspectImport(const QString& name,
                                   const QString& bytesBase64);
    Q_INVOKABLE void confirmImport(const QVariantMap& choices,
                                   const QString& mode = QString());
    Q_INVOKABLE void cancelImport();
    /* Bounded local file IO for the import/export dialogs —
       reads cap at the 64 MiB bundle ceiling, writes refuse to
       overwrite an existing file. QML returns {ok, base64,
       error} / {ok, error} — never a silent truncation. */
    Q_INVOKABLE QVariantMap readLocalFile(const QString& path);
    Q_INVOKABLE QVariantMap writeLocalFile(const QString& path,
                                           const QString& bytesBase64);
    /* Every named entry of the UI profile library, imported as
       its own managed workspace — originals are retained
       (nothing is deleted or renamed). */
    Q_INVOKABLE int  importProfiles(ProfileLibrary* library);
    /* Refresh the managed workspace list (list verb). */
    Q_INVOKABLE void refreshList();
    /* Re-pull the managed snapshot (get) — the self-heal a
       refused legacy save triggers when the engine reports a
       managed workspace this session never saw adopted. */
    Q_INVOKABLE void resync();

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
    /* Task 10 state */
    QString     externalChange() const;
    QString     externalDetail() const;
    QString     workspaceDir() const;
    QString     workspaceRoot() const;
    bool        replaceOffered() const;
    QString     replaceText() const;

    /*---- viewport editor prefs (Task 8) -------------------------*/
    /* {view,projection,tx,ty,tz,yaw,pitch,distance,span,
        middle_drag} — the viewport's CameraController mirrors
        this at gesture end through setCameraState. Writes go
        through WorkspaceEditSession::SetMeta — document prefs,
        never undo records. */
    QVariantMap cameraState() const;
    Q_INVOKABLE void setCameraState(const QVariantMap& state);
    QString     renderQuality() const;
    void        setRenderQuality(const QString& q);
    bool        renderBloom() const;
    void        setRenderBloom(bool on);
    /* meta.ui.reduced_motion — the viewport reads the document
       pref through this; the shell-level default lives on
       UiSettings.viewportReducedMotion. */
    bool        reducedMotion() const;
    void        setReducedMotion(bool on);

    /*---- test hooks --------------------------------------------*/
    void setDraftDirForTesting(const QString& dir);
    void setDebounceForTesting(int ms);
    /* External-change poll interval (ms); <=0 disables. */
    void setExternalPollForTesting(int ms);
    int  PendingJobIdForTesting() const { return pending_job_id_; }
    /* Replaces the explorer.exe launch in openWorkspaceFolder:
       (absolute program, {native folder}) -> started. */
    void setShellLauncherForTesting(
        std::function<bool(const QString&, const QStringList&)>
            launcher);
    /* Poll job_status for the tracked pending job — the
       lost-event reconcile path. */
    void PollPendingJobForTesting();
    /* Job watchdog interval (ms) — default 5000. */
    void setJobWatchdogForTesting(int ms);

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
    /* Task 9 — the import_inspect report for the pending staged
       upload: {valid, types[], conflicts[], assets[],
       warnings[], errors[]}. */
    void importInspectionReady(const QVariantMap& report);
    /* save() completed — `ok` distinguishes committed from
       refused so pages can surface honestly. */
    void saveFinished(bool ok);
    /* Task 10: the engine-reported on-disk compare moved
       (externalChange/externalDetail). */
    void externalChanged();
    /* Task 10: a draft-replacing action latched / resolved. */
    void replaceChanged();
    /* meta.camera changed (setCameraState or a workspace load). */
    void cameraChanged();
    /* meta.render/meta.ui editor prefs changed. */
    void renderPrefsChanged();

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
    void FetchDocument();
    void CheckRecoveryOffer();
    void PumpImportQueue();
    /* Clears the "Importing workspace…" banner once nothing
       import-shaped is outstanding (queue, upload, pending
       accept, tracked job) — the banner must never outlive
       the queue that armed it. */
    void SettleImportStatus();
    /* Fire-and-forget release for a staged upload token that
       no job will ever consume. */
    void ReleaseStagedToken(const QString& token);

    /*---- job bookkeeping ------------------------------------*/
    void OnJobEvent(const QVariantMap& ev);
    void ResolveJob(const QVariantMap& st);
    /* Audit U-7 — job terminals that can never latch:
       NoteJobRevisions applies a terminal record's revisions
       (our workspace only) and reports whether they moved;
       ClaimJob takes `job` off the save/tracked bookkeeping
       (false when it is not ours, or already resolved);
       OnForeignJob is the not-ours path (rebase or refetch). */
    bool NoteJobRevisions(const QVariantMap& ev);
    bool ClaimJob(quint64 job);
    void OnForeignJob(const QVariantMap& ev, bool revs_moved);
    /* An accept reply for a job request is still in flight —
       an unknown terminal may be that job's. */
    bool AwaitingJobAccept() const;
    /* Resolve a buffered early terminal for `job`, if any. */
    void TakeEarlyTerminal(quint64 job);
    /* No accept outstanding: the buffered terminals were
       foreign after all. */
    void DrainEarlyTerminals();
    void OnJobStatusReply(quint64 job, const QVariantMap& body,
                          bool ok);
    void JobWatchdogTick();
    void ArmJobWatchdog();
    void SetError(const QString& e, const QString& source);
    void SetStatus(const QString& s);
    void SetConflict(bool on, const QString& why = QString());
    void SetSaving(bool on);
    void SetLoading(bool on);
    /* Task 10 helpers — the external compare, the poll, and
       the replace latch + the latched actions themselves.
       `action` is "reload" (id = target workspace, empty for
       the active one) or "restore_backup". */
    void SetExternal(const QString& state, const QString& detail);
    void UpdatePoll();
    void OfferReplace(const QString& action, const QString& id,
                      const QString& text);
    void DoReload(const QString& id);
    void DoRestoreBackup();

    /*---- transfers -------------------------------------------*/
    /* Staged upload: `bytes` up, then `verb` with `args` +
       {staged:token}. purpose labels the flow for errors. */
    bool BeginUpload(const QString& purpose,
                     const QByteArray& bytes,
                     const QString& verb,
                     const QVariantMap& args,
                     const QString& kind = QStringLiteral("bundle"));
    /* A save/save-copy whose upload or staged job send failed:
       release the saving latch honestly. */
    void FailStagedSave(const QString& why,
                        const QString& source =
                            QStringLiteral("engine"));
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
    uint64_t    pending_job_id_   = 0;
    nlohmann::json pending_doc_ = nullptr;
    QString     pending_doc_sha_;
    /* This client's last landed commit (content hash + the
       revision it landed as). A snapshot naming exactly these
       is the echo of our own save, not a foreign change: the
       baseline already holds those bytes (audit U-3). */
    QString     committed_sha_;
    quint64     committed_ws_rev_ = 0;
    QString     pending_save_name_;
    /* job_id -> purpose for terminal workspace events not part
       of the save flow (import/export/reload). */
    std::map<uint64_t, QString> tracked_jobs_;
    /* Verb of pending_job_id_ (commit/save_copy) — a job the
       engine no longer reports resolves as THAT verb failing. */
    QString     pending_job_verb_;
    /* Audit U-7: terminal events for job ids not known yet,
       held while an accept reply is in flight (the event can
       overtake it). Bounded; replayed as foreign once no accept
       is outstanding. */
    struct EarlyTerminal
    {
        QVariantMap ev;
        bool        revs_moved = false;
    };
    std::map<quint64, EarlyTerminal> early_terminals_;
    static constexpr size_t kMaxEarlyTerminals = 16;
    /* Watchdog: while any job is outstanding, poll job_status
       so a lost terminal event can never latch saving/import/
       export. One in-flight poll per job. */
    QTimer      job_watchdog_;
    int         job_watchdog_ms_ = 5000;
    std::set<quint64> job_polls_;

    /* Task 9 bundle import — the staged upload token + target
       name between import_inspect and confirmImport (the
       staged token stays consumable for the same session). */
    QString     pending_inspect_token_;
    QString     pending_inspect_name_;

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
        quint64    epoch = 0;      /* fetch_epoch_ at request   */
    };
    std::optional<Download> dl_;
    /* transfer_begin round-trip in flight — snapshot pushes
       and get replies can both ask for the document before
       the first begin reply lands; never issue a second
       begin (duplicate offset-0 chunks corrupt the stream). */
    bool        dl_requested_ = false;
    /* Identity of the in-flight document request — stamped
       onto dl_ at begin-reply (replies carry no id). */
    QString     dl_request_ws_;
    quint64     dl_request_epoch_ = 0;
    /* Bumped by every explicit reload/open/restore — a
       document fetch that began BEFORE the user confirmed a
       replace adopts stale bytes (the pre-switch document) and
       its clean path would Discard the just-staged draft
       record. Stale-epoch finishes are dropped, never
       adopted. */
    quint64     fetch_epoch_ = 0;
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
    /* openWorkspace target between the reload job's terminal
       event and the workspace snapshot naming the new id —
       FetchDocument must NEVER run with the stale id (it would
       download the OLD workspace's document, adopt it, and the
       clean path would Discard the abandoned draft record —
       switching must never lose unsaved work). */
    QString     pending_switch_id_;
    /* Reconnect reconcile: a commit interrupted mid-flight
       keeps its doc + hash until the next snapshot resolves
       it — never resubmitted blindly, never rolled back. */
    bool        reconcile_pending_ = false;
    /* Serial import queue — uploads/transfers are bounded one
       each, so library imports stage one entry at a time. */
    struct PendingImport { QString name; QByteArray bytes; };
    std::deque<PendingImport> import_queue_;
    /* Names ride alongside the serial queue so a failed or
       finished import_commit reports WHICH entry — never an
       empty name — and the queue keeps moving either way.
       req_names_ keys the name on the workspaceCommand REQUEST
       id (replies are never guaranteed to land in submit
       order); names_ holds accepted job_id -> name until the
       terminal event resolves it. */
    std::map<int, QString>        import_req_names_;
    std::map<uint64_t, QString>   import_names_;
    /* The workspace whose durable record is THIS session's
       live-draft shadow — a clean transition drops only that
       record; one staged by a previous session is a recovery
       candidate CheckRecoveryOffer must surface, never
       silently drop. */
    QString     staged_id_;
    /* A draft recovered while no engine document had loaded
       (the UI started first): the workspace its record came
       from. Its record stays keyed by this id (never "local"),
       and the first attach decides: same workspace -> the
       engine copy becomes the baseline under the draft
       (adopt_under_draft_); another workspace -> the record is
       released for a later recovery offer, never overwritten. */
    QString     pre_attach_origin_;
    bool        adopt_under_draft_ = false;
    /* Cached baseline hash for the durable record — the
       baseline only moves at LoadApplied/ApplyRemote/
       AdoptCommitted; recomputing per edit serializes the
       whole document every keystroke. */
    QString     baseline_sha_;

    /*---- Task 10: external-change surface ---------------------*/
    /* Passive poll — issues a `get` (the reply carries the
       engine's file compare). Detection is read-only both
       sides; it never loads a document or applies anything. */
    QTimer      ext_poll_;
    int         ext_poll_ms_ = 2000;
    QString     external_state_;      /* ""|"modified"|"deleted" */
    QString     external_detail_;
    QString     ws_dir_;              /* engine-reported paths  */
    std::function<bool(const QString&, const QStringList&)>
                shell_launcher_;      /* test seam (U-6)        */
    QString     ws_root_;
    /* A draft-replacing action the user must confirm —
       replaceText carries source/destination + the conflict
       consequence before anything is discarded. */
    struct PendingReplace
    {
        QString action;               /* "reload"|"restore_backup" */
        QString id;                   /* reload target, "" = self  */
        QString text;
    };
    std::optional<PendingReplace> pending_replace_;
    int SendCommand(const QString& verb, const QVariantMap& args,
                    const QString& tag);
    QString BaselineSha();
    static QString SlugFor(const QString& name);
    void OnJobReply(int request_id, const QString& verb,
                    const QVariantMap& body, bool ok,
                    const QString& message);
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
