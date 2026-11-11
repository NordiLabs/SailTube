// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
import QtQuick 2.6
import Sailfish.Silica 1.0
import Sailfish.Pickers 1.0

// ACC-04: paste a Cookie header, copied request headers, a cURL command or a
// Netscape cookies.txt, or load such a file. A text without a sign-in is
// refused and nothing is stored.
Dialog {
    id: dialog
    canAccept: area.text.length > 0

    Component {
        id: filePicker
        FilePickerPage {
            //: Title of the file picker for a cookies file
            title: qsTr("Choose a cookies file")
            nameFilters: ["*.txt", "*.json", "*"]
            onSelectedContentPropertiesChanged: {
                var path = selectedContentProperties.filePath
                if (path && account.importFile(path)) pageStack.pop(pageStack.find(function(p) { return p.objectName === "accountPage" }) || null)
            }
        }
    }

    Column {
        width: parent.width
        spacing: Theme.paddingMedium
        DialogHeader {
            //: Accept button of the paste dialog
            acceptText: qsTr("Import")
        }
        TextArea {
            id: area
            width: parent.width
            height: Math.max(Theme.itemSizeHuge * 2, implicitHeight)
            inputMethodHints: Qt.ImhNoPredictiveText | Qt.ImhNoAutoUppercase
            //: Placeholder of the paste field
            placeholderText: qsTr("Paste cookies here")
            //: Label of the paste field
            label: qsTr("Cookies")
        }
        Button {
            anchors.horizontalCenter: parent.horizontalCenter
            //: Button loading cookies from a file
            text: qsTr("Load from a file")
            onClicked: pageStack.push(filePicker)
        }
        Label {
            x: Theme.horizontalPageMargin
            width: parent.width - 2 * x
            visible: account.lastError !== ""
            wrapMode: Text.Wrap
            color: Theme.errorColor
            font.pixelSize: Theme.fontSizeSmall
            text: account.lastError
        }
    }
    onAccepted: {
        account.importText(area.text)
        area.text = ""   // the secret does not stay in the field
    }
}
