// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
import QtQuick 2.6
import Sailfish.Silica 1.0
import "../components"

// Library hub (docs/DESIGN.md §4.7), as the official app's "You" tab: a History shelf and a
// Playlists shelf that scroll sideways (each header opens the full page), then one row per
// collection. A row whose shelf is showing is left out, so nothing is listed twice; with an empty
// history or no playlists the rows are all there. Signed in, the account's Watch later and Liked
// lists have their rows too. The library component adds each page under pages/.
Item {
    // Not `section`: inside a ListView that name is the view's own section property, so the
    // shelves below would read the wrong object.
    id: hub
    property Item mainPage

    // SessionState: 2 signed in, 4 unreachable with a session (signed requests still allowed).
    // A property, so tests can sign in.
    property bool signedIn: typeof account !== "undefined" && (account.state === 2 || account.state === 4)
    //: Detail line of the Library rows that open a list of the signed-in YouTube account
    readonly property string accountDetail: qsTr("From your YouTube account")

    readonly property var rows: [
        //: Library row: the user's playlists
        { title: qsTr("Playlists"), detail: qsTr("Saved and local playlists"), icon: "image://theme/icon-m-media-playlists", page: "PlaylistsPage.qml", shelf: "playlists" },
        //: Library row: videos saved to watch later
        { title: qsTr("Watch later"), detail: qsTr("Videos you saved for later"), icon: "image://theme/icon-m-favorite", page: "PlaylistPage.qml", props: { playlistId: "watch-later" } },
        // ACC-11: the signed-in account's own lists, shown only while signed in (the catalog's
        // preview page sends WL and LL signed).
        //: Library entry: the Watch later list of the signed-in YouTube account
        { title: qsTranslate("LibraryPage", "YouTube Watch later"), detail: hub.accountDetail, icon: "image://theme/icon-m-clock",
          page: "PlaylistPreviewPage.qml", props: { playlistId: "WL" }, account: true },
        //: Library entry: the liked videos of the signed-in YouTube account
        { title: qsTranslate("LibraryPage", "Liked videos"), detail: hub.accountDetail, icon: "image://theme/icon-m-like",
          page: "PlaylistPreviewPage.qml", props: { playlistId: "LL" }, account: true },
        //: Library row: watch history
        { title: qsTr("History"), detail: qsTr("Videos you watched, with resume points"), icon: "image://theme/icon-m-history", page: "HistoryPage.qml", shelf: "history" },
        //: Library row: subscribed channels
        { title: qsTr("Channels"), detail: qsTr("Your subscriptions"), icon: "image://theme/icon-m-people", page: "SubscriptionsPage.qml" },
        //: Library row: downloaded videos
        { title: qsTr("Downloads"), detail: qsTr("Videos and audio saved on this device"), icon: "image://theme/icon-m-cloud-download", page: "DownloadsPage.qml" },
        //: Library row: import from other apps and back up the library
        { title: qsTr("Import and export"), detail: qsTr("NewPipe, Takeout, OPML and SailTube backups"), icon: "image://theme/icon-m-transfer", page: "ImportExportPage.qml" }
    ]

    readonly property bool historyShelf: typeof historyModel !== "undefined" && historyModel.count > 0
    readonly property bool playlistsShelf: typeof playlistsModel !== "undefined" && playlistsModel.count > 0
    readonly property var visibleRows: {
        var out = []
        for (var i = 0; i < rows.length; ++i) {
            if (rows[i].shelf === "history" && historyShelf) continue
            if (rows[i].shelf === "playlists" && playlistsShelf) continue
            if (rows[i].account === true && !signedIn) continue
            out.push(rows[i])
        }
        return out
    }

    function scrollToTop() { list.scrollToTop() }

    SilicaListView {
        id: list
        anchors.fill: parent
        model: hub.visibleRows

        // Every section has a header now that the navigation bar sits at the bottom; the shelves
        // sit under it and scroll away with the list.
        header: Column {
            width: list.width
            PageHeader {
                //: Main page section with playlists, history, watch later and downloads
                title: qsTranslate("MainPage", "Library")
            }

            // ---- History shelf ----
            BackgroundItem {
                id: historyHeader
                objectName: "historyShelfHeader"
                visible: hub.historyShelf
                width: parent.width
                height: visible ? Theme.itemSizeSmall : 0
                onClicked: pageStack.push(Qt.resolvedUrl("../pages/HistoryPage.qml"))
                Label {
                    anchors { left: parent.left; leftMargin: Theme.horizontalPageMargin; right: historyArrow.left; verticalCenter: parent.verticalCenter }
                    //: Library row: watch history
                    text: qsTr("History")
                    truncationMode: TruncationMode.Fade
                    color: historyHeader.highlighted ? Theme.highlightColor : Theme.primaryColor
                }
                Icon {
                    id: historyArrow
                    anchors { right: parent.right; rightMargin: Theme.horizontalPageMargin; verticalCenter: parent.verticalCenter }
                    source: "image://theme/icon-m-right"
                    // Points forward in right-to-left layouts too.
                    mirror: Qt.application.layoutDirection === Qt.RightToLeft
                    highlighted: historyHeader.highlighted
                }
            }
            ListView {
                id: historyShelfView
                objectName: "historyShelf"
                visible: hub.historyShelf
                width: parent.width
                height: visible ? historyProbe.height + Theme.paddingMedium : 0
                orientation: ListView.Horizontal
                boundsBehavior: Flickable.StopAtBounds
                spacing: Theme.paddingMedium
                clip: true
                model: hub.historyShelf ? historyModel : null
                header: Item { width: Theme.horizontalPageMargin; height: 1 }
                footer: Item { width: Theme.horizontalPageMargin; height: 1 }
                delegate: ShelfTile {
                    title: model.title || ""
                    thumbnailUrl: model.thumbnail || ""
                    progress: model.watched === true ? 1 : (model.fraction !== undefined ? model.fraction : -1)
                    onClicked: window.openVideo(model.videoId, { title: model.title, channel: model.channel,
                                                                 channelId: model.channelId, thumbnail: model.thumbnail })
                }
                // Gives the shelf its height before any tile exists.
                ShelfTile { id: historyProbe; visible: false }
            }

            // ---- Playlists shelf ----
            BackgroundItem {
                id: playlistsHeader
                objectName: "playlistsShelfHeader"
                visible: hub.playlistsShelf
                width: parent.width
                height: visible ? Theme.itemSizeSmall : 0
                onClicked: pageStack.push(Qt.resolvedUrl("../pages/PlaylistsPage.qml"))
                Label {
                    anchors { left: parent.left; leftMargin: Theme.horizontalPageMargin; right: playlistsArrow.left; verticalCenter: parent.verticalCenter }
                    //: Library row: the user's playlists
                    text: qsTr("Playlists")
                    truncationMode: TruncationMode.Fade
                    color: playlistsHeader.highlighted ? Theme.highlightColor : Theme.primaryColor
                }
                Icon {
                    id: playlistsArrow
                    anchors { right: parent.right; rightMargin: Theme.horizontalPageMargin; verticalCenter: parent.verticalCenter }
                    source: "image://theme/icon-m-right"
                    // Points forward in right-to-left layouts too.
                    mirror: Qt.application.layoutDirection === Qt.RightToLeft
                    highlighted: playlistsHeader.highlighted
                }
            }
            ListView {
                objectName: "playlistsShelf"
                visible: hub.playlistsShelf
                width: parent.width
                height: visible ? historyProbe.height + Theme.paddingMedium : 0
                orientation: ListView.Horizontal
                boundsBehavior: Flickable.StopAtBounds
                spacing: Theme.paddingMedium
                clip: true
                model: hub.playlistsShelf ? playlistsModel : null
                header: Item { width: Theme.horizontalPageMargin; height: 1 }
                footer: Item { width: Theme.horizontalPageMargin; height: 1 }
                delegate: ShelfTile {
                    //: Library row: videos saved to watch later
                    title: model.watchLater === true ? qsTr("Watch later") : (model.title || "")
                    thumbnailUrl: model.thumbnail || ""
                    badge: model.itemCount !== undefined && model.itemCount >= 0 ? String(model.itemCount) : ""
                    onClicked: pageStack.push(Qt.resolvedUrl("../pages/PlaylistPage.qml"), { playlistId: model.playlistId })
                }
            }
        }

        PullDownMenu {
            MenuItem {
                //: Menu entry opening the about page
                text: qsTr("About SailTube")
                onClicked: pageStack.push(Qt.resolvedUrl("../pages/AboutPage.qml"))
            }
            MenuItem {
                //: Menu entry opening the application settings
                text: qsTr("Settings")
                onClicked: pageStack.push(Qt.resolvedUrl("../pages/SettingsPage.qml"))
            }
        }

        delegate: ListItem {
            id: row
            readonly property bool available: modelData.page !== ""
            // Grows with the system text size (docs/DESIGN.md §7.9).
            contentHeight: Math.max(Theme.itemSizeMedium, texts.height + 2 * Theme.paddingMedium)
            enabled: available
            onClicked: pageStack.push(Qt.resolvedUrl("../pages/" + modelData.page), modelData.props || {})

            Icon {
                id: icon
                // Anchored, not x:, so right-to-left layouts mirror it (I18N-03).
                anchors.left: parent.left
                anchors.leftMargin: Theme.horizontalPageMargin
                anchors.verticalCenter: parent.verticalCenter
                source: modelData.icon
                highlighted: row.highlighted
                opacity: row.available ? 1.0 : Theme.opacityLow
            }
            Column {
                id: texts
                anchors {
                    left: icon.right; leftMargin: Theme.paddingLarge
                    right: parent.right; rightMargin: Theme.horizontalPageMargin
                    verticalCenter: parent.verticalCenter
                }
                Label {
                    width: parent.width
                    text: modelData.title
                    truncationMode: TruncationMode.Fade
                    color: !row.available ? Theme.secondaryColor
                           : row.highlighted ? Theme.highlightColor : Theme.primaryColor
                }
                Label {
                    width: parent.width
                    text: row.available ? modelData.detail
                                        //: Shown under a library row whose page is not in this build yet
                                        : qsTr("Not available yet")
                    truncationMode: TruncationMode.Fade
                    font.pixelSize: Theme.fontSizeExtraSmall
                    color: row.highlighted ? Theme.secondaryHighlightColor : Theme.secondaryColor
                }
            }
        }
        VerticalScrollDecorator { }
    }
}
