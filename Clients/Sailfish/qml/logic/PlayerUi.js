// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
.pragma library
// Watch-page player overlay decisions (docs/DESIGN.md §3.2): the centre disc, the playback speeds,
// the time labels and the rows of the player settings sheet. Pure ES5 (NFR-TST-04).

// Speeds offered in the settings sheet (PLY-07, SpeedPolicy 0.25-2.0), as the official apps do.
var SPEEDS = [0.25, 0.5, 0.75, 1.0, 1.25, 1.5, 1.75, 2.0]

// What the centre disc shows. s: { playing, buffering, loading, ended, failed }
//   "none" (failed: the error box takes the centre), "buffering" (a spinner in place of the
//   disc), "replay" (ended), "pause" (playing: tap pauses), "play" (paused: tap plays).
function centreState(s) {
    if (s.failed) return "none"
    if (s.loading || s.buffering) return "buffering"
    if (s.ended) return "replay"
    return s.playing ? "pause" : "play"
}

// "1.25×"-style label for a speed; the caller shows its own word for 1 ("Normal").
function speedText(rate) {
    var r = Math.round(rate * 100) / 100
    return String(r) + "×"
}

function isNormalSpeed(rate) { return Math.abs(rate - 1.0) < 0.001 }

// The speed in SPEEDS nearest to `rate` (the session may report 1.0000001).
function nearestSpeed(rate) {
    var best = SPEEDS[0]
    for (var i = 1; i < SPEEDS.length; ++i) if (Math.abs(SPEEDS[i] - rate) < Math.abs(best - rate)) best = SPEEDS[i]
    return best
}

// The two time labels of the bottom row, in milliseconds. A tap on them switches the right one
// between the duration and the time remaining ("-m:ss"), as the official apps do.
//   { left, right, rightNegative }
function timeLabels(positionMs, durationMs, live, showRemaining) {
    var pos = positionMs > 0 ? positionMs : 0
    if (live || !(durationMs > 0)) return { left: pos, right: -1, rightNegative: false }
    if (showRemaining) return { left: pos, right: Math.max(0, durationMs - pos), rightNegative: true }
    return { left: pos, right: durationMs, rightNegative: false }
}

// The rows of the settings sheet, in order, for what the session offers. api: booleans
//   qualities (video rungs exist), rate (speed changeable), captions (tracks exist), audioTracks,
//   loop, sleepTimer, stableVolume, audioOnlyAvailable (details loaded), lock (the touch lock is
//   offered, see lockOffered).
function sheetRows(api) {
    var out = []
    if (api.qualities) out.push("quality")
    if (api.rate) out.push("speed")
    if (api.captions) out.push("captions")
    if (api.audioTracks) out.push("audioTrack")
    if (api.loop) out.push("loop")
    if (api.sleepTimer) out.push("sleep")
    if (api.stableVolume) out.push("stableVolume")
    if (api.audioOnlyAvailable) out.push("audioOnly")
    if (api.lock) out.push("lock")
    return out
}

// ---- Touch lock (docs/DESIGN.md §3.2.6) ----

// Release switch of the touch lock: while false, the "Lock screen" row never shows. It goes to
// true once the device check of the lock (docs/DEVICE_DESIGN.md, step D2) has passed.
var TOUCH_LOCK_RELEASED = false

// The lock is for watching in fullscreen: offered there while a video plays or is paused, not on
// an error (its buttons must stay reachable) and not at the end (Replay and the suggestions).
// state.released: the release switch above (the page passes it; tests turn it on).
function lockOffered(state) {
    return !!state.released && !!state.fullscreen && !state.failed && !state.ended
}

// A lock in place ends by itself when what it was offered for is gone.
function lockEnds(state) {
    return !lockOffered(state)
}

// What a touch on the locked picture does: nothing but showing the way out. A tap (or the end of
// any press) shows the Unlock button for a while; swipes, holds, pinches and double taps are not
// read at all.
function lockedTouch() {
    return "showUnlock"
}

// ---- Seek bar (docs/DESIGN.md §3.2.1) ----

// Fraction 0..1 of `ms` in `durationMs` (0 when unknown).
function fraction(ms, durationMs) {
    if (!(durationMs > 0) || !(ms > 0)) return 0
    return ms >= durationMs ? 1 : ms / durationMs
}

// The position a finger at x (0..width) on the bar points to, in ms.
function positionAt(x, width, durationMs) {
    if (!(width > 0) || !(durationMs > 0)) return 0
    var f = x / width
    if (f < 0) f = 0
    if (f > 1) f = 1
    return Math.round(f * durationMs)
}

// Chapter starts as fractions, without the first (0), for the small gaps drawn in the track.
// chapters: [{ title, start (ms), end (ms) }] (PlaybackSession.chapters).
function chapterGaps(chapters, durationMs) {
    var out = []
    if (!chapters || !(durationMs > 0)) return out
    for (var i = 0; i < chapters.length; ++i) {
        var s = chapters[i].start
        if (s > 0 && s < durationMs) out.push(s / durationMs)
    }
    return out
}

// The chapter title at `ms`, "" when there are no chapters.
function chapterAt(chapters, ms) {
    if (!chapters) return ""
    var title = ""
    for (var i = 0; i < chapters.length; ++i) if (chapters[i].start <= ms) title = chapters[i].title || ""
    return title
}

