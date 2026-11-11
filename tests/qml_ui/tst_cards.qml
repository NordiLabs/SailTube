// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
import QtQuick 2.6
import Sailfish.Silica 1.0
import "../../Clients/Sailfish/qml/logic/Rows.js" as Rows

// Cards (docs/DESIGN.md §4.1): lists that opt in show a full-width thumbnail with the avatar (or
// the channel's initial), title and one meta line under it on portrait-wide rows; landscape and
// channel rows keep the compact row; the landing feed uses cards and its skeleton has their shape.
UiTest {
    id: t
    property var feed: mocks.feedModel
    function make(props) {
        var c = Qt.createComponent(Qt.resolvedUrl("../../Clients/Sailfish/qml/components/VideoListItem.qml"))
        var o = c.createObject(current(), props)
        verify(o !== null, "row created: " + c.errorString())
        return o
    }
    function thumbOf(row) {
        var kids = row.contentItem.children
        for (var i = 0; i < kids.length; ++i)
            if (kids[i].hasOwnProperty("color") && kids[i].hasOwnProperty("radius") && Math.abs(kids[i].height - Rows.thumbHeight(kids[i].width)) < 1 && kids[i].width > Theme.iconSizeMedium) return kids[i]
        return null
    }
    function texts(row, text) { return findAll(row, function (i) { return i.visible && i.text === text }) }
    steps: [
        function () {
            var w = Screen.width
            var card = make({ width: w, cardLayout: true, videoTitle: "Title", channelName: "nordtile · 2 days ago", views: 1200, videoId: "c1" })
            verify(card.card, "portrait-wide row of an opted-in list is a card")
            compare(Math.round(thumbOf(card).width), Math.round(w), "full-width thumbnail")
            verify(card.naturalHeight > thumbOf(card).height + Theme.iconSizeMedium, "avatar and text under the picture")
            compare(texts(card, "N").length, 1, "the channel's initial while there is no avatar")
            compare(texts(card, "nordtile · 2 days ago · 1.2K views").length
                    + texts(card, "nordtile · 2 days ago · 1,2K views").length
                    + texts(card, "nordtile · 2 days ago · 1.2 K views").length >= 1, true, "one meta line: channel, age, views")
            card.destroy()

            var wide = make({ width: w * 2, cardLayout: true, videoTitle: "Title", channelName: "C", videoId: "c2" })
            verify(!wide.card, "landscape keeps the compact row")
            compare(Math.round(thumbOf(wide).width), Rows.thumbWidth(w * 2, w))
            wide.destroy()

            var plain = make({ width: w, videoTitle: "Title", channelName: "C", videoId: "c3" })
            verify(!plain.card, "lists that did not opt in keep rows")
            plain.destroy()

            var channel = make({ width: w, cardLayout: true, channelRow: true, videoTitle: "Channel", channelName: "1M subscribers" })
            verify(!channel.card, "a channel result keeps its round row")
            channel.destroy()

            feed.channelCount = 2
            feed.busy = true
            return 200
        },
        function () {
            var s = findAll(window, function (i) { return i.hasOwnProperty("viewHeight") && i.hasOwnProperty("cards") && i.hasOwnProperty("card") })
            verify(s.length >= 1 && s[0].cards, "the landing feed's skeleton is card-shaped")
            feed.append({ videoId: "f1", title: "Feed card", channel: "Chan", thumbnail: "", duration: "1:00",
                          viewCount: 5, live: false, publishedText: "1 day ago" })
            feed.busy = false
            return 200
        },
        function () {
            var rows = findAll(window, function (i) { return i.hasOwnProperty("cardLayout") && i.visible })
            verify(rows.length >= 1 && rows[0].cardLayout, "feed rows opt in to cards")
            feed.clear(); feed.channelCount = 0
        }
    ]
}
