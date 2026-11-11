// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
import QtQuick 2.6
import Sailfish.Silica 1.0

// MPR-02: thumbnail, title, channel and progress; actions from the session's
// cover state (MediaControlsPolicy::coverState):
//   session with a queue: play/pause + next; without one: play/pause + stop;
//   idle: "continue watching" when a resumable video exists, else the app identity.
// Cover video (user decision, FULL only; DESIGN.md §7.1): the picture area at the top shows live
// frames while a video plays and the cover is active, the last frame while paused, the
// thumbnail otherwise. Title and channel sit below on the cover background; only the progress
// line is drawn over the picture.
CoverBackground {
    id: cover
    // 0 identity, 1 continue watching, 2 session (PlaybackSession.coverMode).
    readonly property int mode: playback.coverMode
    readonly property bool session: mode === 2
    readonly property bool small: cover.size === Cover.Small

    // The playback component decides (playback.coverVideoActive: a playing or paused video plan with
    // a first frame, the setting on, the app in the background); until that API is in the
    // build the same conditions are read here.
    readonly property bool sessionDecides: typeof playback.coverVideoActive === "boolean"
    readonly property bool videoWanted: sessionDecides ? playback.coverVideoActive
        : session && playback.hasVideo && !playback.audioOnly
          && playback.state !== 4    // PlaybackSession.Failed
          && appSettings.variant !== "STORE"
          && (appSettings.revision, appSettings.value("playback/coverVideo", true) === true)
    readonly property bool hasFrame: videoLoader.status === Loader.Ready && videoLoader.item.hasFrame === true
    // The cover is on the home screen (a property so tests can drive it).
    property bool shownOnHome: status === Cover.Active
    // Frames are drawn while playing on a visible cover, before the first one too (the item
    // produces hasFrame only once it draws).
    readonly property bool drawing: videoWanted && playback.playing && shownOnHome
    // The picture replaces the thumbnail once it has a frame (frozen while paused).
    readonly property bool live: videoWanted && hasFrame
    // A vertical video (a Short) gets up to 55 % of the cover, fitted on black. The session's
    // aspect, when the playback component offers it, is known before the first frame.
    readonly property real aspect: typeof playback.videoAspect === "number" && playback.videoAspect > 0 ? playback.videoAspect
        : hasFrame && videoLoader.item.videoSize.height > 0 ? videoLoader.item.videoSize.width / videoLoader.item.videoSize.height
        : 0
    readonly property bool vertical: videoWanted && aspect > 0 && aspect < 1
    // Decoding for the cover runs only while it is visible (playback component).
    function tellPlayback() { if (typeof playback.setCoverVisible === "function") playback.setCoverVisible(status === Cover.Active) }
    onStatusChanged: tellPlayback()
    Component.onCompleted: tellPlayback()

    Item {
        id: picture
        objectName: "coverPicture"
        anchors { left: parent.left; right: parent.right; top: parent.top }
        height: cover.vertical ? Math.round(cover.height * 0.55) : Math.round(width * 9 / 16)
        // A late change of shape (aspect known only at the first frame) moves with the fade.
        Behavior on height { enabled: cover.shownOnHome; NumberAnimation { duration: 200 } }
        visible: playback.videoId !== "" && cover.mode !== 0
        clip: true

        Image {
            id: art
            anchors.fill: parent
            source: playback.thumbnail
            sourceSize.width: width     // decode at display size (DSC-19)
            asynchronous: true
            fillMode: Image.PreserveAspectCrop
            opacity: 0.85
        }
        // The live picture over the thumbnail; one 200 ms fade when the first frame arrives.
        Rectangle {
            anchors.fill: parent
            color: "black" // theme-exempt: behind the fitted video, same in every ambience
            opacity: videoLoader.opacity
            visible: cover.live
        }
        Loader {
            id: videoLoader
            anchors.fill: parent
            active: cover.videoWanted
            source: "CoverVideo.qml"
            opacity: cover.live ? 1 : 0
            // No animation runs while the cover is not visible (DESIGN.md §7.1).
            Behavior on opacity { enabled: cover.shownOnHome; FadeAnimator { duration: 200 } }
            onLoaded: item.active = Qt.binding(function () { return cover.drawing })
        }
        // Progress along the picture's bottom edge (hidden for live streams).
        Rectangle {
            anchors { left: parent.left; bottom: parent.bottom }
            visible: cover.session && playback.duration > 0 && !playback.live
            height: Math.max(2, Math.round(Theme.paddingSmall / 2))
            width: parent.width * Math.min(1, playback.position / Math.max(1, playback.duration))
            color: Theme.highlightColor
        }
    }
    Column {
        anchors {
            top: picture.visible ? picture.bottom : parent.top
            topMargin: cover.small ? Theme.paddingSmall : Theme.paddingLarge
            left: parent.left; right: parent.right
            margins: Theme.paddingMedium
        }
        spacing: Theme.paddingSmall
        Label {
            visible: cover.mode === 1 && !cover.small
            width: parent.width
            //: Cover label above the last video, which can be resumed from the cover
            text: qsTr("Continue watching")
            font.pixelSize: Theme.fontSizeExtraSmall
            color: Theme.highlightColor
        }
        Label {
            width: parent.width
            text: cover.mode !== 0 && playback.videoId !== "" ? playback.title : "SailTube" // i18n-exempt: app name
            wrapMode: Text.Wrap
            maximumLineCount: cover.small ? 1 : (cover.mode !== 0 ? 2 : 3)
            elide: Text.ElideRight
            font.pixelSize: Theme.fontSizeSmall
        }
        Label {
            width: parent.width
            visible: cover.mode !== 0 && !cover.small
            text: playback.channel
            truncationMode: TruncationMode.Fade
            font.pixelSize: Theme.fontSizeExtraSmall
            color: Theme.secondaryColor
        }
    }

    // Session with a queue: play/pause + next.
    CoverActionList {
        objectName: "queueActions"
        enabled: cover.session && playback.coverRightAction === 2
        CoverAction {
            iconSource: playback.playing ? "image://theme/icon-cover-pause" : "image://theme/icon-cover-play"
            onTriggered: playback.togglePlayback()
        }
        CoverAction {
            iconSource: "image://theme/icon-cover-next-song"
            onTriggered: playback.next()
        }
    }
    // Session without a queue: play/pause + stop (MPR-01 Stop really stops).
    CoverActionList {
        objectName: "sessionActions"
        enabled: cover.session && playback.coverRightAction !== 2
        CoverAction {
            iconSource: playback.playing ? "image://theme/icon-cover-pause" : "image://theme/icon-cover-play"
            onTriggered: playback.togglePlayback()
        }
        CoverAction {
            iconSource: "image://theme/icon-cover-cancel"
            onTriggered: playback.stop()
        }
    }
    // Idle with a resumable video: continue watching.
    CoverActionList {
        objectName: "continueActions"
        enabled: cover.mode === 1
        CoverAction {
            iconSource: "image://theme/icon-cover-play"
            onTriggered: playback.continueWatching()
        }
    }
}
