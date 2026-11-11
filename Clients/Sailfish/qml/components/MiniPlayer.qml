// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
import QtQuick 2.6
import Sailfish.Silica 1.0
import "../logic/Shell.js" as Shell

// In-app mini-player (PLY-11): docked at the bottom of content pages while a video is loaded
// and the watch page is not on top. Picture (live frame while playing, thumbnail otherwise),
// title, a thin progress line, play/pause, next with a queue, and close. Tapping the body
// returns to the watch page, re-attaching to the live player.
DockedPanel {
    id: bar

    property bool shown: false
    // Set by the playback component when a queue exists (PLY-11 "next").
    property int queueLength: typeof playback.queueLength === "number" ? playback.queueLength : 0
    property int queueIndex: typeof playback.queueIndex === "number" ? playback.queueIndex : -1
    signal openRequested()
    signal closeRequested()

    width: parent ? parent.width : 0
    // Two text lines; grows with the system text size (docs/DESIGN.md §7.9).
    height: Math.max(Theme.itemSizeLarge, barTexts.height + 2 * Theme.paddingMedium)
    dock: Dock.Bottom
    modal: false

    onShownChanged: shown ? show() : hide()
    Component.onCompleted: if (shown) show(true)

    BackgroundItem {
        id: body
        anchors { left: parent.left; right: buttons.left; top: parent.top; bottom: parent.bottom }
        onClicked: bar.openRequested()

        Rectangle {
            id: picture
            anchors { left: parent.left; verticalCenter: parent.verticalCenter; leftMargin: Theme.paddingMedium }
            height: parent.height - 2 * Theme.paddingMedium
            width: Math.round(height * 16 / 9)
            color: Theme.overlayBackgroundColor

            Image {
                anchors.fill: parent
                source: playback.thumbnail
                sourceSize.width: width
                fillMode: Image.PreserveAspectCrop
                clip: true
                asynchronous: true
                visible: !frame.active
            }
            // Live frame only while no watch page holds the video surface.
            Loader {
                id: frame
                anchors.fill: parent
                active: bar.expanded && bar.shown && playback.hasVideo && playback.playing && !window.videoVisible
                // A separate file, so pages without the player module still compile this one.
                source: active ? "MiniPlayerPicture.qml" : ""
            }
        }

        Column {
            id: barTexts
            anchors {
                left: picture.right; leftMargin: Theme.paddingMedium
                right: parent.right; rightMargin: Theme.paddingSmall
                verticalCenter: parent.verticalCenter
            }
            Label {
                width: parent.width
                text: playback.title
                truncationMode: TruncationMode.Fade
                font.pixelSize: Theme.fontSizeSmall
                color: body.highlighted ? Theme.highlightColor : Theme.primaryColor
            }
            Label {
                width: parent.width
                text: playback.channel
                truncationMode: TruncationMode.Fade
                font.pixelSize: Theme.fontSizeExtraSmall
                color: body.highlighted ? Theme.secondaryHighlightColor : Theme.secondaryColor
            }
        }
    }

    Row {
        id: buttons
        anchors { right: parent.right; verticalCenter: parent.verticalCenter; rightMargin: Theme.paddingSmall }
        IconButton {
            icon.source: playback.playing ? "image://theme/icon-m-pause" : "image://theme/icon-m-play"
            Accessible.name: playback.playing
                             //: Accessible name of the pause button
                             ? qsTr("Pause")
                             //: Accessible name of the play button
                             : qsTr("Play")
            onClicked: playback.togglePlayback()
        }
        IconButton {
            // MPR-01 / M3: the session says whether there is a next item.
            visible: playback.canGoNext === true || Shell.hasNext(bar.queueLength, bar.queueIndex)
            icon.source: "image://theme/icon-m-next"
            //: Accessible name of the button playing the next video of the queue
            Accessible.name: qsTr("Next video")
            onClicked: if (typeof playback.next === "function") playback.next()
        }
        IconButton {
            icon.source: "image://theme/icon-m-clear"
            //: Accessible name of the button closing the mini-player and stopping playback
            Accessible.name: qsTr("Close player")
            onClicked: bar.closeRequested()
        }
    }

    // Thin progress line along the top edge. Placed with x, not an anchor: playback progress
    // fills from the left in right-to-left layouts too, like the seek bar (docs/DESIGN.md §7.8).
    Rectangle {
        objectName: "miniProgress"
        x: 0
        y: 0
        height: Math.max(2, Math.round(Theme.paddingSmall / 2))
        width: parent.width * Shell.progress(playback.position, playback.duration, playback.live)
        color: Theme.highlightColor
    }
}
