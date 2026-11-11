// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// harness: qm=ar
import QtQuick 2.6
import Sailfish.Silica 1.0
import "../../Clients/Sailfish/qml/logic/Rows.js" as Rows

// Right-to-left layout (Arabic, I18N): with the compiled Arabic catalog loaded, Qt switches the
// application to RTL through QT_LAYOUT_DIRECTION, the window mirrors and the section bar runs
// right to left.
UiTest {
    steps: [
        function () {
            verify(window.rightToLeft, "application direction is RTL")
            verify(window.LayoutMirroring.enabled, "window mirrors")
            var buttons = findAll(window, function (i) { return i.hasOwnProperty("isCurrent") && i.hasOwnProperty("showLabel") })
            compare(buttons.length, 6, "six bottom bar entries")
            var home = buttons.filter(function (b) { return b.mapToItem(null, 0, 0).x > current().width / 2 && b.width < current().width / 2 })
            verify(home.length >= 1, "first sections sit on the right")
            var first = buttons[0].mapToItem(null, 0, 0).x, last = buttons[5].mapToItem(null, 0, 0).x
            verify(first > last, "Home (first) is right of Downloads (last): " + first + " vs " + last)
            // A video row: thumbnail on the right, text on its left.
            var c = Qt.createComponent(Qt.resolvedUrl("../../Clients/Sailfish/qml/components/VideoListItem.qml"))
            var row = c.createObject(current(), { width: current().width, videoTitle: "Title", channelName: "Channel" })
            verify(row !== null, "row created: " + c.errorString())
            var thumb = null, texts = null
            for (var i = 0; i < row.contentItem.children.length; ++i) {
                var k = row.contentItem.children[i]
                if (k.hasOwnProperty("color") && k.height > 0 && Math.abs(k.width - Rows.thumbWidth(row.width, Screen.width)) < 2) thumb = k
                if (k.hasOwnProperty("spacing")) texts = k
            }
            verify(thumb !== null && texts !== null, "thumbnail and text column found")
            verify(thumb.x > row.width / 2, "thumbnail on the right: x " + thumb.x)
            verify(texts.x + texts.width <= thumb.x + 1, "text left of the thumbnail: " + texts.x + "+" + texts.width + " vs " + thumb.x)
            row.destroy()
        }
    ]
}
