// SPDX-License-Identifier: GPL-2.0-or-later
//
// EffectEditor — the live-effects layer editing surface
// (studio-import Task 6): ordered layer stack, per-layer
// inspector (blend/space/scalars/origin/direction/targets/
// source/palette/path) and the personal-look shelf — all bound
// to the shared workspace draft through `effectEdit`
// (EffectEditorController). One committed op = one undo record;
// the engine only ever sees a committed workspace save.
//
// Live-effects only: while Animation playback is selected the
// uploaded loop owns output and the panel surfaces the notice
// instead of silently implying the controls drive it.
import QtQuick
import QtQuick.Layouts
import QtQuick.Controls
import OpenRGB2.UI.theme
import OpenRGB2.UI.components

Rectangle {
    id: editor
    objectName: "effectEditor"
    color: "transparent"
    implicitHeight: col.implicitHeight

    /* Set by the host (LightingPage): true while Animation
       playback is the requested mode — layer edits still land
       in the draft but the notice keeps the gating honest. */
    property bool animationSelected: false

    readonly property bool ready:
        typeof effectEdit !== "undefined" && effectEdit.ready

    Column {
        id: col
        width: parent.width
        spacing: Theme.spaceM

        Text {
            objectName: "effectEditorAnimNote"
            visible: editor.animationSelected
            width: parent.width
            wrapMode: Text.WordWrap
            font.pixelSize: Theme.fontSizeSmall
            color: Theme.warn
            text: qsTr("Layer edits apply to the Live effects scene — they don't change the uploaded animation loop playing now.")
        }

        EmptyState {
            objectName: "effectEditorEmpty"
            anchors.horizontalCenter: parent.horizontalCenter
            visible: !editor.ready
            title: qsTr("No workspace draft")
            detail: qsTr("Open a managed workspace to edit effect layers.")
        }

        LookShelf {
            visible: editor.ready
            width: parent.width
        }

        /* Simple direction/colors/speed controls for looks built
           from wave layers (hides itself otherwise). */
        WaveControls {
            width: parent.width
        }

        LayerStack {
            visible: editor.ready
            width: parent.width
        }

        EffectInspector {
            visible: editor.ready
            width: parent.width
        }
    }
}
