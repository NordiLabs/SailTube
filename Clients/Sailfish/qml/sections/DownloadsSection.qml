// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
import QtQuick 2.6
import Sailfish.Silica 1.0
import "../components"

// Downloads section of the main page (bottom bar): the shared DownloadsView.
Item {
    id: section
    property Item mainPage

    function scrollToTop() { view.scrollToTop() }

    DownloadsView {
        id: view
        anchors.fill: parent
    }
}
