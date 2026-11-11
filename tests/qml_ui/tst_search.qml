// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
import QtQuick 2.6
import Sailfish.Silica 1.0

// Search (docs/DESIGN.md §4.6): filter chips under the field. The quick chips set the result type
// and Live at once and show the current choice; the Filters chip is marked while filters are active.
// A hashtag tapped in a video's description runs its search and lands in the Search section, with
// the query in the field.
UiTest {
    id: t
    property var sm: mocks.searchModel
    function chips() { return find(window, "objectName", "filterChips") }
    function chip(text) {
        var l = findAll(chips(), function (i) { return i.hasOwnProperty("selected") && i.hasOwnProperty("passive") && i.text === text })
        return l.length ? l[0] : null
    }
    steps: [
        function () { window.selectSection("search"); return 600 },
        function () {
            verify(chips() !== null && chips().visible, "chip row under the field")
            verify(chip("All").selected, "All is the current choice without filters")
            chip("Videos").clicked(null)
            compare(sm.type, 1, "Videos sets the type")
            verify(chip("Videos").selected && !chip("All").selected, "the chosen chip is marked")
            chip("Live").clicked(null)
            verify(sm.live && chip("Live").selected, "Live toggles on")
            chip("All").clicked(null)
            compare(sm.type, 0); verify(!sm.live, "All clears type and Live")
            // The Filters chip leads to the filter page (tst_pages opens it) and is marked while
            // filters are active.
            verify(chip("Filters") !== null && !chip("Filters").selected, "Filters chip, not marked without filters")
            sm.filtersActive = true
            return 100
        },
        function () {
            var marked = findAll(chips(), function (i) { return i.hasOwnProperty("selected") && i.selected && String(i.text).indexOf("Filters") === 0 })
            compare(marked.length, 1, "Filters is marked while filters are active")
            sm.filtersActive = false
            window.selectSection("subscriptions")
            return 300
        },
        function () { window.openVideo("h1", { title: "Hashtags" }); return 600 },
        function () {
            verify(current().isWatchPage === true, "watch page")
            mocks.watchInfo.hashtagRequested("sailing")
            return 700
        },
        function () {
            verify(current() === window.mainPage, "the hashtag leaves the watch page for the main page")
            compare(window.mainPage.current, "search", "the Search section shows")
            verify(sm.calls.indexOf("search:#sailing") >= 0, "the hashtag's search runs: " + sm.calls.join(" "))
            var fields = findAll(window.mainPage, function (i) { return i.hasOwnProperty("placeholderText") && i.hasOwnProperty("text") && i.visible })
            verify(fields.length > 0, "search field")
            compare(fields[0].text, "#sailing", "the field shows the query")
            mocks.playback.stop()
            sm.cancel(); sm.query = ""
            window.selectSection("subscriptions")
        }
    ]
}
