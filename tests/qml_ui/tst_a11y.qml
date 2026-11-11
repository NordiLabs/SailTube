// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
import QtQuick 2.6
import Sailfish.Silica 1.0

// Accessibility: text and buttons drawn on the black video box use light colours in every
// ambience (a light ambience would make Theme colours dark on black). Screen-reader names of
// icon-only buttons are checked by tests/qml_lint.
UiTest {
    id: t
    property var page
    property var pb: mocks.playback
    function buttons(root) {
        return findAll(root, function (i) { return i.hasOwnProperty("preferredWidth") && i.hasOwnProperty("highlighted") && i.hasOwnProperty("color") })
    }
    steps: [
        function () { window.openVideo("a11y", { title: "Contrast" }); return 600 },
        function () {
            page = current()
            var box = find(page, "objectName", "videoBox")
            verify(box !== null, "video box")
            pb.fail("Something went wrong")
            pb.sponsorButton = "Sponsor"
            pb.upNextVideoId = "next1"
            return 200
        },
        function () {
            var box = find(page, "objectName", "videoBox")
            var bs = buttons(box)
            verify(bs.length >= 4, "buttons on the video box: " + bs.length)
            for (var i = 0; i < bs.length; ++i)
                verify(Qt.colorEqual(bs[i].color, Theme.lightPrimaryColor), "light button text on black: " + bs[i].text + " " + bs[i].color)
            pb.sponsorButton = ""
            pb.upNextVideoId = ""
            pb.retry()
            stack().pop(undefined, PageStackAction.Immediate)
        }
    ]
}
