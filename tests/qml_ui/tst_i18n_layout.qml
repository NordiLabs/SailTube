// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// harness: qm=all screen=540x960
import QtQuick 2.6
import Sailfish.Silica 1.0

// I18N-03: every catalog against the real layout on the narrowest phone (540x960 at pixel ratio
// 1). Runs once per language. Translated texts on the chips, the cards, the Library shelves, the
// player's settings sheet, the end screen and the heads of a playlist and a channel are never cut
// or painted outside their box or the screen; in Arabic the sideways rows start at their right
// end and rows are mirrored.
// Mock content is short, so a cut text is a translation that does not fit.
UiTest {
    id: t
    property var pb: mocks.playback
    property var page
    // Texts of the mock content that are cut on purpose (a description shown as two lines).
    property var content: []
    readonly property bool rtl: Qt.application.layoutDirection === Qt.RightToLeft

    function shown(item) {
        for (var p = item; p; p = p.parent) if (p.visible === false || p.opacity === 0) return false
        return true
    }
    function sideScroller(item) {
        for (var p = item.parent; p; p = p.parent)
            if (p.hasOwnProperty("contentWidth") && p.hasOwnProperty("flickableDirection") && p.contentWidth > p.width + 1) return p
        return null
    }
    function texts(root) {
        return findAll(root, function (i) {
            return i.hasOwnProperty("contentWidth") && i.hasOwnProperty("truncated") && typeof i.text === "string" && i.text !== "" && t.shown(i)
        })
    }
    // The width a text needs on a device (harness.textWidth: real font metrics; the headless
    // platform lays text out with a box glyph, so contentWidth is not usable).
    function needed(label) {
        // The widest line of the text.
        var lines = label.text.split("\n"), widest = 0
        // A text that shrinks to fit is judged at its smallest size.
        var size = label.fontSizeMode !== Text.FixedSize && label.minimumPixelSize > 0 ? label.minimumPixelSize : label.font.pixelSize
        for (var i = 0; i < lines.length; ++i) {
            var w = harness.textWidth(lines[i], size)
            if (w < 0) return -1
            widest = Math.max(widest, w)
        }
        return widest
    }
    // Texts below `root` that are cut or leave the screen. Only boxes whose width comes from the
    // layout are judged (a label sized by its own text always fits itself).
    function problems(root, where) {
        var out = []
        var list = texts(root)
        for (var i = 0; i < list.length; ++i) {
            var l = list[i]
            var own = Math.abs(l.width - l.implicitWidth) < 0.5
            if (own || l.width <= 0 || content.indexOf(l.text) >= 0) continue
            var need = needed(l)
            if (need < 0) { out.push(where + ": \"" + l.text + "\" cannot be measured"); continue }
            var lines = l.wrapMode === Text.NoWrap ? 1 : (l.maximumLineCount > 0 && l.maximumLineCount < 100 ? l.maximumLineCount : 0)
            if (lines > 0 && need > l.width * lines + 1)
                out.push(where + ": \"" + l.text + "\" is cut (needs " + Math.round(need) + " of " + Math.round(l.width * lines) + ")")
            if (!sideScroller(l)) {
                var x = l.mapToItem(null, 0, 0).x
                if (x < -1 || x + l.width > Screen.width + 1)
                    out.push(where + ": \"" + l.text + "\" leaves the screen (" + Math.round(x) + ".." + Math.round(x + l.width) + ")")
            }
        }
        return out
    }
    function check(root, where) {
        verify(root !== null && root !== undefined, where + " exists")
        harness.polish(window.contentItem)
        var p = problems(root, where)
        verify(p.length === 0, p.join("; "))
    }
    function chipsOf(row) { return findAll(row, function (i) { return i.hasOwnProperty("selected") && i.hasOwnProperty("passive") && i.visible }) }
    // A sideways chip row shows its first chip at rest: at the left margin, or at the right one
    // in a right-to-left language.
    function checkRowStart(row, where) {
        harness.polish(window.contentItem)
        var chips = chipsOf(row)
        verify(chips.length >= 2, where + ": chips")
        var first = chips[0]
        var x = first.mapToItem(row, 0, 0).x
        if (rtl) {
            verify(x + first.width <= row.width + 1 && x + first.width >= row.width - Theme.horizontalPageMargin - 1,
                   where + ": the first chip (\"" + first.text + "\") starts at the right edge in a right-to-left language: its right edge is at "
                   + Math.round(x + first.width) + " of " + row.width)
            verify(chips[1].mapToItem(row, 0, 0).x < x, where + ": the second chip is left of the first")
        } else {
            verify(Math.abs(x - Theme.horizontalPageMargin) <= 1, where + ": the first chip starts at the left margin: " + Math.round(x))
            verify(chips[1].mapToItem(row, 0, 0).x > x, where + ": the second chip is right of the first")
        }
    }
    function textWidth(text, pixelSize) { return harness.textWidth(text, pixelSize) }
    // No chip of a row is wider than the screen between the page margins (a chip sizes itself by
    // its text, and the row scrolls, but a chip wider than the screen can never be read whole).
    function checkChipsFit(row, where) {
        var chips = chipsOf(row), room = Screen.width - 2 * Theme.horizontalPageMargin, bad = []
        for (var i = 0; i < chips.length; ++i) {
            var w = textWidth(chips[i].text, Theme.fontSizeExtraSmall) + Theme.iconSizeSmall + Theme.paddingSmall + 2 * Theme.paddingLarge
            if (w > room + 1) bad.push("\"" + chips[i].text + "\" needs " + Math.round(w) + " of " + Math.round(room))
        }
        verify(bad.length === 0, where + ": chips fit the screen: " + bad.join("; "))
    }
    // A Silica button grows with its text: the text and the button's padding fit between the margins.
    function checkButtonFits(button, where) {
        verify(button !== null && button.visible, where + " shows")
        var w = textWidth(button.text, Theme.fontSizeMedium) + 2 * Theme.paddingMedium
        var room = Screen.width - 2 * Theme.horizontalPageMargin
        verify(w <= room + 1, where + ": \"" + button.text + "\" needs " + Math.round(w) + " of " + Math.round(room))
    }
    // The channel page's tabs: no title is cut (a tab is as wide as its title needs, and the row
    // scrolls), no tab is wider than the screen, and the row rests on the current tab.
    function tabsOf(strip) { return findAll(strip, function (i) { return i.hasOwnProperty("current") && i.hasOwnProperty("down") && i.visible }) }
    function checkTabs(strip, count, where) {
        harness.polish(window.contentItem)
        var tabs = tabsOf(strip), room = Screen.width - 2 * Theme.horizontalPageMargin, bad = []
        compare(tabs.length, count, where + ": tabs")
        for (var i = 0; i < tabs.length; ++i) {
            var label = texts(tabs[i])[0]
            verify(label !== undefined, where + ": tab " + i + " has a title")
            if (Math.abs(label.width - label.implicitWidth) >= 0.5 || label.implicitWidth > tabs[i].width)
                bad.push("\"" + label.text + "\" does not get the width it asks for")
            var w = textWidth(label.text, label.font.pixelSize) + 2 * Theme.paddingMedium
            if (w > room + 1) bad.push("\"" + label.text + "\" needs " + Math.round(w) + " of " + Math.round(room))
        }
        verify(bad.length === 0, where + ": " + bad.join("; "))
    }
    // The tab is inside the strip's window, the page margin kept beside it.
    function tabInView(strip, tab) {
        var x = tab.mapToItem(strip, 0, 0).x
        return x >= Theme.horizontalPageMargin - 1 && x + tab.width <= strip.width - Theme.horizontalPageMargin + 1
    }
    // The list of a page (the playlist and channel pages put their head into its header).
    function listOf(root) {
        var lists = findAll(root, function (i) { return i.hasOwnProperty("headerItem") && i.hasOwnProperty("model") && i.hasOwnProperty("delegate") })
        return lists.length > 0 ? lists[0] : null
    }
    // A step that gives a loading page more time on a busy machine: short when `ready`.
    function settle(ready) { return ready ? 50 : 3000 }

    steps: [
        // ---- Search: filter chips, and result cards ----
        function () {
            verify(harness.textWidth("Aa", 32) > 0, "real font metrics are available (FreeType and fontconfig in the build)")
            window.selectSection("search")
            return 800
        },
        function () { return settle(find(window, "objectName", "filterChips") !== null) },
        function () {
            var row = find(window, "objectName", "filterChips")
            verify(row !== null, "search filter chips")
            check(row, "search chips")
            checkRowStart(row, "search chips")
            mocks.searchModel.filtersActive = true
            return 100
        },
        function () {
            // "Filters: …" grows the row; the start still holds.
            checkRowStart(find(window, "objectName", "filterChips"), "search chips with active filters")
            mocks.searchModel.filtersActive = false
            // ---- Landing feed: a card with its meta line ----
            window.selectSection("subscriptions")
            mocks.feedModel.channelCount = 1
            mocks.feedModel.append({ videoId: "f1", title: "Card", channel: "Chan", thumbnail: "", duration: "1:00",
                                     viewCount: 1234567, live: false, publishedText: "" })
            return 600
        },
        function () {
            harness.polish(window.contentItem)
            return settle(findAll(window, function (i) { return i.hasOwnProperty("cardLayout") && i.videoTitle === "Card" }).length > 0)
        },
        function () {
            harness.polish(window.contentItem)
            var all = findAll(window, function (i) { return i.hasOwnProperty("cardLayout") })
            var cards = all.filter(function (i) { return t.shown(i) && i.videoTitle === "Card" })
            verify(cards.length >= 1, "feed card: " + all.length + " rows, feed count " + mocks.feedModel.count + ", section " + window.pageStack.currentPage.current)
            check(cards[0], "feed card")
            mocks.feedModel.clear(); mocks.feedModel.channelCount = 0
            // ---- Library: shelves ----
            window.selectSection("library")
            mocks.historyModel.append({ videoId: "h1", title: "Watched", channel: "C", channelId: "", thumbnail: "", fraction: 0.4, watched: false })
            mocks.playlistsModel.append({ playlistId: "p1", title: "My list", itemCount: 3, thumbnail: "", watchLater: false })
            return 800
        },
        function () { return settle(find(window, "objectName", "historyShelfHeader") !== null) },
        function () {
            var header = find(window, "objectName", "historyShelfHeader")
            check(header, "history shelf header")
            check(find(window, "objectName", "playlistsShelfHeader"), "playlists shelf header")
            var hub = header.parent
            check(hub, "library section")
            var shelf = find(window, "objectName", "historyShelf")
            var tiles = findAll(shelf.contentItem, function (i) { return i.hasOwnProperty("progress") && i.visible && i.title === "Watched" })
            verify(tiles.length === 1, "history tile")
            var x = tiles[0].mapToItem(shelf, 0, 0).x
            if (rtl) verify(x + tiles[0].width > shelf.width / 2, "the first tile sits on the right in a right-to-left language: " + Math.round(x))
            else verify(x < shelf.width / 2, "the first tile sits on the left: " + Math.round(x))
            mocks.historyModel.clear(); mocks.playlistsModel.clear()
            // ---- Watch page: action chips, cards under the player ----
            pb.qualities = [{ height: 720, highFrameRate: false, label: "720p", codec: "vp9" }]
            mocks.watchInfo.related.rows = [{ videoId: "w2", title: "Related", channel: "Chan", thumbnail: "" },
                                            { videoId: "w3", title: "Next", channel: "Chan", thumbnail: "" }]
            window.openVideo("w1", { title: "Watch" })
            return 600
        },
        function () { return settle(current().isWatchPage === true) },
        function () {
            page = current()
            pb.finishLoading(true)
            pb.viewCount = 1500; pb.likeCount = 320
            // RYD-01: the dislike estimate joins the row (the setting is on and the catalog has one).
            mocks.watchInfo.dislikeCount = 1200; mocks.watchInfo.dislikeText = "1.2K"
            return 400
        },
        function () {
            var row = find(page, "objectName", "actionChips")
            verify(row !== null && row.visible, "action chips")
            check(row, "action chips")
            checkRowStart(row, "action chips")
            checkChipsFit(row, "action chips")
            // The dislike chip: a number on the chip, the sentence for the screen reader.
            var dislike = find(row, "objectName", "dislikeChip")
            verify(dislike !== null && dislike.visible, "the dislike chip shows with an estimate")
            verify(chipsOf(row).indexOf(dislike) > 0, "the dislike chip is one of the row's chips, after the first")
            var spoken = dislike.Accessible.name
            verify(spoken.indexOf("1.2K") >= 0, "the spoken name carries the count: " + spoken)
            verify(spoken.indexOf("%1") < 0, "the placeholder is filled: " + spoken)
            if (harness.locale !== "" && harness.locale !== "en")
                verify(spoken !== "About 1.2K dislikes (estimate)", "the spoken name is translated [" + harness.locale + "]: " + spoken)
            mocks.watchInfo.dislikeCount = -1; mocks.watchInfo.dislikeText = ""
            check(find(page, "objectName", "descriptionCard"), "description card")
            check(find(page, "objectName", "commentsTeaser"), "comments teaser")
            // ---- Player settings sheet ----
            var sheet = find(page, "objectName", "playerSettings")
            verify(sheet !== null, "settings sheet")
            sheet.captionsAvailable = true
            sheet.openSheet()
            return 500
        },
        function () {
            var sheet = find(page, "objectName", "playerSettings")
            verify(sheet.open, "sheet open")
            check(sheet, "settings sheet rows")
            // Every row title the sheet can show (rows of features the mock session lacks too),
            // against the title label of a value row and of a switch row.
            var rows = findAll(sheet, function (i) { return i.hasOwnProperty("isSwitch") && i.visible })
            verify(rows.length >= 3, "rows shown: " + rows.length)
            function titleOf(row) { return t.texts(row)[0] }
            var valueTitle = null, switchTitle = null
            for (var r = 0; r < rows.length; ++r) {
                if (rows[r].isSwitch) switchTitle = titleOf(rows[r]); else valueTitle = titleOf(rows[r])
            }
            verify(valueTitle && switchTitle, "a value row and a switch row are shown")
            var room = valueTitle.width
            verify(switchTitle.width < room, "a switch row's title ends at the switch: " + switchTitle.width + " of " + room)
            var ids = ["quality", "speed", "captions", "audioTrack", "loop", "sleep", "stableVolume", "audioOnly"]
            var isSwitch = { loop: true, stableVolume: true, audioOnly: true }
            var bad = []
            for (var i = 0; i < ids.length; ++i) {
                var title = sheet.rowTitle(ids[i])
                var like = isSwitch[ids[i]] ? switchTitle : valueTitle
                var w = textWidth(title, like.minimumPixelSize > 0 && like.fontSizeMode !== Text.FixedSize ? like.minimumPixelSize : like.font.pixelSize)
                if (w > like.width + 1) bad.push("\"" + title + "\" needs " + Math.round(w) + " of " + Math.round(like.width))
            }
            verify(bad.length === 0, "sheet row titles fit: " + bad.join("; "))
            var switches = findAll(sheet, function (i) { return i.hasOwnProperty("automaticCheck") && i.hasOwnProperty("checked") && i.visible })
            // Sleep timer choices and values.
            var values = [sheet.sleepLabel(0), sheet.sleepLabel(-1), sheet.sleepLabel(1), sheet.sleepLabel(90), sheet.speedValue(1)]
            for (var j = 0; j < values.length; ++j)
                verify(textWidth(values[j], Theme.fontSizeMedium) <= room + 1, "sheet choice fits: " + values[j])
            if (rtl) {
                var s = switches[0].mapToItem(sheet, 0, 0).x
                verify(s + switches[0].width < sheet.width / 2, "the switch sits on the left in a right-to-left language: " + Math.round(s))
            }
            sheet.level = "quality"
            return 200
        },
        function () {
            var sheet = find(page, "objectName", "playerSettings")
            check(sheet, "settings sheet, quality choices")
            sheet.level = "speed"
            return 200
        },
        function () {
            var sheet = find(page, "objectName", "playerSettings")
            check(sheet, "settings sheet, speed choices")
            sheet.hide()
            // ---- End screen ----
            pb.playing = false; pb.playerChanged(); pb.ended()
            return 300
        },
        function () {
            check(find(page, "objectName", "playerEndScreen"), "end screen suggestions")
            pb.upNextVideoId = "w2"; pb.upNextSecondsLeft = 5
            return 200
        },
        function () {
            var card = find(page, "objectName", "upNextCard")
            verify(card !== null && card.visible, "up-next card")
            check(card, "up-next card")
            pb.upNextVideoId = ""
            stack().pop(undefined, PageStackAction.Immediate)
            pb.stop()
            window.selectSection("subscriptions")
            // ---- Playlist head: Play all, the chips (Shuffle, Share, Copy link), the feedback line ----
            mocks.playlistsModel.listRows = { "p1": [{ videoId: "a", title: "Video a", channel: "Channel", channelId: "", durationSeconds: 60, thumbnail: "" },
                                                     { videoId: "b", title: "Video b", channel: "Channel", channelId: "", durationSeconds: 60, thumbnail: "" },
                                                     { videoId: "c", title: "Video c", channel: "Channel", channelId: "", durationSeconds: 60, thumbnail: "" }] }
            stack().push(Qt.resolvedUrl("../../Clients/Sailfish/qml/pages/PlaylistPage.qml"), { playlistId: "p1" }, PageStackAction.Immediate)
            return 500
        },
        function () { return settle(find(current(), "objectName", "playlistHeader") !== null) },
        function () {
            page = current()
            var head = find(page, "objectName", "playlistHeader")
            verify(head !== null, "the playlist page has its head")
            // A saved YouTube playlist: every chip of the head shows.
            page.items.kind = 2
            page.items.remoteId = "PLabcdefghijklmnopqrstuvwxyz012345"
            return 200
        },
        function () {
            var head = find(page, "objectName", "playlistHeader")
            var strip = find(head, "objectName", "playlistChips")
            verify(find(head, "objectName", "shareChip").visible, "Share shows on a saved YouTube playlist")
            compare(chipsOf(strip).length, 3, "Shuffle, Share and Copy link")
            check(listOf(page).headerItem, "playlist head")
            checkButtonFits(find(head, "objectName", "playAllButton"), "Play all")
            checkRowStart(strip, "playlist chips")
            checkChipsFit(strip, "playlist chips")
            // The feedback line under the chips wraps inside the margins.
            head.message = qsTranslate("WatchActions", "Sharing is not available; link copied")
            return 100
        },
        function () {
            var head = find(page, "objectName", "playlistHeader")
            check(head, "playlist head with its feedback line")
            head.message = ""
            stack().pop(undefined, PageStackAction.Immediate)
            mocks.playlistsModel.listRows = ({})
            // ---- Channel head: counts, About, Subscribe, tabs ----
            stack().push(Qt.resolvedUrl("../../Clients/Sailfish/qml/pages/ChannelPage.qml"), { reference: "@channel" }, PageStackAction.Immediate)
            return 500
        },
        function () { return settle(listOf(current()) !== null) },
        function () {
            page = current()
            var list = listOf(page)
            verify(list !== null, "the channel page has its list")
            var m = list.model
            compare(m.reference, "@channel", "the page opened the channel")
            m.channelId = "UCabcdefghijklmnopqrstuv"
            m.title = "Channel"; m.handle = "@channel"
            // No text from YouTube: the app's own count lines (the translated ones).
            m.subscriberCount = 1234567; m.videoCount = 6200
            m.description = "About this channel. A description long enough to be cut after two lines, so that the card offers to show the rest of it when it is tapped. "
                          + "More words follow here, and still more after them, until the text is surely longer than two lines on any phone."
            // A music channel: the most tabs a channel shows.
            m.tabs = ["videos", "shorts", "streams", "playlists", "releases"]
            m.fallbackTab = true
            t.content = [m.description]
            return 300
        },
        function () {
            var list = listOf(page)
            check(list.headerItem, "channel head, five tabs")
            checkButtonFits(find(list.headerItem, "objectName", "channelPageSubscribe"), "Subscribe")
            var about = find(list.headerItem, "objectName", "channelAbout")
            verify(about !== null && about.visible && about.enabled, "the About card offers the rest of the description")
            var strip = find(list.headerItem, "objectName", "channelTabs")
            verify(strip !== null, "the tab row")
            checkTabs(strip, 5, "channel tabs")
            var tabs = tabsOf(strip)
            verify(tabs[0].current && tabInView(strip, tabs[0]), "the row rests on the first tab, the current one")
            var x0 = tabs[0].mapToItem(strip, 0, 0).x, x1 = tabs[1].mapToItem(strip, 0, 0).x
            if (rtl) verify(x1 < x0, "the second tab is left of the first in a right-to-left language")
            else verify(x1 > x0, "the second tab is right of the first")
            // The last tab becomes the current one (a tap, or a channel that opens on it).
            list.model.tab = "releases"
            return 200
        },
        function () {
            var list = listOf(page)
            harness.polish(window.contentItem)
            var strip = find(list.headerItem, "objectName", "channelTabs")
            var tabs = tabsOf(strip)
            verify(tabs[4].current, "the last tab is the current one")
            verify(tabInView(strip, tabs[4]), "the row moved to show the current tab: it is at "
                   + Math.round(tabs[4].mapToItem(strip, 0, 0).x) + " in " + strip.width)
            mocks.subscriptionsModel.toggle("UCabcdefghijklmnopqrstuv", "Channel", "")
            list.model.tab = "videos"
            list.model.tabs = []
            list.model.fallbackTab = false
            return 200
        },
        function () {
            var list = listOf(page)
            check(list.headerItem, "channel head, four tabs")
            var strip = find(list.headerItem, "objectName", "channelTabs")
            checkTabs(strip, 4, "channel tabs, the usual four")
            verify(tabInView(strip, tabsOf(strip)[0]), "back on the first tab")
            checkButtonFits(find(list.headerItem, "objectName", "channelPageSubscribe"), "Subscribed")
            // The placeholder of an empty tab.
            check(page, "channel page, empty tab")
            mocks.subscriptionsModel.toggle("UCabcdefghijklmnopqrstuv", "Channel", "")
            t.content = []
            stack().pop(undefined, PageStackAction.Immediate)
        }
    ]
}
