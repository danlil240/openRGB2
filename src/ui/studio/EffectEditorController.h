/*---------------------------------------------------------*\
||| EffectEditorController.h                                 ||
|||                                                          ||
|||   The studio effect-layer editing surface (studio-    ||
|||   import Task 6): ONE QObject adapter that wraps the   ||
|||   Qt-free studio::EditorController's layer ops bound   ||
|||   to the shared WorkspaceEditSession draft — the same  ||
|||   Editor() ObjectEditorController and the Lighting     ||
|||   page use. Every committed op produces exactly ONE    ||
|||   undo record through WorkspaceEditSession::CommitEdit ||
|||   / CommitDocEdit; this controller owns NO document    ||
|||   and NO undo stack.                                   ||
|||                                                          ||
|||   Adapted from the source-of-record plugin/SceneBridge ||
|||   effect half + editor/EffectLayerModel (provenance    ||
|||   ledger): same slot surface — the EFFECTIVE stack     ||
|||   (authored inline layers, else the resolved named     ||
|||   look) is what reads answer; the FIRST mutating op    ||
|||   materializes the resolved stack inside the same undo ||
|||   record so one undo returns to registry resolution.   ||
|||   Continuous edits (slider scrubs, stop drags) run     ||
|||   through BeginLayerGesture/preview ops/               ||
|||   CommitLayerGesture — one record per completed drag.  ||
|||                                                          ||
|||   Personal looks: saveLookAs serializes the effective  ||
|||   stack as literal EffectDocument JSON and installs it ||
|||   through the engine's resource_install job            ||
|||   (collision-safe, revision-gated — the engine owns    ||
|||   the library). On the terminal job event the saved    ||
|||   look's document is registered UI-side so the preset  ||
|||   resolves immediately, then adopted through ONE       ||
|||   undoable SetLayers({}, landed_id) record — undo      ||
|||   restores the previous stack AND preset together.     ||
|||                                                          ||
|||   Offline rule: without a WorkspaceController/draft    ||
|||   every mutation refuses honestly and the read side    ||
|||   reports empty — nothing is fabricated.               ||
|||                                                          ||
|||   SPDX-License-Identifier: GPL-2.0-or-later               ||
\*---------------------------------------------------------*/

#pragma once

#include "EffectLayerModel.h"

#include "../EngineBridge.h"
#include "../WorkspaceController.h"

#include "config/StudioConfig.h"
#include "effects/EffectJson.h"
#include "presets/EffectRegistry.h"
#include "scene/SceneTypes.h"

#include <QColor>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>

#include <optional>
#include <set>
#include <string>
#include <vector>

namespace openrgb2
{
namespace ui
{

class EffectEditorController : public QObject
{
    Q_OBJECT

    /* The effective-layer row model (LayerStack panel). */
    Q_PROPERTY(QObject* layerModel READ layerModel CONSTANT)
    /* A draft exists and this controller can mutate it. */
    Q_PROPERTY(bool ready READ ready NOTIFY readyChanged)
    Q_PROPERTY(bool gestureActive READ gestureActive
                   NOTIFY gestureChanged)
    /* Draft effect.preset — the named look ("" = none). */
    Q_PROPERTY(QString presetId READ presetId NOTIFY stackChanged)
    /* True when the draft carries an authored inline stack
       (layers win over the named look). */
    Q_PROPERTY(bool hasInlineLayers READ hasInlineLayers
                   NOTIFY stackChanged)
    Q_PROPERTY(int layerCount READ layerCount NOTIFY stackChanged)
    Q_PROPERTY(int selectedLayer READ selectedLayer
                   WRITE setSelectedLayer NOTIFY selectedLayerChanged)
    /* Valid layer-target terms: resolved object ids + geometry
       tags + emitter groups (matches the engine's dead-target
       guard surface). */
    Q_PROPERTY(QVariantList targetIds READ targetIds
                   NOTIFY stackChanged)
    /* Personal-look shelf rows: [{id,name,needs,fromFile}] —
       the engine catalog's file-supplied effect entries merged
       with locally-known looks. */
    Q_PROPERTY(QVariantList looks READ looks NOTIFY looksChanged)
    /* A resource_install job is in flight for saveLookAs. */
    Q_PROPERTY(bool savingLook READ savingLook
                   NOTIFY lookSaveChanged)
    /* Draft inputs.decay_pct (50..300, default 100) — the
       persisted ripple-decay scale the engine applies to
       runtime layer copies. */
    Q_PROPERTY(int decayPct READ decayPct NOTIFY draftInputsChanged)
    Q_PROPERTY(QString statusText READ statusText
                   NOTIFY statusChanged)

public:
    explicit EffectEditorController(EngineBridge* bridge,
                                    WorkspaceController* ws,
                                    QObject* parent = nullptr);

