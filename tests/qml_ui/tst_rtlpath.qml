// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// harness: qm=ar
import QtQuick 2.6
import Sailfish.Silica 1.0

// Right-to-left along the first minute (docs/DESIGN.md §7.8): Home, Subscriptions, Search,
// Library, the watch page with its settings sheet, a playlist, and the pages beside them. No
// visible text collapses or leaves the screen; the watch page's owner row mirrors (avatar on the
// right, Subscribe on the left); a chip strip starts at its right end; the player's transport and
// time keep their left-to-right order, and so does the mini-player's progress line.
UiTest {
    id: t
    property var problems: []
    property var page
    property var extraPages: ["PlaylistsPage.qml", "HistoryPage.qml", "SubscriptionsPage.qml", "SettingsPage.qml"]
    // Inside something that scrolls sideways, an item may sit off screen.
    function inStrip(item) {
        var p = item.parent
        while (p) {
            if (p.hasOwnProperty("contentX") && p.hasOwnProperty("contentWidth") && p.contentWidth > p.width + 1) return true
            if (p.hasOwnProperty("orientation") && p.orientation === ListView.Horizontal) return true
            p = p.parent
        }
        return false
    }
    function shown(item) {
        var p = item
        while (p) { if (p.visible === false || p.opacity === 0) return false; p = p.parent }
        return true
    }
    function scan(label) {
        var w = window.width
        var items = findAll(current(), function (i) { return typeof i.text === "string" && i.text !== "" && i.hasOwnProperty("font") })
        var out = []
        for (var k = 0; k < items.length; ++k) {
            var i = items[k]
            if (!shown(i)) continue
            var x = i.mapToItem(null, 0, 0).x
            if (i.width <= 1) out.push(label + ": collapsed text, width " + i.width)
            else if (!inStrip(i) && (x < -1 || x + i.width > w + 1)) out.push(label + ": text off screen, x " + x + " width " + i.width)
        }
        problems = problems.concat(out)
    }
    function open(file, properties) {
        stack().push(Qt.resolvedUrl("../../Clients/Sailfish/qml/pages/" + file), properties || {}, PageStackAction.Immediate)
    }
    function row(id) {
        return { videoId: id, title: "Video " + id, channel: "Channel", channelId: "", durationSeconds: 60, thumbnail: "" }
    }
    steps: [
        function () {
            verify(window.rightToLeft && window.LayoutMirroring.enabled, "right-to-left")
            mocks.historyModel.append({ videoId: "h1", title: "Watched video", channel: "C", channelId: "", thumbnail: "", fraction: 0.4, watched: false })
            mocks.playlistsModel.append({ playlistId: "p1", title: "My list", itemCount: 3, thumbnail: "", watchLater: false })
            window.selectSection("home")
            return 600
        },
        function () { scan("home"); window.selectSection("subscriptions"); return 500 },
        function () { scan("subscriptions"); window.selectSection("search"); return 500 },
        function () { mocks.searchModel.search("x"); mocks.searchModel.deliver(3); return 400 },
        function () {
            scan("search")
            var chips = find(window, "objectName", "filterChips")
            verify(chips.mirrored, "the filter chips know the direction")
            compare(chips.contentX, Math.max(0, chips.contentWidth - chips.width), "the filter chips start at their right end")
            window.selectSection("library")
            return 500
        },
        function () { scan("library"); window.openVideo("r1", { title: "RTL video" }); return 600 },
        function () {
            page = current()
            mocks.playback.finishLoading(true)
            mocks.playback.channelId = "UC1234567890123456789012"
            mocks.watchInfo.subscriberText = "1.2M"
            mocks.playback.detailsChanged()
            return 400
        },
        function () {
            scan("watch page")
            // Owner row: avatar at the right, Subscribe at the left, the name between them.
            var owner = find(page, "objectName", "ownerRow")
            var name = find(owner, "objectName", "ownerName"), button = find(owner, "objectName", "watchSubscribe")
            var nameX = name.mapToItem(owner, 0, 0).x, buttonX = button.mapToItem(owner, 0, 0).x
            verify(name.width > 1, "the channel's name has room: " + name.width)
            verify(buttonX < owner.width / 2, "Subscribe sits at the left end: " + buttonX)
            verify(buttonX + button.width <= nameX + 1, "the name starts after Subscribe: " + (buttonX + button.width) + " vs " + nameX)
            // Chips: the strip starts at its right end.
            var strip = find(page, "objectName", "actionChips")
            verify(strip.mirrored, "the action chips know the direction")
            compare(strip.contentX, Math.max(0, strip.contentWidth - strip.width), "the action chips start at their right end")
            // Player: transport and time stay left-to-right; the time is left of the seek bar.
            var overlay = find(page, "objectName", "playerOverlay")
            verify(!find(overlay, "objectName", "playerTransport").LayoutMirroring.enabled, "previous, play and next keep their order")
            var bottom = find(overlay, "objectName", "playerBottomRow")
            verify(!bottom.LayoutMirroring.enabled, "the bottom row is not mirrored")
            var timeX = find(bottom, "objectName", "playerTime").mapToItem(bottom, 0, 0).x
            var barX = overlay.slider.mapToItem(bottom, 0, 0).x
            verify(timeX < barX, "the time is left of the seek bar: " + timeX + " vs " + barX)
            find(page, "objectName", "playerSettings").openSheet()
            return 500
        },
        function () {
            scan("settings sheet")
            find(page, "objectName", "playerSettings").hide()
            return 400
        },
        function () {
            mocks.playback.position = 30000
            stack().pop(undefined, PageStackAction.Immediate)
            return 500
        },
        function () {
            // Back on the main page: the mini-player's progress line fills from the left.
            var line = find(window, "objectName", "miniProgress")
            verify(line !== null && shown(line), "mini-player with its progress line")
            verify(line.width > 1 && line.width < window.width, "a quarter played: " + line.width)
            verify(Math.abs(line.mapToItem(null, 0, 0).x) < 1, "the line starts at the left edge: " + line.mapToItem(null, 0, 0).x)
            scan("mini-player")
            mocks.playback.stop()
            mocks.playlistsModel.listRows = { "p1": [row("a"), row("b")] }
            open("PlaylistPage.qml", { playlistId: "p1" })
            return 500
        },
        function () { scan("playlist"); stack().pop(undefined, PageStackAction.Immediate); open(extraPages[0]); return 500 },
        function () { scan(extraPages[0]); stack().pop(undefined, PageStackAction.Immediate); open(extraPages[1]); return 500 },
        function () { scan(extraPages[1]); stack().pop(undefined, PageStackAction.Immediate); open(extraPages[2]); return 500 },
        function () { scan(extraPages[2]); stack().pop(undefined, PageStackAction.Immediate); open(extraPages[3]); return 600 },
        function () { scan(extraPages[3]); stack().pop(undefined, PageStackAction.Immediate); return 300 },
        function () {
            mocks.historyModel.clear(); mocks.playlistsModel.clear()
            mocks.playlistsModel.listRows = ({})
            mocks.searchModel.cancel(); mocks.searchModel.clear(); mocks.searchModel.query = ""
            window.selectSection("subscriptions")
            compare(problems.length, 0, "text that collapsed or left the screen: " + problems.join("; "))
        }
    ]
}
