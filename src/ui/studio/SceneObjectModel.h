/*---------------------------------------------------------*\
|| SceneObjectModel.h                                       ||
||                                                          ||
||   QAbstractListModel over the scene the studio object ||
||   panels edit (studio-import Task 5): the compact      ||
||   workspace DRAFT is authoritative for instance rows   ||
||   (a just-added/unresolvable instance is still a row);||
||   the resolved SceneDocument snapshot — draft-resolved ||
||   first, engine-applied as the honest fallback —       ||
||   enriches each row with entity children, emitter      ||
||   counts, resolved body sizes and verified/mirror      ||
||   state. The model OWNS nothing: it stores row IDs     ||
||   only and re-reads the bound documents per query, so  ||
||   a replaced resolved snapshot can't leave dangling    ||
||   pointers.                                            ||
||                                                          ||
||   Adapted from the source-of-record editor/             ||
||   SceneObjectModel (provenance ledger): same role set  ||
||   plus world-position and paint roles for the 2D       ||
||   canvas, same two update paths — ResetFrom() for      ||
||   structural changes, UpdateTransforms(ids) for the    ||
||   drag path's granular dataChanged. Rows follow the    ||
||   instance's depth-first order (draft parent chains)   ||
||   so a delegate indenting on '/' depth renders a real  ||
||   hierarchy — groups nest their children.              ||
||                                                          ||
||   Bound status is a callback (compact binding id ->    ||
||   "ok" / "ambiguous" / "unresolved"); the owner joins  ||
||   engine snapshots — the model never touches the wire. ||
||                                                          ||
||   SPDX-License-Identifier: GPL-2.0-or-later               ||
|\*---------------------------------------------------------*/

#pragma once

#include <QAbstractListModel>
#include <QHash>
#include <QString>
#include <QVariant>
#include <QVariantMap>

#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "config/StudioConfig.h"
#include "scene/SceneGraph.h"            /* Mat4                  */
#include "scene/SceneTypes.h"

namespace openrgb2
{
namespace ui
{

class SceneObjectModel : public QAbstractListModel
{
    Q_OBJECT
public:
    enum Role
    {
        IdRole = Qt::UserRole + 1,
        LabelRole,
        KindRole,        /* "device"|"linked"|"group"|"decor"|"instance" */
        GeometryRole,
        ParentIdRole,    /* row id of the parent row ("" = root)        */
        XRole, YRole, ZRole,     /* LOCAL transform (m/deg)             */
        RxRole, RyRole, RzRole,
        WxRole, WyRole, WzRole,  /* WORLD position (m) — 2D canvas      */
        WryRole,                 /* world yaw (deg)                     */
        SxRole, SyRole, SzRole,
        DxRole, DyRole, DzRole,  /* authored size_m                     */
        BxRole, ByRole, BzRole,  /* resolved body size                  */
        VisibleRole,
        VerifiedRole,
        EmittersRole,
        BoundRole,               /* "ok"|"ambiguous"|"unresolved"|"none"*/
        InstancePathRole,        /* owning root instance id             */
        LockedRole,
        ColorRole,               /* painted base color or ""            */
        EmitterColorsRole,       /* QVariantList of "#RRGGBB"           */
        EmitterPointsRole,       /* QVariantList {x,y,z,c,i}            */
        DepthRole,               /* real tree depth (nested groups)     */
    };
    Q_ENUM(Role)

    explicit SceneObjectModel(QObject* parent = nullptr);

    /* Bind the read views. All pointers must stay valid for the
       model's life:
       - workspace  — the session's Working() draft (authoritative
         for instance rows; &session.Working() stays valid — the
         session owns the document member)
       - resolved   — the latest resolved SceneDocument snapshot
         owned by the controller (contents are replaced in place;
         the model only stores ids)
       - bound_fn   — compact binding id -> status token ("ok",
         "ambiguous", "unresolved"); empty binding maps to "none". */
    using BoundFn = std::function<QString(const std::string&)>;
    void Bind(const studio::StudioDocument* workspace,
              const studio::SceneDocument*  resolved,
              BoundFn bound_fn);

    int      rowCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index,
                  int role = Qt::DisplayRole) const override;
    QHash<int, QByteArray> roleNames() const override;

    /* Structure changed (instances added/removed/renamed/
       reparented, or a fresh resolved snapshot landed): rebuild
       the row order. */
    void ResetFrom();
    /* Only transforms changed for these root instance ids:
       per-row dataChanged — the gesture path. */
    void UpdateTransforms(const std::set<std::string>& ids);
    /* Every row re-read (snapshot refresh). */
    void RefreshAll();

    int RowOf(const std::string& id) const;
    /* QML convenience: every role of one row as a plain map —
       matches the objectList QVariantList the tree's stub path
       consumes in tests. */
    Q_INVOKABLE QVariantMap rowAt(int row) const;

private:
    /* One model row. `draft` rows name a compact instance
       (transform/settings read live from the draft); entity rows
       name a resolved object id (re-looked-up per query). */
    struct Row
    {
        std::string id;           /* instance id or resolved object id */
        std::string instance;     /* owning root instance id           */
        std::string parent_row;   /* row id of the parent ("" = root)  */
        int         depth = 0;    /* tree depth for indent             */
        bool        draft = false;
    };

    const Row* RowAt(int row) const;
    const studio::SceneObject* FindResolved(const std::string& id) const;
    /* World transforms of compact instances (parent-chain compose)
       and resolved objects — recomputed inside ResetFrom /
       UpdateTransforms so data() stays a pure lookup. */
    void RecomputeWorlds();
    QString BoundStatusOf(const Row& r) const;
    QString ColorOf(const std::string& object_id) const;

    const studio::StudioDocument* ws_       = nullptr;
    const studio::SceneDocument*  resolved_ = nullptr;
    BoundFn                       bound_fn_;
    std::vector<Row>              rows_;
    std::map<std::string, studio::Mat4> worlds_;      /* object/inst id */
};

} /* namespace ui */
} /* namespace openrgb2 */
