// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
import QtQuick 2.6
import Sailfish.Silica 1.0

// DL-01: audio, or video with a per-download quality. Both start from the "Download quality"
// setting (downloads.defaultKind, downloads.defaultQualityHeight): audio only, or a height.
Dialog {
    id: dialog
    property var video: ({})
    readonly property var qualityHeights: [2160, 1440, 1080, 720, 480, 360]
    property int kind: downloads.defaultKind
    property int qualityHeight: downloads.defaultQualityHeight > 0 ? downloads.defaultQualityHeight : 1080

    Column {
        width: parent.width
        DialogHeader {
            //: Accept button of the download dialog
            acceptText: qsTr("Download")
            title: dialog.video.title || ""
        }
        ComboBox {
            //: Download dialog: audio or video
            label: qsTr("Download")
            currentIndex: dialog.kind
            menu: ContextMenu {
                //: Download kind: the audio track only
                MenuItem { text: qsTr("Audio only") }
                //: Download kind: video with sound
                MenuItem { text: qsTr("Video") }
            }
            onCurrentIndexChanged: dialog.kind = currentIndex
        }
        ComboBox {
            visible: dialog.kind === 1
            //: Download dialog: the video quality
            label: qsTr("Quality")
            currentIndex: {
                var i = dialog.qualityHeights.indexOf(dialog.qualityHeight)
                if (i >= 0) return i
                for (var j = 0; j < dialog.qualityHeights.length; ++j) if (dialog.qualityHeights[j] <= dialog.qualityHeight) return j
                return dialog.qualityHeights.length - 1
            }
            menu: ContextMenu {
                Repeater {
                    model: dialog.qualityHeights
                    MenuItem { text: modelData + "p" }
                }
            }
            onCurrentIndexChanged: dialog.qualityHeight = dialog.qualityHeights[currentIndex]
            //: Explanation under the download quality choice
            description: qsTr("The best available quality up to this height.")
        }
    }
    onAccepted: downloads.enqueue(video, kind, kind === 1 ? qualityHeight : 0)
}
