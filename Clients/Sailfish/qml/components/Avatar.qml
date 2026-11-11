// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
import QtQuick 2.6
import Sailfish.Silica 1.0

// Round channel or author picture (docs/DESIGN.md §4.2). The image is decoded at display size and
// drawn through a small circular-mask shader that reads the Image as a texture directly, so there
// is no offscreen layer (cheap in list delegates, NFR-PERF-07) and no extra QML import. Until the
// picture arrives, a round placeholder in the thumbnail tint holds its place. Takes no input.
Item {
    id: avatar
    property alias source: image.source
    readonly property bool ready: image.status === Image.Ready

    enabled: false
    implicitWidth: Theme.iconSizeMedium
    implicitHeight: implicitWidth

    Rectangle {
        anchors.fill: parent
        visible: !avatar.ready
        radius: width / 2
        color: Theme.rgba(Theme.highlightBackgroundColor, 0.1)
    }

    Image {
        id: image
        anchors.fill: parent
        visible: false
        asynchronous: true
        sourceSize.width: width
        sourceSize.height: height
        fillMode: Image.PreserveAspectCrop
        smooth: true
    }

    ShaderEffect {
        anchors.fill: parent
        visible: avatar.ready && width > 0
        property variant source: image
        // The texture is the whole decoded picture: crop a non-square one to its centre square.
        readonly property real aspect: image.implicitHeight > 0 ? image.implicitWidth / image.implicitHeight : 1
        property point crop: aspect > 1 ? Qt.point(1 / aspect, 1) : Qt.point(1, aspect)
        // One device pixel of soft edge, in texture units.
        property real edge: 1.0 / Math.max(1, width)
        // i18n-exempt: GLSL source
        fragmentShader: "
            varying highp vec2 qt_TexCoord0;
            uniform sampler2D source;
            uniform lowp float qt_Opacity;
            uniform highp vec2 crop;
            uniform highp float edge;
            void main() {
                highp float d = distance(qt_TexCoord0, vec2(0.5));
                lowp float inside = 1.0 - smoothstep(0.5 - edge, 0.5, d);
                highp vec2 uv = vec2(0.5) + (qt_TexCoord0 - vec2(0.5)) * crop;
                gl_FragColor = texture2D(source, uv) * inside * qt_Opacity;
            }"
    }
}
