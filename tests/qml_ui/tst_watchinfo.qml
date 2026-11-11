// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
import QtQuick 2.6
import Sailfish.Silica 1.0

// The watch page below the player (docs/DESIGN.md §3.4): views and age under the title, a row of
// action chips (like count, share, download, save, copy link, browser), the description card, the
// comments teaser, related videos as cards.
UiTest {
    id: t
    property var pb: mocks.playback
    property var info: mocks.watchInfo
    property var page
    function chip(text) {
        var row = find(page, "objectName", "actionChips")
        var l = findAll(row, function (i) { return i.hasOwnProperty("passive") && i.text === text })
        return l.length ? l[0] : null
    }
    steps: [
        function () {
            info.related.rows = [{ videoId: "w2", title: "Related one", channel: "Chan", thumbnail: "" }]
            window.openVideo("w1", { title: "Watch info" })
            return 500
        },
        function () {
            page = current()
            pb.finishLoading(true)
            pb.viewCount = 1500
            pb.likeCount = 320
            pb.channelId = "UC1234567890123456789012"
            info.subscriberText = "1.2M subscribers"
            pb.detailsChanged()
            return 300
        },
        function () {
            // Owner row: the name with the subscriber count on its own line under it, and the
            // app's one Subscribe button at the end.
            var owner = find(page, "objectName", "ownerRow")
            verify(owner !== null && owner.visible, "owner row")
            compare(find(owner, "objectName", "ownerName").text, "Channel", "the channel's name alone on the first line")
            var subs = findAll(owner, function (i) { return i.visible && i.text === "1.2M subscribers" })
            compare(subs.length, 1, "the subscriber count on its own line")
            var button = find(owner, "objectName", "watchSubscribe")
            verify(button !== null && button.visible, "Subscribe on the owner row")
            verify(!button.subscribed, "not subscribed yet")
            button.clicked(null)
            verify(mocks.subscriptionsModel.isSubscribed("UC1234567890123456789012"), "subscribed from the watch page")
            verify(button.subscribed, "the button shows the new state")
            mocks.subscriptionsModel.toggle("UC1234567890123456789012", "", "")
            // The chips start at the leading edge.
            var strip = find(page, "objectName", "actionChips")
            verify(!strip.mirrored, "left-to-right")
            compare(strip.contentX, 0, "the chip strip starts at its left end")
        },
        function () {
            var row = find(page, "objectName", "actionChips")
            verify(row !== null && row.visible, "chip row")
            var likes = findAll(row, function (i) { return i.hasOwnProperty("passive") && i.passive && i.visible })
            compare(likes.length, 1, "the like count is a passive chip")
            // The dislike estimate shows only when the catalog has one (the setting is on).
            var dislike = find(row, "objectName", "dislikeChip")
            verify(dislike !== null && !dislike.visible, "no dislike chip without an estimate")
            info.dislikeCount = 1200; info.dislikeText = "1.2K"
            verify(dislike.visible && dislike.passive, "the estimate is a passive chip beside the likes")
            compare(dislike.text, "\u2248 1.2K", "marked as an estimate")
            compare(dislike.iconRotation, 180, "the thumb points down")
            info.dislikeCount = -1; info.dislikeText = ""
            verify(!dislike.visible, "and leaves with the estimate")
            verify(chip("Share") !== null && chip("Copy link") !== null && chip("Open in browser") !== null
                   && chip("Add to playlist") !== null, "share, copy, browser and save chips")
            verify(row.contentWidth > 0, "the row scrolls sideways when it is wider than the page")
            // Views sit directly under the title; likes are not repeated there.
            var meta = findAll(page, function (i) { return i.visible && typeof i.text === "string" && i.text.indexOf("views") >= 0 && i.text.indexOf("likes") < 0 })
            verify(meta.length >= 1, "views under the title")
            var cards = findAll(page, function (i) { return i.hasOwnProperty("cardLayout") && i.cardLayout && i.videoId === "w2" })
            compare(cards.length, 1, "related videos are cards")
            // The description is a card; a tap on it expands and collapses.
            var card = find(page, "objectName", "descriptionCard")
            verify(card !== null && card.visible, "description card")
            var before = info.collapsed
            info.descriptionTruncated = true
            card.clicked(null)
            compare(info.collapsed, !before, "a tap on the card toggles the description")
            // Comments teaser: the count and the first comment once loaded; a tap opens the page.
            var teaser = find(page, "objectName", "commentsTeaser")
            verify(teaser !== null && teaser.visible && !teaser.loaded, "teaser before the comments load")
            mocks.commentsModel.videoId = "w1"
            mocks.commentsModel.totalText = "1.2K"
            mocks.commentsModel.append({ rowType: "comment", author: "A", avatar: "", text: "First comment" })
            return 200
        },
        function () {
            var teaser = find(page, "objectName", "commentsTeaser")
            verify(teaser.loaded, "teaser shows the loaded comments")
            compare(findAll(teaser, function (i) { return i.visible && i.text === "Comments 1.2K" }).length, 1, "count in the teaser")
            compare(findAll(teaser, function (i) { return i.visible && i.text === "First comment" }).length, 1, "first comment in the teaser")
            teaser.clicked(null)
            return 700
        },
        function () {
            verify(current().isWatchPage !== true && current().hasOwnProperty("token"), "the teaser opens the comments page")
            stack().pop(undefined, PageStackAction.Immediate)
            return 300
        },
        function () {
            mocks.commentsModel.clear(); mocks.commentsModel.totalText = ""; mocks.commentsModel.videoId = ""
            info.related.rows = []
            pb.viewCount = -1; pb.likeCount = -1
            stack().pop(undefined, PageStackAction.Immediate)
            pb.stop()
        }
    ]
}
