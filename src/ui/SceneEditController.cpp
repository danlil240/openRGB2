/*---------------------------------------------------------*\
|||| SceneEditController.cpp                                   ||
||||                                                           ||
||||   SPDX-License-Identifier: GPL-2.0-or-later               ||
||\*---------------------------------------------------------*/

#include "SceneEditController.h"

#include "EngineBridge.h"
#include "JsonVariant.h"

#include <algorithm>

namespace openrgb2
{
namespace ui
{

namespace
{

QString JStr(const nlohmann::json& j, const char* key)
{
    const auto it = j.find(key);
    return (it != j.end() && it->is_string())
               ? QString::fromStdString(it->get<std::string>())
               : QString();
}

/* Mirrors studio::NameMatches (src/core/scene/BindingResolver)
   — the DISPLAY hint for "which controller does this binding
   point at". The engine resolves authoritatively; this is
   only for painting connected/ambiguous/absent on chips. */
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

QVariantMap JObj(const nlohmann::json& j)
{
    return JsonToVariant(j).toMap();
}

} /* anonymous namespace */

SceneEditController::SceneEditController(EngineBridge* bridge,
                                         QObject* parent)
    : QObject(parent), bridge_(bridge)
{
    connect(bridge_, &EngineBridge::sceneChanged,
            this, &SceneEditController::OnSceneChanged);
    connect(bridge_, &EngineBridge::replyReceived,
            this, &SceneEditController::OnReplyReceived);
    connect(bridge_, &EngineBridge::connectionChanged,
            this, &SceneEditController::OnConnectionChanged);
    connect(bridge_, &EngineBridge::engineLost,
            this, [this](const QString& reason)
            {
                if(saving_)
                {
                    /* The reply can never arrive — surface the
                       interrupted save instead of spinning. */
                    saving_ = false;
                    emit savingChanged();
                    SetSaveError(
                        QStringLiteral("engine connection lost "
                                       "before the save reply "
                                       "(%1) — edits preserved")
                            .arg(reason));
                }
            });
    connect(bridge_, &EngineBridge::snapshotsChanged,
            this, &SceneEditController::OnSnapshotsChanged);
    connect(bridge_, &EngineBridge::presetsChanged,
            this, [this] { emit presetSupportChanged(); });
    connect(&library_, &ProfileLibrary::entriesChanged,
            this, &SceneEditController::profilesChanged);
    connect(&library_, &ProfileLibrary::lastErrorChanged,
            this, &SceneEditController::libraryErrorChanged);

    /* Paint whatever the bridge already holds (controller may
       be created after WELCOME landed). */
    OnSceneChanged();
}

/*---------------------------------------------------------*\
||| Engine -> model                                          ||
\*---------------------------------------------------------*/

void SceneEditController::OnSceneChanged()
{
    const QVariant doc_var =
        bridge_->scene().value(QStringLiteral("document"));
    const nlohmann::json doc =
        doc_var.isValid() ? VariantToJson(doc_var) : nlohmann::json(nullptr);
    const bool was_dirty = model_.Dirty();
    const bool had_conf  = model_.Conflict();
    model_.LoadApplied(doc);
    emit sceneChanged();
    emit workingChanged();
    emit groupsChanged();
    emit presetSupportChanged();
    if(model_.Dirty() != was_dirty)
    {
        emit dirtyChanged();
    }
    if(model_.Conflict() != had_conf)
    {
        emit conflictChanged();
    }
}

void SceneEditController::OnReplyReceived(int id, const QString& type,
                                          const QVariantMap& body)
{
    if(id == 0 || id != pending_save_id_)
    {
        return;
    }
    pending_save_id_ = 0;
    if(saving_)
    {
        saving_ = false;
        emit savingChanged();
    }
    if(type == QLatin1String("OK"))
    {
        model_.Commit();
        SetSaveError(QString());
        emit workingChanged();
        emit dirtyChanged();
    }
    else if(type == QLatin1String("ERROR"))
    {
        /* Save failure surfaces honestly and the edit is
           preserved — the focused-save regression from the
           calibration report is exactly this path. */
        QString msg = body.value(QStringLiteral("message"))
                          .toString();
        const QVariantList errs =
            body.value(QStringLiteral("errors")).toList();
        if(!errs.isEmpty())
        {
            msg += QStringLiteral(" (") +
                   errs.first().toString() +
                   QStringLiteral(")");
        }
        SetSaveError(msg.isEmpty()
                         ? QStringLiteral("engine rejected the "
                                          "scene — edits preserved")
                         : msg);
    }
    /* Any other reply type for the correlator is a protocol
       oddity — treat it like a failure, keep the edit. */
    else
    {
        SetSaveError(QStringLiteral("unexpected reply type %1 "
                                    "to SET_SCENE — edits "
                                    "preserved").arg(type));
    }
}

void SceneEditController::OnConnectionChanged()
{
    emit groupsChanged();
}

void SceneEditController::OnSnapshotsChanged()
{
    emit groupsChanged();
}

/*---------------------------------------------------------*\
||| Scene edits                                              ||
\*---------------------------------------------------------*/

void SceneEditController::TouchWorking()
{
    emit workingChanged();
    emit groupsChanged();
    emit presetSupportChanged();
    emit dirtyChanged();
    emit conflictChanged();
}

bool SceneEditController::setPreset(const QString& id)
{
    std::string err;
    if(!model_.SetPreset(id.toStdString(), err))
    {
        SetSaveError(QString::fromStdString(err));
        return false;
    }
    SetSaveError(QString());
    TouchWorking();
    return true;
}

bool SceneEditController::setSceneName(const QString& name)
{
    std::string err;
    if(!model_.SetName(name.toStdString(), err))
    {
        SetSaveError(QString::fromStdString(err));
        return false;
    }
    TouchWorking();
    return true;
}

void SceneEditController::setBrightness(double v)
{
    if(model_.SetBrightness(v))
    {
        TouchWorking();
    }
}

void SceneEditController::setEffectSpeed(double v)
{
    if(model_.SetEffectSpeed(v))
    {
        TouchWorking();
    }
}

void SceneEditController::setEffectIntensity(double v)
{
    if(model_.SetEffectIntensity(v))
    {
        TouchWorking();
    }
}

bool SceneEditController::setGroupColor(const QString& hex)
{
    std::vector<std::string> ids = model_.SelectedGroups();
    if(ids.empty())
    {
        /* "All devices" — every device object in the doc. */
        for(const DeviceGroup& g : model_.DeviceGroups())
        {
            ids.push_back(g.id);
        }
    }
    if(ids.empty())
    {
        SetSaveError(QStringLiteral("no device objects in the "
                                    "scene"));
        return false;
    }
    bool ok = true;
    std::string err;
    for(const std::string& id : ids)
    {
        if(!model_.SetObjectColor(id, hex.toStdString(), err))
        {
            ok = false;
        }
    }
    if(!ok)
    {
        SetSaveError(QString::fromStdString(err));
        return false;
    }
    SetSaveError(QString());
    TouchWorking();
    return true;
}

void SceneEditController::toggleGroup(const QString& objectId)
{
    std::vector<std::string> sel = model_.SelectedGroups();
    const std::string        id  = objectId.toStdString();
    const auto it = std::find(sel.begin(), sel.end(), id);
    if(it == sel.end())
    {
        sel.push_back(id);
    }
    else
    {
        sel.erase(it);
    }
    model_.SetSelectedGroups(sel);
    emit selectedGroupsChanged();
    emit workingChanged();          /* targets may have moved */
    emit dirtyChanged();
}

void SceneEditController::selectAllGroups()
{
    model_.SetSelectedGroups({});
    emit selectedGroupsChanged();
    emit workingChanged();
    emit dirtyChanged();
}

/*---------------------------------------------------------*\
||| Save flow                                                ||
\*---------------------------------------------------------*/

void SceneEditController::save()
{
    if(saving_)
    {
        return;                 /* one SET_SCENE in flight */
    }
    if(!model_.HasWorking())
    {
        SetSaveError(QStringLiteral("nothing to save"));
        return;
    }
    const int id = bridge_->setScene(
        JsonToVariant(model_.Working()).toMap());
    if(id == 0)
    {
        /* Refused before the wire — honest, edit kept. */
        SetSaveError(bridge_->lastError().isEmpty()
                         ? QStringLiteral("not sent — engine not "
                                          "connected")
                         : bridge_->lastError());
        return;
    }
    pending_save_id_ = id;
    saving_ = true;
    emit savingChanged();
    SetSaveError(QString());
}

void SceneEditController::discardEdits()
{
    model_.DiscardEdits();
    TouchWorking();
}

void SceneEditController::reloadFromEngine()
{
    bridge_->getScene();
}

/*---------------------------------------------------------*\
||| Playback requests                                        ||
\*---------------------------------------------------------*/

QString SceneEditController::CurrentControl() const
{
    const QVariantMap st =
        bridge_->playback().value(QStringLiteral("status")).toMap();
    const QString c =
        st.value(QStringLiteral("control")).toString();
    return c.isEmpty() ? QStringLiteral("playing") : c;
}

QString SceneEditController::RequestedMode() const
{
    const QString m = bridge_->playback()
                          .value(QStringLiteral("requested_mode"))
                          .toString();
    return m.isEmpty() ? QStringLiteral("live_effects") : m;
}

QVariantMap SceneEditController::FilteredParams() const
{
    QVariantMap params;
    PlaybackPresetInfo info;
    if(!PlaybackPresetFromName(model_.PresetId(), info))
    {
        return params;      /* no uploaded preset selected */
    }
    if(info.speed)
    {
        params.insert(QStringLiteral("speed"), param_speed_);
    }
    if(info.direction)
    {
        params.insert(QStringLiteral("reverse"), param_reverse_);
    }
    if(info.color)
    {
        params.insert(QStringLiteral("color"), param_color_);
    }
    return params;
}

int SceneEditController::requestMode(const QString& mode)
{
    if(mode != QLatin1String("live_effects") &&
       mode != QLatin1String("animation_playback"))
    {
        return 0;
    }
    /* Always carry the CURRENT control — the wire defaults an
       omitted control to "playing", which would silently
       resume a paused upload (4.1 deferred minor). Params go
       through FilteredParams so unsupported fields are never
       emitted. */
    return bridge_->setPlaybackMode(mode, CurrentControl(),
                                    FilteredParams());
}

int SceneEditController::setPlaybackControl(const QString& control)
{
    if(control != QLatin1String("playing") &&
       control != QLatin1String("paused") &&
       control != QLatin1String("stopped"))
    {
        return 0;
    }
    return bridge_->setPlaybackMode(RequestedMode(), control,
                                    FilteredParams());
}

int SceneEditController::sendPlaybackParams(int speed, bool reverse,
                                            const QString& colorHex)
{
    /* Only fields the selected uploaded preset supports may
       be emitted — the engine is the backstop (it validates),
       the UI is the filter (protocol §5 table). */
    PlaybackPresetInfo info;
    const bool is_playback =
        PlaybackPresetFromName(model_.PresetId(), info);

    QVariantMap params;
    if(is_playback && info.speed)
    {
        params.insert(QStringLiteral("speed"),
                      ClampPlaybackSpeed(speed));
        param_speed_ = ClampPlaybackSpeed(speed);
    }
    if(is_playback && info.direction)
    {
        params.insert(QStringLiteral("reverse"), reverse);
        param_reverse_ = reverse;
    }
    if(is_playback && info.color &&
       SceneEditModel::IsHexColor(colorHex.toStdString()))
    {
        const std::string norm =
            SceneEditModel::NormalizeHexColor(colorHex.toStdString());
        params.insert(QStringLiteral("color"),
                      QString::fromStdString(norm));
        param_color_ = QString::fromStdString(norm);
    }
    emit paramsChanged();
    if(params.isEmpty())
    {
        return 0;    /* nothing supported — nothing sent */
    }
    return bridge_->setPlaybackMode(RequestedMode(),
                                    CurrentControl(), params);
}

/*---------------------------------------------------------*\
||| Profiles                                                 ||
\*---------------------------------------------------------*/

bool SceneEditController::saveProfile(const QString& name)
{
    if(!model_.HasWorking())
    {
        SetSaveError(QStringLiteral("no scene document to save"));
        return false;
    }
    /* The stored copy gets the profile's name as the document
       name so engine-side per-scene persistence keys on it
       (PlaybackController::SceneKey — "name:<name>"). */
    const QString trimmed = name.trimmed();
    nlohmann::json doc = model_.Working();
    doc["name"] = trimmed.toStdString();
    return library_.saveProfile(trimmed, JObj(doc));
}

bool SceneEditController::loadProfile(const QString& name)
{
    const QVariantMap doc = library_.profileDocument(name);
    if(doc.isEmpty())
    {
        SetSaveError(QStringLiteral("profile '%1' not found")
                         .arg(name));
        return false;
    }
    model_.LoadWorking(VariantToJson(doc));
    SetSaveError(QString());
    emit dirtyChanged();
    TouchWorking();
    return true;
}

bool SceneEditController::deleteProfile(const QString& name)
{
    return library_.removeProfile(name);
}

/*---------------------------------------------------------*\
||| Accessors                                                ||
\*---------------------------------------------------------*/

bool SceneEditController::hasScene() const
{
    return model_.HasBaseline();
}

bool SceneEditController::hasWorking() const
{
    return model_.HasWorking();
}

bool SceneEditController::dirty() const    { return model_.Dirty(); }
bool SceneEditController::conflict() const { return model_.Conflict(); }
bool SceneEditController::presetReplacedLayers() const
{
    return model_.PresetReplacedLayers();
}
bool    SceneEditController::saving() const   { return saving_; }
QString SceneEditController::saveError() const { return save_error_; }

QString SceneEditController::sceneName() const
{
    return JStr(model_.Working(), "name");
}

QString SceneEditController::presetId() const
{
    return QString::fromStdString(model_.PresetId());
}

namespace
{
double JNum(const nlohmann::json& j, const char* key, double def)
{
    const auto it = j.find(key);
    return (it != j.end() && it->is_number())
               ? it->get<double>()
               : def;
}
} /* anonymous namespace */

double SceneEditController::brightness() const
{
    return JNum(model_.Working(), "brightness", 1.0);
}

double SceneEditController::effectSpeed() const
{
    const nlohmann::json& w = model_.Working();
    if(w.is_object() && w.contains("effect") &&
       w["effect"].is_object())
    {
        return JNum(w["effect"], "speed", 1.0);
    }
    return 1.0;
}

double SceneEditController::effectIntensity() const
{
    const nlohmann::json& w = model_.Working();
    if(w.is_object() && w.contains("effect") &&
       w["effect"].is_object())
    {
        return JNum(w["effect"], "intensity", 1.0);
    }
    return 1.0;
}

QVariantMap SceneEditController::workingDocument() const
{
    return JObj(model_.Working());
}

/*---------------------------------------------------------*\
||| Groups + resolution hints                                ||
\*---------------------------------------------------------*/

QVariantMap SceneEditController::ResolveGroupHints(
    const DeviceGroup& g) const
{
    QVariantMap out;
    out.insert(QStringLiteral("state"),
               QStringLiteral("unbound"));

    if(g.binding.empty() || g.controller_name.empty())
    {
        return out;              /* unbound object */
    }

    /* Collect candidate snapshots the way BindingResolver
       does: serial wins, else name-substring + vendor +
       device_type, then location narrows. */
    QVariantList matches;
    for(const QVariant& sv : bridge_->snapshots())
    {
        const QVariantMap snap = sv.toMap();
        const QVariantMap key  =
            snap.value(QStringLiteral("key")).toMap();
        const QString name =
            snap.value(QStringLiteral("name")).toString();
        if(!g.serial.empty() &&
           key.value(QStringLiteral("serial")).toString() !=
               QString::fromStdString(g.serial))
        {
            continue;
        }
        if(!NameMatches(QString::fromStdString(g.controller_name),
                        name))
        {
            continue;
        }
        matches.push_back(snap);
    }
    if(matches.isEmpty())
    {
        out.insert(QStringLiteral("state"),
                   QStringLiteral("absent"));
        return out;
    }
    if(matches.size() > 1 && !g.location.empty())
    {
        QVariantList narrowed;
        for(const QVariant& mv : matches)
        {
            if(mv.toMap().value(QStringLiteral("key")).toMap()
                   .value(QStringLiteral("location")).toString() ==
               QString::fromStdString(g.location))
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
        out.insert(QStringLiteral("state"),
                   QStringLiteral("ambiguous"));
        return out;
    }

    const QVariantMap snap = matches.first().toMap();
    out.insert(QStringLiteral("state"),
               QStringLiteral("resolved"));
    out.insert(QStringLiteral("snapshot_name"),
               snap.value(QStringLiteral("name")).toString());
    out.insert(QStringLiteral("connected"),
               snap.value(QStringLiteral("connection")).toString() ==
                   QLatin1String("connected"));
    /* Zone LED count from the snapshot's own zones. */
    int leds = 0;
    if(!g.zone_name.empty())
    {
        for(const QVariant& zv :
            snap.value(QStringLiteral("zones")).toList())
        {
            const QVariantMap z = zv.toMap();
            if(z.value(QStringLiteral("name")).toString() ==
               QString::fromStdString(g.zone_name))
            {
                leds = z.value(QStringLiteral("led_count")).toInt();
                break;
            }
        }
    }
    else
    {
        leds = snap.value(QStringLiteral("led_count")).toInt();
    }
    out.insert(QStringLiteral("led_count"), leds);
    return out;
}

QVariantList SceneEditController::deviceGroups()
{
    QVariantList out;
    for(const DeviceGroup& g : model_.DeviceGroups())
    {
        QVariantMap m;
        m.insert(QStringLiteral("id"),
                 QString::fromStdString(g.id));
        m.insert(QStringLiteral("label"),
                 QString::fromStdString(g.label));
        m.insert(QStringLiteral("binding"),
                 QString::fromStdString(g.binding));
        m.insert(QStringLiteral("controller"),
                 QString::fromStdString(g.controller_name));
        m.insert(QStringLiteral("zone"),
                 QString::fromStdString(g.zone_name));
        m.insert(QStringLiteral("zone_leds"), (int)g.zone_leds);
        m.insert(QStringLiteral("emitters"), (int)g.emitter_count);
        m.insert(QStringLiteral("verified"), g.verified);
        const QVariantMap hint = ResolveGroupHints(g);
        for(auto it = hint.begin(); it != hint.end(); ++it)
        {
            m.insert(it.key(), it.value());
        }
        out.push_back(m);
    }
    return out;
}

QStringList SceneEditController::selectedGroups() const
{
    QStringList out;
    for(const std::string& id : model_.SelectedGroups())
    {
        out.push_back(QString::fromStdString(id));
    }
    return out;
}

bool SceneEditController::groupsTargetable() const
{
    return model_.GroupsTargetable();
}

/*---------------------------------------------------------*\
||| Presets                                                  ||
\*---------------------------------------------------------*/

QVariantList SceneEditController::playbackPresets() const
{
    QVariantList out;
    for(const PlaybackPresetInfo& p : PlaybackPresetCatalog())
    {
        out.push_back(QVariantMap({
            { QStringLiteral("id"),
              QString::fromStdString(p.id) },
            { QStringLiteral("name"),
              QString::fromStdString(p.sdk_name) },
            { QStringLiteral("speed"),     p.speed     },
            { QStringLiteral("direction"), p.direction },
            { QStringLiteral("color"),     p.color     },
        }));
    }
    return out;
}

QVariantMap SceneEditController::presetSupport()
{
    QVariantMap out;
    PlaybackPresetInfo info;
    const bool pb =
        PlaybackPresetFromName(model_.PresetId(), info);
    out.insert(QStringLiteral("playback"),  pb);
    out.insert(QStringLiteral("speed"),     pb && info.speed);
    out.insert(QStringLiteral("direction"), pb && info.direction);
    out.insert(QStringLiteral("color"),     pb && info.color);
    /* `needs` travels on the PRESETS reply for registry
       looks — the reactive-input requirement (4.6 owns the
       inputs). */
    QString needs;
    for(const QVariant& pv : bridge_->presets())
    {
        const QVariantMap p = pv.toMap();
        if(p.value(QStringLiteral("id")).toString() ==
           presetId())
        {
            needs = p.value(QStringLiteral("needs")).toString();
            break;
        }
    }
    out.insert(QStringLiteral("needs"), needs);
    return out;
}

QVariantList SceneEditController::profiles()
{
    return library_.entries();
}

QString SceneEditController::libraryError()
{
    return library_.lastError();
}

int     SceneEditController::paramSpeed()   const { return param_speed_; }
bool    SceneEditController::paramReverse() const { return param_reverse_; }
QString SceneEditController::paramColor()   const { return param_color_; }

/*---------------------------------------------------------*\
||| Mutators                                                 ||
\*---------------------------------------------------------*/

void SceneEditController::SetSaveError(const QString& e)
{
    if(save_error_ == e)
    {
        return;
    }
    save_error_ = e;
    emit saveErrorChanged();
}

} /* namespace ui */
} /* namespace openrgb2 */
