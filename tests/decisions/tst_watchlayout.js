// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// PLY-13 orientation and fullscreen, PLY-14 portrait box, PLY-15 cutouts, PLY-16 edge glow.
var W = modules.WatchLayout

test("orientation constants match Silica's enum", function () {
    eq(W.PORTRAIT, 1); eq(W.LANDSCAPE, 2); eq(W.PORTRAIT_INVERTED, 4); eq(W.LANDSCAPE_INVERTED, 8)
    eq(W.PORTRAIT_MASK, 5); eq(W.LANDSCAPE_MASK, 10); eq(W.ALL, 15)
})

test("aspect below 1 is vertical; unknown sizes are not", function () {
    ok(W.isVertical(1080, 1920)); ok(!W.isVertical(1920, 1080)); ok(!W.isVertical(1000, 1000))
    ok(!W.isVertical(0, 0)); ok(!W.isVertical(undefined, 720))
})

test("PLY-14: landscape video gets a 16:9 box at screen width", function () {
    eq(W.boxHeight(1080, 2400, 1920, 1080), 608)
    eq(W.boxHeight(1080, 2400, 0, 0), 608)
})

test("PLY-14: vertical video gets a taller box capped at 60 % of the screen height", function () {
    eq(W.boxHeight(1080, 2400, 1080, 1920), 1440)       // 1920 natural, 60 % of 2400 = 1440
    eq(W.boxHeight(1080, 2400, 1080, 1350), 1350)       // 4:5 fits below the cap
    eq(W.boxHeight(1080, 2400, 900, 1000), 1200)        // never shorter than 16:9
    eq(W.boxHeight(1080, 1000, 1080, 1920), 608)        // tiny screen: 16:9 floor wins
    eq(W.boxHeight(0, 2400, 1080, 1920), 0)
})

function fs(mode, landscape, vertical, setting) {
    return W.toggleFullscreen({ mode: mode, pageLandscape: landscape, vertical: vertical,
                                verticalSetting: setting || W.VERTICAL_AUTO, basePolicy: W.ALL })
}

test("PLY-13: fullscreen on a landscape video forces both landscape directions", function () {
    var r = fs(W.WINDOWED, false, false)
    eq(r.mode, W.LANDSCAPE_FULLSCREEN); eq(r.allowedOrientations, W.LANDSCAPE_MASK); ok(!r.restorePending)
})

test("PLY-13: a vertical video goes fullscreen in portrait automatically", function () {
    var r = fs(W.WINDOWED, false, true)
    eq(r.mode, W.PORTRAIT_FULLSCREEN); eq(r.allowedOrientations, W.PORTRAIT)
})

test("PLY-13: the setting can force the old landscape behaviour for vertical videos", function () {
    eq(fs(W.WINDOWED, false, true, W.VERTICAL_LANDSCAPE).mode, W.LANDSCAPE_FULLSCREEN)
})

test("PLY-13: leaving portrait fullscreen restores the page's own policy at once", function () {
    var r = fs(W.PORTRAIT_FULLSCREEN, false, true)
    eq(r.mode, W.WINDOWED); eq(r.allowedOrientations, W.ALL); ok(!r.restorePending)
})

test("PLY-13: leaving landscape pins portrait until the device is upright, then restores", function () {
    var r = fs(W.LANDSCAPE_FULLSCREEN, true, false)
    eq(r.mode, W.WINDOWED); eq(r.allowedOrientations, W.PORTRAIT); ok(r.restorePending)
    eq(W.restoreAfterExit(r.restorePending, W.LANDSCAPE, W.ALL), -1)
    eq(W.restoreAfterExit(r.restorePending, W.LANDSCAPE_INVERTED, W.ALL), -1)
    eq(W.restoreAfterExit(r.restorePending, W.PORTRAIT, W.ALL), W.ALL)
    eq(W.restoreAfterExit(r.restorePending, 0, W.ALL), W.ALL)            // unknown / locked
    eq(W.restoreAfterExit(false, W.PORTRAIT, W.ALL), -1)
})

test("PLY-13: the button in rotated immersive landscape returns to portrait", function () {
    var r = fs(W.WINDOWED, true, false)
    eq(r.mode, W.WINDOWED); eq(r.allowedOrientations, W.PORTRAIT); ok(r.restorePending)
})

