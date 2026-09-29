/*---------------------------------------------------------*\
|||| SceneEditController.cpp                                   ||
||||                                                           ||
||||   SPDX-License-Identifier: GPL-2.0-or-later               ||
||\*---------------------------------------------------------*/

#include "SceneEditController.h"

#include "EngineBridge.h"
#include "JsonVariant.h"
#include "UiSettings.h"
#include "WorkspaceController.h"

#include "config/StudioConfig.h"

#include <algorithm>
#include <cmath>
#include <set>

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

/* The normalized playback-preset id a document names in
   effect.preset ("" when it names none) — compares cleanly
   against pending_default_preset_ regardless of the id
   style the document carries. */
std::string DocPresetId(const nlohmann::json& doc)
{
    if(doc.is_object())
    {
        const auto e = doc.find("effect");
        if(e != doc.end() && e->is_object())
        {
            const auto p = e->find("preset");
            if(p != e->end() && p->is_string())
            {
                return NormalizePresetId(p->get<std::string>());
            }
        }
    }
    return std::string();
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
                /* Correlators die with the session — a stale
                   pending_save_id_/echo revision must never
                   collide with request ids the NEXT session
                   hands out, and a pending new-instance
                   default has no session left to fire in. */
                pending_save_id_       = 0;
                pending_echo_revision_ = 0;
                pending_default_preset_.clear();
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
                            .arg(reason),
                        QStringLiteral("connection"));
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
    const QVariantMap scene  = bridge_->scene();
    const QVariant    doc_var =
        scene.value(QStringLiteral("document"));
    const nlohmann::json doc =
        doc_var.isValid() ? VariantToJson(doc_var) : nlohmann::json(nullptr);
    const bool was_dirty = model_.Dirty();
    const bool had_conf  = model_.Conflict();

    /* Correlate the push to our own save: the SET_SCENE OK
       and the scene event it triggered carry the SAME scene
       revision (docs §5 — OK {revision,name}, SCENE/event
       {revision,...}). A matching revision is our own apply
       echoed back reserialized — adopt it, never flag it.
       A strictly newer revision means a different apply
       superseded ours before the echo drained (queued events
       are latest-wins per kind): that IS a remote change and
       the pending correlation is dead. */
    if(WsMode())
    {
        /* Workspace mode: the shared session owns draft
           state — the applied scene push only drives the
           new-instance playback default and the scene-scoped
           param latches. model_ stays dormant. */
        if(!pending_default_preset_.isEmpty())
        {
            if(NormalizePresetId(CurrentPresetId()) !=
               pending_default_preset_.toStdString())
            {
                pending_default_preset_.clear();
            }
            else if(DocPresetId(doc) ==
                    pending_default_preset_.toStdString())
            {
                pending_default_preset_.clear();
                if(RequestedMode() !=
                   QLatin1String("animation_playback"))
                {
                    requestMode(
                        QStringLiteral("animation_playback"));
                }
            }
        }
        const std::string sname =
            JStr(doc, "name").toStdString();
        if(sname != last_scene_name_)
        {
            last_scene_name_ = sname;
            const bool had_state =
                param_speed_touched_ || param_reverse_touched_ ||
                param_color_touched_ || param_speed_ != 100 ||
                param_reverse_ ||
                param_color_ != QLatin1String("#FF0000");
            param_speed_           = 100;
            param_reverse_         = false;
            param_color_           = QStringLiteral("#FF0000");
            param_speed_touched_   = false;
            param_reverse_touched_ = false;
            param_color_touched_   = false;
            RestoreSceneParams(QString::fromStdString(sname));
            if(had_state || param_speed_touched_ ||
               param_reverse_touched_ || param_color_touched_ ||
               param_speed_ != 100 || param_reverse_ ||
               param_color_ != QLatin1String("#FF0000"))
            {
                emit paramsChanged();
            }
        }
        emit sceneChanged();
        emit groupsChanged();
        return;
    }

    const quint64 rev =
        scene.value(QStringLiteral("revision")).toULongLong();
    bool own_echo = false;
    if(pending_echo_revision_ != 0)
    {
        if(rev == pending_echo_revision_)
        {
            pending_echo_revision_ = 0;
            own_echo = true;
        }
        else if(rev > pending_echo_revision_)
        {
            pending_echo_revision_ = 0;
        }
    }
    if(own_echo)
    {
        model_.AdoptApplied(doc);
    }
    else
    {
        model_.LoadApplied(doc);
    }

    /* New-instance default (plan section 3.4; the Task 3.6
       ruling assigns the user-facing default to the UI): an
       explicit uploaded-preset pick armed the latch — once
       the APPLIED document actually names that preset
       (capability validation runs engine-side against the
       applied scene, never the pending working copy) and the
       recorded selection isn't already animation, request
       Animation playback. One-shot: it fires here or dies on
       a reverted pick — a remote apply that coincidentally
       names the same preset is indistinguishable and the
       default it produces is identical anyway. Existing/
       migrated scenes never reach this code: nothing else
       arms the latch. */
    if(!pending_default_preset_.isEmpty())
    {
        if(NormalizePresetId(model_.PresetId()) !=
           pending_default_preset_.toStdString())
        {
            pending_default_preset_.clear();   /* pick reverted */
        }
        else if(DocPresetId(model_.Baseline()) ==
                pending_default_preset_.toStdString())
        {
            pending_default_preset_.clear();
            if(RequestedMode() !=
               QLatin1String("animation_playback"))
            {
                requestMode(QStringLiteral("animation_playback"));
            }
        }
    }

    /* Deferred minor from 4.2: the param touched latches are
       scene-scoped — the engine keys persisted selections on
       the scene name, so a NEW applied name resets them (and
       the trackers back to defaults, the only seed the wire
       offers) — a param touched in one scene can never ride
       into another's requests. */
    const std::string sname =
        JStr(model_.Baseline(), "name").toStdString();
    if(sname != last_scene_name_)
    {
        last_scene_name_ = sname;
        const bool had_state =
            param_speed_touched_ || param_reverse_touched_ ||
            param_color_touched_ || param_speed_ != 100 ||
            param_reverse_ ||
            param_color_ != QLatin1String("#FF0000");
        param_speed_           = 100;
        param_reverse_         = false;
        param_color_           = QStringLiteral("#FF0000");
        param_speed_touched_   = false;
        param_reverse_touched_ = false;
        param_color_touched_   = false;
        /* Task 4.8: after the scene-scope reset, restore the
           params this UI last sent for THIS scene name — the
           wire reports revisions, not values, so this store
           is the only honest seed a UI restart has. */
        RestoreSceneParams(
            QString::fromStdString(sname));
        if(had_state || param_speed_touched_ ||
           param_reverse_touched_ || param_color_touched_ ||
           param_speed_ != 100 || param_reverse_ ||
           param_color_ != QLatin1String("#FF0000"))
        {
            emit paramsChanged();
        }
    }

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
        /* Record the committed revision BEFORE the echo can
           land: the scene event carrying this revision is our
           own save reserialized, adopted rather than
           conflicted (OK replies and EVENT pushes drain on
           different engine threads — the echo may arrive
           after this). */
        pending_echo_revision_ =
            body.value(QStringLiteral("revision")).toULongLong();
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
                         : msg,
                     QStringLiteral("engine"));
    }
    /* Any other reply type for the correlator is a protocol
       oddity — treat it like a failure, keep the edit. */
    else
    {
        SetSaveError(QStringLiteral("unexpected reply type %1 "
                                    "to SET_SCENE — edits "
                                    "preserved").arg(type),
                     QStringLiteral("engine"));
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

void SceneEditController::setUiSettings(UiSettings* prefs)
{
    ui_settings_ = prefs;
}

/*---------------------------------------------------------*\
|||| Shared-workspace adaptation (Task 4)                   ||
\*---------------------------------------------------------*/

void SceneEditController::setWorkspace(WorkspaceController* ws)
{
    ws_ = ws;
    if(ws_ == nullptr)
    {
        return;
    }
    /* Draft movement and every surfaced controller flag re-
       paint the same properties the legacy model used to own
       — the page never sees which document backs it. */
    connect(ws_, &WorkspaceController::draftEdited, this,
            [this] {
                emit workingChanged();
                emit groupsChanged();
                emit presetSupportChanged();
            });
    connect(ws_, &WorkspaceController::draftChanged, this,
            [this] {
                emit sceneChanged();
                emit workingChanged();
                emit groupsChanged();
                emit presetSupportChanged();
            });
    connect(ws_, &WorkspaceController::workspaceChanged, this,
            [this] {
                emit sceneChanged();
                emit groupsChanged();
            });
    connect(ws_, &WorkspaceController::dirtyChanged,
            this, &SceneEditController::dirtyChanged);
    connect(ws_, &WorkspaceController::conflictChanged,
            this, &SceneEditController::conflictChanged);
    connect(ws_, &WorkspaceController::savingChanged,
            this, &SceneEditController::savingChanged);
    connect(ws_, &WorkspaceController::errorChanged,
            this, &SceneEditController::saveErrorChanged);
}

bool SceneEditController::WsMode() const
{
    /* A loaded draft without a capable engine is still the
       shared draft (recovered local work keeps editing —
       save() reports the honest unavailable state). */
    return ws_ != nullptr &&
           ((ws_->available() && ws_->managed()) ||
            ws_->Session().HasWorking());
}

bool SceneEditController::WsCommit(
    const std::string& label,
    const std::function<bool(studio::StudioDocument&)>& mut)
{
    if(!WsMode() || !ws_->Session().HasWorking())
    {
        return false;
    }
    if(!ws_->Session().CommitDocEdit(label, mut))
    {
        return false;
    }
    ws_->NoteDraftEdited();
    return true;
}

std::string SceneEditController::CurrentPresetId() const
{
    if(WsMode() && ws_->Session().HasWorking())
    {
        return ws_->Session().Working().effect.preset;
    }
    return model_.PresetId();
}

std::vector<DeviceGroup> SceneEditController::CompactGroups() const
{
    std::vector<DeviceGroup> out;
    const studio::StudioDocument& w = ws_->Session().Working();
    /* Everything the compact document can take a color for:
       root instance ids plus nested instance/entity paths —
       deduped and ordered for a stable list. */
    std::set<std::string> ids;
    for(const auto& kv : w.devices)
    {
        ids.insert(kv.first);
    }
    for(const auto& kv : w.device_settings)
    {
        ids.insert(kv.first);
    }
    for(const auto& kv : w.object_colors)
    {
        ids.insert(kv.first);
    }
    for(const std::string& id : ids)
    {
        DeviceGroup g;
        g.id    = id;
        g.label = id;
        const auto sit = w.device_settings.find(id);
        if(sit != w.device_settings.end())
        {
            for(const auto& z : sit->second.zones)
            {
                g.verified = g.verified || z.second.verified;
                if(g.binding.empty() &&
                   !z.second.binding.empty())
                {
                    g.binding = z.second.binding;
                }
            }
        }
        if(!g.binding.empty())
        {
            const auto bit = w.bindings.find(g.binding);
            if(bit != w.bindings.end())
            {
                const studio::DeviceBinding& b = bit->second;
                g.controller_name = b.controller_name;
                g.vendor          = b.vendor;
                g.serial          = b.serial;
                g.location        = b.location;
                g.device_type     = b.device_type;
                g.zone_name       = b.zone_name;
                g.zone_leds       = b.zone_leds;
            }
        }
        /* emitter_count stays 0 — the compact document has no
           resolved emitters; that count is engine truth the
           UI does not fabricate. */
        out.push_back(std::move(g));
    }
    return out;
}

void SceneEditController::WsApplyTargets()
{
    if(!WsMode() || !ws_->Session().HasWorking() ||
       ws_->Session().Working().effect.layers.empty())
    {
        return;
    }
    const std::vector<std::string> sel = model_.SelectedGroups();
    WsCommit("set layer targets",
             [&sel](studio::StudioDocument& w) {
                 bool changed = false;
                 for(studio::EffectLayer& l : w.effect.layers)
                 {
                     if(l.targets != sel)
                     {
                         l.targets = sel;
                         changed   = true;
                     }
                 }
                 return changed;
             });
}

void SceneEditController::PersistSceneParams()
{
    if(ui_settings_ == nullptr)
    {
        return;
    }
    const QString sname =
        QString::fromStdString(last_scene_name_);
    if(sname.isEmpty())
    {
        return;   /* unnamed scenes key on a doc hash —
                     nothing stable to hang the memory on */
    }
    QVariantMap p;
    p.insert(QStringLiteral("speed"),           param_speed_);
    p.insert(QStringLiteral("reverse"),         param_reverse_);
    p.insert(QStringLiteral("color"),           param_color_);
    p.insert(QStringLiteral("touched_speed"),
             param_speed_touched_);
    p.insert(QStringLiteral("touched_reverse"),
             param_reverse_touched_);
    p.insert(QStringLiteral("touched_color"),
             param_color_touched_);
    ui_settings_->SetSceneParams(sname, p);
}

void SceneEditController::RestoreSceneParams(
    const QString& sceneName)
{
    if(ui_settings_ == nullptr || sceneName.isEmpty())
    {
        return;
    }
    const QVariantMap p = ui_settings_->SceneParams(sceneName);
    if(p.isEmpty())
    {
        return;
    }
    param_speed_   = p.value(QStringLiteral("speed"),
                             100).toInt();
    param_reverse_ = p.value(QStringLiteral("reverse"),
                             false).toBool();
    const QString c = p.value(QStringLiteral("color"))
                          .toString();
    if(SceneEditModel::IsHexColor(c.toStdString()))
    {
        param_color_ = c;
    }
    param_speed_touched_ =
        p.value(QStringLiteral("touched_speed")).toBool();
    param_reverse_touched_ =
        p.value(QStringLiteral("touched_reverse")).toBool();
    param_color_touched_ =
        p.value(QStringLiteral("touched_color")).toBool();
}

bool SceneEditController::setPreset(const QString& id)
{
    /* The scene-creation default (plan section 3.4 first-
       release rules; the Task 3.6 ruling assigns the
       user-facing default to the UI): picking one of the
       the uploaded presets for a NEW instance arms the
       Animation-playback default — it fires in
       OnSceneChanged once the engine has APPLIED a scene
       naming the preset. Existing/migrated scenes stay
       Live: profile loads (LoadWorking) and remote applies
       never arm the latch. */
    const std::string prev =
        NormalizePresetId(CurrentPresetId());
    if(WsMode())
    {
        if(id.isEmpty())
        {
            SetSaveError(QStringLiteral("empty preset id"),
                         QStringLiteral("input"));
            return false;
        }
        const std::string nid = id.toStdString();
        bool replaced = false;
        /* Same semantics as SceneEditModel::SetPreset: a
           named preset replaces the inline layer stack and
           marks the effect playing. */
        const bool ok = WsCommit("set preset",
            [&](studio::StudioDocument& w) {
                replaced = !w.effect.layers.empty();
                if(w.effect.preset == nid &&
                   w.effect.layers.empty() &&
                   w.effect.playing)
                {
                    return false;      /* no change */
                }
                w.effect.preset = nid;
                w.effect.layers.clear();
                w.effect.playing = true;
                return true;
            });
        if(!ok)
        {
            SetSaveError(QStringLiteral("no working workspace "
                                        "document"),
                         QStringLiteral("input"));
            return false;
        }
        ws_preset_replaced_ = replaced;
    }
    else
    {
        std::string err;
        if(!model_.SetPreset(id.toStdString(), err))
        {
            SetSaveError(QString::fromStdString(err),
                         QStringLiteral("input"));
            return false;
        }
    }
    PlaybackPresetInfo info;
    const std::string picked =
        NormalizePresetId(id.toStdString());
    if(PlaybackPresetFromName(id.toStdString(), info) &&
       picked != prev)
    {
        pending_default_preset_ = QString::fromStdString(picked);
    }
    else
    {
        pending_default_preset_.clear();
    }
    SetSaveError(QString());
    TouchWorking();
    return true;
}

bool SceneEditController::setSceneName(const QString& name)
{
    if(WsMode())
    {
        /* Same name rules as SceneEditModel::SetName —
           trimmed, non-empty, bounded — but the edit lands on
           the compact document's meta.name. */
        std::string trimmed = name.toStdString();
        const std::size_t b = trimmed.find_first_not_of(" \t\r\n");
        const std::size_t e = trimmed.find_last_not_of(" \t\r\n");
        trimmed = (b == std::string::npos)
                      ? std::string()
                      : trimmed.substr(b, e - b + 1);
        if(trimmed.empty() || trimmed.size() > 128)
        {
            SetSaveError(QStringLiteral("scene name is empty "
                                        "or over 128 characters"),
                         QStringLiteral("input"));
            return false;
        }
        if(!WsCommit("rename scene",
                     [&](studio::StudioDocument& w) {
                         if(w.meta.name == trimmed)
                         {
                             return false;
                         }
                         w.meta.name = trimmed;
                         return true;
                     }))
        {
            SetSaveError(QStringLiteral("no working workspace "
                                        "document"),
                         QStringLiteral("input"));
            return false;
        }
        TouchWorking();
        return true;
    }
    std::string err;
    if(!model_.SetName(name.toStdString(), err))
    {
        SetSaveError(QString::fromStdString(err),
                     QStringLiteral("input"));
        return false;
    }
    TouchWorking();
    return true;
}

void SceneEditController::setBrightness(double v)
{
    if(WsMode())
    {
        if(!std::isfinite(v))
        {
            return;
        }
        const float c = (float)std::min(1.0, std::max(0.0, v));
        if(WsCommit("set brightness",
                    [c](studio::StudioDocument& w) {
                        if(w.brightness == c)
                        {
                            return false;
                        }
                        w.brightness = c;
                        return true;
                    }))
        {
            TouchWorking();
        }
        return;
    }
    if(model_.SetBrightness(v))
    {
        TouchWorking();
    }
}

void SceneEditController::setEffectSpeed(double v)
{
    if(WsMode())
    {
        if(!std::isfinite(v))
        {
            return;
        }
        /* SceneEditModel::SetEffectSpeed clamps to the same
           bounds the scene schema enforces. */
        const float c = (float)std::min(8.0, std::max(0.05, v));
        if(WsCommit("set effect speed",
                    [c](studio::StudioDocument& w) {
                        if(w.effect.speed == c)
                        {
                            return false;
                        }
                        w.effect.speed = c;
                        return true;
                    }))
        {
            TouchWorking();
        }
        return;
    }
    if(model_.SetEffectSpeed(v))
    {
        TouchWorking();
    }
}

void SceneEditController::setEffectIntensity(double v)
{
    if(WsMode())
    {
        if(!std::isfinite(v))
        {
            return;
        }
        const float c = (float)std::min(1.0, std::max(0.0, v));
        if(WsCommit("set effect intensity",
                    [c](studio::StudioDocument& w) {
                        if(w.effect.intensity == c)
                        {
                            return false;
                        }
                        w.effect.intensity = c;
                        return true;
                    }))
        {
            TouchWorking();
        }
        return;
    }
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
        if(WsMode())
        {
            for(const DeviceGroup& g : CompactGroups())
            {
                ids.push_back(g.id);
            }
        }
        else
        {
            for(const DeviceGroup& g : model_.DeviceGroups())
            {
                ids.push_back(g.id);
            }
        }
    }
    if(ids.empty())
    {
        SetSaveError(QStringLiteral("no device objects in the "
                                    "scene"),
                     QStringLiteral("input"));
        return false;
    }
    if(WsMode())
    {
        if(!SceneEditModel::IsHexColor(hex.toStdString()))
        {
            SetSaveError(QStringLiteral("color must be "
                                        "\"#RRGGBB\""),
                         QStringLiteral("input"));
            return false;
        }
        const std::string n = SceneEditModel::NormalizeHexColor(
            hex.toStdString());
        const unsigned v = (unsigned)std::stoul(
            n.substr(1), nullptr, 16);
        const studio::SceneColor c = studio::MakeSceneColor(
            (v >> 16) & 0xFF, (v >> 8) & 0xFF, v & 0xFF);
        if(!WsCommit("set color",
                     [&](studio::StudioDocument& w) {
                         for(const std::string& id : ids)
                         {
                             w.object_colors[id] = c;
                         }
                         return !ids.empty();
                     }))
        {
            SetSaveError(QStringLiteral("no working workspace "
                                        "document"),
                         QStringLiteral("input"));
            return false;
        }
        SetSaveError(QString());
        TouchWorking();
        return true;
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
        SetSaveError(QString::fromStdString(err),
                     QStringLiteral("input"));
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
    WsApplyTargets();   /* no-op outside workspace mode      */
    emit selectedGroupsChanged();
    emit workingChanged();          /* targets may have moved */
    emit dirtyChanged();
}

void SceneEditController::selectAllGroups()
{
    model_.SetSelectedGroups({});
    WsApplyTargets();
    emit selectedGroupsChanged();
    emit workingChanged();
    emit dirtyChanged();
}

/*---------------------------------------------------------*\
||| Save flow                                                ||
\*---------------------------------------------------------*/

void SceneEditController::save()
{
    if(WsMode())
    {
        /* Shared-draft save: the workspace commit job carries
           the compact document; dirty/conflict/rejection
           handling lives in WorkspaceController. */
        if(!ws_->Session().HasWorking())
        {
            SetSaveError(QStringLiteral("nothing to save"),
                         QStringLiteral("input"));
            return;
        }
        if(ws_->Session().Working().devices.empty())
        {
            /* Same guard as the legacy path — a deviceless
               commit would freeze output while reporting
               success. */
            SetSaveError(QStringLiteral("no device objects in "
                                        "the scene"),
                         QStringLiteral("input"));
            return;
        }
        SetSaveError(QString());
        ws_->save();
        return;
    }
    if(saving_)
    {
        return;                 /* one SET_SCENE in flight */
    }
    if(!model_.HasWorking())
    {
        SetSaveError(QStringLiteral("nothing to save"),
                     QStringLiteral("input"));
        return;
    }
    if(model_.DeviceGroups().empty())
    {
        /* The engine applies whatever SET_SCENE carries — a
           document with no device objects drops every binding
           and freezes output on the last frame while the save
           reports success. Same rule setGroupColor enforces;
           a deliberate empty apply stays available through
           `openrgb2-ctl set-scene`. */
        SetSaveError(QStringLiteral("no device objects in the "
                                    "scene"),
                     QStringLiteral("input"));
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
                         : bridge_->lastError(),
                     QStringLiteral("connection"));
        return;
    }
    pending_save_id_ = id;
    saving_ = true;
    emit savingChanged();
    SetSaveError(QString());
}

