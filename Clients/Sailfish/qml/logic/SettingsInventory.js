// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
.pragma library
// Settings inventory (SET-01) and where each control lives (SET-02). Pure ES5 (NFR-TST-04).
//
// Every setting has exactly one home: a page and a section. Error messages that refer to a
// setting call locate(key) and open that page with the control focused, so a pointer can
// never go stale. "remembered" settings have no settings row: the player remembers the last
// choice (speed, caption language), and "variant" restricts a row to one build variant.
//
// Defaults: the single source is Core/Settings/Domain/SettingsDefaults.cpp (SET-03), which the
// engine reads through AppSettings. The literals below are the UI's copy; tests/settings fails
// if any differs from the table (NFR-TST-03). Do not duplicate them anywhere else in the UI.

var PAGE_SETTINGS = "settings"
var PAGE_PLAYER = "player"           // remembered from the player, no settings row
var PAGE_SPONSORBLOCK = "sponsorblock"
var PAGE_EQUALIZER = "equalizer"
var PAGE_CAPTIONS = "captions"
var PAGE_DOWNLOADS = "downloads"
var PAGE_DIAGNOSTICS = "diagnostics"

// Section ids of the main settings page, in display order.
var SECTIONS = ["content", "history", "playback", "quality", "audio", "integrations", "sponsorblock", "downloads",
                "providers", "diagnostics"]

