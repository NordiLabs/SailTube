// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
import QtQuick 2.6
import Sailfish.Silica 1.0

// NFR-PERF-01, start-up experience (docs/DESIGN.md §8): the first frame builds only the shell
// and the landing list; MPRIS and the other helpers load after the first frame or with the first
// video; a list without rows shows the loading skeleton or a placeholder, never nothing.
UiTest {
    id: t
    property var feed: mocks.feedModel
    property var timing: mocks.startupTiming
    function loaderOf(name) {
        var l = findAll(window, function (i) { return i.hasOwnProperty("sourceComponent") && String(i.source).indexOf(name) >= 0 })
        return l.length ? l[0] : null
    }
    function view() { return find(window, "standalone", false) }
    function skeleton() {
        var s = findAll(window, function (i) { return i.hasOwnProperty("viewHeight") && i.hasOwnProperty("thumbW") })
        return s.length ? s[0] : null
    }
    steps: [
        function () {
            verify(!window.deferredReady, "helpers wait for the first frame")
            var mpris = findAll(window, function (i) { return String(i.source).indexOf("MediaControls.qml") >= 0 })
            verify(mpris.length === 1 && !mpris[0].item, "MPRIS is not created at start")
            // POCO run 5: nothing off the first screen is compiled or created at start.
            var byType = function (name) { return findAll(window, function (i) { return String(i).indexOf(name + "_QMLTYPE_") === 0 }) }
            compare(byType("MiniPlayer").length, 0, "the mini-player waits for the first video")
            compare(byType("PlaceholderButtons").length, 0, "placeholder buttons wait for a failure")
            compare(byType("Avatar").length, 0, "no avatars without channel rows")
            var v = view()
            verify(v !== null, "landing feed view")
            compare(v.listState, "placeholder", "no subscriptions: the placeholder, not a blank page")
            feed.channelCount = 3
            feed.busy = true
            return 150
        },
        function () {
            var v = view()
            compare(v.listState, "skeleton", "first load without a cache")
            var s = skeleton()
            verify(s !== null && s.visible, "skeleton visible while loading")
            verify(s.height >= s.rowHeight, "skeleton rows laid out: " + s.height)
            var placeholders = findAll(window, function (i) { return i.hasOwnProperty("hintText") && i.enabled })
            compare(placeholders.length, 0, "no empty-state text while loading")
            verify(timing.lines.indexOf("startup skeleton") >= 0, "skeleton marked: " + timing.lines.join(","))
            feed.append({ videoId: "s1", title: "Row", channel: "C", thumbnail: "", duration: "1:00",
                          viewCount: 1, live: false, publishedText: "" })
            feed.busy = false
            return 150
        },
        function () {
            compare(view().listState, "rows")
            verify(!skeleton().visible, "rows replace the skeleton")
            timing.mark("first-frame")
            return 100
        },
        function () {
            verify(window.deferredReady, "first frame releases the deferred helpers")
            verify(!loaderOf("MediaControls.qml").item, "MPRIS still waits for a video")
            window.openVideo("s1", { title: "Row" })
            return 400
        },
        function () {
            verify(loaderOf("MediaControls.qml").item !== null, "MPRIS registered with the first video")
            verify(loaderOf("MiniPlayer.qml").item !== null, "the mini-player exists once a video opened")
            stack().pop(undefined, PageStackAction.Immediate)
            feed.clear()
            feed.channelCount = 0
        }
    ]
}
