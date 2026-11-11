// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// What the Shorts pager does with the shared player (docs/DESIGN.md §3.5). Pure decisions, no QML
// types: when a Short plays, what the session must be told, when its picture shows, and what a
// Short is opened with.
.pragma library

// PlaybackSession::State
// (one per line: the host test harness exposes the first name of a `var a, b` only)
var IDLE = 0
var LOADING = 1
var PREPARING = 2
var READY = 3
var FAILED = 4

// What the pager asks of the session (PlaybackSession.openShort / holdShort / releaseShort). A Short
// plays while the Shorts section is on screen: the pager holds it when the section is covered (the
// tab left, or a page, dialog or another video on top) and releases it when it is back; the user's
// own pause (a tap) is the session's, and survives a hold. A locked screen and the app leaving the
// front hold a Short by themselves.
//   s: { viewActive, currentId, sessionId, sessionShort, holding }
//   sessionShort: the session's playing item is a Short; holding: the pager holds it now.
// "open": the Short on screen is not what the session holds, and the section is on screen;
// "hold" / "release": it is, and the pager's hold differs from what the section being on screen
// asks for; "none" otherwise. The session never holds another video for the pager.
function sessionAction(s) {
    if (!s.currentId) return "none"
    var here = s.sessionId === s.currentId && !!s.sessionShort
    if (!here) return s.viewActive ? "open" : "none"
    if (!s.viewActive) return s.holding ? "none" : "hold"
    return s.holding ? "release" : "none"
}

// What a Short is opened with: its row's own hints (catalog's), or the fields of the row. The
// session adds the loop and the Short rules itself (openShort).
function hintsFor(row) {
    var h = {}, k
    var own = row && row.hints && typeof row.hints === "object" ? row.hints : null
    if (own) for (k in own) if (own.hasOwnProperty(k)) h[k] = own[k]
    if (row) {
        if (h.title === undefined && row.title) h.title = row.title
        if (h.channel === undefined && row.channel) h.channel = row.channel
        if (h.channelId === undefined && row.channelId) h.channelId = row.channelId
        if (h.thumbnail === undefined && row.thumbnail) h.thumbnail = row.thumbnail
        if (h.durationSeconds === undefined && row.durationSeconds > 0) h.durationSeconds = row.durationSeconds
    }
    return h
}

// The ids after the shown Short whose preload is asked for, next first, two at most
// (PlaybackSession.shortsAhead): the first is resolved and held, the second only resolved.
function aheadIds(rows, shown) {
    var out = []
    for (var i = shown + 1; i < (rows ? rows.length : 0) && out.length < 2; ++i)
        if (rows[i] && rows[i].videoId) out.push(rows[i].videoId)
    return out
}

// The progress line's fill, 0..1. The session's duration is 0 until it is known: the row's own
// (seconds, -1 unknown) stands in. A looping Short's position jumps back to 0 at each loop.
function progressFraction(positionMs, durationMs, rowSeconds) {
    var d = durationMs > 0 ? durationMs : (rowSeconds > 0 ? rowSeconds * 1000 : 0)
    if (!(d > 0) || !(positionMs > 0)) return 0
    return positionMs >= d ? 1 : positionMs / d
}

// The progress line grows between the session's position updates (one a second while the pager
// plays) and never animates backwards: a smaller fill is a loop or a seek, and the line jumps.
function progressMove(currentWidth, targetWidth) {
    return targetWidth < currentWidth ? "jump" : "grow"
}

// The live picture replaces the poster only when it is this Short's, the session is ready (a
// session that is loading or switching may still hold the previous Short's last frame) and the
// surface has a frame.
//   s: { sessionHere, hasFrame, sessionState }
function surfaceShown(s) {
    return !!s.sessionHere && !!s.hasFrame && s.sessionState === READY
}

// The Short's own shape: a vertical one fills the screen (cropped a little), anything wider is
// shown whole on black.
function cropsToFill(videoAspect) {
    return videoAspect > 0 && videoAspect < 1
}

// The list of Shorts is fetched again when it is empty, or when the user was away for a while
// (leftAt: when the section was left, ms since the epoch, 0 never). Coming back soon resumes the
// same Short and keeps the list as it was.
function shouldRefresh(count, busy, leftAt, now, awayMs) {
    if (busy) return false
    if (!(count > 0)) return true
    return leftAt > 0 && now - leftAt > awayMs
}

// The Short the pager shows once it settles on `index` of `count` rows, or -1.
function shownIndex(index, count) {
    return count > 0 && index >= 0 && index < count ? index : -1
}
