// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
import QtQuick 2.6
import Sailfish.Silica 1.0
import "../logic/Rows.js" as Rows

// A tile of a horizontal shelf (docs/DESIGN.md §4.7), as on the official app's "You" tab: a 16:9
// picture with an optional watched-progress line or count badge, and a two-line title under it.
BackgroundItem {
    id: tile
    property string title
    property string thumbnailUrl
    // 0..1 watched fraction (history); negative hides the line.
    property real progress: -1
    // Short text on the picture's corner (a playlist's item count).
    property string badge: ""

    width: Rows.thumbWidth(Screen.width, Screen.width)
    height: picture.height + Theme.paddingSmall + titleLabel.height + Theme.paddingSmall
    Accessible.name: title

    Rectangle {
        id: picture
        width: parent.width
        height: Rows.thumbHeight(width)
        color: Theme.rgba(Theme.highlightBackgroundColor, 0.1)
        Image {
            anchors.fill: parent
            source: tile.thumbnailUrl
            sourceSize.width: width
            asynchronous: true
            fillMode: Image.PreserveAspectCrop
            clip: true
        }
        Rectangle {
            visible: tile.badge !== ""
            anchors { right: parent.right; bottom: parent.bottom; margins: Theme.paddingSmall }
            width: badgeLabel.width + Theme.paddingSmall * 2
            height: badgeLabel.height
            radius: Theme.paddingSmall / 2
            color: Qt.rgba(0, 0, 0, 0.75) // theme-exempt: scrim over a thumbnail
            Label {
                id: badgeLabel
                anchors.centerIn: parent
                text: tile.badge
                font.pixelSize: Theme.fontSizeTiny
                color: Theme.lightPrimaryColor
            }
        }
        Rectangle {
            visible: tile.progress > 0.005
            anchors { left: parent.left; right: parent.right; bottom: parent.bottom }
            height: Math.max(2, Math.round(parent.height * 0.05))
            color: Qt.rgba(1, 1, 1, 0.35) // theme-exempt: progress track over a thumbnail
            Rectangle {
                width: parent.width * Math.min(1, Math.max(0, tile.progress))
                height: parent.height
                color: Theme.highlightColor
            }
        }
    }
    Label {
        id: titleLabel
        anchors { top: picture.bottom; topMargin: Theme.paddingSmall }
        width: parent.width
        // Two lines reserved, so every tile of a shelf has the same height.
        height: Math.ceil(Theme.fontSizeExtraSmall * 2.7)
        text: tile.title
        wrapMode: Text.Wrap
        maximumLineCount: 2
        elide: Text.ElideRight
        font.pixelSize: Theme.fontSizeExtraSmall
        color: tile.highlighted ? Theme.highlightColor : Theme.primaryColor
    }
}
