/*---------------------------------------------------------*\
||| EffectLayerModel.cpp                                     ||
|||                                                          ||
|||   SPDX-License-Identifier: GPL-2.0-or-later               ||
\*---------------------------------------------------------*/

#include "EffectLayerModel.h"

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

/* Display-field equality — the same fields RowMap emits, minus the
   row index (suffix rows legitimately shift index across an
   insert/remove). Two rows equal here need no dataChanged. */
bool SameDisplayRow(const studio::EffectLayer& a,
                    const studio::EffectLayer& b)
{
    return a.primitive == b.primitive
        && a.enabled   == b.enabled
        && a.blend     == b.blend
        && a.opacity   == b.opacity
        && a.space     == b.space
        && a.source    == b.source
        && a.palette.stops.size() == b.palette.stops.size()
        && a.path.size()          == b.path.size();
}

} /* anonymous namespace */

EffectLayerModel::EffectLayerModel(QObject* parent)
    : QAbstractListModel(parent)
{
}

int EffectLayerModel::rowCount(const QModelIndex& parent) const
{
    if(parent.isValid())
    {
        return 0;
    }
    return (int)rows_.size();
}

QVariantMap EffectLayerModel::RowMap(const studio::EffectLayer& l,
                                     int row) const
{
    QVariantMap m;
    m["index"]     = row;
    m["primitive"] = QString::fromStdString(l.primitive);
    m["enabled"]   = l.enabled;
    m["blend"]     = BlendName(l.blend);
    m["opacity"]   = l.opacity;
    m["space"]     = SpaceName(l.space);
    m["source"]    = QString::fromStdString(l.source);
    m["stops"]     = (int)l.palette.stops.size();
    m["pathPts"]   = (int)l.path.size();
    /* Compact row summary: "world · ×0.85 · 4 stops". */
    QString s = QString::fromLatin1(SpaceName(l.space));
    if(l.opacity != 1.0f)
    {
        s += QStringLiteral(" · ×%1")
                 .arg(QString::number(l.opacity, 'g', 3));
    }
    if(!l.palette.stops.empty())
    {
        s += QStringLiteral(" · %1 stops")
                 .arg(l.palette.stops.size());
    }
    if(!l.path.empty())
    {
        s += QStringLiteral(" · %1 pts").arg(l.path.size());
    }
    m["summary"] = s;
    return m;
}

QVariant EffectLayerModel::data(const QModelIndex& index,
                                int role) const
{
    if(!index.isValid() || index.row() < 0
       || index.row() >= (int)rows_.size())
    {
        return {};
    }
    const studio::EffectLayer& l = rows_[index.row()];
    switch(role)
    {
    case IndexRole:     return index.row();
    case PrimitiveRole: return QString::fromStdString(l.primitive);
    case EnabledRole:   return l.enabled;
    case BlendRole:     return BlendName(l.blend);
    case OpacityRole:   return l.opacity;
    case SpaceRole:     return SpaceName(l.space);
    case SourceRole:    return QString::fromStdString(l.source);
    case StopsRole:     return (int)l.palette.stops.size();
    case PathRole:      return (int)l.path.size();
    case SummaryRole:   return RowMap(l, index.row())["summary"];
    default:            return {};
    }
}

QHash<int, QByteArray> EffectLayerModel::roleNames() const
{
    return {
        { IndexRole,     "index"     },
        { PrimitiveRole, "primitive" },
        { EnabledRole,   "enabled"   },
        { BlendRole,     "blend"     },
        { OpacityRole,   "opacity"   },
        { SpaceRole,     "space"     },
        { SourceRole,    "source"    },
        { StopsRole,     "stops"     },
        { PathRole,      "pathPts"   },
        { SummaryRole,   "summary"   },
    };
}

bool EffectLayerModel::SetStack(
    const std::vector<studio::EffectLayer>& layers)
{
    if(layers.size() == rows_.size())
    {
        /* Same shape — diff each row's display fields so a scrub
           preview updates the changed row without a reset (this is
           the guarantee that keeps a pressed Slider delegate alive
           mid-gesture). The new stack is assigned BEFORE the emits:
           a delegate resolving dataChanged must read the new row,
           not the stale one. */
        std::vector<int> changed;
        for(int i = 0; i < (int)rows_.size(); i++)
        {
            if(RowMap(rows_[i], i) != RowMap(layers[i], i))
            {
                changed.push_back(i);
            }
        }
        if(changed.empty())
        {
            return false;
        }
        rows_ = layers;
        for(int i : changed)
        {
            emit dataChanged(index(i), index(i));
        }
        return true;
    }
    /* Shape change — express it as ONE contiguous insert/remove
       run when the untouched rows keep their display content
       (longest common prefix + suffix), so add/remove/reorder-by-
       delete keeps surviving delegates alive too. A non-contiguous
       rewrite still resets. */
    const std::vector<studio::EffectLayer>& small =
        layers.size() < rows_.size() ? layers : rows_;
    const std::vector<studio::EffectLayer>& large =
        layers.size() < rows_.size() ? rows_ : layers;
    int pre = 0;
    while(pre < (int)small.size()
          && SameDisplayRow(small[pre], large[pre]))
    {
        ++pre;
    }
    int suf = 0;
    while(suf < (int)small.size() - pre
          && SameDisplayRow(small[small.size() - 1 - suf],
                            large[large.size() - 1 - suf]))
    {
        ++suf;
    }
    if(pre + suf == (int)small.size())
    {
        if(layers.size() > rows_.size())
        {
            const int last =
                pre + (int)(layers.size() - rows_.size()) - 1;
            beginInsertRows(QModelIndex(), pre, last);
            rows_ = layers;
            endInsertRows();
        }
        else
        {
            const int last =
                pre + (int)(rows_.size() - layers.size()) - 1;
            beginRemoveRows(QModelIndex(), pre, last);
            rows_ = layers;
            endRemoveRows();
        }
        return true;
    }
    beginResetModel();
    rows_ = layers;
    endResetModel();
    return true;
}

QVariantMap EffectLayerModel::rowAt(int row) const
{
    if(row < 0 || row >= (int)rows_.size())
    {
        return {};
    }
    return RowMap(rows_[row], row);
}

} /* namespace ui */
} /* namespace openrgb2 */
