/*---------------------------------------------------------*\
|||| SceneEditController.h                                     ||
||||                                                           ||
||||   QObject half of the Lighting edit flow (Task 4.2,   ||
||||   plan section 5): wires the Qt-free SceneEditModel   ||
||||   to EngineBridge so the page's choose -> select ->   ||
||||   adjust -> save round-trips through SET_SCENE with   ||
||||   honest state at every step —                        ||
||||                                                           ||
||||   - edits accumulate in a working copy; dirty is the  ||
||||     truth "unsaved changes" binds to                   ||
||||   - save() issues SET_SCENE and tracks the reply:     ||
||||     OK commits, ERROR surfaces saveError and KEEPS    ||
||||     the edit, a dropped session does the same         ||
||||   - an applied-scene change under pending edits sets  ||
||||     conflict — the user picks Save (overwrite) or     ||
||||     Discard (reload), nothing merges silently         ||
||||   - playback params emit ONLY the fields the selected ||
||||     uploaded preset supports (protocol §5 table) and  ||
||||     preserve the current control instead of silently  ||
||||     resuming a paused upload                           ||
||||                                                           ||
||||   Device groups are derived from the scene document's  ||
||||   objects + bindings joined with engine snapshots     ||
||||   (display hint — the engine resolves authoritatively)||
||||                                                           ||
||||   SPDX-License-Identifier: GPL-2.0-or-later               ||
||\*---------------------------------------------------------*/

#pragma once

#include "ProfileLibrary.h"
#include "SceneEditModel.h"

#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>

namespace openrgb2
{
namespace ui
{

class EngineBridge;

class SceneEditController : public QObject
{
    Q_OBJECT

    /*---- edit state ---------------------------------------*/
    Q_PROPERTY(bool hasScene READ hasScene NOTIFY sceneChanged)
    Q_PROPERTY(bool hasWorking READ hasWorking
                   NOTIFY workingChanged)
    Q_PROPERTY(bool dirty READ dirty NOTIFY dirtyChanged)
    Q_PROPERTY(bool conflict READ conflict
                   NOTIFY conflictChanged)
    Q_PROPERTY(bool presetReplacedLayers READ presetReplacedLayers
                   NOTIFY workingChanged)
    Q_PROPERTY(bool saving READ saving NOTIFY savingChanged)
    Q_PROPERTY(QString saveError READ saveError
                   NOTIFY saveErrorChanged)

    /*---- working-document scalars (bind directly) ---------*/
    Q_PROPERTY(QString sceneName READ sceneName
                   NOTIFY workingChanged)
    Q_PROPERTY(QString presetId READ presetId
                   NOTIFY workingChanged)
    Q_PROPERTY(double brightness READ brightness
                   NOTIFY workingChanged)
    Q_PROPERTY(double effectSpeed READ effectSpeed
                   NOTIFY workingChanged)
    Q_PROPERTY(double effectIntensity READ effectIntensity
                   NOTIFY workingChanged)
    Q_PROPERTY(QVariantMap workingDocument READ workingDocument
                   NOTIFY workingChanged)

    /*---- groups + selection --------------------------------*/
    Q_PROPERTY(QVariantList deviceGroups READ deviceGroups
                   NOTIFY groupsChanged)
    Q_PROPERTY(QStringList selectedGroups READ selectedGroups
                   NOTIFY selectedGroupsChanged)
    Q_PROPERTY(bool groupsTargetable READ groupsTargetable
                   NOTIFY workingChanged)

    /*---- presets -------------------------------------------*/
    /* The three uploaded presets from the wire contract:
       [{id,name,speed,direction,color}]. */
    Q_PROPERTY(QVariantList playbackPresets READ playbackPresets
                   CONSTANT)
    /* Support map for the CURRENT preset:
       {playback:bool, speed,direction,color, needs:QString} */
    Q_PROPERTY(QVariantMap presetSupport READ presetSupport
                   NOTIFY presetSupportChanged)

    /*---- profiles -------------------------------------------*/
    Q_PROPERTY(QVariantList profiles READ profiles
                   NOTIFY profilesChanged)
    Q_PROPERTY(QString libraryError READ libraryError
                   NOTIFY libraryErrorChanged)

    /*---- tracked playback params (what the UI last sent,
           resent on every request so an absent params field
           can't reset the engine's record to defaults) ----*/
    Q_PROPERTY(int paramSpeed READ paramSpeed
                   NOTIFY paramsChanged)
    Q_PROPERTY(bool paramReverse READ paramReverse
                   NOTIFY paramsChanged)
    Q_PROPERTY(QString paramColor READ paramColor
                   NOTIFY paramsChanged)

public:
    explicit SceneEditController(EngineBridge* bridge,
                                 QObject* parent = nullptr);

