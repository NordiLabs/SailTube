// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
import QtQuick 2.6
import Sailfish.Silica 1.0

// One button of the Shorts rail (docs/DESIGN.md §3.5): an icon on a dark translucent disc, so it
// reads over any picture, with the count under it. It always sits on the picture, so everything
// is drawn in the theme's light colours. A button that cannot act stays in the rail, dimmed, so
// the rail keeps its shape. `count` may be empty: the icon stands alone.
Item {
    id: action
    property alias iconSource: icon.source
    property string count: ""
    // Spoken name of the button (a screen reader gets the count with it).
    property string label: ""
    property bool inert: false
    signal clicked()

    width: Theme.itemSizeSmall
    height: disc.height + (countLabel.visible ? countLabel.height : 0) + Theme.paddingSmall
    opacity: inert ? Theme.opacityLow : 1.0

    Rectangle {
        id: disc
        anchors.horizontalCenter: parent.horizontalCenter
        width: Theme.itemSizeSmall * 0.8
        height: width
        radius: width / 2
        color: Qt.rgba(0, 0, 0, area.pressed ? 0.6 : 0.35) // theme-exempt: disc behind a rail icon
        Icon {
            id: icon
            anchors.centerIn: parent
            width: Theme.iconSizeMedium
            height: width
            sourceSize.width: width
            sourceSize.height: height
            color: Theme.lightPrimaryColor
        }
    }
    Label {
        id: countLabel
        anchors { top: disc.bottom; topMargin: Theme.paddingSmall / 2; horizontalCenter: parent.horizontalCenter }
        // Never wider than the rail: a long count shrinks a step before it is cut.
        width: Math.min(implicitWidth, parent.width + Theme.paddingLarge)
        visible: action.count !== ""
        text: action.count
        font.pixelSize: Theme.fontSizeExtraSmall
        fontSizeMode: Text.HorizontalFit
        minimumPixelSize: Theme.fontSizeTiny
        horizontalAlignment: Text.AlignHCenter
        color: Theme.lightPrimaryColor
        style: Text.Outline
        styleColor: Qt.rgba(0, 0, 0, 0.6) // theme-exempt: outline of a count over the picture
    }
    MouseArea {
        id: area
        anchors.fill: parent
        onClicked: action.clicked()
        Accessible.role: Accessible.Button
        Accessible.name: action.label
    }
}
