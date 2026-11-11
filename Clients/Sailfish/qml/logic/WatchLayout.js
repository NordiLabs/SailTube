// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
.pragma library
// Watch-page chrome decisions: orientation and fullscreen (PLY-13), the portrait video box
// (PLY-14), cutouts (PLY-15) and the navigation indicator, "edge glow" (PLY-16).
// Pure ES5, no QML types (NFR-TST-04). Orientation values are Silica's Orientation enum; the
// UI test checks that they match.

var PORTRAIT = 1
var LANDSCAPE = 2
var PORTRAIT_INVERTED = 4
var LANDSCAPE_INVERTED = 8
var PORTRAIT_MASK = 5
var LANDSCAPE_MASK = 10
var ALL = 15

// Fullscreen modes of the watch page.
var WINDOWED = "windowed"
var PORTRAIT_FULLSCREEN = "portrait"
var LANDSCAPE_FULLSCREEN = "landscape"

// "Vertical-video fullscreen" setting values (SET-01).
var VERTICAL_AUTO = "auto"
var VERTICAL_LANDSCAPE = "landscape"

// "Edge glow" setting values (SET-01).
var GLOW_AUTO = "auto"
var GLOW_HIDDEN = "hidden"

// Largest share of the screen height a vertical video takes in windowed portrait (PLY-14).
var VERTICAL_BOX_SHARE = 0.6

// A video is vertical when its aspect is below 1. Unknown sizes count as landscape.
function isVertical(videoWidth, videoHeight) {
    return videoWidth > 0 && videoHeight > 0 && videoWidth < videoHeight
}

// Height of the windowed portrait video box (PLY-14): 16:9 at the given width for landscape
// or unknown videos; a vertical video gets its own aspect, capped at 60 % of the screen height
// and never shorter than the 16:9 box.
function boxHeight(width, screenHeight, videoWidth, videoHeight) {
    if (!(width > 0)) return 0
    var standard = Math.round(width * 9 / 16)
    if (!isVertical(videoWidth, videoHeight)) return standard
    var natural = Math.round(width * videoHeight / videoWidth)
    var cap = screenHeight > 0 ? Math.round(screenHeight * VERTICAL_BOX_SHARE) : natural
    return Math.max(standard, Math.min(natural, cap))
}

// What the fullscreen button does (PLY-13). state:
//   mode: current mode (WINDOWED, PORTRAIT_FULLSCREEN, LANDSCAPE_FULLSCREEN)
//   pageLandscape: the page is currently shown in a landscape orientation
//   vertical: the video is vertical
//   verticalSetting: VERTICAL_AUTO or VERTICAL_LANDSCAPE
//   basePolicy: the page's own allowed orientations outside fullscreen
// Returns { mode, allowedOrientations, restorePending }.
// Leaving a landscape state pins portrait only until the device is upright again (see
// restoreAfterExit), so the page gets its own policy back and auto-rotate keeps working.
function toggleFullscreen(state) {
    if (state.mode === PORTRAIT_FULLSCREEN)
        return { mode: WINDOWED, allowedOrientations: state.basePolicy, restorePending: false }
    if (state.mode === LANDSCAPE_FULLSCREEN || state.pageLandscape)
        return { mode: WINDOWED, allowedOrientations: PORTRAIT, restorePending: true }
    if (state.vertical && state.verticalSetting !== VERTICAL_LANDSCAPE)
        return { mode: PORTRAIT_FULLSCREEN, allowedOrientations: PORTRAIT, restorePending: false }
    // Both landscape directions; works with the rotation lock on.
    return { mode: LANDSCAPE_FULLSCREEN, allowedOrientations: LANDSCAPE_MASK, restorePending: false }
}

// After a forced portrait exit: the orientations to apply once the device itself is no longer
// held sideways (or the rotation lock reports portrait), or -1 to keep portrait pinned.
function restoreAfterExit(restorePending, deviceOrientation, basePolicy) {
    if (!restorePending) return -1
    if (deviceOrientation & LANDSCAPE_MASK) return -1
    return basePolicy
}

// How much of the thumbnail shows behind what is drawn on the picture box (the box is black
// under it). Text and buttons need a dark ground: the error box most; the spinner and status line
// while loading, and a video that has not shown its first frame, less. A video that plays
// without a picture (listen only) shows its thumbnail whole.
function thumbnailOpacity(state) {
    if (state.failed) return 0.3
    if (state.loading || state.hasVideo) return 0.6
    return 1.0
}

