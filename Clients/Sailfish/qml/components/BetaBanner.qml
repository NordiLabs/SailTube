// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Time-limited beta warning (docs/BETA.md): shown from 14 days before the end, at most once a day,
// and on every launch in the last 3 days. Loaded by the shell only in beta builds.
import QtQuick 2.6
import Sailfish.Silica 1.0

BackgroundItem {
    id: banner
    width: parent ? parent.width : Screen.width
    height: Math.max(Theme.itemSizeSmall, text.height + 2 * Theme.paddingMedium)
    anchors.top: parent ? parent.top : undefined
    z: 1000
    onClicked: beta.dismissBanner()

    Rectangle {
        anchors.fill: parent
        color: Theme.rgba(Theme.highlightBackgroundColor, 0.9)
    }
    Label {
        id: text
        anchors {
            left: parent.left; leftMargin: Theme.horizontalPageMargin
            right: close.left; rightMargin: Theme.paddingMedium
            verticalCenter: parent.verticalCenter
        }
        wrapMode: Text.Wrap
        font.pixelSize: Theme.fontSizeSmall
        color: Theme.primaryColor
        //: Beta warning banner; %n is the number of days left, %1 the end date ("30 Dec 2026")
        text: qsTr("This beta ends in %n day(s), on %1.", "", beta.daysLeft).arg(beta.expiryText)
    }
    IconButton {
        id: close
        anchors { right: parent.right; rightMargin: Theme.paddingSmall; verticalCenter: parent.verticalCenter }
        icon.source: "image://theme/icon-m-clear"
        //: Accessible name of the button that hides the beta warning for today
        Accessible.name: qsTr("Dismiss")
        onClicked: beta.dismissBanner()
    }
}
