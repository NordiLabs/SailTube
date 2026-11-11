// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Reports the video rows on screen of a list to `tapIntent` (E12) once the list is still: up to
// three rows of which at least half shows (logic/Rows.js rowOnScreen, so tall cards count too).
// Usage inside a SilicaListView whose model has get(row).videoId:
//   VisibleRows { view: list }
import QtQuick 2.6
import "../logic/Rows.js" as Rows

Item {
    id: root
    property var view: null
    visible: false

    function report() {
        if (!view || typeof tapIntent === "undefined" || !view.model || typeof view.model.get !== "function") return
        var ids = []
        var top = view.contentY
        var bottom = view.contentY + view.height
        var y = top + 1
        var guard = 0
        while (y < bottom && ids.length < 3 && guard < 40) {
            var i = view.indexAt(view.width / 2, y)
            guard++
            if (i < 0) { y += 40; continue }
            var item = view.itemAt(view.width / 2, y)
            if (!item) { y += 40; continue }
            var itemTop = item.y
            var itemBottom = item.y + item.height
            if (Rows.rowOnScreen(itemTop, itemBottom, top, bottom)) {
                var row = view.model.get(i)
                if (row && row.videoId && ids.indexOf(row.videoId) < 0) ids.push(row.videoId)
            }
            y = itemBottom + 1
        }
        tapIntent.rowsVisible(ids)
    }

    Timer { id: settle; interval: 150; onTriggered: root.report() }
    Connections {
        target: root.view
        onMovingChanged: if (!root.view.moving) settle.restart()
        onCountChanged: settle.restart()
    }
    Component.onCompleted: settle.restart()
}
