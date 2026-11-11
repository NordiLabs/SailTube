// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
import QtQuick 2.6
import Sailfish.Silica 1.0
import "../../Clients/Sailfish/qml/logic/SettingsInventory.js" as Inventory

// SET-02: every setting an error may link to opens the page that holds exactly that control,
// and the watch page's error state links to the key the player names.
UiTest {
    id: t
    property int index: 0
    property var pb: mocks.playback
    function openNext() {
        window.openSetting(Inventory.ERROR_LINK_KEYS[index])
        return 700
    }
    function checkAndClose() {
        var key = Inventory.ERROR_LINK_KEYS[index]
        var page = current()
        compare(page.focusKey, key, "settings page opened for " + key)
        compare(findAll(page, function (c) { return c.settingKey === key }).length, 1, key + " control on that page")
        stack().pop(undefined, PageStackAction.Immediate)
        index++
        return 200
    }
    property var linkSteps: {
        var out = []
        for (var i = 0; i < Inventory.ERROR_LINK_KEYS.length; ++i) { out.push(openNext); out.push(checkAndClose) }
        return out
    }
    steps: linkSteps.concat([
        function () {
            window.openVideo("e1", { title: "Errors" })
            pb.errorSettingKey = "playback/maxHeight"
            pb.fail("No stream at or below the chosen quality")
            return 700
        },
        function () {
            var button = find(current(), "settingKey", "playback/maxHeight")
            verify(button !== null && button.visible, "Open setting button shown")
            button.clicked(null)
            return 800
        },
        function () {
            compare(current().focusKey, "playback/maxHeight", "error link opened the setting")
            compare(Inventory.locate("playback/maxHeight").page, "settings")
        }
    ])
}
