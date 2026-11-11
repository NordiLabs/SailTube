// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
import QtQuick 2.6
import Sailfish.Silica 1.0
import Sailfish.Pickers 1.0

// IMP-01/03/04: imports need no account and show one summary at the end.
Page {
    id: page
    property string pendingKind
    property bool applySettings: true

    Component {
        id: filePicker
        FilePickerPage {
            //: Title of the file picker for an import
            title: qsTr("Choose a file to import")
            nameFilters: page.pendingKind === "newpipe" ? ["*.zip", "*.db"]
                         : page.pendingKind === "takeout" ? ["*.csv", "*.zip"]
                         : page.pendingKind === "opml" ? ["*.opml", "*.xml"]
                         : ["*.json"]
            onSelectedContentPropertiesChanged: {
                var path = selectedContentProperties.filePath
                if (path) importer.startImport(page.pendingKind, path, page.applySettings)
            }
        }
    }
    Component {
        id: folderPicker
        FolderPickerPage {
            //: Title of the folder picker for a backup export
            dialogTitle: qsTr("Save backup in")
            onSelectedPathChanged: if (selectedPath) importer.exportBackup(selectedPath)
        }
    }

    function choose(kind) {
        pendingKind = kind
        pageStack.push(filePicker)
    }

    SilicaFlickable {
        anchors.fill: parent
        contentHeight: column.height

        Column {
            id: column
            width: parent.width
            spacing: Theme.paddingMedium

            PageHeader {
                //: Title of the import and export page
                title: qsTr("Import and export")
            }

            Label {
                x: Theme.horizontalPageMargin
                width: parent.width - 2 * x
                visible: importer.busy || importer.summaryText() !== ""
                wrapMode: Text.Wrap
                color: importer.errorText !== "" ? Theme.errorColor : Theme.highlightColor
                text: importer.busy
                      //: Shown while an import or export runs
                      ? qsTr("Working…")
                      : importer.summaryText()
                // Re-evaluate summaryText() when the controller changes.
                property var trigger: importer.summary
            }
            BusyIndicator {
                anchors.horizontalCenter: parent.horizontalCenter
                running: importer.busy
                visible: running
                size: BusyIndicatorSize.Medium
            }

            SectionHeader {
                //: Section of the import page
                text: qsTr("Import")
            }
            Button {
                // IMP-02: shown once the account component has a signed-in session.
                visible: importer.accountImportAvailable
                anchors.horizontalCenter: parent.horizontalCenter
                enabled: !importer.busy
                //: Imports the subscriptions and playlists of the signed-in YouTube account
                text: qsTr("From the YouTube account")
                onClicked: importer.startAccountImport()
            }
            Button {
                anchors.horizontalCenter: parent.horizontalCenter
                enabled: !importer.busy
                //: Imports a NewPipe or PipePipe backup file
                text: qsTr("NewPipe or PipePipe backup")
                onClicked: page.choose("newpipe")
            }
            Button {
                anchors.horizontalCenter: parent.horizontalCenter
                enabled: !importer.busy
                //: Imports a Google Takeout subscriptions.csv file
                text: qsTr("Google Takeout subscriptions")
                onClicked: page.choose("takeout")
            }
            Button {
                anchors.horizontalCenter: parent.horizontalCenter
                enabled: !importer.busy
                //: Imports an OPML list of channel feeds
                text: qsTr("OPML feed list")
                onClicked: page.choose("opml")
            }

            SectionHeader {
                //: Section of the import page about SailTube's own backup
                text: qsTr("SailTube backup")
            }
            Button {
                anchors.horizontalCenter: parent.horizontalCenter
                enabled: !importer.busy
                //: Exports subscriptions, playlists, history and settings to a file
                text: qsTr("Export backup")
                onClicked: pageStack.push(folderPicker)
            }
            Button {
                anchors.horizontalCenter: parent.horizontalCenter
                enabled: !importer.busy
                //: Restores a SailTube backup file
                text: qsTr("Restore backup")
                onClicked: page.choose("backup")
            }
            TextSwitch {
                //: Option of the backup restore
                text: qsTr("Also restore settings")
                checked: page.applySettings
                onClicked: page.applySettings = checked
            }
            Label {
                x: Theme.horizontalPageMargin
                width: parent.width - 2 * x
                wrapMode: Text.Wrap
                font.pixelSize: Theme.fontSizeExtraSmall
                color: Theme.secondaryColor
                //: Explanation on the import page
                text: qsTr("Imports never replace what is already in your library. Running one again adds nothing twice. Backups contain no sign-in data.")
            }
            Item { width: 1; height: Theme.paddingLarge }
        }
        VerticalScrollDecorator { }
    }
}
