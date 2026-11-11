// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
import QtQuick 2.6
import Sailfish.Silica 1.0

Page {
    id: page
    allowedOrientations: Orientation.All

    function support(value) {
        //: A codec decoded by the device's hardware decoder
        if (value === "hardware") return qsTr("Hardware")
        //: A codec decoded in software on the CPU
        if (value === "software") return qsTr("Software")
        //: A codec the device cannot decode
        return qsTr("Not available")
    }
    function yesNo(value) { return value ? qsTr("Yes") : qsTr("No") }

    SilicaFlickable {
        anchors.fill: parent
        contentHeight: column.height

        Column {
            id: column
            width: parent.width

            PageHeader {
                //: Title of the page listing the device's decoders
                title: qsTr("Video capabilities")
                description: deviceCapabilities.loaded
                             ? "GStreamer " + deviceCapabilities.version
                             //: GStreamer could not be inspected
                             : qsTr("GStreamer not inspected; using safe defaults")
            }
            DetailItem { label: "H.264"; value: page.support(deviceCapabilities.h264) } // i18n-exempt: codec name
            DetailItem { label: "VP9"; value: page.support(deviceCapabilities.vp9) } // i18n-exempt: codec name
            DetailItem { label: "AV1"; value: page.support(deviceCapabilities.av1) } // i18n-exempt: codec name
            DetailItem { label: "HEVC"; value: page.support(deviceCapabilities.hevc) } // i18n-exempt: codec name
            DetailItem { label: "VP8"; value: page.support(deviceCapabilities.vp8) } // i18n-exempt: codec name
            DetailItem { label: "AAC"; value: page.yesNo(deviceCapabilities.aac) } // i18n-exempt: codec name
            DetailItem { label: "Opus"; value: page.yesNo(deviceCapabilities.opus) } // i18n-exempt: codec name
            //: Whether live (HLS) streams can be played
            DetailItem { label: qsTr("Live streams"); value: page.yesNo(deviceCapabilities.hls) }
            //: Whether separate audio and video streams can be combined
            DetailItem { label: qsTr("Adaptive streams"); value: page.yesNo(deviceCapabilities.adaptive) }
            //: Whether the hardware decoder was switched off in the settings
            DetailItem { label: qsTr("Hardware decoder"); value: deviceCapabilities.hardwareDecoderDemoted ? qsTr("Off") : page.yesNo((deviceCapabilities.elements || []).indexOf("droidvdec") >= 0) }
            DetailItem {
                label: "playbin3" // i18n-exempt: GStreamer element name
                //: playbin3 is used only for adaptive streams
                value: deviceCapabilities.playbinSwitchable ? qsTr("For adaptive streams") : page.yesNo(deviceCapabilities.playbin3)
            }
            //: Whether the JavaScript runtime for the full resolver profile is packaged
            DetailItem { label: qsTr("JavaScript runtime"); value: page.yesNo(javascriptRuntimeAvailable) }

            SectionHeader {
                //: Section listing the GStreamer elements found on the device
                text: qsTr("Elements")
            }
            Label {
                x: Theme.horizontalPageMargin
                width: parent.width - 2 * x
                wrapMode: Text.Wrap
                font.pixelSize: Theme.fontSizeExtraSmall
                color: Theme.secondaryColor
                text: (deviceCapabilities.elements || []).join(", ")
            }
            SectionHeader {
                //: Section showing the raw capabilities of the hardware decoder
                text: qsTr("Hardware decoder formats")
                visible: hwCaps.text !== ""
            }
            Label {
                id: hwCaps
                x: Theme.horizontalPageMargin
                width: parent.width - 2 * x
                wrapMode: Text.WrapAnywhere
                font.pixelSize: Theme.fontSizeTiny
                color: Theme.secondaryColor
                text: deviceCapabilities.sinkCaps ? (deviceCapabilities.sinkCaps["droidvdec"] || "") : ""
            }
            Item { width: 1; height: Theme.paddingLarge }
        }
        VerticalScrollDecorator { }
    }
}
