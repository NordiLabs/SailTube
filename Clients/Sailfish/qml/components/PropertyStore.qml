// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
import QtQuick 2.6

// A settings store over one property of another component's object (SET-02: the settings row is the
// one control, the owning object keeps the value). `fromTarget` and `toTarget` map between the
// shared table's value and the property's own representation.
QtObject {
    id: store
    property QtObject target: null
    property string property: ""
    property var fromTarget: function (v) { return v }
    property var toTarget: function (v) { return v }

    readonly property var current: target ? target[property] : undefined
    // Controls read `revision` to re-evaluate; a binding (not a counter) avoids a binding loop.
    readonly property var revision: current

    function value(key, fallback) {
        return target && current !== undefined ? fromTarget(current) : fallback
    }
    function setValue(key, v) {
        if (target) target[property] = toTarget(v)
    }
}
