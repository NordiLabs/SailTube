// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// LIB-06: two bare overlays and one list row with overlay and download badge shown.
import QtQuick 2.6
import Sailfish.Silica 1.0
import "../../Clients/Sailfish/qml/components"

Item {
    id: root
    property int clicks: 0

    WatchOverlay { objectName: "overlayW1"; videoId: "w1"; width: 320; height: 180 }
    WatchOverlay { objectName: "overlayW2"; videoId: "w2"; y: 200; width: 320; height: 180 }

    ListView {
        objectName: "rows"
        y: 400
        width: parent.width
        height: 400
        model: ListModel { ListElement { vid: "w1" } }
        delegate: VideoListItem {
            width: ListView.view.width
            videoId: model.vid
            videoTitle: "Row " + model.vid
            durationText: "1:01"
            onClicked: root.clicks++
        }
    }
}
