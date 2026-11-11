// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
import QtQuick 2.6
import Sailfish.Silica 1.0
import "../logic/Rows.js" as Rows

// The rows of SkeletonList (loaded only while a list shows its skeleton). Loading skeleton
// (NFR-PERF-01, docs/DESIGN.md §5.1): placeholder rows with the geometry of
// VideoListItem, shown while a list loads its first page and has nothing cached, so the page is
// never blank and the real rows land where the shapes were. Shapes use the same placeholder tint
// as a thumbnail that is still decoding; one slow opacity pulse on the whole column (render
// thread) runs only while it is visible and the app is in the foreground. Takes no input.
Item {
    id: skeleton
    property bool shown: false
    // Height of the view to cover.
    property real viewHeight: 0
    // Cards (§4.1) for lists that show cards on portrait-wide rows.
    property bool cards: false
    readonly property bool card: Rows.useCard(cards, width, Screen.width)
    readonly property real thumbW: card ? width : Rows.thumbWidth(width, Screen.width)
    readonly property real rowHeight: card
        ? Rows.thumbHeight(thumbW) + Theme.paddingMedium + Theme.iconSizeMedium + Theme.paddingLarge
        : Rows.thumbHeight(thumbW) + 2 * Theme.paddingMedium
    readonly property int rows: shown ? Rows.skeletonCount(viewHeight, rowHeight) : 0

    visible: shown
    enabled: false
    width: parent ? parent.width : 0
    // Explicit geometry rather than a positioner: the list header knows its height at once.
    height: rows * rowHeight

    Repeater {
        model: skeleton.rows
        Item {
            y: index * skeleton.rowHeight
            width: skeleton.width
            height: skeleton.rowHeight
            Rectangle {
                id: thumb
                anchors { left: parent.left; leftMargin: skeleton.card ? 0 : Theme.horizontalPageMargin }
                y: skeleton.card ? 0 : Theme.paddingMedium
                width: skeleton.thumbW
                height: Rows.thumbHeight(width)
                color: Theme.rgba(Theme.highlightBackgroundColor, 0.1)
            }
            Rectangle {
                id: avatarShape
                visible: skeleton.card
                anchors { left: parent.left; leftMargin: Theme.horizontalPageMargin; top: thumb.bottom; topMargin: Theme.paddingMedium }
                width: skeleton.card ? Theme.iconSizeMedium : 0
                height: width
                radius: width / 2
                color: Theme.rgba(Theme.highlightBackgroundColor, 0.1)
            }
            Column {
                anchors {
                    left: skeleton.card ? avatarShape.right : thumb.right; leftMargin: Theme.paddingMedium
                    right: parent.right; rightMargin: Theme.horizontalPageMargin
                    top: skeleton.card ? thumb.bottom : thumb.top; topMargin: skeleton.card ? Theme.paddingMedium : Theme.paddingSmall
                }
                spacing: Theme.paddingMedium
                Rectangle {
                    width: parent.width * 0.92
                    height: Math.round(Theme.fontSizeSmall * 0.7)
                    radius: height / 4
                    color: Theme.rgba(Theme.primaryColor, 0.1)
                }
                Rectangle {
                    width: parent.width * 0.6
                    height: Math.round(Theme.fontSizeSmall * 0.7)
                    radius: height / 4
                    color: Theme.rgba(Theme.primaryColor, 0.1)
                }
                Rectangle {
                    width: parent.width * 0.4
                    height: Math.round(Theme.fontSizeExtraSmall * 0.7)
                    radius: height / 4
                    color: Theme.rgba(Theme.secondaryColor, 0.1)
                }
            }
        }
    }

    SequentialAnimation on opacity {
        running: skeleton.visible && Qt.application.state === Qt.ApplicationActive
        loops: Animation.Infinite
        alwaysRunToEnd: true
        OpacityAnimator { from: 1.0; to: 0.55; duration: 700; easing.type: Easing.InOutQuad }
        OpacityAnimator { from: 0.55; to: 1.0; duration: 700; easing.type: Easing.InOutQuad }
    }
}
