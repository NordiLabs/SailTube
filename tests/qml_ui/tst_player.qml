// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
import QtQuick 2.6
import Sailfish.Silica 1.0

// Watch-page player controls (docs/DESIGN.md §3.2): the centre disc, the settings sheet (quality,
// speed, audio only), Replay at the end, and minimise to the mini-player.
UiTest {
    id: t
    property var pb: mocks.playback
    property var page
    function overlayItem() { return find(page, "objectName", "playerOverlay") }
    function sheet() { return find(page, "objectName", "playerSettings") }
    function byName(root, name) {
        var l = findAll(root, function (i) { return i.visible && i.Accessible && i.Accessible.name === name && typeof i.clicked === "function" })
        return l.length ? l[0] : null
    }
    function textItems(root, text) {
        return findAll(root, function (i) { return i.visible && i.text === text })
    }
    steps: [
        function () {
            pb.qualities = [{ height: 720, highFrameRate: false, label: "720p", codec: "vp9" }]
            window.openVideo("p1", { title: "Player" })
            return 500
        },
        function () {
            page = current()
            pb.finishLoading(true)
            return 200
        },
        function () {
            var o = overlayItem()
            verify(o !== null && o.visible, "controls shown")
            compare(o.centre, "pause", "playing: the disc pauses")
            byName(o, "Pause").clicked(null)
            verify(!pb.playing, "the disc paused playback")
            compare(o.centre, "play")
            // Quality and audio only left the pulley for the sheet.
            compare(textItems(page, "Quality").filter(function (i) { return i.hasOwnProperty("__silica_menuitem") }).length, 0)
            byName(o, "Settings").clicked(null)
            return 400
        },
        function () {
            var s = sheet()
            verify(s.open, "the gear opens the settings sheet")
            compare(s.rows.join(","), "quality,speed,loop,sleep,audioOnly", "rows for what the session offers")
            s.level = "speed"
            return 200
        },
        function () {
            var s = sheet()
            var b = textItems(s, "1.5×")
            verify(b.length === 1, "speed choices listed")
            var row = b[0]
            while (row && typeof row.clicked !== "function") row = row.parent
            row.clicked(null)
            compare(pb.rate, 1.5, "speed set")
            return 400
        },
        function () {
            verify(!sheet().open, "choosing closes the sheet")
            pb.playing = false; pb.ended()
            return 100
        },
        function () {
            compare(overlayItem().centre, "replay", "the end offers Replay")
            byName(overlayItem(), "Replay").clicked(null)
            verify(pb.calls.indexOf("seek:0") >= 0 && pb.playing, "replay from the start: " + pb.calls.join(","))
            byName(overlayItem(), "Minimize player").clicked(null)
            return 500
        },
        function () {
            verify(current().isWatchPage !== true, "minimise leaves the watch page")
            pb.qualities = []
            pb.rate = 1.0
            pb.stop()
        }
    ]
}
