// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
.pragma library
// List row geometry shared by VideoListItem and the loading skeleton (docs/DESIGN.md §4.1, §5.1),
// and the list state a view shows while it has no rows (NFR-PERF-01: never a blank page).
// Pure ES5 (NFR-TST-04).

// The thumbnail takes this share of the row width in portrait.
var THUMB_SHARE = 0.42

// Thumbnail width for a row. In landscape the row is much wider than the portrait screen, so the
// thumbnail keeps its portrait size instead of growing (and making every row very tall).
function thumbWidth(rowWidth, screenShort) {
    var base = rowWidth
    if (screenShort > 0 && screenShort < base) base = screenShort
    return Math.round(Math.max(0, base) * THUMB_SHARE)
}

function thumbHeight(width) {
    return Math.round(width * 9 / 16)
}

// Skeleton rows needed to cover a view of `viewHeight`; at least one, at most `cap` (12).
function skeletonCount(viewHeight, rowHeight, cap) {
    var limit = cap > 0 ? cap : 12
    if (!(rowHeight > 0) || !(viewHeight > 0)) return 1
    var n = Math.ceil(viewHeight / rowHeight)
    return n < 1 ? 1 : (n > limit ? limit : n)
}

// What a list without rows shows. Exactly one of "rows", "skeleton" or "placeholder", so the
// page is never blank and a loading list never says "empty":
//   count: rows in the model; busy: a load is running; pending: a first load is about to start
//   (for example the view was just created and the model has not reported busy yet).
function emptyState(count, busy, pending) {
    if (count > 0) return "rows"
    if (busy || pending) return "skeleton"
    return "placeholder"
}

// The failed-list shortcut to the downloads (docs/DESIGN.md §2.2, §5.3): a list that has no rows
// because loading failed offers "Open downloads" when something is saved on the device, as the
// official apps do when offline.
function showDownloadsShortcut(state, errorText, downloadCount) {
    return state === "placeholder" && !!errorText && downloadCount > 0
}

// Card layout (docs/DESIGN.md §4.1, YouTube Android's home and feed): a full-width thumbnail with
// the avatar, title and meta under it. Only on portrait-wide rows of lists that opt in; landscape
// and narrow pages keep the compact row, where a full-width picture would fill the screen.
function useCard(optIn, rowWidth, screenShort) {
    if (!optIn || !(rowWidth > 0)) return false
    var limit = screenShort > 0 ? screenShort * 1.05 : rowWidth
    return rowWidth <= limit
}

// The meta line of a card: "channel · 1.2M views · 3 days ago" from the parts a row has. Parts
// already joined by the model (the feed's "channel · age") are kept as they are.
function cardMeta(channelLine, viewsText) {
    var parts = []
    if (channelLine) parts.push(channelLine)
    if (viewsText) parts.push(viewsText)
    return parts.join(" · ")
}

// The letter on an avatar placeholder: the first letter of the channel name, upper case.
function initial(name) {
    var s = (name || "").replace(/^[\s@]+/, "")
    return s.length ? s.charAt(0).toUpperCase() : ""
}

// E12 prefetch (VisibleRows): a row counts as on screen when at least half of its height is
// inside the viewport. "Fully visible" was right for compact rows; cards are about three times
// taller, so the half-visible third card a user can already tap would never be reported.
function rowOnScreen(itemTop, itemBottom, viewTop, viewBottom) {
    var h = itemBottom - itemTop
    if (!(h > 0)) return false
    var shown = Math.min(itemBottom, viewBottom) - Math.max(itemTop, viewTop)
    return shown >= h / 2
}

// Decode width for a card's thumbnail. With a large source (catalog's thumbnailLarge) the
// picture decodes at the card's width; the list thumbnail (320 px wide) is never blown up past
// 640 px on the CPU, the GPU scales the rest.
function decodeWidth(displayWidth, hasLarge) {
    if (!(displayWidth > 0)) return 0
    if (hasLarge) return Math.round(displayWidth)
    return Math.round(displayWidth < 640 ? displayWidth : 640)
}

// Card height for a row width (thumbnail, gap, the avatar/text band, bottom gap), for skeletons
// and estimates; `band` is the avatar's size.
function cardHeight(rowWidth, gap, band, bottomGap) {
    return thumbHeight(rowWidth) + gap + band + bottomGap
}

// ---- Home's "Continue watching" shelf (docs/DESIGN.md §4.9) ----

// The videos left unfinished, from history rows given newest first ({ videoId, fraction,
// watched }): started (at least 5 % in) and not marked watched. At most `max`, in the order given.
function continueWatching(rows, max) {
    var out = [], limit = max > 0 ? max : 10
    for (var i = 0; i < (rows ? rows.length : 0) && out.length < limit; ++i) {
        var r = rows[i]
        if (!r || !r.videoId || r.watched === true) continue
        if (!(r.fraction >= 0.05) || r.fraction >= 1) continue
        out.push(r)
    }
    return out
}

// A short text that changes when the shelf's content does (ids and whole percents), so the shelf
// is rebuilt only then and keeps its scroll position otherwise.
function continueKey(rows) {
    var parts = []
    for (var i = 0; i < (rows ? rows.length : 0); ++i)
        parts.push(rows[i].videoId + ":" + Math.round(rows[i].fraction * 100))
    return parts.join(",")
}
