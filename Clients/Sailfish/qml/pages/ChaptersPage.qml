// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
import QtQuick 2.6
import Sailfish.Silica 1.0

// Chapter list with jump-to (DSC-15). Tapping an entry asks the model to jump (the watch page
// seeks the player) and returns to the video; the chapter playing now is highlighted.
Page {
    id: page
    property var chapterModel
    // False on the details-only view, which has no player to seek.
    property bool seekable: true

    SilicaListView {
        id: list
        anchors.fill: parent
        model: page.chapterModel
        header: PageHeader {
            //: Title of the chapter list page
            title: qsTr("Chapters")
        }
        delegate: BackgroundItem {
            id: item
            width: list.width
            readonly property bool current: page.chapterModel && index === page.chapterModel.currentIndex
            highlighted: down || current
            enabled: page.seekable
            onClicked: {
                page.chapterModel.jumpTo(index)
                pageStack.pop()
            }
            Label {
                id: time
                // Anchored, not x:, so right-to-left layouts mirror it (I18N-03).
                anchors.left: parent.left
                anchors.leftMargin: Theme.horizontalPageMargin
                anchors.verticalCenter: parent.verticalCenter
                text: model.startText
                font.pixelSize: Theme.fontSizeSmall
                color: item.highlighted ? Theme.highlightColor : Theme.secondaryHighlightColor
            }
            Label {
                anchors {
                    left: time.right; leftMargin: Theme.paddingMedium
                    right: parent.right; rightMargin: Theme.horizontalPageMargin
                    verticalCenter: parent.verticalCenter
                }
                text: model.title
                truncationMode: TruncationMode.Fade
                font.pixelSize: Theme.fontSizeSmall
                color: item.highlighted ? Theme.highlightColor : Theme.primaryColor
            }
        }
        Component.onCompleted: if (page.chapterModel && page.chapterModel.currentIndex > 0)
                                   positionViewAtIndex(page.chapterModel.currentIndex, ListView.Center)

        ViewPlaceholder {
            enabled: !page.chapterModel || page.chapterModel.count === 0
            //: Shown when a video has no chapters
            text: qsTr("No chapters")
        }
        VerticalScrollDecorator { }
    }
}
