/*---------------------------------------------------------*\
||| FrameFeedModel.cpp                                        ||
|||                                                           ||
|||   SPDX-License-Identifier: GPL-2.0-or-later               ||
|\*---------------------------------------------------------*/

#include "FrameFeedModel.h"

#include <QVariantMap>

namespace openrgb2
{
namespace ui
{

FrameFeedModel::FrameFeedModel(QObject* parent)
    : QAbstractListModel(parent)
{
}

int FrameFeedModel::rowCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : (int)rows_.size();
}

int FrameFeedModel::count() const
{
    return (int)rows_.size();
}

QVariant FrameFeedModel::data(const QModelIndex& index, int role) const
{
    if(!index.isValid() || index.row() < 0
       || index.row() >= (int)rows_.size())
    {
        return QVariant();
    }
    const Row& r = rows_[index.row()];
    switch(role)
    {
    case KeyRole:       return r.key;
    case ColorsRole:    return r.colors;
    case DeliveredRole: return r.delivered;
    default:            return QVariant();
    }
}

QHash<int, QByteArray> FrameFeedModel::roleNames() const
{
    return {
        { KeyRole,       "key" },
        { ColorsRole,    "colors" },
        { DeliveredRole, "delivered" },
    };
}

FrameFeedModel::Row FrameFeedModel::FromVariant(const QVariant& v)
{
    Row r;
    const QVariantMap m = v.toMap();
    r.key       = m.value(QStringLiteral("key"));
    r.colors    = m.value(QStringLiteral("colors")).toList();
    r.delivered = m.value(QStringLiteral("delivered"));
    return r;
}

void FrameFeedModel::apply(const QVariantList& controllers)
{
    const int old_n = (int)rows_.size();
    const int new_n = (int)controllers.size();
    const int common = qMin(old_n, new_n);

    for(int i = 0; i < common; ++i)
    {
        Row nr = FromVariant(controllers[i]);
        Row& cur = rows_[i];
        QList<int> roles;
        if(cur.key != nr.key)
        {
            roles << KeyRole;
        }
        if(cur.colors != nr.colors)
        {
            roles << ColorsRole;
        }
        if(cur.delivered != nr.delivered)
        {
            roles << DeliveredRole;
        }
        if(!roles.isEmpty())
        {
            cur = std::move(nr);
            const QModelIndex ix = index(i);
            emit dataChanged(ix, ix, roles);
        }
    }

    if(new_n > old_n)
    {
        beginInsertRows(QModelIndex(), old_n, new_n - 1);
        for(int i = old_n; i < new_n; ++i)
        {
            rows_.push_back(FromVariant(controllers[i]));
        }
        endInsertRows();
        emit countChanged();
    }
    else if(new_n < old_n)
    {
        beginRemoveRows(QModelIndex(), new_n, old_n - 1);
        rows_.resize(new_n);
        endRemoveRows();
        emit countChanged();
    }
}

QVariantMap FrameFeedModel::get(int row) const
{
    QVariantMap out;
    if(row < 0 || row >= (int)rows_.size())
    {
        return out;
    }
    const Row& r = rows_[row];
    out.insert(QStringLiteral("key"), r.key);
    out.insert(QStringLiteral("colors"), r.colors);
    out.insert(QStringLiteral("delivered"), r.delivered);
    return out;
}

} /* namespace ui */
} /* namespace openrgb2 */
