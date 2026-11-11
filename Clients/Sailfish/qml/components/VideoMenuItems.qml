// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
import QtQuick 2.6
import Sailfish.Silica 1.0

// Library actions for any video row's context menu (LIB-02, LIB-04, DL-01).
// `video` uses the shared keys: videoId, title, channel, channelId, durationSeconds, thumbnail.
ContextMenu {
    id: menu
    property var video: ({})
    property bool showDownloads: true
    // DSC-14: rows that offer the details-only view (it leaves playback alone) set this; the entry
    // comes first. Long-press opens this menu everywhere, so the view is reached from here.
    property bool showDetails: false

    MenuItem {
        visible: menu.showDetails
        //: Context menu entry opening a video's details without playing it
        text: qsTr("Details")
        onClicked: pageStack.push(Qt.resolvedUrl("../pages/VideoDetailsPage.qml"),
                                  { videoId: menu.video.videoId, hints: menu.video })
    }

    MenuItem {
        //: Context menu entry adding a video to a playlist
        text: qsTr("Add to playlist")
        onClicked: pageStack.push(Qt.resolvedUrl("../pages/AddToPlaylistDialog.qml"), { video: menu.video })
    }
    MenuItem {
        text: playlistsModel.revision >= 0 && playlistsModel.inWatchLater(menu.video.videoId)
              //: Context menu entry removing a video from the Watch later list
              ? qsTr("Remove from Watch later")
              //: Context menu entry adding a video to the Watch later list
              : qsTr("Watch later")
        onClicked: {
            // The menu is gone when the remorse ends: the callback uses captured locals only.
            var video = menu.video, lists = playlistsModel
            if (!lists.inWatchLater(video.videoId)) { lists.toggleWatchLater(video); return }
            // Removals wait for the remorse timer (Silica convention for destructive actions).
            //: Remorse text while a video is being removed from the Watch later list
            Remorse.popupAction(pageStack.currentPage, qsTr("Removing from Watch later"),
                                function () { if (lists.inWatchLater(video.videoId)) lists.toggleWatchLater(video) })
        }
    }
    MenuItem {
        property var watch: watchIndex.state(menu.video.videoId)
        text: watch.watched
              //: Context menu entry clearing the watched mark of a video
              ? qsTr("Mark as not watched")
              //: Context menu entry marking a video as watched
              : qsTr("Mark as watched")
        visible: typeof historyModel !== "undefined"
        onClicked: watch.watched ? historyModel.unmarkWatched(menu.video.videoId) : historyModel.markWatched(menu.video)
    }
    MenuItem {
        // LIB-01: the same action as the watch page's Subscribe button.
        readonly property string channelId: menu.video.channelId || ""
        // Pages hosted without the library (tests, previews) simply have no subscribe entry.
        readonly property bool available: typeof subscriptionsModel !== "undefined"
        readonly property bool subscribed: available && subscriptionsModel.revision >= 0 && subscriptionsModel.isSubscribed(channelId)
        visible: available && channelId.length === 24 && channelId.indexOf("UC") === 0
        text: subscribed
              //: Context menu entry unsubscribing from the channel of a video
              ? qsTr("Unsubscribe from %1").arg(menu.video.channel || "")
              //: Context menu entry subscribing to the channel of a video
              : qsTr("Subscribe to %1").arg(menu.video.channel || "")
        onClicked: {
            var id = channelId, name = menu.video.channel || "", subs = subscriptionsModel
            if (!subscribed) { subs.toggle(id, name, ""); return }
            //: Remorse text while a subscription is being removed
            Remorse.popupAction(pageStack.currentPage, qsTr("Unsubscribing"),
                                function () { if (subs.isSubscribed(id)) subs.unsubscribe(id) })
        }
    }
    MenuItem {
        visible: menu.showDownloads && typeof downloads !== "undefined"
        //: Context menu entry downloading a video's audio track
        text: qsTr("Download audio")
        onClicked: downloads.enqueue(menu.video, 0, 0)
    }
    MenuItem {
        visible: menu.showDownloads && typeof downloads !== "undefined"
        //: Context menu entry downloading a video
        text: qsTr("Download video")
        onClicked: pageStack.push(Qt.resolvedUrl("../pages/DownloadQualityDialog.qml"), { video: menu.video })
    }
}
