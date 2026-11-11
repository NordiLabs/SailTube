// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
import QtQuick 2.6
import Sailfish.Silica 1.0

// The player bound to the session's overlay API (docs/DESIGN.md §3.2.1, §3.2.3): the seek bar's
// buffered range and SponsorBlock segments; the settings sheet's Audio track, Loop video, Sleep
// timer and Stable volume rows; captions from the session's tracks and text, with the fine
// position cadence asked for the controls only.
UiTest {
    id: t
    property var pb: mocks.playback
    property var page
    function overlayItem() { return find(page, "objectName", "playerOverlay") }
    function sheet() { return find(page, "objectName", "playerSettings") }
    function rowWithText(root, text) {
        var l = findAll(root, function (i) { return i.visible && i.text === text })
        if (l.length === 0) return null
        var row = l[0]
        while (row && typeof row.clicked !== "function") row = row.parent
        return row
    }
    steps: [
        function () { window.openVideo("api1", { title: "API" }); return 500 },
        function () { page = current(); pb.finishLoading(true); return 200 },
        function () {
            // Seek bar: the buffered range and the skipper's segments come from the session.
            var bar = overlayItem().slider
            compare(bar.buffered, 0, "nothing buffered is known yet")
            pb.bufferedPosition = 60000
            compare(bar.buffered, 60000, "the buffered range follows the session")
            compare(bar.segments.length, 0)
            pb.sponsorSegments = [{ startMs: 0, endMs: 30000, category: "sponsor" }]
            compare(bar.segments.length, 1, "the session's segments are drawn")

            // The sheet: a plain stream offers speed, loop, the sleep timer and audio only.
            var s = sheet()
            compare(s.rows.join(","), "speed,loop,sleep,audioOnly")
            // One audio track: no picker; Stable volume only when the session says a levelled
            // track exists, not merely because there is an audio stream.
            pb.audioTracks = [{ id: "en", label: "English (original)", language: "en", original: true, descriptive: false, selected: true }]
            compare(s.rows.join(","), "speed,loop,sleep,audioOnly", "an audio stream alone does not offer Stable volume")
            pb.stableVolumeAvailable = true
            compare(s.rows.join(","), "speed,loop,sleep,stableVolume,audioOnly", "the session's flag brings the row")
            // Two audio tracks: the picker joins.
            pb.audioTracks = [{ id: "en", label: "English (original)", language: "en", original: true, descriptive: false, selected: true },
                              { id: "es", label: "Español", language: "es", original: false, descriptive: false, selected: false }]
            compare(s.rows.join(","), "speed,audioTrack,loop,sleep,stableVolume,audioOnly")
            compare(s.rowValue("audioTrack"), "English (original)", "the row names the playing track")
            // A live stream has no end to loop at.
            pb.live = true
            verify(s.rows.indexOf("loop") < 0, "no Loop on a live stream")
            pb.live = false

            // Loop and Stable volume are switches on the session.
            pb.calls = []
            s.toggle("loop")
            verify(pb.loop && s.rowSwitchOn("loop"), "Loop video on")
            s.toggle("stableVolume")
            verify(pb.stableVolume && s.rowSwitchOn("stableVolume"), "Stable volume on")
            compare(pb.calls.join(","), "loop:true,stableVolume:true")
            s.toggle("loop"); s.toggle("stableVolume")
            verify(!pb.loop && !pb.stableVolume, "and off again")

            s.openSheet()
            s.level = "audioTrack"
            return 300
        },
        function () {
            var s = sheet()
            var row = rowWithText(s, "Español")
            verify(row !== null, "the audio tracks are listed by their labels")
            pb.calls = []
            row.clicked(null)
            compare(pb.calls.join(","), "audioTrack:es", "a tap plays that track")
            compare(s.rowValue("audioTrack"), "Español", "the row follows")
            return 400
        },
        function () {
            var s = sheet()
            verify(!s.open, "choosing closes the sheet")
            compare(s.rowValue("sleep"), "Off", "no timer")
            s.openSheet()
            s.level = "sleep"
            return 300
        },
        function () {
            var s = sheet()
            var row = rowWithText(s, "30 minute(s)")
            verify(row !== null && rowWithText(s, "End of video") !== null && rowWithText(s, "Off") !== null, "sleep timer choices")
            pb.calls = []
            row.clicked(null)
            compare(pb.calls.join(","), "sleep:30")
            compare(s.rowValue("sleep"), "30:00 left", "the row counts down")
            pb.setSleepTimer(-1)
            compare(s.rowValue("sleep"), "End of video", "at the end of the video, not a time")
            pb.setSleepTimer(0)
            compare(s.rowValue("sleep"), "Off")
            return 400
        },
        function () {
            // Captions: nothing without tracks; with tracks the CC button and the sheet's row.
            var o = overlayItem(), s = sheet()
            verify(!o.captionsAvailable && s.rows.indexOf("captions") < 0, "no CC without tracks")
            pb.captionTracks = [{ id: "en", label: "English", language: "en", autoGenerated: false, translated: false },
                                { id: "es", label: "Spanish", language: "es", autoGenerated: false, translated: false }]
            verify(o.captionsAvailable && s.rows.indexOf("captions") >= 0, "CC and the Captions row with tracks")
            verify(!page.captionsOn && !o.captionsOn, "off until chosen")
            pb.calls = []
            o.captionsRequested()
            compare(pb.calls.join(","), "caption:en", "CC turns a track on through the session")
            verify(page.captionsOn && o.captionsOn, "captions on")
            compare(s.rowValue("captions"), "English", "the row names the track")
            pb.captionText = "Hello from the session"
            verify(page.captionsShown, "the session's text is drawn")
            compare(findAll(page, function (i) { return i.visible && i.text === "Hello from the session" }).length, 1, "over the picture")
            // The fine position cadence is asked for the controls only, not for captions.
            page.overlayController.controlsVisible = false
            verify(!pb._controlsVisible, "captions alone do not ask for fine position updates")
            page.overlayController.controlsVisible = true
            verify(pb._controlsVisible, "the controls do")
            // Off remembers the track; on brings it back.
            pb.setCaptionTrack("es")
            pb.calls = []
            o.captionsRequested()
            verify(!page.captionsOn, "CC turns captions off")
            o.captionsRequested()
            compare(pb.calls.join(","), "caption:,caption:es", "and brings the same track back")
            pb.captionText = ""
            verify(!page.captionsShown, "no box between cues")
            // The chosen track's text cannot be loaded: the choice stays and the box says so.
            var box = find(page, "objectName", "captionBox")
            verify(!box.visible, "nothing to draw, no box")
            page.captionTrackFailed = true
            verify(page.captionNotice && box.visible, "the failure is said in the caption box")
            compare(findAll(box, function (i) { return i.visible && i.text === "This track could not be loaded" }).length, 1, "with the captions page's words")
            verify(page.captionsOn, "the choice stays")
            o.captionsRequested()
            verify(!page.captionsOn && !page.captionNotice && !box.visible, "turning captions off takes the notice away")
            page.captionTrackFailed = false
            o.captionsRequested()
            verify(page.captionsOn && !page.captionNotice, "on again with a track that loads: no notice")
            return 100
        },
        function () {
            pb.setCaptionTrack(""); pb.captionTracks = []; pb.audioTracks = []; pb.stableVolumeAvailable = false
            pb.sponsorSegments = []; pb.bufferedPosition = 0
            stack().pop(undefined, PageStackAction.Immediate)
            pb.stop()
        }
    ]
}
