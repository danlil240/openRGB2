/*---------------------------------------------------------*\
|| ObjectEditorController.h                                 ||
||                                                          ||
||   The studio scene/object editing surface (studio-    ||
||   import Task 5): ONE QObject adapter that wraps the   ||
||   Qt-free studio::EditorController bound to the shared ||
||   WorkspaceEditSession draft — the same Editor() the   ||
||   layout/lighting adapters use. Every committed op     ||
||   produces exactly ONE undo record through             ||
||   WorkspaceEditSession::CommitEdit / CommitDocEdit;    ||
||   this controller owns NO document and NO undo stack.  ||
||                                                          ||
||   Adapted from the source-of-record plugin/SceneBridge ||
||   editor half (provenance ledger): same slot surface — ||
||   selection, transform gestures (Begin/Preview/Commit/||
||   Cancel — one record per gesture), discrete ops       ||
||   (position/rotation in PARENT-local meters+degrees,   ||
||   visibility, lock, rename), hierarchy ops (group/     ||
||   ungroup/delete cascade/mirrored duplicate/align/     ||
||   distribute), device-library add + favorites, and     ||
||   object/emitter painting into the compact color maps. ||
||   Save/load, effects, layers and the output adapter    ||
||   stay with their existing owners — this is the        ||
||   scene-OBJECT seam only.                              ||
||                                                          ||
||   Offline rule: without a WorkspaceController or a     ||
||   draft, every mutation refuses honestly and the read  ||
||   side reports empty — nothing is fabricated. The      ||
||   engine-catalog library rows merge in when a          ||
||   connected engine answers WORKSPACE_COMMAND catalog.  ||
||                                                          ||
||   SPDX-License-Identifier: GPL-2.0-or-later               ||
|\*---------------------------------------------------------*/

#pragma once

#include "SceneObjectModel.h"
#include "PresetListModel.h"

#include "../EngineBridge.h"
#include "../WorkspaceController.h"

#include "config/StudioConfig.h"
#include "presets/PresetRegistry.h"
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

class ObjectEditorController : public QObject
{
    Q_OBJECT

    /* The scene-object row model (tree + canvas). */
    Q_PROPERTY(QObject* objectModel READ objectModel CONSTANT)
    /* The device-type library model. */
    Q_PROPERTY(QObject* presetModel READ presetModel
                   NOTIFY presetLibraryChanged)
    /* Ordered selected root-instance ids; back() = primary. */
    Q_PROPERTY(QVariantList selectedInstances READ selectedInstances
                   NOTIFY selectionChanged)
    Q_PROPERTY(QString selectedId READ selectedId
                   NOTIFY selectionChanged)
    /* A draft exists and this controller can mutate it. */
    Q_PROPERTY(bool ready READ ready NOTIFY readyChanged)
    Q_PROPERTY(bool canUndo READ canUndo NOTIFY undoChanged)
    Q_PROPERTY(bool canRedo READ canRedo NOTIFY undoChanged)
    Q_PROPERTY(QString statusText READ statusText NOTIFY statusChanged)
    /* Tool state for the panels (the 2D canvas uses move +
       paint); editPlane: 0 Free, 1 DeskXZ, 2 FrontXY, 3 SideYZ —
       the core's EditPlane order. */
    Q_PROPERTY(int tool READ tool WRITE setTool NOTIFY toolChanged)
    Q_PROPERTY(int editPlane READ editPlane WRITE setEditPlane
                   NOTIFY editPlaneChanged)
    Q_PROPERTY(bool snapEnabled READ snapEnabled WRITE setSnapEnabled
                   NOTIFY snapChanged)
    Q_PROPERTY(QColor paintColor READ paintColor WRITE setPaintColor
                   NOTIFY paintColorChanged)
    /* True while the resolved rows mirror the engine's APPLIED
       document because the draft doesn't resolve (honest fallback
       — the draft's compact rows still drive the structure). */
    Q_PROPERTY(bool resolvedFromApplied READ resolvedFromApplied
                   NOTIFY sceneChanged)

public:
    explicit ObjectEditorController(EngineBridge* bridge,
                                    WorkspaceController* ws,
                                    QObject* parent = nullptr);

    QObject* objectModel() { return &object_model_; }
    QObject* presetModel() { return &preset_model_; }

    QVariantList selectedInstances() const;
    QString      selectedId() const;
    bool         ready() const;
    bool         canUndo() const;
    bool         canRedo() const;
    QString      statusText() const { return status_text_; }
    int          tool() const       { return tool_; }
    int          editPlane() const  { return edit_plane_; }
    bool         snapEnabled() const{ return snap_; }
    QColor       paintColor() const { return paint_color_; }
    bool         resolvedFromApplied() const
        { return resolved_src_ == ResolvedSrc::Applied; }
    /* QML-callable — the 3D viewport's gesture router gates
       undo/redo and frame commands on a live transform gesture
       (Task 8). */
    Q_INVOKABLE bool gestureActive() const;

