/*---------------------------------------------------------*\
||| CalibrationController.cpp                                 ||
|||                                                           ||
|||   studio-import Task 11 — the wizard's engine bridge.   ||
|||   One status shape serves every surface: the           ||
|||   calibration_* replies and the session-targeted        ||
|||   "calibration" event both feed ApplyStatus, so a       ||
|||   poll (calibInfo), a reply, and a push can never       ||
|||   disagree about what the session is doing.             ||
|||                                                           ||
|||   SPDX-License-Identifier: GPL-2.0-or-later               ||
|\*---------------------------------------------------------*/

#include "CalibrationController.h"

#include <QVariant>

namespace openrgb2
{
namespace ui
{

CalibrationController::CalibrationController(EngineBridge* bridge,
                                             QObject* parent)
    : QObject(parent)
    , bridge_(bridge)
{
    if(bridge_ == nullptr)
    {
        return;
    }
    connect(bridge_, &EngineBridge::replyReceived,
            this, &CalibrationController::OnReplyReceived);
    connect(bridge_, &EngineBridge::eventReceived,
            this, &CalibrationController::OnEventReceived);
    connect(bridge_, &EngineBridge::connectionStateChanged,
            this, [this] {
                if(!bridge_->connected() &&
                   (active_ || starting_ || ending_ || !token_.isEmpty()))
                {
                    /* The engine ends the lease on session
                       teardown — the view clears honestly,
                       never a phantom "still calibrating". */
                    active_ = starting_ = ending_ = false;
                    owner_busy_ = false;
                    token_.clear();
                    leds_ = 0;
                    last_shown_ = -2;
                    message_ = QStringLiteral(
                        "engine disconnected — session ended");
                    emit calibrationChanged();
                }
            });
}

/*---------------------------------------------------------*\
||| Diagnostics pickers — snapshot-backed                    ||
\*---------------------------------------------------------*/

QVariantMap CalibrationController::ControllerRow(int index) const
{
    if(bridge_ == nullptr || index < 0)
    {
        return {};
    }
    const QVariantList snaps = bridge_->snapshots();
    if(index >= snaps.size())
    {
        return {};
    }
    const QVariantMap snap = snaps.at(index).toMap();
    QVariantMap row;
    row["index"] = index;
    row["label"] = QStringLiteral("[%1] %2")
                       .arg(index)
                       .arg(snap.value("name").toString());
    row["name"]  = snap.value("name");
    row["key"]   = snap.value("key");
    return row;
}

QVariantMap CalibrationController::ZoneRow(int controllerIndex,
                                           int zoneIndex) const
{
    if(bridge_ == nullptr || controllerIndex < 0 ||
       zoneIndex < 0)
    {
        return {};
    }
    const QVariantList snaps = bridge_->snapshots();
    if(controllerIndex >= snaps.size())
    {
        return {};
    }
    const QVariantList zones =
        snaps.at(controllerIndex).toMap().value("zones").toList();
    if(zoneIndex >= zones.size())
    {
        return {};
    }
    const QVariantMap z = zones.at(zoneIndex).toMap();
    const int leds = z.value("led_count").toInt();
    QVariantMap row;
    row["index"] = zoneIndex;
    row["name"]  = z.value("name").toString();
    row["leds"]  = leds;
    row["label"] = QStringLiteral("%1 (%2 LEDs)")
                       .arg(z.value("name").toString())
                       .arg(leds);
    return row;
}

QVariantList CalibrationController::diagControllers() const
{
    QVariantList out;
    if(bridge_ == nullptr)
    {
        return out;
    }
    const QVariantList snaps = bridge_->snapshots();
    for(int i = 0; i < snaps.size(); ++i)
    {
        out.append(ControllerRow(i));
    }
    return out;
}

QVariantList CalibrationController::diagZones(int controllerIndex) const
{
    QVariantList out;
    if(bridge_ == nullptr || controllerIndex < 0)
    {
        return out;
    }
    const QVariantList snaps = bridge_->snapshots();
    if(controllerIndex >= snaps.size())
    {
        return out;
    }
    const QVariantList zones =
        snaps.at(controllerIndex).toMap().value("zones").toList();
    for(int i = 0; i < zones.size(); ++i)
    {
        out.append(ZoneRow(controllerIndex, i));
    }
    return out;
}

void CalibrationController::diagRefresh()
{
    /* The snapshot list itself is push-fed — re-arming the
       combos just re-reads it (the QML's refresh() calls this
       before rebuilding the models). */
    emit calibrationChanged();
}

/*---------------------------------------------------------*\
||| Session verbs                                            ||
\*---------------------------------------------------------*/

QVariantMap CalibrationController::calibBegin(int controllerIndex,
                                              int zoneIndex)
{
    QVariantMap out;
    if(bridge_ == nullptr || !bridge_->connected())
    {
        out["ok"]    = false;
        out["error"] = QStringLiteral("engine not connected");
        return out;
    }
    const QVariantMap ctrl = ControllerRow(controllerIndex);
    const QVariantMap zone = ZoneRow(controllerIndex, zoneIndex);
    if(ctrl.isEmpty() || zone.isEmpty())
    {
        out["ok"]    = false;
        out["error"] = QStringLiteral(
            "pick a controller and zone first");
        return out;
    }
    QVariantMap args;
    args["controller"] = ctrl.value("key");
    args["zone"]       = zone.value("name");
    const int id = bridge_->workspaceCommand(
        QStringLiteral("calibration_begin"), args);
    if(id <= 0)
    {
        out["ok"]    = false;
        out["error"] = QStringLiteral(
            "the engine did not accept the request");
        return out;
    }
    pending_begin_req_ = id;
    /* Source parity: begin is async — the caller sees
       "pending", activation lands via the reply/event. */
    starting_ = true;
    emit calibrationChanged();
    out["ok"]      = true;
    out["pending"] = true;
    return out;
}

void CalibrationController::calibShow(int led)
{
    if(bridge_ == nullptr || token_.isEmpty())
    {
        return;
    }
    QVariantMap args;
    args["token"] = token_;
    args["led"]   = led;
    bridge_->workspaceCommand(QStringLiteral("calibration_show"),
                              args);
}

void CalibrationController::calibEnd()
{
    if(bridge_ == nullptr)
    {
        return;
    }
    QVariantMap args;
    if(!token_.isEmpty())
    {
        args["token"] = token_;
    }
    bridge_->workspaceCommand(QStringLiteral("calibration_end"),
                              args);
}

QVariantMap CalibrationController::calibInfo() const
{
    QVariantMap info;
    info["active"]          = active_;
    info["starting"]        = starting_;
    info["ending"]          = ending_;
    /* The source surface's `paused` = hardware still held by
       the end drain — our "ending" is exactly that window. */
    info["paused"]          = ending_;
    info["leds"]            = leds_;
    info["last_shown"]      = last_shown_;
    info["frames_written"]  = (qint64)frames_written_;
    info["controller_name"] = controller_name_;
    info["zone"]            = zone_name_;
    info["zone_index"]      = zone_index_;
    info["generation"]      = (qint64)generation_;
    info["owner"]           = owner_busy_;
    info["message"]         = message_;
    return info;
}

/*---------------------------------------------------------*\
||| Wire feeders                                             ||
\*---------------------------------------------------------*/

void CalibrationController::ApplyStatus(const QVariantMap& body)
{
    /* One authoritative shape: owner-side replies carry the
       token + full fields; a non-owner sees {active:false,
       owner:true} and nothing else. */
    const bool mine = !body.value("owner").toBool();
    owner_busy_ = body.value("owner").toBool();
    if(mine)
    {
        active_   = body.value("active").toBool();
        starting_ = body.value("starting").toBool();
        ending_   = body.value("ending").toBool();
        leds_     = body.value("led_count").toInt();
        if(body.contains("last_shown"))
        {
            last_shown_ = body.value("last_shown").toInt();
        }
        zone_name_  = body.value("zone").toString();
        if(body.contains("zone_index"))
        {
            zone_index_ = body.value("zone_index").toInt();
        }
        controller_name_ =
            body.value("controller_name").toString();
        generation_ =
            (quint64)body.value("generation").toLongLong();
        frames_written_ =
            (quint64)body.value("frames_written").toLongLong();
        if(body.contains("token"))
        {
            token_ = body.value("token").toString();
        }
        if(body.contains("message"))
        {
            message_ = body.value("message").toString();
        }
        if(!active_ && !starting_ && !ending_)
        {
            /* Terminal push: the lease is gone — the token
               must not survive for a stale show. */
            token_.clear();
        }
    }
    else if(body.contains("message"))
    {
        message_ = body.value("message").toString();
    }
    emit calibrationChanged();
}

void CalibrationController::OnReplyReceived(
    int requestId, const QString& typeName,
    const QVariantMap& body)
{
    /* Error replies carry no status body — surface the honest
       message on the request they answer. */
    if(typeName == QLatin1String("ERROR"))
    {
        if(requestId == pending_begin_req_)
        {
            pending_begin_req_ = 0;
            starting_ = false;
            message_  = body.value("message").toString();
            emit calibrationChanged();
            emit statusMessage(message_);
        }
        return;
    }
    if(requestId == pending_begin_req_)
    {
        pending_begin_req_ = 0;
    }
    /* A calibration status body — all four verbs reply with
       it. Guarded by the field set so unrelated replies
       (another controller's workspace verbs) never wedge
       the session view. */
    if(body.contains("active") && body.contains("owner"))
    {
        ApplyStatus(body);
    }
}

void CalibrationController::OnEventReceived(const QString& kind,
                                            const QVariantMap& data)
{
    if(kind == QLatin1String("calibration"))
    {
        ApplyStatus(data);
    }
}

void CalibrationController::FeedStatusForTesting(
    const QVariantMap& body)
{
    ApplyStatus(body);
}

} /* namespace ui */
} /* namespace openrgb2 */
