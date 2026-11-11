// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
import QtQuick 2.6
import Sailfish.Silica 1.0

// Home's "Continue watching" shelf (docs/DESIGN.md §4.9): absent (not even loaded) without an
// unfinished video; with some, it lists the started and not yet watched videos, newest first; a
// tile opens the video and the header opens the history.
UiTest {
    id: t
    property var hist: mocks.historyModel
    function row(id, fraction, watched) {
        return { videoId: id, title: "Video " + id, channel: "Channel", channelId: "", thumbnail: "", fraction: fraction, watched: watched }
    }
    function loader() { return find(window, "objectName", "continueLoader") }
    function tiles() {
        var list = find(window, "objectName", "continueShelfList")
        return list ? findAll(list.contentItem, function (i) { return i.hasOwnProperty("progress") && i.hasOwnProperty("thumbnailUrl") && i.title !== "" }) : []
    }
    steps: [
        function () { window.selectSection("home"); return 600 },
        function () {
            verify(loader() !== null, "Home has the shelf's place")
            verify(!loader().active && loader().item === null, "nothing to continue: the shelf is not loaded")
            hist.append(row("h1", 0.4, false))
            hist.append(row("h2", 1.0, true))
            hist.append(row("h3", 0.01, false))
            hist.append(row("h4", 0.6, false))
            // The shelf is read when Home comes on screen.
            window.selectSection("subscriptions")
            return 300
        },
        function () { window.selectSection("home"); return 500 },
        function () {
            verify(loader().active && loader().item !== null, "the shelf loads with something to continue")
            compare(loader().item.rows.length, 2, "the started, unfinished videos")
            var list = tiles()
            compare(list.length, 2, "one tile each")
            var titles = list.map(function (i) { return i.title }).sort().join(",")
            compare(titles, "Video h1,Video h4", "watched and barely started videos are left out")
            var first = list.filter(function (i) { return i.title === "Video h1" })[0]
            verify(Math.abs(first.progress - 0.4) < 0.001, "the tile shows where the video was left")
            first.clicked(null)
            return 600
        },
        function () {
            verify(current().isWatchPage === true, "a tile opens the video")
            verify(mocks.playback.calls.indexOf("open:h1") >= 0, "the tile's video plays")
            stack().pop(undefined, PageStackAction.Immediate)
            mocks.playback.stop()
            return 400
        },
        function () {
            verify(loader().active, "the shelf is still there after coming back")
            find(window, "objectName", "continueShelfHeader").clicked(null)
            return 600
        },
        function () {
            verify(current() !== window.mainPage, "the shelf's header opens a page")
            // The history page: a list over the history model.
            var lists = findAll(current(), function (i) { return i.hasOwnProperty("model") && i.model === mocks.historyModel })
            verify(lists.length > 0, "the history page")
            stack().pop(undefined, PageStackAction.Immediate)
            return 300
        },
        function () {
            // Nothing left unfinished: the shelf leaves when Home comes back on screen.
            hist.clear()
            window.selectSection("subscriptions")
            return 300
        },
        function () { window.selectSection("home"); return 400 },
        function () {
            verify(!loader().active, "the shelf leaves with the last unfinished video")
            window.selectSection("subscriptions")
        }
    ]
}
