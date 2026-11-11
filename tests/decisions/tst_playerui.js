// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Player overlay decisions (docs/DESIGN.md §3.2).
var P = modules.PlayerUi

test("centre disc: spinner while loading or buffering, replay at the end, nothing on failure", function () {
    eq(P.centreState({ playing: true }), "pause")
    eq(P.centreState({ playing: false }), "play")
    eq(P.centreState({ playing: true, buffering: true }), "buffering")
    eq(P.centreState({ loading: true }), "buffering")
    eq(P.centreState({ ended: true }), "replay")
    eq(P.centreState({ failed: true, playing: true }), "none")
})

test("speeds 0.25-2 in steps of 0.25; labels and the nearest step", function () {
    deepEq(P.SPEEDS, [0.25, 0.5, 0.75, 1.0, 1.25, 1.5, 1.75, 2.0])
    eq(P.speedText(1.25), "1.25×"); eq(P.speedText(2), "2×")
    ok(P.isNormalSpeed(1.0000001)); ok(!P.isNormalSpeed(1.25))
    eq(P.nearestSpeed(1.0000001), 1.0); eq(P.nearestSpeed(1.6), 1.5); eq(P.nearestSpeed(9), 2.0)
})

test("time labels: duration or remaining; live shows no right label", function () {
    deepEq(P.timeLabels(30000, 120000, false, false), { left: 30000, right: 120000, rightNegative: false })
    deepEq(P.timeLabels(30000, 120000, false, true), { left: 30000, right: 90000, rightNegative: true })
    deepEq(P.timeLabels(5000, 0, true, true), { left: 5000, right: -1, rightNegative: false })
    deepEq(P.timeLabels(-1, 0, false, false), { left: 0, right: -1, rightNegative: false })
})

test("settings sheet rows follow what the session offers, in a fixed order", function () {
    deepEq(P.sheetRows({ qualities: true, rate: true, audioOnlyAvailable: true }), ["quality", "speed", "audioOnly"])
    deepEq(P.sheetRows({ qualities: true, rate: true, captions: true, audioTracks: true, loop: true, sleepTimer: true,
                         stableVolume: true, audioOnlyAvailable: true }),
           ["quality", "speed", "captions", "audioTrack", "loop", "sleep", "stableVolume", "audioOnly"])
    deepEq(P.sheetRows({}), [])
    deepEq(P.sheetRows({ qualities: true, audioOnlyAvailable: true, lock: true }), ["quality", "audioOnly", "lock"], "the lock comes last")
})

test("the touch lock is offered in fullscreen and ends with it", function () {
    ok(P.lockOffered({ released: true, fullscreen: true, failed: false, ended: false }))
    ok(!P.lockOffered({ released: true, fullscreen: false }), "windowed: the page scrolls, nothing to lock")
    ok(!P.lockOffered({ released: true, fullscreen: true, failed: true }), "an error keeps its buttons reachable")
    ok(!P.lockOffered({ released: true, fullscreen: true, ended: true }), "the end keeps Replay and the suggestions")
    ok(!P.lockEnds({ released: true, fullscreen: true }), "the lock holds while the video plays in fullscreen")
    ok(P.lockEnds({ released: true, fullscreen: false }), "leaving fullscreen unlocks")
    ok(P.lockEnds({ released: true, fullscreen: true, failed: true }), "an error unlocks")
    ok(P.lockEnds({ released: true, fullscreen: true, ended: true }), "the end unlocks")
    eq(P.lockedTouch(), "showUnlock")
})

test("the touch lock stays hidden until its release switch is on", function () {
    ok(!P.lockOffered({ released: false, fullscreen: true }), "not released: never offered")
    ok(!P.lockOffered({ fullscreen: true }), "the switch must be passed")
    ok(P.lockEnds({ released: false, fullscreen: true }), "a lock cannot outlive the switch")
    eq(typeof P.TOUCH_LOCK_RELEASED, "boolean", "the switch is one boolean in PlayerUi.js")
})

test("seek bar: fractions, finger position, chapter gaps and names", function () {
    eq(P.fraction(30000, 120000), 0.25); eq(P.fraction(5, 0), 0); eq(P.fraction(200, 100), 1)
    eq(P.positionAt(50, 200, 120000), 30000); eq(P.positionAt(-5, 200, 1000), 0); eq(P.positionAt(500, 200, 1000), 1000)
    var ch = [{ title: "Intro", start: 0, end: 30000 }, { title: "Main", start: 30000, end: 90000 }, { title: "End", start: 90000, end: 120000 }]
    deepEq(P.chapterGaps(ch, 120000), [0.25, 0.75])
    eq(P.chapterAt(ch, 45000), "Main"); eq(P.chapterAt(ch, 0), "Intro"); eq(P.chapterAt([], 5), "")
})

