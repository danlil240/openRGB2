// SPDX-License-Identifier: GPL-2.0-or-later
//
// StudioViewportModel (studio-import Task 7) — the QML-side
// draft preview. See the header for the boundary contract:
// pipe traffic only, draft colors never presented as
// delivered output.

#include "StudioViewportModel.h"

#include "JsonVariant.h"

#include "config/StudioConfig.h"      /* studio::ToJson */

#include <QTimer>

namespace openrgb2
{
namespace ui
{

StudioViewportModel::StudioViewportModel(
    EngineBridge* bridge, WorkspaceController* workspace,
    QObject* parent)
    : QObject(parent), bridge_(bridge), workspace_(workspace)
{
    /* ONE trailing-edge debounce timer: every draft signal
       restarts it, so a burst of N edits (and undo/redo, which
       emits both draftEdited and historyChanged) produces ONE
       candidate push after the burst settles — not N delayed
       full-document pushes that could fill the outbound queue. */
    push_timer_.setSingleShot(true);
    push_timer_.setInterval(debounce_ms_);
    connect(&push_timer_, &QTimer::timeout, this, [this]
            {
                if(begin_sent_ && active_)
                {
                    PushUpdate();
                }
            });
    if(bridge_)
    {
        connect(bridge_, &EngineBridge::eventReceived, this,
                [this](const QString& kind,
                       const QVariantMap& data)
                {
                    if(kind == QLatin1String("preview"))
                    {
                        OnPreviewEvent(data);
                    }
                });
        connect(bridge_, &EngineBridge::replyReceived, this,
                &StudioViewportModel::OnReply);
        connect(bridge_, &EngineBridge::engineLost, this,
                [this](const QString&)
                {
                    /* The engine's session-bound preview died
                       with the session — clear, and let the
                       reconnect path re-begin honestly. */
                    ClearFrame();
                    active_     = false;
                    paused_     = false;
                    paused_reason_.clear();
                    begin_sent_ = false;
                    refused_    = false;
                    emit previewChanged();
                });
        connect(bridge_, &EngineBridge::connectionStateChanged,
                this, [this] { RecomputeWanted(); });
    }
    if(workspace_)
    {
        /* Any draft mutation (edit/undo/redo/load/recover)
           (re)starts the one debounce timer — the engine never
           sees a queue of stale drafts. */
        auto push = [this]
        {
            refused_ = false;   /* a NEW document may parse */
            RecomputeWanted();
            push_timer_.start();   /* restarts when running */
        };
        connect(workspace_, &WorkspaceController::draftEdited,
                this, push);
        connect(workspace_, &WorkspaceController::historyChanged,
                this, push);
        connect(workspace_, &WorkspaceController::draftChanged,
                this, [this] { RecomputeWanted(); });
    }
}

QString StudioViewportModel::label() const
{
    if(!active_)
    {
        return QStringLiteral("No draft preview");
    }
    QString s = QStringLiteral(
        "Draft preview — unsaved changes, not applied to "
        "lights");
    if(paused_)
    {
        s += paused_reason_ == QLatin1String("display_off")
            ? QStringLiteral(" (paused — display off)")
            : QStringLiteral(" (paused — hidden)");
    }
    if(reduced_)
    {
        s += QStringLiteral(" (reduced detail)");
    }
    return s;
}

void StudioViewportModel::setDebounceMsForTesting(int ms)
{
    debounce_ms_ = ms;
    push_timer_.setInterval(ms);
}

void StudioViewportModel::setWantPreview(bool want)
{
    if(want == want_preview_)
    {
        return;
    }
    want_preview_ = want;
    emit wantPreviewChanged();
    RecomputeWanted();
}

void StudioViewportModel::setVisible(bool visible)
{
    if(visible == visible_)
    {
        return;
    }
    visible_ = visible;
    emit visibleChanged();
    SendVisibility();
}

void StudioViewportModel::refreshNow()
{
    refused_ = false;
    RecomputeWanted();
    if(begin_sent_ && active_)
    {
        PushUpdate();
    }
}

void StudioViewportModel::SetError(const QString& msg)
{
    if(msg != last_error_)
    {
        last_error_ = msg;
        emit lastErrorChanged();
    }
}

void StudioViewportModel::ClearFrame()
{
    sequence_      = 0;
    scene_time_    = 0.0;
    object_total_  = 0;
    emitter_total_ = 0;
    emitter_sent_  = 0;
    reduced_       = false;
    objects_.clear();
    object_index_.clear();
    partials_.clear();
}

QVariant StudioViewportModel::colorsFor(const QString& objectId,
                                        int emitterCount) const
{
    if(!active_)
    {
        return QVariant();
    }
    const auto it = object_index_.constFind(objectId);
    if(it == object_index_.constEnd())
    {
        return QVariant();
    }
    const QVariantMap o = objects_.at(*it).toMap();
    const QVariantList cols = o.value(QStringLiteral("colors")).toList();
    const QVariant idxv = o.value(QStringLiteral("indices"));
    if(idxv.isValid())
    {
        const QVariantList idxs = idxv.toList();
        if(idxs.size() == cols.size())
        {
            QVariantList out;
            const int n = qMax(0, emitterCount);
            out.reserve(n);
            for(int k = 0; k < n; ++k)
            {
                out.append(QStringLiteral("#000000"));
            }
            for(int j = 0; j < idxs.size(); ++j)
            {
                const int at = idxs[j].toInt();
                if(at >= 0 && at < n)
                {
                    out[at] = cols[j];
                }
            }
            return out;
        }
    }
    return cols;
}

/*---------------------------------------------------------*\
|| Command side — begin/update/end over WORKSPACE_COMMAND  ||
\*---------------------------------------------------------*/

void StudioViewportModel::RecomputeWanted()
{
    const bool wanted =
        want_preview_ && bridge_ && bridge_->connected() &&
        bridge_->hasWorkspace() && workspace_ &&
        workspace_->ready();
    if(wanted && !begin_sent_)
    {
        Begin();
    }
    else if(!wanted && begin_sent_)
    {
        End();
    }
}

static nlohmann::json DraftArgs(WorkspaceController* ws, quint64 rev,
                                bool include_visible, bool visible)
{
    /* The compact authoring document — the SAME serialization
       a commit sends; the engine parses it through the shared
       candidate path. Built straight as JSON (no QVariant round
       trip — the document can approach the 1 MiB IPC bound). */
    nlohmann::json args = nlohmann::json::object();
    args["document"]       = studio::ToJson(ws->Session().Working());
    args["draft_revision"] = (std::uint64_t)rev;
    if(include_visible)
    {
        args["visible"] = visible;
    }
    return args;
}

void StudioViewportModel::Begin()
{
    if(!bridge_ || !workspace_ || !workspace_->ready())
    {
        return;
    }
    /* A refused begin latches until the draft changes — no
       retry storm against a rejecting engine. */
    if(refused_)
    {
        return;
    }
    const int id = bridge_->workspaceCommandJson(
        QStringLiteral("preview_begin"),
        DraftArgs(workspace_, ++push_rev_, true, visible_));
    if(id <= 0)
    {
        /* Over-bound document or offline — honest failure, no
           pretend preview. */
        SetError(QStringLiteral(
            "draft preview could not be sent (offline or "
            "over the IPC bound)"));
        return;
    }
    begin_sent_   = true;
    pending_req_  = id;
    sent_visible_ = visible_;
}

void StudioViewportModel::PushUpdate()
{
    if(!bridge_ || !active_ || !workspace_ ||
       !workspace_->ready() || refused_)
    {
        return;
    }
    const int id = bridge_->workspaceCommandJson(
        QStringLiteral("preview_update"),
        DraftArgs(workspace_, ++push_rev_, false, false));
    if(id <= 0)
    {
        SetError(QStringLiteral(
            "draft preview update could not be sent"));
    }
    pending_req_ = id;
}

void StudioViewportModel::SendVisibility()
{
    /* Before the begin reply lands (!active_) nothing is sent:
       the begin carried the visibility of its moment, and OnReply
       re-sends the CURRENT value if it changed meanwhile, so a
       minimize/tray-hide in that window is never lost. */
    if(!bridge_ || !active_ || sent_visible_ == visible_)
    {
        return;
    }
    QVariantMap args;
    args.insert(QStringLiteral("visible"), visible_);
    const int id = bridge_->workspaceCommand(
        QStringLiteral("preview_update"), args);
    if(id > 0)
    {
        pending_req_  = id;
        sent_visible_ = visible_;
    }
}

void StudioViewportModel::End()
{
    if(bridge_ && begin_sent_)
    {
        bridge_->workspaceCommand(QStringLiteral("preview_end"),
                                  QVariantMap());
    }
    begin_sent_ = false;
    active_     = false;
    paused_     = false;
    paused_reason_.clear();
    ClearFrame();
    emit previewChanged();
}

/*---------------------------------------------------------*\
|| Reply + event application                               ||
\*---------------------------------------------------------*/

void StudioViewportModel::OnReply(int request_id,
                                  const QString& type_name,
                                  const QVariantMap& body)
{
    if(request_id != pending_req_ || request_id == 0)
    {
        return;
    }
    pending_req_ = 0;
    if(type_name == QLatin1String("ERROR"))
    {
        const QString detail =
            body.value(QStringLiteral("detail")).toString();
        const QString msg =
            body.value(QStringLiteral("message")).toString();
        SetError(detail.isEmpty() ? msg
                                  : msg + " (" + detail + ")");
        /* A refused begin latches until the draft changes —
           no retry storm against a rejecting engine. */
        refused_ = true;
        begin_sent_ = false;
        if(active_)
        {
            active_ = false;
            ClearFrame();
            emit previewChanged();
        }
        return;
    }
    /* OK to begin/update lands on engine-reported truth. */
    if(body.contains(QStringLiteral("active")))
    {
        const bool a = body.value("active").toBool();
        if(a != active_)
        {
            active_ = a;
            if(!a)
            {
                ClearFrame();
            }
            emit previewChanged();
        }
        else if(!a)
        {
            active_ = false;
        }
        /* The surface may have been hidden/shown while the begin
           was in flight — reconcile the engine with the current
           visibility now that updates are accepted. */
        if(active_)
        {
            SendVisibility();
        }
    }
}

void StudioViewportModel::OnPreviewEvent(const QVariantMap& data)
{
    const QString type =
        data.value(QStringLiteral("type")).toString();
    if(type == QLatin1String("status"))
    {
        bool changed = false;
        const bool active =
            data.value(QStringLiteral("active")).toBool();
        const bool paused =
            data.value(QStringLiteral("paused")).toBool();
        const QString reason =
            data.value(QStringLiteral("reason")).toString();
        const QString err =
            data.value(QStringLiteral("error")).toString();
        if(!err.isEmpty())
        {
            SetError(err);
        }
        if(active != active_)
        {
            active_ = active;
            if(!active)
            {
                ClearFrame();
            }
            changed = true;
        }
        if(paused != paused_ || reason != paused_reason_)
        {
            paused_        = paused;
            paused_reason_ = reason;
            changed = true;
        }
        if(changed)
        {
            emit previewChanged();
        }
        return;
    }
    if(type != QLatin1String("frame"))
    {
        return;
    }

    /* Chunked frame — assemble strictly by sequence; only a
       COMPLETE matching set becomes the displayed draft. */
    const quint64 seq =
        (quint64)data.value(QStringLiteral("sequence"))
            .toULongLong();
    const int chunk =
        data.value(QStringLiteral("chunk")).toInt();
    const int chunks =
        data.value(QStringLiteral("chunks")).toInt();
    if(seq <= sequence_ || chunks <= 0 ||
       chunk < 0 || chunk >= chunks)
    {
        /* Stale or malformed — dropped, counted, never
           painted as current. */
        ++dropped_frames_;
        emit previewChanged();
        return;
    }
    /* A newer sequence orphans every older partial. */
    for(auto it = partials_.begin(); it != partials_.end();)
    {
        if(it->first < seq)
        {
            ++dropped_frames_;
            it = partials_.erase(it);
        }
        else
        {
            ++it;
        }
    }
    Partial& p = partials_[seq];
    if(p.chunks != chunks)
    {
        p = Partial{};
        p.chunks = chunks;
    }
    p.envelope = data;       /* envelope fields are identical
                                per chunk; keep the newest */
    if(!p.got[chunk])
    {
        p.got[chunk] = true;
        p.chunk_objs[chunk] =
            data.value(QStringLiteral("objects")).toList();
        ++p.seen;
    }
    if(p.seen == chunks)
    {
        /* Reassemble in chunk order — arrival order is NOT
           guaranteed to match. */
        QVariantList merged;
        for(int i = 0; i < chunks; ++i)
        {
            merged.append(p.chunk_objs[i]);
        }
        sequence_      = seq;
        draft_rev_     = (quint64)p.envelope.value(
            QStringLiteral("draft_revision")).toULongLong();
        scene_time_    = p.envelope.value(
            QStringLiteral("scene_time_s")).toDouble();
        reduced_       = p.envelope.value(
            QStringLiteral("reduced")).toBool();
        object_total_  = p.envelope.value(
            QStringLiteral("object_total")).toInt();
        emitter_total_ = p.envelope.value(
            QStringLiteral("emitter_total")).toInt();
        emitter_sent_  = p.envelope.value(
            QStringLiteral("emitter_sent")).toInt();
        objects_       = merged;
        object_index_.clear();
        for(int i = 0; i < objects_.size(); ++i)
        {
            object_index_.insert(objects_[i].toMap()
                                     .value(QStringLiteral("id"))
                                     .toString(), i);
        }
        partials_.erase(seq);
        emit previewChanged();
    }
}

} /* namespace ui */
} /* namespace openrgb2 */
