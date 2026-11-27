/*---------------------------------------------------------*\
||| EffectEditorController.cpp                               ||
|||                                                          ||
|||   SPDX-License-Identifier: GPL-2.0-or-later               ||
\*---------------------------------------------------------*/

#include "EffectEditorController.h"

#include "../JsonVariant.h"

#include "effects/Presets.h"          /* EffectLooks / BuildPreset  */
#include "effects/EffectTypes.h"      /* HashU32                    */
#include "presets/DevicePreset.h"     /* IsPresetId                 */
#include "scene/SceneJson.h"          /* SceneColorHex              */
#include "scene/SceneResolver.h"      /* ResolveScene               */

#include <algorithm>
#include <functional>

namespace openrgb2
{
namespace ui
{
namespace
{

const char* BlendName(studio::BlendMode m)
{
    switch(m)
    {
    case studio::BlendMode::Add:    return "add";
    case studio::BlendMode::Screen: return "screen";
    default:                        return "replace";
    }
}

const char* SpaceName(studio::CoordSpace s)
{
    return s == studio::CoordSpace::Local ? "local" : "world";
}

QVariantMap VecMap(const studio::Vec3& v)
{
    QVariantMap o;
    o["x"] = v.x;  o["y"] = v.y;  o["z"] = v.z;
    return o;
}

studio::ColorF ColorOf(const QColor& c)
{
    studio::ColorF f;
    f.r = (float)c.redF();
    f.g = (float)c.greenF();
    f.b = (float)c.blueF();
    f.a = 1.0f;
    return f;
}

} /* anonymous namespace */

EffectEditorController::EffectEditorController(EngineBridge* bridge,
                                               WorkspaceController* ws,
                                               QObject* parent)
    : QObject(parent)
    , bridge_(bridge)
    , ws_(ws)
{
    if(ws_ != nullptr)
    {
        /* The session object lives inside the controller for its
           life — binding the resolver once covers every draft the
           session loads (the editor resolves through ctx, so a
           later-registered look resolves too). */
        ws_->Session().Editor().SetLayerResolver(&ResolveLayers, this);
        connect(ws_, &WorkspaceController::draftEdited,
                this, &EffectEditorController::OnWorkspaceDraftEdited);
        connect(ws_, &WorkspaceController::draftChanged,
                this, &EffectEditorController::OnWorkspaceDraftChanged);
        connect(ws_, &WorkspaceController::availabilityChanged,
                this, [this] {
                    emit readyChanged();
                    if(bridge_ != nullptr && bridge_->connected())
                    {
                        RequestEffectsCatalog();
                    }
                });
        connect(ws_, &WorkspaceController::historyChanged,
                this, [this] { /* undo labels live on `workspace` */ });
    }
    if(bridge_ != nullptr)
    {
        connect(bridge_, &EngineBridge::replyReceived,
                this, &EffectEditorController::OnReplyReceived);
        connect(bridge_, &EngineBridge::eventReceived,
                this, &EffectEditorController::OnEventReceived);
        connect(bridge_, &EngineBridge::presetsChanged,
                this, [this] { emit stackChanged(); });
        connect(bridge_, &EngineBridge::reactiveChanged,
                this, [this] {
                    ++input_state_rev_;
                    emit inputStateChanged();
                });
        connect(bridge_, &EngineBridge::connectionStateChanged,
                this, [this] {
                    emit readyChanged();
                    if(bridge_->connected())
                    {
                        RequestEffectsCatalog();
                    }
                    else
                    {
                        /* A pending request/job can never resolve
                           after disconnect — unlatch so the save
                           button doesn't wedge, and fail the
                           outcome honestly. */
                        pending_catalog_ = 0;
                        pending_catalog_entries_.clear();
                        if(pending_save_req_ != 0
                           || pending_save_job_ != 0
                           || saving_look_)
                        {
                            pending_save_req_ = 0;
                            pending_save_job_ = 0;
                            saving_look_      = false;
                            emit lookSaveChanged();
                            QVariantMap res;
                            res["ok"]     = false;
                            res["id"]     = pending_save_id_;
                            res["errors"] = QStringList{
                                QStringLiteral(
                                    "engine disconnected during "
                                    "the look install") };
                            emit lookSaveResult(res);
                        }
                    }
                });
        if(bridge_->connected())
        {
            RequestEffectsCatalog();
        }
    }
    RefreshModel();
}

/*---------------------------------------------------------*\
|| session access + state                                   ||
\*---------------------------------------------------------*/

WorkspaceEditSession* EffectEditorController::Session() const
{
    return ws_ != nullptr ? &ws_->Session() : nullptr;
}

studio::EditorController* EffectEditorController::Editor() const
{
    return ws_ != nullptr ? &ws_->Session().Editor() : nullptr;
}

const studio::StudioDocument* EffectEditorController::WsDoc() const
{
    WorkspaceEditSession* s = Session();
    if(s == nullptr || !s->HasWorking())
    {
        return nullptr;
    }
    return &s->Working();
}

bool EffectEditorController::ready() const
{
    return WsDoc() != nullptr;
}

bool EffectEditorController::gestureActive() const
{
    const studio::EditorController* e = Editor();
    return e != nullptr &&
           (e->LayerGestureActive() || e->GestureActive());
}

QString EffectEditorController::presetId() const
{
    const studio::StudioDocument* d = WsDoc();
    return d != nullptr
        ? QString::fromStdString(d->effect.preset) : QString();
}

bool EffectEditorController::hasInlineLayers() const
{
    const studio::StudioDocument* d = WsDoc();
    return d != nullptr && !d->effect.layers.empty();
}

int EffectEditorController::layerCount() const
{
    return (int)EffectiveLayers().size();
}

int EffectEditorController::decayPct() const
{
    const studio::StudioDocument* d = WsDoc();
    return d != nullptr ? d->inputs.decay_pct : 100;
}

std::vector<studio::EffectLayer>
EffectEditorController::EffectiveLayers() const
{
    const studio::StudioDocument* d = WsDoc();
    if(d == nullptr)
    {
        return {};
    }
    if(!d->effect.layers.empty())
    {
        return d->effect.layers;
    }
    std::vector<studio::EffectLayer> out;
    if(!d->effect.preset.empty())
    {
        ResolveLayersImpl(d->effect.preset, d->effect.seed, out);
    }
    return out;
}

bool EffectEditorController::ResolveLayers(
    const std::string& preset_id, unsigned int seed,
    std::vector<studio::EffectLayer>& out, void* ctx)
{
    const EffectEditorController* self =
        static_cast<const EffectEditorController*>(ctx);
    if(self == nullptr)
    {
        return studio::EffectLooks().Build(preset_id, seed,
                                           out, nullptr);
    }
    return self->ResolveLayersImpl(preset_id, seed, out);
}

bool EffectEditorController::ResolveLayersImpl(
    const std::string& preset_id, unsigned int seed,
    std::vector<studio::EffectLayer>& out) const
{
    /* Locally-known looks first (a just-saved look resolves before
       the engine's next preset list lands), then the packaged
       registry — the same order the engine's effective registry
       applies (file layer overrides defaults). */
    if(ui_looks_.Find(preset_id) != nullptr &&
       ui_looks_.Build(preset_id, seed, out, nullptr))
    {
        return true;
    }
    return studio::EffectLooks().Build(preset_id, seed, out, nullptr);
}

/*---------------------------------------------------------*\
|| reads                                                    ||
\*---------------------------------------------------------*/

QVariantMap EffectEditorController::layerAt(int index) const
{
    const std::vector<studio::EffectLayer> layers =
        EffectiveLayers();
    QVariantMap m;
    if(index < 0 || index >= (int)layers.size())
    {
        return m;
    }
    const studio::EffectLayer& l = layers[index];
    m["index"]     = index;
    m["primitive"] = QString::fromStdString(l.primitive);
    m["enabled"]   = l.enabled;
    m["blend"]     = BlendName(l.blend);
    m["opacity"]   = l.opacity;
    m["space"]     = SpaceName(l.space);
    m["speed"]     = l.speed;
    m["scale"]     = l.scale;
    m["phase"]     = l.phase;
    m["density"]   = l.density;
    m["origin"]    = VecMap(l.origin);
    m["direction"] = VecMap(l.direction);
    QVariantList path;
    for(const studio::Vec3& p : l.path)
    {
        path.push_back(VecMap(p));
    }
    m["path"] = path;
    QVariantList stops;
    for(const studio::PaletteStop& s : l.palette.stops)
    {
        QVariantMap st;
        st["pos"]   = s.pos;
        st["color"] = QString::fromStdString(
            studio::SceneColorHex(studio::ToSceneColor(s.color)));
        stops.push_back(st);
    }
    m["palette"] = stops;
    QStringList targets;
    for(const std::string& t : l.targets)
    {
        targets << QString::fromStdString(t);
    }
    m["targets"] = targets;
    m["source"]  = QString::fromStdString(l.source);
    m["seed"]    = (double)l.seed;
    return m;
}

QVariantList EffectEditorController::targetIds() const
{
    /* Valid layer-target terms: resolved object ids + geometry
       tags + emitter groups — the dead-target guard matches
       against the same terms. The draft resolves through an
       empty registry context-free object set is impossible, so
       unresolvable drafts report the device-instance ids — the
       honest subset. */
    QVariantList out;
    std::set<std::string> seen;
    const auto add = [&out, &seen](const std::string& s) {
        if(!s.empty() && seen.insert(s).second)
        {
            out.push_back(QString::fromStdString(s));
        }
    };
    const studio::StudioDocument* d = WsDoc();
    if(d == nullptr)
    {
        return out;
    }
    for(const auto& kv : d->devices)
    {
        add(kv.first);
    }
    studio::SceneDocument resolved;
    if(studio::ResolveScene(*d, studio::PresetRegistry(),
                            resolved, nullptr))
    {
        for(const studio::SceneObject& o : resolved.objects)
        {
            add(o.id);
            add(o.geometry);
            for(const studio::Emitter& em : o.emitters)
            {
                add(em.group);
            }
        }
    }
    return out;
}

QVariantMap EffectEditorController::inputSourceState(
    const QString& source) const
{
    QVariantMap m;
    const QString s = source.toLower();
    m["name"] = s;
    m["enabled"] = false;
    m["ready"]   = false;
    m["status"]  = QString();
    if(bridge_ == nullptr)
    {
        return m;
    }
    const QVariantMap reactive = bridge_->reactive();
    const QVariantList inputs =
        reactive.value("inputs").toList();
    for(const QVariant& v : inputs)
    {
        const QVariantMap it = v.toMap();
        if(it.value("id").toString() == s)
        {
            m["enabled"] = it.value("enabled").toBool();
            m["ready"]   = it.value("available").toBool()
                        && it.value("running").toBool();
            m["status"]  = it.value("status").toString();
            return m;
        }
    }
    return m;
}

/*---------------------------------------------------------*\
|| commit plumbing                                          ||
\*---------------------------------------------------------*/

void EffectEditorController::ApplyOp(
    std::optional<studio::EditorEdit>&& e)
{
    if(e.has_value())
    {
        Session()->CommitEdit(*e);
        /* WorkspaceController::draftEdited fires for undo/redo/
           remote moves — NOT for a local CommitEdit. The adapter
           refreshes its own model here (the ObjectEditorController
           contract). */
        ws_->NoteDraftEdited();
        RefreshModel();
        emit stackChanged();
        return;
    }
    if(Editor() != nullptr && Editor()->LayerGestureActive())
    {
        /* Inside a gesture a nullopt means the write landed as
           preview state — the model still refreshes so the panel
           follows the pointer; no record, no dirty mark. */
        RefreshModel();
        emit stackChanged();
        return;
    }
    if(Editor() != nullptr && !Editor()->LastError().empty())
    {
        emit statusMessage(
            QString::fromStdString(Editor()->LastError()));
    }
}

void EffectEditorController::RefreshModel()
{
    layer_model_.SetStack(EffectiveLayers());
    if(selected_layer_ >= (int)layer_model_.count())
    {
        selected_layer_ = (int)layer_model_.count() - 1;
        emit selectedLayerChanged();
    }
}

bool EffectEditorController::CommitInputs(
    const std::string& label,
    const std::function<bool(studio::StudioDocument&)>& mut)
{
    WorkspaceEditSession* s = Session();
    if(s == nullptr || !s->HasWorking())
    {
        return false;
    }
    if(!s->CommitDocEdit(label, mut))
    {
        return false;
    }
    ws_->NoteDraftEdited();
    emit draftInputsChanged();
    return true;
}

void EffectEditorController::SetStatus(const QString& s)
{
    if(status_text_ == s)
    {
        return;
    }
    status_text_ = s;
    emit statusChanged();
}

void EffectEditorController::OnWorkspaceDraftEdited()
{
    RefreshModel();
    emit stackChanged();
    emit draftInputsChanged();
}

void EffectEditorController::OnWorkspaceDraftChanged()
{
    RefreshModel();
    emit readyChanged();
    emit stackChanged();
    emit draftInputsChanged();
}

/*---------------------------------------------------------*\
|| look selection                                           ||
\*---------------------------------------------------------*/

bool EffectEditorController::selectPreset(const QString& id)
{
    if(!ready() || gestureActive())
    {
        if(gestureActive())
        {
            emit statusMessage(QStringLiteral(
                "look pick refused — finish the drag first"));
        }
        return false;
    }
    ApplyOp(Editor()->SelectPreset(id.toStdString()));
    /* Picking a look marks the effect playing — same authored
       semantic as the Lighting page's preset pick. Not part of
       the EffectDelta (playback state is a sibling field); a
       second record only exists when it was off. */
    const studio::StudioDocument* d = WsDoc();
    if(d != nullptr && !d->effect.playing)
    {
        CommitInputs("play effect",
            [](studio::StudioDocument& w) {
                w.effect.playing = true;
                return true;
            });
    }
    return true;
}

bool EffectEditorController::resetToPreset()
{
    if(!ready() || gestureActive())
    {
        return false;
    }
    const studio::StudioDocument* d = WsDoc();
    if(d == nullptr || d->effect.layers.empty())
    {
        return false;   /* already resolving the named look */
    }
    ApplyOp(Editor()->SetLayers({}, d->effect.preset));
    return true;
}

bool EffectEditorController::remix()
{
    const studio::StudioDocument* d = WsDoc();
    if(d == nullptr || d->effect.preset.empty() || gestureActive())
    {
        return false;
    }
    /* A fresh seed re-rolls every bounded random choice in the
       look; the seed persists with the workspace, so a remix is
       reproducible — same step as the source bridge. */
    return CommitInputs("remix look",
        [](studio::StudioDocument& w) {
            w.effect.seed =
                studio::HashU32(w.effect.seed ^ 0x5D15A5E9u) + 1u;
            return true;
        });
}

/*---------------------------------------------------------*\
|| save-as personal look                                    ||
\*---------------------------------------------------------*/

QVariantMap EffectEditorController::saveLookAs(const QString& id,
                                             const QString& name)
{
    QVariantMap out;
    out["ok"] = false;
    const std::string lid = id.trimmed().toStdString();
    if(!studio::IsPresetId(lid))
    {
        out["errors"] = QStringList{ QStringLiteral(
            "bad look id '%1' — expected [A-Za-z0-9_-]").arg(id) };
        return out;
    }
    if(gestureActive())
    {
        /* Adopting the saved look is refused mid-gesture — refuse
           the whole save up front so ok:true never reports a
           half-done adopt (the source bridge's contract). */
        out["errors"] = QStringList{ QStringLiteral(
            "finish the current edit first") };
        return out;
    }
    if(!ready())
    {
        out["errors"] = QStringList{ QStringLiteral(
            "no workspace draft is loaded") };
        return out;
    }
    if(bridge_ == nullptr || !bridge_->connected())
    {
        out["errors"] = QStringList{ QStringLiteral(
            "engine not connected — looks install through "
            "the workspace job surface") };
        return out;
    }
    if(saving_look_)
    {
        out["errors"] = QStringList{ QStringLiteral(
            "a look install is already in flight") };
        return out;
    }

    /* The saved look's layers are the EFFECTIVE stack — resolved
       literals, no remix specs (a saved look is what the user
       sees, not a template). */
    const std::vector<studio::EffectLayer> stack = EffectiveLayers();
    studio::EffectDocument d;
    d.id   = lid;
    d.name = name.trimmed().isEmpty()
        ? lid : name.trimmed().toStdString();
    /* Derive `needs` from the layers: the first non-empty source
       wins; a screenfield primitive implies screen sampling even
       with no source set. */
    std::string needs;
    for(const studio::EffectLayer& l : stack)
    {
        const std::string want =
            !l.source.empty() ? l.source
            : l.primitive == "screenfield" ? "screen" : "";
        if(!want.empty())
        {
            needs = want;
            break;
        }
    }
    d.needs = needs;
    /* Canonical field order keeps the stored document readable —
       the plain-json serializer would sort keys alphabetically. */
    static const char* const order[] = {
        "primitive", "space", "blend", "opacity", "speed", "scale",
        "phase", "density", "origin", "direction", "path",
        "palette", "targets", "source", "seed", "enabled",
    };
    d.layers = nlohmann::ordered_json::array();
    for(const studio::EffectLayer& l : stack)
    {
        const nlohmann::json jl = studio::EffectLayerToJson(l);
        nlohmann::ordered_json ol;
        for(const char* k : order)
        {
            if(jl.contains(k))
            {
                ol[k] = jl[k];
            }
        }
        d.layers.push_back(ol);
    }

    QVariantMap args;
    args["id"]     = ws_->workspaceId();
    args["effect"] = JsonToVariant(studio::EffectDocumentToJson(d));
    args["base_revision"]     = ws_->workspaceRevision();
    args["resource_revision"] = ws_->resourceRevision();

    const int req = bridge_->workspaceCommand(
        QStringLiteral("resource_install"), args);
    if(req <= 0)
    {
        out["errors"] = QStringList{ QStringLiteral(
            "resource_install could not be sent") };
        return out;
    }
    pending_save_req_ = req;
    pending_save_job_ = 0;
    pending_save_id_  = QString::fromStdString(lid);
    pending_save_name_ = QString::fromStdString(d.name);
    saving_look_      = true;
    emit lookSaveChanged();
    out["ok"] = true;
    out["id"] = QString::fromStdString(lid);
    return out;
}

void EffectEditorController::refreshLooks()
{
    RequestEffectsCatalog();
}

void EffectEditorController::ApplyEffectsCatalog(
    const QVariantList& entries)
{
    QVariantList rows;
    /* The COMPILED-IN look ids — NOT bridge_->presets():
       GetPresets already merges file-supplied looks, so
       filtering against it would hide every personal row. */
    std::set<std::string> packaged;
    for(const studio::PresetInfo& p : studio::PresetList())
    {
        packaged.insert(p.id);
    }
    bool registered = false;
    for(const QVariant& v : entries)
    {
        const QVariantMap e = v.toMap();
        const QString eid = e.value("id").toString();
        /* File-supplied entries carry their document — feed
           the local registry so a saved look resolves across
           sessions/restarts, not only in the saving session. */
        /* document_text keeps the serialized ordered_json byte
           order — remix draws consume the RNG stream in field
           order, so a re-keyed JSON object would resolve the
           same look differently UI-side vs engine-side. The
           legacy `document` object remains a fallback. */
        const QVariant doct = e.value("document_text");
        const QVariant docv = e.value("document");
        if(doct.isValid() || docv.isValid())
        {
            const nlohmann::ordered_json j =
                nlohmann::ordered_json::parse(
                    doct.isValid()
                        ? doct.toString().toStdString()
                        : VariantToJson(docv).dump(),
                    nullptr, false);
            studio::EffectDocument d;
            std::vector<std::string> errs;
            if(studio::EffectDocumentFromJson(j, d, &errs) &&
               ui_looks_.Add(d, nullptr))
            {
                registered = true;
            }
        }
        /* The shelf lists LOOKS the packaged registry doesn't
           carry — personal installs + engine-side files. */
        const bool personal = e.value("from_file").toBool()
            && !packaged.count(eid.toStdString());
        if(!personal)
        {
            continue;
        }
        QVariantMap row;
        row["id"]   = eid;
        row["name"] = e.value("name").toString();
        row["needs"] = e.value("needs").toString();
        row["fromFile"] = true;
        rows.push_back(row);
    }
    looks_ = rows;
    emit looksChanged();
    if(registered)
    {
        /* Newly-resolvable ids can change the effective stack
           a named-preset draft displays. */
        RefreshModel();
        emit stackChanged();
    }
}

void EffectEditorController::RequestEffectsCatalog()
{
    /* Attempt count is recorded even when the wire refuses —
       the offline suite asserts the save-completion path
       re-requested the catalog. */
    ++catalog_requests_;
    if(bridge_ == nullptr || !bridge_->connected())
    {
        return;
    }
    pending_catalog_entries_.clear();
    pending_catalog_off_ = 0;
    pending_catalog_ = bridge_->workspaceCommand(
        QStringLiteral("catalog"),
        QVariantMap{{"kind", QStringLiteral("effects")}});
}

/*---------------------------------------------------------*\
|| wire replies + job events                                ||
\*---------------------------------------------------------*/

void EffectEditorController::OnReplyReceived(int requestId,
                                             const QString& typeName,
                                             const QVariantMap& body)
{
    /* Catalog replies are matched by the request correlator —
       another controller's replies never land here. */
    if(pending_catalog_ != 0 && requestId == pending_catalog_)
    {
        pending_catalog_ = 0;
        if(typeName != QLatin1String("OK") ||
           body.value("kind").toString() != QLatin1String("effects"))
        {
            pending_catalog_entries_.clear();
            return;   /* Unavailable/ERROR — local rows stand */
        }
        pending_catalog_entries_.append(
            body.value("entries").toList());
        /* Byte-budgeted pages: keep walking while the engine
           reports more — every look stays reachable across
           pages without any reply exceeding the 1 MiB body
           cap. next_offset MUST advance: a stalled walk stops
           here with the entries gathered so far rather than
           spinning forever. */
        const quint64 next =
            body.value("next_offset").toULongLong();
        if(body.value("more").toBool() &&
           next > pending_catalog_off_ &&
           bridge_ != nullptr && bridge_->connected())
        {
            pending_catalog_off_ = next;
            pending_catalog_ = bridge_->workspaceCommand(
                QStringLiteral("catalog"),
                QVariantMap{
                    {"kind",   QStringLiteral("effects")},
                    {"offset", next}});
            return;
        }
        const QVariantList all = pending_catalog_entries_;
        pending_catalog_entries_.clear();
        ApplyEffectsCatalog(all);
        return;
    }

    if(pending_save_req_ != 0 && requestId == pending_save_req_)
    {
        pending_save_req_ = 0;
        if(typeName != QLatin1String("OK") ||
           body.value("accepted").toBool() == false)
        {
            const QString err =
                body.value("error").toString().isEmpty()
                ? QStringLiteral("look install refused")
                : body.value("error").toString();
            saving_look_ = false;
            emit lookSaveChanged();
            QVariantMap res;
            res["ok"]     = false;
            res["id"]     = pending_save_id_;
            res["errors"] = QStringList{ err };
            emit lookSaveResult(res);
            return;
        }
        pending_save_job_ =
            body.value("job_id").toULongLong();
        if(pending_save_job_ == 0)
        {
            /* Accepted replies always carry job_id — none means
               nothing will ever resolve; fail honestly. */
            saving_look_ = false;
            emit lookSaveChanged();
            QVariantMap res;
            res["ok"]     = false;
            res["id"]     = pending_save_id_;
            res["errors"] = QStringList{ QStringLiteral(
                "engine accepted no job for the install") };
            emit lookSaveResult(res);
        }
        return;
    }
}

void EffectEditorController::OnEventReceived(const QString& kind,
                                             const QVariantMap& data)
{
    if(kind != QLatin1String("workspace"))
    {
        return;
    }
    if(!data.contains("job_id"))
    {
        return;   /* snapshot event, not a job status record */
    }
    const quint64 job = data.value("job_id").toULongLong();
    if(job == 0 || job != pending_save_job_)
    {
        return;
    }
    const QString state = data.value("state").toString();
    /* The engine's terminal name is "succeeded"
       (JobStateName) — "completed" is the legacy alias
       DevicePresetController accepts too. */
    const bool ok_state = state == QLatin1String("succeeded") ||
                          state == QLatin1String("completed");
    if(!ok_state &&
       state != QLatin1String("failed") &&
       state != QLatin1String("cancelled"))
    {
        return;   /* still in flight */
    }
    pending_save_job_ = 0;
    saving_look_      = false;
    emit lookSaveChanged();

    QVariantMap res;
    res["id"] = pending_save_id_;
    if(!ok_state)
    {
        res["ok"]     = false;
        res["errors"] = QStringList{
            data.value("error").toString().isEmpty()
                ? QStringLiteral("install %1").arg(state)
                : data.value("error").toString() };
        emit lookSaveResult(res);
        return;
    }
    /* The store is collision-safe: the landed id may differ from
       the requested one (never an overwrite). Register the
       document locally so the preset resolves before the next
       preset-list push, then adopt it through ONE undoable
       record — undo restores the previous stack AND preset
       together (the source bridge's saveLookAs contract). */
    const QVariantMap result = data.value("result").toMap();
    const QString landed =
        result.value("landed_id").toString().isEmpty()
            ? pending_save_id_
            : result.value("landed_id").toString();

    /* Rebuild the installed document from the EFFECTIVE stack —
       the same serialization saveLookAs sent; the requested
       display name survives (an id is never the name). */
    studio::EffectDocument doc;
    doc.id    = landed.toStdString();
    doc.name  = pending_save_name_.isEmpty()
        ? doc.id : pending_save_name_.toStdString();
    const std::vector<studio::EffectLayer> stack = EffectiveLayers();
    static const char* const order[] = {
        "primitive", "space", "blend", "opacity", "speed", "scale",
        "phase", "density", "origin", "direction", "path",
        "palette", "targets", "source", "seed", "enabled",
    };
    doc.layers = nlohmann::ordered_json::array();
    for(const studio::EffectLayer& l : stack)
    {
        const nlohmann::json jl = studio::EffectLayerToJson(l);
        nlohmann::ordered_json ol;
        for(const char* k : order)
        {
            if(jl.contains(k))
            {
                ol[k] = jl[k];
            }
        }
        doc.layers.push_back(ol);
    }
    ui_looks_.Add(doc, nullptr);

    if(!gestureActive() && Editor() != nullptr)
    {
        ApplyOp(Editor()->SetLayers({}, doc.id, "save as look"));
    }
    RequestEffectsCatalog();   /* the shelf picks it up */

    res["ok"] = true;
    res["landed_id"] = landed;
    emit lookSaveResult(res);
}

/*---------------------------------------------------------*\
|| gestures                                                 ||
\*---------------------------------------------------------*/

void EffectEditorController::beginLayerGesture()
{
    if(Editor() == nullptr)
    {
        return;
    }
    if(Editor()->BeginLayerGesture())
    {
        emit gestureChanged();
    }
}

void EffectEditorController::commitLayerGesture(const QString& label)
{
    if(Editor() == nullptr)
    {
        return;
    }
    const bool was_active = Editor()->LayerGestureActive();
    std::optional<studio::EditorEdit> e =
        Editor()->CommitLayerGesture(label.toStdString());
    if(e.has_value())
    {
        Session()->CommitEdit(*e);
        ws_->NoteDraftEdited();
        RefreshModel();
        emit stackChanged();
    }
    if(was_active)
    {
        emit gestureChanged();
    }
}

void EffectEditorController::cancelLayerGesture()
{
    if(Editor() == nullptr || !Editor()->LayerGestureActive())
    {
        return;
    }
    Editor()->CancelLayerGesture();
    RefreshModel();
    emit gestureChanged();
    emit stackChanged();
}

/*---------------------------------------------------------*\
|| stack + per-layer ops                                    ||
\*---------------------------------------------------------*/

void EffectEditorController::moveLayer(int from, int to)
{
    if(!ready() || from < 0 || to < 0)
    {
        return;
    }
    ApplyOp(Editor()->MoveLayer((size_t)from, (size_t)to));
}

void EffectEditorController::addLayer(const QString& primitive)
{
    if(!ready())
    {
        return;
    }
    if(!studio::IsPrimitive(primitive.toStdString()))
    {
        emit statusMessage(QStringLiteral(
            "unknown primitive '%1'").arg(primitive));
        return;
    }
    std::optional<studio::EditorEdit> e =
        Editor()->AddLayer(primitive.toStdString());
    const bool added = e.has_value();
    ApplyOp(std::move(e));
    /* Select the appended row only when the op actually
       landed — a refused add must not move selection. */
    if(added)
    {
        selected_layer_ = (int)layer_model_.count() - 1;
        emit selectedLayerChanged();
    }
}

void EffectEditorController::removeLayer(int index)
{
    if(!ready() || index < 0)
    {
        return;
    }
    ApplyOp(Editor()->RemoveLayer((size_t)index));
}

void EffectEditorController::setLayerEnabled(int index, bool on)
{
    if(!ready() || index < 0)
    {
        return;
    }
    ApplyOp(Editor()->SetLayerEnabled((size_t)index, on));
}

void EffectEditorController::setLayerBlend(int index,
                                           const QString& mode)
{
    if(!ready() || index < 0)
    {
        return;
    }
    studio::BlendMode m = studio::BlendMode::Replace;
    if(mode == QLatin1String("add"))
    {
        m = studio::BlendMode::Add;
    }
    else if(mode == QLatin1String("screen"))
    {
        m = studio::BlendMode::Screen;
    }
    else if(mode != QLatin1String("replace"))
    {
        emit statusMessage(QStringLiteral(
            "unknown blend mode '%1'").arg(mode));
        return;
    }
    ApplyOp(Editor()->SetLayerBlend((size_t)index, m));
}

void EffectEditorController::setLayerOpacity(int index, double v)
{
    if(!ready() || index < 0)
    {
        return;
    }
    ApplyOp(Editor()->SetLayerOpacity((size_t)index, (float)v));
}

void EffectEditorController::setLayerField(int index,
                                           const QString& field,
                                           double v)
{
    if(!ready() || index < 0)
    {
        return;
    }
    studio::EditorController::LayerField lf;
    if(field == QLatin1String("speed"))
    {
        lf = studio::EditorController::LayerField::Speed;
    }
    else if(field == QLatin1String("scale"))
    {
        lf = studio::EditorController::LayerField::Scale;
    }
    else if(field == QLatin1String("phase"))
    {
        lf = studio::EditorController::LayerField::Phase;
    }
    else if(field == QLatin1String("density"))
    {
        lf = studio::EditorController::LayerField::Density;
    }
    else if(field == QLatin1String("seed"))
    {
        lf = studio::EditorController::LayerField::Seed;
    }
    else
    {
        emit statusMessage(QStringLiteral(
            "unknown layer field '%1'").arg(field));
        return;
    }
    ApplyOp(Editor()->SetLayerField((size_t)index, lf, v));
}

void EffectEditorController::setLayerSpace(int index,
                                           const QString& space)
{
    if(!ready() || index < 0)
    {
        return;
    }
    studio::CoordSpace cs;
    if(space == QLatin1String("local"))
    {
        cs = studio::CoordSpace::Local;
    }
    else if(space == QLatin1String("world"))
    {
        cs = studio::CoordSpace::World;
    }
    else
    {
        emit statusMessage(QStringLiteral(
            "unknown coordinate space '%1'").arg(space));
        return;
    }
    ApplyOp(Editor()->SetLayerSpace((size_t)index, cs));
}

void EffectEditorController::setLayerOrigin(int index,
                                            double x, double y,
                                            double z)
{
    if(!ready() || index < 0)
    {
        return;
    }
    studio::Vec3 v{ (float)x, (float)y, (float)z };
    ApplyOp(Editor()->SetLayerOrigin((size_t)index, v));
}

void EffectEditorController::setLayerDirection(int index,
                                               double x, double y,
                                               double z)
{
    if(!ready() || index < 0)
    {
        return;
    }
    studio::Vec3 v{ (float)x, (float)y, (float)z };
    ApplyOp(Editor()->SetLayerDirection((size_t)index, v));
}

void EffectEditorController::setLayerSource(int index,
                                            const QString& src)
{
    if(!ready() || index < 0)
    {
        return;
    }
    const QString s = src.toLower();
    if(!s.isEmpty() && s != QLatin1String("audio") &&
       s != QLatin1String("key") && s != QLatin1String("screen"))
    {
        emit statusMessage(QStringLiteral(
            "unknown input source '%1'").arg(src));
        return;
    }
    ApplyOp(Editor()->SetLayerSource((size_t)index,
                                     s.toStdString()));
}

void EffectEditorController::setLayerTargets(
    int index, const QStringList& targets)
{
    if(!ready() || index < 0)
    {
        return;
    }
    std::vector<std::string> ts;
    ts.reserve((size_t)targets.size());
    for(const QString& t : targets)
    {
        const std::string tt = t.trimmed().toStdString();
        if(!tt.empty())
        {
            ts.push_back(tt);
        }
    }
    ApplyOp(Editor()->SetLayerTargets((size_t)index, ts));
}

/*---------------------------------------------------------*\
|| palette-stop ops                                         ||
\*---------------------------------------------------------*/

void EffectEditorController::addLayerStop(int index, double pos,
                                          const QColor& color)
{
    if(!ready() || index < 0)
    {
        return;
    }
    if(!color.isValid())
    {
        emit statusMessage(QStringLiteral(
            "stop color must be a valid color"));
        return;
    }
    ApplyOp(Editor()->AddLayerStop((size_t)index, (float)pos,
                                   ColorOf(color)));
}

void EffectEditorController::removeLayerStop(int index, int stop)
{
    if(!ready() || index < 0 || stop < 0)
    {
        return;
    }
    ApplyOp(Editor()->RemoveLayerStop((size_t)index,
                                      (size_t)stop));
}

int EffectEditorController::moveLayerStop(int index, int stop,
                                          double pos)
{
    if(!ready() || index < 0 || stop < 0)
    {
        return -1;
    }
    ApplyOp(Editor()->MoveLayerStop((size_t)index,
                                    (size_t)stop, (float)pos));
    /* The op re-sorted the palette — hand the dragged stop's NEW
       index back so a scrub keeps hold of the same stop across
       the sort (positions are unique, so the pos we just wrote —
       or a refused move — identifies it exactly). A refused write
       leaves no stop at `pos` → the incoming index stands. */
    const studio::StudioDocument* d = WsDoc();
    if(d != nullptr && index < (int)d->effect.layers.size())
    {
        const std::vector<studio::PaletteStop>& stops =
            d->effect.layers[index].palette.stops;
        for(size_t k = 0; k < stops.size(); k++)
        {
            if(stops[k].pos == (float)pos)
            {
                return (int)k;
            }
        }
    }
    return stop;
}

void EffectEditorController::setLayerStopColor(int index, int stop,
                                               const QColor& color)
{
    if(!ready() || index < 0 || stop < 0)
    {
        return;
    }
    if(!color.isValid())
    {
        emit statusMessage(QStringLiteral(
            "stop color must be a valid color"));
        return;
    }
    ApplyOp(Editor()->SetLayerStopColor((size_t)index,
                                        (size_t)stop,
                                        ColorOf(color)));
}

/*---------------------------------------------------------*\
|| directional flow (Wave card)                             ||
\*---------------------------------------------------------*/

QVariantMap EffectEditorController::flowState() const
{
    QVariantMap m;
    const std::vector<studio::EffectLayer> layers = EffectiveLayers();
    int first = -1;
    int kbd   = -1;
    for(int i = 0; i < (int)layers.size(); i++)
    {
        const studio::EffectLayer& l = layers[i];
        if(first < 0 && l.enabled && l.primitive == "wave")
        {
            first = i;
        }
        if(kbd < 0 && l.primitive == "static"
           && std::find(l.targets.begin(), l.targets.end(),
                        std::string("keyboard_body"))
              != l.targets.end())
        {
            kbd = i;
        }
    }
    m["available"]     = ready() && first >= 0;
    m["keyboardLayer"] = kbd;
    m["keyboardOn"]    = kbd >= 0 && layers[kbd].enabled;
    if(first < 0)
    {
        return m;
    }
    const studio::EffectLayer& w = layers[first];
    m["direction"]  = VecMap(w.direction);
    m["speed"]      = w.speed;
    m["wavelength"] = w.scale;
    QStringList colors;
    for(const studio::PaletteStop& s : w.palette.stops)
    {
        colors.push_back(QString::fromStdString(
            studio::SceneColorHex(studio::ToSceneColor(s.color))));
    }
    m["colors"] = colors;
    return m;
}

bool EffectEditorController::ApplyFlow(
    const studio::EditorController::FlowEdit& f)
{
    if(!ready() || gestureActive())
    {
        return false;
    }
    auto e = Editor()->SetFlow(f);
    const bool changed = e.has_value();
    ApplyOp(std::move(e));
    return changed;
}

bool EffectEditorController::setFlowDirection(double x, double y,
                                              double z)
{
    studio::EditorController::FlowEdit f;
    f.direction = studio::Vec3{ (float)x, (float)y, (float)z };
    return ApplyFlow(f);
}

bool EffectEditorController::setFlowColors(const QStringList& colors)
{
    studio::EditorController::FlowEdit f;
    for(const QString& s : colors)
    {
        const QColor c(s);
        if(!c.isValid())
        {
            emit statusMessage(QStringLiteral(
                "wave color must be a valid color"));
            return false;
        }
        f.colors.push_back(ColorOf(c));
    }
    if(f.colors.size() < 2 || f.colors.size() > 5)
    {
        emit statusMessage(QStringLiteral("wave needs 2 to 5 colors"));
        return false;
    }
    return ApplyFlow(f);
}

bool EffectEditorController::setFlowSpeed(double metersPerSecond)
{
    studio::EditorController::FlowEdit f;
    f.speed = (float)metersPerSecond;
    return ApplyFlow(f);
}

bool EffectEditorController::setFlowWavelength(double meters)
{
    studio::EditorController::FlowEdit f;
    f.wavelength = (float)meters;
    return ApplyFlow(f);
}

/*---------------------------------------------------------*\
|| path-point ops                                           ||
\*---------------------------------------------------------*/

void EffectEditorController::addLayerPathPoint(int index,
                                               double x, double y,
                                               double z)
{
    if(!ready() || index < 0)
    {
        return;
    }
    studio::Vec3 p{ (float)x, (float)y, (float)z };
    ApplyOp(Editor()->AddLayerPathPoint((size_t)index, p));
}

void EffectEditorController::setLayerPathPoint(int index, int pt,
                                               double x, double y,
                                               double z)
{
    if(!ready() || index < 0 || pt < 0)
    {
        return;
    }
    studio::Vec3 p{ (float)x, (float)y, (float)z };
    ApplyOp(Editor()->SetLayerPathPoint((size_t)index,
                                        (size_t)pt, p));
}

void EffectEditorController::removeLayerPathPoint(int index, int pt)
{
    if(!ready() || index < 0 || pt < 0)
    {
        return;
    }
    ApplyOp(Editor()->RemoveLayerPathPoint((size_t)index,
                                           (size_t)pt));
}

/*---------------------------------------------------------*\
|| persisted input settings                                 ||
\*---------------------------------------------------------*/

void EffectEditorController::setDecayPct(int pct)
{
    const int clamped = std::max(50, std::min(300, pct));
    CommitInputs("set ripple decay",
        [clamped](studio::StudioDocument& w) {
            if(w.inputs.decay_pct == clamped)
            {
                return false;
            }
            w.inputs.decay_pct = clamped;
            return true;
        });
}

void EffectEditorController::setInputEnabled(const QString& input,
                                             bool on)
{
    const QString s = input.toLower();
    CommitInputs("set input " + s.toStdString(),
        [s, on](studio::StudioDocument& w) {
            bool* f = nullptr;
            if(s == QLatin1String("audio"))
            {
                f = &w.inputs.audio;
            }
            else if(s == QLatin1String("key") ||
                    s == QLatin1String("keys"))
            {
                f = &w.inputs.keys;
            }
            else if(s == QLatin1String("screen"))
            {
                f = &w.inputs.screen;
            }
            if(f == nullptr || *f == on)
            {
                return false;
            }
            *f = on;
            return true;
        });
}

void EffectEditorController::setInputSensitivity(int pct)
{
    const int clamped = std::max(25, std::min(200, pct));
    CommitInputs("set audio sensitivity",
        [clamped](studio::StudioDocument& w) {
            if(w.inputs.sens_pct == clamped)
            {
                return false;
            }
            w.inputs.sens_pct = clamped;
            return true;
        });
}

void EffectEditorController::setInputScreenIndex(int index)
{
    if(index < 0)
    {
        return;
    }
    CommitInputs("set screen input display",
        [index](studio::StudioDocument& w) {
            if(w.inputs.screen_index == index)
            {
                return false;
            }
            w.inputs.screen_index = index;
            return true;
        });
}

/*---------------------------------------------------------*\
|| undo + selection                                         ||
\*---------------------------------------------------------*/

void EffectEditorController::undo()
{
    /* Route through the workspace controller so its draftEdited
       signal drives a shared refresh (OnWorkspaceDraftEdited). */
    if(ws_ != nullptr)
    {
        ws_->undo();
    }
}

void EffectEditorController::redo()
{
    if(ws_ != nullptr)
    {
        ws_->redo();
    }
}

void EffectEditorController::setSelectedLayer(int index)
{
    const int clamped =
        std::max(-1, std::min(index, layerCount() - 1));
    if(clamped == selected_layer_)
    {
        return;
    }
    selected_layer_ = clamped;
    emit selectedLayerChanged();
}

void EffectEditorController::RegisterLookForTesting(
    const studio::EffectDocument& d)
{
    ui_looks_.Add(d, nullptr);
    RefreshModel();
}

} /* namespace ui */
} /* namespace openrgb2 */
