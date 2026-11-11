// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
import QtQuick 2.6
import Sailfish.Silica 1.0

Page {
    id: page
    allowedOrientations: Orientation.All

    SilicaListView {
        anchors.fill: parent
        header: PageHeader {
            //: Title of the page choosing the video quality
            title: qsTr("Quality")
            description: playback.streamLabel
        }
        model: [{ height: 0, highFrameRate: false, label: "", codec: "" }].concat(playback.qualities)
        delegate: ListItem {
            readonly property bool current: playback.requestedHeight === modelData.height
                                            && playback.requestedHighFrameRate === modelData.highFrameRate
            contentHeight: Math.max(Theme.itemSizeSmall, qualityLabel.implicitHeight + 2 * Theme.paddingMedium)
            onClicked: {
                playback.setQuality(modelData.height, modelData.highFrameRate)
                pageStack.pop()
            }
            Label {
                id: qualityLabel
                x: Theme.horizontalPageMargin
                anchors.verticalCenter: parent.verticalCenter
                width: parent.width - 2 * x
                text: modelData.height === 0
                      //: Automatic quality choice in the quality list
                      ? qsTr("Automatic")
                      : modelData.label + "  ·  " + modelData.codec
                color: current || highlighted ? Theme.highlightColor : Theme.primaryColor
            }
        }
        VerticalScrollDecorator { }
    }
}
