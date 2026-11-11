// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
import QtQuick 2.6
import Sailfish.Silica 1.0
import "../components"

// Search section of the main page: the shared SearchView, active while it is the current section.
Item {
    id: section
    property Item mainPage

    function scrollToTop() { view.scrollToTop() }

    SearchView {
        id: view
        anchors.fill: parent
        active: !!section.mainPage && section.mainPage.current === "search"
                && section.mainPage.status === PageStatus.Active
    }
}