test("immersive: landscape with video, or either fullscreen mode", function () {
    ok(W.immersive(W.WINDOWED, true, true)); ok(!W.immersive(W.WINDOWED, true, false))
    ok(!W.immersive(W.WINDOWED, false, true)); ok(W.immersive(W.PORTRAIT_FULLSCREEN, false, true))
    ok(W.immersive(W.LANDSCAPE_FULLSCREEN, false, true)); ok(!W.immersive(W.PORTRAIT_FULLSCREEN, false, false))
    eq(W.modeWithoutVideo(W.LANDSCAPE_FULLSCREEN, false), W.WINDOWED)
    eq(W.modeWithoutVideo(W.LANDSCAPE_FULLSCREEN, true), W.LANDSCAPE_FULLSCREEN)
})

function insets(portrait, immersive, cutout, vw, vh) {
    return W.cutoutInsets({ portrait: portrait, immersive: immersive, cutoutBottom: cutout, cutoutHeight: cutout,
                            screenLong: 2400, screenShort: 1080, videoWidth: vw, videoHeight: vh })
}

test("PLY-15: no cutout, no insets", function () {
    deepEq(insets(true, false, 0, 1920, 1080), { top: 0, bottom: 0, left: 0, right: 0 })
    deepEq(insets(false, true, 0, 1920, 1080), { top: 0, bottom: 0, left: 0, right: 0 })
})

test("PLY-15: portrait windowed offsets the video by the cutout bottom", function () {
    eq(insets(true, false, 90, 1920, 1080).top, 90)
})

test("PLY-15: portrait fullscreen fits the picture below the cutout", function () {
    eq(insets(true, true, 90, 1080, 1920).top, 90)
})

test("PLY-15: landscape insets both sides only when the picture would reach the cutout", function () {
    deepEq(insets(false, true, 90, 1920, 1080), { top: 0, bottom: 0, left: 0, right: 0 })  // 16:9 pillarbox 240 px
    deepEq(insets(false, true, 90, 2400, 1000), { top: 0, bottom: 0, left: 90, right: 90 }) // 2.4:1 fills the width
    deepEq(insets(false, true, 300, 1920, 1080), { top: 0, bottom: 0, left: 300, right: 300 })
})

function glow(setting, immersive, landscape, playing, controls) {
    return W.showEdgeGlow({ setting: setting, immersive: immersive, pageLandscape: landscape, playing: playing,
                            controlsVisible: controls, hasVideo: true })
}

test("PLY-16: glow hidden in fullscreen, in landscape and while controls hide during playback", function () {
    ok(!glow(W.GLOW_AUTO, true, false, false, true))
    ok(!glow(W.GLOW_AUTO, false, true, false, true))
    ok(!glow(W.GLOW_AUTO, false, false, true, false))
})

test("PLY-16: glow shown in windowed portrait while paused or with the controls visible", function () {
    ok(glow(W.GLOW_AUTO, false, false, false, false))
    ok(glow(W.GLOW_AUTO, false, false, true, true))
})

test("PLY-16: the setting hides it always", function () {
    ok(!glow(W.GLOW_HIDDEN, false, false, false, true))
})

test("PLY-16: audio-only playback keeps the glow", function () {
    ok(W.showEdgeGlow({ setting: W.GLOW_AUTO, immersive: false, pageLandscape: false, playing: true,
                        controlsVisible: false, hasVideo: false }))
})

// ---- SAILTUBE_REQUIREMENTS v1.0 acceptance cases ----

test("PLY-14 accept: 1080x2520 portrait, 16:9 gets 1080x608, 9:16 gets >608 and <=1512", function () {
    eq(W.boxHeight(1080, 2520, 1920, 1080), 608)
    var tall = W.boxHeight(1080, 2520, 1080, 1920)
    ok(tall > 608, "taller than 608: " + tall); ok(tall <= 1512, "at most 60 % of 2520: " + tall)
})

function synthetic(portrait, immersive, vw, vh) {
    return W.cutoutInsets({ portrait: portrait, immersive: immersive, cutoutBottom: 100, cutoutHeight: 100,
                            screenLong: 2520, screenShort: 1080, videoWidth: vw, videoHeight: vh })
}

test("PLY-15 accept: a 100 px cutout puts the windowed video 100 px down", function () {
    var i = synthetic(true, false, 1920, 1080)
    eq(i.top, 100)       // the page fills the strip above the box black
})