test("SponsorBlock segments from either shape, coloured by category", function () {
    var r = P.segmentRects([{ start: 0, end: 30000, category: "sponsor" },
                            { startMs: 60000, endMs: 90000, category: "selfpromo" },
                            { start: 1, end: 2, category: "unknown" }], 120000)
    eq(r.length, 2)
    deepEq(r[0], { from: 0, to: 0.25, colour: "#00d400" })
    eq(r[1].from, 0.5); eq(r[1].to, 0.75)
})

test("vertical swipes: up enters fullscreen, down leaves it or minimises; horizontal is Silica's", function () {
    eq(P.swipeAction(0, -200, false, 100), "enterFullscreen")
    eq(P.swipeAction(0, 200, false, 100), "minimise")
    eq(P.swipeAction(0, 200, true, 100), "exitFullscreen")
    eq(P.swipeAction(0, -200, true, 100), "none")
    eq(P.swipeAction(300, 200, false, 100), "none", "mostly horizontal")
    eq(P.swipeAction(0, 50, false, 100), "none", "too short")
})

test("CC: off remembers the track; on picks the last or preferred track, else asks", function () {
    deepEq(P.captionsToggle({ on: true, currentKey: "en", lastKey: "" }), { action: "off", remember: "en" })
    deepEq(P.captionsToggle({ on: false, lastKey: "es", preferredKey: "en" }), { action: "pick", key: "es" })
    deepEq(P.captionsToggle({ on: false, lastKey: "", preferredKey: "en" }), { action: "pick", key: "en" })
    deepEq(P.captionsToggle({ on: false }), { action: "choose" })
    eq(P.captionBottomMargin(true, 100, 8), 108); eq(P.captionBottomMargin(false, 100, 8), 16)
    deepEq(P.SLEEP_CHOICES, [0, 15, 30, 45, 60, -1])
    // The session's values: -1 off; seconds left while it runs; atEnd for "end of video".
    eq(P.sleepState(-1, false), "off")
    eq(P.sleepState(undefined, undefined), "off", "a session without the timer")
    eq(P.sleepState(900, false), "running")
    eq(P.sleepState(0, false), "running", "the last second still runs")
    eq(P.sleepState(312, true), "end", "the media time left, pausing at the end of the video")
    eq(P.sleepState(-1, true), "off", "off wins over a stale flag")
})

test("CC turns on a track in the interface language, real before automatic", function () {
    var tracks = [
        { id: "en", label: "English", language: "en", autoGenerated: false, translated: false },
        { id: "es.auto", label: "Spanish (auto)", language: "es", autoGenerated: true, translated: false },
        { id: "es", label: "Spanish", language: "es-419", autoGenerated: false, translated: false },
        { id: "fr.tr", label: "French", language: "fr", autoGenerated: false, translated: true }
    ]
    eq(P.preferredCaption(tracks, "es_MX"), "es", "a real track in the interface language")
    eq(P.preferredCaption(tracks.slice(0, 2), "es"), "es.auto", "an automatic one when no real one exists")
    eq(P.preferredCaption(tracks, "fr_FR"), "en", "a translation is never the first pick: the first track")
    eq(P.preferredCaption(tracks, "de"), "en", "no match: the first track")
    eq(P.preferredCaption(tracks, ""), "en")
    eq(P.preferredCaption([], "es"), "")
    eq(P.preferredCaption(null, "es"), "")
    eq(P.captionLabel(tracks, "es"), "Spanish")
    eq(P.captionLabel(tracks, ""), "", "off")
    eq(P.captionLabel(tracks, "xx"), "", "a track that is gone")
})

test("a chosen caption track that cannot be loaded is said, not hidden", function () {
    ok(P.captionLoadFailed({ on: true, failed: true, cuesUrl: "u1", trackUrl: "u1" }))
    ok(!P.captionLoadFailed({ on: false, failed: true, cuesUrl: "u1", trackUrl: "u1" }), "captions are off")
    ok(!P.captionLoadFailed({ on: true, failed: false, cuesUrl: "u1", trackUrl: "u1" }), "the track loaded")
    ok(!P.captionLoadFailed({ on: true, failed: true, cuesUrl: "u0", trackUrl: "u1" }), "the failure is the last track's, not this one's")
    ok(!P.captionLoadFailed({ on: true, failed: true, cuesUrl: "", trackUrl: "" }), "no track address known")
})

test("the seek preview frame takes the storyboard cell's shape", function () {
    var f = P.previewFrame(160, 90, 320, 400)
    eq(f.width, 320); eq(f.height, 180, "16:9")
    f = P.previewFrame(216, 90, 324, 400)
    eq(f.width, 324); eq(f.height, 135, "a 2.4:1 film is flatter")
    f = P.previewFrame(90, 160, 320, 240)
    eq(f.height, 240, "a vertical video is capped in height"); eq(f.width, 135, "and narrows to keep its shape")
    f = P.previewFrame(0, 90, 320, 240)
    eq(f.width, 0); eq(f.height, 0, "no cell, no frame")
    f = P.previewFrame(160, 90, 0, 240)
    eq(f.width, 0)
})
