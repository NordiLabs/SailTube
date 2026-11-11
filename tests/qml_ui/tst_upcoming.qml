// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
import QtQuick 2.6
import Sailfish.Silica 1.0

// The picture box in its states without a picture (docs/DESIGN.md §3.2.4): the thumbnail is
// dimmed under the spinner and more under the error box, so their text reads on a bright picture;
// a live event that has not started is said in plain light text with Retry and Open in browser,
// not in the error colour; a real failure keeps the error colour.
UiTest {
    id: t
    property var pb: mocks.playback
    property var page
    function thumbnail() { return find(page, "objectName", "pictureThumbnail") }
    function message() { return find(page, "objectName", "errorMessage") }
    // The buttons of the error box (the column that holds the message), not the pulley's entries.
    function buttons(text) {
        return findAll(message().parent, function (i) { return i.visible && i.text === text && typeof i.clicked === "function" })
    }
    function shown(item) {
        for (var p = item; p; p = p.parent) if (p.visible === false) return false
        return true
    }
    steps: [
        function () { window.openVideo("u1", { title: "Upcoming", thumbnail: "image://theme/icon-m-play" }); return 500 },
        function () {
            page = current()
            verify(page.isWatchPage === true, "watch page")
            // Loading: the spinner and the status line sit on a dimmed thumbnail.
            compare(pb.state, 1, "loading")
            verify(thumbnail().visible, "the thumbnail stands in for the picture")
            compare(thumbnail().opacity, 0.6, "dimmed under the spinner")
            verify(!shown(message()), "no error box while loading")
            // A live event that has not started yet: the session fails with its own text.
            pb.upcoming = true
            pb.fail("This live event has not started yet. Scheduled to start 12/11/26 20:00.")
            return 200
        },
        function () {
            verify(page.failed, "the upcoming answer is the failed state")
            compare(thumbnail().opacity, 0.3, "the thumbnail is dimmed further under the box")
            var m = message()
            verify(shown(m), "the message shows")
            compare(m.text, "This live event has not started yet. Scheduled to start 12/11/26 20:00.", "the session's words, with the start")
            verify(Qt.colorEqual(m.color, Theme.lightPrimaryColor), "plain light text: news, not a failure")
            compare(buttons("Retry").length, 1, "Retry")
            compare(buttons("Open in browser").length, 1, "Open in browser")
            compare(buttons("Open setting").length, 0, "no setting to open")
            verify(!find(page, "objectName", "playerOverlay").shown, "no player controls over the box")
            // A real failure keeps the error colour (light on a light ambience, where it is dark).
            pb.upcoming = false
            pb.fail("The network is not reachable.")
            return 200
        },
        function () {
            var m = message()
            compare(m.text, "The network is not reachable.")
            var expected = Theme.colorScheme === Theme.LightOnDark ? Theme.errorColor : Theme.lightPrimaryColor
            verify(Qt.colorEqual(m.color, expected), "a failure is drawn in the error colour")
            compare(thumbnail().opacity, 0.3, "still dimmed under the box")
            stack().pop(undefined, PageStackAction.Immediate)
            pb.stop()
        }
    ]
}
