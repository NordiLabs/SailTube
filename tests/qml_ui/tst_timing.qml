// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
import QtQuick 2.6
import Sailfish.Silica 1.0

// NFR-PERF-01/07: the shell marks its start-up stages, and a tap on a video times details,
// playing and the first advancing frame once each.
UiTest {
    id: t
    property var timing: mocks.startupTiming
    property var pb: mocks.playback
    steps: [
        function () {
            verify(timing.lines.indexOf("startup shell") >= 0, "main page marks the shell: " + timing.lines.join(","))
            mocks.feedModel.append({ videoId: "f1", title: "Feed 1", channel: "C", thumbnail: "", duration: "1:00",
                                     viewCount: 5, live: false, publishedText: "" })
            return 200
        },
        function () {
            var view = find(window, "standalone", false)
            verify(view !== null, "feed view in the main page")
            verify(view.hasRows, "feed view shows the row: count " + mocks.feedModel.count + " main " + current().current)
            verify(timing.lines.indexOf("startup content") >= 0, "first landing rows mark content: " + timing.lines.join(",")
                   + " active=" + (view ? view.active : "?") + " status=" + current().status)
            window.openVideo("tap1", {})
            verify(timing.running("video"), "a tap starts the video interval")
            return 300
        },
        function () {
            pb.playing = true
            pb.playerChanged()
            pb.position = 40
            pb.positionChanged()
            pb.position = 80
            pb.positionChanged()
            return 100
        },
        function () {
            var v = timing.lines.filter(function (l) { return l.indexOf("video ") === 0 })
            verify(v.indexOf("video playing") >= 0, "playing logged: " + v.join(","))
            compare(v.filter(function (l) { return l === "video first-frame" }).length, 1, "first frame logged once")
            stack().pop(undefined, PageStackAction.Immediate)
            pb.playing = false
            mocks.feedModel.clear()
        }
    ]
}
