// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
import QtQuick 2.6
import Sailfish.Silica 1.0

// LIB-06: progress bar above 0.5 %, watched badge. Fills a thumbnail; takes no
// input. Updates only when watchIndex reports this video (incremental lookups).
Item {
    id: overlay
    property string videoId
    property var watch: ({ showProgress: false, progress: 0, watched: false })

    enabled: false
    function refresh() { watch = videoId !== "" ? watchIndex.state(videoId) : { showProgress: false, progress: 0, watched: false } }
    onVideoIdChanged: refresh()
    Component.onCompleted: refresh()

    Connections {
        target: watchIndex
        onChanged: if (videoId === overlay.videoId) overlay.refresh()
    }

    Rectangle {
        objectName: "progressBar"
        visible: overlay.watch.showProgress
        anchors { left: parent.left; right: parent.right; bottom: parent.bottom }
        height: Math.max(2, Math.round(parent.height * 0.05))
        color: Qt.rgba(1, 1, 1, 0.35) // theme-exempt: progress track over a thumbnail
        Rectangle {
            objectName: "progressFill"
            width: parent.width * Math.min(1, overlay.watch.progress)
            height: parent.height
            color: Theme.highlightColor
        }
    }
    Rectangle {
        objectName: "watchedBadge"
        visible: overlay.watch.watched
        anchors { left: parent.left; top: parent.top; margins: Theme.paddingSmall }
        width: badge.width + Theme.paddingSmall * 2
        height: badge.height
        radius: Theme.paddingSmall / 2
        color: Qt.rgba(0, 0, 0, 0.75) // theme-exempt: scrim over a thumbnail
        Label {
            id: badge
            anchors.centerIn: parent
            font.pixelSize: Theme.fontSizeTiny
            color: Theme.lightPrimaryColor
            //: Badge on the thumbnail of a video the user has watched
            text: qsTr("Watched")
        }
    }
}
