// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
import QtQuick 2.6
import Sailfish.Silica 1.0

// MPR-02 (playback component's cover): exactly one action list is live per cover state: idle (none),
// continue watching, session without a queue (play/pause + stop), session with a queue
// (play/pause + next). Cover video: the live layout once the first frame is there, the
// thumbnail layout before it, for listen-only, on errors and with the setting off.
UiTest {
    id: t
    property var cover
    property var pb: mocks.playback
    property var settings: mocks.appSettings
    function live() {
        var out = []
        for (var i = 0; i < cover.data.length; ++i) {
            var d = cover.data[i]
            if (String(d).indexOf("CoverActionList") >= 0 && d.enabled) out.push(d.objectName)
        }
        return out.join(",")
    }
    steps: [
        function () {
            var c = Qt.createComponent(Qt.resolvedUrl("../../Clients/Sailfish/qml/cover/CoverPage.qml"))
            cover = c.createObject(window, { width: 270, height: 432 })
            verify(cover !== null, "cover created: " + c.errorString())
            pb.coverMode = 0
            compare(live(), "", "idle: no actions")
            pb.coverMode = 1
            compare(live(), "continueActions", "continue watching")
            pb.coverMode = 2; pb.coverRightAction = 0
            compare(live(), "sessionActions", "session without a queue")
            pb.coverRightAction = 2
            compare(live(), "queueActions", "session with a queue")
            pb.coverRightAction = 0
            // Cover video: a video session, no frame yet.
            pb.videoId = "cv1"; pb.title = "Cover video"; pb.hasVideo = true
            return 100
        },
        function () {
            verify(cover.videoWanted, "video wanted for a video session")
            verify(!cover.live, "before the first frame: thumbnail layout")
            var surface = find(cover, "videoAspect", 0)
            verify(surface !== null, "live picture loaded")
            // Like CoverVideoItem, the picture gets a frame only once it is active: drawing
            // must not wait for a frame (review of 0f6bd28).
            cover.shownOnHome = true
            pb.playing = true
            verify(surface.active, "drawing starts before the first frame on a visible, playing cover")
            if (surface.active) surface.hasFrame = true
            return 50
        },
        function () {
            verify(cover.live, "first frame: live picture")
            var pic = find(cover, "objectName", "coverPicture")
            compare(pic.height, Math.round(pic.width * 9 / 16), "landscape: a 16:9 picture area")
            var title = find(cover, "text", "Cover video")
            verify(title !== null && title.mapToItem(cover, 0, 0).y >= pic.height - 1, "title below the picture, not over it")
            compare(live(), "sessionActions", "actions unchanged while live")
            var surface = find(cover, "videoAspect", 0)
            surface.videoSize = Qt.size(1080, 1920)
            return 400    // the area grows with the 200 ms animation
        },
        function () {
            var pic = find(cover, "objectName", "coverPicture")
            verify(cover.vertical, "vertical video detected")
            pb.playing = false
            verify(!find(cover, "videoAspect", 0).active, "paused: no drawing, last frame kept")
            verify(cover.live, "paused: the frozen picture stays")
            pb.playing = true
            compare(pic.height, Math.round(cover.height * 0.55), "vertical: 55 % of the cover")
            find(cover, "videoAspect", 0).videoSize = Qt.size(1920, 1080)
            pb.audioOnly = true
            return 50
        },
        function () {
            verify(!cover.live && !cover.videoWanted, "listen-only: thumbnail layout")
            pb.audioOnly = false
            settings.setValue("playback/coverVideo", false)
            return 50
        },
        function () {
            verify(!cover.videoWanted, "setting off: thumbnail layout")
            settings.setValue("playback/coverVideo", true)
            pb.state = 4
            return 50
        },
        function () {
            verify(!cover.videoWanted, "error: thumbnail layout")
            pb.state = 0
            // With the playback component's decision in the build, the cover follows it.
            verify(!cover.sessionDecides, "mock has no coverVideoActive yet")
            return 10
        },
        function () {
            pb.state = 0; pb.hasVideo = false; pb.videoId = ""; pb.playing = false
            pb.coverMode = Qt.binding(function () { return pb.videoId !== "" ? 2 : 0 })
            cover.destroy()
        }
    ]
}
