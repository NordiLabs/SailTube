// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
import QtQuick 2.6
import Sailfish.Silica 1.0
import "../components"
import "../components/Format.js" as Format
import "../logic/ShareLinks.js" as Links

// One list: play all, shuffle, download all (DL-08); remove with remorse; reorder.
Page {
    id: page
    property string playlistId
    // A model owned by this page (JavaScript ownership), see PlaylistsModel::openList.
    property var items: playlistsModel.openList(playlistId)
    // A saved YouTube playlist can be shared by its public address; a local list cannot.
    readonly property string shareLink: items && items.kind === 2 ? Links.playlist(items.remoteId) : ""
    // The cover: the first video's picture.
    readonly property string coverUrl: items && items.count > 0 ? (items.get(0).thumbnail || "") : ""

    WatchActions { id: actions }

    function playList(list) {
        if (list.length === 0) return
        // The playback component's queue takes the whole list when it exists.
        if (typeof window.playQueue === "function") { window.playQueue(list); return }
        window.openVideo(list[0].videoId, list[0])
    }

    SilicaListView {
        id: list
        anchors.fill: parent
        model: items

        PullDownMenu {
            // Play all and Shuffle are in the head; with nothing left to offer there is no menu.
            visible: refreshItem.visible || downloadAllItem.visible
            MenuItem {
                id: refreshItem
                visible: items.kind === 2 && typeof remotePlaylists !== "undefined"
                //: Menu entry fetching a saved YouTube playlist again
                text: qsTr("Refresh from YouTube")
                onClicked: remotePlaylists.refresh(items.playlistId)
            }
            MenuItem {
                id: downloadAllItem
                visible: items.count > 0 && typeof downloads !== "undefined"
                //: Menu entry downloading every video of a playlist that is not downloaded yet
                text: qsTr("Download all")
                onClicked: downloads.enqueueAll(items.items(false), -1, 0)   // the "Download quality" setting: kind and height
            }
        }

        // The head (docs/DESIGN.md §4.8): title, then the cover, Play all and the chips.
        header: Item {
            width: list.width
            height: pageHeader.height + head.height
            PageHeader {
                id: pageHeader
                //: Name of the built-in watch later playlist
                title: items.kind === 1 ? qsTr("Watch later") : items.title
                //: Number of videos in a playlist
                description: qsTr("%n video(s)", "", items.count)
            }
            PlaylistHeader {
                id: head
                anchors.top: pageHeader.bottom
                coverUrl: page.coverUrl
                count: items.count
                landscape: page.isLandscape
                message: actions.message
                onPlayRequested: page.playList(items.items(false))
                onShuffleRequested: page.playList(items.items(true))
                ActionChip {
                    objectName: "shareChip"
                    visible: page.shareLink !== ""
                    iconSource: "image://theme/icon-m-share"
                    //: Button sharing the video link through the system share sheet
                    text: qsTranslate("VideoPage", "Share")
                    onClicked: actions.share(page.shareLink, items.title)
                }
                ActionChip {
                    visible: page.shareLink !== ""
                    iconSource: "image://theme/icon-m-clipboard"
                    //: Button copying the video link to the clipboard
                    text: qsTranslate("VideoPage", "Copy link")
                    onClicked: actions.copy(page.shareLink)
                }
            }
        }

        delegate: VideoListItem {
            id: row
            videoId: model.videoId
            videoTitle: model.title
            channelName: model.channel
            thumbnailUrl: model.thumbnail
            durationText: model.durationSeconds > 0 ? Format.time(model.durationSeconds * 1000) : ""
            // The shared row menu, then this list's own entries; the destructive one last
            // (docs/DESIGN.md §4.5). In Watch later the shared "Remove from Watch later" removes.
            menu: Component {
                VideoMenuItems {
                    video: items.get(index)
                    MenuItem {
                        visible: items.kind !== 2 && index > 0
                        //: Playlist item menu: move the video one place up
                        text: qsTr("Move up")
                        onClicked: items.move(index, index - 1)
                    }
                    MenuItem {
                        visible: items.kind !== 2 && index < items.count - 1
                        //: Context menu entry moving a video one place down in a playlist
                        text: qsTr("Move down")
                        onClicked: items.move(index, index + 1)
                    }
                    MenuItem {
                        visible: items.kind !== 2 && items.kind !== 1
                        //: Context menu entry removing a video from a playlist
                        text: qsTr("Remove from playlist")
                        onClicked: {
                            // The menu is gone when the remorse ends: capture what the callback needs.
                            var list = items, id = model.videoId
                            //: Remorse text while a video is being removed from a playlist
                            row.remorseAction(qsTr("Removing"), function() { list.remove(id) })
                        }
                    }
                }
            }
            onClicked: {
                window.openVideo(model.videoId, items.get(index))
            }
        }

        ViewPlaceholder {
            enabled: items.count === 0
            text: items.kind === 2
                  //: Empty saved YouTube playlist that has not been fetched yet
                  ? qsTr("Not fetched yet")
                  //: Empty playlist
                  : qsTr("No videos")
            //: Hint on an empty local playlist
            hintText: items.kind === 2 ? "" : qsTr("Long press a video anywhere to add it.")
        }
        VerticalScrollDecorator { }
    }

    Component.onCompleted: {
        // Imported mirrors start empty and are fetched once on first open (LIB-03).
        if (items.kind === 2 && items.refreshed === 0 && typeof remotePlaylists !== "undefined")
            remotePlaylists.refresh(items.playlistId)
    }
}