    QObject*     layerModel()     { return &layer_model_; }
    bool         ready() const;
    bool         gestureActive() const;
    QString      presetId() const;
    bool         hasInlineLayers() const;
    int          layerCount() const;
    int          selectedLayer() const   { return selected_layer_; }
    QVariantList targetIds() const;
    QVariantList looks() const           { return looks_; }
    bool         savingLook() const      { return saving_look_; }
    int          decayPct() const;
    QString      statusText() const      { return status_text_; }

    /*---- reads -------------------------------------------------*/
    /* Full detail row for the inspector — {index, primitive,
       enabled, blend, opacity, space, speed, scale, phase,
       density, origin{x,y,z}, direction{x,y,z}, path[{x,y,z}],
       palette[{pos,color}], targets[...], source, seed}. */
    Q_INVOKABLE QVariantMap layerAt(int index) const;
    /* Reactive-input badge state for a source name — honest
       engine-reported {enabled, ready, status}; unknown sources
       and an unwired surface report not-ready. */
    Q_INVOKABLE QVariantMap inputSourceState(const QString& source)
        const;

    /*---- look selection ----------------------------------------*/
    /* Undoable preset pick — clears the authored inline stack,
       resets the seed on a different id (the source bridge's
       playPreset contract, one record). */
    Q_INVOKABLE bool selectPreset(const QString& id);
    /* Reset to the named look — clears the inline stack through
       ONE undoable record. The QML dialog gives the explicit
       notice before calling this (authored layers are replaced). */
    Q_INVOKABLE bool resetToPreset();
    /* Deterministic re-roll — the persisted seed advances through
       the same HashU32 step as the source bridge's remix. */
    Q_INVOKABLE bool remix();
    /* Install the effective stack as a personal look
       (resource_install job, revision-gated); on success the
       landed id is adopted as the preset through one undoable
       record. Immediate failure returns a map with
       ok:false + errors; job outcomes land in lookSaveResult. */
    Q_INVOKABLE QVariantMap saveLookAs(const QString& id,
                                       const QString& name);
    Q_INVOKABLE void refreshLooks();   /* catalog kind=effects     */

    /*---- gestures — one undo record per completed drag ---------*/
    Q_INVOKABLE void beginLayerGesture();
    Q_INVOKABLE void commitLayerGesture(const QString& label);
    Q_INVOKABLE void cancelLayerGesture();

    /*---- stack + per-layer ops ---------------------------------*/
    Q_INVOKABLE void moveLayer(int from, int to);
    Q_INVOKABLE void addLayer(const QString& primitive);
    Q_INVOKABLE void removeLayer(int index);
    Q_INVOKABLE void setLayerEnabled(int index, bool on);
    Q_INVOKABLE void setLayerBlend(int index, const QString& mode);
    Q_INVOKABLE void setLayerOpacity(int index, double v);
    /* field: "speed" | "scale" | "phase" | "density" | "seed". */
    Q_INVOKABLE void setLayerField(int index, const QString& field,
                                   double v);
    Q_INVOKABLE void setLayerSpace(int index, const QString& space);
    Q_INVOKABLE void setLayerOrigin(int index,
                                    double x, double y, double z);
    Q_INVOKABLE void setLayerDirection(int index,
                                       double x, double y, double z);
    Q_INVOKABLE void setLayerSource(int index, const QString& src);
    Q_INVOKABLE void setLayerTargets(int index,
                                     const QStringList& targets);

    /*---- palette-stop ops --------------------------------------*/
    Q_INVOKABLE void addLayerStop(int index, double pos,
                                  const QColor& color);
    Q_INVOKABLE void removeLayerStop(int index, int stop);
    /* Returns the stop's index after the re-sort (-1 refused). */
    Q_INVOKABLE int  moveLayerStop(int index, int stop, double pos);
    Q_INVOKABLE void setLayerStopColor(int index, int stop,
                                       const QColor& color);

    /*---- directional flow (Wave card) ---------------------------
       Drives every enabled `wave` layer of the effective stack
       together (core EditorController::SetFlow — one undo record
       per call). flowState() = {available, direction{x,y,z},
       colors[hex], speed (m/s), wavelength (m), keyboardLayer
       (index of a static layer targeting keyboard_body, -1 none),
       keyboardOn}. */
    Q_INVOKABLE QVariantMap flowState() const;
    Q_INVOKABLE bool setFlowDirection(double x, double y, double z);
    Q_INVOKABLE bool setFlowColors(const QStringList& colors);
    Q_INVOKABLE bool setFlowSpeed(double metersPerSecond);
    Q_INVOKABLE bool setFlowWavelength(double meters);

    /*---- path-point ops (comet) ---------------------------------*/
    Q_INVOKABLE void addLayerPathPoint(int index,
                                       double x, double y, double z);
    Q_INVOKABLE void setLayerPathPoint(int index, int pt,
                                       double x, double y, double z);
    Q_INVOKABLE void removeLayerPathPoint(int index, int pt);

    /*---- persisted input settings (draft inputs.*) --------------
       The runtime toggles ride REACTIVE_COMMAND; these mirror the
       authored settings into the draft so a committed workspace
       restores them — one record each (call at gesture end for
       sliders). */
    Q_INVOKABLE void setDecayPct(int pct);
    Q_INVOKABLE void setInputEnabled(const QString& input, bool on);
    Q_INVOKABLE void setInputSensitivity(int pct);
    Q_INVOKABLE void setInputScreenIndex(int index);

