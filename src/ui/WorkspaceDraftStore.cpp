// SPDX-License-Identifier: GPL-2.0-or-later
//
// WorkspaceDraftStore — per-workspace durable draft records.
// One JSON file per workspace id under the client-scoped
// root; Stage() is debounced so an edit stream costs one
// write, Flush() materializes pending records on normal
// close, and a corrupt record is renamed aside (.rejected)
// and reported — never silently dropped or guessed.

#include "WorkspaceDraftStore.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QSaveFile>
#include <QStandardPaths>

namespace openrgb2
{
namespace ui
{

namespace
{

/* Workspace ids are slugged by the engine; sanitize anyway —
   a record name never escapes the drafts directory. */
QString SanitizedId(const QString& id)
{
    QString out;
    out.reserve(id.size());
    for(const QChar c : id)
    {
        out += (c.isLetterOrNumber() || c == '-' || c == '_')
                   ? c : QLatin1Char('_');
    }
    return out.isEmpty() ? QStringLiteral("local") : out;
}

QString NowIso()
{
    return QDateTime::currentDateTimeUtc()
        .toString(Qt::ISODateWithMs);
}

} /* anonymous namespace */

WorkspaceDraftStore::WorkspaceDraftStore(QObject* parent)
    : QObject(parent)
{
    debounce_.setSingleShot(true);
    connect(&debounce_, &QTimer::timeout,
            this, &WorkspaceDraftStore::Flush);
    max_wait_.setSingleShot(true);
    connect(&max_wait_, &QTimer::timeout,
            this, &WorkspaceDraftStore::Flush);
}

void WorkspaceDraftStore::setRootForTesting(const QString& dir)
{
    root_ = dir;
}

void WorkspaceDraftStore::setClientIdForTesting(
    const QString& client_id)
{
    client_id_ = client_id.isEmpty() ? QStringLiteral("ui")
                                     : client_id;
    root_.clear();
}

void WorkspaceDraftStore::setDebounceForTesting(int ms)
{
    debounce_ms_ = ms < 0 ? 0 : ms;
}

void WorkspaceDraftStore::setMaxWaitForTesting(int ms)
{
    max_wait_ms_ = ms < 0 ? 0 : ms;
}

QString WorkspaceDraftStore::rootDir() const
{
    if(!root_.isEmpty())
    {
        return root_;
    }
    return QStandardPaths::writableLocation(
               QStandardPaths::AppLocalDataLocation)
        + QStringLiteral("/workspace-drafts/") + client_id_;
}

QString WorkspaceDraftStore::FileFor(const QString& id) const
{
    return rootDir() + QLatin1Char('/') + SanitizedId(id)
        + QStringLiteral(".draft.json");
}

void WorkspaceDraftStore::Stage(const QString& workspace_id,
                                uint64_t baseline_revision,
                                uint64_t resource_revision,
                                const QString& baseline_sha256,
                                const nlohmann::json& document)
{
    WorkspaceDraftRecord rec;
    rec.exists            = true;
    rec.workspace_id      = workspace_id;
    rec.baseline_revision = baseline_revision;
    rec.resource_revision = resource_revision;
    rec.baseline_sha256   = baseline_sha256;
    rec.saved_at          = NowIso();
    rec.document          = document;
    pending_[workspace_id] = std::move(rec);
    debounce_.start(debounce_ms_);
    /* The debounce restarts on every edit, so a continuous
       edit stream (slider scrub, drag) would never write; the
       maximum wait runs from the first unwritten Stage and is
       never pushed back (audit U-8). */
    if(!max_wait_.isActive())
    {
        max_wait_.start(max_wait_ms_);
    }
}

void WorkspaceDraftStore::Flush()
{
    debounce_.stop();
    max_wait_.stop();
    if(pending_.empty())
    {
        return;
    }
    const auto pending = std::move(pending_);
    pending_.clear();
    bool changed = false;
    for(const auto& kv : pending)
    {
        QString err;
        if(!WriteRecord(kv.first, kv.second, err))
        {
            SetError(err);
        }
        else
        {
            changed = true;
        }
    }
    if(changed)
    {
        emit recordsChanged();
    }
}

void WorkspaceDraftStore::Discard(const QString& workspace_id)
{
    const bool had_pending = pending_.erase(workspace_id) > 0;
    const QString f = FileFor(workspace_id);
    const bool had_file = QFile::exists(f);
    if(had_file)
    {
        QFile::remove(f);
    }
    if(had_pending || had_file)
    {
        emit recordsChanged();
    }
}

bool WorkspaceDraftStore::WriteRecord(
    const QString& workspace_id,
    const WorkspaceDraftRecord& rec, QString& err)
{
    const QString dir = rootDir();
    if(!QDir().mkpath(dir))
    {
        err = QStringLiteral("cannot create draft directory %1")
                  .arg(dir);
        return false;
    }
    const nlohmann::json j = nlohmann::json::object({
        { "format",            "openrgb2-workspace-draft" },
        { "version",           1                        },
        { "workspace_id",      rec.workspace_id.toStdString() },
        { "baseline_revision", rec.baseline_revision      },
        { "resource_revision", rec.resource_revision      },
        { "baseline_sha256",   rec.baseline_sha256.toStdString() },
        { "saved_at",          rec.saved_at.toStdString() },
        { "document",          rec.document             },
    });
    QSaveFile f(FileFor(workspace_id));
    if(!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
    {
        err = QStringLiteral("cannot write draft for '%1'")
                  .arg(workspace_id);
        return false;
    }
    const std::string bytes = j.dump(2);
    f.write(bytes.data(), (qint64)bytes.size());
    if(!f.commit())
    {
        err = QStringLiteral("draft write for '%1' did not "
                             "commit").arg(workspace_id);
        return false;
    }
    return true;
}

WorkspaceDraftRecord WorkspaceDraftStore::Read(
    const QString& workspace_id)
{
    WorkspaceDraftRecord rec;
    const QString        f = FileFor(workspace_id);
    if(!QFile::exists(f))
    {
        return rec;                     /* exists=false */
    }
    std::string err;
    nlohmann::json j;
    {
        /* Scope the handle: the corrupt-file rename below can
           never succeed while the file is still open. */
        QFile in(f);
        if(!in.open(QIODevice::ReadOnly))
        {
            err = "unreadable draft file";
        }
        else
        {
            j = nlohmann::json::parse(
                in.readAll().toStdString(), nullptr, false);
            if(j.is_discarded() || !j.is_object() ||
               !j.value("document", nlohmann::json())
                    .is_object())
            {
                err = "malformed draft record";
            }
        }
    }
    if(!err.empty())
    {
        /* Honest set-aside: the record is renamed, reported,
           and never guessed at. */
        const QString aside = f + QStringLiteral(".rejected");
        QFile::remove(aside);
        QFile::rename(f, aside);
        rec.error = QString::fromStdString(err);
        SetError(rec.error + QStringLiteral(" (%1)")
                     .arg(workspace_id));
        return rec;
    }
    rec.exists            = true;
    rec.workspace_id      = QString::fromStdString(
        j.value("workspace_id", workspace_id.toStdString()));
    rec.baseline_revision =
        j.value("baseline_revision", (uint64_t)0);
    rec.resource_revision =
        j.value("resource_revision", (uint64_t)0);
    rec.baseline_sha256   = QString::fromStdString(
        j.value("baseline_sha256", std::string()));
    rec.saved_at          = QString::fromStdString(
        j.value("saved_at", std::string()));
    rec.document          = j["document"];
    return rec;
}

QStringList WorkspaceDraftStore::recordedIds()
{
    /* Durable records only — a pending (unflushed) Stage is
       not yet a record a future run could recover. */
    QStringList out;
    const QDir dir(rootDir());
    const QStringList files = dir.entryList(
        { QStringLiteral("*.draft.json") }, QDir::Files);
    for(const QString& f : files)
    {
        QString id = f;
        id.chop(QStringLiteral(".draft.json").size());
        out << id;
    }
    out.sort();
    return out;
}

void WorkspaceDraftStore::SetError(const QString& e)
{
    if(e == last_error_)
    {
        return;
    }
    last_error_ = e;
    emit lastErrorChanged();
}

} /* namespace ui */
} /* namespace openrgb2 */
