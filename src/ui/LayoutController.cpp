/*---------------------------------------------------------*\
||| LayoutController.cpp                                      ||
|||                                                           ||
|||   SPDX-License-Identifier: GPL-2.0-or-later               ||
\*---------------------------------------------------------*/

#include "LayoutController.h"

#include "EngineBridge.h"
#include "JsonVariant.h"

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

    /* Engine-side expiry is authoritative — the timer only
       drops the stale banner (the blink itself already
       ended). The acknowledged duration plus one half-period
       of slack keeps it visible through the last phase. */
    identify_timer_.setSingleShot(true);
    connect(&identify_timer_, &QTimer::timeout, this, [this] {
        identifying_ = QVariantMap{};
        emit identifyingChanged();
        SetStatus(QStringLiteral("identification ended"));
    });

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
        pending_identify_id_ = 0;
        identify_timer_.stop();
        identifying_ = QVariantMap{};
        emit identifyingChanged();
        SetLastError(QStringLiteral("engine connection lost"));
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
    if(id == pending_identify_id_ && id != 0)
    {
        pending_identify_id_ = 0;
        if(ok)
        {
            const bool active =
                body.value(QStringLiteral("identifying"))
                    .toBool();
            if(active)
            {
                identifying_ = body;
                emit identifyingChanged();
                /* No push arrives when the blink ends — the
                   banner clears on the acknowledged duration
                   (plus one 250 ms half-period of slack). */
                identify_timer_.start(
                    (int)body.value(
                             QStringLiteral("duration_ms"))
                        .toLongLong() + 250);
                SetStatus(QStringLiteral(
                              "identifying controller…"));
            }
            else
            {
                /* Cancel acknowledgement — clear like an
                   expiry: the reply still carries the key,
                   which must NOT read as an active banner. */
                identify_timer_.stop();
                identifying_ = QVariantMap{};
                emit identifyingChanged();
                SetStatus(QStringLiteral(
                              "identification cleared"));
            }
        }
        else
        {
            SetLastError(body.value(QStringLiteral("message"))
                             .toString());
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
                         .toString());
    }
    emit busyChanged();
    emit historyChanged();
}

/*---- helpers ------------------------------------------------*/

void LayoutController::SetLastError(const QString& e)
{
    if(last_error_ != e)
    {
        last_error_ = e;
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
            "an edit is still applying — wait for it"));
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
            "not sent — no live engine connection"));
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
        SetLastError(QStringLiteral("unknown scene object"));
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
        SetLastError(QString::fromStdString(err));
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
        SetLastError(QStringLiteral("unknown scene object"));
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
        SetLastError(QString::fromStdString(err));
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
        SetLastError(QString::fromStdString(err));
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
        SetLastError(QString::fromStdString(err));
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
        SetLastError(QString::fromStdString(err));
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
        SetLastError(QString::fromStdString(err));
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
            "an edit is still applying — wait for it"));
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
            "not sent — no live engine connection"));
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
            "an edit is still applying — wait for it"));
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
            "not sent — no live engine connection"));
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
            "not sent — no live engine connection"));
        return 0;
    }
    pending_identify_id_ = req;
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
    /* Route the cancel reply through the same pending slot —
       its {identifying:false} acknowledgement clears the
       banner like an expiry (it used to be dropped, leaving
       identifying_ stuck active). */
    pending_identify_id_ = req;
    return req;
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

QString LayoutController::statusText() const
{
    return status_text_;
}

QVariantMap LayoutController::identifying() const
{
    return identifying_;
}

} /* namespace ui */
} /* namespace openrgb2 */
