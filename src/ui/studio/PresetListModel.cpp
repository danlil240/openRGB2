/*---------------------------------------------------------*\
|| PresetListModel.cpp                                      ||
||                                                          ||
||   SPDX-License-Identifier: GPL-2.0-or-later               ||
|\*---------------------------------------------------------*/

#include "PresetListModel.h"

#include <algorithm>
#include <set>

namespace openrgb2
{
namespace ui
{

PresetListModel::PresetListModel(QObject* parent)
    : QAbstractListModel(parent)
{
}

void PresetListModel::Bind(const studio::PresetRegistry* reg,
                           const std::vector<std::string>* favorites)
{
    reg_       = reg;
    favorites_ = favorites;
}

int PresetListModel::rowCount(const QModelIndex& parent) const
{
    if(parent.isValid())
    {
        return 0;
    }
    return (int)rows_.size() + (int)extra_.size();
}

bool PresetListModel::IsFavorite(const std::string& id) const
{
    if(favorites_ == nullptr)
    {
        return false;
    }
    return std::find(favorites_->begin(), favorites_->end(), id)
           != favorites_->end();
}

QVariantMap PresetListModel::RowMap(const QString& id,
                                    const QString& name,
                                    const QString& category,
                                    bool from_file, int zone_count,
                                    int led_total,
                                    const QVector3D& bounds,
                                    bool engine_only) const
{
    QVariantMap m;
    m.insert(QStringLiteral("typeId"),     id);
    m.insert(QStringLiteral("name"),       name);
    m.insert(QStringLiteral("category"),   category);
    m.insert(QStringLiteral("fromFile"),   from_file);
    m.insert(QStringLiteral("zoneCount"),  zone_count);
    m.insert(QStringLiteral("ledTotal"),   led_total);
    m.insert(QStringLiteral("bounds"),     QVariant::fromValue(bounds));
    m.insert(QStringLiteral("engineOnly"), engine_only);
    m.insert(QStringLiteral("favorite"),   IsFavorite(id.toStdString()));
    return m;
}

QVariant PresetListModel::data(const QModelIndex& index, int role) const
{
    if(!index.isValid() || index.row() < 0)
    {
        return {};
    }
    /* Registry rows first, engine-catalog extras after — the QML
       pipeline re-sorts anyway (favorites-first, then A-Z). */
    const int row = index.row();
    if(row < (int)rows_.size())
    {
        const studio::PresetRegistry::PresetInfo& info =
            rows_[(size_t)row];
        switch(role)
        {
        case TypeIdRole:    return QString::fromStdString(info.id);
        case NameRole:      return QString::fromStdString(info.name);
        case CategoryRole:  return QString::fromStdString(info.category);
        case FromFileRole:  return info.from_file;
        case ZoneCountRole: return (int)info.zone_count;
        case LedTotalRole:  return (int)info.led_total;
        case BoundsRole:
            return QVariant::fromValue(
                QVector3D(info.bounds_m.x, info.bounds_m.y,
                          info.bounds_m.z));
        case FavoriteRole:   return IsFavorite(info.id);
        case EngineOnlyRole: return false;
        default:             return {};
        }
    }
    const int ex = row - (int)rows_.size();
    if(ex >= (int)extra_.size())
    {
        return {};
    }
    const QVariantMap m = extra_.at(ex).toMap();
    switch(role)
    {
    case TypeIdRole:    return m.value(QStringLiteral("typeId"));
    case NameRole:      return m.value(QStringLiteral("name"));
    case CategoryRole:  return m.value(QStringLiteral("category"));
    case FromFileRole:  return m.value(QStringLiteral("fromFile"));
    case ZoneCountRole: return m.value(QStringLiteral("zoneCount"));
    case LedTotalRole:  return m.value(QStringLiteral("ledTotal"));
    case BoundsRole:    return m.value(QStringLiteral("bounds"));
    case FavoriteRole:
        return IsFavorite(m.value(QStringLiteral("typeId"))
                              .toString().toStdString());
    case EngineOnlyRole: return true;
    default:             return {};
    }
}

QHash<int, QByteArray> PresetListModel::roleNames() const
{
    return {
        { TypeIdRole,     "typeId"     },
        { NameRole,       "name"       },
        { CategoryRole,   "category"   },
        { FromFileRole,   "fromFile"   },
        { ZoneCountRole,  "zoneCount"  },
        { LedTotalRole,   "ledTotal"   },
        { BoundsRole,     "bounds"     },
        { FavoriteRole,   "favorite"   },
        { EngineOnlyRole, "engineOnly" },
    };
}

void PresetListModel::Reload()
{
    beginResetModel();
    rows_ = (reg_ != nullptr)
          ? reg_->List()
          : std::vector<studio::PresetRegistry::PresetInfo>();
    /* Drop catalog extras the registry now knows locally. */
    QVariantList kept;
    std::set<std::string> known;
    for(const auto& r : rows_)
    {
        known.insert(r.id);
    }
    for(const QVariant& v : extra_)
    {
        if(!known.count(v.toMap()
                 .value(QStringLiteral("typeId")).toString()
                 .toStdString()))
        {
            kept.push_back(v);
        }
    }
    extra_ = kept;
    endResetModel();
}

void PresetListModel::SetEngineCatalog(const QVariantList& entries)
{
    beginResetModel();
    extra_.clear();
    std::set<std::string> known;
    for(const auto& r : rows_)
    {
        known.insert(r.id);
    }
    for(const QVariant& v : entries)
    {
        const QVariantMap e = v.toMap();
        const QString id = e.value(QStringLiteral("id")).toString();
        if(id.isEmpty() || known.count(id.toStdString()))
        {
            continue;
        }
        known.insert(id.toStdString());
        extra_.push_back(RowMap(
            id,
            e.value(QStringLiteral("name")).toString(),
            e.value(QStringLiteral("category")).toString(),
            e.value(QStringLiteral("from_file")).toBool(),
            e.value(QStringLiteral("zone_count")).toInt(),
            e.value(QStringLiteral("led_total")).toInt(),
            QVector3D(),      /* the catalog carries no bounds */
            /*engine_only*/ true));
    }
    endResetModel();
}

void PresetListModel::RefreshFavorites()
{
    const int n = rowCount();
    if(n == 0)
    {
        return;
    }
    emit dataChanged(index(0, 0), index(n - 1, 0), { FavoriteRole });
}

const studio::PresetRegistry::PresetInfo*
PresetListModel::InfoFor(const QString& id) const
{
    const std::string s = id.toStdString();
    for(const studio::PresetRegistry::PresetInfo& info : rows_)
    {
        if(info.id == s)
        {
            return &info;
        }
    }
    return nullptr;
}

bool PresetListModel::Knows(const QString& id) const
{
    if(InfoFor(id) != nullptr)
    {
        return true;
    }
    const std::string s = id.toStdString();
    for(const QVariant& v : extra_)
    {
        if(v.toMap().value(QStringLiteral("typeId")).toString()
               .toStdString() == s)
        {
            return true;
        }
    }
    return false;
}

QVariantMap PresetListModel::rowAt(int row) const
{
    if(row < 0 || row >= rowCount())
    {
        return {};
    }
    if(row < (int)rows_.size())
    {
        const studio::PresetRegistry::PresetInfo& info =
            rows_[(size_t)row];
        return RowMap(QString::fromStdString(info.id),
                      QString::fromStdString(info.name),
                      QString::fromStdString(info.category),
                      info.from_file, (int)info.zone_count,
                      (int)info.led_total,
                      QVector3D(info.bounds_m.x, info.bounds_m.y,
                                info.bounds_m.z),
                      /*engine_only*/ false);
    }
    QVariantMap m = extra_.at(row - (int)rows_.size()).toMap();
    /* Favorite state is live — never the catalog-arrival snapshot. */
    m.insert(QStringLiteral("favorite"),
             IsFavorite(m.value(QStringLiteral("typeId"))
                            .toString().toStdString()));
    return m;
}

} /* namespace ui */
} /* namespace openrgb2 */
