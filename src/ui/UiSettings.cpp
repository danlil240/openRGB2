/*---------------------------------------------------------*\
|||| UiSettings.cpp                                            ||
||||                                                           ||
||||   SPDX-License-Identifier: GPL-2.0-or-later               ||
||\*---------------------------------------------------------*/

#include "UiSettings.h"

#include <QByteArray>
#include <QDir>
#include <QFileInfo>
#include <QSettings>

namespace openrgb2
{
namespace ui
{

UiSettings::UiSettings(QObject* parent)
    : QObject(parent)
{
}

UiSettings::~UiSettings() = default;

void UiSettings::setPathForTesting(const QString& iniPath)
{
    /* Re-bind the backing store; anything already read
       re-reads from the new path on next access. */
    settings_.reset();
    path_override_ = iniPath;
    emit prefsChanged();
}

QSettings* UiSettings::Ensure()
{
    if(settings_)
    {
        return settings_.get();
    }
    if(!path_override_.isEmpty())
    {
        settings_ = std::make_unique<QSettings>(
            path_override_, QSettings::IniFormat);
        return settings_.get();
    }
    /* The platform user store — org/app identity is set by
       main.cpp; the explicit ctor keeps tests that forgot
       setPathForTesting from silently sharing the real
       user's keys. */
    settings_ = std::make_unique<QSettings>(
        QStringLiteral("OpenRGB2"), QStringLiteral("OpenRGB2"));
    return settings_.get();
}

/*---------------------------------------------------------*\
|||| Shell prefs                                             ||
\*---------------------------------------------------------*/

int UiSettings::lastView() const
{
    return const_cast<UiSettings*>(this)->Ensure()
        ->value(QStringLiteral("ui/lastView"), 0).toInt();
}

void UiSettings::setLastView(int v)
{
    const int c = v < 0 ? 0 : (v > 4 ? 4 : v);
    if(c == lastView())
    {
        return;
    }
    Ensure()->setValue(QStringLiteral("ui/lastView"), c);
    emit prefsChanged();
}

bool UiSettings::restoreLastView() const
{
    return const_cast<UiSettings*>(this)->Ensure()
        ->value(QStringLiteral("ui/restoreLastView"), true)
        .toBool();
}

void UiSettings::setRestoreLastView(bool on)
{
    if(on == restoreLastView())
    {
        return;
    }
    Ensure()->setValue(QStringLiteral("ui/restoreLastView"),
                       on);
    emit prefsChanged();
}

int UiSettings::windowX() const
{
    return const_cast<UiSettings*>(this)->Ensure()
        ->value(QStringLiteral("ui/windowX"), -1).toInt();
}

int UiSettings::windowY() const
{
    return const_cast<UiSettings*>(this)->Ensure()
        ->value(QStringLiteral("ui/windowY"), -1).toInt();
}

int UiSettings::windowWidth() const
{
    return const_cast<UiSettings*>(this)->Ensure()
        ->value(QStringLiteral("ui/windowWidth"), 0).toInt();
}

int UiSettings::windowHeight() const
{
    return const_cast<UiSettings*>(this)->Ensure()
        ->value(QStringLiteral("ui/windowHeight"), 0).toInt();
}

bool UiSettings::hasWindowPosition() const
{
    QSettings* s = const_cast<UiSettings*>(this)->Ensure();
    return s->contains(QStringLiteral("ui/windowX"))
           && s->contains(QStringLiteral("ui/windowY"));
}

bool UiSettings::windowMaximized() const
{
    return const_cast<UiSettings*>(this)->Ensure()
        ->value(QStringLiteral("ui/windowMaximized"), false)
        .toBool();
}

void UiSettings::setWindowMaximized(bool maximized)
{
    if(maximized == windowMaximized())
    {
        return;
    }
    Ensure()->setValue(QStringLiteral("ui/windowMaximized"),
                       maximized);
    emit prefsChanged();
}

void UiSettings::saveGeometry(int x, int y, int w, int h)
{
    QSettings* s = Ensure();
    s->setValue(QStringLiteral("ui/windowX"),      x);
    s->setValue(QStringLiteral("ui/windowY"),      y);
    s->setValue(QStringLiteral("ui/windowWidth"),  w);
    s->setValue(QStringLiteral("ui/windowHeight"), h);
    emit prefsChanged();
}

double UiSettings::paletteHue() const
{
    return const_cast<UiSettings*>(this)->Ensure()
        ->value(QStringLiteral("ui/paletteHue"), 0.55)
        .toDouble();
}

void UiSettings::setPaletteHue(double v)
{
    Ensure()->setValue(QStringLiteral("ui/paletteHue"), v);
    emit prefsChanged();
}

double UiSettings::paletteSat() const
{
    return const_cast<UiSettings*>(this)->Ensure()
        ->value(QStringLiteral("ui/paletteSat"), 0.6)
        .toDouble();
}

void UiSettings::setPaletteSat(double v)
{
    Ensure()->setValue(QStringLiteral("ui/paletteSat"), v);
    emit prefsChanged();
}

double UiSettings::paletteVal() const
{
    return const_cast<UiSettings*>(this)->Ensure()
        ->value(QStringLiteral("ui/paletteVal"), 1.0)
        .toDouble();
}

void UiSettings::setPaletteVal(double v)
{
    Ensure()->setValue(QStringLiteral("ui/paletteVal"), v);
    emit prefsChanged();
}

QString UiSettings::storageLocation() const
{
    QSettings* s = const_cast<UiSettings*>(this)->Ensure();
    const QString f = s->fileName();
    return f.isEmpty() ? QStringLiteral("platform store")
                       : f;
}

void UiSettings::clear()
{
    Ensure()->clear();
    emit prefsChanged();
}

/*---------------------------------------------------------*\
||| 3D viewport shell prefs (studio-import Task 8)         ||
\*---------------------------------------------------------*/

QString UiSettings::viewportQuality() const
{
    const QString q = const_cast<UiSettings*>(this)->Ensure()
        ->value(QStringLiteral("ui/viewportQuality"),
                QStringLiteral("balanced")).toString();
    return (q == QLatin1String("low") || q == QLatin1String("high"))
           ? q : QStringLiteral("balanced");
}

void UiSettings::setViewportQuality(const QString& q)
{
    if(q != QLatin1String("low") && q != QLatin1String("balanced")
       && q != QLatin1String("high"))
    {
        return;
    }
    if(q == viewportQuality())
    {
        return;
    }
    Ensure()->setValue(QStringLiteral("ui/viewportQuality"), q);
    emit prefsChanged();
}

bool UiSettings::viewportBloom() const
{
    return const_cast<UiSettings*>(this)->Ensure()
        ->value(QStringLiteral("ui/viewportBloom"), true).toBool();
}

void UiSettings::setViewportBloom(bool on)
{
    if(on == viewportBloom())
    {
        return;
    }
    Ensure()->setValue(QStringLiteral("ui/viewportBloom"), on);
    emit prefsChanged();
}

bool UiSettings::viewportReducedMotion() const
{
    return const_cast<UiSettings*>(this)->Ensure()
        ->value(QStringLiteral("ui/viewportReducedMotion"), false)
        .toBool();
}

void UiSettings::setViewportReducedMotion(bool on)
{
    if(on == viewportReducedMotion())
    {
        return;
    }
    Ensure()->setValue(QStringLiteral("ui/viewportReducedMotion"),
                       on);
    emit prefsChanged();
}

bool UiSettings::viewportCaseGhost() const
{
    return const_cast<UiSettings*>(this)->Ensure()
        ->value(QStringLiteral("ui/viewportCaseGhost"), false)
        .toBool();
}

void UiSettings::setViewportCaseGhost(bool on)
{
    if(on == viewportCaseGhost())
    {
        return;
    }
    Ensure()->setValue(QStringLiteral("ui/viewportCaseGhost"), on);
    emit prefsChanged();
}

bool UiSettings::layoutShow3d() const
{
    /* The 2D overview stays the DEFAULT — the 3D viewport is an
       opt-in the user picks once and the shell remembers. */
    return const_cast<UiSettings*>(this)->Ensure()
        ->value(QStringLiteral("ui/layoutShow3d"), false).toBool();
}

void UiSettings::setLayoutShow3d(bool on)
{
    if(on == layoutShow3d())
    {
        return;
    }
    Ensure()->setValue(QStringLiteral("ui/layoutShow3d"), on);
    emit prefsChanged();
}

/*---------------------------------------------------------*\
|||| Layout editor splitter sizes                            ||
||||   0 = never set (the page picks its default). Clamped   ||
||||   so a corrupt store can't open a rail wider than any   ||
||||   screen; the page re-clamps to the live window.        ||
\*---------------------------------------------------------*/
namespace
{
constexpr int kMaxSplitPx = 4000;
int ClampSplit(int px)
{
    return px < 0 ? 0 : (px > kMaxSplitPx ? kMaxSplitPx : px);
}
}

int UiSettings::layoutRailWidth() const
{
    return ClampSplit(const_cast<UiSettings*>(this)->Ensure()
        ->value(QStringLiteral("ui/layoutRailWidth"), 0).toInt());
}

void UiSettings::setLayoutRailWidth(int px)
{
    px = ClampSplit(px);
    if(px == layoutRailWidth())
    {
        return;
    }
    Ensure()->setValue(QStringLiteral("ui/layoutRailWidth"), px);
    emit prefsChanged();
}

int UiSettings::layoutInspectorHeight() const
{
    return ClampSplit(const_cast<UiSettings*>(this)->Ensure()
        ->value(QStringLiteral("ui/layoutInspectorHeight"), 0).toInt());
}

void UiSettings::setLayoutInspectorHeight(int px)
{
    px = ClampSplit(px);
    if(px == layoutInspectorHeight())
    {
        return;
    }
    Ensure()->setValue(QStringLiteral("ui/layoutInspectorHeight"), px);
    emit prefsChanged();
}

/*---------------------------------------------------------*\
|||| Per-scene uploaded-preset params                        ||
\*---------------------------------------------------------*/

QString UiSettings::SceneKey(const QString& sceneName)
{
    /* Scene names are user text — base64 the utf-8 so any
       characters survive the settings key namespace. */
    return QStringLiteral("sceneParams/") +
           QString::fromLatin1(
               sceneName.toUtf8().toBase64());
}

QVariantMap UiSettings::SceneParams(const QString& sceneName)
{
    if(sceneName.isEmpty())
    {
        return QVariantMap();
    }
    return Ensure()->value(SceneKey(sceneName)).toMap();
}

void UiSettings::SetSceneParams(const QString& sceneName,
                                const QVariantMap& params)
{
    if(sceneName.isEmpty())
    {
        return;
    }
    QSettings* s = Ensure();
    const QString key = SceneKey(sceneName);
    s->setValue(key, params);

    /* Bounded LRU-ish index — the store stays finite
       regardless of how many scene names the user cycles
       through (plan section 6 bounds). */
    QStringList idx =
        s->value(QStringLiteral("sceneParamsIndex"))
            .toStringList();
    const QString tag =
        QString::fromLatin1(sceneName.toUtf8().toBase64());
    idx.removeAll(tag);
    idx.append(tag);
    while(idx.size() > kSceneParamBound)
    {
        const QString evict =
            QStringLiteral("sceneParams/") + idx.first();
        s->remove(evict);
        idx.removeFirst();
    }
    s->setValue(QStringLiteral("sceneParamsIndex"), idx);
}

} /* namespace ui */
} /* namespace openrgb2 */
