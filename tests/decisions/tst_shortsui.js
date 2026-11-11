// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// The Shorts pager's decisions (docs/DESIGN.md §3.5): when a Short plays, what the session is told,
// when the live picture replaces the poster, what a Short is opened with.
var U = modules.ShortsUi

test("what the pager asks of the session", function () {
    function act(over) {
        var s = { viewActive: true, currentId: "a", sessionId: "a", sessionShort: true, holding: false }, k
        for (k in over) s[k] = over[k]
        return U.sessionAction(s)
    }
    eq(act({}), "none", "playing as wanted")
    eq(act({ sessionId: "b" }), "open", "another video: open this Short")
    eq(act({ sessionId: "" }), "open", "an empty session: open it")
    eq(act({ sessionShort: false }), "open", "the same video but not as a Short: open it as one")
    eq(act({ viewActive: false }), "hold", "the section is covered: hold it")
    eq(act({ viewActive: false, holding: true }), "none", "already held")
    eq(act({ holding: true }), "release", "the section is back: release")
    eq(act({ viewActive: false, sessionId: "b" }), "none", "a hold never touches another video")
    eq(act({ viewActive: false, sessionShort: false }), "none", "nor a normal video")
    eq(act({ currentId: "" }), "none", "no Short on screen")
    eq(act({ currentId: "", viewActive: false }), "none")
})

test("a Short opens with its row's hints; the session adds the loop", function () {
    var h = U.hintsFor({ videoId: "a", title: "T", channel: "C", thumbnail: "u", durationSeconds: 25, hints: { title: "Own", durationSeconds: 20 } })
    eq(h.title, "Own", "the row's own hints win")
    eq(h.durationSeconds, 20)
    eq(h.channel, "C", "missing ones come from the row")
    eq(h.thumbnail, "u")
    ok(h.loop === undefined, "no loop hint: openShort adds it")
    var plain = U.hintsFor({ videoId: "b", title: "Plain", channel: "", thumbnail: "", durationSeconds: -1 })
    eq(plain.title, "Plain")
    ok(plain.channel === undefined, "an empty channel is not a hint")
    ok(plain.durationSeconds === undefined, "an unknown duration is not a hint")
    deepEq(U.hintsFor(null), {})
    var own = { title: "Kept" }
    U.hintsFor({ hints: own, title: "Row" })
    eq(own.title, "Kept", "the row's hints are left as they were")
})

test("the preload asked for is the next two ids", function () {
    var rows = [{ videoId: "a" }, { videoId: "b" }, { videoId: "c" }, { videoId: "d" }]
    deepEq(U.aheadIds(rows, 0), ["b", "c"])
    deepEq(U.aheadIds(rows, 2), ["d"], "the end of the list")
    deepEq(U.aheadIds(rows, 3), [], "nothing after the last")
    deepEq(U.aheadIds(rows, -1), ["a", "b"], "nothing shown yet")
    deepEq(U.aheadIds([{ videoId: "a" }, {}, { videoId: "c" }], 0), ["c"], "a row without an id is left out")
    deepEq(U.aheadIds(null, 0), [])
})

test("the progress line", function () {
    eq(U.progressFraction(5000, 20000, -1), 0.25)
    eq(U.progressFraction(5000, 0, 20), 0.25, "the row's duration until the session knows it")
    eq(U.progressFraction(5000, 0, -1), 0, "no duration at all")
    eq(U.progressFraction(0, 20000, -1), 0)
    eq(U.progressFraction(25000, 20000, -1), 1, "never past the end")
    eq(U.progressMove(50, 80), "grow")
    eq(U.progressMove(50, 50), "grow", "no change is not a jump")
    eq(U.progressMove(240, 3), "jump", "the loop starts again: no backward animation")
})

test("the live picture replaces the poster only when it is this Short's and ready", function () {
    ok(U.surfaceShown({ sessionHere: true, hasFrame: true, sessionState: U.READY }))
    ok(!U.surfaceShown({ sessionHere: false, hasFrame: true, sessionState: U.READY }), "the session holds another Short")
    ok(!U.surfaceShown({ sessionHere: true, hasFrame: false, sessionState: U.READY }), "no frame yet")
    ok(!U.surfaceShown({ sessionHere: true, hasFrame: true, sessionState: U.LOADING }), "a switch in progress may still hold the last frame")
    ok(!U.surfaceShown({ sessionHere: true, hasFrame: true, sessionState: U.FAILED }))
    ok(U.cropsToFill(0.5625), "vertical fills the screen")
    ok(!U.cropsToFill(1.7778), "wide is shown whole")
    ok(!U.cropsToFill(0), "unknown aspect: whole")
})

test("the list is fetched again when empty or after a long absence, not on a quick return", function () {
    var away = 600000
    ok(U.shouldRefresh(0, false, 0, 1000, away), "empty")
    ok(!U.shouldRefresh(0, true, 0, 1000, away), "already loading")
    ok(!U.shouldRefresh(8, false, 0, 1000, away), "first visit with rows (the section fetched them)")
    ok(!U.shouldRefresh(8, false, 100000, 200000, away), "back after 100 s: the same Short resumes")
    ok(U.shouldRefresh(8, false, 100000, 800000, away), "back after 700 s")
    ok(!U.shouldRefresh(8, true, 100000, 800000, away), "busy")
})

test("which row is shown", function () {
    eq(U.shownIndex(2, 5), 2); eq(U.shownIndex(0, 0), -1); eq(U.shownIndex(5, 5), -1); eq(U.shownIndex(-1, 5), -1)
})
