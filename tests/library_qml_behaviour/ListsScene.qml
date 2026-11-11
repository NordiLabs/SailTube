// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// DL-04: the same VideoListItem rows in three list contexts (feed, search, channel).
import QtQuick 2.6
import Sailfish.Silica 1.0
import "../../Clients/Sailfish/qml/components"

Column {
    Repeater {
        model: ["feed", "search", "channel"]
        ListView {
            objectName: modelData
            width: parent.width
            height: 310
            interactive: false
            model: ListModel { ListElement { vid: "dl1" } ListElement { vid: "dl2" } }
            delegate: VideoListItem {
                width: ListView.view.width
                videoId: model.vid
                videoTitle: "Row " + model.vid
            }
        }
    }
}
