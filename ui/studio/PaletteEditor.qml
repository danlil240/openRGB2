// SPDX-License-Identifier: GPL-2.0-or-later
//
// PaletteEditor — gradient-stop editor for the selected effect
// layer (studio-import Task 6). Stops read live from
// effectEdit.layerAt(selectedLayer).palette; every mutation is
// a controller op so a stop drag or insert lands exactly ONE
// undo record through the shared session.
//
// Layout: a bar shows the gradient sample; stop handles sit on
// it at their positions and drag horizontally (wrapped in a
// layer gesture); the row below edits the selected stop's
// color and offers insert/delete.
import QtQuick
import QtQuick.Layouts
import QtQuick.Controls
import OpenRGB2.UI.theme
import OpenRGB2.UI.components

Rectangle {
    id: pal
    objectName: "paletteEditor"
    color: "transparent"
    implicitHeight: col.implicitHeight

    /* The layer this edits — set by the inspector. */
    property int layerIndex: -1

    readonly property bool ready:
        typeof effectEdit !== "undefined" && effectEdit.ready
        && layerIndex >= 0

    /* Live stops snapshot — re-read on every stack change so a
       drag preview paints what the draft holds. The handles are
       NOT a Repeater over this array (see stopHandles): a drag
       reloads it on every move. */
    property var stops: []
    property int selStop: -1
    function reload() {
        /* Reads layerIndex directly, not `ready`: from
           onLayerIndexChanged the `ready` binding may not have
           re-evaluated yet, which left a newly selected layer
           with an empty stop list until the next stack edit. */
        const live = typeof effectEdit !== "undefined"
                     && effectEdit.ready && layerIndex >= 0
        stops = live ? (effectEdit.layerAt(layerIndex).palette || [])
                     : []
        if(selStop >= stops.length)
            selStop = stops.length - 1
    }
    Component.onCompleted: reload()
    onLayerIndexChanged: { selStop = -1; reload() }

    /* Piecewise-linear sample of the stop list at u in 0..1 —
       matches the core's Palette interpolation for display. */
    function sample(u) {
        const s = pal.stops;
        if(s.length === 0)
            return "#000000";
        if(u <= s[0].pos)
            return s[0].color;
        for(let i = 1; i < s.length; ++i) {
            if(u <= s[i].pos) {
                const t = (u - s[i - 1].pos)
                        / Math.max(1e-6, s[i].pos - s[i - 1].pos);
                const a = parseInt(
                    String(s[i - 1].color).slice(1), 16) || 0;
                const b = parseInt(
                    String(s[i].color).slice(1), 16) || 0;
                const r = ((a >> 16) & 255)
                        + (((b >> 16) & 255) - ((a >> 16) & 255)) * t;
                const g = ((a >> 8) & 255)
                        + (((b >> 8) & 255) - ((a >> 8) & 255)) * t;
                const bl = (a & 255) + ((b & 255) - (a & 255)) * t;
                return Qt.rgba(r / 255, g / 255, bl / 255, 1);
            }
        }
        return s[s.length - 1].color;
    }
    Connections {
        target: typeof effectEdit !== "undefined" ? effectEdit : null
        function onStackChanged() { pal.reload() }
    }

    Column {
        id: col
        width: parent.width
        spacing: Theme.spaceS

        Text {
            visible: !pal.ready || pal.stops.length === 0
            text: qsTr("No palette stops — add one below.")
            color: Theme.textDisabled
            font.pixelSize: Theme.fontSizeSmall
            wrapMode: Text.WordWrap
            width: parent.width
        }

        /* Gradient bar + stop handles. */
        Item {
            id: bar
            width: parent.width
            height: 44
            visible: pal.stops.length > 0

            /* Sampled slices — the bar interpolates the real
               stop list (piecewise-linear like Palette::Sample),
               never a fixed-segment approximation. */
            Row {
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.top: parent.top
                height: 20
                Repeater {
                    model: 32
                    delegate: Rectangle {
                        required property int index
                        width: bar.width / 32
                        height: 20
                        color: pal.sample(index / 31)
                    }
                }
            }

            /* Stop handles (audit Q-1). The model is the stop
               COUNT, not the stops array: reload() reassigns that
               array on every stackChanged — every preview move of
               a drag — and a Repeater over a JS array rebuilds all
               its delegates on each assignment. That destroyed the
               handle whose MouseArea held the press, so onReleased
               never fired and the layer gesture stayed open. An
               integer model keeps the delegates while the count
               holds; each reads its stop by index. A handle never
               moves itself (no drag.target): x stays bound to the
               draft, so after a re-sort the dragged stop is drawn
               by its new index's delegate while the pressed one
               keeps the grab and steers by the pointer. */
            Repeater {
                objectName: "stopHandles"
                model: pal.stops.length
                delegate: Rectangle {
                    id: stopHandle
                    required property int index
                    readonly property var stop: pal.stops[index]
                        || ({ pos: 0, color: "#000000" })
                    width: 14; height: 14; radius: 7
                    y: 24
                    x: bar.width * stop.pos - width / 2
                    color: stop.color
                    border.width: pal.selStop === index ? 2 : 1
                    border.color: pal.selStop === index
                                  ? Theme.accent : Theme.borderStrong
                    objectName: "stopHandle_" + index

                    MouseArea {
                        id: dragArea
                        anchors.fill: parent
                        /* The inspector sits in a page Flickable
                           that would otherwise steal the drag
                           past its threshold (as in 21a7b63). */
                        preventStealing: true
                        /* This press opened the layer gesture —
                           exactly one commit or cancel closes it. */
                        property bool gestureOpen: false
                        property bool moved: false
                        property real pressX: 0
                        property real grabDx: 0
                        onPressed: function(m) {
                            pal.selStop = stopHandle.index;
                            pressX = mapToItem(bar, m.x, m.y).x;
                            grabDx = m.x - stopHandle.width / 2;
                            moved = false;
                            effectEdit.beginLayerGesture();
                            gestureOpen = true;
                        }
                        onPositionChanged: function(m) {
                            if(!gestureOpen)
                                return;
                            /* Pointer in bar coordinates — this
                               handle may have jumped to another
                               stop's position after a re-sort. */
                            const bx = mapToItem(bar, m.x, m.y).x;
                            if(!moved && Math.abs(bx - pressX)
                                    < Qt.styleHints.startDragDistance)
                                return;
                            moved = true;
                            const p = Math.max(0, Math.min(1,
                                (bx - grabDx) / bar.width));
                            /* Re-sort can move the dragged stop —
                               track the index the controller
                               hands back. */
                            pal.selStop = effectEdit.moveLayerStop(
                                pal.layerIndex, pal.selStop, p);
                        }
                        onReleased: {
                            if(!gestureOpen)
                                return;
                            gestureOpen = false;
                            effectEdit.commitLayerGesture(
                                qsTr("move gradient stop"));
                        }
                        /* A grab taken away mid-drag (focus loss,
                           popup, disable) drops the uncommitted
                           move instead of leaving the gesture
                           open for the next press to revert. */
                        onCanceled: {
                            if(!gestureOpen)
                                return;
                            gestureOpen = false;
                            effectEdit.cancelLayerGesture();
                        }
                        Component.onDestruction: {
                            if(gestureOpen)
                                effectEdit.cancelLayerGesture();
                        }
                    }
                }
            }
        }

        /* Selected-stop editor + insert/delete. */
        RowLayout {
            width: parent.width
            spacing: Theme.spaceM
            visible: pal.stops.length > 0

            ColorWell {
                objectName: "stopColor"
                swatch: pal.selStop >= 0 && pal.selStop < pal.stops.length
                        ? pal.stops[pal.selStop].color : "#000000"
                label: pal.selStop >= 0 && pal.selStop < pal.stops.length
                       ? qsTr("pos %1").arg(
                             Number(pal.stops[pal.selStop].pos)
                                 .toFixed(2))
                       : ""
            }
            AppTextField {
                id: colorField
                objectName: "stopColorField"
                Accessible.name: qsTr("gradient stop color")
                Layout.preferredWidth: 90
                enabled: pal.selStop >= 0
                placeholderText: "#RRGGBB"
                text: pal.selStop >= 0 && pal.selStop < pal.stops.length
                      ? String(pal.stops[pal.selStop].color) : ""
                /* Commits when the edit ends — Enter or focus loss
                   (audit Q-10); unchanged text adds no record, and
                   a refused color does not stay in the field. */
                onEditingFinished: {
                    if(pal.selStop < 0 || pal.selStop >= pal.stops.length)
                        return;
                    if(String(text).toUpperCase() !== String(
                           pal.stops[pal.selStop].color).toUpperCase())
                        effectEdit.setLayerStopColor(pal.layerIndex,
                                                     pal.selStop,
                                                     text);
                    pal.reload();
                }
            }
            AppButton {
                objectName: "stopRemove"
                text: qsTr("Remove stop")
                enabled: pal.selStop >= 0
                onClicked:
                    effectEdit.removeLayerStop(pal.layerIndex,
                                               pal.selStop)
            }
            Item { Layout.fillWidth: true }
            AppButton {
                objectName: "stopAdd"
                text: qsTr("Add stop")
                enabled: pal.ready
                /* Insert midway past the last stop (or at 0.5 on
                   an empty palette) with the neighboring color —
                   the parser refuses colliding positions, so the
                   controller picks a free slot honestly via the
                   engine rejection if the midpoint is taken. */
                onClicked: {
                    const n = pal.stops.length;
                    const pos = n === 0 ? 0.5
                        : Math.min(0.98,
                            pal.stops[n - 1].pos
                            + (1.0 - pal.stops[n - 1].pos) / 2);
                    const col = n === 0 ? "#FFFFFF"
                                        : pal.stops[n - 1].color;
                    effectEdit.addLayerStop(pal.layerIndex, pos, col);
                }
            }
        }
    }
}
