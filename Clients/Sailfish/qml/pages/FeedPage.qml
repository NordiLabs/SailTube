// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
import QtQuick 2.6
import Sailfish.Silica 1.0
import "../components"

// Standalone page around the shared view (links, the landing setting, pushes from other
// pages). The main page shows the same view as a section.
Page {
    id: page
    allowedOrientations: Orientation.All

    FeedView {
        anchors.fill: parent
        standalone: true
        active: page.status === PageStatus.Active
    }
}
