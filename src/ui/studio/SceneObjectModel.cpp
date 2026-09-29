/*---------------------------------------------------------*\
|| SceneObjectModel.cpp                                     ||
||                                                          ||
||   SPDX-License-Identifier: GPL-2.0-or-later               ||
|\*---------------------------------------------------------*/

#include "SceneObjectModel.h"

#include "editor/TransformCommands.h"   /* EulerDegFromMat4      */
#include "scene/SceneGraph.h"           /* matrices, topo order  */
#include "scene/SceneJson.h"            /* SceneColorHex         */

#include <algorithm>
#include <functional>
#include <set>

namespace openrgb2
{
namespace ui
{

namespace
{

QString KindName(studio::ObjectKind kind)
{
    switch(kind)
    {
    case studio::ObjectKind::Device: return QStringLiteral("device");
    case studio::ObjectKind::Linked: return QStringLiteral("linked");
    case studio::ObjectKind::Group:  return QStringLiteral("group");
    default:                         return QStringLiteral("decor");
    }
}

/* DeviceInstance -> Transform (unit scale). */
studio::Transform InstanceTransform(const studio::DeviceInstance& d)
{
    studio::Transform t;
    t.position     = d.position;
    t.rotation_deg = d.rotation_deg;
    t.scale        = { 1.0f, 1.0f, 1.0f };
    return t;
}

} /* namespace */

SceneObjectModel::SceneObjectModel(QObject* parent)
    : QAbstractListModel(parent)
{
}

void SceneObjectModel::Bind(const studio::StudioDocument* workspace,
                            const studio::SceneDocument*  resolved,
                            BoundFn bound_fn)
{
    ws_       = workspace;
    resolved_ = resolved;
    bound_fn_ = std::move(bound_fn);
}

const SceneObjectModel::Row* SceneObjectModel::RowAt(int row) const
{
    if(row < 0 || row >= (int)rows_.size())
    {
        return nullptr;
    }
    return &rows_[(size_t)row];
}

const studio::SceneObject*
SceneObjectModel::FindResolved(const std::string& id) const
{
    if(resolved_ == nullptr)
    {
        return nullptr;
    }
    return studio::FindObject(*resolved_, id);
}

/*---------------------------------------------------------*\
|| Row order: depth-first over the DRAFT's parent chains.||
|| Each instance row is followed by its resolved entity  ||
|| subtree (topological order filtered to the instance    ||
|| prefix) — the source model's own row contract — then ||
|| its child instances. An instance the resolver failed  ||
|| to expand still owns a row; it just has no children.  ||
\*---------------------------------------------------------*/
void SceneObjectModel::ResetFrom()
{
    beginResetModel();
    rows_.clear();
    if(ws_ != nullptr)
    {
        /* children-of map (instance -> direct child instances) */
        std::map<std::string, std::vector<std::string>> kids;
        std::vector<std::string> roots;
        for(const auto& kv : ws_->devices)
        {
            const std::string& pid = kv.second.parent;
            if(!pid.empty() && ws_->devices.count(pid))
            {
                kids[pid].push_back(kv.first);
            }
            else
            {
                roots.push_back(kv.first);
            }
        }
        /* Resolved objects grouped under their root instance,
           in topological order (parents precede children). */
        std::map<std::string, std::vector<const studio::SceneObject*>>
            ent;
        if(resolved_ != nullptr)
        {
            for(const studio::SceneObject* o :
                studio::TopologicalOrder(*resolved_))
            {
                const std::string iid =
                    o->id.substr(0, o->id.find('/'));
                if(iid != o->id)   /* entity rows only — the root is
                                      the instance row's data source */
                {
                    ent[iid].push_back(o);
                }
            }
        }
        std::function<void(const std::string&)> walk =
            [&](const std::string& iid)
        {
            /* Instance row — the draft is authoritative; when the
               resolved root exists it shares the same id so
               FindResolved(iid) still answers for roles like
               bound/verified that only resolution knows. */
            const auto dit = ws_->devices.find(iid);
            Row inst;
            inst.id         = iid;
            inst.instance   = iid;
            inst.draft      = true;
            if(dit != ws_->devices.end())
            {
                inst.parent_row = dit->second.parent;
            }
            rows_.push_back(inst);

            for(const studio::SceneObject* o : ent[iid])
            {
                Row r;
                r.id         = o->id;
                r.instance   = iid;
                r.parent_row = o->parent_id;
                r.draft      = false;
                rows_.push_back(r);
            }
            for(const std::string& k : kids[iid])
            {
                walk(k);
            }
        };
        for(const std::string& r : roots)
        {
            walk(r);
        }
    }
    RecomputeWorlds();
    endResetModel();
}

void SceneObjectModel::RecomputeWorlds()
{
    worlds_.clear();
    /* Compact instance worlds — parent-chain compose, exact the
       way ResolveWorldMatrices does it for resolved objects.
       A dangling/cyclic parent degrades to "no parent" (the
       document validator is what rejects bad chains). */
    if(ws_ != nullptr)
    {
        std::map<std::string, studio::Mat4> memo;
        std::function<studio::Mat4(const std::string&,
                                   std::set<std::string>&)> world_of =
            [&](const std::string& iid,
                std::set<std::string>& stack) -> studio::Mat4
        {
            const auto mi = memo.find(iid);
            if(mi != memo.end())
            {
                return mi->second;
            }
            studio::Mat4 m = studio::Mat4Identity();
            const auto it = ws_->devices.find(iid);
            if(it != ws_->devices.end() && stack.insert(iid).second)
            {
                m = studio::LocalMatrix(InstanceTransform(it->second));
                const std::string& pid = it->second.parent;
                if(!pid.empty() && ws_->devices.count(pid))
                {
                    m = studio::Mat4Mul(world_of(pid, stack), m);
                }
            }
            memo[iid] = m;
            return m;
        };
        for(const auto& kv : ws_->devices)
        {
            std::set<std::string> stack;
            worlds_[kv.first] = world_of(kv.first, stack);
        }
    }
    if(resolved_ != nullptr)
    {
        for(auto& kv : studio::ResolveWorldMatrices(*resolved_))
        {
            worlds_[kv.first] = kv.second;
        }
    }
}

/* draft row -> the first zone binding the instance carries (or its
   mirror owner's), mapped through the owner's snapshot join. */
QString SceneObjectModel::BoundStatusOf(const Row& r) const
{
    std::string binding;
    const studio::SceneObject* obj = FindResolved(r.id);

    if(obj != nullptr && obj->kind == studio::ObjectKind::Linked)
    {
        /* Linked rows read through to their output owner — the
           linked copy shows the SOURCE's reachability. */
        const studio::SceneObject* owner =
            studio::OutputOwner(*resolved_, obj->id);
        if(owner != nullptr)
        {
            binding = owner->binding;
        }
    }
    else if(obj != nullptr && !obj->binding.empty())
    {
        binding = obj->binding;
    }
    else if(ws_ != nullptr)
    {
        /* Unresolved/unknown rows: answer from the compact doc so
           a bound-but-unresolvable instance still reports. */
        std::string path = r.id;
        const auto sit = ws_->device_settings.find(path);
        if(sit != ws_->device_settings.end())
        {
            if(!sit->second.mirror_of.empty())
            {
                const auto oit =
                    ws_->device_settings.find(sit->second.mirror_of);
                if(oit != ws_->device_settings.end())
                {
                    for(const auto& z : oit->second.zones)
                    {
                        if(!z.second.binding.empty())
                        {
                            binding = z.second.binding;
                            break;
                        }
                    }
                }
            }
            if(binding.empty())
            {
                for(const auto& z : sit->second.zones)
                {
                    if(!z.second.binding.empty())
                    {
                        binding = z.second.binding;
                        break;
                    }
                }
            }
        }
    }

    if(binding.empty())
    {
        return QStringLiteral("none");
    }
    if(!bound_fn_)
    {
        return QStringLiteral("unresolved");
    }
    const QString s = bound_fn_(binding);
    return s.isEmpty() ? QStringLiteral("unresolved") : s;
}

QString SceneObjectModel::ColorOf(const std::string& object_id) const
{
    /* Painted base color — the draft's map is authoritative (the
       resolved snapshot mirrors it after a successful resolve). */
    if(ws_ != nullptr)
    {
        const auto it = ws_->object_colors.find(object_id);
        if(it != ws_->object_colors.end())
        {
            return QString::fromStdString(
                studio::SceneColorHex(it->second));
        }
    }
    if(resolved_ != nullptr)
    {
        const auto it = resolved_->object_colors.find(object_id);
        if(it != resolved_->object_colors.end())
        {
            return QString::fromStdString(
                studio::SceneColorHex(it->second));
        }
    }
    return QString();
}

int SceneObjectModel::rowCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : (int)rows_.size();
}

QVariant SceneObjectModel::data(const QModelIndex& index, int role) const
{
    const Row* r = RowAt(index.row());
    if(r == nullptr)
    {
        return QVariant();
    }
    const studio::SceneObject*   o  = FindResolved(r->id);
    const studio::DeviceInstance* di = nullptr;
    const studio::DeviceSettings* ds = nullptr;
    if(r->draft && ws_ != nullptr)
    {
        const auto dit = ws_->devices.find(r->id);
        if(dit != ws_->devices.end())
        {
            di = &dit->second;
        }
        const auto sit = ws_->device_settings.find(r->id);
        if(sit != ws_->device_settings.end())
        {
            ds = &sit->second;
        }
    }

    switch(role)
    {
    case IdRole:
        return QString::fromStdString(r->id);
    case LabelRole:
        if(o != nullptr && !o->label.empty() && !r->draft)
        {
            return QString::fromStdString(o->label);
        }
        return QString::fromStdString(r->id);
    case KindRole:
        if(r->draft)
        {
            if(ds != nullptr && !ds->mirror_of.empty())
            {
                return QStringLiteral("linked");
            }
            if(di != nullptr && di->type == "group")
            {
                return QStringLiteral("group");
            }
            /* Any resolved emitter-carrying child => a device. */
            return QStringLiteral("device");
        }
        return o != nullptr ? KindName(o->kind)
                            : QStringLiteral("decor");
    case GeometryRole:
        return o != nullptr ? QString::fromStdString(o->geometry)
                            : QVariant();
    case ParentIdRole:
        return QString::fromStdString(r->parent_row);
    case InstancePathRole:
        return QString::fromStdString(r->instance);
    case XRole:
        if(r->draft && di != nullptr) return (double)di->position.x;
        return o != nullptr ? (double)o->transform.position.x : 0.0;
    case YRole:
        if(r->draft && di != nullptr) return (double)di->position.y;
        return o != nullptr ? (double)o->transform.position.y : 0.0;
    case ZRole:
        if(r->draft && di != nullptr) return (double)di->position.z;
        return o != nullptr ? (double)o->transform.position.z : 0.0;
    case RxRole:
        if(r->draft && di != nullptr) return (double)di->rotation_deg.x;
        return o != nullptr ? (double)o->transform.rotation_deg.x : 0.0;
    case RyRole:
        if(r->draft && di != nullptr) return (double)di->rotation_deg.y;
        return o != nullptr ? (double)o->transform.rotation_deg.y : 0.0;
    case RzRole:
        if(r->draft && di != nullptr) return (double)di->rotation_deg.z;
        return o != nullptr ? (double)o->transform.rotation_deg.z : 0.0;
    case WxRole:
    case WyRole:
    case WzRole:
    {
        const auto wit = worlds_.find(r->id);
        if(wit == worlds_.end())
        {
            return 0.0;
        }
        const studio::Vec3 p = studio::Mat4Translation(wit->second);
        if(role == WxRole) return (double)p.x;
        if(role == WyRole) return (double)p.y;
        return (double)p.z;
    }
    case WryRole:
    {
        const auto wit = worlds_.find(r->id);
        if(wit == worlds_.end())
        {
            return 0.0;
        }
        return (double)studio::EulerDegFromMat4(wit->second).y;
    }
    case SxRole: return o != nullptr ? (double)o->transform.scale.x : 1.0;
    case SyRole: return o != nullptr ? (double)o->transform.scale.y : 1.0;
    case SzRole: return o != nullptr ? (double)o->transform.scale.z : 1.0;
    case DxRole: return o != nullptr ? (double)o->size_m.x : 0.0;
    case DyRole: return o != nullptr ? (double)o->size_m.y : 0.0;
    case DzRole: return o != nullptr ? (double)o->size_m.z : 0.0;
    case BxRole:
    case ByRole:
    case BzRole:
    {
        /* Resolved body size — the "0 axis = canonical" contract
           lives in the core, not in QML. */
        const studio::Vec3 body = o != nullptr
            ? studio::ResolvedBodySize(*o) : studio::Vec3{};
        if(role == BxRole) return (double)body.x;
        if(role == ByRole) return (double)body.y;
        return (double)body.z;
    }
    case VisibleRole:
        if(r->draft) return ds == nullptr || ds->visible;
        return o == nullptr || o->visible;
    case LockedRole:
        /* Lock is instance state — a child row reports its root
           instance's lock (locked descendants are locked roots). */
        if(ws_ != nullptr)
        {
            const auto it = ws_->device_settings.find(r->instance);
            return it != ws_->device_settings.end() && it->second.locked;
        }
        return false;
    case VerifiedRole:
        return o != nullptr && o->verified;
    case EmittersRole:
    {
        /* Instance rows total their subtree's emitters; entity
           rows report their own. */
        if(!r->draft || resolved_ == nullptr)
        {
            return o != nullptr ? (int)o->emitters.size() : 0;
        }
        int n = 0;
        const std::string prefix = r->id + "/";
        for(const auto& ro : rows_)
        {
            if(ro.draft || ro.id.compare(0, prefix.size(), prefix) != 0)
            {
                continue;
            }
            const studio::SceneObject* eo = FindResolved(ro.id);
            if(eo != nullptr)
            {
                n += (int)eo->emitters.size();
            }
        }
        return n;
    }
    case BoundRole:
        return BoundStatusOf(*r);
    case ColorRole:
        return ColorOf(r->id);
    case EmitterColorsRole:
    {
        QVariantList out;
        if(resolved_ != nullptr)
        {
            /* Effective per-emitter colors — painted override, base
               color, or linked-owner read-through handled by the
               core's EmitterColor. */
            const std::string oid = r->id;
            const studio::SceneObject* owner =
                studio::OutputOwner(*resolved_, oid);
            const studio::SceneObject* base =
                owner != nullptr ? owner : FindResolved(oid);
            if(base != nullptr)
            {
                for(size_t i = 0; i < base->emitters.size(); ++i)
                {
                    out.push_back(QString::fromStdString(
                        studio::SceneColorHex(studio::EmitterColor(
                            *resolved_, base->id, (int)i))));
                }
            }
        }
        return out;
    }
    case EmitterPointsRole:
    {
        /* {x,y,z,c,i} in object-local space — the canvas projects
           through the row's world matrix. */
        QVariantList out;
        if(resolved_ != nullptr)
        {
            const studio::SceneObject* owner =
                studio::OutputOwner(*resolved_, r->id);
            const studio::SceneObject* base =
                owner != nullptr ? owner : FindResolved(r->id);
            if(base != nullptr)
            {
                for(size_t i = 0; i < base->emitters.size(); ++i)
                {
                    const studio::Emitter& e = base->emitters[i];
                    QVariantMap m;
                    m.insert(QStringLiteral("x"), (double)e.local_pos.x);
                    m.insert(QStringLiteral("y"), (double)e.local_pos.y);
                    m.insert(QStringLiteral("z"), (double)e.local_pos.z);
                    m.insert(QStringLiteral("i"), (int)i);
                    m.insert(QStringLiteral("o"),
                             QString::fromStdString(base->id));
                    m.insert(QStringLiteral("c"),
                             QString::fromStdString(
                                 studio::SceneColorHex(
                                     studio::EmitterColor(
                                         *resolved_, base->id, (int)i))));
                    out.push_back(m);
                }
            }
        }
        return out;
    }
    default:
        return QVariant();
    }
}

QHash<int, QByteArray> SceneObjectModel::roleNames() const
{
    return {
        { IdRole,           "id" },
        { LabelRole,        "label" },
        { KindRole,         "kind" },
        { GeometryRole,     "geometry" },
        { ParentIdRole,     "parentId" },
        { XRole,            "x" },
        { YRole,            "y" },
        { ZRole,            "z" },
        { RxRole,           "rx" },
        { RyRole,           "ry" },
        { RzRole,           "rz" },
        { WxRole,           "wx" },
        { WyRole,           "wy" },
        { WzRole,           "wz" },
        { WryRole,          "wry" },
        { SxRole,           "sx" },
        { SyRole,           "sy" },
        { SzRole,           "sz" },
        { DxRole,           "dx" },
        { DyRole,           "dy" },
        { DzRole,           "dz" },
        { BxRole,           "bx" },
        { ByRole,           "by" },
        { BzRole,           "bz" },
        { VisibleRole,      "visible" },
        { VerifiedRole,     "verified" },
        { EmittersRole,     "emitters" },
        { BoundRole,        "bound" },
        { InstancePathRole, "instancePath" },
        { LockedRole,       "locked" },
        { ColorRole,        "color" },
        { EmitterColorsRole,"emitterColors" },
        { EmitterPointsRole,"emitterPoints" },
    };
}

void SceneObjectModel::UpdateTransforms(const std::set<std::string>& ids)
{
    RecomputeWorlds();
    for(int row = 0; row < (int)rows_.size(); ++row)
    {
        /* An instance's transform lives on its draft row; entity
           rows keep local transforms but a re-key may touch any
           path, so prefix-match the instance segment. */
        const std::string& id  = rows_[(size_t)row].id;
        const std::string  inst = id.substr(0, id.find('/'));
        if(ids.count(id) || ids.count(inst))
        {
            const QModelIndex mi = index(row, 0);
            emit dataChanged(mi, mi);
        }
    }
}

void SceneObjectModel::RefreshAll()
{
    RecomputeWorlds();
    if(rows_.empty())
    {
        return;
    }
    emit dataChanged(index(0, 0), index((int)rows_.size() - 1, 0));
}

int SceneObjectModel::RowOf(const std::string& id) const
{
    for(int i = 0; i < (int)rows_.size(); ++i)
    {
        if(rows_[(size_t)i].id == id)
        {
            return i;
        }
    }
    return -1;
}

QVariantMap SceneObjectModel::rowAt(int row) const
{
    QVariantMap m;
    const QModelIndex mi = index(row, 0);
    if(!mi.isValid())
    {
        return m;
    }
    const auto names = roleNames();
    for(auto it = names.begin(); it != names.end(); ++it)
    {
        m.insert(QString::fromLatin1(it.value()), data(mi, it.key()));
    }
    return m;
}

} /* namespace ui */
} /* namespace openrgb2 */