    /*---- scene edits (pending until save) ------------------*/
    Q_INVOKABLE bool setPreset(const QString& id);
    Q_INVOKABLE bool setSceneName(const QString& name);
    Q_INVOKABLE void setBrightness(double v);
    Q_INVOKABLE void setEffectSpeed(double v);
    Q_INVOKABLE void setEffectIntensity(double v);
    /* Applies `hex` ("#RRGGBB") to the selected groups —
       or every device object when nothing is selected. */
    Q_INVOKABLE bool setGroupColor(const QString& hex);
    Q_INVOKABLE void toggleGroup(const QString& objectId);
    Q_INVOKABLE void selectAllGroups();   /* empty selection  */

    /*---- save flow -----------------------------------------*/
    Q_INVOKABLE void save();
    Q_INVOKABLE void discardEdits();
    Q_INVOKABLE void reloadFromEngine();   /* GET_SCENE pull  */

    /*---- playback (immediate — not part of the scene doc) --*/
    /* Mode request preserving the current control + params
       (the wire defaults an omitted control to "playing" —
       passing nothing would silently resume a paused loop). */
    Q_INVOKABLE int  requestMode(const QString& mode);
    Q_INVOKABLE int  setPlaybackControl(const QString& control);
    /* Emits only params the selected uploaded preset
       supports; the current control is preserved. */
    Q_INVOKABLE int  sendPlaybackParams(int speed, bool reverse,
                                        const QString& colorHex);

    /*---- profiles -------------------------------------------*/
    Q_INVOKABLE bool saveProfile(const QString& name);
    /* Stages the stored document as pending edits — Save
       applies it through SET_SCENE (engine validates +
       migrates; the UI never parses scene fields). */
    Q_INVOKABLE bool loadProfile(const QString& name);
    Q_INVOKABLE bool deleteProfile(const QString& name);

    /*---- state ----------------------------------------------*/
    bool        hasScene() const;
    bool        hasWorking() const;
    bool        dirty() const;
    bool        conflict() const;
    bool        presetReplacedLayers() const;
    bool        saving() const;
    QString     saveError() const;
    QString     sceneName() const;
    QString     presetId() const;
    double      brightness() const;
    double      effectSpeed() const;
    double      effectIntensity() const;
    QVariantMap workingDocument() const;
    QVariantList deviceGroups();
    QStringList selectedGroups() const;
    bool        groupsTargetable() const;
    QVariantList playbackPresets() const;
    QVariantMap presetSupport();
    QVariantList profiles();
    QString     libraryError();
    int         paramSpeed() const;
    bool        paramReverse() const;
    QString     paramColor() const;

    /*---- test hooks ------------------------------------------*/
    SceneEditModel&    ModelForTesting()   { return model_; }
    ProfileLibrary&    LibraryForTesting() { return library_; }

signals:
    void sceneChanged();
    void workingChanged();
    void dirtyChanged();
    void conflictChanged();
    void savingChanged();
    void saveErrorChanged();
    void groupsChanged();
    void selectedGroupsChanged();
    void presetSupportChanged();
    void profilesChanged();
    void libraryErrorChanged();
    void paramsChanged();

private:
    void OnSceneChanged();
    void OnReplyReceived(int id, const QString& type,
                         const QVariantMap& body);
    void OnConnectionChanged();
    void OnSnapshotsChanged();

    QString     CurrentControl() const;
    QString     RequestedMode() const;
    /* Tracked params trimmed to what the selected uploaded
       preset supports — empty when the scene names no
       playback preset (omitted from the request). */
    QVariantMap FilteredParams() const;
    void        TouchWorking();
    void        SetSaveError(const QString& e);
    QVariantMap ResolveGroupHints(const DeviceGroup& g) const;

    EngineBridge*   bridge_;
    SceneEditModel  model_;
    ProfileLibrary  library_;

    int             pending_save_id_ = 0;
    bool            saving_          = false;
    QString         save_error_;

    /* Tracked last-sent uploaded-preset params — resent on
       every playback request so absent fields can't reset
       the engine's record to defaults (protocol gap noted). */
    int             param_speed_   = 100;
    bool            param_reverse_ = false;
    QString         param_color_   = QStringLiteral("#FF0000");
};

} /* namespace ui */
} /* namespace openrgb2 */
