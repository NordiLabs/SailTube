// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
import QtQuick 2.6
import Sailfish.Silica 1.0

// Sub-page of the settings row "Download folder" (SET-01 downloads/folder, DL-06) with the
// DL-07 offline extras.
Page {
    id: page
    allowedOrientations: Orientation.All
    property bool miniPlayerOptOut: true

    SilicaFlickable {
        anchors.fill: parent
        contentHeight: column.height

        Column {
            id: column
            width: parent.width
            PageHeader {
                //: Title of the download folder page
                title: qsTr("Download folder")
                //: Subtitle of the download folder page, which also holds the offline extras
                description: qsTr("And what is saved with each download")
            }
            Loader {
                width: parent.width
                active: typeof downloads !== "undefined"
                source: "DownloadSettingsSection.qml"
                onLoaded: item.showUnmetered = false
            }
            Label {
                x: Theme.horizontalPageMargin
                width: parent.width - 2 * x
                visible: typeof downloads === "undefined"
                wrapMode: Text.Wrap
                color: Theme.secondaryHighlightColor
                //: Shown on the download folder page in a build without downloads
                text: qsTr("Downloads are not available in this build.")
            }
        }
        VerticalScrollDecorator { }
    }
}
