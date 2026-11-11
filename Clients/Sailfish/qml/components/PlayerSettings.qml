// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
import QtQuick 2.6
import Sailfish.Silica 1.0
import "Format.js" as Format
import "../logic/PlayerUi.js" as P

// The player's settings sheet (docs/DESIGN.md §3.2, parity gap 7): a modal Silica DockedPanel at
// the bottom of the watch page, opened by the gear of the overlay. Each row names a setting and
// shows its current value; a tap opens its choices in the same sheet (Back returns to the rows).
// Rows appear for what the session offers for the playing video (PlayerUi.sheetRows): Quality,
// Playback speed, Captions, Audio track, Loop video, Sleep timer, Stable volume, Audio only. In
// fullscreen the last row locks the screen against touches (§3.2.6).
DockedPanel {
    id: sheet
    objectName: "playerSettings"
    // "rows", "quality" or "speed".
    property string level: "rows"
    property bool captionsAvailable: false
    // The touch lock is offered (fullscreen, PlayerUi.lockOffered).
    property bool lockAvailable: false
    signal captionsRequested()
    signal lockRequested()

    dock: Dock.Bottom
    modal: true
    width: parent ? parent.width : 0
    height: Math.min(content.height + 2 * Theme.paddingMedium, (parent ? parent.height : 0) * 0.75)

    // The surface. Silica's own panel background is a light tint (15 to 30 % of the highlight
    // background) made to sit on the wallpaper; on the video and the info column everything
    // showed through it, and the rows could not be read (found on the device, beta 3 candidate).
    // The sheet therefore has an opaque base in the ambience's overlay colour (dark in a dark
    // ambience, light in a light one, so the theme's text colours read on it in both), with
    // Silica's tint on top so it still looks like a Silica panel.
    background: Item {
        Rectangle {
            objectName: "sheetSurface"
            anchors.fill: parent
            color: Theme.overlayBackgroundColor
        }
        PanelBackground {
            anchors.fill: parent
            position: Dock.Bottom
        }
    }

    // A modal panel dims the whole application through a switch on the application window, which
    // DockedPanel holds on while it is open. If the panel goes away while it is open (its page is
    // popped or replaced), nothing turns the switch off again and the whole app stays dimmed
    // (found on the device: "the shadow stays over the whole screen"). Pages hide the sheet before
    // they leave; this is the last line of defence, for a route nobody thought of.
    Component.onDestruction: {
        if (open && modal && typeof __silica_applicationwindow_instance !== "undefined" && __silica_applicationwindow_instance)
            __silica_applicationwindow_instance._dimScreen = false
    }

    function openSheet() { level = "rows"; show() }

    // A choice was tapped. These run in the sheet's own scope, not the tapped row's: closing the
    // sheet or applying the choice rebuilds the choice list (the session's lists change with
    // it), and a row that is destroyed in the middle of its handler cannot finish it.
    function pickQuality(height, highFrameRate) { hide(); playback.setQuality(height, highFrameRate) }
    function pickAudioTrack(id) { hide(); playback.setAudioTrack(id) }
    function pickSleep(minutes) { hide(); playback.setSleepTimer(minutes) }
    function pickSpeed(rate) { hide(); playback.setRate(rate) }
    onOpenChanged: if (!open) level = "rows"

    // What the session offers for the playing video. Audio tracks exist only with a separate
    // audio stream (never muxed, live or a download), and the picker needs two. Stable volume
    // shows only when the session says the switch can change what plays
    // (`stableVolumeAvailable`: the playing language has a levelled and a plain track). A live
    // stream has no end to loop at.
    readonly property var audioTrackList: playback.audioTracks

    readonly property var rows: P.sheetRows({
        qualities: playback.qualities.length > 0 && !playback.audioOnly,
        rate: playback.rateChangeable === true,
        captions: sheet.captionsAvailable,
        audioTracks: sheet.audioTrackList.length > 1,
        loop: !playback.live,
        sleepTimer: true,
        stableVolume: playback.stableVolumeAvailable,
        audioOnlyAvailable: playback.detailsLoaded,
        lock: sheet.lockAvailable
    })

    function rowTitle(id) {
        //: Menu entry opening the video quality choice
        if (id === "quality") return qsTranslate("VideoPage", "Quality")
        //: Player settings row: playback speed
        if (id === "speed") return qsTr("Playback speed")
        //: Title of the captions page
        if (id === "captions") return qsTranslate("CaptionsPage", "Captions")
        //: Player settings row: the audio track (original, dubbed, descriptive)
        if (id === "audioTrack") return qsTr("Audio track")
        //: Player settings row: play the video again when it ends
        if (id === "loop") return qsTr("Loop video")
        //: Player settings row: stop playback after a while
        if (id === "sleep") return qsTr("Sleep timer")
        //: Player settings row: even out loud and quiet videos
        if (id === "stableVolume") return qsTr("Stable volume")
        //: Player settings row: ignore touches on the video in full screen until it is unlocked
        if (id === "lock") return qsTr("Lock screen")
        //: Download quality choice: sound only
        return qsTranslate("SettingsPage", "Audio only")
    }
    function rowValue(id) {
        if (id === "quality") return qualityValue()
        if (id === "speed") return speedValue(playback.rate)
        if (id === "captions")
            return playback.captionTrackId !== "" ? P.captionLabel(playback.captionTracks, playback.captionTrackId)
                 //: Caption choice that turns captions off
                 : qsTranslate("CaptionsPage", "Off")
        if (id === "audioTrack") return audioTrackValue()
        if (id === "sleep") return sleepValue()
        return ""
    }
    function rowSwitchOn(id) {
        if (id === "loop") return playback.loop === true
        if (id === "stableVolume") return playback.stableVolume === true
        return playback.audioOnly
    }
    function toggle(id) {
        if (id === "loop") playback.setLoop(!playback.loop)
        else if (id === "stableVolume") playback.setStableVolume(!playback.stableVolume)
        else playback.setAudioOnlyMode(!playback.audioOnly)
    }
    function qualityValue() {
        if (playback.requestedHeight === 0)
            //: Automatic quality choice in the quality list
            return qsTranslate("QualityPage", "Automatic")
        var q = playback.qualities
        for (var i = 0; i < q.length; ++i)
            if (q[i].height === playback.requestedHeight && q[i].highFrameRate === playback.requestedHighFrameRate) return q[i].label
        return playback.requestedHeight + "p"
    }
    function audioTrackValue() {
        var l = audioTrackList
        for (var i = 0; i < l.length; ++i) if (l[i].selected) return l[i].label
        return ""
    }
    function sleepLabel(minutes) {
        //: Sleep timer choice: no timer
        if (minutes === 0) return qsTr("Off")
        //: Sleep timer choice: stop when the current video ends
        if (minutes < 0) return qsTr("End of video")
        //: Sleep timer choice, %n is a number of minutes
        return qsTr("%n minute(s)", "", minutes)
    }
    function sleepValue() {
        // The session's values: -1 off, else seconds left; sleepTimerAtEnd for "End of video".
        var left = playback.sleepTimerSecondsLeft
        var state = P.sleepState(left, playback.sleepTimerAtEnd)
        if (state === "end") return sleepLabel(-1)
        if (state === "running")
            //: Sleep timer value in the settings sheet, %1 is the time left such as 12:30
            return qsTr("%1 left").arg(Format.time(left * 1000))
        return sleepLabel(0)
    }
    function speedValue(rate) {
        //: Normal playback speed (1×)
        return P.isNormalSpeed(rate) ? qsTr("Normal") : P.speedText(rate)
    }

    SilicaFlickable {
        anchors.fill: parent
        contentHeight: content.height + 2 * Theme.paddingMedium
        clip: true

        Column {
            id: content
            y: Theme.paddingMedium
            width: parent.width

            // A choice list starts with the way back to the rows, named after the setting.
            BackgroundItem {
                id: backRow
                visible: sheet.level !== "rows"
                width: content.width
                height: Theme.itemSizeSmall
                onClicked: sheet.level = "rows"
                Icon {
                    id: backIcon
                    anchors { left: parent.left; leftMargin: Theme.horizontalPageMargin; verticalCenter: parent.verticalCenter }
                    source: "image://theme/icon-m-left"
                    // Points back in right-to-left layouts too.
                    mirror: backRow.LayoutMirroring.enabled
                    highlighted: backRow.highlighted
                }
                Label {
                    anchors {
                        left: backIcon.right; leftMargin: Theme.paddingMedium
                        right: parent.right; rightMargin: Theme.horizontalPageMargin
                        verticalCenter: parent.verticalCenter
                    }
                    truncationMode: TruncationMode.Fade
                    // Long translations ("Скорость воспроизведения") shrink a step before they fade.
                    fontSizeMode: Text.HorizontalFit
                    minimumPixelSize: Theme.fontSizeSmall
                    color: Theme.highlightColor
                    text: sheet.rowTitle(sheet.level)
                }
            }

            // ---- The rows ----
            Repeater {
                model: sheet.level === "rows" ? sheet.rows : []
                delegate: BackgroundItem {
                    id: rowItem
                    width: content.width
                    height: Math.max(Theme.itemSizeSmall, rowTexts.height + 2 * Theme.paddingSmall)
                    readonly property bool isSwitch: modelData === "audioOnly" || modelData === "loop" || modelData === "stableVolume"
                    onClicked: {
                        if (isSwitch) sheet.toggle(modelData)
                        else if (modelData === "captions") { sheet.hide(); sheet.captionsRequested() }
                        else if (modelData === "lock") { sheet.hide(); sheet.lockRequested() }
                        else sheet.level = modelData
                    }
                    Column {
                        id: rowTexts
                        anchors {
                            left: parent.left; leftMargin: Theme.horizontalPageMargin
                            // A switch row's texts end at the switch (a long title ran under it).
                            right: rowItem.isSwitch ? rowSwitch.left : parent.right
                            rightMargin: rowItem.isSwitch ? Theme.paddingSmall : Theme.horizontalPageMargin
                            verticalCenter: parent.verticalCenter
                        }
                        Label {
                            width: parent.width
                            truncationMode: TruncationMode.Fade
                            fontSizeMode: Text.HorizontalFit
                            minimumPixelSize: Theme.fontSizeSmall
                            color: rowItem.highlighted ? Theme.highlightColor : Theme.primaryColor
                            text: sheet.rowTitle(modelData)
                        }
                        Label {
                            width: parent.width
                            truncationMode: TruncationMode.Fade
                            font.pixelSize: Theme.fontSizeExtraSmall
                            color: rowItem.highlighted ? Theme.secondaryHighlightColor : Theme.secondaryColor
                            text: rowItem.isSwitch ? "" : sheet.rowValue(modelData)
                            visible: text !== ""
                        }
                    }
                    Switch {
                        id: rowSwitch
                        visible: rowItem.isSwitch
                        anchors { right: parent.right; rightMargin: Theme.paddingSmall; verticalCenter: parent.verticalCenter }
                        automaticCheck: false
                        checked: sheet.rowSwitchOn(modelData)
                        onClicked: sheet.toggle(modelData)
                    }
                }
            }

            // ---- Quality choices ----
            Repeater {
                model: sheet.level === "quality"
                       ? [{ height: 0, highFrameRate: false, label: "", codec: "" }].concat(playback.qualities) : []
                delegate: BackgroundItem {
                    id: qualityItem
                    readonly property bool current: playback.requestedHeight === modelData.height
                                                     && playback.requestedHighFrameRate === modelData.highFrameRate
                    width: content.width
                    height: Math.max(Theme.itemSizeSmall, qualityLabel.implicitHeight + 2 * Theme.paddingSmall)
                    onClicked: sheet.pickQuality(modelData.height, modelData.highFrameRate)
                    Label {
                        id: qualityLabel
                        anchors {
                            left: parent.left; leftMargin: Theme.horizontalPageMargin
                            right: parent.right; rightMargin: Theme.horizontalPageMargin
                            verticalCenter: parent.verticalCenter
                        }
                        truncationMode: TruncationMode.Fade
                        text: modelData.height === 0
                              //: Automatic quality choice in the quality list
                              ? qsTranslate("QualityPage", "Automatic") : modelData.label
                        color: qualityItem.current || qualityItem.highlighted ? Theme.highlightColor : Theme.primaryColor
                    }
                }
            }

            // ---- Audio tracks (SEL-04/05): original, dubbed, descriptive ----
            Repeater {
                model: sheet.level === "audioTrack" ? sheet.audioTrackList : []
                delegate: BackgroundItem {
                    id: trackItem
                    width: content.width
                    height: Math.max(Theme.itemSizeSmall, trackLabel.implicitHeight + 2 * Theme.paddingSmall)
                    onClicked: sheet.pickAudioTrack(modelData.id)
                    Label {
                        id: trackLabel
                        anchors {
                            left: parent.left; leftMargin: Theme.horizontalPageMargin
                            right: parent.right; rightMargin: Theme.horizontalPageMargin
                            verticalCenter: parent.verticalCenter
                        }
                        truncationMode: TruncationMode.Fade
                        text: modelData.label
                        color: modelData.selected || trackItem.highlighted ? Theme.highlightColor : Theme.primaryColor
                    }
                }
            }

            // ---- Sleep timer choices ----
            Repeater {
                model: sheet.level === "sleep" ? P.SLEEP_CHOICES : []
                delegate: BackgroundItem {
                    id: sleepItem
                    width: content.width
                    height: Theme.itemSizeSmall
                    onClicked: sheet.pickSleep(modelData)
                    Label {
                        anchors {
                            left: parent.left; leftMargin: Theme.horizontalPageMargin
                            right: parent.right; rightMargin: Theme.horizontalPageMargin
                            verticalCenter: parent.verticalCenter
                        }
                        text: sheet.sleepLabel(modelData)
                        color: sleepItem.highlighted ? Theme.highlightColor : Theme.primaryColor
                    }
                }
            }

            // ---- Speed choices ----
            Repeater {
                model: sheet.level === "speed" ? P.SPEEDS : []
                delegate: BackgroundItem {
                    id: speedItem
                    readonly property bool current: P.nearestSpeed(playback.rate) === modelData
                    width: content.width
                    height: Theme.itemSizeSmall
                    onClicked: sheet.pickSpeed(modelData)
                    Label {
                        anchors {
                            left: parent.left; leftMargin: Theme.horizontalPageMargin
                            right: parent.right; rightMargin: Theme.horizontalPageMargin
                            verticalCenter: parent.verticalCenter
                        }
                        text: sheet.speedValue(modelData)
                        color: speedItem.current || speedItem.highlighted ? Theme.highlightColor : Theme.primaryColor
                    }
                }
            }
        }
    }
}
