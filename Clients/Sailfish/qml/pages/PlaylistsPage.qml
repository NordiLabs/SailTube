// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
import QtQuick 2.6
import Sailfish.Silica 1.0
import "../logic/Rows.js" as Rows

// LIB-02/03: Watch later first, then local lists (new on top) and saved YouTube playlists.
Page {
    id: page

    SilicaListView {
        id: list
        anchors.fill: parent
        model: playlistsModel

        PullDownMenu {
            MenuItem {
                //: Menu entry creating a new local playlist
                text: qsTr("New playlist")
                onClicked: {
                    var d = pageStack.push(Qt.resolvedUrl("PlaylistNameDialog.qml"),
                                           //: Header of the dialog creating a playlist
                                           { heading: qsTr("New playlist") })
                    d.accepted.connect(function() { playlistsModel.create(d.name) })
                }
            }
        }

        header: PageHeader {
            //: Title of the playlists page
            title: qsTr("Playlists")
        }

        delegate: ListItem {
            id: item
            // Grows with the system text size (docs/DESIGN.md §7.9).
            // The shared row geometry (docs/DESIGN.md §4.1): 42 % thumbnail, text column beside it.
            contentHeight: Math.max(thumb.height, rowTexts.height) + 2 * Theme.paddingMedium
            menu: model.watchLater ? null : contextMenu
            Component {
                id: contextMenu
                ContextMenu {
                    MenuItem {
                        visible: !model.mirror
                        //: Context menu entry renaming a playlist
                        text: qsTr("Rename")
                        onClicked: {
                            // This menu is destroyed once it closes, before the callback runs: keep the model.
                            var id = model.playlistId, lists = playlistsModel
                            var d = pageStack.push(Qt.resolvedUrl("PlaylistNameDialog.qml"),
                                                   //: Header of the dialog renaming a playlist
                                                   { heading: qsTr("Rename playlist"), name: model.title })
                            d.accepted.connect(function() { lists.rename(id, d.name) })
                        }
                    }
                    MenuItem {
                        visible: !model.mirror && index > 1
                        //: Context menu entry moving a playlist one place up
                        text: qsTr("Move up")
                        onClicked: playlistsModel.move(model.playlistId, index - 1)
                    }
                    MenuItem {
                        text: model.mirror
                              //: Context menu entry removing a saved YouTube playlist from the library
                              ? qsTr("Remove from library")
                              //: Context menu entry deleting a local playlist
                              : qsTr("Delete")
                        onClicked: {
                            // As above: the remorse callback outlives this menu.
                            var id = model.playlistId, lists = playlistsModel
                            //: Remorse text while a playlist is being deleted
                            item.remorseAction(qsTr("Deleting"), function() { lists.remove(id) })
                        }
                    }
                }
            }
            Rectangle {
                id: thumb
                // Anchored, not x:, so right-to-left layouts mirror it (I18N-03).
                anchors.left: parent.left
                anchors.leftMargin: Theme.horizontalPageMargin
                y: Theme.paddingMedium
                width: Rows.thumbWidth(item.width, Screen.width)
                height: Rows.thumbHeight(width)
                color: Theme.rgba(Theme.highlightBackgroundColor, 0.1)
                Image {
                    anchors.fill: parent
                    source: model.thumbnail
                    fillMode: Image.PreserveAspectCrop
                    clip: true
                    asynchronous: true
                    sourceSize.width: width
                }
            }
            Column {
                id: rowTexts
                anchors {
                    left: thumb.right; leftMargin: Theme.paddingMedium
                    right: parent.right; rightMargin: Theme.horizontalPageMargin
                    top: thumb.top
                }
                Label {
                    width: parent.width
                    //: Title of the built-in Watch later list
                    text: model.watchLater ? qsTr("Watch later") : model.title
                    wrapMode: Text.Wrap
                    maximumLineCount: 2
                    elide: Text.ElideRight
                    // The shared row's type sizes (docs/DESIGN.md §4.1): title small, meta extra small.
                    font.pixelSize: Theme.fontSizeSmall
                    color: item.highlighted ? Theme.highlightColor : Theme.primaryColor
                }
                Label {
                    width: parent.width
                    //: Number of videos in a playlist
                    text: qsTr("%n video(s)", "", model.itemCount)
                          + (model.mirror && model.channel !== "" ? " · " + model.channel : "")
                    truncationMode: TruncationMode.Fade
                    font.pixelSize: Theme.fontSizeExtraSmall
                    color: item.highlighted ? Theme.secondaryHighlightColor : Theme.secondaryColor
                }
            }
            onClicked: pageStack.push(Qt.resolvedUrl("PlaylistPage.qml"), { playlistId: model.playlistId })
        }
        VerticalScrollDecorator { }
    }
}
