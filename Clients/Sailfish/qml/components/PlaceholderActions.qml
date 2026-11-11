// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
import QtQuick 2.6
import Sailfish.Silica 1.0

// Actions under a list's error placeholder (docs/DESIGN.md §2.2, §5.3): Retry when the list can
// reload, and "Open downloads" when loading failed (usually offline) and videos are saved on the
// device. Declared inside the SilicaListView next to its ViewPlaceholder, so both live in the
// list's content item; Silica's placeholder takes no input, so these cannot be its children.
// The buttons (PlaceholderButtons.qml) are compiled and created only once the list has failed
// (NFR-PERF-01).
Item {
    id: actions
    property Item placeholder
    property string listState
    property string errorText
    // Shows Retry; the owner reloads in onRetry.
    property bool canRetry: false
    signal retry()

    readonly property int savedCount: typeof downloads !== "undefined" ? downloads.count : 0
    readonly property bool failed: listState === "placeholder" && errorText !== ""
    visible: !!placeholder && failed
    width: buttons.item ? buttons.item.width : 0
    height: buttons.item ? buttons.item.height : 0
    anchors {
        top: placeholder ? placeholder.bottom : undefined
        topMargin: Theme.paddingLarge * 2
        horizontalCenter: placeholder ? placeholder.horizontalCenter : undefined
    }

    Loader {
        id: buttons
        active: actions.failed
        source: active ? "PlaceholderButtons.qml" : ""
    }
    Binding { target: buttons.item; when: buttons.status === Loader.Ready; property: "listState"; value: actions.listState }
    Binding { target: buttons.item; when: buttons.status === Loader.Ready; property: "errorText"; value: actions.errorText }
    Binding { target: buttons.item; when: buttons.status === Loader.Ready; property: "canRetry"; value: actions.canRetry }
    Binding { target: buttons.item; when: buttons.status === Loader.Ready; property: "savedCount"; value: actions.savedCount }
    Connections {
        target: buttons.item
        onRetry: actions.retry()
    }
}