    /*---- undo/redo — the shared session's history ---------------*/
    Q_INVOKABLE void undo();
    Q_INVOKABLE void redo();

    void         setSelectedLayer(int index);

    /*---- test hooks --------------------------------------------*/
    EffectLayerModel&  ModelForTesting() { return layer_model_; }
    /* Install an extra look document (tests register saved-look
       fixtures the engine library would supply). */
    void RegisterLookForTesting(const studio::EffectDocument& d);
    std::vector<studio::EffectLayer> EffectiveLayersForTesting()
        const { return EffectiveLayers(); }
    /* Feed a catalog kind=effects entries array through the same
       path the wire reply uses (offline tests can't correlate a
       real request). */
    void FeedEffectsCatalogForTesting(const QVariantList& entries)
        { ApplyEffectsCatalog(entries); }
    /* Latch a fake in-flight install so the disconnect-unlatch
       path is exercisable offline. */
    void SetSavingForTesting()
        { pending_save_req_ = 1; pending_save_job_ = 7;
          pending_save_id_ = QStringLiteral("test_look");
          saving_look_ = true; }
    /* How many catalog refreshes the controller asked for —
       the save-completion path's re-request is observable even
       with a disconnected bridge. */
    int CatalogRequestsForTesting() const { return catalog_requests_; }

signals:
    void readyChanged();
    void gestureChanged();
    void stackChanged();
    void selectedLayerChanged();
    void looksChanged();
    void lookSaveChanged();
    /* Terminal saveLookAs outcome: {ok, id, errors[]}. */
    void lookSaveResult(const QVariantMap& result);
    void draftInputsChanged();
    void statusChanged();
    void statusMessage(const QString& text);

private:
    WorkspaceEditSession*      Session() const;
    studio::EditorController*  Editor() const;
    const studio::StudioDocument* WsDoc() const;

    /* The EFFECTIVE stack: authored inline layers when present,
       else the named look resolved through the UI-side registries
       (locally-saved looks first, then the packaged registry). */
    std::vector<studio::EffectLayer> EffectiveLayers() const;
    /* LayerResolver the shared editor calls when the first
       mutating op materializes a named look's stack. */
    static bool ResolveLayers(const std::string& preset_id,
                              unsigned int seed,
                              std::vector<studio::EffectLayer>& out,
                              void* ctx);
    bool ResolveLayersImpl(const std::string& preset_id,
                           unsigned int seed,
                           std::vector<studio::EffectLayer>& out) const;

    /* Commit one edit through the shared session + refresh the
       model at the right granularity. */
    void ApplyOp(std::optional<studio::EditorEdit>&& e);
    /* Wave-card op: refuses during a gesture, true when it made
       a record. */
    bool ApplyFlow(const studio::EditorController::FlowEdit& f);
    void RefreshModel();
    /* One draft doc-edit record for inputs.* fields. */
    bool CommitInputs(const std::string& label,
                      const std::function<bool(
                          studio::StudioDocument&)>& mut);

    /* Build shelf rows + feed the local look registry from a
       catalog kind=effects entries array (reply handler and the
       test hook share this one path). */
    void ApplyEffectsCatalog(const QVariantList& entries);
    void SetStatus(const QString& s);
    void OnReplyReceived(int requestId, const QString& typeName,
                         const QVariantMap& body);
    void OnEventReceived(const QString& kind,
                         const QVariantMap& data);
    void OnWorkspaceDraftEdited();
    void OnWorkspaceDraftChanged();
    void RequestEffectsCatalog();

    EngineBridge*        bridge_ = nullptr;
    WorkspaceController* ws_     = nullptr;

    EffectLayerModel        layer_model_;
    /* UI-side look registry: documents this session installed
       (saveLookAs) or pulled from the engine catalog's verbose
       effect entries — resolution for ids the packaged registry
       doesn't know. */
    studio::EffectRegistry  ui_looks_;

    QVariantList looks_;
    QString      status_text_;
    int          selected_layer_ = -1;

    /* saveLookAs in flight — the request correlator, then the
       job id once the engine accepts; the requested name rides
       along so the adopted document keeps the display name. */
    bool         saving_look_      = false;
    int          pending_save_req_ = 0;
    quint64      pending_save_job_ = 0;
    QString      pending_save_id_;
    QString      pending_save_name_;
    /* catalog kind=effects request correlator. */
    int          pending_catalog_  = 0;
    /* Test-visible count of RequestEffectsCatalog calls. */
    int          catalog_requests_ = 0;
    /* Byte-budgeted catalog pages arrive in more/next_offset
       steps — entries accumulate here until the walk ends. */
    QVariantList pending_catalog_entries_;
    quint64      pending_catalog_off_ = 0;
};

} /* namespace ui */
} /* namespace openrgb2 */
