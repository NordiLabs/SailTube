// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
import QtQuick 2.6
import Sailfish.Silica 1.0
import "../logic/Shell.js" as Shell

// Bottom navigation bar (docs/DESIGN.md §2.2): Home, Shorts, Subscriptions, Library, Search,
// Downloads, as the official mobile apps place them, drawn with Silica parts on the docked-panel
// background. Every entry is a full-height touch target with a theme icon; the labels show under
// every icon when the widest one fits, otherwise only under the current entry
// (Shell.navigationLabels), so six entries fit narrow phones, landscape and the largest font.
// The current entry is highlighted with a line along the bar's top edge. Lives in the window,
// below the page stack, so the mini-player can dock above it.
Item {
    id: bar
    property string current
    property var sections: []      // [{ id, label, icon, activity }]
    // Shows a small dot on entries that set `activity: true` (for example running downloads).
    property bool activity: false
    signal selected(string id)

    readonly property real entryWidth: width / Math.max(1, sections.length)
    property real widestLabel: 0
    readonly property string labelMode: Shell.navigationLabels(entryWidth, widestLabel, Theme.paddingSmall)
    readonly property real labelHeight: Math.ceil(Theme.fontSizeTiny * 1.3)
    // Icon plus label; grows with the system text size (docs/DESIGN.md §7.9).
    readonly property real barHeight: Math.max(Theme.itemSizeSmall,
                                               Theme.iconSizeMedium + labelHeight + 2 * Theme.paddingSmall)

    width: parent ? parent.width : 0
    height: barHeight

    // The docked-panel background of Silica, so the bar reads as part of the system UI in every
    // ambience; a hairline separates it from the content above.
    Rectangle {
        anchors.fill: parent
        color: Theme.rgba(Theme.highlightDimmerColor, Theme.opacityOverlay)
    }
    Rectangle {
        anchors { left: parent.left; right: parent.right; top: parent.top }
        height: Math.max(1, Math.round(Theme.paddingSmall / 4))
        color: Theme.rgba(Theme.primaryColor, 0.1)
    }

    // Measures the widest label once per text change (a hidden Label per entry would cost items).
    TextMetrics {
        id: metrics
        font.pixelSize: Theme.fontSizeTiny
    }
    function measure() {
        var w = 0
        for (var i = 0; i < sections.length; ++i) {
            metrics.text = sections[i].label
            if (metrics.advanceWidth > w) w = metrics.advanceWidth
        }
        widestLabel = Math.ceil(w)
    }
    onSectionsChanged: measure()
    Component.onCompleted: measure()

    // Entries are placed explicitly (no Row: a positioner lays out only on the next polish), and
    // mirrored by hand for right-to-left languages, where Home sits on the right.
    Item {
        id: row
        anchors.fill: parent
        Repeater {
            model: bar.sections
            BackgroundItem {
                id: button
                readonly property bool isCurrent: modelData.id === bar.current
                // Every entry always shows its name under the icon, as YouTube does (user request, beta 1).
                readonly property bool showLabel: true
                x: (bar.LayoutMirroring.enabled ? bar.sections.length - 1 - index : index) * bar.entryWidth
                width: bar.entryWidth
                height: row.height
                onClicked: bar.selected(modelData.id)
                // A tab for assistive technology, named even when its label is hidden.
                Accessible.role: Accessible.PageTab
                Accessible.name: modelData.label
                Accessible.checkable: true
                Accessible.checked: isCurrent
                // The activity dot, spoken (DL-04): the downloads model's own "Downloading" text.
                Accessible.description: bar.activity && modelData.activity === true
                                        //: Download state shown while a download runs
                                        ? qsTranslate("nordtile::video::sailfish::DownloadsModel", "Downloading") : ""

                Rectangle {
                    visible: button.isCurrent
                    anchors { top: parent.top; horizontalCenter: parent.horizontalCenter }
                    width: parent.width * 0.5
                    height: Math.max(2, Math.round(Theme.paddingSmall / 2))
                    color: Theme.highlightColor
                }
                Icon {
                    id: icon
                    anchors.horizontalCenter: parent.horizontalCenter
                    // Centred alone, or above its label.
                    y: button.showLabel ? Theme.paddingSmall
                                        : Math.round((parent.height - height) / 2)
                    source: modelData.icon
                    highlighted: button.highlighted || button.isCurrent
                    Rectangle {
                        visible: bar.activity && modelData.activity === true
                        anchors { right: parent.right; top: parent.top }
                        width: Theme.paddingMedium
                        height: width
                        radius: width / 2
                        color: Theme.highlightColor
                    }
                }
                Label {
                    visible: button.showLabel
                    anchors { top: icon.bottom; horizontalCenter: parent.horizontalCenter }
                    width: button.width - Theme.paddingSmall / 2
                    text: modelData.label
                    // Long names ("Suscripciones") shrink to fit the entry, then elide.
                    fontSizeMode: Text.HorizontalFit
                    minimumPixelSize: Math.round(Theme.fontSizeTiny * 0.75)
                    elide: Text.ElideRight
                    horizontalAlignment: Text.AlignHCenter
                    font.pixelSize: Theme.fontSizeTiny
                    color: button.isCurrent || button.highlighted ? Theme.highlightColor : Theme.secondaryColor
                }
            }
        }
    }
}
