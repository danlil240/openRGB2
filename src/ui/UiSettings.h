/*---------------------------------------------------------*\
|||| UiSettings.h                                              ||
||||                                                           ||
||||   UI-only preference persistence (Task 4.8, plan       ||
||||   section 5). The engine owns engine state — this     ||
||||   store exists ONLY for conveniences that are the     ||
||||   shell's own: window geometry, the last-opened       ||
||||   view, the palette picker's defaults, and the        ||
||||   per-scene uploaded-preset params the wire reports   ||
||||   as revisions (never values — the UI's last-sent     ||
||||   values are the only honest seed it has).            ||
||||                                                           ||
||||   Backed by QSettings (platform store — HKCU on       ||
||||   Windows; an INI path under test). Writes are        ||
||||   write-through and bounded; a corrupt/missing store  ||
||||   reads as defaults, never a crash or a fabricated    ||
||||   state.                                              ||
||||                                                           ||
||||   SPDX-License-Identifier: GPL-2.0-or-later               ||
||\*---------------------------------------------------------*/

#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariantMap>

#include <memory>

class QSettings;

namespace openrgb2
{
namespace ui
{

class UiSettings : public QObject
{
    Q_OBJECT

    /* Last-opened view index — restored on launch when
       restoreLastView is on (the shell's own preference;
       playback/scene state itself always comes back from the
       ENGINE, never from here). */
    Q_PROPERTY(int lastView READ lastView WRITE setLastView
                   NOTIFY prefsChanged)
    Q_PROPERTY(bool restoreLastView READ restoreLastView
                   WRITE setRestoreLastView NOTIFY prefsChanged)

    /* Last window geometry — -1 / 0 = unset (center + the
       declared default size). */
    Q_PROPERTY(int windowX READ windowX NOTIFY prefsChanged)
    Q_PROPERTY(int windowY READ windowY NOTIFY prefsChanged)
    Q_PROPERTY(int windowWidth READ windowWidth
                   NOTIFY prefsChanged)
    Q_PROPERTY(int windowHeight READ windowHeight
                   NOTIFY prefsChanged)

    /* Palette-picker defaults (0..1 HSV) — the picker's
       remembered position, purely cosmetic. */
    Q_PROPERTY(double paletteHue READ paletteHue
                   WRITE setPaletteHue NOTIFY prefsChanged)
    Q_PROPERTY(double paletteSat READ paletteSat
                   WRITE setPaletteSat NOTIFY prefsChanged)
    Q_PROPERTY(double paletteVal READ paletteVal
                   WRITE setPaletteVal NOTIFY prefsChanged)

    /* Where the store actually lives — the honest readout the
       Settings page shows. */
    Q_PROPERTY(QString storageLocation READ storageLocation
                   NOTIFY prefsChanged)

public:
    explicit UiSettings(QObject* parent = nullptr);
    ~UiSettings() override;

    /*---- shell prefs -----------------------------------------*/
    int      lastView() const;
    void     setLastView(int v);
    bool     restoreLastView() const;
    void     setRestoreLastView(bool on);
    int      windowX() const;
    int      windowY() const;
    int      windowWidth() const;
    int      windowHeight() const;
    Q_INVOKABLE void saveGeometry(int x, int y, int w, int h);
    double   paletteHue() const;
    void     setPaletteHue(double v);
    double   paletteSat() const;
    void     setPaletteSat(double v);
    double   paletteVal() const;
    void     setPaletteVal(double v);
    QString  storageLocation() const;

    /* Drop every stored preference — the next launch reads
       defaults. Engine state is untouched (it was never
       here). */
    Q_INVOKABLE void clear();

    /*---- per-scene uploaded-preset params ---------------------
       {speed,reverse,color,touched_speed,touched_reverse,
        touched_color} keyed by scene name — the same key the
       engine uses for its own per-scene record. Values are
       the UI's last-SENT ones; they only ever re-ride a
       playback request for the field the user touched.
       Bounded to kSceneParamBound entries, oldest evicted. */
    static constexpr int kSceneParamBound = 64;
    QVariantMap SceneParams(const QString& sceneName);
    void        SetSceneParams(const QString& sceneName,
                               const QVariantMap& params);

    /*---- test hook (call before first use) -------------------*/
    void setPathForTesting(const QString& iniPath);

signals:
    void prefsChanged();

private:
    QSettings* Ensure();
    static QString SceneKey(const QString& sceneName);

    std::unique_ptr<QSettings> settings_;
    QString                    path_override_;
};

} /* namespace ui */
} /* namespace openrgb2 */
