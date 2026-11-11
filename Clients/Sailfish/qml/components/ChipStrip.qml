// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
import QtQuick 2.6
import Sailfish.Silica 1.0

// A row of ActionChips that scrolls sideways (docs/DESIGN.md §3.4), inset by the page margin: the
// watch page's actions, the search filters, a playlist's actions. The chips are the strip's
// children. In right-to-left layouts the first chip sits at the right end and the strip starts
// there (docs/DESIGN.md §7.8).
SilicaFlickable {
    id: strip
    default property alias chips: row.data
    // No chip shows: the strip takes no room (it stays visible, so its chips can show again).
    readonly property bool empty: row.visibleChildren.length === 0
    readonly property bool mirrored: strip.LayoutMirroring.enabled
    width: parent ? parent.width : 0
    height: empty ? 0 : Theme.itemSizeSmall
    // At least the strip's own width, so a short row keeps to the leading edge in both directions.
    contentWidth: Math.max(width, row.width + 2 * Theme.horizontalPageMargin)
    flickableDirection: Flickable.HorizontalFlick
    boundsBehavior: Flickable.StopAtBounds
    clip: true

    // The leading end: the left edge, or the right edge when mirrored. Never while the user is
    // moving the strip (a chip that changes its text would pull it from under the finger).
    function toStart() { if (!moving) contentX = mirrored ? Math.max(0, contentWidth - width) : 0 }
    onMirroredChanged: toStart()
    onContentWidthChanged: if (mirrored) toStart()
    onWidthChanged: if (mirrored) toStart()
    Component.onCompleted: toStart()

    Row {
        id: row
        // Anchored, so mirroring moves it to the right end of the content.
        anchors { left: parent.left; leftMargin: Theme.horizontalPageMargin; verticalCenter: parent.verticalCenter }
        spacing: Theme.paddingMedium
    }
}
