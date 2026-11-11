// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
import QtQuick 2.6
import Sailfish.Silica 1.0

// Home's "Continue watching" shelf (docs/DESIGN.md §4.9), as on the official app: the videos left
// unfinished, newest first, each with its watched line. A tile opens the video (it resumes where
// it was left); the header opens the history. Home loads this file only when there is something
// to continue.
Item {
    id: shelf
    objectName: "continueShelf"
    // History rows ({ videoId, title, channel, channelId, thumbnail, fraction }).
    property var rows: []
    width: parent ? parent.width : 0
    height: shelfHeader.height + strip.height

    BackgroundItem {
        id: shelfHeader
        objectName: "continueShelfHeader"
        width: parent.width
        height: Theme.itemSizeSmall
        onClicked: pageStack.push(Qt.resolvedUrl("../pages/HistoryPage.qml"))
        Label {
            anchors {
                left: parent.left; leftMargin: Theme.horizontalPageMargin
                right: arrow.left; rightMargin: Theme.paddingMedium
                verticalCenter: parent.verticalCenter
            }
            //: Cover label above the last video, which can be resumed from the cover
            text: qsTranslate("CoverPage", "Continue watching")
            truncationMode: TruncationMode.Fade
            color: shelfHeader.highlighted ? Theme.highlightColor : Theme.primaryColor
        }
        Icon {
            id: arrow
            anchors { right: parent.right; rightMargin: Theme.horizontalPageMargin; verticalCenter: parent.verticalCenter }
            source: "image://theme/icon-m-right"
            // Points forward in right-to-left layouts too.
            mirror: Qt.application.layoutDirection === Qt.RightToLeft
            highlighted: shelfHeader.highlighted
        }
    }

    ListView {
        id: strip
        objectName: "continueShelfList"
        anchors.top: shelfHeader.bottom
        width: parent.width
        height: probe.height + Theme.paddingMedium
        orientation: ListView.Horizontal
        boundsBehavior: Flickable.StopAtBounds
        spacing: Theme.paddingMedium
        clip: true
        model: shelf.rows
        header: Item { width: Theme.horizontalPageMargin; height: 1 }
        footer: Item { width: Theme.horizontalPageMargin; height: 1 }
        delegate: ShelfTile {
            title: modelData.title || ""
            thumbnailUrl: modelData.thumbnail || ""
            progress: modelData.fraction
            onClicked: window.openVideo(modelData.videoId, { title: modelData.title, channel: modelData.channel,
                                                             channelId: modelData.channelId, thumbnail: modelData.thumbnail })
        }
        // Gives the shelf its height before any tile exists.
        ShelfTile { id: probe; visible: false }
    }
}
