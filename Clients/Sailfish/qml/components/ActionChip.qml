// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
import QtQuick 2.6
import Sailfish.Silica 1.0

// One action of the watch page's chip row (docs/DESIGN.md §3.4), as on the official app: a pill
// with an icon and a short label on Silica's highlight tint. A chip without an action (the like
// count) is shown but takes no taps.
BackgroundItem {
    id: chip
    property alias iconSource: icon.source
    property alias text: label.text
    property bool passive: false
    // Turns the icon (degrees): the thumb of the like icon points down for dislikes.
    property real iconRotation: 0
    // The current choice of a group of chips (search filters).
    property bool selected: false

    width: content.width + 2 * Theme.paddingLarge
    // The touch target is a full small item (docs/DESIGN.md §7.7); the pill inside is slimmer.
    height: Theme.itemSizeSmall
    enabled: !passive
    // The pill is the highlight; Silica's rectangular one stays off.
    highlightedColor: "transparent" // theme-exempt: the pill below shows the press
    Accessible.name: label.text
    Accessible.role: passive ? Accessible.StaticText : Accessible.Button

    Rectangle {
        anchors { left: parent.left; right: parent.right; verticalCenter: parent.verticalCenter }
        height: Theme.itemSizeExtraSmall
        radius: height / 2
        color: Theme.rgba(Theme.highlightBackgroundColor, chip.down || chip.selected ? 0.4 : 0.15)
    }
    Row {
        id: content
        anchors.centerIn: parent
        spacing: Theme.paddingSmall
        Icon {
            id: icon
            anchors.verticalCenter: parent.verticalCenter
            width: Theme.iconSizeSmall
            height: width
            sourceSize.width: width
            sourceSize.height: height
            highlighted: chip.down || chip.selected
            visible: source != ""
            rotation: chip.iconRotation
        }
        Label {
            id: label
            anchors.verticalCenter: parent.verticalCenter
            font.pixelSize: Theme.fontSizeExtraSmall
            color: chip.down || chip.selected ? Theme.highlightColor : Theme.primaryColor
        }
    }
}
