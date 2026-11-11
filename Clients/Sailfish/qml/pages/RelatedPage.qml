// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
import QtQuick 2.6
import Sailfish.Silica 1.0
import "../components"
import "../logic/Rows.js" as Rows

// All related videos of a watch page (DSC-13), paged through the watch-next continuation.
// Tap plays the video on the watch page; long-press opens the shared row menu, whose first entry
// is the details-only view (DSC-14).
Page {
    id: page
    property var info   // WatchInfo
    readonly property var related: info ? info.related : null
    // Rows, the loading skeleton or the placeholder; never blank (NFR-PERF-01).
    readonly property string listState: related ? Rows.emptyState(related.count, related.busy, false) : "placeholder"

    // Through the shell, so the watch page is reused and the tap is timed (docs/DESIGN.md §2.6).
    function play(row) {
        window.openVideo(row.videoId, row)
    }

    SilicaListView {
        id: list
        VisibleRows { view: list }   // E12: rows on screen for tapIntent
        anchors.fill: parent
        model: page.related

        header: Column {
            width: list.width
            PageHeader {
                //: Title of the related videos page
                title: qsTr("Related videos")
                description: page.info ? page.info.title : ""
            }
            // The loading skeleton under the header, like every list (docs/DESIGN.md §5.1).
            SkeletonList {
                cards: true
                shown: page.listState === "skeleton"
                viewHeight: list.height
            }
        }

        delegate: VideoListItem {
            // Watched and download badges and the shared menu (LIB-06, DL-04, docs/DESIGN.md §4.5).
            // Cards in portrait, like the related videos under the player this list continues (§4.1).
            cardLayout: true
            avatarUrl: model.channelAvatar !== undefined ? (model.channelAvatar || "") : ""
            largeThumbnailUrl: model.thumbnailLarge !== undefined ? (model.thumbnailLarge || "") : ""
            videoId: model.videoId
            menu: Component { VideoMenuItems { video: page.related.get(index); showDetails: true } }
            videoTitle: model.title
            channelName: model.channel
            thumbnailUrl: model.thumbnail
            durationText: model.duration
            views: model.viewCount
            isLive: model.live
            onClicked: page.play(page.related.get(index))
        }

        footer: Item {
            width: list.width
            height: page.related && (page.related.busy || page.related.errorText !== "") ? Theme.itemSizeLarge : Theme.paddingLarge
            BusyIndicator {
                anchors.centerIn: parent
                running: page.related && page.related.busy && page.related.count > 0
                size: BusyIndicatorSize.Medium
            }
            // The next page failed: the error with a tap to retry, as in every list (§5.3); never
            // retried on scroll.
            RetryRow {
                anchors.verticalCenter: parent.verticalCenter
                visible: !!page.related && !page.related.busy && page.related.errorText !== "" && page.related.count > 0
                errorText: page.related ? page.related.errorText : ""
                onClicked: page.related.loadMore()
            }
        }

        onAtYEndChanged: if (atYEnd && page.related && page.related.canLoadMore && page.related.errorText === "")
                             page.related.loadMore()

        ViewPlaceholder {
            enabled: page.listState === "placeholder"
            text: page.related && page.related.errorText !== "" ? page.related.errorText
                  //: Shown when a video has no related videos
                  : qsTr("No related videos")
        }
        VerticalScrollDecorator { }
    }
}
