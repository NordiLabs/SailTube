// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
import QtQuick 2.6
import Sailfish.Silica 1.0
import "../../Clients/Sailfish/qml/logic/SettingsInventory.js" as Inventory

// SET-01/02 at runtime: every inventory key has exactly one control on the settings page,
// opening the page writes nothing (combo first bind, PLT-06), a change writes once, and
// openSetting() scrolls to and flashes the control.
UiTest {
    id: t
    property var page
    property var settings: mocks.appSettings
    steps: [
        function () { stack().push(Qt.resolvedUrl("../../Clients/Sailfish/qml/pages/SettingsPage.qml"), {}, PageStackAction.Immediate); return 500 },
        function () {
            page = current()
            compare(settings.writes.length, 0, "opening the settings page writes nothing: " + settings.writes.join(","))
            for (var i = 0; i < Inventory.ENTRIES.length; ++i) {
                var e = Inventory.ENTRIES[i]
                var controls = findAll(page, function (c) { return c.settingKey === e.key })
                if (e.kind === "remembered") compare(controls.length, 0, e.key + " has no row")
                else compare(controls.length, 1, e.key + " has exactly one control")
            }
            // No dead switch ships: a hidden key keeps its control (SET-02), invisible.
            for (var h = 0; h < Inventory.HIDDEN_KEYS.length; ++h)
                verify(!find(page, "settingKey", Inventory.HIDDEN_KEYS[h]).visible, Inventory.HIDDEN_KEYS[h] + " is hidden")
            verify(find(page, "settingKey", "providers/poProvider").visible, "rows in use stay visible")
            verify(find(page, "settingKey", "playback/hardwareDecode").enabled,
                   "hardware decoding stays switchable while the probe demotes the decoder")
            var glow = find(page, "settingKey", "playback/edgeGlow")
            compare(glow.currentIndex, 0, "default shown")
            glow.currentIndex = 1
            return 100
        },
        function () {
            compare(settings.writes.join(","), "playback/edgeGlow=hidden", "one write for one change")
            var sw = find(page, "settingKey", "content/hideShorts")
            verify(sw.checked, "hide Shorts on by default")
            sw.clicked(null)
            return 100
        },
        function () {
            compare(settings.value("content/hideShorts", true), false, "switch wrote false")
            verify(!find(page, "settingKey", "content/hideShorts").checked, "switch follows the store")
            stack().pop(undefined, PageStackAction.Immediate)
            window.openSetting("diagnostics/timingLog")
            return 800
        },
        function () {
            page = current()
            compare(page.focusKey, "diagnostics/timingLog")
            var control = find(page, "settingKey", "diagnostics/timingLog")
            verify(control.flash, "target control flashes")
            var flick = control.parent
            while (flick && !flick.hasOwnProperty("contentY")) flick = flick.parent
            verify(flick.contentY > 0, "scrolled to the control: contentY " + flick.contentY + " y " + control.mapToItem(flick.contentItem, 0, 0).y + " h " + flick.height + " ch " + flick.contentHeight)
            return 1800
        },
        function () {
            verify(!find(page, "settingKey", "diagnostics/timingLog").flash, "flash ends")
            // SET-02: rows for keys another component stores write that component's object, not appSettings.
            var before = settings.writes.length
            find(page, "settingKey", "history/paused").clicked(null)
            find(page, "settingKey", "playback/resume").currentIndex = 2
            find(page, "settingKey", "content/hideWatched").clicked(null)
            return 100
        },
        function () {
            compare(mocks.historyModel.paused, true, "pause history writes the library's history model")
            compare(mocks.libraryPlayback.resumeBehaviour, 2, "resume Never writes the library's bridge")
            compare(mocks.watchIndex.hideWatched, true, "hide watched writes the library's watch index")
            verify(find(page, "settingKey", "history/paused").checked, "switch reads the library's value back")
            compare(find(page, "settingKey", "playback/resume").currentIndex, 2, "combo reads the library's value back")
            var leaked = settings.writes.filter(function (w) {
                return w.indexOf("history/") === 0 || w.indexOf("playback/resume") === 0 || w.indexOf("content/hideWatched") === 0 })
            compare(leaked.length, 0, "nothing written to appSettings for library keys: " + leaked.join(","))
            verify(find(page, "settingKey", "downloads/folder").available, "download folder row opens its page")
        }
    ]
}