// kind: "switch", "choice" (values), "number" (min/max/step), "page" (opens a sub-page),
//       "remembered" (no row)
var ENTRIES = [
    { key: "content/landingPage", section: "content", kind: "choice", def: "subscriptions",
      values: ["subscriptions", "recommended", "search"], req: "SET-01, DSC-04" },
    { key: "content/hideShorts", section: "content", kind: "switch", def: true, req: "SET-01" },
    { key: "content/hideWatched", section: "content", kind: "switch", def: false, req: "SET-01" },
    { key: "history/cap", section: "history", kind: "number", def: 500, min: 100, max: 2000, step: 100,
      req: "SET-01" },
    { key: "history/paused", section: "history", kind: "switch", def: false, req: "SET-01" },
    { key: "playback/resume", section: "playback", kind: "choice", def: "always",
      values: ["always", "ask", "never"], req: "SET-01" },
    { key: "playback/keepDisplayOn", section: "playback", kind: "switch", def: true, req: "SET-01, PLY-12" },
    { key: "playback/background", section: "playback", kind: "switch", def: true, req: "SET-01" },
    { key: "playback/verticalFullscreen", section: "playback", kind: "choice", def: "auto",
      values: ["auto", "landscape"], req: "SET-01, PLY-13" },
    { key: "playback/edgeGlow", section: "playback", kind: "choice", def: "auto",
      values: ["auto", "hidden"], req: "SET-01, PLY-16" },
    { key: "playback/autoplayRelated", section: "playback", kind: "switch", def: true, req: "SET-01" },
    { key: "playback/keepSessionOnLeave", section: "playback", kind: "switch", def: true, req: "PLY-10" },
    { key: "playback/coverVideo", section: "playback", kind: "switch", def: true, variant: "FULL",
      req: "cover video (user decision)" },
    { key: "playback/resumeAfterInterruption", section: "playback", kind: "switch", def: true, req: "PLY-23" },
    { key: "playback/doubleTapSeconds", section: "playback", kind: "choice", def: 10,
      values: [5, 10, 15], req: "SET-01" },
    { key: "playback/speed", section: "", kind: "remembered", def: 1.0, page: PAGE_PLAYER, req: "SET-01" },
    { key: "playback/dubLanguage", section: "", kind: "remembered", def: "", page: PAGE_PLAYER, req: "SEL-05" },
    { key: "comments/autoLoad", section: "playback", kind: "choice", def: "near",
      values: ["near", "always", "never"], req: "SET-01" },
    { key: "playback/maxHeight", section: "quality", kind: "choice", def: 0,
      values: [0, 2160, 1440, 1080, 720, 480, 360], req: "SET-01" },
    { key: "playback/maxHeightMobile", section: "quality", kind: "choice", def: 480,
      values: [0, 2160, 1440, 1080, 720, 480, 360], req: "SET-01" },
    { key: "playback/hardwareDecode", section: "quality", kind: "switch", def: false, req: "SET-01, PLY-21" },
    { key: "playback/codecPreference", section: "quality", kind: "choice", def: 0, values: [0, 1, 2],
      req: "core slice" },
    { key: "playback/highFrameRate", section: "quality", kind: "switch", def: true, req: "core slice" },
    { key: "playback/adaptivePipeline", section: "quality", kind: "choice", def: 1, values: [0, 1, 2],
      req: "core slice" },
    { key: "audio/equalizer", section: "audio", kind: "page", def: false, page: PAGE_EQUALIZER, req: "SET-01" },
    { key: "audio/boost", section: "audio", kind: "number", def: 100, min: 100, max: 500, step: 10,
      req: "SET-01" },
    { key: "audio/loudness", section: "audio", kind: "switch", def: false, req: "SET-01" },
    { key: "playback/stableVolume", section: "audio", kind: "switch", def: false, req: "playback component" },
    { key: "captions/style", section: "audio", kind: "page", def: "", page: PAGE_CAPTIONS, req: "SET-01" },
    { key: "sponsorblock/enabled", section: "sponsorblock", kind: "switch", def: true, req: "SET-01, SB-02" },
    { key: "sponsorblock/sponsor", section: "sponsorblock", kind: "choice", def: 1, values: [0, 1, 2], req: "SB-02" },
    { key: "sponsorblock/selfpromo", section: "sponsorblock", kind: "choice", def: 1, values: [0, 1, 2], req: "SB-02" },
    { key: "sponsorblock/interaction", section: "sponsorblock", kind: "choice", def: 1, values: [0, 1, 2], req: "SB-02" },
    { key: "sponsorblock/intro", section: "sponsorblock", kind: "choice", def: 0, values: [0, 1, 2], req: "SB-02" },
    { key: "sponsorblock/outro", section: "sponsorblock", kind: "choice", def: 0, values: [0, 1, 2], req: "SB-02" },
    { key: "sponsorblock/preview", section: "sponsorblock", kind: "choice", def: 0, values: [0, 1, 2], req: "SB-02" },
    { key: "sponsorblock/music_offtopic", section: "sponsorblock", kind: "choice", def: 0, values: [0, 1, 2], req: "SB-02" },
    { key: "sponsorblock/filler", section: "sponsorblock", kind: "choice", def: 0, values: [0, 1, 2], req: "SB-02" },
    { key: "sponsorblock/poi_highlight", section: "sponsorblock", kind: "choice", def: 0, values: [0, 2], req: "SB-02" },
    { key: "integrations/ryd", section: "integrations", kind: "switch", def: false, req: "SET-01" },
    { key: "downloads/folder", section: "downloads", kind: "page", def: "", page: PAGE_DOWNLOADS, req: "SET-01" },
    { key: "downloads/quality", section: "downloads", kind: "choice", def: 720,
      values: [1080, 720, 480, 360, -1], req: "SET-01" },
    { key: "downloads/unmeteredOnly", section: "downloads", kind: "switch", def: true, req: "SET-01" },
    { key: "providers/extractorChannel", section: "providers", kind: "choice", def: "stable",
      values: ["stable", "nightly"], variant: "FULL", req: "SET-01" },
    { key: "providers/poProvider", section: "providers", kind: "switch", def: false, variant: "FULL",
      req: "SET-01" },
    { key: "providers/poTokenHeapMb", section: "providers", kind: "number", def: 512, min: 128, max: 2048, step: 64,
      variant: "FULL", req: "SET-01, RES-14" },
    { key: "providers/clientOverride", section: "diagnostics", kind: "choice", def: "auto",
      values: ["auto"], req: "SET-01" },
    { key: "diagnostics/timingLog", section: "diagnostics", kind: "switch", def: false, req: "SET-01, DIA-03" },
    { key: "diagnostics/startLowRung", section: "diagnostics", kind: "switch", def: false, req: "SET-01, NFR-PERF-02" }
]