// SponsorBlock categories (SB-01/02): the community's usual colours, so users who know the
// extension recognise them.
var SEGMENT_COLOURS = {
    sponsor: "#00d400", selfpromo: "#ffff00", interaction: "#cc00ff", intro: "#00ffff",
    outro: "#0202ed", preview: "#008fd6", music_offtopic: "#ff9900", filler: "#7300ff",
    poi_highlight: "#ff1684", exclusive_access: "#008a5c"
}

// Segments as fractions with their colour, from either shape: catalog's {start, end, category}
// or playback's {startMs, endMs, category}. Unknown categories are skipped.
function segmentRects(segments, durationMs) {
    var out = []
    if (!segments || !(durationMs > 0)) return out
    for (var i = 0; i < segments.length; ++i) {
        var g = segments[i]
        var a = g.startMs !== undefined ? g.startMs : g.start
        var b = g.endMs !== undefined ? g.endMs : g.end
        var c = SEGMENT_COLOURS[g.category]
        if (c === undefined || !(b > a)) continue
        out.push({ from: fraction(a, durationMs), to: fraction(b, durationMs), colour: c })
    }
    return out
}

// ---- Gestures on the picture (docs/DESIGN.md §3.2.2) ----

// A vertical swipe on the picture, as in the official app: up enters fullscreen, down leaves it
// or, windowed, minimises to the mini-player. Horizontal drags stay with Silica (back swipe).
//   dx, dy: the drag in pixels; threshold: the minimum travel.
function swipeAction(dx, dy, fullscreen, threshold) {
    var t = threshold > 0 ? threshold : 1
    if (Math.abs(dy) < t || Math.abs(dy) < 2 * Math.abs(dx)) return "none"
    if (dy < 0) return fullscreen ? "none" : "enterFullscreen"
    return fullscreen ? "exitFullscreen" : "minimise"
}

// ---- Captions, sleep timer (docs/DESIGN.md §3.2.3) ----

// Sleep timer choices in minutes: 0 = off, -1 = at the end of the video.
var SLEEP_CHOICES = [0, 15, 30, 45, 60, -1]

// The sleep timer as the sheet's row shows it, from the session's values: secondsLeft is -1
// while the timer is off, else the seconds until playback pauses; atEnd says it pauses at the end
// of the current video (secondsLeft is then the media time left). A session without the timer
// gives undefined, which reads as off.
function sleepState(secondsLeft, atEnd) {
    if (!(secondsLeft >= 0)) return "off"
    return atEnd === true ? "end" : "running"
}

// The track the CC button turns on when none was chosen before, from the session's tracks
// ([{ id, language, autoGenerated, translated }], real tracks first): a real track in the
// interface language, then an automatic one in that language, then the first track. `locale` is
// a name such as "es_MX" or "pt"; only its language part counts.
function preferredCaption(tracks, locale) {
    if (!tracks || tracks.length === 0) return ""
    var lang = String(locale || "").split(/[_-]/)[0].toLowerCase()
    var auto = ""
    for (var i = 0; i < tracks.length; ++i) {
        var t = tracks[i]
        if (!t || !t.id || t.translated) continue
        var tl = String(t.language || "").split(/[_-]/)[0].toLowerCase()
        if (lang === "" || tl !== lang) continue
        if (!t.autoGenerated) return t.id
        if (auto === "") auto = t.id
    }
    if (auto !== "") return auto
    return tracks[0] && tracks[0].id ? tracks[0].id : ""
}

// A chosen track whose text could not be fetched: the choice stays and nothing can be drawn. The
// caption box then says so for a moment instead of staying silent with CC showing as on.
// s: { on (a track is chosen), failed (the cue model failed), cuesUrl, trackUrl }; the failure
// counts only when it is the chosen track's own (the cue model may still hold the last one's).
function captionLoadFailed(s) {
    return !!s.on && !!s.failed && !!s.trackUrl && s.cuesUrl === s.trackUrl
}

// The label of the chosen track for the sheet's row ("" when off or unknown).
function captionLabel(tracks, id) {
    if (!id) return ""
    for (var i = 0; i < (tracks ? tracks.length : 0); ++i)
        if (tracks[i] && tracks[i].id === id) return tracks[i].label || ""
    return ""
}

// The preview frame of the seek bubble: `frameWidth` wide and as tall as the storyboard cell's
// own shape asks (cells follow the video's aspect: wide for a film, tall for a vertical video),
// within `maxHeight`; then the width shrinks to keep the shape. No cell: no frame.
function previewFrame(cellWidth, cellHeight, frameWidth, maxHeight) {
    if (!(cellWidth > 0) || !(cellHeight > 0) || !(frameWidth > 0)) return { width: 0, height: 0 }
    var w = frameWidth, h = Math.round(w * cellHeight / cellWidth)
    if (maxHeight > 0 && h > maxHeight) { h = Math.round(maxHeight); w = Math.round(h * cellWidth / cellHeight) }
    return { width: Math.round(w), height: h }
}

// The CC button: what a tap does. s: { on, currentKey, lastKey, preferredKey }
//   { action: "off", remember: key } | { action: "pick", key } | { action: "choose" } (no track
//   known yet: open the track list).
function captionsToggle(s) {
    if (s.on) return { action: "off", remember: s.currentKey || s.lastKey || "" }
    var key = s.lastKey || s.preferredKey || ""
    return key ? { action: "pick", key: key } : { action: "choose" }
}

// Where the caption box sits: above the bottom row while the controls show, near the bottom of
// the picture otherwise (the official apps move captions up for the controls).
function captionBottomMargin(controlsShown, bottomRowHeight, padding) {
    return controlsShown ? bottomRowHeight + padding : padding * 2
}
