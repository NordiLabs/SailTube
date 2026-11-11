// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Remote playlist preview (LIB-03, LNK-04): nothing is saved until the user asks.
// Push with pageStack.push(Qt.resolvedUrl("PlaylistPreviewPage.qml"), {playlistId: "PL..."}).
import QtQuick 2.0
import Sailfish.Silica 1.0
import org.nordilabs.sailtube 1.0
import "../components"
import "../logic/ShareLinks.js" as Links
import "../logic/Rows.js" as Rows

Page {
    id: page
    property string playlistId
    allowedOrientations: Orientation.All

    PlaylistModel { id: playlist }
    WatchActions { id: actions }
    // Rows, the loading skeleton or the placeholder; never blank (NFR-PERF-01).
    readonly property string listState: Rows.emptyState(playlist.count, playlist.busy, false)
    // The address to share; empty for a list without a public id (no Share chip then).
    readonly property string shareLink: Links.playlist(playlist.playlistId)

    Component.onCompleted: playlist.open(page.playlistId)

    // Opens row `i` with the playlist as the queue hint ({playlistId, index}, LNK-04).
    function play(i) {
        var row = playlist.get(i)
        window.openVideo(row.videoId, row)
        // LNK-04: the loaded rows become the queue, from the tapped video on (more pages
        // join as they load: queueSeeder walks the rest).
        var ids = []
        for (var k = 0; k < playlist.count; ++k) ids.push(playlist.get(k).videoId)
        playback.setQueue(ids, i)
        if (playlist.canLoadMore && typeof queueSeeder !== "undefined") queueSeeder.seed(playlist.playlistId, row.videoId, i)
    }

    // Shuffle: the rows loaded so far, in random order, as the queue.
    function playShuffled() {
        var rows = []
        for (var k = 0; k < playlist.count; ++k) rows.push(playlist.get(k))
        window.playQueue(Links.shuffled(rows), 0)
    }

    SilicaListView {
        id: list
        VisibleRows { view: list }   // E12: rows on screen for tapIntent
        anchors.fill: parent
        model: playlist

        PullDownMenu {
            // Play all and Shuffle are in the head; with nothing left to offer there is no menu.
            visible: saveItem.visible || channelItem.visible
            // LIB-03: the preview never saves by itself; "Save to library" creates the mirror
            // (library component) and fetches all of its items (remotePlaylists, catalog component).
            MenuItem {
                id: saveItem
                readonly property bool available: typeof playlistsModel !== "undefined" && playlist.playlistId !== ""
                readonly property string mirror: available && playlistsModel.revision >= 0 ? playlistsModel.mirrorFor(playlist.playlistId) : ""
                visible: available && (mirror !== "" || playlist.title !== "")
                text: mirror !== ""
                      //: Menu entry opening the saved copy of a YouTube playlist in the library
                      ? qsTr("Open in library")
                      //: Menu entry saving a YouTube playlist to the library
                      : qsTr("Save to library")
                onClicked: {
                    if (mirror !== "") {
                        pageStack.push(Qt.resolvedUrl("PlaylistPage.qml"), { playlistId: mirror })
                        return
                    }
                    var id = playlistsModel.saveRemote(playlist.playlistId, playlist.title, playlist.owner,
                                                       playlist.thumbnail, playlist.videoCount >= 0 ? playlist.videoCount : -1)
                    if (id !== "" && typeof remotePlaylists !== "undefined") remotePlaylists.refresh(id)
                }
            }
            MenuItem {
                id: channelItem
                visible: playlist.ownerId !== ""
                //: Menu entry opening the channel that owns the playlist
                text: qsTr("Open channel")
                onClicked: pageStack.push(Qt.resolvedUrl("ChannelPage.qml"), {reference: playlist.ownerId})
            }
        }

        // The head (docs/DESIGN.md §4.8): title, then the cover, Play all and the chips.
        header: Item {
            width: list.width
            height: pageHeader.height + head.height + skeleton.height
            PageHeader {
                id: pageHeader
                //: Fallback title of a playlist preview while it loads
                title: playlist.title !== "" ? playlist.title : qsTr("Playlist")
                description: {
                    var parts = []
                    if (playlist.owner !== "") parts.push(playlist.owner)
                    //: Number of videos in a playlist
                    if (playlist.videoCount >= 0) parts.push(qsTr("%n videos", "", playlist.videoCount))
                    else if (playlist.videoCountText !== "") parts.push(playlist.videoCountText)
                    return parts.join(" · ")
                }
            }
            PlaylistHeader {
                id: head
                anchors.top: pageHeader.bottom
                coverUrl: playlist.thumbnail
                count: playlist.count
                landscape: page.isLandscape
                message: actions.message
                onPlayRequested: page.play(0)
                onShuffleRequested: page.playShuffled()
                ActionChip {
                    objectName: "shareChip"
                    visible: page.shareLink !== ""
                    iconSource: "image://theme/icon-m-share"
                    //: Button sharing the video link through the system share sheet
                    text: qsTranslate("VideoPage", "Share")
                    onClicked: actions.share(page.shareLink, playlist.title)
                }
                ActionChip {
                    visible: page.shareLink !== ""
                    iconSource: "image://theme/icon-m-clipboard"
                    //: Button copying the video link to the clipboard
                    text: qsTranslate("VideoPage", "Copy link")
                    onClicked: actions.copy(page.shareLink)
                }
            }
            // The loading skeleton under the head, like every list (docs/DESIGN.md §5.1).
            SkeletonList {
                id: skeleton
                anchors.top: head.bottom
                shown: page.listState === "skeleton"
                viewHeight: list.height
            }
        }

        delegate: VideoListItem {
            // Watched and download badges and the shared menu (LIB-06, DL-04, docs/DESIGN.md §4.5).
            videoId: model.videoId
            menu: Component { VideoMenuItems { video: playlist.get(index) } }
            videoTitle: model.title
            channelName: model.membersOnly
                         //: Prefix of the channel line of a members-only video, %1 is the channel
                         ? qsTr("Members only · %1").arg(model.channel)
                         : model.restricted
                           //: Prefix of the channel line of a restricted video, %1 is the channel
                           ? qsTr("Restricted · %1").arg(model.channel)
                           : model.channel
            thumbnailUrl: model.thumbnail
            durationText: model.duration
            views: model.viewCount
            isLive: model.live
            onClicked: page.play(index)
        }

        footer: Item {
            width: list.width
            height: playlist.busy || playlist.canLoadMore || playlist.errorText !== "" ? Theme.itemSizeLarge : Theme.paddingLarge
            BusyIndicator {
                anchors.centerIn: parent
                running: playlist.busy && playlist.count > 0
                size: BusyIndicatorSize.Medium
            }
            // The next page failed: the error with a tap to retry, as in every list (§5.3).
            RetryRow {
                anchors.verticalCenter: parent.verticalCenter
                visible: !playlist.busy && playlist.errorText !== "" && playlist.count > 0
                errorText: playlist.errorText
                onClicked: playlist.loadMore()
            }
        }

        onAtYEndChanged: if (atYEnd && playlist.canLoadMore && playlist.errorText === "") playlist.loadMore()

        ViewPlaceholder {
            enabled: page.listState === "placeholder"
            text: playlist.errorText !== "" ? playlist.errorText
                  //: Shown for a playlist without videos
                  : qsTr("This playlist is empty")
        }

        VerticalScrollDecorator { }
    }
}
