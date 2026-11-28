/*---------------------------------------------------------*\
||| TrayController.cpp — see TrayController.h (R-1).          ||
|||                                                           ||
|||   SPDX-License-Identifier: GPL-2.0-or-later               ||
|\*---------------------------------------------------------*/

#include "TrayController.h"

#include "EngineBridge.h"

#include <QMetaObject>
#include <QStringList>
#include <QtMath>

namespace openrgb2
{
namespace ui
{

namespace
{

QVariantMap Entry(const QString& id, const QString& kind,
                  const QString& text, bool enabled,
                  bool checkable = false, bool checked = false)
{
    return QVariantMap({
        { QStringLiteral("id"),        id        },
        { QStringLiteral("kind"),      kind      },
        { QStringLiteral("text"),      text      },
        { QStringLiteral("enabled"),   enabled   },
        { QStringLiteral("checkable"), checkable },
        { QStringLiteral("checked"),   checked   },
    });
}

QVariantMap Separator()
{
    return Entry(QString(), QStringLiteral("separator"), QString(),
                 false);
}

} /* anonymous namespace */

const QList<int>& TrayController::Presets()
{
    static const QList<int> presets = { 100, 75, 50, 25 };
    return presets;
}

TrayController::TrayController(EngineBridge* bridge, QObject* workspace,
                               QObject* parent)
    : QObject(parent), bridge_(bridge), workspace_(workspace)
{
    if(bridge_)
    {
        connect(bridge_, &EngineBridge::connectionStateChanged,
                this, &TrayController::Rebuild);
        connect(bridge_, &EngineBridge::outputChanged,
                this, &TrayController::Rebuild);
        connect(bridge_, &EngineBridge::hasOutputControlChanged,
                this, &TrayController::Rebuild);
    }
    if(workspace_)
    {
        /* Duck-typed like the QML bindings: subscribe to the
           change signals the object actually declares. */
        const QMetaObject* mo = workspace_->metaObject();
        const int rebuild = metaObject()->indexOfMethod(
            QMetaObject::normalizedSignature("Rebuild()"));
        for(const char* sig : { "availabilityChanged()",
                                "workspaceListChanged()",
                                "workspaceChanged()",
                                "replaceChanged()" })
        {
            const int idx = mo->indexOfSignal(
                QMetaObject::normalizedSignature(sig));
            if(idx >= 0 && rebuild >= 0)
            {
                QMetaObject::connect(workspace_, idx, this, rebuild);
            }
        }
    }
    Rebuild();
}

QVariantList TrayController::menu() const   { return menu_; }
QString      TrayController::toolTip() const { return tooltip_; }

bool TrayController::Connected() const
{
    return bridge_ && bridge_->connected();
}

/* Hidden only when a CONNECTED engine lacks the capability —
   before the first connection (and while disconnected) the
   entries stay visible, disabled, so the menu does not jump. */
bool TrayController::OutputShown() const
{
    return !Connected() || (bridge_ && bridge_->hasOutputControl());
}

bool TrayController::OutputEnabled() const
{
    return Connected() && bridge_->hasOutputControl();
}

QVariant TrayController::WsProp(const char* name) const
{
    return workspace_ ? workspace_->property(name) : QVariant();
}

void TrayController::Rebuild()
{
    const bool        connected = Connected();
    const QVariantMap out = bridge_ ? bridge_->output() : QVariantMap();
    const bool  blackout = out.value(QStringLiteral("blackout")).toBool();
    const bool  hold     = out.value(QStringLiteral("hold")).toBool();
    const bool  has_master = out.contains(QStringLiteral("master"));
    const int   pct = qRound(
        out.value(QStringLiteral("master"), 1.0).toDouble() * 100.0);
    const bool  uploaded =
        out.value(QStringLiteral("uploaded_unscaled")).toBool();

    QVariantList menu;

    /*---- Workspaces ------------------------------------------*/
    {
        const bool ws_on = connected && workspace_ &&
                           WsProp("available").toBool();
        const QString current = WsProp("workspaceId").toString();
        QVariantList children;
        for(const QVariant& v : WsProp("workspaceList").toList())
        {
            const QString id = v.toString();
            if(id.isEmpty())
            {
                continue;
            }
            children << Entry(QStringLiteral("workspace:") + id,
                              QStringLiteral("action"), id, ws_on,
                              true, id == current);
        }
        if(children.isEmpty())
        {
            children << Entry(QStringLiteral("workspace:none"),
                              QStringLiteral("action"),
                              tr("No workspaces"), false);
        }
        QVariantMap sub = Entry(QStringLiteral("workspaces"),
                                QStringLiteral("submenu"),
                                tr("Workspace"), ws_on);
        sub.insert(QStringLiteral("children"), children);
        menu << sub;
    }

    if(OutputShown())
    {
        const bool on = OutputEnabled();
        /*---- Brightness --------------------------------------*/
        QVariantList children;
        for(int p : Presets())
        {
            children << Entry(
                QStringLiteral("brightness:%1").arg(p),
                QStringLiteral("action"),
                QStringLiteral("%1%").arg(p), on, true,
                has_master && !blackout && pct == p);
        }
        children << Separator();
        children << Entry(QStringLiteral("brightness:off"),
                          QStringLiteral("action"), tr("Off"), on,
                          true, blackout);
        if(uploaded)
        {
            children << Entry(QStringLiteral("brightness:note"),
                              QStringLiteral("action"),
                              tr("Uploaded loop: brightness not "
                                 "applied"), false);
        }
        QString title = tr("Brightness");
        if(on && has_master)
        {
            title += blackout ? tr(" (Off)")
                              : QStringLiteral(" (%1%)").arg(pct);
        }
        QVariantMap sub = Entry(QStringLiteral("brightness"),
                                QStringLiteral("submenu"), title, on);
        sub.insert(QStringLiteral("children"), children);
        menu << sub;

        /*---- Pause / Resume ----------------------------------*/
        menu << Entry(QStringLiteral("pause"), QStringLiteral("action"),
                      hold ? tr("Resume lighting")
                           : tr("Pause lighting"),
                      on);
    }

    /*---- Tooltip ----------------------------------------------*/
    QStringList tip{ QStringLiteral("OpenRGB2") };
    if(!connected)
    {
        tip << tr("Engine not connected");
    }
    else
    {
        QString ws = WsProp("workspaceName").toString();
        if(ws.isEmpty())
        {
            ws = WsProp("workspaceId").toString();
        }
        if(!ws.isEmpty())
        {
            tip << tr("Workspace: %1").arg(ws);
        }
        if(OutputEnabled() && has_master)
        {
            tip << (blackout ? tr("Brightness: Off")
                             : tr("Brightness: %1%").arg(pct));
            if(hold)
            {
                tip << tr("Paused");
            }
            /* R-4: the strongest active dimming rule (an OUTPUT
               contributor target) says why the lights are low. */
            const QVariantMap contrib =
                out.value(QStringLiteral("contributors")).toMap();
            QString strongest;
            double  level = 2.0;
            for(auto it = contrib.cbegin(); it != contrib.cend(); ++it)
            {
                const double v = it.value().toDouble();
                if(v < level)
                {
                    level     = v;
                    strongest = it.key();
                }
            }
            if(!strongest.isEmpty() && !blackout)
            {
                const QString why =
                    strongest == QLatin1String("lock")
                        ? tr("while locked")
                    : strongest == QLatin1String("idle")
                        ? tr("while idle")
                    : strongest == QLatin1String("schedule")
                        ? tr("by schedule")
                        : tr("by %1").arg(strongest);
                tip << (level <= 0.0
                            ? tr("Off %1").arg(why)
                            : tr("Dimmed %1 (%2%)")
                                  .arg(why)
                                  .arg(qRound(level * 100.0)));
            }
        }
    }
    const QString tooltip = tip.join(QLatin1Char('\n'));

    if(menu == menu_ && tooltip == tooltip_)
    {
        return;
    }
    menu_    = menu;
    tooltip_ = tooltip;
    emit menuChanged();
}

void TrayController::trigger(const QString& id)
{
    if(!Connected())
    {
        return;
    }
    if(id.startsWith(QStringLiteral("workspace:")))
    {
        if(!workspace_ || !WsProp("available").toBool())
        {
            return;
        }
        const QString ws = id.mid(int(sizeof("workspace:")) - 1);
        if(ws.isEmpty() || ws == QStringLiteral("none") ||
           ws == WsProp("workspaceId").toString())
        {
            return;
        }
        QMetaObject::invokeMethod(workspace_, "openWorkspace",
                                  Q_ARG(QString, ws));
        /* A dirty draft is never replaced silently: the switch
           latched the confirmation, which lives in the window. */
        if(WsProp("replaceOffered").toBool())
        {
            emit showWindowRequested();
        }
        return;
    }

    if(!OutputEnabled())
    {
        return;
    }
    const QVariantMap out = bridge_->output();
    const bool blackout = out.value(QStringLiteral("blackout")).toBool();
    if(id == QStringLiteral("pause"))
    {
        bridge_->outputCommand(QStringLiteral("set_hold"),
            QVariantMap({{ QStringLiteral("on"),
                           !out.value(QStringLiteral("hold")).toBool() }}));
        return;
    }
    if(id == QStringLiteral("brightness:off"))
    {
        /* A toggle: picking Off again turns the lights back on
           at the kept master. */
        bridge_->outputCommand(QStringLiteral("set_blackout"),
            QVariantMap({{ QStringLiteral("on"), !blackout }}));
        return;
    }
    if(id.startsWith(QStringLiteral("brightness:")))
    {
        bool ok = false;
        const int p = id.mid(int(sizeof("brightness:")) - 1).toInt(&ok);
        if(!ok || !Presets().contains(p))
        {
            return;
        }
        if(blackout)
        {
            /* Picking a level while Off means "on, at this level". */
            bridge_->outputCommand(QStringLiteral("set_blackout"),
                QVariantMap({{ QStringLiteral("on"), false }}));
        }
        bridge_->outputCommand(QStringLiteral("set_master"),
            QVariantMap({{ QStringLiteral("value"), p / 100.0 }}));
    }
}

} /* namespace ui */
} /* namespace openrgb2 */
