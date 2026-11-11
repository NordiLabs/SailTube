// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SET-01 inventory and defaults, SET-02 one place per control.
var I = modules.SettingsInventory

test("SET-01: spec defaults", function () {
    eq(I.defaultValue("content/landingPage"), "subscriptions")
    eq(I.defaultValue("content/hideShorts"), true)
    eq(I.defaultValue("content/hideWatched"), false)
    eq(I.defaultValue("history/cap"), 500)
    eq(I.defaultValue("history/paused"), false)
    eq(I.defaultValue("playback/resume"), "always")
    eq(I.defaultValue("playback/maxHeight"), 0)   // Automatic (SEL-02)
    eq(I.defaultValue("playback/maxHeightMobile"), 480)
    eq(I.defaultValue("playback/hardwareDecode"), false)
    eq(I.defaultValue("playback/keepDisplayOn"), true)
    eq(I.defaultValue("playback/background"), true)
    eq(I.defaultValue("playback/verticalFullscreen"), "auto")
    eq(I.defaultValue("playback/edgeGlow"), "auto")
    eq(I.defaultValue("playback/autoplayRelated"), true)
    eq(I.defaultValue("comments/autoLoad"), "near")
    eq(I.defaultValue("playback/doubleTapSeconds"), 10)
    eq(I.defaultValue("playback/keepSessionOnLeave"), true)
    eq(I.defaultValue("playback/coverVideo"), true)
    eq(I.defaultValue("playback/dubLanguage"), "")
    eq(I.defaultValue("playback/stableVolume"), false)
    eq(I.defaultValue("playback/resumeAfterInterruption"), true)
    eq(I.defaultValue("sponsorblock/enabled"), true)
    eq(I.defaultValue("sponsorblock/sponsor"), 1)
    eq(I.defaultValue("sponsorblock/intro"), 0)
    eq(I.defaultValue("sponsorblock/poi_highlight"), 0)
    eq(I.defaultValue("audio/boost"), 100)
    eq(I.defaultValue("audio/loudness"), false)
    eq(I.defaultValue("integrations/ryd"), false)
    eq(I.defaultValue("providers/extractorChannel"), "stable")
    eq(I.defaultValue("providers/poProvider"), false)
    eq(I.defaultValue("providers/clientOverride"), "auto")
    eq(I.defaultValue("diagnostics/timingLog"), false)
})

test("keys are unique and every entry is well formed", function () {
    var seen = {}
    for (var i = 0; i < I.ENTRIES.length; ++i) {
        var e = I.ENTRIES[i]
        ok(!seen[e.key], "duplicate " + e.key); seen[e.key] = true
        ok(/^(switch|choice|number|page|remembered)$/.test(e.kind), e.key + " kind")
        if (e.kind === "choice") ok(e.values.indexOf(e.def) >= 0, e.key + " default among values")
        if (e.kind === "number") ok(e.def >= e.min && e.def <= e.max && (e.def - e.min) % e.step === 0, e.key + " default in range")
        if (e.kind !== "remembered") ok(I.SECTIONS.indexOf(e.section) >= 0, e.key + " section")
    }
})

test("SET-02: every key has exactly one location", function () {
    for (var i = 0; i < I.ENTRIES.length; ++i) {
        var key = I.ENTRIES[i].key
        // A hidden row (no engine reads the key yet) has no location, so nothing links to it.
        eq(I.locate(key) !== null, !I.isHidden(key), key)
    }
    deepEq(I.locate("playback/edgeGlow"), { page: "settings", section: "playback" })
    deepEq(I.locate("playback/speed"), { page: "player", section: "" })
    deepEq(I.locate("sponsorblock/filler"), { page: "settings", section: "sponsorblock" })
    eq(I.locate("nope"), null)
    // Each entry is listed in exactly one (page, section) row set.
    var count = {}
    var pages = ["settings", "player", "diagnostics"]
    var sections = I.SECTIONS.concat([""])
    for (var p = 0; p < pages.length; ++p) for (var s = 0; s < sections.length; ++s) {
        var full = I.rows(pages[p], sections[s], "FULL")
        for (var r = 0; r < full.length; ++r) count[full[r].key] = (count[full[r].key] || 0) + 1
    }
    for (var j = 0; j < I.ENTRIES.length; ++j) {
        var k = I.ENTRIES[j].key
        // A hidden row is in no row set; a remembered key has no row.
        if (I.isHidden(k)) eq(count[k], undefined, k + " is hidden")
        else if (I.ENTRIES[j].kind !== "remembered") eq(count[k], 1, k)
    }
})

test("STORE builds hide FULL-only rows", function () {
    var full = I.rows("settings", "providers", "FULL"), store = I.rows("settings", "providers", "STORE")
    // PO provider and PO heap ceiling; the extractor channel is hidden until the updater has a UI.
    eq(full.length, 3 - (I.isHidden("providers/extractorChannel") ? 1 : 0)); eq(store.length, 0)
})

test("choice index falls back to the default for stale values", function () {
    eq(I.choiceIndex("playback/edgeGlow", "hidden"), 1)
    eq(I.choiceIndex("playback/edgeGlow", "bogus"), 0)
    eq(I.choiceIndex("playback/maxHeight", 999), 0)   // the default, Automatic
    eq(I.choiceIndex("nope", 1), -1)
})

test("numbers clamp to range and step", function () {
    eq(I.clampNumber("history/cap", 50), 100); eq(I.clampNumber("history/cap", 5000), 2000)
    eq(I.clampNumber("history/cap", 549), 500); eq(I.clampNumber("history/cap", 551), 600)
    eq(I.clampNumber("history/cap", "x"), 500); eq(I.clampNumber("audio/boost", 123), 120)
})

test("PLT-06: the first combo write is not a user change", function () {
    ok(!I.isUserChange(false, 1, 2)); ok(!I.isUserChange(true, 2, 2)); ok(I.isUserChange(true, 1, 2)); ok(!I.isUserChange(true, 1, -1))
})

test("hidden rows: in the inventory, without a location or an error link", function () {
    for (var i = 0; i < I.HIDDEN_KEYS.length; ++i) {
        var key = I.HIDDEN_KEYS[i]
        ok(I.entry(key) !== null, key + " stays in the inventory (SET-01)")
        eq(I.locate(key), null, key)
        eq(I.ERROR_LINK_KEYS.indexOf(key), -1, key + " has no error link while hidden")
    }
    ok(I.isHidden("providers/extractorChannel"), "beta 3: nothing reads the extractor channel")
    ok(I.isHidden("audio/equalizer"), "beta 3: the equalizer has no page or preset key")
    ok(!I.isHidden("playback/maxHeight"))
})
