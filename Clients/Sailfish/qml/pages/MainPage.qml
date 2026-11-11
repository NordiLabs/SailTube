// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
import QtQuick 2.6
import Sailfish.Silica 1.0
import "../components"
import "../logic/Shell.js" as Shell

// The app's first page: the six sections of the bottom navigation bar, one shown at a time,
// each created lazily. Only the landing section is built before the first frame (NFR-PERF-01);
// others are created on first visit and kept, so switching back keeps their scroll position.
// The bar itself lives in the window (harbour-sailtube.qml) so the mini-player docks above it.
Page {
    id: page
    allowedOrientations: Orientation.All
    readonly property bool isMainPage: true

    // DSC-04: the landing section is chosen once at start (a later change of the setting, or a
    // first subscription, applies on the next start); the catalog component's HomeSettings holds the
    // choice. Without subscriptions the default lands on Home (Shell.landingSection).
    property string current: Shell.landingSection(
        typeof homeSettings !== "undefined" ? homeSettings.landingPage : window.settingValue("content/landingPage"),
        typeof subscriptionsModel !== "undefined" ? subscriptionsModel.count : 0)
    property var visited: [current]
    Component.onCompleted: {
        current = current   // freeze: later setting changes do not switch
        window.mainPage = page
        if (typeof startupTiming !== "undefined") startupTiming.mark("shell")
    }

    function show(id) {
        if (Shell.SECTIONS.indexOf(id) < 0) return
        visited = Shell.visit(visited, id)
        current = id
    }

    // A second tap on the current section: back to the top of its list.
    function scrollToTop() {
        var loaders = sectionLoaders
        for (var i = 0; i < loaders.count; ++i) {
            var l = loaders.itemAt(i)
            if (l && l.sectionId === current && l.item && typeof l.item.scrollToTop === "function") l.item.scrollToTop()
        }
    }

    Item {
        id: area
        anchors.fill: parent
        clip: true

        Repeater {
            id: sectionLoaders
            model: Shell.SECTIONS
            Loader {
                readonly property string sectionId: modelData
                anchors.fill: parent
                active: Shell.loadedSections(page.current, page.visited)[sectionId]
                visible: sectionId === page.current
                asynchronous: sectionId !== page.current
                source: active ? Qt.resolvedUrl("../sections/" + Shell.sectionFile(sectionId)) : ""
                onLoaded: item.mainPage = page
            }
        }
    }
}
