/*---------------------------------------------------------*\
||| EffectLayerModel.h                                       ||
|||                                                          ||
|||   QAbstractListModel over the EFFECTIVE effect-layer    ||
|||   stack (authored inline layers when present, else the  ||
|||   resolved named-look stack) — Qt, UI-only, NOT part    ||
|||   of the Qt-free editor core. Feeds the effect editor's ||
|||   layer-stack panel: one row per layer with the summary ||
|||   facts the strip needs (primitive, enabled, blend,     ||
|||   opacity, space, palette/path sizes). Per-layer detail ||
|||   fields (stops, path points, targets) ride             ||
|||   EffectEditorController::layerAt(i) — a QVariantMap —  ||
|||   so the inspector doesn't need per-row role churn for  ||
|||   nested data.                                          ||
|||                                                          ||
|||   Adapted from the source-of-record                      ||
|||   plugins/DesktopLightingStudio/editor/                  ||
|||   EffectLayerModel (provenance ledger): same roles,     ||
|||   same update path — SetStack() diffs against the       ||
|||   snapshot and emits a row-granular dataChanged when    ||
|||   the shape is unchanged (scrub preview keeps           ||
|||   delegates alive), a contiguous                        ||
|||   rowsInserted/rowsRemoved for add/remove, a full       ||
|||   reset only for non-contiguous rewrites.               ||
|||                                                          ||
|||   SPDX-License-Identifier: GPL-2.0-or-later               ||
\*---------------------------------------------------------*/

#pragma once

#include <QAbstractListModel>
#include <QHash>
#include <QString>
#include <QVariant>
#include <QVariantMap>

#include "scene/SceneTypes.h"

#include <string>
#include <vector>

namespace openrgb2
{
namespace ui
{

class EffectLayerModel : public QAbstractListModel
{
    Q_OBJECT
public:
    enum Role
    {
        IndexRole = Qt::UserRole + 1,
        PrimitiveRole,
        EnabledRole,
        BlendRole,      /* "replace" | "add" | "screen"            */
        OpacityRole,
        SpaceRole,      /* "world" | "local"                       */
        SourceRole,     /* "" | "audio" | "key" | "screen"         */
        StopsRole,      /* palette stop count                      */
        PathRole,       /* path point count                        */
        SummaryRole,    /* one-line row summary                    */
    };
    Q_ENUM(Role)

    explicit EffectLayerModel(QObject* parent = nullptr);

    int      rowCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index,
                  int role = Qt::DisplayRole) const override;
    QHash<int, QByteArray> roleNames() const override;

    /* Replace the snapshot. Same-size stacks diff per row; a size
       change emits a contiguous rowsInserted/rowsRemoved when the
       untouched rows keep their content (resets otherwise).
       Returns true when the effective rows changed. */
    bool SetStack(const std::vector<studio::EffectLayer>& layers);

    /* QML helpers — plain QVariantMap rows so the same panel code
       runs against a plain-JS stub in tests. */
    Q_INVOKABLE int count() const { return (int)rows_.size(); }
    Q_INVOKABLE QVariantMap rowAt(int row) const;

private:
    QVariantMap RowMap(const studio::EffectLayer& l, int row) const;

    std::vector<studio::EffectLayer> rows_;
};

} /* namespace ui */
} /* namespace openrgb2 */
