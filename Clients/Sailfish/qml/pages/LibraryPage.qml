// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
import QtQuick 2.6
import Sailfish.Silica 1.0

// Entry to the local library. Everything here works without an account; the
// YouTube Watch later and Liked lists (ACC-11) appear only while signed in.
Page {
    id: page
    // SessionState: 2 signed in, 4 unreachable with a session (signed requests still allowed).
    readonly property bool signedIn: typeof account !== "undefined" && (account.state === 2 || account.state === 4)

    SilicaListView {
        anchors.fill: parent
        header: PageHeader {
            //: Title of the library page
            title: qsTr("Library")
        }
        model: ListModel {
            ListElement { key: "subscriptions" }
            ListElement { key: "watchLater" }
            ListElement { key: "playlists" }
            ListElement { key: "history" }
            ListElement { key: "downloads" }
            ListElement { key: "ytWatchLater" }
            ListElement { key: "ytLiked" }
            ListElement { key: "import" }
        }
        delegate: BackgroundItem {
            id: row
            width: parent.width
            visible: model.key === "downloads" ? typeof downloads !== "undefined"
                     : model.key === "ytWatchLater" || model.key === "ytLiked" ? page.signedIn
                     : true
            height: visible ? Theme.itemSizeMedium : 0
            function label() {
                switch (model.key) {
                //: Library entry: list of subscribed channels
                case "subscriptions": return qsTr("Subscriptions")
                //: Library entry: the built-in Watch later list
                case "watchLater": return qsTr("Watch later")
                //: Library entry: local and saved playlists
                case "playlists": return qsTr("Playlists")
                //: Library entry: watch history
                case "history": return qsTr("History")
                //: Library entry: downloaded videos
                case "downloads": return qsTr("Downloads")
                //: Library entry: the Watch later list of the signed-in YouTube account
                case "ytWatchLater": return qsTr("YouTube Watch later")
                //: Library entry: the liked videos of the signed-in YouTube account
                case "ytLiked": return qsTr("Liked videos")
                //: Library entry: import and export of the library
                default: return qsTr("Import and export")
                }
            }
            function detail() {
                switch (model.key) {
                case "subscriptions": return subscriptionsModel.count > 0 ? "" + subscriptionsModel.count : ""
                case "history": return historyModel.total > 0 ? "" + historyModel.total : ""
                case "downloads": return typeof downloads !== "undefined" && downloads.activeCount > 0
                                         ? "" + downloads.activeCount : ""
                default: return ""
                }
            }
            Label {
                anchors { left: parent.left; leftMargin: Theme.horizontalPageMargin; verticalCenter: parent.verticalCenter }
                text: row.label()
                color: row.highlighted ? Theme.highlightColor : Theme.primaryColor
            }
            Label {
                anchors { right: parent.right; rightMargin: Theme.horizontalPageMargin; verticalCenter: parent.verticalCenter }
                text: row.detail()
                color: Theme.secondaryColor
            }
            onClicked: {
                switch (model.key) {
                case "subscriptions": pageStack.push(Qt.resolvedUrl("SubscriptionsPage.qml")); break
                case "watchLater": pageStack.push(Qt.resolvedUrl("PlaylistPage.qml"), { playlistId: "watch-later" }); break
                case "playlists": pageStack.push(Qt.resolvedUrl("PlaylistsPage.qml")); break
                case "history": pageStack.push(Qt.resolvedUrl("HistoryPage.qml")); break
                case "downloads": pageStack.push(Qt.resolvedUrl("DownloadsPage.qml")); break
                // ACC-11: the catalog's preview page sends WL/LL signed (VLWL/VLLL).
                case "ytWatchLater": pageStack.push(Qt.resolvedUrl("PlaylistPreviewPage.qml"), { playlistId: "WL" }); break
                case "ytLiked": pageStack.push(Qt.resolvedUrl("PlaylistPreviewPage.qml"), { playlistId: "LL" }); break
                default: pageStack.push(Qt.resolvedUrl("ImportExportPage.qml"))
                }
            }
        }
        VerticalScrollDecorator { }
    }
}
