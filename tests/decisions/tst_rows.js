// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// List row geometry and the empty-list state (docs/DESIGN.md §4.1, §5.1; NFR-PERF-01).
var R = modules.Rows

test("thumbnail is 42 % of the portrait width and keeps that size in landscape", function () {
    eq(R.thumbWidth(1080, 1080), 454)
    eq(R.thumbWidth(2520, 1080), 454, "landscape row on a 1080 px wide phone")
    eq(R.thumbWidth(540, 1080), 227, "narrower than the screen (split or dialog)")
    eq(R.thumbWidth(1080, 0), 454, "unknown screen size falls back to the row")
    eq(R.thumbWidth(-5, 1080), 0)
    eq(R.thumbHeight(454), 255)
})

test("skeleton covers the view with whole rows, capped", function () {
    eq(R.skeletonCount(2000, 287), 7)
    eq(R.skeletonCount(287, 287), 1)
    eq(R.skeletonCount(100000, 287), 12)
    eq(R.skeletonCount(100000, 287, 4), 4)
    eq(R.skeletonCount(0, 287), 1, "before layout")
    eq(R.skeletonCount(500, 0), 1)
})

test("a list without rows shows a skeleton while loading, a placeholder otherwise", function () {
    eq(R.emptyState(3, true, false), "rows")
    eq(R.emptyState(0, true, false), "skeleton")
    eq(R.emptyState(0, false, true), "skeleton", "first load about to start")
    eq(R.emptyState(0, false, false), "placeholder")
})

test("a failed list offers the downloads when something is saved", function () {
    ok(R.showDownloadsShortcut("placeholder", "No network", 3))
    ok(!R.showDownloadsShortcut("placeholder", "No network", 0), "nothing saved")
    ok(!R.showDownloadsShortcut("placeholder", "", 3), "empty, not failed")
    ok(!R.showDownloadsShortcut("skeleton", "No network", 3), "still loading")
    ok(!R.showDownloadsShortcut("rows", "No network", 3), "rows on screen")
})

test("cards on portrait-wide rows of lists that opt in; compact rows elsewhere", function () {
    ok(R.useCard(true, 1080, 1080))
    ok(!R.useCard(true, 2520, 1080), "landscape")
    ok(!R.useCard(false, 1080, 1080), "list without cards")
    ok(R.useCard(true, 540, 1080), "narrower is fine")
    eq(R.cardMeta("Channel · 2 days ago", "1.2M views"), "Channel · 2 days ago · 1.2M views")
    eq(R.cardMeta("Channel", ""), "Channel")
    eq(R.initial("@nordtile"), "N"); eq(R.initial(""), ""); eq(R.initial("élan"), "É")
})

test("prefetch: a row is on screen when at least half of it shows", function () {
    ok(R.rowOnScreen(100, 300, 0, 1000), "fully inside")
    ok(R.rowOnScreen(-90, 110, 0, 1000), "top card, a bit over half visible")
    ok(!R.rowOnScreen(-150, 50, 0, 1000), "mostly scrolled away")
    ok(R.rowOnScreen(600, 1400, 0, 1000), "half-visible card at the bottom")
    ok(!R.rowOnScreen(900, 1700, 0, 1000), "only a sliver")
    ok(!R.rowOnScreen(5, 5, 0, 10))
})

test("card thumbnails: decode at the card width with a large source, at most 640 px otherwise", function () {
    eq(R.decodeWidth(1080, true), 1080)
    eq(R.decodeWidth(1080, false), 640)
    eq(R.decodeWidth(454, false), 454)
    eq(R.decodeWidth(0, true), 0)
    eq(R.cardHeight(1080, 24, 96, 48), 608 + 24 + 96 + 48)
})

test("Continue watching lists the started, unfinished videos, newest first", function () {
    var rows = [
        { videoId: "a", fraction: 0.4, watched: false },
        { videoId: "b", fraction: 1, watched: true },
        { videoId: "c", fraction: 0.01, watched: false },
        { videoId: "d", fraction: 0.93, watched: true },
        { videoId: "e", fraction: 0.6, watched: false },
        { fraction: 0.5, watched: false },
        null,
        { videoId: "f", watched: false }
    ]
    var out = R.continueWatching(rows, 10)
    eq(out.length, 2)
    eq(out[0].videoId, "a"); eq(out[1].videoId, "e")
    eq(R.continueWatching(rows, 1).length, 1, "capped")
    eq(R.continueWatching([], 10).length, 0)
    eq(R.continueWatching(null, 10).length, 0)
    eq(R.continueWatching([{ videoId: "x", fraction: 0.05, watched: false }]).length, 1, "5 % counts as started")
    eq(R.continueWatching([{ videoId: "x", fraction: 1, watched: false }]).length, 0, "played to the end")
})

test("the shelf is rebuilt only when its content changes", function () {
    var a = [{ videoId: "a", fraction: 0.401 }, { videoId: "e", fraction: 0.6 }]
    var same = [{ videoId: "a", fraction: 0.404 }, { videoId: "e", fraction: 0.6 }]
    var moved = [{ videoId: "a", fraction: 0.52 }, { videoId: "e", fraction: 0.6 }]
    eq(R.continueKey(a), "a:40,e:60")
    eq(R.continueKey(a), R.continueKey(same), "a fraction of a percent is not a change")
    ok(R.continueKey(a) !== R.continueKey(moved))
    eq(R.continueKey([]), ""); eq(R.continueKey(null), "")
})
