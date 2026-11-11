// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
.pragma library
// App shell decisions: the main page's sections, the landing section (DSC-04, SET-01),
// the in-app mini-player (PLY-11) and pulley limits (PLT-06). Pure ES5 (NFR-TST-04).

// Main page sections, in the bottom bar's order (the user's order, docs/DESIGN.md §2.2). Each one
// is a lazily loaded file in qml/sections/.
var SECTIONS = ["home", "shorts", "subscriptions", "library", "search", "downloads"]

var SECTION_FILES = {
    home: "HomeSection.qml",
    shorts: "ShortsSection.qml",
    subscriptions: "SubscriptionsSection.qml",
    library: "LibrarySection.qml",
    search: "SearchSection.qml",
    downloads: "DownloadsSection.qml"
}

// "Landing page" setting values (SET-01, DSC-04).
var LANDING_SUBSCRIPTIONS = "subscriptions"
var LANDING_RECOMMENDED = "recommended"
var LANDING_SEARCH = "search"

// What a tap on a bottom-bar entry does. Every entry is a section of the main page; from a page
// pushed above it the stack returns to the main page first (as the official apps do).
function barTarget(id, mainPageOnTop) {
    if (SECTIONS.indexOf(id) < 0) return { kind: "none" }
    return { kind: mainPageOnTop ? "section" : "return", id: id }
}

// ---- Bottom navigation bar (docs/DESIGN.md §2.2) ----

// Whether the bar shows. state:
//   watchPageOnTop: the watch page is current (it is immersive content, YouTube-like)
//   pageOptOut: the current page declares navigationBarOptOut (settings, forms) or is a Dialog
//   keyboardOpen: the virtual keyboard is shown (the bar would sit above it)
//   immersive: something is fullscreen
function navigationBarVisible(state) {
    if (state.watchPageOnTop || state.pageOptOut || state.immersive) return false
    return !state.keyboardOpen
}

// Label policy for N entries of width `entryWidth`: "all" when the widest label fits, otherwise
// "current" (icons for all, the label under the current entry only, so six entries still fit a
// 540 px phone or the largest font).
function navigationLabels(entryWidth, widestLabel, padding) {
    if (!(entryWidth > 0)) return "current"
    return widestLabel + 2 * (padding > 0 ? padding : 0) <= entryWidth ? "all" : "current"
}

// A pulley menu holds at most this many items (PLT-06).
var PULLEY_LIMIT = 5

function sectionIndex(id) {
    var i = SECTIONS.indexOf(id)
    return i < 0 ? 0 : i
}

function sectionFile(id) {
    return SECTION_FILES[id] || SECTION_FILES.subscriptions
}

// The section the main page opens on (DSC-04). Home is useful without an account (Popular and
// Continue watching), so the "Recommended" choice is honoured signed out too. The default is
// Subscriptions, but a user without subscriptions would meet an empty list there, which is the
// first thing a new user would see: they land on Home instead, until they subscribe to something.
// subscriptionCount: the number of subscribed channels when the app starts.
function landingSection(setting, subscriptionCount) {
    if (setting === LANDING_SEARCH) return "search"
    if (setting === LANDING_RECOMMENDED) return "home"
    return subscriptionCount > 0 ? "subscriptions" : "home"
}

// Which sections the main page instantiates: the current one, plus those already visited, so
// first frame only pays for one section (NFR-PERF-01) and switching back keeps scroll state.
function loadedSections(current, visited) {
    var out = {}
    var i
    for (i = 0; i < SECTIONS.length; ++i) out[SECTIONS[i]] = false
    if (visited) for (i = 0; i < visited.length; ++i) if (out.hasOwnProperty(visited[i])) out[visited[i]] = true
    if (out.hasOwnProperty(current)) out[current] = true
    return out
}

// Adds a section to a visited list without duplicates; returns a new array (list models and
// bindings must see a new value).
function visit(visited, id) {
    var out = (visited || []).slice(0)
    if (SECTIONS.indexOf(id) >= 0 && out.indexOf(id) < 0) out.push(id)
    return out
}

// Number of visible items a pulley will show; items is an array of booleans (visible flags).
function visiblePulleyItems(items) {
    var n = 0
    for (var i = 0; i < items.length; ++i) if (items[i]) ++n
    return n
}

// ---- In-app mini-player (PLY-11) ----

// state:
//   hasSession: a video is loaded in the shared player (playing, paused or frozen)
//   dismissed: the user closed the bar for this video
//   watchPageOnTop: the watch page is the current page
//   pageOptOut: the current page has its own bottom controls (or opts out explicitly)
//   keyboardOpen: the virtual keyboard is shown
function miniPlayerVisible(state) {
    if (!state.hasSession || state.dismissed) return false
    if (state.watchPageOnTop || state.pageOptOut) return false
    return !state.keyboardOpen
}

// Fraction for the thin progress line; 0 when unknown, live, or before playback starts.
function progress(position, duration, live) {
    if (live || !(duration > 0) || !(position > 0)) return 0
    var f = position / duration
    return f > 1 ? 1 : f
}

// The next button is shown only with a queue that has an item after the current one.
function hasNext(queueLength, currentIndex) {
    return queueLength > 0 && currentIndex >= 0 && currentIndex < queueLength - 1
}

// What tapping the bar body does: re-attach to the live player when the same video is still
// loaded; otherwise reopen it (the playback component decides whether a resolve is needed).
function tapAction(sessionVideoId, barVideoId) {
    if (!barVideoId) return "none"
    return sessionVideoId === barVideoId ? "reattach" : "reopen"
}

// Playing a list as the queue (PLY-25): which ids go to the session's queue and which row opens
// first. Rows without a video id are left out; `index` counts the rows as given and falls back
// to the first playable one. An empty plan (no ids) plays nothing.
function queuePlan(list, index) {
    var ids = [], rows = [], at = 0
    for (var i = 0; i < (list ? list.length : 0); ++i) {
        var row = list[i]
        if (!row || !row.videoId) continue
        if (i === index) at = ids.length
        ids.push(row.videoId)
        rows.push(row)
    }
    return { ids: ids, at: at, first: rows.length > 0 ? rows[at] : null }
}
