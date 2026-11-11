// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
import QtQuick 2.6
import Sailfish.Silica 1.0
import "../logic/Rows.js" as Rows

// The buttons of PlaceholderActions (docs/DESIGN.md §2.2, §5.3), loaded only once a list has
// failed: Retry when the list can reload, and "Open downloads" when videos are saved.
Column {
    id: actions
    property string listState
    property string errorText
    property bool canRetry: false
    property int savedCount: 0
    signal retry()
    spacing: Theme.paddingLarge

    Button {
        anchors.horizontalCenter: parent.horizontalCenter
        visible: actions.canRetry
        //: Button under a playback error that tries the video again
        text: qsTranslate("VideoPage", "Retry")
        onClicked: actions.retry()
    }
    Button {
        anchors.horizontalCenter: parent.horizontalCenter
        visible: Rows.showDownloadsShortcut(actions.listState, actions.errorText, actions.savedCount)
        //: Button under a failed list (usually offline) that opens the downloaded videos
        text: qsTr("Open downloads")
        onClicked: pageStack.push(Qt.resolvedUrl("../pages/DownloadsPage.qml"))
    }
}
