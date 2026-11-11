// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
import QtQuick 2.6
import Sailfish.Silica 1.0

// Gestures on the picture and the seek bar (docs/DESIGN.md §3.2.1, §3.2.2): a swipe up enters
// fullscreen and down leaves it; a long press plays at 2x while held; the seek bar seeks on
// release and keeps the controls up while held.
UiTest {
    id: t
    property var pb: mocks.playback
    property var page
    function area() { return find(page, "objectName", "pictureArea") }
    steps: [
        function () { window.openVideo("g1", { title: "Gestures" }); return 500 },
        function () {
            page = current()
            pb.finishLoading(true)
            pb.duration = 120000
            return 200
        },
        function () {
            var a = area()
            verify(a !== null, "picture gesture area")
            a.startX = 100; a.startY = 300
            a.release(100, 100)         // swipe up
            return 300
        },
        function () {
            verify(page.immersive, "swipe up enters fullscreen")
            var a = area()
            a.startX = 100; a.startY = 100
            a.release(100, 400)         // swipe down
            return 300
        },
        function () {
            verify(!page.immersive, "swipe down leaves fullscreen")
            var a = area()
            pb.calls = []
            a.startHold()
            compare(pb.rate, 2.0, "long press plays at 2x")
            verify(a.holding, "holding")
            a.release(0, 0)
            compare(pb.rate, 1.0, "release restores the speed")
            compare(pb.calls.join(","), "hold:true,hold:false", "the session holds the speed, the page sets no rate")
            // The seek bar: press, drag, release seeks once.
            var bar = find(page, "objectName", "playerOverlay").slider
            bar.dragValue = 60000
            bar.seekRequested(60000)
            verify(pb.calls.indexOf("seek:60000") >= 0, "seek on release: " + pb.calls.join(","))
            stack().pop(undefined, PageStackAction.Immediate)
            pb.stop()
        }
    ]
}
