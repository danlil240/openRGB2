// SPDX-License-Identifier: GPL-2.0-or-later
#include "OwnershipNotice.h"

#include <QVariantList>

namespace openrgb2::ui
{

QStringList ConflictNoticeTracker::NewConflicts(const QVariantMap& ownership)
{
    QStringList out;
    const QVariantList conflicts =
        ownership.value(QStringLiteral("conflicts")).toList();
    for(const QVariant& v : conflicts)
    {
        const QVariantMap c = v.toMap();
        const QString owner = c.value(QStringLiteral("owner")).toString();
        QString key = c.value(QStringLiteral("key")).toString();
        if(key.isEmpty())
        {
            key = QStringLiteral("label:") + owner;
        }
        if(seen_.contains(key))
        {
            continue;
        }
        seen_.insert(key);
        out << owner;
    }
    return out;
}

} /* namespace openrgb2::ui */
