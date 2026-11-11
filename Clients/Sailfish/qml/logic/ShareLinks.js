// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// The link a user shares for a channel or a playlist (docs/DESIGN.md §4.8). Pure decisions: the
// address is built from the id alone and never requested by the app; an id that is not a plain
// YouTube id gives "", and the page then offers no Share action.
.pragma library

var BASE = "https://www.youtube.com"

// A channel id: "UC" and 22 more characters of the base64url alphabet.
function isChannelId(id) {
    return typeof id === "string" && /^UC[0-9A-Za-z_-]{22}$/.test(id)
}

// A handle: "@" and 3 to 30 letters, digits, dots, hyphens or underscores. Handles in other
// scripts exist; they are left out here because the id is the stable address.
function isHandle(handle) {
    return typeof handle === "string" && /^@[0-9A-Za-z._-]{3,30}$/.test(handle)
}

// The channel's address: by id when known, else by handle, else "".
function channel(id, handle) {
    if (isChannelId(id)) return BASE + "/channel/" + id
    if (isHandle(handle)) return BASE + "/" + handle
    return ""
}

// A public playlist id (PL…, UU…, OLAK5uy_…): 10 to 64 characters of the base64url alphabet.
// The two-letter private lists (WL, LL) belong to an account and are not shareable.
function isPlaylistId(id) {
    return typeof id === "string" && /^[0-9A-Za-z_-]{10,64}$/.test(id)
}

function playlist(id) {
    return isPlaylistId(id) ? BASE + "/playlist?list=" + id : ""
}

// A video id: 11 characters of the base64url alphabet.
function isVideoId(id) {
    return typeof id === "string" && /^[0-9A-Za-z_-]{11}$/.test(id)
}

// A Short's own address (the Shorts player opens it); "" for anything that is not a video id.
function shortVideo(id) {
    return isVideoId(id) ? BASE + "/shorts/" + id : ""
}

// Which parts of a playlist's head show (PlaylistHeader): the cover needs a picture, Play all
// needs a video, Shuffle needs two and a queue to put them in.
function headParts(o) {
    var count = o && o.count > 0 ? o.count : 0
    return {
        cover: !!(o && o.coverUrl),
        play: count > 0,
        shuffle: count > 1 && !!(o && o.queue)
    }
}

// A shuffled copy of `list` (Fisher-Yates). `random` returns a number in [0, 1); tests pass
// their own. The input is left as it was.
function shuffled(list, random) {
    var out = [], i
    for (i = 0; i < (list ? list.length : 0); ++i) out.push(list[i])
    var rnd = typeof random === "function" ? random : Math.random
    for (i = out.length - 1; i > 0; --i) {
        var j = Math.floor(rnd() * (i + 1))
        if (j > i) j = i
        if (j < 0) j = 0
        var t = out[i]; out[i] = out[j]; out[j] = t
    }
    return out
}
