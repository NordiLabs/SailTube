// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
import QtQuick 2.6
import Sailfish.Silica 1.0
import "../components"

// DL-04 downloads page (Library row, failed-list shortcut). The list itself is the shared
// components/DownloadsView.qml, which the Downloads section of the bottom bar shows too.
Page {
    id: page
    allowedOrientations: Orientation.All

    DownloadsView {
        anchors.fill: parent
        standalone: true
    }
}
