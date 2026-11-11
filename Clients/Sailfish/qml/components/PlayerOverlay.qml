// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
import QtQuick 2.6
import Sailfish.Silica 1.0
import "Format.js" as Format
import "../logic/PlayerUi.js" as P

// The watch page's player controls (docs/DESIGN.md §3.2), laid out as on the official mobile apps
// and drawn with Silica: a full scrim over the picture; the centre play/pause disc with previous and
// next beside it; a top row (minimise, the title in fullscreen, autoplay, captions, settings); a
// bottom row (time, seek bar, fullscreen). Shown on a tap and hidden by the overlay controller.
// Everything is drawn in the theme's light colours because it always sits on the picture.
Item {
    id: overlay
    objectName: "playerOverlay"
    property bool shown: true
    property bool fullscreen: false
    property bool failed: false
    property bool ended: false
    // Something else holds the centre (the up-next card).
    property bool centreHidden: false
    // Some captions exist for this video (the CC button shows).
    property bool captionsAvailable: false
    property bool captionsOn: false
    readonly property alias slider: seekSlider
    // A tap on the time label switches the right label between duration and remaining.
    property bool showRemaining: false

    signal interacted()
    signal fullscreenRequested()
    signal minimiseRequested()
    signal settingsRequested()
    signal captionsRequested()
    signal replayRequested()

    readonly property string centre: P.centreState({
        playing: playback.playing,
        buffering: playback.buffering,
        loading: playback.state === 1,          // PlaybackSession.Loading
        ended: overlay.ended,
        failed: overlay.failed
    })

    opacity: shown ? 1 : 0
    visible: opacity > 0
    Behavior on opacity { FadeAnimator { duration: 150 } }

    // The scrim: the picture stays visible, the controls stay legible in every ambience.
    Rectangle {
        anchors.fill: parent
        color: Qt.rgba(0, 0, 0, 0.45) // theme-exempt: scrim over the video picture
    }

    // ---- Top row ----
    Item {
        id: topRow
        anchors { left: parent.left; right: parent.right; top: parent.top }
        height: Theme.itemSizeSmall

        // Minimise to the mini-player. Inset from the corner: the back swipe starts at the edge.
        IconButton {
            id: minimiseButton
            anchors { left: parent.left; leftMargin: Theme.horizontalPageMargin; verticalCenter: parent.verticalCenter }
            icon.source: "image://theme/icon-m-down"
            icon.color: Theme.lightPrimaryColor
            //: Accessible name of the button that leaves the watch page for the mini-player
            Accessible.name: qsTr("Minimize player")
            onClicked: { overlay.minimiseRequested(); overlay.interacted() }
        }
        Column {
            anchors {
                left: minimiseButton.right; leftMargin: Theme.paddingSmall
                right: topActions.left; rightMargin: Theme.paddingSmall
                verticalCenter: parent.verticalCenter
            }
            visible: overlay.fullscreen
            Label {
                width: parent.width
                text: playback.title
                truncationMode: TruncationMode.Fade
                font.pixelSize: Theme.fontSizeSmall
                color: Theme.lightPrimaryColor
            }
            Label {
                width: parent.width
                text: playback.channel
                visible: text !== ""
                truncationMode: TruncationMode.Fade
                font.pixelSize: Theme.fontSizeExtraSmall
                color: Theme.lightSecondaryColor
            }
        }
        Row {
            id: topActions
            anchors { right: parent.right; rightMargin: Theme.paddingSmall; verticalCenter: parent.verticalCenter }
            // Autoplay (PLY-25), the same setting as Settings → Playback.
            Switch {
                id: autoplaySwitch
                automaticCheck: false
                checked: (appSettings.revision, window.settingValue("playback/autoplayRelated") === true)
                //: Setting: play a related video after the current one ends
                Accessible.name: qsTranslate("SettingsPage", "Autoplay next video")
                onClicked: {
                    appSettings.setValue("playback/autoplayRelated", !checked)
                    overlay.interacted()
                }
            }
            // Captions: on/off, with the track choice in the settings sheet (SEL-06).
            MouseArea {
                id: captionsButton
                visible: overlay.captionsAvailable
                width: Theme.itemSizeSmall
                height: Theme.itemSizeSmall
                //: Title of the captions page
                Accessible.name: qsTranslate("CaptionsPage", "Captions")
                Accessible.role: Accessible.Button
                onClicked: { overlay.captionsRequested(); overlay.interacted() }
                Rectangle {
                    anchors.centerIn: parent
                    width: ccLabel.width + Theme.paddingMedium
                    height: ccLabel.height
                    radius: Theme.paddingSmall / 2
                    color: overlay.captionsOn ? Theme.lightPrimaryColor : "transparent" // theme-exempt: open badge when captions are off
                    border.color: Theme.lightPrimaryColor
                    border.width: Math.max(1, Math.round(Theme.paddingSmall / 4))
                    opacity: captionsButton.pressed ? 0.6 : 1.0
                    Label {
                        id: ccLabel
                        anchors.centerIn: parent
                        text: "CC" // i18n-exempt: the international captions mark
                        font.pixelSize: Theme.fontSizeExtraSmall
                        font.bold: true
                        color: overlay.captionsOn ? "black" : Theme.lightPrimaryColor // theme-exempt: text on the light badge
                    }
                }
            }
            IconButton {
                icon.source: "image://theme/icon-m-setting"
                icon.color: Theme.lightPrimaryColor
                //: Settings
                Accessible.name: qsTranslate("SettingsPage", "Settings")
                onClicked: { overlay.settingsRequested(); overlay.interacted() }
            }
        }
    }

    // ---- Centre: previous, play/pause disc, next ----
    // Transport and time keep their left-to-right order in right-to-left layouts, as media
    // controls do everywhere (docs/DESIGN.md §7.8); the top row mirrors with the page.
    Row {
        id: centreRow
        objectName: "playerTransport"
        LayoutMirroring.enabled: false
        LayoutMirroring.childrenInherit: true
        anchors.centerIn: parent
        spacing: Theme.paddingLarge * 2
        visible: overlay.centre !== "none" && !overlay.centreHidden

        IconButton {
            anchors.verticalCenter: parent.verticalCenter
            visible: playback.canGoPrevious === true
            icon.source: "image://theme/icon-m-previous"
            icon.color: Theme.lightPrimaryColor
            //: Accessible name of the button playing the previous video of the queue
            Accessible.name: qsTr("Previous video")
            onClicked: { playback.previous(); overlay.interacted() }
        }
        MouseArea {
            id: discButton
            readonly property real size: Math.round(Theme.itemSizeMedium * 1.5)
            width: size
            height: size
            enabled: overlay.centre !== "buffering"
            Accessible.role: Accessible.Button
            Accessible.name: overlay.centre === "replay"
                             //: Accessible name of the button playing the video again from the start
                             ? qsTr("Replay")
                             : overlay.centre === "pause"
                               //: Accessible name of the pause button
                               ? qsTranslate("PlayerControls", "Pause")
                               //: Accessible name of the play button
                               : qsTranslate("PlayerControls", "Play")
            onClicked: {
                if (overlay.centre === "replay") overlay.replayRequested()
                else playback.togglePlayback()
                overlay.interacted()
            }
            Rectangle {
                anchors.fill: parent
                radius: width / 2
                color: Qt.rgba(0, 0, 0, discButton.pressed ? 0.6 : 0.35) // theme-exempt: disc behind the play icon
                visible: overlay.centre !== "buffering"
            }
            Icon {
                anchors.centerIn: parent
                visible: overlay.centre !== "buffering"
                source: overlay.centre === "pause" ? "image://theme/icon-l-pause"
                      : overlay.centre === "replay" ? "image://theme/icon-m-repeat"
                      : "image://theme/icon-l-play"
                color: Theme.lightPrimaryColor
            }
            BusyIndicator {
                anchors.centerIn: parent
                size: BusyIndicatorSize.Large
                running: overlay.centre === "buffering"
                color: Theme.lightPrimaryColor
            }
        }
        IconButton {
            anchors.verticalCenter: parent.verticalCenter
            visible: playback.canGoNext === true
            icon.source: "image://theme/icon-m-next"
            icon.color: Theme.lightPrimaryColor
            //: Accessible name of the button playing the next video of the queue
            Accessible.name: qsTranslate("MiniPlayer", "Next video")
            onClicked: { playback.next(); overlay.interacted() }
        }
    }

    // ---- Bottom row: time, seek bar, fullscreen ----
    Item {
        id: bottomRow
        objectName: "playerBottomRow"
        LayoutMirroring.enabled: false
        LayoutMirroring.childrenInherit: true
        anchors { left: parent.left; right: parent.right; bottom: parent.bottom }
        height: Theme.itemSizeMedium
        readonly property var times: P.timeLabels(seekSlider.down ? seekSlider.value : playback.position,
                                                  playback.duration, playback.live, overlay.showRemaining)

        MouseArea {
            id: timeButton
            objectName: "playerTime"
            anchors { left: parent.left; leftMargin: Theme.horizontalPageMargin; verticalCenter: parent.verticalCenter }
            width: timeLabel.width
            height: parent.height
            onClicked: { overlay.showRemaining = !overlay.showRemaining; overlay.interacted() }
            Accessible.role: Accessible.Button
            Accessible.name: timeLabel.text
            Label {
                id: timeLabel
                anchors.verticalCenter: parent.verticalCenter
                text: Format.time(bottomRow.times.left) + " / "
                      + (playback.live
                         //: Shown instead of the duration for a live stream
                         ? qsTranslate("PlayerControls", "LIVE")
                         : (bottomRow.times.rightNegative ? "-" : "") + Format.time(bottomRow.times.right))
                font.pixelSize: Theme.fontSizeExtraSmall
                color: Theme.lightPrimaryColor
            }
        }
        // Buffered range, chapters, SponsorBlock colours, growing thumb, time bubble (§3.2.1).
        SeekBar {
            id: seekSlider
            anchors {
                left: timeButton.right; leftMargin: Theme.paddingLarge
                right: fullscreenButton.left; rightMargin: Theme.paddingLarge
                verticalCenter: parent.verticalCenter
            }
            height: parent.height
            position: playback.position
            duration: playback.duration
            // 0 while unknown: no buffered range then.
            buffered: playback.bufferedPosition
            chapters: playback.chapters
            // The segments the session's skipper uses (SB-02).
            segments: playback.sponsorSegments
            seekable: playback.seekable
            onPressed: overlay.interacted()
            onSeekRequested: { playback.seek(positionMs); overlay.interacted() }
        }
        MouseArea {
            id: fullscreenButton
            // At least small icon size x 1.5 (PLY-17).
            width: Math.max(Theme.iconSizeSmall * 1.5, Theme.itemSizeExtraSmall)
            height: bottomRow.height
            anchors { right: parent.right; rightMargin: Theme.paddingSmall; bottom: parent.bottom }
            onClicked: { overlay.fullscreenRequested(); overlay.interacted() }
            Accessible.role: Accessible.Button
            Accessible.name: overlay.fullscreen
                             //: Accessible name of the button leaving full screen
                             ? qsTranslate("PlayerControls", "Exit full screen")
                             //: Accessible name of the full screen button
                             : qsTranslate("PlayerControls", "Full screen")
            FullscreenGlyph {
                anchors.centerIn: parent
                width: Math.round(Theme.iconSizeSmall * 0.93)
                height: width
                exit: overlay.fullscreen
                opacity: fullscreenButton.pressed ? 0.6 : 1.0
            }
        }
    }
}