// The page shows the picture alone, without the info panel.
function immersive(mode, pageLandscape, hasVideo) {
    if (mode === PORTRAIT_FULLSCREEN) return hasVideo
    return hasVideo && (pageLandscape || mode === LANDSCAPE_FULLSCREEN)
}

// The page leaves fullscreen when the video goes away (audio only, failure, close).
function modeWithoutVideo(mode, hasVideo) {
    return hasVideo ? mode : WINDOWED
}

// Insets of the picture for the top cutout (PLY-15). geometry:
//   portrait: the page is portrait
//   immersive: fullscreen (either orientation)
//   cutoutBottom: bottom edge of the top cutout in portrait coordinates (0 without one)
//   cutoutHeight: height of the top cutout (its depth into the long edge in landscape)
//   screenLong, screenShort: screen size in pixels
//   videoWidth, videoHeight: picture size (0 when unknown)
// Returns { top, bottom, left, right } in page coordinates; the strip is filled black.
function cutoutInsets(geometry) {
    var none = { top: 0, bottom: 0, left: 0, right: 0 }
    var cutout = geometry.cutoutBottom > 0 ? geometry.cutoutBottom : 0
    if (cutout === 0) return none
    if (geometry.portrait) {
        // Windowed: the video box starts below the cutout over a black strip. Fullscreen: the
        // picture is fitted to the safe area below the cutout, so the notch never covers it.
        return { top: cutout, bottom: 0, left: 0, right: 0 }
    }
    // Landscape: the cutout sits on one short side. Inset only when the fitted picture would
    // reach under it; inset both sides so the picture stays centred.
    var depth = geometry.cutoutHeight > 0 ? geometry.cutoutHeight : cutout
    var w = geometry.videoWidth, h = geometry.videoHeight
    if (!(w > 0 && h > 0)) { w = 16; h = 9 }
    var pictureWidth = Math.min(geometry.screenLong, geometry.screenShort * w / h)
    var margin = (geometry.screenLong - pictureWidth) / 2
    if (margin >= depth) return none
    return { top: 0, bottom: 0, left: depth, right: depth }
}

// The picture rectangle inside an area (x, y, width, height) for a video of the given size,
// fitted and centred (PreserveAspectFit). Unknown sizes count as 16:9. With the insets of
// cutoutInsets() as the area, the picture never reaches under the cutout (PLY-15).
function pictureRect(area, videoWidth, videoHeight) {
    var w = videoWidth > 0 && videoHeight > 0 ? videoWidth : 16
    var h = videoWidth > 0 && videoHeight > 0 ? videoHeight : 9
    var scale = Math.min(area.width / w, area.height / h)
    var pw = Math.floor(w * scale), ph = Math.floor(h * scale)
    return { x: area.x + Math.floor((area.width - pw) / 2), y: area.y + Math.floor((area.height - ph) / 2),
             width: pw, height: ph }
}

// The area left for the picture on a page of the given size after the insets.
function safeArea(pageWidth, pageHeight, insets) {
    return { x: insets.left, y: insets.top, width: pageWidth - insets.left - insets.right,
             height: pageHeight - insets.top - insets.bottom }
}

// Whether the page navigation indicator ("edge glow") is shown (PLY-16). state:
//   setting: GLOW_AUTO or GLOW_HIDDEN
//   immersive, pageLandscape, playing, controlsVisible, hasVideo
// The back swipe keeps working either way; only the indicator is hidden.
function showEdgeGlow(state) {
    if (state.setting === GLOW_HIDDEN) return false
    if (state.immersive || state.pageLandscape) return false
    if (state.hasVideo && state.playing && !state.controlsVisible) return false
    return true
}

// ---- Picture box while there is no picture yet (docs/DESIGN.md §3.1) ----

// After this long in Loading the box also offers Retry, so a spinner never sits alone forever.
var SLOW_LOADING_MS = 12000

// s: { loading, buffering, recovering, failed, elapsedMs, statusText }
// spinner: the Large BusyIndicator; status: the status line under it; retry: Retry under it.
function loadingChrome(s) {
    var busy = !s.failed && (!!s.loading || !!s.buffering || !!s.recovering)
    return {
        spinner: busy,
        status: busy && !!s.statusText,
        retry: busy && !!s.loading && s.elapsedMs >= SLOW_LOADING_MS
    }
}

// The watch page's download note: 100 only once the file is ready (the last step, the remux,
// runs at 100 % of the bytes), so a running download reads at most 99 %.
function downloadPercent(progress, done) {
    if (done) return 100
    var p = Math.floor((progress > 0 ? progress : 0) * 100)
    return p > 99 ? 99 : p
}
