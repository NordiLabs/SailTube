// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
import QtQuick 2.6
import Sailfish.Silica 1.0
import "../logic/SettingsInventory.js" as Inventory

// A settings row that opens a sub-page (equalizer, SponsorBlock, captions, downloads). Owning
// components set `page` when the sub-page exists; until then the row is disabled with a note.
BackgroundItem {
    id: row
    property string settingKey
    // False for a row that is hidden until an engine reads its key (SettingsInventory.HIDDEN_KEYS).
    // A page that sets `visible` itself adds `&& inUse`.
    readonly property bool inUse: !Inventory.isHidden(settingKey)
    visible: inUse
    property string page: ""
    property alias text: title.text
    property string description: ""
    property bool flash: false
    readonly property bool available: page !== ""

    width: parent ? parent.width : implicitWidth
    height: Math.max(Theme.itemSizeMedium, column.height + 2 * Theme.paddingMedium)
    enabled: available
    highlighted: down || flash
    onClicked: pageStack.push(Qt.resolvedUrl("../pages/" + page))

    Column {
        id: column
        anchors {
            left: parent.left; leftMargin: Theme.horizontalPageMargin
            right: arrow.left; rightMargin: Theme.paddingMedium
            verticalCenter: parent.verticalCenter
        }
        Label {
            id: title
            width: parent.width
            wrapMode: Text.Wrap
            color: !row.available ? Theme.secondaryColor : row.highlighted ? Theme.highlightColor : Theme.primaryColor
        }
        Label {
            width: parent.width
            wrapMode: Text.Wrap
            font.pixelSize: Theme.fontSizeExtraSmall
            color: row.highlighted ? Theme.secondaryHighlightColor : Theme.secondaryColor
            text: row.available ? row.description
                                //: Shown under a settings row whose page is not in this build yet
                                : qsTr("Not available yet")
            visible: text !== ""
        }
    }
    Icon {
        id: arrow
        anchors { right: parent.right; rightMargin: Theme.horizontalPageMargin; verticalCenter: parent.verticalCenter }
        source: "image://theme/icon-m-right"
        visible: row.available
        // Points the other way in right-to-left layouts.
        mirror: row.LayoutMirroring.enabled
    }
}
