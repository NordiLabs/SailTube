// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
import QtQuick 2.6
import Sailfish.Silica 1.0
import "../logic/SettingsInventory.js" as Inventory

// A switch bound to one inventory key (SET-01). Reads through the app settings with the
// inventory default; writes immediately (SET-02: settings apply at once).
TextSwitch {
    id: control
    property string settingKey
    // False for a row that is hidden until an engine reads its key (SettingsInventory.HIDDEN_KEYS).
    // A page that sets `visible` itself adds `&& inUse`.
    readonly property bool inUse: !Inventory.isHidden(settingKey)
    visible: inUse
    // Where the value lives: the app settings, or an adapter with revision, value(key, fallback)
    // and setValue(key, value) for a key another component stores (SET-02: one control per key).
    property var store: appSettings
    property bool flash: false
    readonly property bool stored: (control.store.revision, control.store.value(settingKey, Inventory.defaultValue(settingKey)) === true)

    checked: stored
    automaticCheck: false
    onClicked: control.store.setValue(settingKey, !stored)
    highlighted: down || flash
}
