// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
import QtQuick 2.6
import Sailfish.Silica 1.0

// End of a video (docs/DESIGN.md §3.2.4): suggestions under the Replay disc once it ends; the
// "Up next" card takes the centre during the autoplay countdown and Cancel stops it; the error
// state offers the browser.
UiTest {
    id: t
    property var pb: mocks.playback
    property var rel: mocks.watchInfo.related
    property var page
    function item(name) { return find(page, "objectName", name) }
    function textItems(root, text) { return findAll(root, function (i) { return i.visible && i.text === text }) }
    steps: [
        function () {
            rel.rows = [{ videoId: "r1", title: "First", thumbnail: "" }, { videoId: "r2", title: "Second", thumbnail: "" },
                        { videoId: "r3", title: "Third", thumbnail: "" }, { videoId: "r4", title: "Fourth", thumbnail: "" }]
            window.openVideo("e1", { title: "Ending" })
            return 500
        },
        function () { page = current(); pb.finishLoading(true); return 200 },
        function () {
            pb.playing = false; pb.playerChanged(); pb.ended()
            return 200
        },
        function () {
            verify(item("endSuggestions").visible, "suggestions after the end")
            compare(item("playerEndScreen").suggestionCount, 3, "three suggestions")
            compare(item("playerOverlay").centre, "replay", "Replay in the centre")
            pb.upNextVideoId = "r1"; pb.upNextSecondsLeft = 5
            return 100
        },
        function () {
            verify(item("upNextCard").visible, "the up-next card during the countdown")
            verify(!item("endSuggestions").visible, "no suggestions while counting down")
            verify(item("playerOverlay").centreHidden, "the card holds the centre")
            var cancel = textItems(item("upNextCard"), "Cancel").filter(function (i) { return typeof i.clicked === "function" })
            cancel[0].clicked(null)
            compare(pb.upNextVideoId, "", "Cancel stops autoplay")
            pb.fail("Video unavailable")
            return 200
        },
        function () {
            var b = textItems(page, "Open in browser").filter(function (i) { return typeof i.clicked === "function" })
            verify(b.length >= 1, "the error offers the browser")
            rel.rows = []
            stack().pop(undefined, PageStackAction.Immediate)
            pb.stop()
        }
    ]
}
