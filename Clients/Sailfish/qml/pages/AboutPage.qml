// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
import QtQuick 2.6
import Sailfish.Silica 1.0

// About page. In the time-limited beta (docs/BETA.md) it follows the same layout as SailTune's
// "About this beta": detail rows, what happens at the end date, how updates work, and a link to
// the download page.
Page {
    id: page
    allowedOrientations: Orientation.All
    readonly property bool isBeta: typeof beta !== "undefined"
    // Comes from the beta object (compiled only in beta builds), so no host is named in shared QML.
    readonly property string downloadUrl: isBeta ? beta.downloadUrl : ""

    SilicaFlickable {
        anchors.fill: parent
        contentHeight: column.height + Theme.paddingLarge

        Column {
            id: column
            width: parent.width
            spacing: Theme.paddingMedium

            PageHeader {
                //: About page title in beta builds
                title: page.isBeta ? qsTr("About this beta") : "SailTube" // i18n-exempt: app name
            }

            DetailItem {
                //: About page detail row label: the app version
                label: qsTr("Version")
                value: page.isBeta ? beta.buildText : appVersion
            }
            DetailItem {
                visible: page.isBeta
                //: About page detail row label: the date this beta stops working
                label: qsTr("Ends")
                value: page.isBeta ? beta.expiryText : ""
            }

            Label {
                visible: page.isBeta
                x: Theme.horizontalPageMargin
                width: parent.width - 2 * x
                wrapMode: Text.Wrap
                //: About page paragraph in beta builds
                text: qsTr("This is a test version. After the end date it opens only a page that lets you export a backup; your subscriptions, playlists, history and downloads are never deleted or changed by it.")
            }
            Label {
                visible: page.isBeta
                x: Theme.horizontalPageMargin
                width: parent.width - 2 * x
                wrapMode: Text.Wrap
                color: Theme.secondaryColor
                //: About page paragraph in beta builds
                text: qsTr("Updates: install the next beta or the stable version from the place you got this one. SailTube does not update itself and sends nothing about you or your use.")
            }
            Button {
                visible: page.isBeta
                anchors.horizontalCenter: parent.horizontalCenter
                //: About page button opening the download page in the browser
                text: qsTr("Open the download page")
                onClicked: Qt.openUrlExternally(page.downloadUrl)
            }
            Label {
                visible: page.isBeta
                anchors.horizontalCenter: parent.horizontalCenter
                width: Math.min(implicitWidth, parent.width - 2 * Theme.horizontalPageMargin)
                truncationMode: TruncationMode.Fade
                font.pixelSize: Theme.fontSizeExtraSmall
                color: Theme.secondaryColor
                text: page.downloadUrl
            }

            SectionHeader { text: "SailTube" } // i18n-exempt: app name

            Label {
                x: Theme.horizontalPageMargin
                width: parent.width - 2 * x
                wrapMode: Text.Wrap
                font.pixelSize: Theme.fontSizeSmall
                //: Short description on the about page
                text: qsTr("A native YouTube video client for Sailfish OS. Video data is resolved on the device by the packaged yt-dlp; no account is needed.")
            }
            Label {
                x: Theme.horizontalPageMargin
                width: parent.width - 2 * x
                wrapMode: Text.Wrap
                font.pixelSize: Theme.fontSizeExtraSmall
                color: Theme.secondaryColor
                //: License notice on the about page
                text: qsTr("SailTube is offered under the PolyForm Noncommercial License 1.0.0. Bundled components keep their own licenses; their notices are installed in /usr/share/harbour-sailtube/licenses.")
            }
            Label {
                x: Theme.horizontalPageMargin
                width: parent.width - 2 * x
                wrapMode: Text.Wrap
                font.pixelSize: Theme.fontSizeExtraSmall
                color: Theme.secondaryColor
                //: Disclaimer on the about page
                text: qsTr("SailTube is an independent project and is not affiliated with or endorsed by YouTube or Google.")
            }
        }
        VerticalScrollDecorator { }
    }
}