void SceneEditController::discardEdits()
{
    /* Discarding reverts the working copy — the preset pick
       that armed the new-instance default is gone with it. */
    pending_default_preset_.clear();
    if(WsMode())
    {
        /* Back to the engine-accepted baseline — the durable
           draft record drops with it (the controller owns
           that bookkeeping). */
        ws_preset_replaced_ = false;
        ws_->resetEdits();
        TouchWorking();
        return;
    }
    model_.DiscardEdits();
    TouchWorking();
}

void SceneEditController::reloadFromEngine()
{
    if(WsMode())
    {
        /* Re-open the managed workspace from disk — the
           fetched document adopts wholesale. */
        ws_->reload();
        return;
    }
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
    if(!PlaybackPresetFromName(CurrentPresetId(), info))
    {
        return params;      /* no uploaded preset selected */
    }
    /* Only fields the user TOUCHED this session ride the
       request: the wire reports parameter revisions, not
       values, so an untouched field has nothing honest to
       send — and the engine preserves its recorded per-scene
       value for any field the request omits (protocol §5).
       Resending UI defaults for untouched fields would
       silently clobber the persisted selection. */
    if(info.speed && param_speed_touched_)
    {
        params.insert(QStringLiteral("speed"), param_speed_);
    }
    if(info.direction && param_reverse_touched_)
    {
        params.insert(QStringLiteral("reverse"), param_reverse_);
    }
    if(info.color && param_color_touched_)
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
    /* An explicit Live pick while the new-instance default is
       armed disarms it — the user's stated choice wins over
       the pending default. An explicit Animation pick leaves
       it armed as the retry net: a request rejected before
       the scene applies still defaults on apply. */
    if(mode == QLatin1String("live_effects"))
    {
        pending_default_preset_.clear();
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
       the UI is the filter (protocol §5 table). And only
       fields the user touched this session go on the wire: a
       changed argument marks its field touched (the QML
       controls also call noteParamTouched from the handler
       that moved, which covers a deliberate set-to-same-value
       a diff can't see). Fields still untouched are omitted
       so the engine preserves its recorded per-scene value —
       the wire reports parameter revisions, not values, so
       the UI has nothing honest to seed them with. */
    PlaybackPresetInfo info;
    const bool is_playback =
        PlaybackPresetFromName(CurrentPresetId(), info);

    QVariantMap params;
    if(is_playback && info.speed)
    {
        const int clamped = ClampPlaybackSpeed(speed);
        if(clamped != param_speed_)
        {
            param_speed_touched_ = true;
        }
        param_speed_ = clamped;
        if(param_speed_touched_)
        {
            params.insert(QStringLiteral("speed"), clamped);
        }
    }
    if(is_playback && info.direction)
    {
        if(reverse != param_reverse_)
        {
            param_reverse_touched_ = true;
        }
        param_reverse_ = reverse;
        if(param_reverse_touched_)
        {
            params.insert(QStringLiteral("reverse"), reverse);
        }
    }
    if(is_playback && info.color &&
       SceneEditModel::IsHexColor(colorHex.toStdString()))
    {
        const std::string norm =
            SceneEditModel::NormalizeHexColor(colorHex.toStdString());
        const QString qnorm = QString::fromStdString(norm);
        if(qnorm != param_color_)
        {
            param_color_touched_ = true;
        }
        param_color_ = qnorm;
        if(param_color_touched_)
        {
            params.insert(QStringLiteral("color"), qnorm);
        }
    }
    emit paramsChanged();
    if(params.isEmpty())
    {
        return 0;    /* nothing touched — nothing sent */
    }
    /* The last-sent set is the UI's seed for the next session
       — persist it under the applied scene's name (Task 4.8). */
    PersistSceneParams();
    return bridge_->setPlaybackMode(RequestedMode(),
                                    CurrentControl(), params);
}

void SceneEditController::noteParamTouched(const QString& field)
{
    /* Latching: once the user has touched a field this
       session, the tracked value is UI-authoritative and
       keeps riding subsequent requests (the engine merges
       omitted fields over its own record). */
    if(field == QLatin1String("speed"))
    {
        param_speed_touched_ = true;
    }
    else if(field == QLatin1String("reverse"))
    {
        param_reverse_touched_ = true;
    }
    else if(field == QLatin1String("color"))
    {
        param_color_touched_ = true;
    }
    /* Persist eagerly on the touch itself — the touched set
       is the restart seed, and a touch can precede a request
       that fails or a session that ends before one goes. */
    PersistSceneParams();
}

/*---------------------------------------------------------*\
||| Profiles                                                 ||
\*---------------------------------------------------------*/

bool SceneEditController::saveProfile(const QString& name)
{
    if(WsMode())
    {
        if(!ws_->Session().HasWorking())
        {
            SetSaveError(QStringLiteral("no workspace document "
                                        "to save"),
                         QStringLiteral("input"));
            return false;
        }
        /* The library stores the compact workspace document —
           loading it back stages a draft; importing it lands
           it on the engine as its own workspace. */
        const QString trimmed = name.trimmed();
        nlohmann::json doc =
            studio::ToJson(ws_->Session().Working());
        doc["name"] = trimmed.toStdString();
        return library_.saveProfile(trimmed, JObj(doc));
    }
    if(!model_.HasWorking())
    {
        SetSaveError(QStringLiteral("no scene document to save"),
                     QStringLiteral("input"));
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
                         .arg(name),
                     QStringLiteral("input"));
        return false;
    }
    /* A staged profile is an EXISTING scene instance — it
       keeps the engine's persisted selection (Live effects
       for migrated profiles), never the new-instance
       animation default. */
    pending_default_preset_.clear();
    if(WsMode())
    {
        /* Only compact workspace documents can become the
           shared draft — a stored legacy scene doc is refused
           honestly (the engine migrates it at import time). */
        studio::StudioDocument d;
        std::vector<std::string> errs;
        if(!studio::FromJson(VariantToJson(doc), d, &errs))
        {
            SetSaveError(QStringLiteral("profile '%1' is a "
                                        "legacy scene document — "
                                        "import it into a "
                                        "workspace instead")
                             .arg(name),
                         QStringLiteral("input"));
            return false;
        }
        ws_->Session().StageWorking(
            d, "load profile " + name.toStdString());
        ws_->NoteDraftEdited();
        ws_preset_replaced_ = false;
        SetSaveError(QString());
        TouchWorking();
        return true;
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

int SceneEditController::importProfilesToWorkspaces()
{
    if(ws_ == nullptr || !ws_->available())
    {
        SetSaveError(QStringLiteral("workspaces are not "
                                    "available on this engine"),
                     QStringLiteral("input"));
        return 0;
    }
    return ws_->importProfiles(&library_);
}

/*---------------------------------------------------------*\
||| Accessors                                                ||
\*---------------------------------------------------------*/

bool SceneEditController::hasScene() const
{
    if(WsMode())
    {
        return ws_->Session().HasBaseline();
    }
    return model_.HasBaseline();
}

bool SceneEditController::hasWorking() const
{
    if(WsMode())
    {
        return ws_->Session().HasWorking();
    }
    return model_.HasWorking();
}

bool SceneEditController::dirty() const
{
    return WsMode() ? ws_->dirty() : model_.Dirty();
}
bool SceneEditController::conflict() const
{
    return WsMode() ? ws_->conflict() : model_.Conflict();
}
bool SceneEditController::presetReplacedLayers() const
{
    return WsMode() ? ws_preset_replaced_
                    : model_.PresetReplacedLayers();
}
bool    SceneEditController::saving() const
{
    return WsMode() ? ws_->saving() : saving_;
}
QString SceneEditController::saveError() const
{
    /* In workspace mode engine-side failures surface through
       the workspace controller; a local refusal (input) still
       wins while it is fresh. */
    if(WsMode() && save_error_.isEmpty())
    {
        return ws_->error();
    }
    return save_error_;
}
QString SceneEditController::saveErrorSource() const
{
    if(WsMode() && save_error_.isEmpty())
    {
        return ws_->errorSource();
    }
    return save_error_source_;
}

QString SceneEditController::sceneName() const
{
    if(WsMode())
    {
        return ws_->Session().HasWorking()
            ? QString::fromStdString(
                  ws_->Session().Working().meta.name)
            : QString();
    }
    return JStr(model_.Working(), "name");
}

QString SceneEditController::presetId() const
{
    return QString::fromStdString(CurrentPresetId());
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
    if(WsMode())
    {
        return ws_->Session().HasWorking()
            ? (double)ws_->Session().Working().brightness
            : 1.0;
    }
    return JNum(model_.Working(), "brightness", 1.0);
}

double SceneEditController::effectSpeed() const
{
    if(WsMode())
    {
        return ws_->Session().HasWorking()
            ? (double)ws_->Session().Working().effect.speed
            : 1.0;
    }
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
    if(WsMode())
    {
        return ws_->Session().HasWorking()
            ? (double)ws_->Session().Working().effect.intensity
            : 1.0;
    }
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
    if(WsMode())
    {
        /* Scene-shaped PROJECTION of the compact draft — the
           page reads name/brightness/effect.layers/
           object_colors, so it sees the same fields it always
           did without ever learning the workspace schema. */
        if(!ws_->Session().HasWorking())
        {
            return QVariantMap();
        }
        const nlohmann::json j =
            studio::ToJson(ws_->Session().Working());
        nlohmann::json out = nlohmann::json::object();
        out["name"]       = j.value("name", std::string());
        out["brightness"] = j.value(
            "output", nlohmann::json::object())
                .value("brightness", 1.0);
        out["effect"]        = j.value("effects",
                                       nlohmann::json::object());
        out["object_colors"] = j.value(
            "colors", nlohmann::json::object())
                .value("objects", nlohmann::json::object());
        return JObj(out);
    }
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

    /* BindingResolver treats an empty controller_name as
       match-any — an unbound OBJECT is what reports
       "unbound", not a binding that leaves the name open
       (the hint must agree with the authoritative resolver;
       serial/vendor narrowing below still applies). */
    if(g.binding.empty())
    {
        return out;              /* unbound object */
    }

    /* Collect candidate snapshots the way BindingResolver
       does (src/core/scene/BindingResolver.cpp — the hint
       must agree with the engine's authoritative answer):
         1. serial non-empty -> serial + name must match
            (vendor/device_type are NOT consulted);
         2. otherwise name-substring AND device_type when the
            binding pins one (>= 0) AND vendor when non-empty
            (case-insensitive equality);
         3. >1 candidate -> a non-empty location narrows;
            still >1 -> ambiguous;
         4. one candidate -> a named zone must exist on it,
            else unresolved. */
    const QString vendor =
        QString::fromStdString(g.vendor).toLower();
    QVariantList matches;
    for(const QVariant& sv : bridge_->snapshots())
    {
        const QVariantMap snap = sv.toMap();
        const QVariantMap key  =
            snap.value(QStringLiteral("key")).toMap();
        const QString name =
            snap.value(QStringLiteral("name")).toString();
        if(!g.serial.empty())
        {
            /* Serial is the disambiguator — vendor and
               device_type stay out of it, mirroring the
               resolver's serial branch exactly. */
            if(key.value(QStringLiteral("serial")).toString() !=
                   QString::fromStdString(g.serial) ||
               !NameMatches(QString::fromStdString(g.controller_name),
                            name))
            {
                continue;
            }
            matches.push_back(snap);
            continue;
        }
        if(!NameMatches(QString::fromStdString(g.controller_name),
                        name))
        {
            continue;
        }
        if(g.device_type >= 0 &&
           snap.value(QStringLiteral("device_type")).toInt() !=
               g.device_type)
        {
            continue;
        }
        if(!vendor.isEmpty() &&
           snap.value(QStringLiteral("vendor")).toString().toLower() !=
               vendor)
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
    /* The single candidate must actually carry the bound
       zone — the resolver reports Unresolved when
       zone_name has no match on the controller. */
    int leds = 0;
    if(!g.zone_name.empty())
    {
        bool found = false;
        for(const QVariant& zv :
            snap.value(QStringLiteral("zones")).toList())
        {
            const QVariantMap z = zv.toMap();
            if(z.value(QStringLiteral("name")).toString() ==
               QString::fromStdString(g.zone_name))
            {
                found = true;
                leds  = z.value(QStringLiteral("led_count")).toInt();
                break;
            }
        }
        if(!found)
        {
            out.insert(QStringLiteral("state"),
                       QStringLiteral("absent"));
            return out;
        }
    }
    else
    {
        leds = snap.value(QStringLiteral("led_count")).toInt();
    }
    out.insert(QStringLiteral("state"),
               QStringLiteral("resolved"));
    out.insert(QStringLiteral("snapshot_name"),
               snap.value(QStringLiteral("name")).toString());
    out.insert(QStringLiteral("connected"),
               snap.value(QStringLiteral("connection")).toString() ==
                   QLatin1String("connected"));
    out.insert(QStringLiteral("led_count"), leds);
    return out;
}

QVariantList SceneEditController::deviceGroups()
{
    QVariantList out;
    const std::vector<DeviceGroup> groups =
        WsMode() ? CompactGroups() : model_.DeviceGroups();
    for(const DeviceGroup& g : groups)
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
    if(WsMode())
    {
        return ws_->Session().HasWorking() &&
               !ws_->Session().Working().effect.layers.empty();
    }
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
        PlaybackPresetFromName(CurrentPresetId(), info);
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

void SceneEditController::SetSaveError(const QString& e,
                                       const QString& source)
{
    const QString src = e.isEmpty() ? QString() : source;
    if(save_error_ == e && save_error_source_ == src)
    {
        return;
    }
    save_error_        = e;
    save_error_source_ = src;
    emit saveErrorChanged();
}

} /* namespace ui */
} /* namespace openrgb2 */
