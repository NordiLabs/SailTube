// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
import QtQuick 2.6
import Sailfish.Silica 1.0

// The head of a playlist (docs/DESIGN.md §4.8): each part shows only when the list can do it
// (cover, Play all, Shuffle, Share); Play all and Shuffle hand the whole list to the queue and
// open the first video; a saved YouTube playlist can be shared, a local list cannot.
UiTest {
    id: t
    property var head
    property int played: 0
    property int shuffles: 0
    function row(id) {
        return { videoId: id, title: "Video " + id, channel: "Channel", channelId: "", durationSeconds: 60, thumbnail: "" }
    }
    function part(name) { return find(t.head, "objectName", name) }
    steps: [
        function () {
            var c = Qt.createComponent(Qt.resolvedUrl("../../Clients/Sailfish/qml/components/PlaylistHeader.qml"))
            t.head = c.createObject(current(), { width: Screen.width })
            verify(t.head !== null, "playlist header: " + c.errorString())
            t.head.playRequested.connect(function () { t.played += 1 })
            t.head.shuffleRequested.connect(function () { t.shuffles += 1 })
            return 200
        },
        function () {
            var h = t.head, button = part("playAllButton"), chip = part("shuffleChip")
            var strip = part("playlistChips"), cover = part("playlistCover").parent
            verify(button !== null && chip !== null && strip !== null, "the parts exist")
            verify(!button.visible && !chip.visible, "an empty list offers nothing to play")
            verify(strip.empty, "no chip shows")
            compare(strip.height, 0, "no chips, no strip")
            verify(!cover.visible, "no picture, no cover")
            compare(cover.height, 0)
            var emptyHeight = h.height

            h.count = 1
            verify(button.visible, "one video: Play all")
            verify(!chip.visible, "one video cannot be shuffled")
            h.count = 3
            verify(chip.visible && !strip.empty, "Shuffle with two or more")
            verify(strip.height > 0, "the strip takes its row")
            h.queue = false
            verify(!chip.visible, "no queue, no Shuffle")
            h.queue = true
            h.coverUrl = "image://theme/icon-m-play"
            verify(cover.visible && cover.height > 0, "the cover shows with a picture")
            verify(cover.width <= h.width - 2 * Theme.horizontalPageMargin, "the cover stays inside the margins")
            verify(h.height > emptyHeight, "the head grows with its parts")

            button.clicked(null)
            chip.clicked(null)
            compare(t.played, 1, "Play all asks the page")
            compare(t.shuffles, 1, "Shuffle asks the page")

            var before = h.height
            h.message = "Link copied"
            verify(h.height > before, "the feedback line takes its room")
            h.destroy()
            return 200
        },
        function () {
            // The shell plays a list as the queue, from the chosen row; rows without an id are left out.
            window.playQueue([row("q1"), { title: "no id" }, row("q2"), row("q3")], 2)
            return 600
        },
        function () {
            verify(current().isWatchPage === true, "the chosen row opens on the watch page")
            var calls = mocks.playback.calls
            verify(calls.indexOf("open:q2") >= 0, "the chosen row plays first: " + calls.join(" "))
            verify(calls.indexOf("setQueue:q1,q2,q3@1") >= 0, "the list is the queue: " + calls.join(" "))
            verify(calls.indexOf("open:q2") < calls.indexOf("setQueue:q1,q2,q3@1"), "the queue follows the open")
            stack().pop(undefined, PageStackAction.Immediate)
            mocks.playback.stop()
            return 300
        },
        function () {
            mocks.playlistsModel.listRows = { "p1": [row("a"), row("b"), row("c")] }
            stack().push(Qt.resolvedUrl("../../Clients/Sailfish/qml/pages/PlaylistPage.qml"), { playlistId: "p1" }, PageStackAction.Immediate)
            return 500
        },
        function () {
            t.head = find(current(), "objectName", "playlistHeader")
            verify(t.head !== null, "the playlist page has the head")
            compare(t.head.count, 3)
            verify(part("playAllButton").visible && part("shuffleChip").visible, "Play all and Shuffle")
            verify(!part("shareChip").visible, "a local list has no public address")
            compare(current().shareLink, "")
            // A saved YouTube playlist is shared by its public address.
            current().items.kind = 2
            current().items.remoteId = "PLabcdefghijklmnopqrstuvwxyz012345"
            compare(current().shareLink, "https://www.youtube.com/playlist?list=PLabcdefghijklmnopqrstuvwxyz012345")
            verify(part("shareChip").visible, "Share on a saved YouTube playlist")
            part("shuffleChip").clicked(null)
            return 600
        },
        function () {
            verify(current().isWatchPage === true, "Shuffle opens the watch page")
            var calls = mocks.playback.calls
            // The mock's shuffle reverses the list.
            verify(calls.indexOf("open:c") >= 0, "the first of the shuffled list plays: " + calls.join(" "))
            verify(calls.indexOf("setQueue:c,b,a@0") >= 0, "the shuffled list is the queue: " + calls.join(" "))
            stack().pop(undefined, PageStackAction.Immediate)
            mocks.playback.stop()
            return 300
        },
        function () {
            t.head = find(current(), "objectName", "playlistHeader")
            part("playAllButton").clicked(null)
            return 600
        },
        function () {
            var calls = mocks.playback.calls
            verify(calls.indexOf("open:a") >= 0 && calls.indexOf("setQueue:a,b,c@0") >= 0, "Play all queues the list in order: " + calls.join(" "))
            stack().pop(undefined, PageStackAction.Immediate)
            mocks.playback.stop()
            stack().pop(undefined, PageStackAction.Immediate)
            mocks.playlistsModel.listRows = ({})
            return 300
        }
    ]
}
