/*---------------------------------------------------------*\
|| WorkspaceDraftStore.h                                     ||
||                                                           ||
||   Per-workspace, per-client draft records (studio-     ||
||   import Task 4, plan section 3.2): the UI's unsaved   ||
||   edits persist OUTSIDE the engine so a UI crash or    ||
||   an engine restart can never silently lose — or       ||
||   silently APPLY — draft work.                         ||
||                                                           ||
||   Contract:                                            ||
||   - one JSON record per workspace id under a client-   ||
||     scoped directory; content = the compact draft      ||
||     document PLUS the baseline workspace/resource      ||
||     revision it was edited against and the baseline    ||
||     content hash — recovery can then honestly report   ||
||     "engine moved since" instead of guessing           ||
||   - Stage() is debounced (2 s) so a slider scrub does  ||
||     not rewrite the file per edit; Flush() writes the  ||
||     pending record immediately (normal close); a 10 s  ||
||     maximum wait bounds the delay while edits keep     ||
||     restarting the debounce                            ||
||   - a record NEVER applies itself anywhere —           ||
||     WorkspaceController surfaces it as an explicit     ||
||     recover/discard offer only                         ||
||   - writes are atomic (QSaveFile); a corrupt record is ||
||     reported and set aside, never guessed              ||
||                                                           ||
||   SPDX-License-Identifier: GPL-2.0-or-later               ||
||\*---------------------------------------------------------*/

#pragma once

#include <nlohmann/json.hpp>

#include <QObject>
#include <QString>
#include <QStringList>
#include <QTimer>

#include <cstdint>
#include <map>

namespace openrgb2
{
namespace ui
{

/* One durable draft record — the shape written to disk. */
struct WorkspaceDraftRecord
{
    bool           exists             = false;
    QString        workspace_id;
    /* The accepted revisions the draft was edited against —
       recovery compares these to the engine's current durable
       revisions and marks a mismatch "stale", never silent. */
    uint64_t       baseline_revision  = 0;
    uint64_t       resource_revision  = 0;
    /* SHA-256 of the baseline document bytes at edit time. */
    QString        baseline_sha256;
    QString        saved_at;          /* ISO-8601 */
    nlohmann::json document;          /* the compact draft */
    QString        error;             /* read/parse failure */
};

class WorkspaceDraftStore : public QObject
{
    Q_OBJECT

    /* Ids that currently have a durable draft record. */
    Q_PROPERTY(QStringList recordedIds READ recordedIds
                   NOTIFY recordsChanged)
    Q_PROPERTY(QString lastError READ lastError
                   NOTIFY lastErrorChanged)

public:
    explicit WorkspaceDraftStore(QObject* parent = nullptr);

    /* Default root: <AppLocalDataLocation>/workspace-drafts/
       <client-id>. The client id scopes records to this UI
       install (a second client edits against its own). */
    void setRootForTesting(const QString& dir);
    void setClientIdForTesting(const QString& client_id);
    void setDebounceForTesting(int ms);
    /* Maximum time a staged record may stay pending while
       edits keep restarting the debounce (default 10 s). */
    void setMaxWaitForTesting(int ms);

    /* Stage a draft snapshot for `workspace_id` — debounced;
       the pending record materializes on the timer or Flush().
       Passing has_baseline=false records a local-only draft
       (engine never seen — revision fields stay 0). */
    void Stage(const QString& workspace_id,
               uint64_t baseline_revision,
               uint64_t resource_revision,
               const QString& baseline_sha256,
               const nlohmann::json& document);
    /* Flush pending writes NOW (normal close path). */
    void Flush();
    /* Drop the record for `workspace_id` (explicit discard or
       the draft became clean — saved/discard edits). */
    void Discard(const QString& workspace_id);

    /* Read the durable record — exists=false when absent;
       exists=true + error set when the file was unreadable
       (the record is set aside as .rejected, honestly). */
    WorkspaceDraftRecord Read(const QString& workspace_id);
    QStringList recordedIds();
    QString     lastError() const { return last_error_; }
    QString     rootDir() const;

signals:
    void recordsChanged();
    void lastErrorChanged();

private:
    QString FileFor(const QString& workspace_id) const;
    bool    WriteRecord(const QString& workspace_id,
                        const WorkspaceDraftRecord& rec,
                        QString& err);
    void    SetError(const QString& e);

    QString root_;
    QString client_id_ = QStringLiteral("ui");
    int     debounce_ms_ = 2000;
    int     max_wait_ms_ = 10000;
    QString last_error_;
    QTimer  debounce_;
    /* Started by the first Stage of a pending batch and NOT
       restarted by later ones: continuous editing still writes
       at least every max_wait_ms_ (audit U-8). */
    QTimer  max_wait_;
    /* Pending staged writes, workspace id -> record. */
    std::map<QString, WorkspaceDraftRecord> pending_;
};

} /* namespace ui */
} /* namespace openrgb2 */
