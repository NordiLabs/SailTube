// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
import QtQuick 2.6
import Sailfish.Silica 1.0
import "../logic/SettingsInventory.js" as Inventory

// A combo box over one inventory key's values; `labels` gives the visible text for each value
// in inventory order. The first write a ComboBox makes while binding is suppressed (PLT-06).
ComboBox {
    id: control
    property string settingKey
    // False for a row that is hidden until an engine reads its key (SettingsInventory.HIDDEN_KEYS).
    // A page that sets `visible` itself adds `&& inUse`.
    readonly property bool inUse: !Inventory.isHidden(settingKey)
    visible: inUse
    // Where the value lives: the app settings, or an adapter with revision, value(key, fallback)
    // and setValue(key, value) for a key another component stores (SET-02: one control per key).
    property var store: appSettings
    property var labels: []
    property bool flash: false
    property bool _ready: false
    readonly property var entry: Inventory.entry(settingKey)
    readonly property int storedIndex: (control.store.revision,
        Inventory.choiceIndex(settingKey, control.store.value(settingKey, Inventory.defaultValue(settingKey))))

    currentIndex: storedIndex
    highlighted: down || flash
    menu: ContextMenu {
        Repeater {
            model: control.labels
            MenuItem { text: modelData }
        }
    }
    onCurrentIndexChanged: {
        if (Inventory.isUserChange(_ready, storedIndex, currentIndex))
            control.store.setValue(settingKey, entry.values[currentIndex])
    }
    Component.onCompleted: _ready = true
}
