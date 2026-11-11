// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
import QtQuick 2.6
import Sailfish.Silica 1.0

// DL-06 download folder, DL-07 offline extras (and, when `showUnmetered`, DL-02 "unmetered
// only"). Loaded by DownloadFolderPage, the sub-page of the settings row "Download folder"; the
// unmetered switch lives on the settings page (SET-02), so the page hides it here.
Column {
    id: section
    width: parent ? parent.width : 0
    property string folderError
    property bool showUnmetered: true

    SectionHeader {
        visible: section.showUnmetered
        //: Settings section about downloads
        text: qsTr("Downloads")
    }
    TextSwitch {
        visible: section.showUnmetered
        //: Setting: download only on Wi-Fi or other unmetered connections
        text: qsTr("Only on unmetered connections")
        checked: downloads.unmeteredOnly
        onClicked: downloads.unmeteredOnly = checked
    }
    TextSwitch {
        //: Setting: save the captions of a video together with its download
        text: qsTr("Save captions with downloads")
        //: Description of the setting that saves captions with downloads
        description: qsTr("Captions show when the video plays offline")
        checked: downloads.saveCaptions
        onClicked: downloads.saveCaptions = checked
    }
    TextSwitch {
        //: Setting: save the SponsorBlock segments of a video together with its download
        text: qsTr("Save SponsorBlock segments with downloads")
        //: Description of the setting that saves SponsorBlock segments with downloads
        description: qsTr("Segments are skipped when the video plays offline")
        checked: downloads.saveSegments
        onClicked: downloads.saveSegments = checked
    }
    ComboBox {
        id: folderBox
        property bool ready: false
        readonly property var folders: [downloads.defaultFolder].concat(downloads.candidateFolders())
        //: Setting: where downloads are saved
        label: qsTr("Download folder")
        value: downloads.folder === "" || downloads.folder === downloads.defaultFolder
               //: Download folder choice: the app's private folder
               ? qsTr("App folder") : downloads.folder
        menu: ContextMenu {
            Repeater {
                model: folderBox.folders
                //: Download folder choice: the app's own folder
                MenuItem { text: index === 0 ? qsTr("App folder") : modelData }
            }
        }
        onCurrentIndexChanged: {
            if (!ready) return
            if (currentIndex <= 0) { downloads.resetFolder(); section.folderError = ""; return }
            var r = downloads.chooseFolder(folders[currentIndex])
            section.folderError = r.ok ? "" : r.error
        }
        Component.onCompleted: ready = true
    }
    Label {
        x: Theme.horizontalPageMargin
        width: parent.width - 2 * x
        visible: text !== ""
        text: section.folderError
        wrapMode: Text.Wrap
        font.pixelSize: Theme.fontSizeExtraSmall
        color: Theme.errorColor
    }
}
