// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Main page sections, landing page (DSC-04), mini-player (PLY-11), pulley limit (PLT-06).
var S = modules.Shell

test("six sections in the user's order, each with a file", function () {
    deepEq(S.SECTIONS, ["home", "shorts", "subscriptions", "library", "search", "downloads"])
    for (var i = 0; i < S.SECTIONS.length; ++i) ok(/Section\.qml$/.test(S.sectionFile(S.SECTIONS[i])))
    eq(S.sectionIndex("search"), 4); eq(S.sectionIndex("nope"), 0)
})

test("DSC-04: the landing follows the setting; the default is Subscriptions for a user who has some", function () {
    eq(S.landingSection(undefined, 12), "subscriptions")
    eq(S.landingSection("subscriptions", 1), "subscriptions")
    eq(S.landingSection("garbage", 3), "subscriptions")
    eq(S.landingSection("search", 0), "search")
    eq(S.landingSection("search", 9), "search")
})

test("DSC-04: Home is honoured without an account", function () {
    eq(S.landingSection("recommended", 0), "home", "Home shows Popular signed out")
    eq(S.landingSection("recommended", 25), "home", "the choice wins over the subscriptions")
})

test("a first run without subscriptions lands on Home, not on an empty feed", function () {
    eq(S.landingSection(undefined, 0), "home")
    eq(S.landingSection("subscriptions", 0), "home", "the default has nothing to show yet")
    eq(S.landingSection(undefined, undefined), "home", "an unknown count is treated as none")
    eq(S.landingSection(undefined, -1), "home")
})

test("NFR-PERF-01: only the current and visited sections are instantiated", function () {
    deepEq(S.loadedSections("subscriptions", []), { home: false, shorts: false, subscriptions: true, library: false, search: false, downloads: false })
    deepEq(S.loadedSections("search", ["subscriptions"]), { home: false, shorts: false, subscriptions: true, library: false, search: true, downloads: false })
    deepEq(S.visit(S.visit(["home"], "home"), "bogus"), ["home"])
    var a = ["home"]; ok(S.visit(a, "search") !== a, "visit returns a new array")
})

test("PLT-06: pulley limit is 5 visible items", function () {
    eq(S.PULLEY_LIMIT, 5)
    eq(S.visiblePulleyItems([true, false, true, true]), 3)
})

function bar(o) {
    var s = { hasSession: true, dismissed: false, watchPageOnTop: false, pageOptOut: false, keyboardOpen: false }
    for (var k in o) s[k] = o[k]
    return S.miniPlayerVisible(s)
}

test("PLY-11: the bar shows on content pages while a video is loaded", function () {
    ok(bar({})); ok(!bar({ hasSession: false })); ok(!bar({ dismissed: true }))
})

test("PLY-11: hidden on the watch page, on opted-out pages and with the keyboard open", function () {
    ok(!bar({ watchPageOnTop: true })); ok(!bar({ pageOptOut: true })); ok(!bar({ keyboardOpen: true }))
})

test("PLY-11: progress line, next button and tap action", function () {
    eq(S.progress(30000, 120000, false), 0.25); eq(S.progress(5, 0, false), 0); eq(S.progress(500, 100, false), 1)
    eq(S.progress(30000, 120000, true), 0); eq(S.progress(-1, 100, false), 0)
    ok(S.hasNext(3, 1)); ok(!S.hasNext(3, 2)); ok(!S.hasNext(0, 0)); ok(!S.hasNext(3, -1))
    eq(S.tapAction("a", "a"), "reattach"); eq(S.tapAction("b", "a"), "reopen"); eq(S.tapAction("a", ""), "none")
})

test("bottom bar: a section switches on the main page and returns there from other pages", function () {
    deepEq(S.barTarget("library", true), { kind: "section", id: "library" })
    deepEq(S.barTarget("downloads", false), { kind: "return", id: "downloads" })
    eq(S.barTarget("bogus", true).kind, "none")
})

test("bottom bar hides on the watch page, opted-out pages, fullscreen and with the keyboard", function () {
    ok(S.navigationBarVisible({}))
    ok(!S.navigationBarVisible({ watchPageOnTop: true }))
    ok(!S.navigationBarVisible({ pageOptOut: true }))
    ok(!S.navigationBarVisible({ immersive: true }))
    ok(!S.navigationBarVisible({ keyboardOpen: true }))
})

test("bottom bar labels: all when the widest fits, else only the current one", function () {
    eq(S.navigationLabels(180, 150, 8), "all")
    eq(S.navigationLabels(90, 150, 8), "current")
    eq(S.navigationLabels(0, 10, 0), "current")
})


test("a list plays as the queue from the chosen row", function () {
    var list = [{ videoId: "a", title: "A" }, { videoId: "b", title: "B" }, { videoId: "c", title: "C" }]
    var p = S.queuePlan(list, 0)
    eq(p.ids.join(","), "a,b,c"); eq(p.at, 0); eq(p.first.title, "A")
    p = S.queuePlan(list, 2)
    eq(p.at, 2); eq(p.first.videoId, "c")
    p = S.queuePlan(list)
    eq(p.at, 0, "no index: from the first")
    p = S.queuePlan(list, 7)
    eq(p.at, 0, "an index outside the list starts at the first")
})

test("rows without a video id never reach the queue", function () {
    var p = S.queuePlan([{ title: "header" }, { videoId: "a" }, null, { videoId: "b" }], 3)
    eq(p.ids.join(","), "a,b")
    eq(p.at, 1, "the chosen row keeps its place among the playable ones")
    eq(p.first.videoId, "b")
    p = S.queuePlan([], 0)
    eq(p.ids.length, 0); eq(p.first, null)
    p = S.queuePlan(null, 0)
    eq(p.ids.length, 0); eq(p.first, null)
})
