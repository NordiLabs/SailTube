// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
import QtQuick 2.6
import Sailfish.Silica 1.0

// The player's settings sheet as a modal panel (docs/DESIGN.md §3.2). Found on the device with the
// beta 3 candidate: the sheet had no surface of its own, and leaving the page with it open left
// the whole app dimmed. So: the sheet draws an opaque surface and sits above the info column;
// while it is open Back is off (a press outside closes the sheet instead); and the page never
// leaves with it open, by Back or by the next video replacing the page, so the window's dimmer is
// never left on.
UiTest {
    id: t
    property var pb: mocks.playback
    property var page
    function sheet() { return find(page, "objectName", "playerSettings") }
    steps: [
        function () { window.openVideo("s1", { title: "Sheet" }); return 500 },
        function () { page = current(); pb.finishLoading(true); return 200 },
        function () {
            var s = sheet()
            verify(!s.open && page.backNavigation, "closed: Back works")
            verify(!window._dimScreen, "nothing is dimmed")
            verify(s.z > 0, "the sheet is above the info column, which is declared after it")
            s.openSheet()
            return 700
        },
        function () {
            var s = sheet()
            verify(s.open, "open")
            verify(!page.backNavigation, "Back is off while the sheet is open")
            verify(window._dimScreen, "the modal sheet dims the page behind it")
            // An opaque surface in the ambience's overlay colour, as large as the sheet.
            var surface = find(s, "objectName", "sheetSurface")
            verify(surface !== null, "the sheet draws its own surface")
            compare(surface.color.a, 1, "opaque")
            verify(Qt.colorEqual(surface.color, Theme.overlayBackgroundColor), "the ambience's overlay colour")
            verify(surface.width >= s.width - 1 && surface.height >= s.height - 1,
                   "covers the whole sheet: " + surface.width + "x" + surface.height + " of " + s.width + "x" + s.height)
            // The press outside closes the sheet while the finger is still down: Back must stay
            // off until the gesture is over (the harness cannot press the page stack, so the
            // state logic is driven directly: pageStack.pressed is false here).
            s.hide()
            verify(!s.open, "the press closed the sheet")
            verify(!page.backNavigation, "closed by the press: Back is still off for the rest of the gesture")
            return 150
        },
        function () {
            verify(!page.backNavigation, "still off shortly after the close, before the gesture can end")
            return 600
        },
        function () {
            verify(!sheet().open && page.backNavigation, "closed again: Back works")
            verify(!window._dimScreen, "the dimmer is gone")
            // Leaving the page with the sheet open (here: inside a choice list) hides it first.
            sheet().openSheet()
            sheet().level = "speed"
            return 700
        },
        function () {
            verify(sheet().open && window._dimScreen, "open inside a choice list, dimmed")
            stack().pop(undefined, PageStackAction.Immediate)
            return 400
        },
        function () {
            verify(current().isWatchPage !== true, "the page left")
            verify(!window._dimScreen, "no dimmer is left behind when the page leaves with the sheet open")
            // The next video replacing the page (the queue moving on) with the sheet open.
            window.openVideo("s1", { title: "Sheet" })
            return 600
        },
        function () { page = current(); sheet().openSheet(); return 700 },
        function () {
            verify(sheet().open && window._dimScreen, "open on the new page")
            pb.replacePageRequested()
            return 600
        },
        function () {
            verify(current().isWatchPage === true && current() !== page, "the page was replaced")
            verify(!window._dimScreen, "no dimmer is left behind when the page is replaced")
            verify(!find(current(), "objectName", "playerSettings").open, "the new page's sheet is closed")
            stack().pop(undefined, PageStackAction.Immediate)
            pb.stop()
        }
    ]
}
