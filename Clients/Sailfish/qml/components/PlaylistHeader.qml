// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
import QtQuick 2.6
import Sailfish.Silica 1.0
import "../logic/Rows.js" as Rows
import "../logic/ShareLinks.js" as Links

// The head of a playlist (docs/DESIGN.md §4.8), as on the official app in Silica's language: the
// list's cover, a Play all button, and a strip of chips for its other actions (Shuffle first,
// then what the page adds, such as Share). The title stays in the PageHeader above. Each part
// shows only when the list can do it (ShareLinks.headParts).
Item {
    id: head
    objectName: "playlistHeader"
    property string coverUrl: ""
    // Videos in the list (those loaded so far, for a list that loads in pages).
    property int count: 0
    // A queue exists for Shuffle to fill.
    property bool queue: true
    // Landscape: a smaller cover leaves room for the first rows.
    property bool landscape: false
    // A short line under the chips ("Link copied"); empty hides it.
    property string message: ""
    default property alias chips: strip.chips
    signal playRequested()
    signal shuffleRequested()

    readonly property var parts: Links.headParts({ coverUrl: coverUrl, count: count, queue: queue })
    readonly property real coverWidth: Math.round(Math.min(width - 2 * Theme.horizontalPageMargin,
                                                           landscape ? Screen.width * 0.5 : width * 0.62))

    width: parent ? parent.width : 0
    height: messageLabel.y + messageLabel.height + Theme.paddingMedium

    Rectangle {
        id: cover
        anchors.horizontalCenter: parent.horizontalCenter
        visible: head.parts.cover
        width: head.coverWidth
        height: visible ? Rows.thumbHeight(width) : 0
        color: Theme.rgba(Theme.highlightBackgroundColor, 0.1)
        Image {
            objectName: "playlistCover"
            anchors.fill: parent
            source: head.coverUrl
            // Never blown up on the CPU beyond what the list thumbnail holds (Rows.decodeWidth).
            sourceSize.width: Rows.decodeWidth(width, false)
            asynchronous: true
            fillMode: Image.PreserveAspectCrop
            clip: true
        }
    }

    Button {
        id: playButton
        objectName: "playAllButton"
        anchors { horizontalCenter: parent.horizontalCenter; top: cover.bottom; topMargin: cover.visible ? Theme.paddingLarge : 0 }
        visible: head.parts.play
        height: visible ? implicitHeight : 0
        preferredWidth: Theme.buttonWidthMedium
        //: Menu entry playing a playlist from the start
        text: qsTranslate("PlaylistPage", "Play all")
        onClicked: head.playRequested()
    }

    ChipStrip {
        id: strip
        objectName: "playlistChips"
        anchors { top: playButton.bottom; topMargin: playButton.visible ? Theme.paddingMedium : 0 }
        ActionChip {
            objectName: "shuffleChip"
            visible: head.parts.shuffle
            iconSource: "image://theme/icon-m-shuffle"
            //: Menu entry playing a playlist in random order
            text: qsTranslate("PlaylistPage", "Shuffle")
            onClicked: head.shuffleRequested()
        }
    }

    Label {
        id: messageLabel
        anchors {
            top: strip.bottom
            left: parent.left; leftMargin: Theme.horizontalPageMargin
            right: parent.right; rightMargin: Theme.horizontalPageMargin
        }
        visible: text !== ""
        height: visible ? implicitHeight : 0
        text: head.message
        wrapMode: Text.Wrap
        font.pixelSize: Theme.fontSizeExtraSmall
        color: Theme.highlightColor
    }
}
