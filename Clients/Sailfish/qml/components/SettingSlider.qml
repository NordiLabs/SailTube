// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
import QtQuick 2.6
import Sailfish.Silica 1.0
import "../logic/SettingsInventory.js" as Inventory

// A slider over one numeric inventory key: range and step from the inventory, saved on release.
Slider {
    id: control
    property string settingKey
    // False for a row that is hidden until an engine reads its key (SettingsInventory.HIDDEN_KEYS).
    // A page that sets `visible` itself adds `&& inUse`.
    readonly property bool inUse: !Inventory.isHidden(settingKey)
    visible: inUse
    // Where the value lives: the app settings, or an adapter with revision, value(key, fallback)
    // and setValue(key, value) for a key another component stores (SET-02: one control per key).
    property var store: appSettings
    property string unit: ""
    property bool flash: false
    readonly property var entry: Inventory.entry(settingKey)
    readonly property real stored: (control.store.revision, Number(control.store.value(settingKey, Inventory.defaultValue(settingKey))))

    width: parent ? parent.width : implicitWidth
    minimumValue: entry ? entry.min : 0
    maximumValue: entry ? entry.max : 1
    stepSize: entry ? entry.step : 1
    valueText: Math.round(value) + unit
    highlighted: down || flash
    onReleased: control.store.setValue(settingKey, Inventory.clampNumber(settingKey, value))
    Binding { target: control; property: "value"; value: control.stored; when: !control.down }
}
