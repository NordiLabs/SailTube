// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
import QtQuick 2.6
import Sailfish.Silica 1.0

// Name entry for a new or renamed local playlist (title at most 100 characters).
Dialog {
    id: dialog
    property string name
    property string heading
    canAccept: true

    Column {
        width: parent.width
        DialogHeader { title: dialog.heading }
        TextField {
            id: field
            width: parent.width
            text: dialog.name
            //: Placeholder of the playlist name field
            placeholderText: qsTr("Playlist name")
            label: placeholderText
            maximumLength: 100
            focus: true
            EnterKey.iconSource: "image://theme/icon-m-enter-accept"
            EnterKey.onClicked: dialog.accept()
        }
    }
    onAccepted: name = field.text
}
