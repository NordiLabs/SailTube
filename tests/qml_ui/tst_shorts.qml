// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
import QtQuick 2.6
import Sailfish.Silica 1.0

// Shorts section (docs/DESIGN.md §3.5), stage 2: the Short on screen plays by itself in the shared
// player, as a Short (openShort), with no watch page; the next two are asked for; a tap is the
// user's own pause and resumes; a swipe starts the next one; leaving the tab or a pushed page holds
// it and coming back releases it (a hold keeps the user's pause); a Short has no mini-player; the live picture replaces the poster only when the player is ready; a failed Short
// offers Retry; the mini-player stays away and the bottom bar stays. The details of a Short
// (counts, channel, related video) arrive after its row exists, and the model's `get()` hands out
// snapshots that tell nobody: the rail and the bottom block must follow a row that changes later.
UiTest {
    id: t
    property var sm: mocks.shortsModel
    property var pb: mocks.playback
    function shortsView() { return find(window, "objectName", "shortsView") }
    function pages(v) { return findAll(v, function (i) { return i.hasOwnProperty("entry") && i.hasOwnProperty("isShown") }) }
    function shownPage(v) { var p = pages(v).filter(function (i) { return i.isShown }); return p.length ? p[0] : null }
    function miniPlayer() {
        var m = findAll(window, function (i) { return i.hasOwnProperty("queueLength") && i.hasOwnProperty("shown") })
        return m.length ? m[0] : null
    }
    steps: [
        function () {
            // The first Short has all its details; the second has none and its creator turned
            // comments off; the third has only what a popular Short has before its details arrive.
            sm.append({ videoId: "shortAAAA01", title: "Short one", thumbnail: "", durationSeconds: -1,
                        channel: "C", channelId: "UC1234567890123456789012", channelAvatar: "",
                        likeText: "4.8K", commentText: "120", commentsDisabled: false,
                        description: "A long description of the first Short, long enough to need more than two lines when it is shown on a narrow screen, so a tap opens it in place and a second tap closes it again.",
                        relatedVideoId: "relVideo0001", relatedVideoTitle: "The video it came from", relatedVideoThumbnail: "" })
            sm.append({ videoId: "shortAAAA02", title: "Short two", thumbnail: "", commentsDisabled: true })
            sm.append({ videoId: "shortAAAA03", title: "Short three", thumbnail: "" })
            pb.calls = []
            window.selectSection("shorts")
            return 700
        },
        function () {
            var v = shortsView()
            verify(v !== null, "Shorts view loaded from the bar")
            compare(v.count, 3, "the model's three Shorts")
            compare(v.row(1).videoId, "shortAAAA02")
            // The harness window may not count as the front application: say so, as a real one is.
            v.appActive = true
            return 300
        },
        function () {
            var v = shortsView()
            // The first Short opens by itself, as a Short (the session loops it), in place: no watch
            // page, the next two held or resolved.
            compare(v.shownId, "shortAAAA01", "the first Short is the shown one")
            verify(pb.calls.indexOf("openShort:shortAAAA01") >= 0, "it plays by itself: " + pb.calls.join(","))
            verify(pb.calls.indexOf("open:shortAAAA01") < 0, "through openShort, not open")
            compare(pb._lastHints.title, "Short one", "with the row's hints")
            verify(pb.shortActive, "the session knows it is a Short")
            verify(sm.calls.indexOf("current:0") >= 0, "catalog is told which Short is on screen: " + sm.calls.join(","))
            verify(pb.calls.indexOf("ahead:shortAAAA02,shortAAAA03") >= 0, "the next two are asked for: " + pb.calls.join(","))
            verify(pb.calls.indexOf("openShort:shortAAAA01") < pb.calls.indexOf("ahead:shortAAAA02,shortAAAA03"), "after the open of the current one")
            verify(current().isMainPage === true, "no watch page")
            verify(window.navigationShown, "the bottom bar stays")
            verify(!miniPlayer().shown, "no mini-player: a Short is never parked")
            // Starting: the poster with a spinner; the live picture is not shown yet.
            var p = shownPage(v)
            verify(find(p, "objectName", "shortsBusy").running, "a spinner over the poster while it starts")
            verify(!find(p, "objectName", "shortsLive").visible, "no live picture yet")
            pb.finishLoading(true)
            return 200
        },
        function () {
            var v = shortsView(), p = shownPage(v)
            var live = find(p, "objectName", "shortsLive")
            verify(!live.visible, "ready but no frame yet: still the poster")
            find(live, "objectName", "shortsSurface").hasFrame = true
            verify(live.visible, "the first frame: the live picture replaces the poster")
            verify(!find(p, "objectName", "shortsBusy").running, "no spinner")
            verify(find(v, "objectName", "shortsProgress").visible, "the progress line shows")
            verify(window.videoVisible, "the display stays on while a Short plays")
            verify(sm.playbackRunning, "catalog is told the Short on screen plays")
            // A tap is the user's own pause, through the session; no watch page.
            pb.calls = []
            p.clicked(null)
            verify(!pb.playing && v.userPaused, "a tap pauses")
            verify(!sm.playbackRunning, "and catalog is told it no longer plays")
            verify(pb.calls.indexOf("pause") >= 0, "through the session: " + pb.calls.join(","))
            verify(find(p, "objectName", "shortsPauseMark").visible, "a pause mark")
            p.clicked(null)
            verify(pb.playing && !v.userPaused, "a second tap resumes")
            verify(current().isMainPage === true, "still no watch page")
            return 100
        },
        function () {
            // The rail: like, comments, save, share, in that order at the right edge, with the counts
            // as the row gives them.
            var v = shortsView(), p = shownPage(v)
            var rail = find(p, "objectName", "shortsRail")
            verify(rail !== null && rail.visible, "the right rail")
            var items = ["shortsLike", "shortsComments", "shortsSave", "shortsShare"].map(function (n) { return find(rail, "objectName", n) })
            verify(items.every(function (i) { return i !== null && i.visible }), "four buttons")
            verify(items[0].y < items[1].y && items[1].y < items[2].y && items[2].y < items[3].y, "from top to bottom")
            compare(items[0].count, "4.8K", "likes")
            compare(items[1].count, "120", "comments")
            verify(rail.mapToItem(null, 0, 0).x + rail.width >= v.width - Theme.paddingLarge * 2, "at the right edge")
            verify(!items[1].inert, "comments are on")
            // Like: the app cannot like, so a tap says so; the button stays.
            var notice = find(v, "objectName", "shortsNotice")
            verify(!notice.visible, "no notice yet")
            items[0].clicked()
            verify(notice.visible, "a notice answers the like")
            compare(findAll(notice, function (i) { return i.text === "Liking videos is not available yet" }).length, 1, "it says why")
            verify(items[0].visible, "the like button stays")
            // Share: the harness has no D-Bus, hence no share sheet: the link is copied instead and
            // the notice says so (WatchActions' own fallback, which a missing sheet takes).
            findAll(v, function (i) { return i.hasOwnProperty("shareAction") })[0].shareAction = false
            items[3].clicked()
            compare(Clipboard.text, "https://www.youtube.com/shorts/shortAAAA01", "the Short's own address")
            verify(notice.visible, "and the notice shows the message")
            return 100
        },
        function () {
            // The bottom block: channel, Subscribe, the description in two lines that open on a tap,
            // the related video.
            var v = shortsView(), p = shownPage(v)
            var info = find(p, "objectName", "shortsInfo")
            compare(find(info, "objectName", "shortsChannel").text, "C", "the channel's name")
            var sub = find(info, "objectName", "shortsSubscribe")
            verify(sub.visible, "Subscribe, the app's one button")
            verify(!mocks.subscriptionsModel.isSubscribed("UC1234567890123456789012"), "not subscribed yet")
            sub.clicked(null)
            verify(mocks.subscriptionsModel.isSubscribed("UC1234567890123456789012"), "subscribed from the Short")
            mocks.subscriptionsModel.toggle("UC1234567890123456789012", "", "")
            // The caption is the title; the description, when the Short has one, opens under it.
            var desc = find(info, "objectName", "shortsDescription")
            compare(desc.text, "Short one", "the title is the caption")
            compare(desc.maximumLineCount, 2, "two lines")
            var area = find(info, "objectName", "shortsDescriptionArea")
            verify(area.enabled, "a Short with a description opens on a tap")
            area.clicked(null)
            compare(desc.maximumLineCount, 8, "opened in place")
            verify(desc.text.indexOf("A long description of the first Short") >= 0 && desc.text.indexOf("Short one") === 0, "the description under the title")
            area.clicked(null)
            compare(desc.maximumLineCount, 2, "and closed again")
            compare(desc.text, "Short one", "back to the title alone")
            var chip = find(info, "objectName", "shortsRelated")
            verify(chip.visible, "the related video chip")
            compare(findAll(chip, function (i) { return i.text === "The video it came from" }).length, 1, "with its title")
            compare(chip.parent.height, Theme.itemSizeExtraSmall, "its room is kept")
            pb.calls = []
            findAll(chip, function (i) { return typeof i.clicked === "function" && i.hasOwnProperty("pressed") })[0].clicked(null)
            return 600
        },
        function () {
            verify(current().isWatchPage === true, "the related video opens on the normal watch page")
            verify(pb.calls.indexOf("open:relVideo0001") >= 0, "and plays: " + pb.calls.join(","))
            stack().pop(undefined, PageStackAction.Immediate)
            return 500
        },
        function () {
            // Comments open the existing page (the Short pauses under it); save opens the playlist dialog.
            var v = shortsView(), p = shownPage(v)
            verify(v.active, "back on the Shorts section")
            pb.calls = []
            find(find(p, "objectName", "shortsRail"), "objectName", "shortsComments").clicked()
            return 500
        },
        function () {
            var v = shortsView()
            verify(!v.active, "the comments page covers the section")
            compare(current().videoId, "shortAAAA01", "the comments of this Short")
            compare(current().token, "", "the model fetches the watch data itself")
            stack().pop(undefined, PageStackAction.Immediate)
            return 500
        },
        function () {
            var v = shortsView(), p = shownPage(v)
            verify(v.active, "back")
            find(find(p, "objectName", "shortsRail"), "objectName", "shortsSave").clicked()
            return 500
        },
        function () {
            var v = shortsView()
            verify(!v.active, "the dialog covers the section")
            compare(current().video.videoId, "shortAAAA01", "it saves this Short")
            compare(current().video.title, "Short one")
            stack().pop(undefined, PageStackAction.Immediate)
            return 500
        },
        function () {
            // A Short whose details are not known: the rail keeps its shape, comments are off, there is no
            // like count, the channel line and the related chip keep their room.
            var v = shortsView()
            verify(v.active, "back")
            // A swipe: the pager settles on the second Short; it starts at once, the one after is held.
            var v = shortsView()
            pb.calls = []
            find(v, "objectName", "shortsPager").currentIndex = 1
            return 400
        },
        function () {
            var v = shortsView(), p = shownPage(v)
            var rail = find(p, "objectName", "shortsRail")
            var like = find(rail, "objectName", "shortsLike"), comments = find(rail, "objectName", "shortsComments")
            verify(like.visible && like.count === "", "the like button stays, with no number")
            verify(comments.visible && comments.inert, "comments stay, switched off for this Short")
            compare(find(rail, "objectName", "shortsSave").visible && find(rail, "objectName", "shortsShare").visible, true, "save and share stay")
            var info = find(p, "objectName", "shortsInfo")
            verify(!find(info, "objectName", "shortsSubscribe").visible, "no Subscribe without a channel")
            verify(!find(info, "objectName", "shortsRelated").visible, "no related chip")
            compare(find(info, "objectName", "shortsRelated").parent.height, Theme.itemSizeExtraSmall, "but its room is kept")
            compare(find(info, "objectName", "shortsDescription").text, "Short two", "the title is the caption")
            verify(!find(info, "objectName", "shortsDescriptionArea").enabled || find(info, "objectName", "shortsDescription").truncated,
                   "no description: nothing to open unless the title itself is cut")
            // The details arrive after the row exists (catalog's next request answers): the model
            // changes the row's data and tells nobody through get().
            sm.setProperty(1, "likeText", "2.2K")
            sm.setProperty(1, "commentText", "9")
            sm.setProperty(1, "commentsDisabled", false)
            sm.setProperty(1, "channel", "Late channel")
            sm.setProperty(1, "channelId", "UC1234567890123456789012")
            sm.setProperty(1, "relatedVideoId", "relLate00001")
            sm.setProperty(1, "relatedVideoTitle", "Late related")
            sm.revision += 1
            return 100
        },
        function () {
            // The rail and the bottom block follow the row that changed: before the fix they were
            // read once per page and stayed empty until the page was recreated.
            var v = shortsView(), p = shownPage(v)
            var rail = find(p, "objectName", "shortsRail"), info = find(p, "objectName", "shortsInfo")
            compare(find(rail, "objectName", "shortsLike").count, "2.2K", "the like count that arrived")
            compare(find(rail, "objectName", "shortsComments").count, "9", "the comment count that arrived")
            verify(!find(rail, "objectName", "shortsComments").inert, "comments are on")
            compare(find(info, "objectName", "shortsChannel").text, "Late channel", "the channel that arrived")
            verify(find(info, "objectName", "shortsSubscribe").visible, "Subscribe once there is a channel id")
            var chip = find(info, "objectName", "shortsRelated")
            verify(chip.visible, "the related chip that arrived")
            compare(findAll(chip, function (i) { return i.text === "Late related" }).length, 1, "with its title")
            return 100
        },
        function () {
            var v = shortsView()
            compare(v.shownIndex, 1, "the pager settled on the second Short")
            compare(v.shownId, "shortAAAA02")
            verify(pb.calls.indexOf("openShort:shortAAAA02") >= 0, "it starts at once: " + pb.calls.join(","))
            verify(pb.calls.indexOf("ahead:shortAAAA03") >= 0, "and the one after it is asked for: " + pb.calls.join(","))
            compare(pb.videoId, "shortAAAA02", "the one left behind is replaced")
            verify(!v.userPaused, "the new Short plays")
            pb.finishLoading(true)
            return 100
        },
        function () {
            // Leaving the tab pauses; coming back resumes the same Short.
            var v = shortsView()
            pb.calls = []
            window.selectSection("home")
            return 400
        },
        function () {
            var v = shortsView()
            verify(!v.active, "the section is not the current one")
            verify(pb.calls.indexOf("hold:away") >= 0 && !pb.playing && pb.shortHeld, "leaving the tab holds the Short: " + pb.calls.join(","))
            verify(!window.videoVisible, "and the display may go off")
            verify(!miniPlayer().shown, "a held Short has no mini-player either: it comes back with the tab")
            pb.calls = []
            window.selectSection("shorts")
            return 400
        },
        function () {
            var v = shortsView()
            verify(v.active, "back on the tab")
            verify(pb.calls.indexOf("release:away") >= 0 && pb.playing && !pb.shortHeld, "the hold is released and it resumes: " + pb.calls.join(","))
            verify(pb.calls.indexOf("openShort:shortAAAA02") < 0, "the same Short, no new open: " + pb.calls.join(","))
            // The user's own pause survives a hold and a release.
            shownPage(v).clicked(null)
            verify(!pb.playing, "paused by a tap")
            pb.calls = []
            window.selectSection("home")
            return 400
        },
        function () {
            var v = shortsView()
            verify(pb.calls.indexOf("hold:away") >= 0, "held again: " + pb.calls.join(","))
            pb.calls = []
            window.selectSection("shorts")
            return 400
        },
        function () {
            var v = shortsView()
            verify(pb.calls.indexOf("release:away") >= 0 && !pb.playing, "released, and still paused: the user wins: " + pb.calls.join(","))
            verify(v.userPaused, "the pause mark is back")
            shownPage(v).clicked(null)
            verify(pb.playing, "a tap resumes")
            // A locked screen or another app: the session holds a Short by itself; the pager only lets
            // the display go off.
            v.appActive = false
            return 100
        },
        function () {
            var v = shortsView()
            verify(!window.videoVisible, "the app is not in front: the display may go off")
            v.appActive = true
            return 100
        },
        function () {
            var v = shortsView()
            verify(window.videoVisible, "the app is back")
            // A pushed page (the comments, the save dialog) covers the section: pause, and resume on return.
            pb.calls = []
            window.openVideo("other", { title: "Other" })
            return 600
        },
        function () {
            var v = shortsView()
            verify(current().isWatchPage === true, "another video on its watch page")
            verify(!v.active, "the section is covered")
            pb.finishLoading(true)
            stack().pop(undefined, PageStackAction.Immediate)
            return 500
        },
        function () {
            var v = shortsView()
            verify(v.active, "back")
            verify(pb.calls.indexOf("openShort:shortAAAA02") >= 0, "the session held another video: the Short opens again: " + pb.calls.join(","))
            compare(pb.videoId, "shortAAAA02")
            // A Short that cannot play: the reason and Retry; the pager is still usable.
            pb.fail("This Short is not available.")
            return 200
        },
        function () {
            var v = shortsView(), p = shownPage(v)
            var err = find(p, "objectName", "shortsError")
            verify(err.visible, "the error shows on the Short")
            verify(!find(p, "objectName", "shortsBusy").running, "no spinner on a failure")
            var retry = findAll(err, function (i) { return i.visible && i.text === "Retry" && typeof i.clicked === "function" })
            compare(retry.length, 1, "Retry")
            pb.calls = []
            retry[0].clicked(null)
            verify(pb.calls.indexOf("retry") >= 0, "Retry tries again: " + pb.calls.join(","))
            pb.stop()
            window.selectSection("subscriptions")
            sm.clear()
        }
    ]
}
