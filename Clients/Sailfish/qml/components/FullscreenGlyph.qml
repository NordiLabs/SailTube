// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
import QtQuick 2.6
import Sailfish.Silica 1.0

// Four corner brackets: pointing outwards to enter fullscreen, inwards to leave it. Drawn with
// rectangles so it needs no icon file and survives backgrounding (no canvas buffer).
Item {
    id: glyph
    property bool exit: false
    property color color: Theme.lightPrimaryColor
    readonly property real stroke: Math.max(2, Math.round(width / 12))
    readonly property real arm: Math.round(width * 0.32)

    Repeater {
        model: 4
        Item {
            readonly property bool isRight: index === 1 || index === 3
            readonly property bool isBottom: index >= 2
            // Exit: the corner's lines meet at the inner end, so the brackets point inwards.
            x: isRight ? glyph.width - glyph.arm : 0
            y: isBottom ? glyph.height - glyph.arm : 0
            width: glyph.arm
            height: glyph.arm
            Rectangle {
                color: glyph.color
                width: parent.width; height: glyph.stroke
                y: (parent.isBottom !== glyph.exit) ? parent.height - glyph.stroke : 0
            }
            Rectangle {
                color: glyph.color
                width: glyph.stroke; height: parent.height
                x: (parent.isRight !== glyph.exit) ? parent.width - glyph.stroke : 0
            }
        }
    }
}
