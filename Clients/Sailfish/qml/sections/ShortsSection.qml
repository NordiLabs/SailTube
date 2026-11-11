// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
import QtQuick 2.6
import Sailfish.Silica 1.0
import "../components"

// Shorts section of the main page (bottom bar): the vertical ShortsView.
Item {
    id: section
    property Item mainPage

    function scrollToTop() { view.scrollToTop() }

    ShortsView {
        id: view
        anchors.fill: parent
        active: !!section.mainPage && section.mainPage.current === "shorts"
                && section.mainPage.status === PageStatus.Active
    }
}
