// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
import QtQuick 2.6
import Sailfish.Silica 1.0

// Watch page chrome (PLY-13/14/16), the mini-player (PLY-11) and a page destroyed while its
// video is still loading (PLT-06).
UiTest {
    id: t
    property var page
    property var pb: mocks.playback
    steps: [
        // PLT-06: open, then leave before the answer arrives; the late answer touches nothing.
        function () { window.openVideo("a1", { title: "First" }); return 600 },
        function () {
            page = current()
            verify(page.isWatchPage === true, "watch page on top")
            verify(window.watchPageOnTop, "window knows the watch page is on top")
            stack().pop(undefined, PageStackAction.Immediate)
            return 300
        },
        function () {
            verify(current().isWatchPage !== true, "back on the main page")
            pb.finishLoading(true)          // answer lands after the page is gone
            pb.fail("late failure")         // and a late error too
            return 300
        },
        function () {
            // The video is still loaded, so the mini-player shows on content pages.
            var bar = find(window, "queueLength", 0)
            verify(bar !== null, "mini-player exists")
            verify(bar.shown, "mini-player shown after leaving the watch page")
            pb.state = 3; pb.errorText = ""; pb.play()
            window.openVideo("a1")           // same video: re-attach, no reload
            return 600
        },
        function () {
            page = current()
            verify(page.isWatchPage === true, "watch page reopened from the bar")
            compare(pb.calls.filter(function (c) { return c === "open:a1" }).length, 1, "re-attach did not call open again")
            var bar = find(window, "queueLength", 0)
            verify(!bar.shown, "mini-player hidden on the watch page")
            // PLY-16: windowed portrait, playing, controls visible -> glow shown.
            verify(page.showNavigationIndicator, "glow with controls visible")
            var controls = find(page, "fullscreen", false)
            verify(controls !== null, "overlay controls")
            page.overlayController.controlsVisible = false
            return 100
        },
        function () {
            verify(!page.showNavigationIndicator, "glow hidden while playing with controls hidden")
            pb.pause()
            return 50
        },
        function () {
            verify(page.showNavigationIndicator, "glow shown when paused")
            // PLY-14: 16:9 box at page width for a landscape picture, taller for vertical.
            page.videoWidth = 1920; page.videoHeight = 1080
            var box = find(page, "objectName", "")   // any item; geometry checked below
            compare(page.immersive, false)
            return 200
        },
        function () {
            var boxes = findAll(page, function (i) { return i.hasOwnProperty("x") && i.children && i.children.length > 4 && i.width === page.width && Math.abs(i.height - Math.round(page.width * 9 / 16)) < 1 })
            verify(boxes.length >= 1, "16:9 video box at page width")
            page.videoWidth = 1080; page.videoHeight = 1920
            return 400
        },
        function () {
            var expected = Math.min(Math.round(page.width * 1920 / 1080), Math.round(page.height * 0.6))
            var boxes = findAll(page, function (i) { return i.children && i.children.length > 4 && i.width === page.width && Math.abs(i.height - expected) < 1 })
            verify(boxes.length >= 1, "vertical box up to 60 % of the height (want " + expected + ")")
            // PLY-13: a vertical video goes fullscreen upright.
            page.toggleFullscreen()
            compare(page.fullscreenMode, "portrait"); compare(page.allowedOrientations, Orientation.Portrait)
            verify(page.immersive, "portrait fullscreen is immersive")
            verify(!page.showNavigationIndicator, "no glow in fullscreen")
            page.toggleFullscreen()
            compare(page.fullscreenMode, "windowed"); compare(page.allowedOrientations, page.basePolicy, "policy restored")
            // Landscape video: forced landscape mask, then back with the policy restored.
            page.videoWidth = 1920; page.videoHeight = 1080
            page.toggleFullscreen()
            compare(page.fullscreenMode, "landscape"); compare(page.allowedOrientations, Orientation.LandscapeMask)
            return 600
        },
        function () {
            page.toggleFullscreen()
            compare(page.fullscreenMode, "windowed")
            verify(page.allowedOrientations === Orientation.Portrait || page.allowedOrientations === page.basePolicy,
                   "leaving landscape pins portrait at most until upright")
            return 600
        },
        function () {
            page.tryRestore()
            if (!(window.deviceOrientation & Orientation.LandscapeMask))
                compare(page.allowedOrientations, page.basePolicy, "auto-rotate policy is back")
            // Audio only leaves fullscreen and shows the thumbnail.
            page.toggleFullscreen()
            pb.setAudioOnlyMode(true)
            return 300
        },
        function () {
            compare(page.fullscreenMode, "windowed", "audio only leaves fullscreen")
            pb.fail("No playable stream")
            return 200
        },
        function () {
            verify(page.failed, "failed state")
            var labels = findAll(page, function (i) { return i.text === "No playable stream" && i.visible })
            compare(labels.length, 1, "error message shown once")
            // Close from the mini-player stops playback.
            stack().pop(undefined, PageStackAction.Immediate)
            return 300
        },
        function () {
            var bar = find(window, "queueLength", 0)
            bar.closeRequested()
            return 300
        },
        function () {
            verify(pb.calls.indexOf("stop") >= 0, "close stops playback")
            verify(!find(window, "queueLength", 0).shown, "bar gone after close")
        }
    ]
}
