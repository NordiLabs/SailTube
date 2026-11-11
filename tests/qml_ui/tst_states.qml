// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
import QtQuick 2.6
import Sailfish.Silica 1.0

// docs/DESIGN.md §5.3: a failed list without rows shows its placeholder with Retry under it; a
// failed next page ends the list with a tap-to-retry row; nothing retries on scroll.
UiTest {
    id: t
    property var feed: mocks.feedModel
    function view() { return find(window, "standalone", false) }
    function actions() {
        var a = findAll(window, function (i) { return i.hasOwnProperty("canRetry") && i.hasOwnProperty("savedCount") })
        return a.length ? a[0] : null
    }
    function retryRow() {
        var r = findAll(window, function (i) { return i.hasOwnProperty("errorText") && i.hasOwnProperty("down") })
        return r.length ? r[0] : null
    }
    function button(root, text) {
        var b = findAll(root, function (i) { return i.text === text && typeof i.clicked === "function" })
        return b.length ? b[0] : null
    }
    steps: [
        function () {
            feed.channelCount = 2
            feed.errorText = "No network"
            feed.busy = false
            feed.calls = []
            return 150
        },
        function () {
            compare(view().listState, "placeholder", "failed with no rows")
            var a = actions()
            verify(a !== null && a.visible, "actions under the placeholder")
            var retry = button(a, "Retry")
            verify(retry !== null && retry.visible, "Retry button")
            retry.clicked(null)
            verify(feed.calls.indexOf("refresh:true") >= 0, "Retry forces a refresh: " + feed.calls.join(","))
            // Without downloads (none mocked) the shortcut stays hidden.
            var open = button(a, "Open downloads")
            verify(open !== null && !open.visible, "no downloads shortcut without saved videos")
            feed.append({ videoId: "r1", title: "Row", channel: "C", thumbnail: "", duration: "1:00",
                          viewCount: 1, live: false, publishedText: "" })
            feed.canLoadMore = true
            return 150
        },
        function () {
            compare(view().listState, "rows")
            verify(!actions().visible, "no placeholder actions with rows")
            var row = retryRow()
            verify(row !== null && row.visible, "inline retry row after a failed page")
            feed.calls = []
            row.clicked(null)
            compare(feed.calls.join(","), "loadMore", "the tap retries the page")
            feed.errorText = ""
            return 100
        },
        function () {
            verify(!retryRow().visible, "retry row gone once the error clears")
            feed.clear()
            feed.canLoadMore = false
            feed.channelCount = 0
        }
    ]
}
