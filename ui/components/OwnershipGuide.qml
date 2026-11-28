// SPDX-License-Identifier: GPL-2.0-or-later
//
// OwnershipGuide — R-10: short, concrete steps to turn off ONE
// competing RGB writer, chosen by the engine's stable finding key
// (never by matching the owner label). Report-only: the buttons
// open a Windows surface or copy a command; the user takes the
// action. OpenRGB2 never stops a process, a service or a setting.
//
// Each step is {text, copy?}: `copy` is a command shown with a
// Copy button, for the user to run in an elevated terminal.
import QtQuick
import QtQuick.Layouts
import OpenRGB2.UI.theme

Column {
    id: root

    property string key: ""
    property string owner: ""

    objectName: "ownershipGuide_" + root.key
    spacing: Theme.spaceXS

    readonly property bool hasCtl: typeof diagCtl !== "undefined"

    readonly property var steps: {
        const app = root.owner || qsTr("the other app");
        switch(root.key) {
        case "dynamic_lighting": return [
            { text: qsTr("Open Settings > Personalization > Dynamic Lighting.") },
            { text: qsTr("Turn off \"Use Dynamic Lighting on my devices\".") },
            { text: qsTr("A Windows update can turn it back on — this banner will say so.") } ];
        case "lamparray": return [
            { text: qsTr("The Logitech LampArray service (logi_lamparray_service) is installed by Windows Update and repaints Logitech devices for Dynamic Lighting.") },
            { text: qsTr("Stop it — run in an administrator terminal:"),
              copy: "sc stop logi_lamparray_service" },
            { text: qsTr("Keep it from starting again (Manual start):"),
              copy: "sc config logi_lamparray_service start= demand" },
            { text: qsTr("Windows Update may switch it back on — this banner will say so.") } ];
        case "logitech": return [
            { text: qsTr("Quit G HUB: right-click its tray icon > Quit.") },
            { text: qsTr("In G HUB settings, turn off \"Start G HUB on system startup\" — otherwise it comes back at the next sign-in.") },
            { text: qsTr("Logi Options+: quit it from the tray, or turn off its lighting control for the device.") } ];
        case "openrgb": return [
            { text: qsTr("Another OpenRGB is running. Close it from its tray icon or window, and turn off its start-with-Windows option.") } ];
        case "lianli": return [
            { text: qsTr("Quit L-Connect from its tray icon and turn off its start-with-Windows option.") } ];
        case "gigabyte":
        case "razer":
        case "corsair":
        case "signalrgb": return [
            { text: qsTr("Quit %1, or turn off its lighting module for these devices.").arg(app) },
            { text: qsTr("Turn off its start-with-Windows option so it does not come back.") } ];
        default: return [
            { text: qsTr("Close %1, or move its RGB control off this hardware.").arg(app) } ];
        }
    }

    /* Plain text of every step — read by the offline suite and
       by screen readers. */
    readonly property string stepsText: {
        const parts = [];
        for(let i = 0; i < root.steps.length; ++i)
            parts.push((i + 1) + ". " + root.steps[i].text
                       + (root.steps[i].copy ? " " + root.steps[i].copy : ""));
        return parts.join("\n");
    }
    Accessible.name: root.stepsText

    Repeater {
        model: root.steps
        RowLayout {
            width: root.width
            spacing: Theme.spaceS
            Text {
                Layout.alignment: Qt.AlignTop
                text: (index + 1) + "."
                color: Theme.textSecondary
                font.pixelSize: Theme.fontSizeSmall
            }
            Column {
                Layout.fillWidth: true
                spacing: Theme.spaceXS
                Text {
                    width: parent.width
                    text: modelData.text
                    color: Theme.textSecondary
                    font.pixelSize: Theme.fontSizeSmall
                    wrapMode: Text.WordWrap
                }
                RowLayout {
                    visible: !!modelData.copy
                    width: parent.width
                    spacing: Theme.spaceS
                    Rectangle {
                        Layout.fillWidth: true
                        implicitHeight: cmd.implicitHeight + 2 * Theme.spaceXS
                        color: Theme.surface
                        border.color: Theme.border
                        radius: 3
                        Text {
                            id: cmd
                            anchors {
                                left: parent.left; right: parent.right
                                verticalCenter: parent.verticalCenter
                                leftMargin: Theme.spaceS; rightMargin: Theme.spaceS
                            }
                            text: modelData.copy || ""
                            color: Theme.textPrimary
                            font.family: "Consolas"
                            font.pixelSize: Theme.fontSizeSmall
                            elide: Text.ElideRight
                        }
                    }
                    AppButton {
                        text: qsTr("Copy")
                        Accessible.name: qsTr("Copy command: %1").arg(modelData.copy || "")
                        enabled: root.hasCtl
                        onClicked: diagCtl.copyText(modelData.copy)
                    }
                }
            }
        }
    }

    Row {
        spacing: Theme.spaceS
        visible: root.key === "dynamic_lighting" || root.key === "lamparray"
        AppButton {
            objectName: "openLightingSettingsButton"
            visible: root.key === "dynamic_lighting"
            text: qsTr("Open Dynamic Lighting settings")
            enabled: root.hasCtl
            onClicked: diagCtl.openOwnershipTarget("dynamic_lighting_settings")
        }
        AppButton {
            objectName: "openServicesButton"
            visible: root.key === "lamparray"
            text: qsTr("Open Services")
            enabled: root.hasCtl
            onClicked: diagCtl.openOwnershipTarget("services")
        }
    }
}
