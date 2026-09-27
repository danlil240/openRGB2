/*---------------------------------------------------------*\
||| LayoutController.h                                        ||
|||                                                           ||
|||   QObject half of the Layout view (Task 4.4, plan      ||
|||   section 5): wires the Qt-free LayoutEditModel to     ||
|||   EngineBridge so arrangement edits, orientation,      ||
|||   mirror links, bindings, calibration, and LED         ||
|||   identification all travel as real LAYOUT_COMMAND /   ||
|||   SET_SCENE traffic (docs/ipc-protocol.md §5):         ||
|||                                                           ||
|||   - every layout op issues ONE LAYOUT_COMMAND and      ||
|||     applies on the engine's OK; the engine's scene     ||
|||     event refreshes the applied document               ||
|||   - ops are single-flight (one mutation in flight at   ||
|||     a time — undo's snapshot contract assumes the      ||
|||     applied doc can't move between issue and reply)    ||
|||   - undo/redo is whole-document snapshot reversion     ||
|||     through SET_SCENE: the only honest inverse for     ||
|||     ops like set_mirror that destroy emitter/binding   ||
|||     data. A new edit clears the redo timeline; the     ||
|||     stack is bounded (LayoutEditModel::kHistoryBound)  ||
|||   - an ERROR reply drops the recorded history entry    ||
|||     and surfaces lastError — refused edits leave no    ||
|||     ghost undo steps                                   ||
|||   - calibration fields are range-checked in the model  ||
|||     BEFORE anything is sent (the engine validates      ||
|||     again — the UI is the first rejection boundary)    ||
|||                                                           ||
|||   The UI is a CLIENT: nothing here touches hardware —  ||
|||   identification rides the engine's own frame path.    ||
|||                                                           ||
|||   SPDX-License-Identifier: GPL-2.0-or-later               ||
\*---------------------------------------------------------*/

#pragma once

#include "LayoutEditModel.h"

#include <QObject>
#include <QSet>
#include <QString>
#include <QTimer>
#include <QVariantList>
#include <QVariantMap>

namespace openrgb2
{
namespace ui
{

class EngineBridge;

class LayoutController : public QObject
{
    Q_OBJECT

    Q_PROPERTY(bool hasScene READ hasScene NOTIFY itemsChanged)
    /* Per-object view rows for the canvas + inspector —
       [{id,label,kind,x,y,z,rx,ry,rz,w,d,binding,mirror_of,
         geometry,emitter_count,addressed_count,verified,
         visible,state,connected,controller_name,zone_name,
         zone_leds}]. */
    Q_PROPERTY(QVariantList items READ items NOTIFY itemsChanged)
    /* The document's bindings (picker model). */
    Q_PROPERTY(QVariantList bindings READ bindings
                   NOTIFY itemsChanged)
    /* One mutation in flight (apply/undo/redo). */
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)

    Q_PROPERTY(QString selectedId READ selectedId
                   NOTIFY selectedChanged)
    Q_PROPERTY(QVariantMap selectedItem READ selectedItem
                   NOTIFY selectedChanged)
    /* Emitters of the selected object [{index,address,group}]. */
    Q_PROPERTY(QVariantList selectedEmitters READ selectedEmitters
                   NOTIFY selectedChanged)
    /* Device objects eligible as the selected object's mirror
       source [{id,label}]. */
    Q_PROPERTY(QVariantList linkSources READ linkSources
                   NOTIFY selectedChanged)
    /* LED bound for the selected object's zone — 0 = unknown
       (calibration of positive addresses is refused). */
    Q_PROPERTY(int zoneBound READ zoneBound NOTIFY selectedChanged)

    Q_PROPERTY(bool canUndo READ canUndo NOTIFY historyChanged)
    Q_PROPERTY(bool canRedo READ canRedo NOTIFY historyChanged)
    Q_PROPERTY(QString undoLabel READ undoLabel
                   NOTIFY historyChanged)
    Q_PROPERTY(QString redoLabel READ redoLabel
                   NOTIFY historyChanged)
    Q_PROPERTY(int undoCount READ undoCount NOTIFY historyChanged)
    Q_PROPERTY(int redoCount READ redoCount NOTIFY historyChanged)

    Q_PROPERTY(QString lastError READ lastError
                   NOTIFY lastErrorChanged)
    /* Where the last error came from — a UI-side validation
       refusal is a DIFFERENT state than an engine rejection
       and never reads as one (Task 4.5): "" no error,
       "input" refused before send, "engine" the engine's
       ERROR reply, "connection" nothing could be sent. */
    Q_PROPERTY(QString lastErrorSource READ lastErrorSource
                   NOTIFY lastErrorChanged)
    /* Last command outcome — "applied: move fan0",
       "identify acknowledged", or the engine's error. */
    Q_PROPERTY(QString statusText READ statusText
                   NOTIFY statusTextChanged)
    /* The ACTIVE identifications as last acknowledged — a
       list because a second controller's overlay doesn't
       replace the first's (Task 4.8 minor): each entry is
       the reply body {controller:{device,serial,location},
       zone, led_start, led_count, duration_ms} plus a local
       deadline_ms the pruning timer clears against (the
       blink end has no push event). Empty when none/
       cancelled/expired. */
    Q_PROPERTY(QVariantList identifying READ identifying
                   NOTIFY identifyingChanged)

public:
    explicit LayoutController(EngineBridge* bridge,
                              QObject* parent = nullptr);

