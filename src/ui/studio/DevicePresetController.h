/*---------------------------------------------------------*\
||| DevicePresetController.h                                 ||
|||                                                          ||
|||   The studio device-TYPE authoring surface (studio-   ||
|||   import Task 9): ONE QObject adapter over the shared ||
|||   WorkspaceEditSession draft + the engine's bounded   ||
|||   resource jobs — the OpenRGB2 port of the            ||
|||   source-of-record plugin/SceneBridge preset half     ||
|||   (provenance ledger):                                 ||
|||                                                          ||
|||   - presetDocument / fetchPresetDocument — the        ||
|||     editor's doc read: local registry view first,     ||
|||     the engine's resource_get for library docs the    ||
|||     UI never saw                                       ||
|||   - validatePreset / previewPreset — candidate        ||
|||     validation + a throwaway __preview__ resolve       ||
|||     against a PRIVATE registry copy: doc, workspace,   ||
|||     bindings, hardware and live output are never       ||
|||     touched                                            ||
|||   - savePresetType / deletePresetType — engine        ||
|||     resource_install{kind:"device_type",replace} and  ||
|||     resource_remove jobs, revision-gated, collision-  ||
|||     safe, never overwriting                            ||
|||   - saveInstanceAsVariant / createTypeFromSelection — ||
|||     the source bridge's two library flows: validate   ||
|||     -> install -> (variant only) ONE undoable Retype   ||
|||     through the shared session                         ||
|||                                                          ||
|||   This controller owns NO document and NO undo stack. ||
|||   The in-session registration of a landed type rides   ||
|||   ObjectEditorController::RegisterPresetForSession so  ||
|||   the draft resolves it before the next catalog pull.  ||
|||                                                          ||
|||   Offline rule: without a bridge/session every        ||
|||   mutation refuses honestly and the read side reports  ||
|||   empty — nothing is fabricated.                       ||
|||                                                          ||
|||   SPDX-License-Identifier: GPL-2.0-or-later               ||
\*---------------------------------------------------------*/

#pragma once

#include "../EngineBridge.h"
#include "../WorkspaceController.h"
#include "ObjectEditorController.h"

#include "presets/DevicePreset.h"
#include "presets/PresetRegistry.h"

#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>

#include <string>
#include <vector>

namespace openrgb2
{
namespace ui
{

class DevicePresetController : public QObject
{
    Q_OBJECT

    /* A draft exists and this controller can mutate it. */
    Q_PROPERTY(bool ready READ ready NOTIFY readyChanged)
    /* A resource_install/resource_remove job is in flight. */
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(QString statusText READ statusText NOTIFY statusChanged)

public:
    DevicePresetController(EngineBridge* bridge,
                           WorkspaceController* ws,
                           ObjectEditorController* objects,
                           QObject* parent = nullptr);

    bool    ready() const;
    bool    busy() const
        { return pending_install_req_ != 0 || pending_install_job_ != 0 ||
                 pending_delete_req_  != 0 || pending_delete_job_  != 0; }
    QString statusText() const { return status_text_; }

    /*---- reads -------------------------------------------------*/
    /* Local registry view of a type: {exists, fromFile,
       packaged, instances, doc?} — `doc` rides only when the
       UI-side registry holds the definition; a library-only
       type reports exists:false locally and fetchPresetDocument
       pulls the engine's copy (presetDocumentReady). */
    Q_INVOKABLE QVariantMap presetDocument(const QString& typeId) const;
    /* Engine-side read: WORKSPACE_COMMAND resource_get — the
       reply lands in presetDocumentReady (full document plus
       from_file/packaged_default flags). */
    Q_INVOKABLE void fetchPresetDocument(const QString& typeId);
    /* Draft instances currently using `typeId`. */
    Q_INVOKABLE int instanceCount(const QString& typeId) const;

    /*---- candidate authoring ------------------------------------*/
    /* Schema + dependency validation against a PRIVATE registry
       copy — the live library is never touched. */
    Q_INVOKABLE QVariantMap validatePreset(const QVariantMap& candidate) const;
    /* validate + a throwaway __preview__ resolve at the origin;
       returns {ok, errors, objects[]}. No workspace, binding,
       or hardware contact. */
    Q_INVOKABLE QVariantMap previewPreset(const QVariantMap& candidate) const;

