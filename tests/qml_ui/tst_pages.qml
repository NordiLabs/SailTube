// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
import QtQuick 2.6
import Sailfish.Silica 1.0

// Every page opens and closes cleanly in portrait and landscape, and its touch targets are at
// least Theme.itemSizeSmall high and Theme.itemSizeExtraSmall wide (docs/DESIGN.md).
UiTest {
    id: t
    property var pages: ["AboutPage.qml", "CapabilitiesPage.qml", "QualityPage.qml", "SettingsPage.qml", "VideoPage.qml",
                         "FeedPage.qml", "RecommendedPage.qml", "SearchPage.qml", "SearchFilterPage.qml", "HistoryPage.qml",
                         "SubscriptionsPage.qml", "PlaylistsPage.qml", "LibraryPage.qml", "DownloadFolderPage.qml"]
    property int index: 0
    property bool landscape: false
    function targets(page) {
        return findAll(page, function (i) {
            return i.visible && i.enabled !== false && i.width > 0 && i.hasOwnProperty("down") && i.hasOwnProperty("highlighted")
                && typeof i.clicked === "function"
        })
    }
    function check(page) {
        var bad = []
        var ts = targets(page)
        for (var i = 0; i < ts.length; ++i)
            if (ts[i].height + 0.5 < Theme.itemSizeSmall || ts[i].width + 0.5 < Theme.itemSizeExtraSmall)
                bad.push((ts[i].text || ts[i].label || "item") + " " + Math.round(ts[i].width) + "x" + Math.round(ts[i].height))
        verify(bad.length === 0, "small touch targets on " + pages[index] + (landscape ? " (landscape)" : "") + ": " + bad.join("; "))
    }
    function open() {
        stack().push(Qt.resolvedUrl("../../Clients/Sailfish/qml/pages/" + pages[index]), {}, PageStackAction.Immediate)
        if (landscape) current().allowedOrientations = Orientation.Landscape
        return 500
    }
    function close() {
        check(current())
        stack().pop(undefined, PageStackAction.Immediate)
        if (++index >= pages.length) { index = 0; landscape = !landscape }
        return 150
    }
    Component.onCompleted: harness.expectWarning("Cannot open: file:///usr/share/icons/hicolor/172x172/apps/harbour-sailtube.png")
    // open and close every page, in portrait and then landscape
    steps: {
        var out = []
        for (var i = 0; i < pages.length * 2; ++i) out.push(open, close)
        return out
    }
}
