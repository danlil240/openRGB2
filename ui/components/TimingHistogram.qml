// SPDX-License-Identifier: GPL-2.0-or-later
//
// TimingHistogram — one compact row of the E-12c timing card:
// a label, one plain bar per bucket (height = count relative to
// the fullest bucket; no chart dependency) and the p50/p95/p99
// summary. `hist` is DiagnosticsController::HistogramView:
// {available, measured, total, p50_text, p95_text, p99_text,
//  bars:[{label, count, frac}]}. No samples renders as
// "unmeasured", an engine without the field as "not reported" —
// never as zeros.
import QtQuick
import QtQuick.Layouts
import OpenRGB2.UI.theme

Item {
    id: root

    property string label: ""
    property var    hist: ({})

    readonly property bool available: !!root.hist && !!root.hist.available
    readonly property bool measured:  root.available && !!root.hist.measured
    readonly property int  barCount:  root.measured && root.hist.bars
                                      ? root.hist.bars.length : 0
    readonly property string summaryText:
        !root.available ? qsTr("not reported by this engine")
        : !root.measured ? qsTr("unmeasured — no samples yet")
        : qsTr("p50 %1 · p95 %2 · p99 %3 · n = %4")
              .arg(root.hist.p50_text).arg(root.hist.p95_text)
              .arg(root.hist.p99_text).arg(root.hist.total)

    readonly property int barHeight: 22

    implicitWidth:  row.implicitWidth
    implicitHeight: row.implicitHeight

    RowLayout {
        id: row
        width: parent.width
        spacing: Theme.spaceM

        Text {
            Layout.preferredWidth: 220
            text: root.label
            color: Theme.textPrimary
            font.pixelSize: Theme.fontSizeSmall
            elide: Text.ElideRight
        }

        /* Fixed-width bar strip so rows align whether or not a
           histogram has samples. */
        Rectangle {
            Layout.preferredWidth: 17 * 8 + 2
            Layout.preferredHeight: root.barHeight + 2
            color: Theme.surfaceAlt
            radius: 2
            Row {
                x: 1; y: 1
                spacing: 1
                Repeater {
                    model: root.measured ? root.hist.bars : []
                    Item {
                        width: 7
                        height: root.barHeight
                        Rectangle {
                            anchors.bottom: parent.bottom
                            width: parent.width
                            /* a non-empty bucket is always visible */
                            height: modelData.count > 0
                                    ? Math.max(2, root.barHeight * modelData.frac)
                                    : 0
                            color: Theme.accent
                        }
                    }
                }
            }
        }

        Text {
            objectName: "timingSummaryText"
            Layout.fillWidth: true
            text: root.summaryText
            color: root.measured ? Theme.textSecondary : Theme.textDisabled
            font.pixelSize: Theme.fontSizeSmall
            elide: Text.ElideRight
        }
    }
}
