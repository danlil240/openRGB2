/*---------------------------------------------------------*\
||| FrameFeedModel.h                                          ||
|||                                                           ||
|||   The frame_status `controllers[]` array as a list model ||
|||   with STABLE rows: a push that keeps the same          ||
|||   controller count updates the rows in place            ||
|||   (dataChanged), so QML Repeaters keep their delegates  ||
|||   instead of destroying and recreating every swatch on  ||
|||   each ~30 Hz event. Rows are only inserted/removed when ||
|||   the controller count itself changes.                  ||
|||                                                           ||
|||   Roles: key (the controller key object), colors (the   ||
|||   "#RRGGBB" list, last transport-accepted frame),       ||
|||   delivered (bool, or undefined when the wire omits it). ||
|||                                                           ||
|||   SPDX-License-Identifier: GPL-2.0-or-later               ||
|\*---------------------------------------------------------*/

#pragma once

#include <QAbstractListModel>
#include <QVariant>
#include <QVariantList>
#include <QVector>

namespace openrgb2
{
namespace ui
{

class FrameFeedModel : public QAbstractListModel
{
    Q_OBJECT
    Q_PROPERTY(int count READ count NOTIFY countChanged)

public:
    enum Roles
    {
        KeyRole = Qt::UserRole + 1,
        ColorsRole,
        DeliveredRole,
    };

    explicit FrameFeedModel(QObject* parent = nullptr);

    int      rowCount(const QModelIndex& parent = QModelIndex())
                 const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    int count() const;

    /* Apply one frame_status `controllers` list. Rows present
       before and after are updated in place; only the tail is
       inserted/removed. Non-map entries become empty rows. */
    void apply(const QVariantList& controllers);

    /* Test/inspection helper: row -> {key, colors, delivered}. */
    Q_INVOKABLE QVariantMap get(int row) const;

signals:
    void countChanged();

private:
    struct Row
    {
        QVariant     key;
        QVariantList colors;
        QVariant     delivered;
    };
    static Row FromVariant(const QVariant& v);

    QVector<Row> rows_;
};

} /* namespace ui */
} /* namespace openrgb2 */
