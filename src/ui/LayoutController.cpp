/*---------------------------------------------------------*\
||| LayoutController.cpp                                      ||
|||                                                           ||
|||   SPDX-License-Identifier: GPL-2.0-or-later               ||
\*---------------------------------------------------------*/

#include "LayoutController.h"

#include "EngineBridge.h"
#include "JsonVariant.h"
#include "WorkspaceController.h"

#include "config/StudioConfig.h"
#include "editor/EditorController.h"

#include <QDateTime>

namespace openrgb2
{
namespace ui
{

LayoutController::LayoutController(EngineBridge* bridge,
                                   QObject* parent)
    : QObject(parent)
    , bridge_(bridge)
{
    connect(bridge_, &EngineBridge::sceneChanged,
            this, &LayoutController::OnSceneChanged);
    connect(bridge_, &EngineBridge::snapshotsChanged,
            this, &LayoutController::OnSnapshotsChanged);
    connect(bridge_, &EngineBridge::replyReceived,
            this, &LayoutController::OnReplyReceived);

    /* Engine-side expiry is authoritative — the sweep only
       drops stale banners (the blinks themselves already
       ended). Each entry carries its acknowledged duration
       plus one half-period of slack as deadline_ms. */
    identify_timer_.setInterval(250);
    connect(&identify_timer_, &QTimer::timeout, this,
            &LayoutController::PruneIdentifies);

    connect(bridge_, &EngineBridge::engineLost, this, [this] {
        /* The pending entry is a live request — an engine loss
           forfeits it; drop the recorded undo so a refused
           timeline can't resurrect. */
        if(pending_.id != 0)
        {
            if(pending_.kind == QLatin1String("apply"))
            {
                model_.DropUndo();
            }
            pending_ = Pending{};
            emit busyChanged();
        }
        pending_identify_ids_.clear();
        identify_timer_.stop();
        if(!identifying_.isEmpty())
        {
            identifying_ = QVariantList{};
            emit identifyingChanged();
        }
        SetLastError(QStringLiteral("engine connection lost"),
                     QStringLiteral("connection"));
        emit itemsChanged();
    });
}

/*---------------------------------------------------------*\
|||| Shared-workspace adaptation (Task 4)                   ||
\*---------------------------------------------------------*/

void LayoutController::setWorkspace(WorkspaceController* ws)
{
    ws_ = ws;
    if(ws_ == nullptr)
    {
        return;
    }
    /* The shared draft's mutation signals re-drive exactly
       the properties the LAYOUT_COMMAND replies used to
       refresh. */
    connect(ws_, &WorkspaceController::draftEdited, this,
            [this] {
                emit itemsChanged();
                emit selectedChanged();
                emit historyChanged();
            });
    connect(ws_, &WorkspaceController::draftChanged, this,
            [this] {
                emit itemsChanged();
                emit selectedChanged();
            });
    connect(ws_, &WorkspaceController::workspaceChanged, this,
            [this] {
                emit itemsChanged();
                emit selectedChanged();
            });
    connect(ws_, &WorkspaceController::historyChanged,
            this, &LayoutController::historyChanged);
    connect(ws_, &WorkspaceController::savingChanged,
            this, &LayoutController::busyChanged);
    connect(ws_, &WorkspaceController::loadingChanged,
            this, &LayoutController::busyChanged);
    connect(ws_, &WorkspaceController::errorChanged, this,
            [this] {
                /* Engine rejections to workspace ops surface
                   on the page's existing error row. */
                if(!ws_->error().isEmpty())
                {
                    SetLastError(ws_->error(),
                                 ws_->errorSource());
                }
            });
}

bool LayoutController::WsMode() const
{
    return ws_ != nullptr &&
           ((ws_->available() && ws_->managed()) ||
            ws_->Session().HasWorking());
}

const studio::StudioDocument* LayoutController::WsDoc() const
{
    if(!WsMode() || !ws_->Session().HasWorking())
    {
        return nullptr;
    }
    return &ws_->Session().Working();
}

bool LayoutController::WsApply(std::optional<studio::EditorEdit> e,
                               const QString& label)
{
    if(!e.has_value())
    {
        const std::string& le =
            ws_->Session().Editor().LastError();
        SetLastError(le.empty()
                         ? QStringLiteral("edit refused")
                         : QString::fromStdString(le),
                     QStringLiteral("input"));
        return false;
    }
    e->label = label.toStdString();
    ws_->Session().CommitEdit(*e);
    ws_->NoteDraftEdited();
    SetLastError(QString());
    SetStatus(QStringLiteral("staged: %1 — unsaved until "
                             "Save").arg(label));
    emit itemsChanged();
    emit selectedChanged();
    emit historyChanged();
    return true;
}

bool LayoutController::WsDocEdit(
    const std::string& label,
    const std::function<bool(studio::StudioDocument&)>& mut)
{
    if(!WsMode() || !ws_->Session().HasWorking())
    {
        SetLastError(QStringLiteral("no workspace draft "
                                    "loaded"),
                     QStringLiteral("input"));
        return false;
    }
    if(!ws_->Session().CommitDocEdit(label, mut))
    {
        SetLastError(QStringLiteral("edit refused"),
                     QStringLiteral("input"));
        return false;
    }
    ws_->NoteDraftEdited();
    SetLastError(QString());
    SetStatus(QStringLiteral("staged: %1 — unsaved until "
                             "Save").arg(QString::fromStdString(label)));
    emit itemsChanged();
    emit selectedChanged();
    emit historyChanged();
    return true;
}

namespace
{

/* Mirrors studio::NameMatches — the same display-hint
   convention SceneEditController uses for binding joins.
   The engine's resolver is authoritative; this only paints
   badges. */
bool NameMatches(const QString& wanted, const QString& actual)
{
    if(wanted.isEmpty())
    {
        return true;
    }
    const QString w = wanted.toLower();
    const QString a = actual.toLower();
    return a == w || a.contains(w);
}

} /* anonymous namespace */

QString LayoutController::WsBindingState(
    const studio::DeviceBinding& b, bool& connected) const
{
    connected = false;
    if(b.controller_name.empty() && b.serial.empty())
    {
        return QStringLiteral("unbound");
    }
    QVariantList matches;
    for(const QVariant& sv : bridge_->snapshots())
    {
        const QVariantMap snap = sv.toMap();
        const QVariantMap key  =
            snap.value(QStringLiteral("key")).toMap();
        const QString name =
            snap.value(QStringLiteral("name")).toString();
        if(!b.serial.empty())
        {
            if(key.value(QStringLiteral("serial")).toString() !=
                   QString::fromStdString(b.serial) ||
               !NameMatches(QString::fromStdString(
                                b.controller_name), name))
            {
                continue;
            }
            matches.push_back(snap);
            continue;
        }
        if(!NameMatches(QString::fromStdString(
                            b.controller_name), name))
        {
            continue;
        }
        matches.push_back(snap);
    }
    if(matches.isEmpty())
    {
        return QStringLiteral("absent");
    }
    if(matches.size() > 1 && !b.location.empty())
    {
        QVariantList narrowed;
        for(const QVariant& mv : matches)
        {
            if(mv.toMap().value(QStringLiteral("key")).toMap()
                   .value(QStringLiteral("location"))
                   .toString() ==
               QString::fromStdString(b.location))
            {
                narrowed.push_back(mv);
            }
        }
        if(!narrowed.isEmpty())
        {
            matches = narrowed;
        }
    }
    if(matches.size() > 1)
    {
        return QStringLiteral("ambiguous");
    }
    connected = matches.first().toMap()
                    .value(QStringLiteral("connection"))
                    .toString() == QLatin1String("connected");
    return QStringLiteral("resolved");
}

QVariantList LayoutController::WsItems() const
{
    QVariantList out;
    const studio::StudioDocument* w = WsDoc();
    if(w == nullptr)
    {
        return out;
    }
    for(const auto& kv : w->devices)
    {
        const std::string&            iid = kv.first;
        const studio::DeviceInstance& d   = kv.second;
        const auto sit = w->device_settings.find(iid);
        const studio::DeviceSettings* s =
            sit != w->device_settings.end() ? &sit->second
                                            : nullptr;

        QVariantMap m;
        m.insert(QStringLiteral("id"),
                 QString::fromStdString(iid));
        m.insert(QStringLiteral("label"),
                 QString::fromStdString(iid));
        QString kind = QStringLiteral("device");
        if(d.type == "group")
        {
            kind = QStringLiteral("group");
        }
        else if(s != nullptr && !s->mirror_of.empty())
        {
            kind = QStringLiteral("linked");
        }
        m.insert(QStringLiteral("kind"), kind);
        m.insert(QStringLiteral("x"), (double)d.position.x);
        m.insert(QStringLiteral("y"), (double)d.position.y);
        m.insert(QStringLiteral("z"), (double)d.position.z);
        m.insert(QStringLiteral("rx"), (double)d.rotation_deg.x);
        m.insert(QStringLiteral("ry"), (double)d.rotation_deg.y);
        m.insert(QStringLiteral("rz"), (double)d.rotation_deg.z);
        /* Footprint + emitter counts are resolved-scene data —
           not stored in the compact doc and never invented. */
        m.insert(QStringLiteral("w"), 0.0);
        m.insert(QStringLiteral("d"), 0.0);
        m.insert(QStringLiteral("emitter_count"), 0);
        m.insert(QStringLiteral("addressed_count"), 0);
        m.insert(QStringLiteral("geometry"), QString());
        m.insert(QStringLiteral("mirror_of"),
                 QString::fromStdString(
                     s != nullptr ? s->mirror_of
                                  : std::string()));
        m.insert(QStringLiteral("visible"),
                 s == nullptr || s->visible);

        bool        verified = false;
        std::string binding;
        if(s != nullptr)
        {
            for(const auto& z : s->zones)
            {
                verified = verified || z.second.verified;
                if(binding.empty() && !z.second.binding.empty())
                {
                    binding = z.second.binding;
                }
            }
        }
        m.insert(QStringLiteral("verified"), verified);
        m.insert(QStringLiteral("binding"),
                 QString::fromStdString(binding));
        QString state     = QStringLiteral("unbound");
        QString ctrl, zname;
        int         zleds = 0;
        bool        conn  = false;
        const auto bit = binding.empty()
                             ? w->bindings.end()
                             : w->bindings.find(binding);
        if(bit != w->bindings.end())
        {
            const studio::DeviceBinding& b = bit->second;
            state = WsBindingState(b, conn);
            ctrl  = QString::fromStdString(b.controller_name);
            zname = QString::fromStdString(b.zone_name);
            zleds = (int)b.zone_leds;
        }
        m.insert(QStringLiteral("state"), state);
        m.insert(QStringLiteral("connected"), conn);
        m.insert(QStringLiteral("controller_name"), ctrl);
        m.insert(QStringLiteral("zone_name"), zname);
        m.insert(QStringLiteral("zone_leds"), zleds);
        out.push_back(m);
    }
    return out;
}

QVariantList LayoutController::WsEmitters(
    const std::string& iid) const
{
    /* Emitter positions/addresses only exist in the RESOLVED
       scene — read the engine-applied document (model_ keeps
       it fed in both modes) and flatten every object under
       the instance path. */
    QVariantList out;
    const nlohmann::json& doc = model_.Applied();
    if(!doc.is_object())
    {
        return out;
    }
    const std::string prefix = iid + "/";
    int flat = 0;
    const auto objs = doc.find("objects");
    if(objs == doc.end() || !objs->is_array())
    {
        return out;
    }
    for(const nlohmann::json& o : *objs)
    {
        if(!o.is_object())
        {
            continue;
        }
        const std::string oid =
            o.value("id", std::string());
        if(oid != iid &&
           oid.compare(0, prefix.size(), prefix) != 0)
        {
            continue;
        }
        const std::string bind =
            o.value("binding", std::string());
        const auto ems = o.find("emitters");
        if(ems == o.end() || !ems->is_array())
        {
            continue;
        }
        for(const nlohmann::json& e : *ems)
        {
            QVariantMap m;
            m.insert(QStringLiteral("index"), flat++);
            m.insert(QStringLiteral("address"),
                     e.value("address", -1));
            m.insert(QStringLiteral("group"),
                     QString::fromStdString(
                         e.value("group", std::string())));
            m.insert(QStringLiteral("object"),
                     QString::fromStdString(oid));
            m.insert(QStringLiteral("binding"),
                     QString::fromStdString(bind));
            out.push_back(m);
        }
    }
    return out;
}

QString LayoutController::WsUniqueBindingId(
    const QString& base) const
{
    const studio::StudioDocument* w = WsDoc();
    QString cand = base.isEmpty()
                       ? QStringLiteral("b_layout")
                       : base;
    auto exists = [&](const QString& id) {
        return w != nullptr &&
               w->bindings.count(id.toStdString()) > 0;
    };
    if(!exists(cand))
    {
        return cand;
    }
    for(int i = 2;; ++i)
    {
        const QString c =
            QStringLiteral("%1_%2").arg(cand).arg(i);
        if(!exists(c))
        {
            return c;
        }
    }
}

/*---- bridge feeds ------------------------------------------*/

void LayoutController::OnSceneChanged()
{
    const QVariantMap s = bridge_->scene();
    model_.LoadApplied(
        VariantToJson(s.value(QStringLiteral("document"))));
    emit itemsChanged();
    emit selectedChanged();
    emit historyChanged();   /* undo docs still hold snapshots,
                                counts only change on edits —
                                but canUndo/canRedo reads stay
                                honest across LOAD_SCENE      */
}

void LayoutController::OnSnapshotsChanged()
{
    std::vector<nlohmann::json> snaps;
    for(const QVariant& sv : bridge_->snapshots())
    {
        snaps.push_back(VariantToJson(sv));
    }
    model_.SetSnapshots(std::move(snaps));
    emit itemsChanged();
    emit selectedChanged();
}

/*---- replies ------------------------------------------------*/

void LayoutController::OnReplyReceived(int id,
                                       const QString& type,
                                       const QVariantMap& body)
{
    const bool ok = (type == QLatin1String("OK"));
    if(id != 0 && pending_identify_ids_.remove(id))
    {
        if(ok)
        {
            const bool active =
                body.value(QStringLiteral("identifying"))
                    .toBool();
            const QString tag = IdentifyTag(
                body.value(QStringLiteral("controller"))
                    .toMap());
            /* Rekey the entry on this controller — a second
               identify on ANOTHER controller adds a row
               rather than clobbering the first's banner. */
            for(int i = identifying_.size() - 1; i >= 0; --i)
            {
                if(IdentifyTag(
                       identifying_[i].toMap().value(
                           QStringLiteral("controller"))
                           .toMap()) == tag)
                {
                    identifying_.removeAt(i);
                }
            }
            if(active)
            {
                QVariantMap entry = body;
                /* No push arrives when the blink ends — the
                   entry clears on the acknowledged duration
                   (plus one 250 ms half-period of slack). */
                entry.insert(
                    QStringLiteral("deadline_ms"),
                    QDateTime::currentMSecsSinceEpoch() +
                        body.value(
                            QStringLiteral("duration_ms"))
                            .toLongLong() + 250);
                identifying_.push_back(entry);
                identify_timer_.start();
                SetStatus(QStringLiteral(
                              "identifying controller…"));
            }
            else
            {
                /* Cancel acknowledgement — drop the row like
                   an expiry: the reply still carries the key,
                   which must NOT read as an active banner. */
                SetStatus(QStringLiteral(
                              "identification cleared"));
            }
            if(identifying_.isEmpty())
            {
                identify_timer_.stop();
            }
            emit identifyingChanged();
        }
        else
        {
            SetLastError(body.value(QStringLiteral("message"))
                             .toString(),
                         QStringLiteral("engine"));
        }
        return;
    }
    if(id != pending_.id || pending_.id == 0)
    {
        return;
    }
    const Pending p = pending_;
    pending_        = Pending{};

    if(ok)
    {
        if(p.kind == QLatin1String("undo"))
        {
            /* Restore happened: the popped undo entry's
               PRE-edit document became applied; what the
               user saw before the undo migrates to redo. */
            model_.PopUndo();
            model_.PushRedo(p.label.toStdString(),
                            p.prior_doc);
            SetStatus(QStringLiteral("undid: %1").arg(p.label));
        }
        else if(p.kind == QLatin1String("redo"))
        {
            model_.PopRedo();
            model_.PushUndoEntry(p.label.toStdString(),
                                 p.prior_doc);
            SetStatus(QStringLiteral("redid: %1").arg(p.label));
        }
        else
        {
            /* LAYOUT_COMMAND OK: the undo entry recorded at
               issue already holds the pre-edit document; the
               engine's scene event lands the new applied. */
            SetStatus(QStringLiteral("applied: %1").arg(p.label));
        }
        SetLastError(QString());
    }
    else
    {
        /* A refused edit leaves no ghost history. */
        if(p.kind == QLatin1String("apply"))
        {
            model_.DropUndo();
        }
        SetLastError(body.value(QStringLiteral("message"))
                         .toString(),
                     QStringLiteral("engine"));
    }
    emit busyChanged();
    emit historyChanged();
}

/*---- helpers ------------------------------------------------*/

void LayoutController::SetLastError(const QString& e,
                                    const QString& source)
{
    const QString src =
        e.isEmpty() ? QString() : source;
    if(last_error_ != e || last_error_source_ != src)
    {
        last_error_        = e;
        last_error_source_ = src;
        emit lastErrorChanged();
    }
}

void LayoutController::SetStatus(const QString& s)
{
    if(status_text_ != s)
    {
        status_text_ = s;
        emit statusTextChanged();
    }
}

bool LayoutController::Issue(const LayoutOp& op)
{
    if(pending_.id != 0)
    {
        SetLastError(QStringLiteral(
            "an edit is still applying — wait for it"),
                     QStringLiteral("input"));
        return false;
    }
    /* Record the restore point BEFORE issuing so a
       dispatch failure can still cleanly drop it. */
    model_.PushUndo(op.label);
    const QString verb = QString::fromStdString(
        op.body.value("verb", std::string()));
    const int req = bridge_->layoutCommand(
        verb, JsonToVariant(op.body).toMap());
    if(req <= 0)
    {
        model_.DropUndo();
        SetLastError(QStringLiteral(
            "not sent — no live engine connection"),
                     QStringLiteral("connection"));
        return false;
    }
    pending_ = Pending{ req, QStringLiteral("apply"),
                        QString::fromStdString(op.label),
                        nlohmann::json(nullptr) };
    SetLastError(QString());
    emit busyChanged();
    emit historyChanged();
    return true;
}

QString LayoutController::UniqueBindingId(
    const QString& base) const
{
    QString cand = base.isEmpty()
                       ? QStringLiteral("b_layout")
                       : base;
    const nlohmann::json bindings = model_.Bindings();
    auto exists = [&](const QString& id) {
        for(const nlohmann::json& b : bindings)
        {
            if(b.is_object() &&
               b.value("id", std::string()) ==
                   id.toStdString())
            {
                return true;
            }
        }
        return false;
    };
    if(!exists(cand))
    {
        return cand;
    }
    for(int i = 2;; ++i)
    {
        const QString c = QStringLiteral("%1_%2").arg(cand)
                              .arg(i);
        if(!exists(c))
        {
            return c;
        }
    }
}

/*---- selection ----------------------------------------------*/

void LayoutController::select(const QString& objectId)
{
    if(selected_id_ != objectId)
    {
        selected_id_ = objectId;
        emit selectedChanged();
    }
}

/*---- ops -----------------------------------------------------*/

bool LayoutController::moveObject(const QString& objectId,
                                  double x, double z)
{
    if(WsMode())
    {
        const studio::StudioDocument* w = WsDoc();
        if(w == nullptr)
        {
            SetLastError(QStringLiteral("no workspace draft "
                                        "loaded"),
                         QStringLiteral("input"));
            return false;
        }
        const auto it = w->devices.find(objectId.toStdString());
        if(it == w->devices.end())
        {
            SetLastError(QStringLiteral("unknown scene object"),
                         QStringLiteral("input"));
            return false;
        }
        /* Desk-plane move: y stays where the draft put it. */
        const studio::Vec3 pos{ (float)x, it->second.position.y,
                                (float)z };
        return WsApply(
            ws_->Session().Editor().SetPosition(
                objectId.toStdString(), pos),
            QStringLiteral("move %1").arg(objectId));
    }
    const nlohmann::json* o =
        model_.FindObject(objectId.toStdString());
    if(o == nullptr)
    {
        SetLastError(QStringLiteral("unknown scene object"),
                     QStringLiteral("input"));
        return false;
    }
    /* y (height) stays where the document put it — the canvas
       edits the desk plane only. */
    const double y = o->value("position", nlohmann::json())
                         .value("y", 0.0);
    LayoutOp op;
    std::string err;
    if(!model_.BuildSetPosition(objectId.toStdString(),
                                x, y, z, op, err))
    {
        SetLastError(QString::fromStdString(err),
                     QStringLiteral("input"));
        return false;
    }
    return Issue(op);
}

bool LayoutController::rotateObject(const QString& objectId,
                                    double yawDeg)
{
    if(WsMode())
    {
        const studio::StudioDocument* w = WsDoc();
        if(w == nullptr)
        {
            SetLastError(QStringLiteral("no workspace draft "
                                        "loaded"),
                         QStringLiteral("input"));
            return false;
        }
        const auto it = w->devices.find(objectId.toStdString());
        if(it == w->devices.end())
        {
            SetLastError(QStringLiteral("unknown scene object"),
                         QStringLiteral("input"));
            return false;
        }
        const studio::Vec3 rot{ it->second.rotation_deg.x,
                                (float)yawDeg,
                                it->second.rotation_deg.z };
        return WsApply(
            ws_->Session().Editor().SetRotation(
                objectId.toStdString(), rot),
            QStringLiteral("rotate %1").arg(objectId));
    }
    const nlohmann::json* o =
        model_.FindObject(objectId.toStdString());
    if(o == nullptr)
    {
        SetLastError(QStringLiteral("unknown scene object"),
                     QStringLiteral("input"));
        return false;
    }
    const nlohmann::json r =
        o->value("rotation", nlohmann::json());
    LayoutOp op;
    std::string err;
    if(!model_.BuildSetOrientation(objectId.toStdString(),
                                   r.value("x", 0.0), yawDeg,
                                   r.value("z", 0.0), op, err))
    {
        SetLastError(QString::fromStdString(err),
                     QStringLiteral("input"));
        return false;
    }
    return Issue(op);
}

bool LayoutController::setMirrorTarget(const QString& objectId,
                                       const QString& targetId)
{
    if(WsMode())
    {
        const studio::StudioDocument* w = WsDoc();
        if(w == nullptr)
        {
            SetLastError(QStringLiteral("no workspace draft "
                                        "loaded"),
                         QStringLiteral("input"));
            return false;
        }
        const std::string id  = objectId.toStdString();
        const std::string tgt = targetId.toStdString();
        if(w->devices.count(id) == 0)
        {
            SetLastError(QStringLiteral("unknown scene object"),
                         QStringLiteral("input"));
            return false;
        }
        if(!tgt.empty())
        {
            if(tgt == id)
            {
                SetLastError(QStringLiteral("an object cannot "
                                            "mirror itself"),
                             QStringLiteral("input"));
                return false;
            }
            if(w->devices.count(tgt) == 0)
            {
                SetLastError(QStringLiteral("unknown mirror "
                                            "target"),
                             QStringLiteral("input"));
                return false;
            }
            /* No mirror chains — the resolver refuses them;
               refuse here first (the UI is the first
               rejection boundary). */
            const auto ts = w->device_settings.find(tgt);
            if(ts != w->device_settings.end() &&
               !ts->second.mirror_of.empty())
            {
                SetLastError(QStringLiteral("mirror chains are "
                                            "not supported"),
                             QStringLiteral("input"));
                return false;
            }
        }
        return WsDocEdit(
            tgt.empty() ? "unlink mirror" : "set mirror",
            [id, tgt](studio::StudioDocument& d) {
                studio::DeviceSettings& s = d.device_settings[id];
                if(s.mirror_of == tgt)
                {
                    return false;
                }
                s.mirror_of = tgt;
                return true;
            });
    }
    LayoutOp op;
    std::string err;
    if(!model_.BuildSetMirror(objectId.toStdString(),
                              targetId.toStdString(), op, err))
    {
        SetLastError(QString::fromStdString(err),
                     QStringLiteral("input"));
        return false;
    }
    return Issue(op);
}

bool LayoutController::unlinkMirror(const QString& objectId)
{
    return setMirrorTarget(objectId, QString());
}

bool LayoutController::assignBinding(const QString& objectId,
                                     const QString& bindingId)
{
    if(WsMode())
    {
        const studio::StudioDocument* w = WsDoc();
        if(w == nullptr)
        {
            SetLastError(QStringLiteral("no workspace draft "
                                        "loaded"),
                         QStringLiteral("input"));
            return false;
        }
        const std::string id  = objectId.toStdString();
        const std::string bid = bindingId.toStdString();
        if(w->devices.count(id) == 0)
        {
            SetLastError(QStringLiteral("unknown scene object"),
                         QStringLiteral("input"));
            return false;
        }
        const auto sit = w->device_settings.find(id);
        if(sit == w->device_settings.end() ||
           sit->second.zones.empty())
        {
            /* Zone ids come from the device type — not the
               document — so an instance that never had a zone
               row cannot be bound from here (the engine
               invents nothing either). */
            SetLastError(QStringLiteral("the instance declares "
                                        "no zones to bind"),
                         QStringLiteral("input"));
            return false;
        }
        if(!bid.empty() && w->bindings.count(bid) == 0)
        {
            SetLastError(QStringLiteral("unknown binding"),
                         QStringLiteral("input"));
            return false;
        }
        return WsDocEdit("assign binding",
            [id, bid](studio::StudioDocument& d) {
                bool changed = false;
                for(auto& z : d.device_settings[id].zones)
                {
                    if(z.second.binding != bid)
                    {
                        z.second.binding = bid;
                        changed = true;
                    }
                }
                return changed;
            });
    }
    LayoutOp op;
    std::string err;
    if(!model_.BuildSetBinding(objectId.toStdString(),
                               bindingId.toStdString(), op, err))
    {
        SetLastError(QString::fromStdString(err),
                     QStringLiteral("input"));
        return false;
    }
    return Issue(op);
}

bool LayoutController::bindNewController(
    const QString& objectId, const QString& newBindingId,
    const QVariantMap& controllerKey)
{
    if(WsMode())
    {
        const studio::StudioDocument* w = WsDoc();
        if(w == nullptr)
        {
            SetLastError(QStringLiteral("no workspace draft "
                                        "loaded"),
                         QStringLiteral("input"));
            return false;
        }
        const std::string id = objectId.toStdString();
        if(w->devices.count(id) == 0)
        {
            SetLastError(QStringLiteral("unknown scene object"),
                         QStringLiteral("input"));
            return false;
        }
        const auto sit = w->device_settings.find(id);
        if(sit == w->device_settings.end() ||
           sit->second.zones.empty())
        {
            SetLastError(QStringLiteral("the instance declares "
                                        "no zones to bind"),
                         QStringLiteral("input"));
            return false;
        }
        const QString bid = newBindingId.isEmpty()
            ? WsUniqueBindingId(
                  QStringLiteral("b_%1").arg(objectId))
            : newBindingId;
        if(w->bindings.count(bid.toStdString()) > 0)
        {
            SetLastError(QStringLiteral("binding id already "
                                        "exists"),
                         QStringLiteral("input"));
            return false;
        }
        /* controllerKey is a snapshot's `key` map —
           {device,serial,location,zone} — the same identity
           triple + zone the engine derives a binding from. */
        studio::DeviceBinding b;
        b.id              = bid.toStdString();
        b.controller_name = controllerKey
            .value(QStringLiteral("device")).toString()
            .toStdString();
        b.serial    = controllerKey
            .value(QStringLiteral("serial")).toString()
            .toStdString();
        b.location  = controllerKey
            .value(QStringLiteral("location")).toString()
            .toStdString();
        b.zone_name = controllerKey
            .value(QStringLiteral("zone")).toString()
            .toStdString();
        const std::string bids = bid.toStdString();
        return WsDocEdit("bind controller",
            [id, bids, b](studio::StudioDocument& d) {
                d.bindings[bids] = b;
                for(auto& z : d.device_settings[id].zones)
                {
                    z.second.binding = bids;
                }
                return true;
            });
    }
    QString bid = newBindingId;
    if(bid.isEmpty())
    {
        bid = UniqueBindingId(
            QStringLiteral("b_%1").arg(objectId));
    }
    LayoutOp op;
    std::string err;
    if(!model_.BuildSetBindingFromController(
            objectId.toStdString(), bid.toStdString(),
            VariantToJson(controllerKey), op, err))
    {
        SetLastError(QString::fromStdString(err),
                     QStringLiteral("input"));
        return false;
    }
    return Issue(op);
}

bool LayoutController::calibrateEmitter(const QString& objectId,
                                        int emitterIndex,
                                        int address)
{
    if(WsMode())
    {
        const studio::StudioDocument* w = WsDoc();
        if(w == nullptr)
        {
            SetLastError(QStringLiteral("no workspace draft "
                                        "loaded"),
                         QStringLiteral("input"));
            return false;
        }
        const std::string iid = objectId.toStdString();
        if(w->devices.count(iid) == 0)
        {
            SetLastError(QStringLiteral("unknown scene object"),
                         QStringLiteral("input"));
            return false;
        }
        /* Per-emitter address freedom does not exist in the
           compact model — the authored unit is the zone's
           addr_base, so calibration shifts the whole zone. */
        if(address < 0)
        {
            SetLastError(QStringLiteral("workspace calibration "
                                        "moves a zone's address "
                                        "base — per-emitter "
                                        "render-only is not "
                                        "expressible"),
                         QStringLiteral("input"));
            return false;
        }
        const QVariantList ems = WsEmitters(iid);
        if(emitterIndex < 0 || emitterIndex >= ems.size())
        {
            SetLastError(QStringLiteral("no emitter %1 on the "
                                        "applied scene — "
                                        "resolve needs the "
                                        "engine")
                             .arg(emitterIndex),
                         QStringLiteral("input"));
            return false;
        }
        const QVariantMap em = ems.at(emitterIndex).toMap();
        const int  old_addr =
            em.value(QStringLiteral("address"), -1).toInt();
        const QString bind =
            em.value(QStringLiteral("binding")).toString();
        if(old_addr < 0 || bind.isEmpty())
        {
            SetLastError(QStringLiteral("that emitter is "
                                        "render-only or "
                                        "unbound"),
                         QStringLiteral("input"));
            return false;
        }
        /* Bound check against the emitter's own binding (the
           engine validates again on commit). */
        const auto bit =
            w->bindings.find(bind.toStdString());
        if(bit != w->bindings.end() &&
           bit->second.zone_leds > 0 &&
           address >= (int)bit->second.zone_leds)
        {
            SetLastError(QStringLiteral("address out of range "
                                        "for the bound zone"),
                         QStringLiteral("input"));
            return false;
        }
        /* Which zone row? The emitter's object binds ONE
           binding; exactly one settings zone row may carry it
           or the write would be ambiguous. */
        const auto sit = w->device_settings.find(iid);
        std::string zone_id;
        int  rows = 0;
        int  cur_base = 0;
        if(sit != w->device_settings.end())
        {
            for(const auto& z : sit->second.zones)
            {
                if(z.second.binding == bind.toStdString())
                {
                    zone_id  = z.first;
                    cur_base = z.second.addr_base;
                    ++rows;
                }
            }
        }
        if(rows != 1)
        {
            SetLastError(QStringLiteral("cannot attribute the "
                                        "emitter to one zone "
                                        "row — ambiguous "
                                        "calibration"),
                         QStringLiteral("input"));
            return false;
        }
        const int new_base = cur_base + (address - old_addr);
        if(new_base < 0)
        {
            SetLastError(QStringLiteral("address out of range "
                                        "for the zone's base"),
                         QStringLiteral("input"));
            return false;
        }
        return WsDocEdit("calibrate zone",
            [iid, zone_id, new_base](studio::StudioDocument& d) {
                studio::ZoneSetting& z =
                    d.device_settings[iid].zones[zone_id];
                if(z.addr_base == new_base)
                {
                    return false;
                }
                z.addr_base = new_base;
                return true;
            });
    }
    LayoutOp op;
    std::string err;
    if(!model_.BuildCalibrateLed(objectId.toStdString(),
                                 emitterIndex, address, op,
                                 err))
    {
        SetLastError(QString::fromStdString(err),
                     QStringLiteral("input"));
        return false;
    }
    return Issue(op);
}

/*---- undo/redo -----------------------------------------------*/

void LayoutController::undo()
{
    if(WsMode())
    {
        /* Shared timeline — the draft edit is local; undo is
           a doc revert, no engine round-trip. */
        ws_->undo();
        emit itemsChanged();
        emit selectedChanged();
        emit historyChanged();
        return;
    }
    if(pending_.id != 0)
    {
        SetLastError(QStringLiteral(
            "an edit is still applying — wait for it"),
                     QStringLiteral("input"));
        return;
    }
    const LayoutEditModel::HistoryEntry* e =
        model_.PeekUndo();
    if(e == nullptr)
    {
        return;
    }
    /* Revert = the recorded pre-edit document, applied through
       the same engine path as any scene (SET_SCENE). prior
       preserves the CURRENT applied doc for the redo entry
       — captured at issue, since the confirm arrives only
       after the engine has already overwritten `scene`. */
    const nlohmann::json prior = model_.Applied();
    const int req = bridge_->setScene(
        JsonToVariant(e->doc).toMap());
    if(req <= 0)
    {
        SetLastError(QStringLiteral(
            "not sent — no live engine connection"),
                     QStringLiteral("connection"));
        return;
    }
    pending_ = Pending{ req, QStringLiteral("undo"),
                        QString::fromStdString(e->label),
                        prior };
    emit busyChanged();
}

void LayoutController::redo()
{
    if(WsMode())
    {
        ws_->redo();
        emit itemsChanged();
        emit selectedChanged();
        emit historyChanged();
        return;
    }
    if(pending_.id != 0)
    {
        SetLastError(QStringLiteral(
            "an edit is still applying — wait for it"),
                     QStringLiteral("input"));
        return;
    }
    const LayoutEditModel::HistoryEntry* e =
        model_.PeekRedo();
    if(e == nullptr)
    {
        return;
    }
    const nlohmann::json prior = model_.Applied();
    const int req = bridge_->setScene(
        JsonToVariant(e->doc).toMap());
    if(req <= 0)
    {
        SetLastError(QStringLiteral(
            "not sent — no live engine connection"),
                     QStringLiteral("connection"));
        return;
    }
    pending_ = Pending{ req, QStringLiteral("redo"),
                        QString::fromStdString(e->label),
                        prior };
    emit busyChanged();
}

void LayoutController::clearHistory()
{
    if(WsMode())
    {
        ws_->Session().ClearHistory();
        ws_->NoteDraftEdited();
        return;
    }
    model_.ClearHistory();
    emit historyChanged();
}

/*---- identification ------------------------------------------*/

int LayoutController::identify(const QVariantMap& controllerKey,
                               const QString& zone, int ledIndex,
                               int durationMs)
{
    QVariantMap args;
    args.insert(QStringLiteral("controller"), controllerKey);
    if(!zone.isEmpty())
    {
        args.insert(QStringLiteral("zone"), zone);
    }
    if(ledIndex >= 0)
    {
        args.insert(QStringLiteral("led_index"), ledIndex);
    }
    if(durationMs > 0)
    {
        args.insert(QStringLiteral("duration_ms"), durationMs);
    }
    const int req = bridge_->layoutCommand(
        QStringLiteral("identify"), args);
    if(req <= 0)
    {
        SetLastError(QStringLiteral(
            "not sent — no live engine connection"),
                     QStringLiteral("connection"));
        return 0;
    }
    pending_identify_ids_.insert(req);
    return req;
}

int LayoutController::cancelIdentify(
    const QVariantMap& controllerKey)
{
    QVariantMap args;
    args.insert(QStringLiteral("controller"), controllerKey);
    args.insert(QStringLiteral("duration_ms"), 0);
    const int req = bridge_->layoutCommand(
        QStringLiteral("identify"), args);
    if(req <= 0)
    {
        return 0;
    }
    /* Route the cancel reply through the same pending set —
       its {identifying:false} acknowledgement clears the
       row like an expiry. */
    pending_identify_ids_.insert(req);
    return req;
}

QString LayoutController::IdentifyTag(const QVariantMap& key)
{
    /* device+serial+location — the key triple the engine
       uses to name a controller. */
    return key.value(QStringLiteral("device")).toString() +
           QLatin1Char('\x1f') +
           key.value(QStringLiteral("serial")).toString() +
           QLatin1Char('\x1f') +
           key.value(QStringLiteral("location")).toString();
}

QVariantMap LayoutController::identifyingFor(
    const QVariantMap& controllerKey) const
{
    const QString tag = IdentifyTag(controllerKey);
    for(const QVariant& v : identifying_)
    {
        const QVariantMap e = v.toMap();
        if(IdentifyTag(
               e.value(QStringLiteral("controller")).toMap())
           == tag)
        {
            return e;
        }
    }
    return QVariantMap();
}

void LayoutController::PruneIdentifies()
{
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    bool changed = false;
    for(int i = identifying_.size() - 1; i >= 0; --i)
    {
        if(now >= identifying_[i].toMap().value(
                      QStringLiteral("deadline_ms"))
                      .toLongLong())
        {
            identifying_.removeAt(i);
            changed = true;
        }
    }
    if(changed)
    {
        emit identifyingChanged();
        SetStatus(QStringLiteral("identification ended"));
    }
    if(identifying_.isEmpty())
    {
        identify_timer_.stop();
    }
}

void LayoutController::reload()
{
    if(WsMode())
    {
        ws_->reload();
        return;
    }
    bridge_->getScene();
}

/*---- getters ---------------------------------------------------*/

bool LayoutController::hasScene() const
{
    if(WsMode())
    {
        return ws_->Session().HasBaseline();
    }
    return model_.HasScene();
}

QVariantList LayoutController::items() const
{
    if(WsMode())
    {
        return WsItems();
    }
    QVariantList out;
    for(const LayoutItem& it : model_.Items())
    {
        QVariantMap m;
        m.insert(QStringLiteral("id"),
                 QString::fromStdString(it.id));
        m.insert(QStringLiteral("label"),
                 QString::fromStdString(it.label));
        m.insert(QStringLiteral("kind"),
                 QString::fromStdString(it.kind));
        m.insert(QStringLiteral("x"), it.x);
        m.insert(QStringLiteral("y"), it.y);
        m.insert(QStringLiteral("z"), it.z);
        m.insert(QStringLiteral("rx"), it.rx);
        m.insert(QStringLiteral("ry"), it.ry);
        m.insert(QStringLiteral("rz"), it.rz);
        m.insert(QStringLiteral("w"), it.w);
        m.insert(QStringLiteral("d"), it.d);
        m.insert(QStringLiteral("binding"),
                 QString::fromStdString(it.binding));
        m.insert(QStringLiteral("mirror_of"),
                 QString::fromStdString(it.mirror_of));
        m.insert(QStringLiteral("geometry"),
                 QString::fromStdString(it.geometry));
        m.insert(QStringLiteral("emitter_count"),
                 it.emitter_count);
        m.insert(QStringLiteral("addressed_count"),
                 it.addressed_count);
        m.insert(QStringLiteral("verified"), it.verified);
        m.insert(QStringLiteral("visible"), it.visible);
        m.insert(QStringLiteral("state"),
                 QString::fromStdString(it.state));
        m.insert(QStringLiteral("connected"), it.connected);
        m.insert(QStringLiteral("controller_name"),
                 QString::fromStdString(it.controller_name));
        m.insert(QStringLiteral("zone_name"),
                 QString::fromStdString(it.zone_name));
        m.insert(QStringLiteral("zone_leds"),
                 (int)it.zone_leds);
        out.push_back(m);
    }
    return out;
}

QVariantList LayoutController::bindings() const
{
    if(WsMode())
    {
        QVariantList out;
        const studio::StudioDocument* w = WsDoc();
        if(w == nullptr)
        {
            return out;
        }
        for(const auto& kv : w->bindings)
        {
            QVariantMap b;
            b.insert(QStringLiteral("id"),
                     QString::fromStdString(kv.first));
            b.insert(QStringLiteral("controller_name"),
                     QString::fromStdString(
                         kv.second.controller_name));
            b.insert(QStringLiteral("vendor"),
                     QString::fromStdString(kv.second.vendor));
            b.insert(QStringLiteral("serial"),
                     QString::fromStdString(kv.second.serial));
            b.insert(QStringLiteral("location"),
                     QString::fromStdString(kv.second.location));
            b.insert(QStringLiteral("device_type"),
                     kv.second.device_type);
            b.insert(QStringLiteral("zone_name"),
                     QString::fromStdString(
                         kv.second.zone_name));
            b.insert(QStringLiteral("zone_leds"),
                     (int)kv.second.zone_leds);
            out.push_back(b);
        }
        return out;
    }
    return JsonToVariant(model_.Bindings()).toList();
}

bool LayoutController::busy() const
{
    if(WsMode())
    {
        /* Draft edits are local; "busy" only covers the
           save/fetch round-trips the controller owns. */
        return ws_->saving() || ws_->loading();
    }
    return pending_.id != 0;
}

QString LayoutController::selectedId() const
{
    return selected_id_;
}

QVariantMap LayoutController::selectedItem() const
{
    for(const QVariant& v : items())
    {
        if(v.toMap().value(QStringLiteral("id")).toString() ==
           selected_id_)
        {
            return v.toMap();
        }
    }
    return QVariantMap{};
}

QVariantList LayoutController::selectedEmitters() const
{
    if(WsMode())
    {
        return WsEmitters(selected_id_.toStdString());
    }
    return JsonToVariant(
               model_.ObjectEmitters(selected_id_.toStdString()))
        .toList();
}

QVariantList LayoutController::linkSources() const
{
    if(WsMode())
    {
        /* Mirror sources = other device instances of the
           draft (groups excluded — they own no output). */
        QVariantList out;
        const studio::StudioDocument* w = WsDoc();
        if(w == nullptr)
        {
            return out;
        }
        const std::string sel = selected_id_.toStdString();
        for(const auto& kv : w->devices)
        {
            if(kv.first == sel || kv.second.type == "group")
            {
                continue;
            }
            QVariantMap m;
            m.insert(QStringLiteral("id"),
                     QString::fromStdString(kv.first));
            m.insert(QStringLiteral("label"),
                     QString::fromStdString(kv.first));
            out.push_back(m);
        }
        return out;
    }
    QVariantList out;
    for(const std::string& id :
        model_.LinkSources(selected_id_.toStdString()))
    {
        QVariantMap m;
        m.insert(QStringLiteral("id"),
                 QString::fromStdString(id));
        const nlohmann::json* o = model_.FindObject(id);
        m.insert(QStringLiteral("label"),
                 QString::fromStdString(
                     o && o->contains("label") &&
                             (*o)["label"].is_string()
                         ? o->value("label", std::string())
                         : id));
        out.push_back(m);
    }
    return out;
}

int LayoutController::zoneBound() const
{
    if(WsMode())
    {
        /* Display bound: the selected instance's bound zone's
           declared LED count (calibrateEmitter validates per
           emitter against its own binding). */
        const studio::StudioDocument* w = WsDoc();
        if(w == nullptr)
        {
            return 0;
        }
        const auto sit =
            w->device_settings.find(selected_id_.toStdString());
        if(sit == w->device_settings.end())
        {
            return 0;
        }
        int bound = 0;
        for(const auto& z : sit->second.zones)
        {
            const auto bit = w->bindings.find(z.second.binding);
            if(bit != w->bindings.end() &&
               (int)bit->second.zone_leds > bound)
            {
                bound = (int)bit->second.zone_leds;
            }
        }
        return bound;
    }
    return (int)model_.BoundZoneLeds(selected_id_.toStdString());
}

bool LayoutController::canUndo() const
{
    if(WsMode())
    {
        return ws_->canUndo();
    }
    return model_.PeekUndo() != nullptr;
}

bool LayoutController::canRedo() const
{
    if(WsMode())
    {
        return ws_->canRedo();
    }
    return model_.PeekRedo() != nullptr;
}

QString LayoutController::undoLabel() const
{
    if(WsMode())
    {
        return ws_->undoLabel();
    }
    const LayoutEditModel::HistoryEntry* e =
        model_.PeekUndo();
    return e ? QString::fromStdString(e->label) : QString();
}

QString LayoutController::redoLabel() const
{
    if(WsMode())
    {
        return ws_->redoLabel();
    }
    const LayoutEditModel::HistoryEntry* e =
        model_.PeekRedo();
    return e ? QString::fromStdString(e->label) : QString();
}

int LayoutController::undoCount() const
{
    if(WsMode())
    {
        return (int)ws_->Session().UndoCount();
    }
    return (int)model_.UndoCount();
}

int LayoutController::redoCount() const
{
    if(WsMode())
    {
        return (int)ws_->Session().RedoCount();
    }
    return (int)model_.RedoCount();
}

QString LayoutController::lastError() const
{
    return last_error_;
}

QString LayoutController::lastErrorSource() const
{
    return last_error_source_;
}

QString LayoutController::statusText() const
{
    return status_text_;
}

QVariantList LayoutController::identifying() const
{
    return identifying_;
}

} /* namespace ui */
} /* namespace openrgb2 */
