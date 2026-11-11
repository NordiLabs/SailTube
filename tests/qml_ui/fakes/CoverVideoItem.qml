// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
import QtQuick 2.6

// Headless stand-in for the playback component's CoverVideoItem (Core/Player/Infrastructure).
Item {
    property QtObject source: null
    property var fillMode
    property bool hasFrame: false
    property size videoSize: Qt.size(0, 0)
    property real videoAspect: 0
    property bool active: false
    property int framesDrawn: 0
    function repaint() { }
}
