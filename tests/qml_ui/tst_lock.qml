// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
import QtQuick 2.6
import Sailfish.Silica 1.0

// Touch lock (docs/DESIGN.md §3.2.6): behind its release switch (PlayerUi.TOUCH_LOCK_RELEASED);
// once released, offered in the settings sheet in fullscreen only; while
// locked the controls stay away, touches on the picture only show Unlock, and Back is off; Unlock
// gives everything back; the end of the video and leaving fullscreen unlock by themselves.
UiTest {
    id: t
    property var pb: mocks.playback
    property var page
    function area() { return find(page, "objectName", "pictureArea") }
    function controls() { return find(page, "objectName", "playerOverlay") }
    function sheet() { return find(page, "objectName", "playerSettings") }
    function unlock() { return find(page, "objectName", "unlockButton") }
    steps: [
        function () { window.openVideo("l1", { title: "Lock" }); return 500 },
        function () {
            page = current()
            pb.finishLoading(true)
            return 200
        },
        function () {
            // The release switch decides first: off, the row never shows, in fullscreen either.
            page.touchLockReleased = false
            var a = area()
            a.startX = 100; a.startY = 300
            a.release(100, 100)         // swipe up: fullscreen
            return 300
        },
        function () {
            verify(page.immersive, "fullscreen")
            verify(!page.lockOffered, "not released: no lock in fullscreen")
            verify(sheet().rows.indexOf("lock") < 0, "not released: no Lock screen row")
            page.setTouchLocked(true)
            verify(!page.touchLocked, "not released: the lock cannot start")
            var a = area()
            a.startX = 100; a.startY = 100
            a.release(100, 400)         // swipe down: windowed again
            return 300
        },
        function () {
            verify(!page.immersive, "windowed again")
            page.touchLockReleased = true
            verify(!page.lockOffered, "windowed: no lock")
            verify(sheet().rows.indexOf("lock") < 0, "no Lock screen row outside fullscreen")
            page.setTouchLocked(true)
            verify(!page.touchLocked, "the lock cannot start outside fullscreen")
            var a = area()
            a.startX = 100; a.startY = 300
            a.release(100, 100)         // swipe up: fullscreen
            return 300
        },
        function () {
            verify(page.immersive && page.lockOffered, "fullscreen offers the lock")
            var rows = sheet().rows
            compare(rows[rows.length - 1], "lock", "Lock screen is the sheet's last row")
            sheet().lockRequested()
            return 100
        },
        function () {
            verify(page.touchLocked, "locked from the sheet")
            verify(!controls().shown, "no controls while locked")
            verify(unlock().visible, "Unlock shows when the lock starts")
            verify(!page.backNavigation, "Back is off while locked")
            // A tap, a swipe down and a second tap: nothing but Unlock.
            unlock().revealed = false
            var shownBefore = page.overlayController.controlsVisible
            var a = area()
            a.startX = 100; a.startY = 100
            a.release(100, 100)
            compare(page.overlayController.controlsVisible, shownBefore, "a tap does not reach the controls")
            verify(unlock().visible, "a touch shows Unlock again")
            a.startX = 100; a.startY = 100
            a.release(100, 400)
            return 300
        },
        function () {
            verify(page.immersive, "a swipe down does not leave fullscreen while locked")
            verify(page.touchLocked, "still locked")
            unlock().clicked(null)
            return 100
        },
        function () {
            verify(!page.touchLocked, "Unlock ends the lock")
            verify(page.backNavigation, "Back works again")
            verify(controls().shown, "the controls come back")
            verify(!unlock().visible, "Unlock leaves with the lock")
            // The end of the video unlocks: Replay and the suggestions must be reachable.
            page.setTouchLocked(true)
            verify(page.touchLocked, "locked again")
            pb.playing = false; pb.playerChanged(); pb.ended()
            return 200
        },
        function () {
            verify(!page.touchLocked, "the end of the video unlocks")
            pb.play()
            return 200
        },
        function () {
            // Leaving fullscreen (here: the video goes away) unlocks too.
            page.setTouchLocked(true)
            verify(page.touchLocked, "locked a third time")
            pb.hasVideo = false; pb.streamChanged()
            return 200
        },
        function () {
            verify(!page.immersive, "no picture, no fullscreen")
            verify(!page.touchLocked, "leaving fullscreen unlocks")
            verify(page.backNavigation, "Back works")
            stack().pop(undefined, PageStackAction.Immediate)
            pb.stop()
        }
    ]
}
