// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
import QtQuick 2.6

// Headless stand-in for the playback component's VideoSurfaceItem (Core/Player/Infrastructure).
Item {
    property QtObject source: null
    property var fillMode
    property bool hasFrame: false
    property size videoSize: Qt.size(0, 0)
    property real videoAspect: 0
    // Shorts pager: present frames (true) or keep the last picture (false).
    property bool active: true
    function repaint() { }
}