    /*---- mutations (engine jobs; outcomes land in the signals) -*/
    /* Install/replace the candidate in the managed library.
       `overwrite` asks for the replace path (rewriting the
       file-layer entry); without it differing content lands
       suffixed — never an overwrite. Immediate refusal returns
       {ok:false, errors[]}; else {ok:true, pending:true} and the
       terminal result arrives in presetSaveResult. */
    Q_INVOKABLE QVariantMap savePresetType(const QVariantMap& candidate,
                                           bool overwrite);
    /* File-layer delete via resource_remove — packaged defaults
       and in-use/child-referenced ids refuse honestly (locally
       first, then the engine's authoritative check). */
    Q_INVOKABLE QVariantMap deletePresetType(const QString& typeId);

    /* Save instance's resolved type (+ baked paint) under a new
       id, then repoint the instance through ONE undoable Retype.
       Refusals arrive via statusMessage; the install + repoint
       land via presetSaveResult. */
    Q_INVOKABLE void saveInstanceAsVariant(const QString& instanceId,
                                           const QString& newTypeId,
                                           const QString& displayName);
    /* Child-reference assembly type over the CURRENT selection —
       no workspace edit; the instances stay put. */
    Q_INVOKABLE void createTypeFromSelection(
        const QStringList& instanceIds,
        const QString& newTypeId,
        const QString& displayName);

    /*---- test hooks --------------------------------------------*/
    /* Feed a resource_get reply body (offline tests can't
       correlate a real request). */
    void FeedResourceGetForTesting(const QVariantMap& body);
    /* Feed a terminal job event body for the pending job. */
    void FeedJobEventForTesting(const QVariantMap& body);
    studio::PresetRegistry& SessionTypes() { return session_types_; }

signals:
    void readyChanged();
    void busyChanged();
    void statusChanged();
    void statusMessage(const QString& text);
    /* fetchPresetDocument reply: the presetDocument() shape plus
       `document` (the serialized type) or `ok:false`. */
    void presetDocumentReady(const QVariantMap& doc);
    /* Terminal save/install outcome: {ok, id, landed_id,
       errors[], warnings[]}. */
    void presetSaveResult(const QVariantMap& result);
    /* Terminal delete outcome: {ok, id, error}. */
    void presetDeleteResult(const QVariantMap& result);
    /* The library view changed (install/remove landed). */
    void presetLibraryChanged();

private:
    const studio::StudioDocument* WsDoc() const;
    /* The registry the candidate validates against: the object
       controller's library + types this session installed. */
    studio::PresetRegistry EffectiveRegistry() const;
    /* Post-install draft warnings — affected instances, changed
       zone counts, dangling bindings (the source's contract). */
    QVariantList InstanceWarnings(const studio::DevicePreset& p) const;
    /* One resource_install{kind:"device_type"} job for `p`;
       `post` selects the terminal handler. Returns "" on send,
       else the refusal text. */
    QString SendInstall(const studio::DevicePreset& p,
                        bool replace, const QString& post_op);
    void OnReplyReceived(int requestId, const QString& typeName,
                         const QVariantMap& body);
    void OnEventReceived(const QString& kind,
                         const QVariantMap& data);
    void ResolveInstall(const QVariantMap& ev, bool ok,
                        const QString& err);
    void ResolveDelete(const QVariantMap& ev, bool ok,
                       const QString& err);
    void SetStatus(const QString& s);

    EngineBridge*            bridge_  = nullptr;
    WorkspaceController*     ws_      = nullptr;
    ObjectEditorController*  objects_ = nullptr;

    /* Types this session's installs landed — resolves before
       the next catalog refresh (the ui_looks_ pattern). */
    studio::PresetRegistry  session_types_;
    QString                 status_text_;

    /* resource_install bookkeeping — one in flight. `post_op`
       selects the terminal action: "" plain save, "retype:<iid>"
       (variant follow-up). The staged preset rides along so the
       terminal handler never re-derives it. */
    int                 pending_install_req_ = 0;
    quint64             pending_install_job_ = 0;
    QString             pending_install_post_;
    QString             pending_install_iid_;
    studio::DevicePreset pending_preset_;

    /* resource_remove bookkeeping — one in flight. */
    int                 pending_delete_req_ = 0;
    quint64             pending_delete_job_ = 0;
    QString             pending_delete_id_;

    /* resource_get correlator. */
    int                 pending_get_req_ = 0;
    QString             pending_get_id_;
};

} /* namespace ui */
} /* namespace openrgb2 */