// Settings an error message may link to (SET-02). The player reports one of these keys as
// `playback.errorSettingKey`; the watch page shows "Open setting", which calls
// window.openSetting(key). tests/qml_ui/tst_errorlinks.qml opens every one.
// "playback/audioOnly" is not one of them: listen-only is per session (SEL-08) and has no row;
// playback reports it when only the video mode would play, and the watch page offers
// "Play video" (setAudioOnlyMode(false) + retry) instead of "Open setting".
var ERROR_LINK_KEYS = [
    "playback/maxHeight",          // no stream at or below the quality ceiling
    "playback/maxHeightMobile",    // the same on mobile data
    "playback/adaptivePipeline",   // only combined streams allowed
    "playback/hardwareDecode",     // hardware decoder failed or stalled
    "playback/codecPreference"     // preferred codec not available
    // "providers/extractorChannel" (resolver outdated, [FULL]) returns with its row: see HIDDEN_KEYS.
]

// Rows that are not shown because no engine reads the key yet (no dead switch ships). The key
// stays in the table (SET-01) and keeps its one control (SET-02); the control is invisible and
// locate() answers null, so nothing links to it. Remove a key here when its engine reads it.
var HIDDEN_KEYS = [
    // Beta 3: the extractor updater has no UI yet, so nothing reads
    // the channel (extractorUpdater.check(channel) has no caller).
    "providers/extractorChannel",
    // Beta 3: the equalizer has no page behind its row
    // and no stored preset key.
    "audio/equalizer"
]

function isHidden(key) { return HIDDEN_KEYS.indexOf(key) >= 0 }

function entry(key) {
    for (var i = 0; i < ENTRIES.length; ++i) if (ENTRIES[i].key === key) return ENTRIES[i]
    return null
}

function defaultValue(key) {
    var e = entry(key)
    return e ? e.def : undefined
}

// Where the control for a setting lives: { page, section } or null for an unknown key.
// Choice rows live on the main settings page unless they name a page; "page" rows live on the
// main settings page too (the row opens the sub-page).
function locate(key) {
    var e = entry(key)
    if (!e || isHidden(key)) return null
    if (e.kind === "remembered") return { page: e.page || PAGE_PLAYER, section: "" }
    if (e.kind === "page") return { page: PAGE_SETTINGS, section: e.section, subPage: e.page }
    return { page: e.page || PAGE_SETTINGS, section: e.section }
}

// Entries shown in one section of a page, honouring the build variant ("FULL" or "STORE").
function rows(page, section, variant) {
    var out = []
    for (var i = 0; i < ENTRIES.length; ++i) {
        var e = ENTRIES[i]
        var where = locate(e.key)
        if (!where || where.page !== page || where.section !== section) continue
        if (e.variant && e.variant !== variant) continue
        out.push(e)
    }
    return out
}

// Index of a value in a choice row, or the default's index when the stored value is not
// allowed (a stale or hand-edited config). Combo boxes bind to this.
function choiceIndex(key, value) {
    var e = entry(key)
    if (!e || !e.values) return -1
    var i = e.values.indexOf(value)
    if (i < 0) i = e.values.indexOf(e.def)
    return i
}

// Clamp a number row to its range and step.
function clampNumber(key, value) {
    var e = entry(key)
    if (!e || e.kind !== "number") return value
    var v = Number(value)
    if (isNaN(v)) return e.def
    if (v < e.min) v = e.min
    if (v > e.max) v = e.max
    return e.min + Math.round((v - e.min) / e.step) * e.step
}

// Combo boxes write their value on first bind (PLT-06). A write is real only when the index
// differs from the stored value's index and the control has finished its initial binding.
function isUserChange(ready, storedIndex, newIndex) {
    return !!ready && newIndex >= 0 && newIndex !== storedIndex
}
