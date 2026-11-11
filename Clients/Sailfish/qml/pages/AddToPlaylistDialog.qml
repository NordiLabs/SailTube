// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
import QtQuick 2.6
import Sailfish.Silica 1.0

// LIB-02 picker: Watch later and local lists, plus "New list and add". A list
// that already holds the video is shown checked and is not added again.
Page {
    id: page
    property var video: ({})

    SilicaListView {
        id: list
        anchors.fill: parent
        model: playlistsModel
        header: Column {
            width: list.width
            PageHeader {
                //: Title of the page choosing a playlist for a video
                title: qsTr("Add to playlist")
                description: page.video.title || ""
            }
            BackgroundItem {
                width: parent.width
                Label {
                    // Anchored, not x:, so right-to-left layouts mirror it (I18N-03).
                    anchors { left: parent.left; leftMargin: Theme.horizontalPageMargin; verticalCenter: parent.verticalCenter }
                    //: Creates a playlist and adds the video to it
                    text: qsTr("New playlist and add")
                    color: parent.highlighted ? Theme.highlightColor : Theme.primaryColor
                }
                onClicked: {
                    //: Heading of the dialog naming a new playlist
                    var d = pageStack.push(Qt.resolvedUrl("PlaylistNameDialog.qml"), { heading: qsTr("New playlist") })
                    d.accepted.connect(function() {
                        playlistsModel.createWith(d.name, page.video)
                        pageStack.pop(pageStack.previousPage(page))
                    })
                }
            }
        }
        delegate: BackgroundItem {
            id: row
            visible: !model.mirror
            height: visible ? Theme.itemSizeSmall : 0
            property bool present: playlistsModel.revision >= 0 && playlistsModel.contains(model.playlistId, page.video.videoId)
            Label {
                anchors {
                    left: parent.left; leftMargin: Theme.horizontalPageMargin
                    right: mark.left; rightMargin: Theme.paddingMedium
                    verticalCenter: parent.verticalCenter
                }
                //: Name of the built-in watch later playlist
                text: model.watchLater ? qsTr("Watch later") : model.title
                truncationMode: TruncationMode.Fade
                color: row.highlighted ? Theme.highlightColor : Theme.primaryColor
            }
            Image {
                id: mark
                anchors { right: parent.right; rightMargin: Theme.horizontalPageMargin; verticalCenter: parent.verticalCenter }
                visible: row.present
                source: "image://theme/icon-s-installed"
            }
            onClicked: {
                if (!row.present) playlistsModel.add(model.playlistId, page.video)
                pageStack.pop()
            }
        }
        VerticalScrollDecorator { }
    }
}
