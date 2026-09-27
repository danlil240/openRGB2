/*---------------------------------------------------------*\
|||| ProfileLibrary.cpp                                        ||
||||                                                           ||
||||   SPDX-License-Identifier: GPL-2.0-or-later               ||
||\*---------------------------------------------------------*/

#include "ProfileLibrary.h"

#include "JsonVariant.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QStandardPaths>

namespace openrgb2
{
namespace ui
{

namespace
{

constexpr int      kSchemaVersion = 1;
constexpr size_t   kMaxEntries    = 64;
constexpr qint64   kMaxFileBytes  = 4 * 1024 * 1024;  /* bound */
constexpr size_t   kMaxNameChars  = 128;

} /* anonymous namespace */

ProfileLibrary::ProfileLibrary(QObject* parent)
    : QObject(parent)
{
    const QString base =
        QStandardPaths::writableLocation(
            QStandardPaths::AppDataLocation);
    path_ = (base.isEmpty() ? QDir::tempPath() : base) +
            QStringLiteral("/scene-library.json");
}

QString ProfileLibrary::filePath() const { return path_; }
QString ProfileLibrary::lastError() const { return last_error_; }

void ProfileLibrary::setPathForTesting(const QString& path)
{
    path_   = path;
    loaded_ = false;
    scenes_ = nlohmann::json::array();
    SetError(QString());
}

void ProfileLibrary::SetError(const QString& e)
{
    if(last_error_ == e)
    {
        return;
    }
    last_error_ = e;
    emit lastErrorChanged();
}

/*---------------------------------------------------------*\
||| Load                                                    ||
\*---------------------------------------------------------*/

void ProfileLibrary::EnsureLoaded()
{
    if(loaded_)
    {
        return;
    }
    loaded_ = true;
    scenes_ = nlohmann::json::array();

    QFile f(path_);
    if(!f.exists())
    {
        return;                    /* empty library, not an error */
    }
    if(f.size() > kMaxFileBytes)
    {
        SetError(QStringLiteral("scene library exceeds the %1 "
                                "MiB bound — left untouched")
                     .arg(kMaxFileBytes / (1024 * 1024)));
        return;
    }
    if(!f.open(QIODevice::ReadOnly))
    {
        SetError(QStringLiteral("scene library unreadable: %1")
                     .arg(f.errorString()));
        return;
    }
    const QByteArray raw = f.readAll();
    f.close();

    nlohmann::json j = nlohmann::json::parse(
        raw.constData(), nullptr, false);
    if(!j.is_object() || !j.contains("scenes") ||
       !j["scenes"].is_array())
    {
        /* Corrupt file: preserve it for forensics (.rejected),
           report honestly, start empty — the pattern the
           engine's own profile store uses at a smaller scale. */
        const QString aside = path_ + QStringLiteral(".rejected");
        QFile::remove(aside);
        QFile::rename(path_, aside);
        SetError(QStringLiteral("scene library was corrupt — set "
                                "aside as %1").arg(aside));
        return;
    }
    /* Entries are trusted only as far as SHAPE: name:string +
       document:object. Scene-field validation/migration stays
       with the engine's loader on SET_SCENE. */
    for(const nlohmann::json& e : j["scenes"])
    {
        if(e.is_object() && e.contains("name") &&
           e["name"].is_string() && e.contains("document") &&
           e["document"].is_object())
        {
            scenes_.push_back(e);
        }
    }
}

/*---------------------------------------------------------*\
||| Persist — bounded atomic write                          ||
\*---------------------------------------------------------*/

bool ProfileLibrary::Persist(QString& err)
{
    QFileInfo info(path_);
    QDir().mkpath(info.absolutePath());

    const nlohmann::json root = nlohmann::json::object({
        { "schema_version", kSchemaVersion },
        { "app",            "openrgb2-ui"  },
        { "scenes",         scenes_        },
    });
    const std::string text = root.dump(2);

    QSaveFile out(path_);
    if(!out.open(QIODevice::WriteOnly | QIODevice::Truncate))
    {
        err = QStringLiteral("cannot write %1: %2")
                  .arg(path_, out.errorString());
        return false;
    }
    out.write(text.data(), (qint64)text.size());
    if(!out.commit())
    {
        err = QStringLiteral("commit failed for %1: %2")
                  .arg(path_, out.errorString());
        return false;
    }
    return true;
}

/*---------------------------------------------------------*\
||| Entries                                                 ||
\*---------------------------------------------------------*/

QVariantList ProfileLibrary::entries()
{
    EnsureLoaded();
    QVariantList out;
    for(const nlohmann::json& e : scenes_)
    {
        QVariantMap m;
        m.insert(QStringLiteral("name"),
                 QString::fromStdString(
                     e.value("name", std::string())));
        m.insert(QStringLiteral("saved_at"),
                 QString::fromStdString(
                     e.value("saved_at", std::string())));
        out.push_back(m);
    }
    return out;
}

bool ProfileLibrary::saveProfile(const QString& name,
                                 const QVariantMap& document)
{
    const QString trimmed = name.trimmed();
    if(trimmed.isEmpty())
    {
        SetError(QStringLiteral("profile name is empty"));
        return false;
    }
    if(trimmed.size() > (qsizetype)kMaxNameChars)
    {
        SetError(QStringLiteral("profile name over %1 characters")
                     .arg(kMaxNameChars));
        return false;
    }
    const nlohmann::json doc = VariantToJson(document);
    if(!doc.is_object())
    {
        SetError(QStringLiteral("no scene document to store"));
        return false;
    }
    EnsureLoaded();

    /* Replace same-name (profiles are keyed by name). */
    for(auto it = scenes_.begin(); it != scenes_.end(); ++it)
    {
        if(it->value("name", std::string()) ==
           trimmed.toStdString())
        {
            scenes_.erase(it);
            break;
        }
    }
    if(scenes_.size() >= kMaxEntries)
    {
        SetError(QStringLiteral("scene library is full (%1 "
                                "profiles) — delete one first")
                     .arg(kMaxEntries));
        return false;
    }

    scenes_.push_back(nlohmann::json::object({
        { "name",     trimmed.toStdString() },
        { "saved_at",
          QDateTime::currentDateTime()
              .toString(Qt::ISODate).toStdString() },
        { "document", doc },
    }));

    QString err;
    if(!Persist(err))
    {
        /* Keep the store consistent with the file. */
        scenes_.erase(std::prev(scenes_.end()));
        SetError(err);
        return false;
    }
    SetError(QString());
    emit entriesChanged();
    return true;
}

QVariantMap ProfileLibrary::profileDocument(const QString& name)
{
    EnsureLoaded();
    for(const nlohmann::json& e : scenes_)
    {
        if(e.value("name", std::string()) == name.toStdString())
        {
            return JsonToVariant(e["document"]).toMap();
        }
    }
    return QVariantMap();
}

bool ProfileLibrary::removeProfile(const QString& name)
{
    EnsureLoaded();
    bool found = false;
    for(auto it = scenes_.begin(); it != scenes_.end(); ++it)
    {
        if(it->value("name", std::string()) == name.toStdString())
        {
            scenes_.erase(it);
            found = true;
            break;
        }
    }
    if(!found)
    {
        return false;
    }
    QString err;
    if(!Persist(err))
    {
        SetError(err);
        return false;
    }
    SetError(QString());
    emit entriesChanged();
    return true;
}

} /* namespace ui */
} /* namespace openrgb2 */
