/*---------------------------------------------------------*\
||| CalibrationController.h                                   ||
|||                                                           ||
|||   The Qt Quick bridge half of the physical LED-         ||
|||   calibration wizard (studio-import Task 11): the       ||
|||   OpenRGB2 port of the source plugin's studioHost       ||
|||   calib* surface (calibBegin/calibShow/calibEnd/        ||
|||   calibInfo + the diag controller/zone pickers —        ||
|||   provenance ledger).                                   ||
|||                                                           ||
|||   Every verb rides WORKSPACE_COMMAND on the live        ||
|||   engine — the wizard NEVER owns a hardware path of     ||
|||   its own:                                              ||
|||                                                           ||
|||   - calibBegin(ctrl,zone) -> "calibration_begin"        ||
|||     {controller:<DeviceKey>, zone:<name>}; the reply    ||
|||     carries the session-bound opaque token + status     ||
|||   - calibShow(led) -> "calibration_show" {token, led};  ||
|||     latest-only coalescing lives engine-side            ||
|||   - calibEnd()   -> "calibration_end" {token}           ||
|||   - calibInfo()  -> the LAST status the replies and     ||
|||     the session-targeted "calibration" event published  ||
|||     (the QML's syncSession poll reads this)             ||
|||                                                           ||
|||   Hardware truth — active/starting/ending, LED count,   ||
|||   last shown index, transport rejections — comes ONLY   ||
|||   from engine status; the controller fabricates        ||
|||   nothing. A disconnect clears the session view         ||
|||   honestly (the engine ends the session on teardown).  ||
|||                                                           ||
|||   SPDX-License-Identifier: GPL-2.0-or-later               ||
|\*---------------------------------------------------------*/

#pragma once

#include "../EngineBridge.h"

#include <QObject>
#include <QString>
#include <QVariantList>
#include <QVariantMap>

namespace openrgb2
{
namespace ui
{

class CalibrationController : public QObject
{
    Q_OBJECT

    /* Live session view — every change publishes through
       calibrationChanged (the calibrator's Connections +
       pollTimer both land on this). */
    Q_PROPERTY(bool active        READ active        NOTIFY calibrationChanged)
    Q_PROPERTY(bool starting      READ starting      NOTIFY calibrationChanged)
    Q_PROPERTY(bool ending        READ ending        NOTIFY calibrationChanged)
    Q_PROPERTY(bool ownerBusy     READ ownerBusy     NOTIFY calibrationChanged)
    Q_PROPERTY(QString message    READ message       NOTIFY calibrationChanged)
    Q_PROPERTY(int leds           READ leds          NOTIFY calibrationChanged)
    Q_PROPERTY(int lastShown      READ lastShown     NOTIFY calibrationChanged)
    Q_PROPERTY(QString zoneName   READ zoneName      NOTIFY calibrationChanged)
    Q_PROPERTY(QString controllerName
                READ controllerName NOTIFY calibrationChanged)

public:
    explicit CalibrationController(EngineBridge* bridge,
                                   QObject* parent = nullptr);

    bool    active()        const { return active_;        }
    bool    starting()      const { return starting_;      }
    bool    ending()        const { return ending_;        }
    /* A calibration session SOME OTHER client holds — the
       wizard's busy affordance. */
    bool    ownerBusy()     const { return owner_busy_;    }
    QString message()       const { return message_;       }
    int     leds()          const { return leds_;          }
    int     lastShown()     const { return last_shown_;    }
    QString zoneName()      const { return zone_name_;     }
    QString controllerName() const { return controller_name_; }

    /*---- diagnostics pickers --------------------------------
       Rows for the wizard's controller/zone combos — the
       same snapshot surface the Devices page renders.
       diagControllers: {index, label, key{device,serial,
       location,zone}}; diagZones: {index, label, leds, name}. */
    Q_INVOKABLE QVariantList diagControllers() const;
    Q_INVOKABLE QVariantList diagZones(int controllerIndex) const;
    /* Picker re-arm — topology push events refresh the bridge
       snapshots; this just re-reads them. */
    Q_INVOKABLE void         diagRefresh();

    /*---- session verbs ---------------------------------------
       Source-parity surface (tst_calibrator.qml drives exactly
       this shape through the hostOverride seam). */
    /* {ok,pending} on send — activation lands async; the
       reply/event drive the status. {ok:false,error} on an
       immediate refusal (offline, unwired). */
    Q_INVOKABLE QVariantMap calibBegin(int controllerIndex,
                                       int zoneIndex);
    /* Latest-only LED request; -1 blacks the zone. */
    Q_INVOKABLE void        calibShow(int led);
    Q_INVOKABLE void        calibEnd();
    /* The last published status — {active, starting, ending,
       paused, leds, last_shown, frames_written, controller_name,
       zone, zone_index, generation, owner, message}. */
    Q_INVOKABLE QVariantMap calibInfo() const;

    /*---- test hook -------------------------------------------*/
    /* Feed a status body without the wire — the offline suite
       drives session transitions deterministically. */
    void FeedStatusForTesting(const QVariantMap& body);

signals:
    void calibrationChanged();
    void statusMessage(const QString& text);

private:
    void OnReplyReceived(int requestId, const QString& typeName,
                         const QVariantMap& body);
    void OnEventReceived(const QString& kind,
                         const QVariantMap& data);
    /* Apply one status body (reply or event) — the single
       shape all four verbs and the push share. */
    void ApplyStatus(const QVariantMap& body);
    /* Picker resolution shared by calibBegin/diagZones. */
    QVariantMap ControllerRow(int index) const;
    QVariantMap ZoneRow(int controllerIndex, int zoneIndex) const;

    EngineBridge* bridge_ = nullptr;

    /*---- engine-published status (never fabricated) ----------*/
    bool     active_     = false;
    bool     starting_   = false;
    bool     ending_     = false;
    bool     owner_busy_ = false;
    QString  token_;                 /* opaque lease material  */
    QString  message_;
    QString  zone_name_;
    QString  controller_name_;
    int      zone_index_      = -1;
    int      leds_            = 0;
    int      last_shown_      = -2;  /* -2 none, -1 dark, >=0  */
    quint64  generation_      = 0;
    quint64  frames_written_  = 0;

    /* Begin correlation — the error reply must land on the
       request it answers. */
    int      pending_begin_req_ = 0;
};

} /* namespace ui */
} /* namespace openrgb2 */
