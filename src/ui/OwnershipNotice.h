// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
/*---------------------------------------------------------*\
||| OwnershipNotice — R-10 competing-writer notices          ||
|||                                                          ||
||| ConflictNoticeTracker decides when the tray should say   ||
||| "another app is controlling your lights": once per       ||
||| writer key per UI session. A writer that stays, or that  ||
||| clears and comes back, never re-notifies — the in-window ||
||| banner covers the rest. Keys come from the engine        ||
||| (OwnerFinding::key); a keyless finding is keyed by its   ||
||| label. No UI-side re-classification.                     ||
\*---------------------------------------------------------*/

#include <QObject>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVariantMap>

namespace openrgb2::ui
{

class ConflictNoticeTracker
{
public:
    /* `ownership` is the OWNERSHIP body. Returns the owner labels
       of conflicts whose key was not seen before in this session
       (one per key), and marks every current key seen. */
    QStringList NewConflicts(const QVariantMap& ownership);

private:
    QSet<QString> seen_;
};

/* The startup takeover dialog's conflict block. `lines` are the
   script's "ownership CONFLICT: <owner> (...) -- <detail>
   [key: <key>]" lines (ParseOwnershipConflict). Lists every line
   without its key tag, then one short "how to turn it off" hint
   per distinct key — the engine is not running yet, so the
   Devices page cannot show its steps. Lines from an older script
   (no key tag) are listed with no hint. */
QString TakeoverConflictText(const QStringList& lines);

} /* namespace openrgb2::ui */