    /*---- selection -------------------------------------------*/
    /* Resolved object id or instance id → select its owning
       root instance (the viewport-click rule). */
    Q_INVOKABLE void select(const QString& objectOrInstanceId);
    Q_INVOKABLE void selectInstance(const QString& instanceId,
                                    bool additive = false);
    Q_INVOKABLE void clearEditorSelection();

    /*---- transform gesture — ONE undo record -----------------*/
    Q_INVOKABLE void beginTransformGesture();
    Q_INVOKABLE void updateTransformGesture(double dx, double dy,
                                            double dz, int plane,
                                            bool snap);
    Q_INVOKABLE void updateRotateGesture(double degrees, bool snap);
    Q_INVOKABLE void updateRotateGestureAxis(double ax, double ay,
                                             double az, double degrees,
                                             bool snap);
    Q_INVOKABLE void commitTransformGesture();
    Q_INVOKABLE void cancelTransformGesture();

    /*---- discrete ops -----------------------------------------*/
    Q_INVOKABLE bool setInstancePosition(const QString& id,
                                         double x, double y, double z);
    Q_INVOKABLE bool setInstanceRotation(const QString& id,
                                         double rx, double ry,
                                         double rz);
    /* World-space numeric edit — converted into the parent frame. */
    Q_INVOKABLE bool setInstancePositionWorld(const QString& id,
                                              double x, double y,
                                              double z);
    Q_INVOKABLE bool renameInstance(const QString& id,
                                    const QString& newId);
    Q_INVOKABLE void setInstanceVisible(const QString& id, bool on);
    Q_INVOKABLE void setInstanceLocked(const QString& id, bool on);

    /*---- hierarchy ops ----------------------------------------*/
    Q_INVOKABLE void groupSelected();
    Q_INVOKABLE void ungroupSelected();
    Q_INVOKABLE void deleteSelected();
    /* The kill set Delete would take — the tree previews the
       cascade BEFORE committing. Read-only. */
    Q_INVOKABLE QStringList deletePreview() const;
    Q_INVOKABLE void duplicateMirrored();
    /* axis 0/1/2 = X/Y/Z; mode 0 min, 1 center, 2 max. */
    Q_INVOKABLE void alignSelected(int axis, int mode);
    Q_INVOKABLE void distributeSelected(int axis);

    /*---- device library ---------------------------------------*/
    /* Drop a NEW unbound instance of `typeId` at (x,y,z) meters —
       the y is lifted by the type's footprint floor so it rests on
       the surface, exactly like the source bridge. */
    Q_INVOKABLE void addDeviceInstance(const QString& typeId,
                                       double x, double y, double z);
    /* UI pref — the draft's meta.ui.favorites; never an undo
       record (same contract as the source bridge). */
    Q_INVOKABLE void setTypeFavorite(const QString& typeId, bool fav);
    /* Re-fetch the engine's device catalog (WORKSPACE_COMMAND
       catalog kind=devices) and re-snapshot the local registry. */
    Q_INVOKABLE void refreshLibrary();

    /*---- row/state reads ---------------------------------------*/
    /* {id,type,x,y,z,rx,ry,rz,visible,locked} for an instance id —
       the TransformInspector's numeric fields bind to this. */
    Q_INVOKABLE QVariantMap instanceState(const QString& id) const;
    /* Emitters of a resolved object (linked copies read their
       owner's) — {x,y,z,c,i} / ["#RRGGBB", ...]. */
    Q_INVOKABLE QVariantList emittersOf(const QString& objectId) const;
    Q_INVOKABLE QVariantList emitterColorsOf(const QString& objectId) const;

    /*---- painting ----------------------------------------------*/
    /* Paint the selected (or named) object's base color — one undo
       record through the shared session. */
    Q_INVOKABLE void setSelectedColor(const QColor& color);
    Q_INVOKABLE void paintObject(const QString& objectId,
                                 const QColor& color);
    Q_INVOKABLE void paintEmitter(const QString& objectId, int index,
                                  const QColor& color);

    /* 2D-canvas snap seam: quantize a desk-plane meters value to
       the 10 mm grid when snapEnabled, else pass it through. The
       Layout canvas drag commits through layoutCtl.moveObject
       (which has no snap param), so the page rounds the released
       position here — the same 10 mm step the gestures use. */
    Q_INVOKABLE double snapMeters(double meters) const;

    /* Transient status line — the viewport's input router uses
       this for swallowed-shortcut explanations ("finish the
       drag first") instead of a silent ignore (Task 8). */
    Q_INVOKABLE void setStatusMessage(const QString& text);

    /*---- undo/redo — the shared session's history --------------*/
    Q_INVOKABLE void undo();
    Q_INVOKABLE void redo();

