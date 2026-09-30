/*---------------------------------------------------------*\
||| DevicePresetController.cpp                               ||
|||                                                          ||
|||   SPDX-License-Identifier: GPL-2.0-or-later               ||
\*---------------------------------------------------------*/

#include "DevicePresetController.h"

#include "../JsonVariant.h"

#include "presets/PresetBundle.h"      /* CollectUsedTypes    */
#include "scene/SceneGraph.h"          /* TopologicalOrder    */
#include "scene/SceneJson.h"           /* scene json helpers  */
#include "scene/SceneResolver.h"       /* ResolveScene        */
#include "scene/SceneTypes.h"          /* ResolvedBodySize    */

#include <optional>

namespace openrgb2
{
namespace ui
{
namespace
{

QVariantList ErrList(const std::vector<std::string>& errs)
{
    QVariantList out;
    for(const std::string& e : errs)
    {
        out.push_back(QString::fromStdString(e));
    }
    return out;
}

const char* KindName(studio::ObjectKind kind)
{
    switch(kind)
    {
    case studio::ObjectKind::Device: return "device";
    case studio::ObjectKind::Linked: return "linked";
    case studio::ObjectKind::Group:  return "group";
    default:                         return "decor";
    }
}

} /* anonymous namespace */

DevicePresetController::DevicePresetController(
    EngineBridge* bridge, WorkspaceController* ws,
    ObjectEditorController* objects, QObject* parent)
    : QObject(parent)
    , bridge_(bridge)
    , ws_(ws)
    , objects_(objects)
{
    if(ws_ != nullptr)
    {
        connect(ws_, &WorkspaceController::draftChanged,
                this, &DevicePresetController::readyChanged);
        connect(ws_, &WorkspaceController::availabilityChanged,
                this, &DevicePresetController::readyChanged);
    }
    if(bridge_ != nullptr)
    {
        connect(bridge_, &EngineBridge::replyReceived,
                this, &DevicePresetController::OnReplyReceived);
        connect(bridge_, &EngineBridge::eventReceived,
                this, &DevicePresetController::OnEventReceived);
        connect(bridge_, &EngineBridge::connectionStateChanged,
                this, [this] {
                    emit readyChanged();
                    if(!bridge_->connected())
                    {
                        /* A pending request/job can never resolve
                           after disconnect — unlatch so the
                           editor doesn't wedge, and fail the
                           outcome honestly. */
                        if(pending_install_req_ != 0 ||
                           pending_install_job_ != 0)
                        {
                            pending_install_req_ = 0;
                            pending_install_job_ = 0;
                            QVariantMap r;
                            r["ok"]     = false;
                            r["id"]     = QString::fromStdString(
                                pending_preset_.id);
                            r["errors"] = QStringList{
                                QStringLiteral(
                                    "engine disconnected") };
                            emit presetSaveResult(r);
                        }
                        if(pending_delete_req_ != 0 ||
                           pending_delete_job_ != 0)
                        {
                            pending_delete_req_ = 0;
                            pending_delete_job_ = 0;
                            QVariantMap r;
                            r["ok"]    = false;
                            r["id"]    = pending_delete_id_;
                            r["error"] = QStringLiteral(
                                "engine disconnected");
                            emit presetDeleteResult(r);
                        }
                        pending_get_req_ = 0;
                        emit busyChanged();
                    }
                });
    }
}

/*---------------------------------------------------------*\
||| state + helpers                                          ||
\*---------------------------------------------------------*/

bool DevicePresetController::ready() const
{
    return ws_ != nullptr && ws_->Session().HasWorking();
}

const studio::StudioDocument* DevicePresetController::WsDoc() const
{
    return (ws_ != nullptr && ws_->Session().HasWorking())
        ? &ws_->Session().Working() : nullptr;
}

studio::PresetRegistry DevicePresetController::EffectiveRegistry() const
{
    /* The object controller's UI-side library + types this
       session installed (they resolve before the next catalog
       pull). The copy is cheap at library scale. */
    studio::PresetRegistry reg =
        (objects_ != nullptr) ? objects_->Presets()
                              : studio::PresetRegistry();
    for(const std::string& id : session_types_.Ids())
    {
        if(const studio::DevicePreset* p = session_types_.Find(id))
        {
            reg.Add(*p, nullptr);
        }
    }
    return reg;
}

void DevicePresetController::SetStatus(const QString& s)
{
    if(s == status_text_)
    {
        return;
    }
    status_text_ = s;
    emit statusChanged();
}

/*---------------------------------------------------------*\
||| reads                                                    ||
\*---------------------------------------------------------*/

QVariantMap DevicePresetController::presetDocument(
    const QString& typeId) const
{
    QVariantMap out;
    const std::string tid = typeId.toStdString();
    const studio::DevicePreset* p = session_types_.Find(tid);
    bool session_owned = p != nullptr;
    if(p == nullptr && objects_ != nullptr)
    {
        p = objects_->Presets().Find(tid);
    }
    out["exists"] = (p != nullptr);
    if(p == nullptr)
    {
        return out;
    }
    bool from_file = session_owned;
    if(!from_file && objects_ != nullptr)
    {
        for(const studio::PresetRegistry::PresetInfo& info :
                objects_->Presets().List())
        {
            if(info.id == tid)
            {
                from_file = info.from_file;
                break;
            }
        }
    }
    out["fromFile"] = from_file;
    /* Resolved without a file => the packaged-defaults layer
       supplied it — a save creates a user override shadowing
       it (the source bridge's contract). */
    out["packaged"] = !from_file;
    out["doc"]      = JsonToVariant(studio::ToJson(*p));
    int instances = 0;
    if(const studio::StudioDocument* w = WsDoc())
    {
        for(const auto& kv : w->devices)
        {
            if(kv.second.type == tid)
            {
                instances++;
            }
        }
    }
    out["instances"] = instances;
    return out;
}

void DevicePresetController::fetchPresetDocument(const QString& typeId)
{
    if(bridge_ == nullptr || !bridge_->connected())
    {
        QVariantMap out;
        out["ok"]    = false;
        out["id"]    = typeId;
        out["error"] = QStringLiteral("engine not connected");
        emit presetDocumentReady(out);
        return;
    }
    pending_get_id_  = typeId;
    pending_get_req_ = bridge_->workspaceCommand(
        QStringLiteral("resource_get"),
        QVariantMap({
            { QStringLiteral("kind"), QStringLiteral("device_type") },
            { QStringLiteral("id"),   typeId },
        }));
    if(pending_get_req_ <= 0)
    {
        pending_get_req_ = 0;
        QVariantMap out;
        out["ok"]    = false;
        out["id"]    = typeId;
        out["error"] = QStringLiteral("resource_get could not be sent");
        emit presetDocumentReady(out);
    }
}

int DevicePresetController::instanceCount(const QString& typeId) const
{
    int n = 0;
    const std::string tid = typeId.toStdString();
    if(const studio::StudioDocument* w = WsDoc())
    {
        for(const auto& kv : w->devices)
        {
            n += kv.second.type == tid;
        }
    }
    return n;
}

/*---------------------------------------------------------*\
||| candidate validation + preview                           ||
\*---------------------------------------------------------*/

QVariantMap DevicePresetController::validatePreset(
    const QVariantMap& candidate) const
{
    QVariantMap out;
    const nlohmann::json j = VariantToJson(candidate);
    studio::DevicePreset p;
    std::vector<std::string> errs;
    if(j.is_discarded() ||
       !studio::DevicePresetFromJson(j, p, &errs))
    {
        if(errs.empty())
        {
            errs.push_back("candidate is not a preset document");
        }
        out["ok"]     = false;
        out["errors"] = ErrList(errs);
        return out;
    }
    /* Dependency rules are file-layer rules — the candidate is
       checked against a throwaway copy so the live library is
       untouched. */
    studio::PresetRegistry tmp = EffectiveRegistry();
    if(!tmp.Add(p, &errs))
    {
        out["ok"]     = false;
        out["errors"] = ErrList(errs);
        return out;
    }
    out["ok"]     = true;
    out["errors"] = ErrList(errs);
    return out;
}

QVariantMap DevicePresetController::previewPreset(
    const QVariantMap& candidate) const
{
    QVariantMap out;
    const QVariantMap v = validatePreset(candidate);
    if(!v["ok"].toBool())
    {
        out["ok"]     = false;
        out["errors"] = v["errors"];
        return out;
    }
    const nlohmann::json j = VariantToJson(candidate);
    studio::DevicePreset p;
    std::vector<std::string> errs;
    studio::DevicePresetFromJson(j, p, &errs); /* known-good */

    /* Throwaway scene: private registry copy + one root
       instance at the origin. No doc/workspace/binding/
       hardware contact — the source bridge's contract. */
    studio::PresetRegistry tmp = EffectiveRegistry();
    if(!tmp.Add(p, &errs))
    {
        out["ok"]     = false;
        out["errors"] = ErrList(errs);
        return out;
    }
    studio::StudioDocument w;
    w.meta.name = "preview";
    studio::DeviceInstance inst;
    inst.type = p.id;
    w.devices["__preview__"] = inst;
    studio::SceneDocument resolved;
    if(!studio::ResolveScene(w, tmp, resolved, &errs))
    {
        out["ok"]     = false;
        out["errors"] = ErrList(errs);
        return out;
    }

    /* Same row shape the object model emits — minus binding
       state — plus emitter positions inline. Parents precede
       children. */
    QVariantList objs;
    for(const studio::SceneObject* po :
            studio::TopologicalOrder(resolved))
    {
        const studio::SceneObject& o = *po;
        QVariantMap m;
        m["id"]       = QString::fromStdString(o.id);
        m["label"]    = QString::fromStdString(o.label);
        m["kind"]     = QString::fromLatin1(KindName(o.kind));
        m["geometry"] = QString::fromStdString(o.geometry);
        m["parentId"] = QString::fromStdString(o.parent_id);
        m["x"] = o.transform.position.x;
        m["y"] = o.transform.position.y;
        m["z"] = o.transform.position.z;
        const studio::Quat q =
            studio::RotationQuat(o.transform.rotation_deg);
        m["qw"] = q.w; m["qx"] = q.x; m["qy"] = q.y; m["qz"] = q.z;
        m["sx"] = o.transform.scale.x;
        m["sy"] = o.transform.scale.y;
        m["sz"] = o.transform.scale.z;
        m["dx"] = o.size_m.x;
        m["dy"] = o.size_m.y;
        m["dz"] = o.size_m.z;
        const studio::Vec3 body = studio::ResolvedBodySize(o);
        m["bx"] = body.x;
        m["by"] = body.y;
        m["bz"] = body.z;
        m["visible"] = o.visible;
        m["emitterCount"] = (int)o.emitters.size();
        QVariantList ems;
        for(size_t i = 0; i < o.emitters.size(); i++)
        {
            QVariantMap em;
            em["x"] = o.emitters[i].local_pos.x;
            em["y"] = o.emitters[i].local_pos.y;
            em["z"] = o.emitters[i].local_pos.z;
            em["i"] = (int)i;
            em["a"] = o.emitters[i].address;
            ems.push_back(em);
        }
        m["emitters"] = ems;
        objs.push_back(m);
    }
    out["ok"]      = true;
    out["objects"] = objs;
    out["errors"]  = QVariantList();
    return out;
}

/*---------------------------------------------------------*\
||| mutations — engine jobs                                  ||
\*---------------------------------------------------------*/

QString DevicePresetController::SendInstall(
    const studio::DevicePreset& p, bool replace,
    const QString& post_op)
{
    if(bridge_ == nullptr || !bridge_->connected() ||
       ws_ == nullptr)
    {
        return QStringLiteral(
            "engine not connected — types install through "
            "the workspace job surface");
    }
    if(pending_install_req_ != 0 || pending_install_job_ != 0)
    {
        return QStringLiteral(
            "a type install is already in flight");
    }
    QVariantMap args;
    args["kind"]        = QStringLiteral("device_type");
    args["id"]          = ws_->workspaceId();
    args["device_type"] = JsonToVariant(studio::ToJson(p));
    args["replace"]     = replace;
    args["base_revision"]     = ws_->workspaceRevision();
    args["resource_revision"] = ws_->resourceRevision();
    const int req = bridge_->workspaceCommand(
        QStringLiteral("resource_install"), args);
    if(req <= 0)
    {
        return QStringLiteral("resource_install could not be sent");
    }
    pending_install_req_  = req;
    pending_install_job_  = 0;
    pending_install_post_ = post_op;
    pending_preset_       = p;
    emit busyChanged();
    return QString();
}

QVariantMap DevicePresetController::savePresetType(
    const QVariantMap& candidate, bool overwrite)
{
    QVariantMap out;
    const nlohmann::json j = VariantToJson(candidate);
    studio::DevicePreset p;
    std::vector<std::string> errs;
    if(j.is_discarded() ||
       !studio::DevicePresetFromJson(j, p, &errs))
    {
        if(errs.empty())
        {
            errs.push_back("candidate is not a preset document");
        }
        out["ok"]     = false;
        out["errors"] = ErrList(errs);
        return out;
    }
    /* Same dependency check a file load would enforce — a
       private copy so a failure leaves the live registry
       alone. */
    {
        studio::PresetRegistry tmp = EffectiveRegistry();
        if(!tmp.Add(p, &errs))
        {
            out["ok"]     = false;
            out["errors"] = ErrList(errs);
            return out;
        }
    }
    const QString err = SendInstall(p, overwrite,
                                    QStringLiteral("save"));
    if(!err.isEmpty())
    {
        out["ok"]     = false;
        out["errors"] = QStringList{ err };
        return out;
    }
    out["ok"]      = true;
    out["pending"] = true;
    out["id"]      = QString::fromStdString(p.id);
    return out;
}

QVariantMap DevicePresetController::deletePresetType(
    const QString& typeId)
{
    QVariantMap out;
    const std::string tid = typeId.toStdString();
    const studio::PresetRegistry reg = EffectiveRegistry();
    if(!studio::IsPresetId(tid) || !reg.Contains(tid))
    {
        out["ok"]    = false;
        out["error"] = QStringLiteral("no such type '%1'").arg(typeId);
        return out;
    }
    /* Only the file layer is deletable — a packaged default
       has nothing on disk to remove (the engine re-checks; a
       session-installed id counts as file-layer). */
    const bool file_layer = session_types_.HasFile(tid) ||
                            reg.HasFile(tid);
    if(!file_layer)
    {
        out["ok"]    = false;
        out["error"] = QStringLiteral(
            "'%1' is a packaged default — nothing to delete")
                .arg(typeId);
        return out;
    }
    /* In-use guard on the DRAFT — desk instances resolve
       through this type; the engine repeats the check across
       every managed workspace's ACTIVE document. */
    if(const studio::StudioDocument* w = WsDoc())
    {
        int instances = 0;
        for(const auto& kv : w->devices)
        {
            instances += kv.second.type == tid;
        }
        if(instances > 0)
        {
            out["ok"]    = false;
            out["error"] = QStringLiteral(
                "'%1' is in use by %2 instance(s)")
                    .arg(typeId).arg(instances);
            return out;
        }
        for(const std::string& f : w->meta.ui.favorites)
        {
            if(f == tid)
            {
                out["ok"]    = false;
                out["error"] = QStringLiteral(
                    "'%1' is a favorite — unfavorite first")
                        .arg(typeId);
                return out;
            }
        }
    }
    /* Child-reference guard — another type's entity `type` ref
       would strand on the delete. */
    for(const std::string& oid : reg.Ids())
    {
        if(oid == tid)
        {
            continue;
        }
        const studio::DevicePreset* op = reg.Find(oid);
        if(op == nullptr)
        {
            continue;
        }
        for(const std::string& dep : studio::PresetDependencies(*op))
        {
            if(dep == tid)
            {
                out["ok"]    = false;
                out["error"] = QStringLiteral(
                    "'%1' is referenced by type '%2'")
                        .arg(typeId, QString::fromStdString(oid));
                return out;
            }
        }
    }
    if(bridge_ == nullptr || !bridge_->connected() ||
       ws_ == nullptr)
    {
        out["ok"]    = false;
        out["error"] = QStringLiteral(
            "engine not connected — deletes run through the "
            "workspace job surface");
        return out;
    }
    if(pending_delete_req_ != 0 || pending_delete_job_ != 0)
    {
        out["ok"]    = false;
        out["error"] = QStringLiteral(
            "a type delete is already in flight");
        return out;
    }
    const int req = bridge_->workspaceCommand(
        QStringLiteral("resource_remove"),
        QVariantMap({
            { QStringLiteral("kind"),        QStringLiteral("device_type") },
            { QStringLiteral("resource_id"), typeId },
            { QStringLiteral("id"),          ws_->workspaceId() },
            { QStringLiteral("base_revision"),     ws_->workspaceRevision() },
            { QStringLiteral("resource_revision"), ws_->resourceRevision() },
        }));
    if(req <= 0)
    {
        out["ok"]    = false;
        out["error"] = QStringLiteral("resource_remove could not be sent");
        return out;
    }
    pending_delete_req_ = req;
    pending_delete_job_ = 0;
    pending_delete_id_  = typeId;
    emit busyChanged();
    out["ok"]      = true;
    out["pending"] = true;
    return out;
}

void DevicePresetController::saveInstanceAsVariant(
    const QString& instanceId, const QString& newTypeId,
    const QString& displayName)
{
    const std::string iid =
        studio::EditorController::InstanceOf(instanceId.toStdString());
    const std::string tid = newTypeId.toStdString();
    const std::string nm  = displayName.trimmed().toStdString();
    auto refuse = [this](const QString& m)
    {
        SetStatus(m);
        emit statusMessage(m);
    };
    const studio::StudioDocument* w = WsDoc();
    if(w == nullptr)
    {
        refuse(QStringLiteral("variant refused: no draft"));
        return;
    }
    const auto dit = w->devices.find(iid);
    if(dit == w->devices.end())
    {
        refuse(QStringLiteral(
            "variant refused: '%1' is not an instance").arg(instanceId));
        return;
    }
    const studio::PresetRegistry reg = EffectiveRegistry();
    const studio::DevicePreset* src = reg.Find(dit->second.type);
    if(src == nullptr)
    {
        refuse(QStringLiteral(
            "variant refused: '%1' has unresolvable type '%2'")
                .arg(instanceId,
                     QString::fromStdString(dit->second.type)));
        return;
    }
    if(!studio::IsPresetId(tid))
    {
        refuse(QStringLiteral(
            "variant refused: '%1' is not a valid type id")
                .arg(newTypeId));
        return;
    }
    if(reg.Contains(tid))
    {
        refuse(QStringLiteral(
            "variant refused: type '%1' already exists").arg(newTypeId));
        return;
    }
    if(nm.empty())
    {
        refuse(QStringLiteral("variant refused: name required"));
        return;
    }
    /* A repoint must survive resolution: a mirrored instance
       (or a mirror target) is type-locked to its partner, so
       the edit would roll back AFTER the file landed — refuse
       before any write (the source bridge's guard). */
    /* Mirror state rides device_settings (instance-level), not
       the placement record. */
    auto sit = w->device_settings.find(iid);
    if(sit != w->device_settings.end() && !sit->second.mirror_of.empty())
    {
        refuse(QStringLiteral(
            "variant refused: '%1' mirrors %2 — repoint would "
            "break the link")
                .arg(instanceId,
                     QString::fromStdString(sit->second.mirror_of)));
        return;
    }
    for(const auto& kv : w->device_settings)
    {
        if(kv.second.mirror_of == iid)
        {
            refuse(QStringLiteral(
                "variant refused: '%1' is mirrored by %2 — "
                "repoint would break the link")
                    .arg(instanceId, QString::fromStdString(kv.first)));
            return;
        }
    }
    /* The resolved type definition is the variant's seed;
       painted body colors bake onto the variant's entity
       appearance (emitter paint stays workspace data). */
    studio::DevicePreset v = *src;
    v.id   = tid;
    v.name = nm;
    ws_->Session().Editor().BakePaintedColors(iid, v);
    const QString err = SendInstall(
        v, /*replace=*/false,
        QStringLiteral("retype"));
    if(!err.isEmpty())
    {
        refuse(QStringLiteral("variant write failed: %1").arg(err));
        return;
    }
    pending_install_iid_ = QString::fromStdString(iid);
}

void DevicePresetController::createTypeFromSelection(
    const QStringList& instanceIds, const QString& newTypeId,
    const QString& displayName)
{
    const std::string tid = newTypeId.toStdString();
    const std::string nm  = displayName.trimmed().toStdString();
    auto refuse = [this](const QString& m)
    {
        SetStatus(m);
        emit statusMessage(m);
    };
    if(instanceIds.isEmpty())
    {
        refuse(QStringLiteral(
            "create preset refused: nothing selected"));
        return;
    }
    if(!studio::IsPresetId(tid))
    {
        refuse(QStringLiteral(
            "create preset refused: '%1' is not a valid type id")
                .arg(newTypeId));
        return;
    }
    const studio::PresetRegistry reg = EffectiveRegistry();
    if(reg.Contains(tid))
    {
        refuse(QStringLiteral(
            "create preset refused: type '%1' already exists")
                .arg(newTypeId));
        return;
    }
    if(nm.empty())
    {
        refuse(QStringLiteral(
            "create preset refused: name required"));
        return;
    }
    std::vector<std::string> ids;
    ids.reserve((size_t)instanceIds.size());
    for(const QString& q : instanceIds)
    {
        ids.push_back(q.toStdString());
    }
    /* Child-reference type — one `type`-ref entity per listed
       root instance; the placed instances stay put (the preset
       is a new library entry, not a desk rewrite). */
    studio::DevicePreset p;
    if(ws_ == nullptr ||
       !ws_->Session().Editor().BuildPresetFromInstances(
           ids, tid, nm, reg, p))
    {
        refuse(QString::fromStdString(
            ws_ != nullptr
                ? ws_->Session().Editor().LastError()
                : "no draft"));
        return;
    }
    const QString err = SendInstall(p, /*replace=*/false,
                                    QStringLiteral("create"));
    if(!err.isEmpty())
    {
        refuse(QStringLiteral("create preset failed: %1").arg(err));
    }
}

/*---------------------------------------------------------*\
||| warnings + terminal resolution                           ||
\*---------------------------------------------------------*/

QVariantList DevicePresetController::InstanceWarnings(
    const studio::DevicePreset& p) const
{
    /* Bound-hardware warnings (the source's contract): for
       every draft instance of this type compare each bound
       zone's hardware count against the NEW zone's emitter
       count; a removed zone leaves a dangling binding — report
       it. Informational only — the save already landed. */
    QVariantList warns;
    const studio::StudioDocument* w = WsDoc();
    if(w == nullptr)
    {
        return warns;
    }
    for(const auto& kv : w->devices)
    {
        if(kv.second.type != p.id)
        {
            continue;
        }
        const auto sit = w->device_settings.find(kv.first);
        if(sit == w->device_settings.end())
        {
            continue;
        }
        for(const auto& zk : sit->second.zones)
        {
            if(zk.second.binding.empty())
            {
                continue;
            }
            const studio::DeviceZone* z = nullptr;
            for(const studio::DeviceZone& zz : p.zones)
            {
                if(zz.id == zk.first)
                {
                    z = &zz;
                    break;
                }
            }
            const auto bit = w->bindings.find(zk.second.binding);
            const unsigned int hw = (bit == w->bindings.end())
                                  ? 0 : bit->second.zone_leds;
            if(z == nullptr)
            {
                warns.push_back(QStringLiteral(
                    "%1: zone '%2' no longer exists on the type — "
                    "its binding is dangling; rebind or remove it")
                        .arg(QString::fromStdString(kv.first),
                             QString::fromStdString(zk.first)));
                continue;
            }
            if(hw == 0)
            {
                continue;   /* count unchecked — nothing to compare */
            }
            /* Dynamic matrix: the bound hardware owns the
               mapping — a count comparison would warn "now has
               0 LEDs" on every save. */
            if(z->layout.type == "matrix" && z->layout.dynamic)
            {
                continue;
            }
            const size_t n =
                studio::GenerateZoneEmitters(*z, "", 0).size();
            if(n < hw)
            {
                warns.push_back(QStringLiteral(
                    "%1: zone %2 now has %3 LEDs; bound hardware "
                    "expects %4 — rebind in Inspector")
                        .arg(QString::fromStdString(kv.first),
                             QString::fromStdString(zk.first))
                        .arg((int)n)
                        .arg((int)hw));
            }
        }
    }
    return warns;
}

void DevicePresetController::ResolveInstall(const QVariantMap& ev,
                                            bool ok,
                                            const QString& err)
{
    QVariantMap res;
    res["id"] = QString::fromStdString(pending_preset_.id);
    if(!ok)
    {
        res["ok"]     = false;
        res["errors"] = QStringList{
            err.isEmpty() ? QStringLiteral("install failed") : err };
        emit presetSaveResult(res);
        return;
    }
    const QVariantMap result =
        ev.value(QStringLiteral("result")).toMap();
    const QString landed =
        result.value(QStringLiteral("landed_id")).toString();
    const QString landed_id =
        landed.isEmpty() ? res["id"].toString() : landed;
    /* Register the landed id so the draft + library resolve it
       before the next catalog pull; the suffix-renamed copy
       keeps the requested preset's fields under the landed id. */
    studio::DevicePreset landed_preset = pending_preset_;
    landed_preset.id = landed_id.toStdString();
    session_types_.Add(landed_preset, nullptr);
    if(objects_ != nullptr)
    {
        objects_->RegisterPresetForSession(landed_preset);
    }

    /* Variant follow-up — ONE undoable Retype through the
       shared session (the source's saveInstanceAsVariant
       contract; the file already landed either way). */
    if(pending_install_post_ == QLatin1String("retype") &&
       ws_ != nullptr && !pending_install_iid_.isEmpty())
    {
        std::optional<studio::EditorEdit> e =
            ws_->Session().Editor().Retype(
                pending_install_iid_.toStdString(),
                landed_id.toStdString());
        if(e.has_value() && !e->Empty())
        {
            ws_->Session().CommitEdit(*e);
            ws_->NoteDraftEdited();
            SetStatus(QStringLiteral(
                "saved variant '%1' — %2 now uses it")
                    .arg(landed_id, pending_install_iid_));
        }
        else
        {
            SetStatus(QStringLiteral(
                "saved variant '%1' — %2 kept its type")
                    .arg(landed_id, pending_install_iid_));
        }
    }
    else
    {
        SetStatus(QStringLiteral("saved type '%1'").arg(landed_id));
    }
    if(objects_ != nullptr)
    {
        objects_->refreshLibrary();   /* catalog picks it up */
    }
    emit presetLibraryChanged();
    res["ok"]        = true;
    res["landed_id"] = landed_id;
    res["replaced"]  =
        result.value(QStringLiteral("replaced")).toBool();
    res["instances"] = instanceCount(landed_id);
    QVariantList warns = InstanceWarnings(landed_preset);
    for(const QVariant& wv :
            result.value(QStringLiteral("warnings")).toList())
    {
        warns.push_back(wv);
    }
    res["warnings"] = warns;
    res["errors"]   = QVariantList();
    emit presetSaveResult(res);
}

void DevicePresetController::ResolveDelete(const QVariantMap& ev,
                                           bool ok,
                                           const QString& err)
{
    QVariantMap res;
    res["id"] = pending_delete_id_;
    if(!ok)
    {
        res["ok"]    = false;
        res["error"] = err.isEmpty()
            ? QStringLiteral("delete failed") : err;
        emit presetDeleteResult(res);
        return;
    }
    /* Drop the local shadow so the draft stops resolving
       through a definition that no longer exists. */
    session_types_.RemoveFile(pending_delete_id_.toStdString());
    if(objects_ != nullptr)
    {
        objects_->UnregisterPresetForSession(
            pending_delete_id_.toStdString());
        objects_->refreshLibrary();
    }
    emit presetLibraryChanged();
    const QVariantMap result =
        ev.value(QStringLiteral("result")).toMap();
    res["ok"]                 = true;
    res["reverted_to_default"] =
        result.value(QStringLiteral("reverted_to_default"))
            .toBool();
    res["warnings"] =
        result.value(QStringLiteral("warnings")).toList();
    emit presetDeleteResult(res);
    SetStatus(QStringLiteral("deleted type '%1'")
                  .arg(pending_delete_id_));
}

/*---------------------------------------------------------*\
||| wire replies + job events                                ||
\*---------------------------------------------------------*/

void DevicePresetController::OnReplyReceived(
    int requestId, const QString& typeName,
    const QVariantMap& body)
{
    /* resource_get — the editor's document fetch. */
    if(pending_get_req_ != 0 && requestId == pending_get_req_)
    {
        pending_get_req_ = 0;
        QVariantMap out;
        out["id"] = pending_get_id_;
        if(typeName != QLatin1String("OK") ||
           !body.value(QStringLiteral("document")).isValid())
        {
            out["ok"]    = false;
            out["error"] = body.value(QStringLiteral("error"))
                               .toString();
            if(out["error"].toString().isEmpty())
            {
                out["error"] = QStringLiteral(
                    "no such type in the managed library");
            }
            emit presetDocumentReady(out);
            return;
        }
        out["ok"]              = true;
        out["exists"]          = true;
        out["doc"]             = body.value(QStringLiteral("document"));
        out["fromFile"]        =
            body.value(QStringLiteral("from_file")).toBool();
        out["packaged"]        =
            body.value(QStringLiteral("packaged_default")).toBool() &&
            !out["fromFile"].toBool();
        out["instances"]       = instanceCount(pending_get_id_);
        emit presetDocumentReady(out);
        return;
    }

    /* Job-accept replies — the terminal outcome rides the
       workspace job event; only the refusal lands here. */
    if(pending_install_req_ != 0 && requestId == pending_install_req_)
    {
        pending_install_req_ = 0;
        if(typeName != QLatin1String("OK") ||
           !body.value(QStringLiteral("accepted")).toBool())
        {
            const QString err =
                body.value(QStringLiteral("error")).toString();
            ResolveInstall({}, false,
                err.isEmpty()
                    ? (body.value(QStringLiteral("detail"))
                              .toString() == QLatin1String("busy")
                          ? QStringLiteral(
                              "engine job queue is busy — try again")
                          : QStringLiteral("type install refused"))
                    : err);
            emit busyChanged();
            return;
        }
        pending_install_job_ =
            body.value(QStringLiteral("job_id")).toULongLong();
        if(pending_install_job_ == 0)
        {
            ResolveInstall({}, false, QStringLiteral(
                "engine accepted no job for the install"));
            emit busyChanged();
        }
        return;
    }
    if(pending_delete_req_ != 0 && requestId == pending_delete_req_)
    {
        pending_delete_req_ = 0;
        if(typeName != QLatin1String("OK") ||
           !body.value(QStringLiteral("accepted")).toBool())
        {
            QString err =
                body.value(QStringLiteral("error")).toString();
            if(err.isEmpty())
            {
                err = body.value(QStringLiteral("detail"))
                          .toString() == QLatin1String("busy")
                    ? QStringLiteral(
                        "engine job queue is busy — try again")
                    : QStringLiteral("type delete refused");
            }
            ResolveDelete({}, false, err);
            emit busyChanged();
            return;
        }
        pending_delete_job_ =
            body.value(QStringLiteral("job_id")).toULongLong();
        if(pending_delete_job_ == 0)
        {
            ResolveDelete({}, false, QStringLiteral(
                "engine accepted no job for the delete"));
            emit busyChanged();
        }
        return;
    }
}

void DevicePresetController::OnEventReceived(
    const QString& kind, const QVariantMap& data)
{
    if(kind != QLatin1String("workspace") ||
       !data.contains(QStringLiteral("job_id")))
    {
        return;
    }
    const quint64 job =
        data.value(QStringLiteral("job_id")).toULongLong();
    const QString state =
        data.value(QStringLiteral("state")).toString();
    const bool terminal =
        state == QLatin1String("completed") ||
        state == QLatin1String("failed")    ||
        state == QLatin1String("cancelled") ||
        state == QLatin1String("succeeded");
    if(!terminal)
    {
        return;
    }
    const QString err =
        data.value(QStringLiteral("error")).toString();
    const bool ok = state == QLatin1String("completed") ||
                    state == QLatin1String("succeeded");
    if(job != 0 && job == pending_install_job_)
    {
        pending_install_job_ = 0;
        ResolveInstall(data, ok, err);
        pending_install_post_ = QString();
        emit busyChanged();
        return;
    }
    if(job != 0 && job == pending_delete_job_)
    {
        pending_delete_job_ = 0;
        ResolveDelete(data, ok, err);
        emit busyChanged();
        return;
    }
}

/*---------------------------------------------------------*\
||| test hooks                                               ||
\*---------------------------------------------------------*/

void DevicePresetController::FeedResourceGetForTesting(
    const QVariantMap& body)
{
    /* Same handling as the wire reply — the request id match
       is bypassed (offline tests have no correlator). */
    QVariantMap out;
    out["id"] = pending_get_id_.isEmpty()
        ? body.value(QStringLiteral("id")).toString()
        : pending_get_id_;
    if(!body.value(QStringLiteral("document")).isValid())
    {
        out["ok"]    = false;
        out["error"] = body.value(QStringLiteral("error"))
            .toString();
        emit presetDocumentReady(out);
        return;
    }
    out["ok"]       = true;
    out["exists"]   = true;
    out["doc"]      = body.value(QStringLiteral("document"));
    out["fromFile"] =
        body.value(QStringLiteral("from_file")).toBool();
    out["packaged"] =
        body.value(QStringLiteral("packaged_default")).toBool() &&
        !out["fromFile"].toBool();
    out["instances"] = instanceCount(out["id"].toString());
    emit presetDocumentReady(out);
}

void DevicePresetController::FeedJobEventForTesting(
    const QVariantMap& body)
{
    OnEventReceived(QStringLiteral("workspace"), body);
}

} /* namespace ui */
} /* namespace openrgb2 */
