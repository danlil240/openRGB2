/*---------------------------------------------------------*\
||| LayoutController.cpp                                      ||
|||                                                           ||
|||   SPDX-License-Identifier: GPL-2.0-or-later               ||
\*---------------------------------------------------------*/

#include "LayoutController.h"

#include "EngineBridge.h"
#include "JsonVariant.h"

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
    bridge_->getScene();
}

/*---- getters ---------------------------------------------------*/

bool LayoutController::hasScene() const
{
    return model_.HasScene();
}

QVariantList LayoutController::items() const
{
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
    return JsonToVariant(model_.Bindings()).toList();
}

bool LayoutController::busy() const
{
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
    return JsonToVariant(
               model_.ObjectEmitters(selected_id_.toStdString()))
        .toList();
}

QVariantList LayoutController::linkSources() const
{
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
    return (int)model_.BoundZoneLeds(selected_id_.toStdString());
}

bool LayoutController::canUndo() const
{
    return model_.PeekUndo() != nullptr;
}

bool LayoutController::canRedo() const
{
    return model_.PeekRedo() != nullptr;
}

QString LayoutController::undoLabel() const
{
    const LayoutEditModel::HistoryEntry* e =
        model_.PeekUndo();
    return e ? QString::fromStdString(e->label) : QString();
}

QString LayoutController::redoLabel() const
{
    const LayoutEditModel::HistoryEntry* e =
        model_.PeekRedo();
    return e ? QString::fromStdString(e->label) : QString();
}

int LayoutController::undoCount() const
{
    return (int)model_.UndoCount();
}

int LayoutController::redoCount() const
{
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
