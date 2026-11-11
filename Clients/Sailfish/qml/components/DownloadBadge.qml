// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
import QtQuick 2.6
import Sailfish.Silica 1.0

// DL-04 row badge: percentage while downloading, a mark once downloaded. No input.
Rectangle {
    id: badge
    property string videoId
    property var info: ({ active: false, done: false, progress: 0 })
    readonly property bool available: typeof downloads !== "undefined"

    function refresh() { if (available && videoId !== "") info = downloads.stateFor(videoId) }
    onVideoIdChanged: refresh()
    Component.onCompleted: refresh()

    Connections {
        target: badge.available ? downloads : null
        onItemChanged: if (videoId === badge.videoId) badge.refresh()
    }

    enabled: false
    visible: info.active || info.done
    width: text.width + Theme.paddingSmall * 2
    height: text.height
    radius: Theme.paddingSmall / 2
    color: Qt.rgba(0, 0, 0, 0.75) // theme-exempt: scrim over a thumbnail
    Label {
        id: text
        objectName: "downloadBadgeText"
        anchors.centerIn: parent
        font.pixelSize: Theme.fontSizeTiny
        color: Theme.lightPrimaryColor
        text: badge.info.done
              //: Badge on the thumbnail of a downloaded video
              ? qsTr("Saved")
              : Math.round(badge.info.progress * 100) + " %"
    }
}