    /*---- selection -----------------------------------------*/
    Q_INVOKABLE void select(const QString& objectId);

    /*---- arrangement ops (immediate — LAYOUT_COMMAND) ------
       All return false without sending when the model
       refuses the input (unknown object, bad bounds, busy). */
    /* Top-view move: x/z are meters; the object's y (height)
       is preserved from the applied document. */
    Q_INVOKABLE bool moveObject(const QString& objectId,
                                double x, double z);
    /* Absolute yaw (desk rotation, degrees); pitch/roll are
       preserved. */
    Q_INVOKABLE bool rotateObject(const QString& objectId,
                                  double yawDeg);
    Q_INVOKABLE bool setMirrorTarget(const QString& objectId,
                                     const QString& targetId);
    Q_INVOKABLE bool unlinkMirror(const QString& objectId);
    /* Point the object at an existing binding id, or "" to
       unbind. */
    Q_INVOKABLE bool assignBinding(const QString& objectId,
                                   const QString& bindingId);
    /* Create a NEW binding from a live controller
       (controllerKey = a snapshot's `key` map) and point the
       object at it. newBindingId "" auto-generates a unique
       id. */
    Q_INVOKABLE bool bindNewController(
        const QString& objectId, const QString& newBindingId,
        const QVariantMap& controllerKey);
    /* Calibration: address -1 = render-only; the model
       range-checks against the bound zone BEFORE send. */
    Q_INVOKABLE bool calibrateEmitter(const QString& objectId,
                                      int emitterIndex,
                                      int address);

    /*---- undo/redo -----------------------------------------*/
    Q_INVOKABLE void undo();
    Q_INVOKABLE void redo();
    Q_INVOKABLE void clearHistory();

    /*---- identification (transient — never undoable) -------
       Returns the request correlator (>0) or 0 when nothing
       was sent. zone "" = whole controller; ledIndex <0 =
       whole scope; durationMs <=0 cancels. */
    Q_INVOKABLE int identify(const QVariantMap& controllerKey,
                             const QString& zone, int ledIndex,
                             int durationMs);
    Q_INVOKABLE int cancelIdentify(
        const QVariantMap& controllerKey);
    /* The active identify entry for ONE controller key, or
       an empty map — per-row UI (Stop button, status text)
       needs its own target, not the whole list. */
    Q_INVOKABLE QVariantMap identifyingFor(
        const QVariantMap& controllerKey) const;

    Q_INVOKABLE void reload();   /* GET_SCENE pull */

    /*---- state ----------------------------------------------*/
    bool         hasScene() const;
    QVariantList items() const;
    QVariantList bindings() const;
    bool         busy() const;
    QString      selectedId() const;
    QVariantMap  selectedItem() const;
    QVariantList selectedEmitters() const;
    QVariantList linkSources() const;
    int          zoneBound() const;
    bool         canUndo() const;
    bool         canRedo() const;
    QString      undoLabel() const;
    QString      redoLabel() const;
    int          undoCount() const;
    int          redoCount() const;
    QString      lastError() const;
    QString      lastErrorSource() const;
    QString      statusText() const;
    QVariantList identifying() const;

    LayoutEditModel& ModelForTesting() { return model_; }

signals:
    void itemsChanged();
    void selectedChanged();
    void busyChanged();
    void historyChanged();
    void lastErrorChanged();
    void statusTextChanged();
    void identifyingChanged();

private:
    void OnSceneChanged();
    void OnSnapshotsChanged();
    void OnReplyReceived(int id, const QString& type,
                         const QVariantMap& body);
    bool Issue(const LayoutOp& op);
    /* source: "" | "input" | "engine" | "connection" — see
       lastErrorSource. */
    void SetLastError(const QString& e,
                      const QString& source = QString());
    void SetStatus(const QString& s);
    QString UniqueBindingId(const QString& base) const;
    /* Identify bookkeeping — entries keyed by controller;
       the timer sweeps expired deadlines (the engine's
       blink end has no push event). */
    static QString IdentifyTag(const QVariantMap& key);
    void           PruneIdentifies();

    EngineBridge*   bridge_;
    LayoutEditModel model_;
    QString         selected_id_;
    QString         last_error_;
    QString         last_error_source_;
    QString         status_text_;
    QVariantList    identifying_;

    /* Single-flight mutation tracking: undo/redo/apply all
       serialize through here so the recorded before/after
       documents can't be crossed by a second in-flight
       edit. */
    struct Pending
    {
        int           id = 0;
        QString       kind;        /* "apply"|"undo"|"redo" */
        QString       label;
        nlohmann::json prior_doc;  /* undo/redo revert doc */
    };
    Pending         pending_;

    /* Identify replies are informational only (transient op,
       no history) — every in-flight correlate lands here so
       a cancel and a start can't cross each other's replies. */
    QSet<int>       pending_identify_ids_;

    /* The engine's overlays expire on their own clock with
       no push event — this repeating sweep drops each local
       entry past its acknowledged duration (plus one
       half-period of slack), running only while the list is
       non-empty. */
    QTimer          identify_timer_;
};

} /* namespace ui */
} /* namespace openrgb2 */
