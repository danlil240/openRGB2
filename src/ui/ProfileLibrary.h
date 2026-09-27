/*---------------------------------------------------------*\
|||| ProfileLibrary.h                                          ||
||||                                                           ||
||||   The UI's named-scene library (Task 4.2). The wire    ||
||||   protocol exposes no profile-enumeration verb — the  ||
||||   engine's own profile (profiles/local/engine-state.)||
||||   holds per-scene playback selections + resolved     ||
||||   binding identities, keyed by scene name, and is     ||
||||   restored automatically through SET_SCENE. So named||
||||   scene DOCUMENTS live in this UI-side store:        ||
||||   save snapshots the working document under a name,  ||
||||   load hands the stored document back for staging    ||
||||   through SceneEditController -> SET_SCENE — the     ||
||||   engine's loader still validates and migrates on    ||
||||   apply (migrate-or-preserve stays engine-owned;     ||
||||   nothing here parses or upgrades scene fields).     ||
||||                                                           ||
||||   Bounds (plan section 6): 64 named scenes, one      ||
||||   bounded JSON file, atomic temp+rename writes, and  ||
||||   a corrupt file is set aside (.rejected) and        ||
||||   reported — never silently guessed.                 ||
||||                                                           ||
||||   SPDX-License-Identifier: GPL-2.0-or-later               ||
||\*---------------------------------------------------------*/

#pragma once

#include <nlohmann/json.hpp>

#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>

namespace openrgb2
{
namespace ui
{

class ProfileLibrary : public QObject
{
    Q_OBJECT

    /* [{name, saved_at}] — newest first. */
    Q_PROPERTY(QVariantList entries READ entries
                   NOTIFY entriesChanged)
    Q_PROPERTY(QString lastError READ lastError
                   NOTIFY lastErrorChanged)

public:
    explicit ProfileLibrary(QObject* parent = nullptr);

    /*---- store --------------------------------------------*/
    /* Save `document` (a SceneDocument QVariantMap) under
       `name`. Replaces a same-named entry. False + lastError
       on a bound or I/O failure — the store is unchanged. */
    Q_INVOKABLE bool saveProfile(const QString& name,
                                 const QVariantMap& document);
    /* The stored document, or an invalid map when absent —
       callers stage it through SET_SCENE; the engine decides
       whether it still loads (migration is engine-side). */
    Q_INVOKABLE QVariantMap profileDocument(const QString& name);
    Q_INVOKABLE bool        removeProfile(const QString& name);

    QVariantList entries();
    QString      lastError() const;
    QString      filePath() const;

    /*---- test hooks (call before first use) ----------------*/
    void setPathForTesting(const QString& path);

signals:
    void entriesChanged();
    void lastErrorChanged();

private:
    void EnsureLoaded();
    bool Persist(QString& err);
    void SetError(const QString& e);

    QString     path_;
    bool        loaded_  = false;
    QString     last_error_;
    /* Ordered store — insertion order is display order. */
    nlohmann::json scenes_ = nlohmann::json::array();
};

} /* namespace ui */
} /* namespace openrgb2 */
