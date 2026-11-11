// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
import QtQuick 2.6
import Sailfish.Silica 1.0

// The end of a video (docs/DESIGN.md §3.2.4), as on the official app: while autoplay counts down,
// an "Up next" card with the next video's picture and title, the countdown and Cancel; once the
// video has ended without autoplay, up to three suggested videos along the bottom (the overlay's
// centre disc offers Replay above them). A tap on a suggestion plays it on this watch page.
Item {
    id: end
    objectName: "playerEndScreen"
    property bool ended: false
    // Related videos (catalog's RelatedModel); the first three are suggested.
    property var related: null
    readonly property bool countingDown: playback.upNextVideoId !== ""
    readonly property int suggestionCount: related ? Math.min(3, related.count) : 0
    readonly property bool suggesting: ended && !countingDown && suggestionCount > 0
    signal openRequested(string videoId, var hints)

    visible: countingDown || suggesting

    // ---- Up next (PLY-25) ----
    Rectangle {
        id: upNext
        objectName: "upNextCard"
        visible: end.countingDown
        anchors.centerIn: parent
        readonly property var next: end.related && end.related.count > 0 ? end.related.get(0) : null
        width: Math.min(parent.width - 2 * Theme.horizontalPageMargin, Theme.itemSizeHuge * 3)
        height: upNextColumn.height + 2 * Theme.paddingMedium
        radius: Theme.paddingSmall
        color: Qt.rgba(0, 0, 0, 0.75) // theme-exempt: card over the picture
        Column {
            id: upNextColumn
            anchors { left: parent.left; right: parent.right; top: parent.top; margins: Theme.paddingMedium }
            spacing: Theme.paddingSmall
            Label {
                width: parent.width
                //: Autoplay countdown, %1 is the number of seconds before the next video starts
                text: qsTranslate("VideoPage", "Up next in %1 s").arg(playback.upNextSecondsLeft)
                font.pixelSize: Theme.fontSizeExtraSmall
                color: Theme.lightSecondaryColor
            }
            Row {
                width: parent.width
                spacing: Theme.paddingMedium
                Image {
                    id: nextThumb
                    width: Math.round(parent.width * 0.4)
                    height: Math.round(width * 9 / 16)
                    source: upNext.next ? (upNext.next.thumbnail || "") : ""
                    sourceSize.width: width
                    asynchronous: true
                    fillMode: Image.PreserveAspectCrop
                    clip: true
                }
                Label {
                    width: parent.width - nextThumb.width - parent.spacing
                    text: upNext.next ? (upNext.next.title || "") : ""
                    wrapMode: Text.Wrap
                    maximumLineCount: 3
                    elide: Text.ElideRight
                    font.pixelSize: Theme.fontSizeSmall
                    color: Theme.lightPrimaryColor
                }
            }
            Button {
                anchors.horizontalCenter: parent.horizontalCenter
                color: Theme.lightPrimaryColor
                //: Cancels the autoplay countdown
                text: qsTranslate("VideoPage", "Cancel")
                onClicked: playback.cancelUpNext()
            }
        }
    }

    // ---- Suggestions after the end ----
    Row {
        id: suggestions
        objectName: "endSuggestions"
        visible: end.suggesting
        anchors { horizontalCenter: parent.horizontalCenter; bottom: parent.bottom; bottomMargin: Theme.itemSizeMedium + Theme.paddingSmall }
        spacing: Theme.paddingMedium
        readonly property real tileWidth: Math.min(Theme.itemSizeHuge * 1.2,
                                                   (end.width - 2 * Theme.horizontalPageMargin - 2 * spacing) / 3)
        Repeater {
            model: end.suggesting ? end.suggestionCount : 0
            BackgroundItem {
                id: tile
                readonly property var row: end.related.get(index)
                width: suggestions.tileWidth
                height: tileThumb.height + tileTitle.height + Theme.paddingSmall
                onClicked: end.openRequested(row.videoId, row)
                Accessible.name: row.title || ""
                Image {
                    id: tileThumb
                    width: parent.width
                    height: Math.round(width * 9 / 16)
                    source: tile.row.thumbnail || ""
                    sourceSize.width: width
                    asynchronous: true
                    fillMode: Image.PreserveAspectCrop
                    clip: true
                }
                Label {
                    id: tileTitle
                    anchors { top: tileThumb.bottom; topMargin: Theme.paddingSmall / 2 }
                    width: parent.width
                    text: tile.row.title || ""
                    wrapMode: Text.Wrap
                    maximumLineCount: 2
                    elide: Text.ElideRight
                    font.pixelSize: Theme.fontSizeTiny
                    // Pressed: the highlight as it reads on a dark picture (§7.4).
                    color: tile.highlighted ? Theme.highlightFromColor(Theme.highlightColor, Theme.LightOnDark)
                                            : Theme.lightPrimaryColor
                }
            }
        }
    }
}
