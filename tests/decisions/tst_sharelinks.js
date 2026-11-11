// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Channel and playlist share links and the playlist head (docs/DESIGN.md §4.8).
var L = modules.ShareLinks

test("a channel is shared by its id, else by its handle", function () {
    eq(L.channel("UCabcdefghijklmnopqrstuv", "@some.handle"), "https://www.youtube.com/channel/UCabcdefghijklmnopqrstuv")
    eq(L.channel("", "@some.handle"), "https://www.youtube.com/@some.handle")
    eq(L.channel("UC_x-0123456789abcdefghi", ""), "https://www.youtube.com/channel/UC_x-0123456789abcdefghi")
})

test("a channel without a plain id or handle has no link", function () {
    eq(L.channel("", ""), "")
    eq(L.channel("UCshort", ""), "", "too short")
    eq(L.channel("XXabcdefghijklmnopqrstuv", ""), "", "not a UC id")
    eq(L.channel("UCabcdefghijklmnopqrst/v", ""), "", "a path character never reaches the link")
    eq(L.channel("", "handle"), "", "a handle starts with @")
    eq(L.channel("", "@a b"), "", "no spaces")
    eq(L.channel("", "@x?y=1"), "", "no query characters")
    eq(L.channel(undefined, null), "")
})

test("a public playlist is shared by its id", function () {
    eq(L.playlist("PLabcdefghijklmnopqrstuvwxyz012345"), "https://www.youtube.com/playlist?list=PLabcdefghijklmnopqrstuvwxyz012345")
    eq(L.playlist("OLAK5uy_abcdefghijklmnopqrstuvwxyz01234567"), "https://www.youtube.com/playlist?list=OLAK5uy_abcdefghijklmnopqrstuvwxyz01234567")
})

test("private, local and malformed playlist ids have no link", function () {
    eq(L.playlist("WL"), "", "Watch later belongs to an account")
    eq(L.playlist("LL"), "")
    eq(L.playlist(""), "")
    eq(L.playlist("PLabc&feature=share"), "", "no query characters")
    eq(L.playlist("local playlist 1"), "")
    eq(L.playlist(42), "")
})

test("the playlist head shows what the list can do", function () {
    var all = L.headParts({ coverUrl: "https://i.ytimg.com/vi/x/hqdefault.jpg", count: 12, queue: true })
    ok(all.cover); ok(all.play); ok(all.shuffle)
    var one = L.headParts({ coverUrl: "", count: 1, queue: true })
    ok(!one.cover, "no picture, no cover"); ok(one.play); ok(!one.shuffle, "one video cannot be shuffled")
    var noQueue = L.headParts({ coverUrl: "x", count: 5, queue: false })
    ok(noQueue.play); ok(!noQueue.shuffle, "Shuffle needs a queue")
    var empty = L.headParts({ count: 0, queue: true })
    ok(!empty.play); ok(!empty.shuffle)
    var none = L.headParts(null)
    ok(!none.cover); ok(!none.play); ok(!none.shuffle)
})

test("shuffle keeps every entry once and leaves the input alone", function () {
    var input = ["a", "b", "c", "d", "e"]
    var seq = [0.9, 0.1, 0.5, 0.0], at = 0
    var out = L.shuffled(input, function () { return seq[at++ % seq.length] })
    eq(input.join(""), "abcde")
    eq(out.length, 5)
    eq(out.slice(0).sort().join(""), "abcde")
    ok(out.join("") !== "abcde", "this sequence moves something")
    eq(L.shuffled([], function () { return 0 }).length, 0)
    eq(L.shuffled(null).length, 0)
    eq(L.shuffled(["x"], function () { return 0.99 }).join(""), "x")
    // A random source that misbehaves (1.0) still stays inside the list.
    eq(L.shuffled(["a", "b", "c"], function () { return 1 }).slice(0).sort().join(""), "abc")
})

test("a Short is shared by its own address", function () {
    eq(L.shortVideo("dQw4w9WgXcQ"), "https://www.youtube.com/shorts/dQw4w9WgXcQ")
    eq(L.shortVideo("a_b-c0123456"), "", "twelve characters")
    eq(L.shortVideo("short"), "", "too short")
    eq(L.shortVideo("dQw4w9WgXc/"), "", "a path character never reaches the link")
    eq(L.shortVideo("dQw4w9WgXc?"), "", "no query characters")
    eq(L.shortVideo(""), ""); eq(L.shortVideo(undefined), ""); eq(L.shortVideo(12345678901), "")
    ok(L.isVideoId("abcdefghijk") && !L.isVideoId("abcdefghij"))
})
