// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
import QtQuick 2.6
import Sailfish.Silica 1.0
import "Format.js" as Format
import "../logic/PlayerUi.js" as P

// The player's seek bar (docs/DESIGN.md §3.2.1), as on the official app in Silica's colours: a
// thin track with the buffered range, the played part in the highlight colour, small gaps at
// chapter starts, SponsorBlock segments in their categories' colours, and a thumb that grows
// while held. While dragging, a bubble above the finger shows the time and the chapter name (and a
// storyboard frame when the session provides one); the seek happens on release, as one flush.
Item {
    id: bar
    property real position: 0          // ms
    property real duration: 0          // ms
    property real buffered: -1         // ms; -1 when unknown
    property var chapters: []
    property var segments: []
    property bool seekable: true
    // While dragging: the position under the finger.
    property real dragValue: 0
    readonly property bool down: area.pressed
    readonly property real value: down ? dragValue : position

    signal pressed()
    signal seekRequested(real positionMs)

    implicitHeight: Theme.itemSizeExtraSmall

    readonly property real trackHeight: Math.max(2, Math.round(Theme.paddingSmall * 0.75))
    // The ambience's highlight as it reads on a dark picture: in a light ambience the plain
    // highlight colour is dark (docs/DESIGN.md §7.4).
    readonly property color accent: Theme.highlightFromColor(Theme.highlightColor, Theme.LightOnDark)
    readonly property real trackY: Math.round((height - trackHeight) / 2)

    // Track: the unplayed part.
    Rectangle {
        id: track
        x: 0
        y: bar.trackY
        width: bar.width
        height: bar.trackHeight
        color: Theme.rgba(Theme.lightPrimaryColor, 0.3)
    }
    // Buffered range.
    Rectangle {
        x: 0
        y: bar.trackY
        height: bar.trackHeight
        width: bar.buffered >= 0 ? bar.width * P.fraction(bar.buffered, bar.duration) : 0
        color: Theme.rgba(Theme.lightPrimaryColor, 0.55)
    }
    // SponsorBlock segments (SB-02), under the played part.
    Repeater {
        model: P.segmentRects(bar.segments, bar.duration)
        Rectangle {
            x: bar.width * modelData.from
            y: bar.trackY
            width: Math.max(1, bar.width * (modelData.to - modelData.from))
            height: bar.trackHeight
            color: modelData.colour // theme-exempt: SponsorBlock category colour
        }
    }
    // Played part.
    Rectangle {
        x: 0
        y: bar.trackY
        height: bar.trackHeight
        width: bar.width * P.fraction(bar.value, bar.duration)
        color: bar.accent
    }
    // Chapter gaps: small cuts in the track at each chapter start.
    Repeater {
        model: P.chapterGaps(bar.chapters, bar.duration)
        Rectangle {
            x: Math.round(bar.width * modelData) - width / 2
            y: bar.trackY
            width: Math.max(2, Math.round(Theme.paddingSmall / 2))
            height: bar.trackHeight
            color: "black" // theme-exempt: a cut in the track, the picture's colour behind it
        }
    }
    // Thumb: visible with the controls, larger while held.
    Rectangle {
        id: thumb
        readonly property real size: bar.down ? Theme.paddingLarge * 1.5 : Theme.paddingLarge * 0.8
        width: size
        height: size
        radius: size / 2
        x: bar.width * P.fraction(bar.value, bar.duration) - size / 2
        y: Math.round(bar.height / 2 - size / 2)
        color: bar.accent
        visible: bar.seekable && bar.duration > 0
        Behavior on width { NumberAnimation { duration: 100 } }
        Behavior on height { NumberAnimation { duration: 100 } }
    }

    // The bubble above the finger while dragging: time, chapter name, storyboard frame.
    Rectangle {
        id: bubble
        visible: bar.down
        readonly property string chapter: P.chapterAt(bar.chapters, bar.dragValue)
        // A storyboard is a sprite sheet: the session names the sheet and the cell for a position
        // (playback.scrubPreview(ms) -> { url, x, y, width, height }, an empty map without one).
        // Empty: the bubble shows the time and chapter only, which is the normal case in beta 3.
        readonly property var cell: bar.down ? playback.scrubPreview(bar.dragValue) : null
        readonly property string preview: cell && cell.url ? cell.url : ""
        // The cell's rectangle in the sheet; zeros without a storyboard.
        readonly property real cellX: cell && cell.x > 0 ? cell.x : 0
        readonly property real cellY: cell && cell.y > 0 ? cell.y : 0
        readonly property real cellWidth: cell && cell.width > 0 ? cell.width : 0
        readonly property real cellHeight: cell && cell.height > 0 ? cell.height : 0
        // The frame takes the cell's own shape (cells follow the video's aspect: flat for a film,
        // tall for a vertical video), so no strip of the next row shows.
        readonly property var frame: P.previewFrame(cellWidth, cellHeight, Theme.itemSizeExtraLarge * 1.5,
                                                    Theme.itemSizeExtraLarge * 1.5)
        width: Math.max(bubbleColumn.width, previewImage.visible ? previewImage.width : 0) + 2 * Theme.paddingMedium
        height: bubbleColumn.height + 2 * Theme.paddingSmall
        radius: Theme.paddingSmall
        color: Qt.rgba(0, 0, 0, 0.75) // theme-exempt: bubble over the picture
        x: Math.max(0, Math.min(bar.width - width, thumb.x + thumb.width / 2 - width / 2))
        y: -height - Theme.paddingMedium
        Column {
            id: bubbleColumn
            anchors.centerIn: parent
            spacing: Theme.paddingSmall / 2
            // The cell of the sprite sheet, scaled into a frame of its own shape.
            Item {
                id: previewImage
                objectName: "scrubPreviewFrame"
                anchors.horizontalCenter: parent.horizontalCenter
                visible: bubble.preview !== "" && bubble.frame.width > 0
                width: bubble.frame.width
                height: bubble.frame.height
                clip: true
                readonly property real factor: bubble.cellWidth > 0 ? width / bubble.cellWidth : 1
                Image {
                    source: bubble.preview
                    asynchronous: true
                    sourceSize.width: 0   // a sprite sheet decodes at its own size (cells are cut from it)
                    x: -bubble.cellX * previewImage.factor
                    y: -bubble.cellY * previewImage.factor
                    width: implicitWidth * previewImage.factor
                    height: implicitHeight * previewImage.factor
                    smooth: true
                }
            }
            Label {
                anchors.horizontalCenter: parent.horizontalCenter
                text: Format.time(bar.dragValue)
                font.pixelSize: Theme.fontSizeSmall
                color: Theme.lightPrimaryColor
            }
            Label {
                anchors.horizontalCenter: parent.horizontalCenter
                visible: text !== ""
                text: bubble.chapter
                width: Math.min(implicitWidth, bar.width * 0.6)
                truncationMode: TruncationMode.Fade
                font.pixelSize: Theme.fontSizeExtraSmall
                color: Theme.lightSecondaryColor
            }
        }
    }

    // Touch: the whole bar height, taller than the track. preventStealing keeps the drag from
    // becoming Silica's back swipe while the finger is on the bar.
    MouseArea {
        id: area
        anchors { fill: parent; topMargin: -Theme.paddingMedium; bottomMargin: -Theme.paddingMedium }
        enabled: bar.seekable && bar.duration > 0
        preventStealing: true
        onPressed: { bar.dragValue = P.positionAt(mouse.x, bar.width, bar.duration); bar.pressed() }
        onPositionChanged: if (pressed) bar.dragValue = P.positionAt(mouse.x, bar.width, bar.duration)
        onReleased: bar.seekRequested(bar.dragValue)
        //: Accessible name of the seek bar
        Accessible.name: qsTr("Seek")
        Accessible.role: Accessible.Slider
    }
}