    /*---- panel state -------------------------------------------*/
    void setTool(int t);
    void setEditPlane(int p);
    void setSnapEnabled(bool on);
    void setPaintColor(const QColor& c);

    /*---- test/diagnostic hooks ----------------------------------*/
    /* Extra preset directory probe (tests set this to the packaged
       devices dir; production probes applicationDirPath()/cwd
       candidates + DLS_PRESET_DIR). Returns loaded-file errors. */
    std::vector<std::string> LoadPresetDirectory(
        const std::string& dir);
    const studio::PresetRegistry& Presets() const { return types_; }
    /* Task 9 device-type authoring: register/remove a type the
       engine library just gained/lost so the draft resolves it
       before the next catalog pull (mirrors the effect editor's
       session-look overlay). Both refresh the preset model and
       re-resolve — a type change is never transforms-only. */
    void RegisterPresetForSession(const studio::DevicePreset& p);
    void UnregisterPresetForSession(const std::string& id);
    SceneObjectModel&     ModelForTesting() { return object_model_; }
    /* Test seam: the in-flight device-catalog request id. */
    int  PendingCatalogForTesting() const { return pending_catalog_; }
    void SetPendingCatalogForTesting(int id) { pending_catalog_ = id; }

signals:
    void selectionChanged();
    void sceneChanged();
    void undoChanged();
    void statusChanged();
    void statusMessage(const QString& text);
    void presetLibraryChanged();
    void toolChanged();
    void editPlaneChanged();
    void snapChanged();
    void paintColorChanged();
    void readyChanged();
    void gestureActiveChanged();
    /* Emitter colors under `objectId` changed (paint). */
    void emittersChanged(const QString& objectId);

private:
    enum class ResolvedSrc { None, Draft, Applied };

    WorkspaceEditSession* Session() const;
    studio::EditorController*     Editor() const;
    /* The draft document — nullptr when no session/draft. */
    const studio::StudioDocument* WsDoc() const;

    /* Commit one edit through the shared session; rebuild the
       resolved snapshot; refresh the models the right granularity
       (TransformsOnly -> per-row dataChanged). */
    void CommitEdit(studio::EditorEdit e);
    /* Gesture preview: re-resolve and update only the touched
       instance rows — never a history record, never dirty. */
    void PreviewAdopt(const std::set<std::string>& ids);
    /* Re-resolve the draft (or adopt the applied scene on failure)
       and reset the object model rows. */
    void RebuildResolved();
    void RefreshAll();       /* rebuild + full model reset + emits  */
    /* RefreshAll()'s SceneObjectModel reset destroys tree/canvas
       delegates — a synchronous call inside the input dispatch
       that caused the change (shortcut, button click, field
       commit) tears down the item still being delivered to
       (F02). These post the destructive half through the event
       loop; the pending flags dedup burst commits. */
    void ScheduleModelRefresh();
    void ScheduleModelReset();

    QString BoundStatusFor(const std::string& binding_id) const;
    const studio::SceneObject* ResolvedObject(
        const std::string& id) const;
    /* Effective paint target: linked objects write their OUTPUT
       OWNER's color key (the core's EmitterColor reads through). */
    std::string PaintOwnerOf(const std::string& object_id) const;
    /* Every color key a paint op on `id` should write: the id's
       own owner key PLUS every emitter-bearing resolved object
       under it (an instance/group id paints the whole device —
       the 2D canvas clicks land on instances). Unresolvable ids
       fall back to the raw key so the paint still records. */
    std::set<std::string> PaintTargets(const std::string& id) const;
    /* True when `iid` or any ancestor in its draft parent chain
       is locked — the discrete transform ops refuse so a locked
       group freezes its subtree (core IsLocked is self-only). */
    bool ChainLocked(const std::string& iid) const;

    void SetStatus(const QString& s);
    void RequestCatalog();
    void OnBridgeReply(int requestId, const QString& typeName,
                       const QVariantMap& body);
    void OnWorkspaceDraftEdited();
    void OnWorkspaceDraftChanged();
    void OnAvailabilityChanged();

    EngineBridge*        bridge_ = nullptr;
    WorkspaceController* ws_     = nullptr;

    studio::PresetRegistry    types_;
    studio::SceneDocument     resolved_;
    ResolvedSrc               resolved_src_ = ResolvedSrc::None;
    SceneObjectModel          object_model_;
    PresetListModel           preset_model_;

    QString     status_text_;
    int         tool_       = 0;    /* 0 move, 1 rotate, 2 paint   */
    int         edit_plane_ = 1;    /* EditPlane::DeskXZ           */
    bool        snap_       = false;
    QColor      paint_color_ = QColor(255, 255, 255);
    int         pending_catalog_ = 0;
    bool        refresh_pending_  = false;
    bool        reset_pending_    = false;
};

} /* namespace ui */
} /* namespace openrgb2 */
