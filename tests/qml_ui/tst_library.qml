// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
import QtQuick 2.6
import Sailfish.Silica 1.0

// Library tab (docs/DESIGN.md §4.7): with history and playlists the tab shows a shelf for each
// (the header opens the full page, a tile opens the video or the playlist) and leaves their rows
// out; without them every row is listed. Search: a channel result carries Subscribe.
UiTest {
    id: t
    property var hist: mocks.historyModel
    property var lists: mocks.playlistsModel
    property var channelRowItem
    function section() {
        var l = findAll(window, function (i) { return i.hasOwnProperty("visibleRows") && i.hasOwnProperty("historyShelf") })
        return l.length ? l[0] : null
    }
    steps: [
        function () { window.selectSection("library"); return 600 },
        function () {
            var s = section()
            verify(s !== null, "library section")
            verify(!s.historyShelf && !s.playlistsShelf, "no shelves without history and playlists")
            compare(s.visibleRows.length, 6, "all six rows")
            // Signed in, the account's Watch later and Liked lists are reachable from here.
            verify(!s.signedIn, "signed out")
            s.signedIn = true
            compare(s.visibleRows.length, 8, "two account rows while signed in")
            var accountLists = s.visibleRows.filter(function (r) { return r.account === true }).map(function (r) { return r.props.playlistId })
            compare(accountLists.join(","), "WL,LL", "the account's Watch later and Liked lists")
            verify(s.visibleRows.every(function (r) { return r.title !== "" && r.detail !== "" && r.icon !== "" }), "every row has a title, a detail and an icon")
            s.signedIn = false
            compare(s.visibleRows.length, 6, "signed out again: the account rows leave")
            hist.append({ videoId: "h1", title: "Watched video", channel: "C", channelId: "", thumbnail: "", fraction: 0.4, watched: false })
            lists.append({ playlistId: "p1", title: "My list", itemCount: 3, thumbnail: "", watchLater: false })
            return 300
        },
        function () {
            var s = section()
            verify(s.historyShelf && s.playlistsShelf, "shelves appear")
            compare(s.visibleRows.length, 4, "the History and Playlists rows leave for their shelves")
            // Delegates live in the list's contentItem.
            var tiles = findAll(find(window, "objectName", "historyShelf").contentItem, function (i) { return i.hasOwnProperty("progress") && i.visible && i.title === "Watched video" })
            compare(tiles.length, 1, "history tile")
            var ptiles = findAll(find(window, "objectName", "playlistsShelf").contentItem, function (i) { return i.hasOwnProperty("badge") && i.title === "My list" })
            compare(ptiles.length, 1, "playlist tile")
            compare(ptiles[0].badge, "3", "item count on the tile")
            tiles[0].clicked(null)
            return 600
        },
        function () {
            verify(current().isWatchPage === true, "a history tile opens the video")
            verify(mocks.playback.calls.indexOf("open:h1") >= 0)
            stack().pop(undefined, PageStackAction.Immediate)
            mocks.playback.stop()
            return 300
        },
        function () {
            hist.clear(); lists.clear()
            // A channel row carries Subscribe (search results use it).
            var c = Qt.createComponent(Qt.resolvedUrl("../../Clients/Sailfish/qml/components/VideoListItem.qml"))
            t.channelRowItem = c.createObject(current(), { width: Screen.width, channelRow: true, channelId: "UC1234567890123456789012",
                                                           videoTitle: "A channel", channelName: "1M subscribers" })
            verify(t.channelRowItem !== null, "channel row: " + c.errorString())
            return 200
        },
        function () {
            var loader = find(t.channelRowItem, "objectName", "channelSubscribe")
            verify(loader !== null && loader.item !== null, "Subscribe on the channel row")
            loader.item.clicked(null)
            verify(mocks.subscriptionsModel.isSubscribed("UC1234567890123456789012"), "subscribed from the row")
            mocks.subscriptionsModel.toggle("UC1234567890123456789012", "", "")
            t.channelRowItem.destroy()
            var video = Qt.createComponent(Qt.resolvedUrl("../../Clients/Sailfish/qml/components/VideoListItem.qml"))
                          .createObject(current(), { width: Screen.width, videoTitle: "A video", channelId: "UC1234567890123456789012" })
            verify(find(video, "objectName", "channelSubscribe").item === null, "video rows do not load the button")
            video.destroy()
            window.selectSection("subscriptions")
        }
    ]
}
