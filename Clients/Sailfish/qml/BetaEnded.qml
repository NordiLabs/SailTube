// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// The only page of an expired beta build (docs/BETA.md): nothing else is restored or contacted.
// Export backup stays available; user data is never deleted or modified.
import QtQuick 2.6
import Sailfish.Silica 1.0
import Sailfish.Pickers 1.0

ApplicationWindow {
    allowedOrientations: defaultAllowedOrientations
    Component {
        id: folderPicker
        FolderPickerPage {
            //: Title of the folder picker for the backup on the expired-beta page
            dialogTitle: qsTr("Save backup in")
            onSelectedPathChanged: if (selectedPath) importer.exportBackup(selectedPath)
        }
    }
    initialPage: Component {
        Page {
            SilicaFlickable {
                anchors.fill: parent
                contentHeight: column.height + Theme.paddingLarge

                Column {
                    id: column
                    width: parent.width
                    spacing: Theme.paddingLarge

                    PageHeader {
                        //: Title of the page shown when the beta build has expired
                        title: qsTr("This beta has ended")
                    }
                    Label {
                        x: Theme.horizontalPageMargin
                        width: parent.width - 2 * x
                        wrapMode: Text.Wrap
                        color: Theme.highlightColor
                        //: %1 is the end date ("30 Dec 2026")
                        text: qsTr("This beta of SailTube ended on %1. Install the current version to keep using it; your library, history and settings stay as they are.")
                              .arg(typeof beta !== "undefined" ? beta.expiryText : "")
                    }
                    Label {
                        x: Theme.horizontalPageMargin
                        width: parent.width - 2 * x
                        wrapMode: Text.Wrap
                        font.pixelSize: Theme.fontSizeSmall
                        color: Theme.secondaryHighlightColor
                        //: %1 is the build, e.g. 0.1.0-0.beta1
                        text: qsTr("Build %1").arg(typeof beta !== "undefined" ? beta.buildText : "")
                    }
                    Button {
                        anchors.horizontalCenter: parent.horizontalCenter
                        enabled: typeof importer !== "undefined" && !importer.busy
                        //: Button on the expired-beta page; saves a backup of the library
                        text: qsTr("Export backup")
                        onClicked: pageStack.push(folderPicker)
                    }
                    Label {
                        x: Theme.horizontalPageMargin
                        width: parent.width - 2 * x
                        wrapMode: Text.Wrap
                        font.pixelSize: Theme.fontSizeSmall
                        color: Theme.secondaryColor
                        visible: typeof importer !== "undefined" && (importer.lastExportPath !== "" || importer.errorText !== "")
                        text: typeof importer === "undefined" ? ""
                              : importer.errorText !== "" ? importer.errorText
                              //: %1 is the file the backup was written to
                              : qsTr("Saved to %1").arg(importer.lastExportPath)
                    }
                }
            }
        }
    }
}
