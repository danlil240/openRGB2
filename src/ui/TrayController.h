/*---------------------------------------------------------*\
||| TrayController.h                                          ||
|||                                                           ||
|||   R-1 tray quick controls. Builds the tray's quick-      ||
|||   control menu as DATA — main.cpp only renders it into   ||
|||   a QMenu around its own Open / Exit UI / Quit actions — ||
|||   so the whole surface is testable offscreen             ||
|||   (tests/ui/ui_tray_test).                               ||
|||                                                           ||
|||   Entries (in order):                                    ||
|||   - "workspaces"  submenu, one checkable child per       ||
|||                   managed workspace ("workspace:<id>")   ||
|||   - "brightness"  submenu: "brightness:100|75|50|25",    ||
|||                   a separator, "brightness:off"          ||
|||                   (+ a disabled "brightness:note" while  ||
|||                   an uploaded loop ignores the master)   ||
|||   - "pause"       Pause lighting / Resume lighting       ||
|||   Every entry is disabled while the engine is            ||
|||   disconnected; brightness + pause are omitted when a    ||
|||   connected engine lacks output_control_v1.              ||
|||                                                           ||
|||   Actions go through the engine-side OutputControl       ||
|||   (EngineBridge::outputCommand) — never the document, so ||
|||   no draft goes dirty. A workspace pick calls            ||
|||   WorkspaceController::openWorkspace; when that latches  ||
|||   the dirty-draft confirmation (replaceOffered) the tray ||
|||   asks for the window so the dialog is seen.             ||
|||                                                           ||
|||   The workspace side is reached through its Qt meta-     ||
|||   object (properties available / workspaceList /         ||
|||   workspaceId / workspaceName / replaceOffered, the      ||
|||   invokable openWorkspace) — exactly what QML binds —    ||
|||   so tests can substitute a small fake.                  ||
|||                                                           ||
|||   SPDX-License-Identifier: GPL-2.0-or-later               ||
|\*---------------------------------------------------------*/

#pragma once

#include <QObject>
#include <QPointer>
#include <QString>
#include <QVariantList>
#include <QVariantMap>

namespace openrgb2
{
namespace ui
{

class EngineBridge;

class TrayController : public QObject
{
    Q_OBJECT

    /* [{id, kind:"action"|"submenu"|"separator", text, enabled,
         checkable, checked, children:[...]}] */
    Q_PROPERTY(QVariantList menu READ menu NOTIFY menuChanged)
    Q_PROPERTY(QString toolTip READ toolTip NOTIFY menuChanged)

public:
    /* `workspace` may be null (no managed-workspace surface). */
    TrayController(EngineBridge* bridge, QObject* workspace,
                   QObject* parent = nullptr);

    QVariantList menu() const;
    QString      toolTip() const;

    /* Run the action behind a menu entry id. Unknown ids,
       disabled entries and submenus are ignored. */
    Q_INVOKABLE void trigger(const QString& id);

    /* The master presets, in menu order (percent). */
    static const QList<int>& Presets();

signals:
    void menuChanged();
    /* A workspace pick latched the dirty-draft confirmation —
       show the window so the dialog is visible. */
    void showWindowRequested();

private slots:
    /* A slot so the duck-typed workspace signals can reach it
       through QMetaObject::connect. */
    void Rebuild();

private:
    bool Connected() const;
    bool OutputShown() const;
    bool OutputEnabled() const;
    QVariant WsProp(const char* name) const;

    QPointer<EngineBridge> bridge_;
    QPointer<QObject>      workspace_;
    QVariantList           menu_;
    QString                tooltip_;
};

} /* namespace ui */
} /* namespace openrgb2 */
