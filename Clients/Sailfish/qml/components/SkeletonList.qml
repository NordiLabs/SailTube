// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
import QtQuick 2.6
import Sailfish.Silica 1.0
import "../logic/Rows.js" as Rows

// Loading skeleton of a list (NFR-PERF-01, docs/DESIGN.md §5.1). This wrapper is all a list
// pays for at start; the rows themselves (SkeletonRows.qml) are compiled and created only while
// `shown`, so a start with cached rows never builds them.
Item {
    id: skeleton
    property bool shown: false
    // Height of the view to cover.
    property real viewHeight: 0
    // The list shows cards on portrait-wide rows (§4.1): the skeleton takes their shape.
    property bool cards: false
    readonly property bool card: Rows.useCard(cards, width, Screen.width)
    readonly property real thumbW: card ? width : Rows.thumbWidth(width, Screen.width)
    readonly property real rowHeight: card
        ? Rows.thumbHeight(thumbW) + Theme.paddingMedium + Theme.iconSizeMedium + Theme.paddingLarge
        : Rows.thumbHeight(thumbW) + 2 * Theme.paddingMedium

    visible: shown
    enabled: false
    width: parent ? parent.width : 0
    height: rows.item ? rows.item.height : 0

    Loader {
        id: rows
        width: parent.width
        active: skeleton.shown
        source: active ? "SkeletonRows.qml" : ""
    }
    Binding { target: rows.item; when: rows.status === Loader.Ready; property: "shown"; value: skeleton.shown }
    Binding { target: rows.item; when: rows.status === Loader.Ready; property: "viewHeight"; value: skeleton.viewHeight }
    Binding { target: rows.item; when: rows.status === Loader.Ready; property: "cards"; value: skeleton.cards }
}
