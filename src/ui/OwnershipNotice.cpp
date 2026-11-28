// SPDX-License-Identifier: GPL-2.0-or-later
#include "OwnershipNotice.h"

#include <QList>
#include <QPair>
#include <QRegularExpression>
#include <QVariantList>

namespace openrgb2::ui
{

namespace
{

/* (key, owner) per conflict; a keyless finding is keyed by its
   label. */
QList<QPair<QString, QString>> ConflictKeys(const QVariantMap& ownership)
{
    QList<QPair<QString, QString>> out;
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
        out.append(qMakePair(key, owner));
    }
    return out;
}

} /* anonymous namespace */

QStringList ConflictNoticeTracker::Pending(const QVariantMap& ownership) const
{
    QStringList out;
    QSet<QString> listed;
    for(const auto& ko : ConflictKeys(ownership))
    {
        if(seen_.contains(ko.first) || listed.contains(ko.first))
        {
            continue;
        }
        listed.insert(ko.first);
        out << ko.second;
    }
    return out;
}

void ConflictNoticeTracker::MarkSeen(const QVariantMap& ownership)
{
    for(const auto& ko : ConflictKeys(ownership))
    {
        seen_.insert(ko.first);
    }
}

QStringList ConflictNoticeTracker::NewConflicts(const QVariantMap& ownership)
{
    const QStringList out = Pending(ownership);
    MarkSeen(ownership);
    return out;
}

namespace
{

QString HintForKey(const QString& key, const QString& owner)
{
    if(key == QLatin1String("lamparray"))
        return QObject::tr("LampArray service: stop it in Services "
                           "(logi_lamparray_service), or run "
                           "\"sc stop logi_lamparray_service\" as "
                           "administrator.");
    if(key == QLatin1String("logitech"))
        return QObject::tr("G HUB / Options+: quit it from the tray and "
                           "turn off \"Start G HUB on system startup\".");
    if(key == QLatin1String("dynamic_lighting"))
        return QObject::tr("Windows Dynamic Lighting: Settings > "
                           "Personalization > Dynamic Lighting > turn off "
                           "\"Use Dynamic Lighting on my devices\".");
    if(key == QLatin1String("openrgb"))
        return QObject::tr("Another OpenRGB: close it from its tray icon "
                           "or window.");
    return QObject::tr("%1: quit it, or turn off its lighting module.")
        .arg(owner);
}

} /* anonymous namespace */

QString TakeoverConflictText(const QStringList& lines)
{
    static const QRegularExpression kKeyTag(
        QStringLiteral("\\s*\\[key:\\s*([a-z0-9_]*)\\]\\s*$"));
    static const QRegularExpression kOwner(
        QStringLiteral("ownership CONFLICT:\\s*(.*?)\\s*\\(pid"));

    QString list;
    QStringList hints;
    QSet<QString> keys;
    for(const QString& raw : lines)
    {
        QString line = raw.trimmed();
        const QRegularExpressionMatch k = kKeyTag.match(line);
        QString key;
        if(k.hasMatch())
        {
            key  = k.captured(1);
            line = line.left(k.capturedStart()).trimmed();
        }
        list += QStringLiteral("  ") + line + QLatin1Char('\n');
        if(key.isEmpty() || keys.contains(key))
            continue;
        keys.insert(key);
        const QRegularExpressionMatch o = kOwner.match(line);
        hints << QStringLiteral("  - ") +
                 HintForKey(key, o.hasMatch() ? o.captured(1)
                                               : QObject::tr("the app"));
    }
    if(hints.isEmpty())
        return list;
    return list + QLatin1Char('\n') +
           QObject::tr("To turn them off:") + QLatin1Char('\n') +
           hints.join(QLatin1Char('\n')) + QLatin1Char('\n') +
           QObject::tr("Full steps: OpenRGB2 > Devices & Diagnostics > "
                       "Ownership scan, once lighting runs.") +
           QLatin1Char('\n');
}

} /* namespace openrgb2::ui */
