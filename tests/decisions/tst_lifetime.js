// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// PLT-06: callbacks that arrive after their page is destroyed do nothing.
var L = modules.Lifetime

test("a wrapped callback runs while the page lives", function () {
    var t = L.create("page"), hits = 0
    var cb = L.wrap(t, function (x) { hits += x; return "ran" })
    eq(cb(2), "ran"); eq(hits, 2)
})

test("after release the callback is dropped", function () {
    var t = L.create("page"), hits = 0
    var cb = L.wrap(t, function () { ++hits })
    L.release(t)
    eq(cb(), undefined); eq(hits, 0); ok(!L.isCurrent(t))
})

test("an answer for an older request generation is dropped", function () {
    var t = L.create("search"), got = []
    var first = L.wrap(t, function (q) { got.push(q) }, true)
    L.renew(t)
    var second = L.wrap(t, function (q) { got.push(q) }, true)
    first("old"); second("new")
    deepEq(got, ["new"])
})

test("tokens are distinct and releasing null is harmless", function () {
    ok(L.create().id !== L.create().id); L.release(null); eq(L.renew(null), 0)
})