test("PLY-15 accept: the fullscreen picture stays inside the safe area", function () {
    var sizes = [[1080, 1920], [1920, 1080], [1080, 1080], [0, 0], [720, 2400]]
    for (var k = 0; k < sizes.length; ++k) {
        var vw = sizes[k][0], vh = sizes[k][1]
        var portrait = W.safeArea(1080, 2520, synthetic(true, true, vw, vh))
        var r = W.pictureRect(portrait, vw, vh)
        ok(r.y >= 100 && r.y + r.height <= 2520 && r.x >= 0 && r.x + r.width <= 1080, "portrait " + vw + "x" + vh + " " + JSON.stringify(r))
        var land = synthetic(false, true, vw, vh)
        var lr = W.pictureRect(W.safeArea(2520, 1080, land), vw, vh)
        // The cutout occupies the first 100 px of the long edge in landscape.
        ok(lr.x >= 100 || (2520 - lr.width) / 2 >= 100, "landscape " + vw + "x" + vh + " " + JSON.stringify(lr))
    }
})

test("PLY-15 accept: without the cutout API the same code gives zero insets", function () {
    deepEq(W.cutoutInsets({ portrait: true, immersive: true }), { top: 0, bottom: 0, left: 0, right: 0 })
    deepEq(W.cutoutInsets({ portrait: false, immersive: true, cutoutBottom: undefined }), { top: 0, bottom: 0, left: 0, right: 0 })
})

test("PLY-16 accept: indicator in every listed state", function () {
    var base = { setting: W.GLOW_AUTO, immersive: false, pageLandscape: false, playing: false, controlsVisible: true, hasVideo: true }
    function with_(o) { var s = {}; for (var k in base) s[k] = base[k]; for (var j in o) s[j] = o[j]; return W.showEdgeGlow(s) }
    eq(with_({ immersive: true }), false, "fullscreen")
    eq(with_({ pageLandscape: true }), false, "landscape")
    eq(with_({ playing: true, controlsVisible: false }), false, "playing, controls hidden")
    eq(with_({ playing: false, controlsVisible: false }), true, "paused")
    eq(with_({ playing: true, controlsVisible: true }), true, "controls visible")
    eq(with_({ setting: W.GLOW_HIDDEN }), false, "always-hide setting")
})

test("loading chrome: spinner with a status line, Retry once loading is slow, nothing when failed", function () {
    var W = modules.WatchLayout
    var c = W.loadingChrome({ loading: true, elapsedMs: 1000, statusText: "Finding the stream" })
    ok(c.spinner && c.status && !c.retry, "early loading")
    ok(W.loadingChrome({ loading: true, elapsedMs: W.SLOW_LOADING_MS }).retry, "slow loading offers Retry")
    ok(!W.loadingChrome({ loading: true, elapsedMs: 99999, failed: true }).spinner, "the error state takes over")
    var b = W.loadingChrome({ buffering: true, elapsedMs: 99999 })
    ok(b.spinner && !b.retry && !b.status, "buffering: spinner only")
    ok(W.loadingChrome({ recovering: true, statusText: "Reconnecting" }).status, "recovery shows its status")
    ok(!W.loadingChrome({}).spinner, "idle")
})

test("download note reads 99 % until the file is ready", function () {
    var W = modules.WatchLayout
    eq(W.downloadPercent(0.5, false), 50)
    eq(W.downloadPercent(1, false), 99)
    eq(W.downloadPercent(1, true), 100)
    eq(W.downloadPercent(-1, false), 0)
})

test("the thumbnail is dimmed under the spinner and more under the error box", function () {
    var W = modules.WatchLayout
    eq(W.thumbnailOpacity({ failed: true, loading: false, hasVideo: false }), 0.3, "error text and buttons need a dark ground")
    eq(W.thumbnailOpacity({ failed: false, loading: true, hasVideo: false }), 0.6, "spinner and status line while loading")
    eq(W.thumbnailOpacity({ failed: false, loading: false, hasVideo: true }), 0.6, "a video before its first frame")
    eq(W.thumbnailOpacity({ failed: false, loading: false, hasVideo: false }), 1.0, "listen only: the picture whole")
    eq(W.thumbnailOpacity({ failed: true, loading: true, hasVideo: true }), 0.3, "a failure wins")
})
