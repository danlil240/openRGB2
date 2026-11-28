// SPDX-License-Identifier: GPL-2.0-or-later
//
// WorkspaceActionsDialog — Task 10 file actions popup.
// Gathers the workspace file actions that don't live on the
// Lighting save bar or the conflict strip: open a managed
// workspace, Save Copy, Reload, Restore Backup, Reset draft,
// and Open Workspace Folder. Every action goes through the
// WorkspaceController — which latches a dirty-draft replace
// for confirmation (the shell strip shows it) — so nothing
// here ever discards edits silently.

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import OpenRGB2.UI.theme
import OpenRGB2.UI.components

Popup {
    id: dialog
    objectName: "workspaceActionsDialog"

    property var ctlOverride: null
    function ctl() {
        if (ctlOverride !== null)
            return ctlOverride
        try { return workspace } catch (e) { return null }
    }
    /* A ctl is "live" when it answers the Task-10 surface. */
    function live() {
        var c = ctl()
        return c !== null && c.managed === true
    }
    /* Connected-but-unmanaged still supports the workspace list
       + Open — that is how an imported workspace becomes current
       on a fresh install. */
    function connected() {
        var c = ctl()
        return c !== null && c.available === true
    }

    width: 560; height: 560
    modal: true
    /* Audit Q-9: a modal popup must own keyboard focus — without
       it typing went to the item behind the modal and Escape
       (CloseOnEscape acts only while the popup has focus) never
       closed it. The first field takes focus on open. */
    focus: true
    closePolicy: Popup.CloseOnEscape
    padding: 0
    background: Rectangle {
        color: Theme.surface
        radius: Theme.radiusM
        border.color: Theme.borderStrong
    }

    onOpened: {
        copyName.forceActiveFocus()
        var c = ctl()
        if (c !== null && c.refreshList !== undefined)
            c.refreshList()
    }

    /* F08: the footer Close button must NOT live inside the
       clipped Flickable — at the fixed 560px height it rendered
       half-clipped and untouchable. Same structure as
       WorkspaceImportDialog: header + scroll body + footer row
       as siblings in one ColumnLayout. */
    ColumnLayout {
        anchors.fill: parent
        anchors.margins: Theme.spaceL
        spacing: Theme.spaceM

        Text {
            text: qsTr("WORKSPACE")
            color: Theme.textSecondary
            font.pixelSize: Theme.fontSizeSmall
            font.letterSpacing: 1.5
        }
        Text {
            objectName: "wsActionsSummary"
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            color: Theme.textSecondary
            font.pixelSize: Theme.fontSizeBody
            text: {
                var c = dialog.ctl()
                if (c === null || c.managed !== true)
                    return (c !== null && c.workspaceList !== undefined
                            && c.workspaceList.length > 0)
                           ? qsTr("No managed workspace is active — open one below.")
                           : qsTr("No managed workspace is active.")
                var s = qsTr("'%1' (%2) · revision %3")
                    .arg(c.workspaceName || c.workspaceId)
                    .arg(c.workspaceId)
                    .arg(c.workspaceRevision)
                if (c.dirty)
                    s += qsTr(" — unsaved edits")
                if (c.conflict)
                    s += qsTr(" — engine copy changed under your draft")
                if (c.externalChange !== undefined
                        && c.externalChange !== "")
                    s += qsTr(" — files on disk: %1")
                             .arg(c.externalChange)
                return s
            }
        }

        /* Audit Q-8: Open / Reload / Restore over a dirty draft
           latch a replace for confirmation. The shell strip that
           also shows it sits BEHIND this modal popup — its buttons
           can't be reached while the dialog is up, so the action
           looked like it did nothing. The choice is offered here
           too, on the same controller calls. */
        Rectangle {
            objectName: "wsDlgReplaceBar"
            Layout.fillWidth: true
            visible: dialog.connected()
                     && dialog.ctl().replaceOffered === true
            implicitHeight: replaceCol.implicitHeight
                            + 2 * Theme.spaceM
            radius: Theme.radiusS
            color: Qt.rgba(0.878, 0.643, 0.345, 0.14)
            border.color: Theme.warn
            ColumnLayout {
                id: replaceCol
                anchors.fill: parent
                anchors.margins: Theme.spaceM
                spacing: Theme.spaceS
                Text {
                    objectName: "wsDlgReplaceText"
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    color: Theme.textPrimary
                    font.pixelSize: Theme.fontSizeBody
                    text: {
                        var c = dialog.ctl()
                        return (c !== null && c.replaceText !== undefined)
                               ? c.replaceText : ""
                    }
                }
                RowLayout {
                    Layout.fillWidth: true
                    spacing: Theme.spaceS
                    Item { Layout.fillWidth: true }
                    AppButton {
                        objectName: "wsDlgConfirmReplace"
                        text: qsTr("Replace my draft")
                        onClicked: dialog.ctl().confirmReplace()
                    }
                    AppButton {
                        objectName: "wsDlgCancelReplace"
                        text: qsTr("Keep my draft")
                        onClicked: dialog.ctl().cancelReplace()
                    }
                }
            }
        }

        Flickable {
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            contentHeight: body.implicitHeight
            boundsBehavior: Flickable.StopAtBounds

            ColumnLayout {
                id: body
                width: parent.width
                spacing: Theme.spaceM

                /*---- managed workspaces -----------------------*/
                Card {
                    title: qsTr("OPEN")
                    Layout.fillWidth: true
                    ColumnLayout {
                        width: parent.width
                        spacing: Theme.spaceS
                        Repeater {
                            objectName: "wsList"
                            model: {
                                var c = dialog.ctl()
                                return (c !== null && c.workspaceList !== undefined)
                                       ? c.workspaceList : []
                            }
                            delegate: RowLayout {
                                required property var modelData
                                width: parent.width
                                spacing: Theme.spaceS
                                Text {
                                    Layout.fillWidth: true
                                    text: {
                                        var c = dialog.ctl()
                                        return String(modelData)
                                            + (c !== null
                                               && c.workspaceId === modelData
                                               ? qsTr("  (current)") : "")
                                    }
                                    color: Theme.textPrimary
                                    font.pixelSize: Theme.fontSizeBody
                                    elide: Text.ElideRight
                                }
                                AppButton {
                                    objectName: "wsOpen_" + modelData
                                    text: qsTr("Open")
                                    enabled: dialog.connected()
                                             && modelData !== dialog.ctl().workspaceId
                                    onClicked: dialog.ctl().openWorkspace(modelData)
                                }
                            }
                        }
                        EmptyState {
                            visible: {
                                var c = dialog.ctl()
                                return c === null || c.workspaceList === undefined
                                       || c.workspaceList.length === 0
                            }
                            Layout.fillWidth: true
                            title: qsTr("No other workspaces")
                            detail: qsTr("Managed workspaces appear here once one is imported or saved.")
                        }
                        Text {
                            Layout.fillWidth: true
                            visible: dialog.live() && dialog.ctl().dirty
                            wrapMode: Text.WordWrap
                            color: Theme.textSecondary
                            font.pixelSize: Theme.fontSizeSmall
                            text: qsTr("Opening another workspace while your draft is dirty asks for confirmation first — the draft record stays in the recovery store.")
                        }
                    }
                }

                /*---- save copy ---------------------------------*/
                Card {
                    title: qsTr("SAVE COPY")
                    Layout.fillWidth: true
                    ColumnLayout {
                        width: parent.width
                        spacing: Theme.spaceS
                        RowLayout {
                            width: parent.width
                            spacing: Theme.spaceS
                            AppTextField {
                                id: copyName
                                objectName: "wsCopyName"
                                Layout.fillWidth: true
                                placeholderText: qsTr("new workspace name")
                                selectByMouse: true
                            }
                            AppButton {
                                objectName: "wsSaveCopy"
                                text: qsTr("Save Copy")
                                enabled: dialog.live()
                                         && copyName.text.trim() !== ""
                                         && !dialog.ctl().saving
                                onClicked: dialog.ctl().saveCopy(copyName.text.trim())
                            }
                        }
                        Text {
                            Layout.fillWidth: true
                            wrapMode: Text.WordWrap
                            color: Theme.textDisabled
                            font.pixelSize: Theme.fontSizeSmall
                            text: qsTr("Commits the draft under a new workspace id — the open workspace is untouched.")
                        }
                    }
                }

                /*---- file actions ------------------------------*/
                Card {
                    title: qsTr("ACTIONS")
                    Layout.fillWidth: true
                    ColumnLayout {
                        width: parent.width
                        spacing: Theme.spaceS
                        AppButton {
                            objectName: "wsActionReload"
                            Layout.fillWidth: true
                            text: qsTr("Reload from disk")
                            enabled: dialog.live()
                            onClicked: dialog.ctl().reload()
                        }
                        AppButton {
                            objectName: "wsActionRestoreBackup"
                            Layout.fillWidth: true
                            text: qsTr("Restore Backup")
                            enabled: dialog.live()
                            onClicked: dialog.ctl().restoreBackup()
                        }
                        AppButton {
                            objectName: "wsActionReset"
                            Layout.fillWidth: true
                            text: qsTr("Reset draft to last saved")
                            enabled: dialog.live() && dialog.ctl().dirty
                            onClicked: dialog.ctl().resetEdits()
                        }
                        AppButton {
                            objectName: "wsActionOpenFolder"
                            Layout.fillWidth: true
                            text: qsTr("Open Workspace Folder")
                            enabled: dialog.live()
                            onClicked: dialog.ctl().openWorkspaceFolder()
                        }
                        Text {
                            Layout.fillWidth: true
                            wrapMode: Text.WordWrap
                            color: Theme.textDisabled
                            font.pixelSize: Theme.fontSizeSmall
                            text: qsTr("Reload re-reads the engine's accepted generation. Restore Backup commits the last-valid (marker backup) generation forward as a new generation. File edits made outside the app are reported by the notice strip — never applied automatically.")
                        }
                    }
                }
            }
        }

        /*---- footer — outside the clipped scroll area --------*/
        RowLayout {
            Layout.fillWidth: true
            Item { Layout.fillWidth: true }
            AppButton {
                objectName: "wsActionsClose"
                text: qsTr("Close")
                onClicked: dialog.close()
            }
        }
    }
}
