// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
import QtQuick 2.6
import Sailfish.Silica 1.0
import org.nordilabs.sailtube 1.0
import "../components"
import "../components/Format.js" as Format
import "../logic/WatchLayout.js" as W
import "../logic/PlayerUi.js" as P
import "../logic/Rows.js" as Rows

// Watch page. Layout and chrome are decided in logic/WatchLayout.js: the video box (PLY-14),
// fullscreen and orientation (PLY-13), cutout insets (PLY-15) and the navigation indicator
// (PLY-16). The player controls (components/PlayerOverlay.qml, settings sheet PlayerSettings.qml)
// are the design component's; the player engine belongs to the playback component.
Page {
    id: page

    objectName: "videoPage"
    readonly property bool isWatchPage: true
    // The page's own orientation policy outside fullscreen; restored on every exit (PLY-13).
    readonly property int basePolicy: Orientation.All
    property string fullscreenMode: W.WINDOWED
    property bool restorePending: false
    // Pinch in fullscreen: fill the screen (crop) instead of fitting the picture (§3.2.2).
    property bool fillVideo: false

    readonly property bool failed: playback.state === PlaybackSession.Failed
    // Picture size as decoded; tests may set these directly.
    property real videoWidth: surface.videoSize.width
    property real videoHeight: surface.videoSize.height
    readonly property bool vertical: W.isVertical(videoWidth, videoHeight)
    readonly property bool immersive: W.immersive(fullscreenMode, isLandscape, playback.hasVideo)
    readonly property bool controlsVisible: overlay.controlsVisible || !playback.playing
    // The playback component's overlay controller (tests drive it).
    readonly property QtObject overlayController: overlay

    readonly property bool hasCutout: typeof Screen.hasCutouts !== "undefined" && Screen.hasCutouts === true
    readonly property var insets: W.cutoutInsets({
        portrait: page.isPortrait,
        immersive: page.immersive,
        cutoutBottom: hasCutout ? Screen.topCutout.y + Screen.topCutout.height : 0,
        cutoutHeight: hasCutout ? Screen.topCutout.height : 0,
        screenLong: Math.max(Screen.width, Screen.height),
        screenShort: Math.min(Screen.width, Screen.height),
        videoWidth: page.videoWidth,
        videoHeight: page.videoHeight
    })

    allowedOrientations: basePolicy
    showNavigationIndicator: W.showEdgeGlow({
        setting: (appSettings.revision, window.settingValue("playback/edgeGlow")),
        immersive: page.immersive,
        pageLandscape: page.isLandscape,
        playing: playback.playing,
        controlsVisible: page.controlsVisible,
        hasVideo: playback.hasVideo
    })

    function toggleFullscreen() {
        var r = W.toggleFullscreen({
            mode: fullscreenMode,
            pageLandscape: isLandscape,
            vertical: vertical,
            verticalSetting: window.settingValue("playback/verticalFullscreen"),
            basePolicy: basePolicy
        })
        fullscreenMode = r.mode
        restorePending = r.restorePending
        allowedOrientations = r.allowedOrientations
        tryRestore()
    }

    function tryRestore() {
        var o = W.restoreAfterExit(restorePending, window.deviceOrientation, basePolicy)
        if (o < 0) return
        restorePending = false
        allowedOrientations = o
    }

    onOrientationChanged: tryRestore()

    // Touch lock (docs/DESIGN.md §3.2.6): in fullscreen the picture ignores touches and Back until
    // Unlock is tapped. Offered from the settings sheet; it ends by itself when fullscreen ends,
    // on an error and at the end of the video.
    property bool touchLocked: false
    // The release switch (PlayerUi.TOUCH_LOCK_RELEASED): off, the row never shows. Tests set it.
    property bool touchLockReleased: P.TOUCH_LOCK_RELEASED
    readonly property bool lockOffered: P.lockOffered({ released: touchLockReleased, fullscreen: immersive,
                                                        failed: failed, ended: videoBox.ended })
    onLockOfferedChanged: if (!lockOffered) setTouchLocked(false)
    // Back is off while locked, and while the settings sheet is open: the sheet is modal, so any
    // press outside it (a Back swipe starts with one) closes the sheet, never the page. That press
    // flips `open` at once, so the same swipe would still pop the page on release: Back stays off
    // until the gesture is over (the page stack is no longer pressed).
    property bool sheetHoldsBack: false
    backNavigation: !touchLocked && !settingsSheet.open && !sheetHoldsBack
    Timer {
        id: sheetBackRelease
        interval: 350
        onTriggered: { if (pageStack.pressed) restart(); else page.sheetHoldsBack = false }
    }
    Connections {
        target: settingsSheet
        onOpenChanged: {
            if (settingsSheet.open) { page.sheetHoldsBack = true; sheetBackRelease.stop() }
            else sheetBackRelease.restart()
        }
    }
    function setTouchLocked(on) {
        if (on === touchLocked || (on && !lockOffered)) return
        touchLocked = on
        // Fullscreen by rotation alone is not pinned: hold landscape while locked.
        if (fullscreenMode === W.WINDOWED) allowedOrientations = on ? Orientation.LandscapeMask : basePolicy
        if (on) unlockButton.reveal()
        else { unlockButton.revealed = false; overlay.interact() }
    }
    onStatusChanged: {
        window.videoVisible = status === PageStatus.Active && playback.hasVideo
        // The page never leaves with the modal sheet open (Back, a link, the mini-player, the next
        // video replacing the page): the window would stay dimmed behind it.
        if (status !== PageStatus.Active) { settingsSheet.hide(true); sheetHoldsBack = false }
    }
    Component.onCompleted: {
        // Edge to edge (PLY-15); feature-detected for older Silica.
        if (page.hasOwnProperty("cutoutMode") && typeof CutoutMode !== "undefined")
            page.cutoutMode = CutoutMode.FullScreen
    }

    // Library component (LIB-02, DL-01): the current video as a library row.
    // SEL-06: the caption track choice (catalog's CaptionsPage over captionTracks).
    function openCaptions() {
        pageStack.push(Qt.resolvedUrl("CaptionsPage.qml"))
    }

    // Captions drawn over the picture (§3.2.3), all from the session (SEL-06): its tracks, the
    // chosen one ("" is off) and the cue at the playing position, on the session's own clock.
    readonly property bool hasCaptionTracks: playback.captionTracks.length > 0
    readonly property bool captionsOn: playback.captionTrackId !== ""
    readonly property string captionText: playback.captionText
    readonly property bool captionsShown: captionsOn && captionText !== ""
    // The chosen track's text could not be fetched (the catalog's cue model says so; the session
    // has no word for it). The choice stays, so the caption box says it for a few seconds: a CC
    // badge that reads "on" over a silent picture looks broken. A property, so tests can set it.
    property bool captionTrackFailed: typeof captionCues !== "undefined" && !!captionCues
                                      && typeof captionTracks !== "undefined" && !!captionTracks
                                      && P.captionLoadFailed({ on: captionsOn, failed: captionCues.failed === true,
                                                               cuesUrl: captionCues.url, trackUrl: captionTracks.currentUrl })
    property bool captionNotice: false
    onCaptionTrackFailedChanged: {
        captionNotice = captionTrackFailed && captionsOn
        if (captionNotice) captionNoticeTimer.restart()
    }
    onCaptionsOnChanged: if (!captionsOn) captionNotice = false
    Timer { id: captionNoticeTimer; interval: 4000; onTriggered: page.captionNotice = false }
    // The CC button: off remembers the track; on brings the last one back, or a track in the
    // interface language the first time.
    property string lastCaptionKey: ""
    function toggleCaptions() {
        if (!hasCaptionTracks) { openCaptions(); return }
        var t = P.captionsToggle({ on: captionsOn, currentKey: playback.captionTrackId, lastKey: lastCaptionKey,
                                   preferredKey: P.preferredCaption(playback.captionTracks, Qt.locale().name) })
        if (t.action === "off") { lastCaptionKey = t.remember; playback.setCaptionTrack("") }
        else if (t.action === "pick") {
            playback.setCaptionTrack(t.key)
            // The track is gone (another video's key): let the user choose.
            if (playback.captionTrackId === "") openCaptions()
        } else openCaptions()
    }

    function currentVideo() {
        return { videoId: playback.videoId, title: playback.title, channel: playback.channel,
                 channelId: playback.channelId, thumbnail: playback.thumbnail,
                 durationSeconds: playback.duration > 0 ? Math.round(playback.duration / 1000) : -1 }
    }
    Component.onDestruction: window.videoVisible = false

    Connections {
        target: window
        onDeviceOrientationChanged: page.tryRestore()
    }
    Connections {
        target: playback
        // Leaving video (audio only, failure, another stream) leaves fullscreen too.
        onStreamChanged: {
            window.videoVisible = page.status === PageStatus.Active && playback.hasVideo
            if (!playback.hasVideo && page.fullscreenMode !== W.WINDOWED) page.toggleFullscreen()
        }
    }

    // Watch page data (DSC-13/14/15, catalog component): watch-next + WEB player fetched next to
    // playback by WatchInfo, never delaying it.
    // Related videos and the comments token wait for the player (or 2.5 s), so they never compete
    // with the first video bytes (WatchInfo.waitForPlayback).
    // DL-07: a download plays offline (playback.diagnostics.local): no catalog network fetch.
    readonly property bool localPlay: playback.diagnostics.local === true
    Binding { target: watchInfo; property: "offline"; value: page.localPlay }
    Binding { target: watchInfo; property: "waitForPlayback"; value: true }
    Binding { target: watchInfo; property: "playbackRunning"; value: playback.playing }
    Binding { target: watchInfo; property: "videoId"; value: playback.videoId }
    // DL-07 / W4: a downloaded video's saved caption tracks reach the caption models through the
    // session itself (PlaybackCaptionSource.setLocalTracks); the page has nothing to hand over.
    Binding { target: watchInfo; property: "durationHint"; value: playback.duration / 1000 }
    Binding { target: watchInfo; property: "fallbackChapters"; value: playback.chapters }
    Connections {
        target: watchInfo
        onSeekRequested: playback.seek(seconds * 1000)
        onOpenUrlRequested: Qt.openUrlExternally(url)
        onHashtagRequested: {
            // A hashtag of the description: its search runs and the Search section shows it.
            searchModel.search("#" + tag)
            if (window.mainPage) window.selectSection("search")
            else pageStack.pop(null)
        }
    }
    Connections {
        target: watchInfo.chapters
        onJumpRequested: playback.seek(seconds * 1000)
    }
    Connections {
        target: playback
        onPositionChanged: watchInfo.chapters.setPosition(playback.position / 1000)
    }
    // PLY-24: the session polls the position finely only while the controls show. Captions do not
    // ask for it: the session's caption text runs on its own cue clock.
    function syncControlsVisible() {
        playback.setControlsVisible(overlay.controlsVisible && status === PageStatus.Active)
    }
    WatchActions { id: actions }

    // Letterbox behind an immersive picture, and the strip under a portrait cutout.
    Rectangle {
        anchors.fill: parent
        visible: page.immersive
        color: "black" // theme-exempt: letterbox around the video picture
    }
    Rectangle {
        width: parent.width
        height: page.insets.top
        visible: !page.immersive && height > 0
        color: "black" // theme-exempt: strip under the camera cutout
    }

    // Everything in the box sits on the picture (or on black): Silica's controls inside it take
    // the light-on-dark palette in every ambience (docs/DESIGN.md §7.4).
    SilicaItem {
        id: videoBox
        objectName: "videoBox"
        palette.colorScheme: Theme.LightOnDark
        x: page.immersive ? page.insets.left : 0
        y: page.insets.top
        width: page.immersive ? page.width - page.insets.left - page.insets.right : page.width
        height: page.immersive ? page.height - page.insets.top - page.insets.bottom
                               : W.boxHeight(page.width, page.height, page.videoWidth, page.videoHeight)
        Behavior on height { enabled: !page.immersive; NumberAnimation { duration: 150 } }

        Rectangle {
            anchors.fill: parent
            color: "black" // theme-exempt: background of the video picture
        }

        Image {
            objectName: "pictureThumbnail"
            anchors.fill: parent
            source: playback.thumbnail
            sourceSize.width: width
            fillMode: Image.PreserveAspectFit
            visible: !playback.hasVideo || !surface.hasFrame
            // Dimmed under the spinner and under the error box, so their text reads on a bright
            // picture (WatchLayout.thumbnailOpacity).
            opacity: W.thumbnailOpacity({ failed: page.failed, loading: playback.state === PlaybackSession.Loading,
                                          hasVideo: playback.hasVideo })
            asynchronous: true
        }

        // The playback component's player (PLY-01): frames of playback.mediaObject (VideoPlayer).
        VideoSurfaceItem {
            id: surface
            anchors.fill: parent
            source: playback.mediaObject
            visible: playback.hasVideo
            fillMode: page.fillVideo && page.immersive ? VideoSurfaceItem.PreserveAspectCrop : VideoSurfaceItem.PreserveAspectFit
            // NFR-PERF-02: first-frame is marked at the next frame swap.
            onHasFrameChanged: if (hasFrame) playback.surfaceFrameReady()
            Connections {
                target: playback
                // PLT-07 / PLY-09: re-upload the held frame on the (new) GL context.
                onRepaintRequested: surface.repaint()
            }
        }

        // PLY-17: tap stacking, one-flush double-tap seeks, 3.5 s auto-hide.
        OverlayController {
            id: overlay
            stepSeconds: appSettings.doubleTapSeconds
            onSeekRequested: playback.seek(positionMs)
            // PLY-24: 250 ms position updates only while the controls (or cue-drawn captions) show.
            onControlsChanged: page.syncControlsVisible()
            property bool playing: playback.playing
            onPlayingChanged: setPlaying(playing)
            Component.onCompleted: setPlaying(playing)
        }
        // The end of the video: the centre disc offers Replay until playback moves again.
        property bool ended: false
        Connections {
            target: playback
            onEnded: { overlay.setEnded(); videoBox.ended = true }
            onPlayerChanged: if (playback.playing) videoBox.ended = false
            // PLY-25: the next item replaces this page, so Back returns to the list.
            onReplacePageRequested: if (page.status === PageStatus.Active)
                pageStack.replace(Qt.resolvedUrl("VideoPage.qml"), {}, PageStackAction.Immediate)
            onDetailsChanged: {
                // SB-01 data (catalog component) for the SB-02 decisions; owner page only.
                if (typeof sponsorSegments === "undefined" || playback.videoId === "" || sponsorSegments.videoId === playback.videoId) return
                if (page.localPlay) {
                    // DL-07: the segments saved with the download, never a request.
                    var saved = typeof downloads !== "undefined" ? downloads.localMedia(playback.videoId) : null
                    sponsorSegments.setLocal(playback.videoId, saved && saved.segments ? saved.segments : [])
                } else {
                    sponsorSegments.fetch(playback.videoId)
                }
            }
        }
        Connections {
            target: typeof sponsorSegments !== "undefined" ? sponsorSegments : null
            onSegmentsChanged: if (sponsorSegments.videoId === playback.videoId) playback.setSponsorSegments(sponsorSegments.segments)
        }
        Connections {
            // PLY-25: the first related video is the autoplay candidate.
            target: typeof watchInfo !== "undefined" && watchInfo.related ? watchInfo.related : null
            onStateChanged: {
                var r = watchInfo.related
                playback.setRelatedCandidate(r.count > 0 && r.videoId === playback.videoId ? r.get(0).videoId : "")
            }
        }
        // PLY-10: this page is one watch context of the shared session.
        Item {
            id: watchContext
            property int contextId: -1
            Component.onCompleted: contextId = playback.claimContext()
            Component.onDestruction: playback.closeContext(contextId)
            Connections {
                target: page
                onStatusChanged: {
                    if (page.status === PageStatus.Active) playback.activateContext(watchContext.contextId)
                    page.syncControlsVisible()
                }
            }
        }

        // No picture yet (docs/DESIGN.md §3.1): spinner, the session's status line under it, and
        // Retry once loading is slow, so the spinner never sits alone forever.
        property real loadingMs: 0
        Timer {
            interval: 1000
            repeat: true
            running: playback.state === PlaybackSession.Loading && !page.failed
            onRunningChanged: if (running) videoBox.loadingMs = 0
            onTriggered: videoBox.loadingMs += interval
        }
        readonly property var chrome: W.loadingChrome({
            loading: playback.state === PlaybackSession.Loading,
            buffering: playback.buffering,
            recovering: playback.recovering === true,
            failed: page.failed,
            elapsedMs: loadingMs,
            statusText: playback.statusText
        })
        Column {
            anchors.centerIn: parent
            width: parent.width - 2 * Theme.horizontalPageMargin
            spacing: Theme.paddingMedium
            BusyIndicator {
                anchors.horizontalCenter: parent.horizontalCenter
                size: BusyIndicatorSize.Large
                running: videoBox.chrome.spinner
                // On the black picture box in every ambience (a light ambience's highlight is dark).
                color: Theme.lightPrimaryColor
            }
            Label {
                width: parent.width
                visible: videoBox.chrome.status
                text: playback.statusText
                horizontalAlignment: Text.AlignHCenter
                truncationMode: TruncationMode.Fade
                font.pixelSize: Theme.fontSizeExtraSmall
                color: Theme.lightPrimaryColor
            }
            Button {
                anchors.horizontalCenter: parent.horizontalCenter
                visible: videoBox.chrome.retry
                // Light text on the black video box in every ambience.
                color: Theme.lightPrimaryColor
                //: Button under a playback error that tries the video again
                text: qsTr("Retry")
                onClicked: playback.retry()
            }
        }

        // Error state (§6.8): the message centred in the error colour, a way to retry, and a
        // direct link when the error names a setting (SET-02).
        Column {
            anchors { left: parent.left; right: parent.right; verticalCenter: parent.verticalCenter; margins: Theme.horizontalPageMargin }
            visible: page.failed
            spacing: Theme.paddingMedium
            Label {
                objectName: "errorMessage"
                width: parent.width
                text: playback.errorText
                wrapMode: Text.Wrap
                horizontalAlignment: Text.AlignHCenter
                // Readable on black in light ambiences too, where errorColor is dark. A live event
                // that has not started ("Scheduled to start …") is news, not a failure: plain light.
                color: playback.upcoming === true ? Theme.lightPrimaryColor
                     : Theme.colorScheme === Theme.LightOnDark ? Theme.errorColor : Theme.lightPrimaryColor
                font.pixelSize: Theme.fontSizeSmall
            }
            // PLY-18: after automatic recovery is exhausted, a Retry button.
            Button {
                // Light text on the black video box in every ambience (light ambiences included).

                color: Theme.lightPrimaryColor

                anchors.horizontalCenter: parent.horizontalCenter
                //: Button under a playback error that tries the video again
                text: qsTr("Retry")
                onClicked: playback.retry()
            }
            Button {
                // Light text on the black video box in every ambience (light ambiences included).

                color: Theme.lightPrimaryColor

                anchors.horizontalCenter: parent.horizontalCenter
                readonly property string settingKey: playback.errorSettingKey
                visible: window.canOpenSetting(settingKey)
                //: Button under a playback error that opens the setting the error refers to
                text: qsTr("Open setting")
                onClicked: window.openSetting(settingKey)
            }
            // The browser as the last resort, in fullscreen too (§3.2.4).
            Button {
                color: Theme.lightPrimaryColor
                anchors.horizontalCenter: parent.horizontalCenter
                visible: playback.videoId !== ""
                //: Button opening the video on youtube.com in the web browser
                text: qsTr("Open in browser")
                onClicked: watchInfo.openInBrowser(false, playback.position / 1000)
            }
            // Listen-only (SEL-08) is a session mode, not a setting: when only the video would play,
            // the error offers the mode switch itself.
            Button {
                color: Theme.lightPrimaryColor
                anchors.horizontalCenter: parent.horizontalCenter
                visible: playback.errorSettingKey === "playback/audioOnly"
                //: Menu entry switching from audio-only back to video
                text: qsTr("Play video")
                onClicked: {
                    playback.setAudioOnlyMode(false)
                    playback.retry()
                }
            }
        }

        // Gestures on the picture (docs/DESIGN.md §3.2.2): taps go to the overlay controller
        // (toggle, double-tap seek); a long press plays at 2x while held; a vertical swipe enters or
        // leaves fullscreen, or minimises; a pinch in fullscreen fills or fits the picture.
        // Horizontal drags are left to Silica (the back swipe), so there is no scrub-by-drag on the
        // picture; the seek bar scrubs.
        PinchArea {
            id: pinchArea
            anchors.fill: parent
            // Always enabled (a disabled PinchArea would disable the taps below); acts in fullscreen.
            onPinchFinished: if (page.immersive && !page.touchLocked) page.fillVideo = pinch.scale > 1.0
            MouseArea {
                id: pictureArea
                objectName: "pictureArea"
                anchors.fill: parent
                enabled: !page.failed
                property real startX: 0
                property real startY: 0
                property bool holding: false
                // Hold to speed: the session plays at 2x while held and restores the rate after
                // (pitch kept, not remembered, nothing at the live edge).
                function startHold() {
                    holding = true
                    playback.holdSpeed(true)
                }
                function endHold() {
                    if (!holding) return
                    holding = false
                    playback.holdSpeed(false)
                }
                function release(x, y) {
                    if (holding) { endHold(); return }
                    // Locked: a touch only shows the way out (PlayerUi.lockedTouch).
                    if (page.touchLocked) { unlockButton.reveal(); return }
                    var act = P.swipeAction(x - startX, y - startY, page.immersive, Theme.itemSizeMedium)
                    if (act === "minimise") pageStack.navigateBack()
                    else if (act === "enterFullscreen" || act === "exitFullscreen") page.toggleFullscreen()
                    else if (Math.abs(x - startX) < Theme.startDragDistance && Math.abs(y - startY) < Theme.startDragDistance)
                        overlay.tap(x, width, playback.position, playback.duration)
                }
                onPressed: { startX = mouse.x; startY = mouse.y }
                onPressAndHold: if (playback.playing && !page.touchLocked) startHold()
                onReleased: release(mouse.x, mouse.y)
                onCanceled: endHold()
            }
        }

        // Long press: "2x" while held.
        Rectangle {
            anchors { top: parent.top; horizontalCenter: parent.horizontalCenter; topMargin: Theme.paddingLarge }
            visible: pictureArea.holding
            width: holdLabel.width + 2 * Theme.paddingLarge
            height: holdLabel.height + Theme.paddingSmall
            radius: height / 2
            color: Qt.rgba(0, 0, 0, 0.6) // theme-exempt: pill over the picture
            Label {
                id: holdLabel
                anchors.centerIn: parent
                text: "2\u00d7 \u25b8\u25b8" // i18n-exempt: speed mark
                font.pixelSize: Theme.fontSizeSmall
                color: Theme.lightPrimaryColor
            }
        }

        // PLY-17 double-tap seek: a ripple on the tapped side with the running total.
        Item {
            id: seekRipple
            anchors { top: parent.top; bottom: parent.bottom }
            width: parent.width * 0.4
            x: overlay.gestureSide < 0 ? 0 : parent.width - width
            visible: overlay.gestureActive
            clip: true
            Rectangle {
                width: parent.height * 1.4
                height: width
                radius: width / 2
                anchors.verticalCenter: parent.verticalCenter
                x: overlay.gestureSide < 0 ? parent.width - width : 0
                color: Qt.rgba(1, 1, 1, 0.15) // theme-exempt: ripple over the picture
            }
            Column {
                anchors.centerIn: parent
                spacing: Theme.paddingSmall
                Icon {
                    anchors.horizontalCenter: parent.horizontalCenter
                    source: overlay.gestureSide < 0 ? "image://theme/icon-m-10s-back" : "image://theme/icon-m-10s-forward"
                    color: Theme.lightPrimaryColor
                }
                Label {
                    anchors.horizontalCenter: parent.horizontalCenter
                    //: Running total of a double-tap seek, %1 is a signed number of seconds such as +20
                    text: qsTr("%1 s").arg((overlay.gestureSeconds > 0 ? "+" : "") + overlay.gestureSeconds)
                    color: Theme.lightPrimaryColor
                    font.pixelSize: Theme.fontSizeMedium
                }
            }
        }

        // SB-02: 1.6 s notice naming the skipped category, and the manual button.
        Label {
            anchors { top: parent.top; horizontalCenter: parent.horizontalCenter; topMargin: Theme.paddingLarge }
            visible: playback.sponsorNotice !== ""
            //: Notice after a SponsorBlock segment was skipped, %1 is the category name
            text: qsTr("Skipped: %1").arg(playback.sponsorNotice)
            color: Theme.lightPrimaryColor
            font.pixelSize: Theme.fontSizeSmall
        }
        Button {
            // Light text on the black video box in every ambience (light ambiences included).

            color: Theme.lightPrimaryColor

            anchors { right: parent.right; bottom: parent.bottom; margins: Theme.paddingMedium
                      bottomMargin: Theme.itemSizeMedium + Theme.paddingMedium }
            visible: playback.sponsorButton !== ""
            //: Button skipping a SponsorBlock segment, %1 is the category name
            text: qsTr("Skip %1").arg(playback.sponsorButton)
            onClicked: { playback.skipSponsor(); overlay.interact() }
        }

        // PLY-25 and the end of the video (§3.2.4): the "Up next" card with its countdown and
        // Cancel, then suggestions under the Replay disc.
        PlayerEndScreen {
            id: endScreen
            anchors.fill: parent
            z: 3
            ended: videoBox.ended
            related: typeof watchInfo !== "undefined" && watchInfo.related ? watchInfo.related : null
            onOpenRequested: { videoBox.ended = false; window.openVideo(videoId, hints) }
        }

        // Library component (LIB-04): offer to resume from the saved position.
        ResumeOffer {
            anchors.centerIn: parent
            z: 2
        }

        // Captions over the picture (§3.2.3): white on a dark box, centred, clear of the controls.
        Rectangle {
            id: captionBox
            objectName: "captionBox"
            visible: page.captionsShown || page.captionNotice
            anchors { horizontalCenter: parent.horizontalCenter; bottom: parent.bottom
                      bottomMargin: P.captionBottomMargin(controls.shown, Theme.itemSizeMedium, Theme.paddingMedium) }
            width: Math.min(captionLabel.implicitWidth, parent.width * 0.9) + 2 * Theme.paddingMedium
            height: captionLabel.height + Theme.paddingSmall
            radius: Theme.paddingSmall / 2
            color: Qt.rgba(0, 0, 0, 0.6) // theme-exempt: caption background over the picture
            z: 1
            Label {
                id: captionLabel
                anchors.centerIn: parent
                width: Math.min(implicitWidth, captionBox.parent.width * 0.9)
                text: page.captionNotice
                      //: Shown under the title when the chosen caption track could not be loaded
                      ? qsTranslate("CaptionsPage", "This track could not be loaded")
                      : page.captionText
                wrapMode: Text.Wrap
                horizontalAlignment: Text.AlignHCenter
                textFormat: Text.PlainText
                font.pixelSize: page.immersive ? Theme.fontSizeMedium : Theme.fontSizeSmall
                color: Theme.lightPrimaryColor
            }
        }

        // The player controls (docs/DESIGN.md §3.2): scrim, centre disc, top and bottom rows.
        PlayerOverlay {
            id: controls
            anchors.fill: parent
            fullscreen: page.immersive
            failed: page.failed
            ended: videoBox.ended
            shown: (overlay.controlsVisible || !playback.playing || settingsSheet.open || controls.slider.down)
                   && !page.failed && !pictureArea.holding && !overlay.gestureActive && !page.touchLocked
            captionsAvailable: page.hasCaptionTracks
            captionsOn: page.captionsOn
            onInteracted: overlay.interact()
            onFullscreenRequested: page.toggleFullscreen()
            onMinimiseRequested: pageStack.navigateBack()
            onSettingsRequested: settingsSheet.openSheet()
            onCaptionsRequested: page.toggleCaptions()
            onReplayRequested: { playback.seek(0); playback.play(); videoBox.ended = false }
            // The up-next card takes the centre while autoplay counts down.
            centreHidden: endScreen.countingDown
        }

        // Touch lock (§3.2.6): the way out, shown when the lock starts and for a while after
        // every touch on the locked picture.
        MouseArea {
            id: unlockButton
            objectName: "unlockButton"
            z: 4
            property bool revealed: false
            function reveal() { revealed = true; unlockTimer.restart() }
            visible: page.touchLocked && revealed
            anchors { horizontalCenter: parent.horizontalCenter; bottom: parent.bottom; bottomMargin: 2 * Theme.paddingLarge }
            width: unlockRow.width + 2 * Theme.paddingLarge
            height: Theme.itemSizeSmall
            Accessible.role: Accessible.Button
            Accessible.name: unlockLabel.text
            onClicked: page.setTouchLocked(false)
            Rectangle {
                anchors.fill: parent
                radius: height / 2
                color: Qt.rgba(0, 0, 0, unlockButton.pressed ? 0.8 : 0.6) // theme-exempt: pill over the picture
            }
            Row {
                id: unlockRow
                anchors.centerIn: parent
                spacing: Theme.paddingSmall
                Icon {
                    anchors.verticalCenter: parent.verticalCenter
                    source: "image://theme/icon-m-device-lock"
                    width: Theme.iconSizeSmall
                    height: width
                    sourceSize.width: width
                    sourceSize.height: height
                    color: Theme.lightPrimaryColor
                }
                Label {
                    id: unlockLabel
                    anchors.verticalCenter: parent.verticalCenter
                    //: Button ending the touch lock of the full screen player
                    text: qsTr("Unlock")
                    font.pixelSize: Theme.fontSizeSmall
                    color: Theme.lightPrimaryColor
                }
            }
            Timer {
                id: unlockTimer
                interval: 3000
                onTriggered: unlockButton.revealed = false
            }
        }
    }

    // The player's settings sheet (Quality, Playback speed, Captions, Audio only; parity gap 7).
    PlayerSettings {
        id: settingsSheet
        // Above the info column, which is declared after it and would otherwise paint over it.
        z: 10
        captionsAvailable: controls.captionsAvailable
        onCaptionsRequested: page.openCaptions()
        lockAvailable: page.lockOffered
        onLockRequested: page.setTouchLocked(true)
        // The controls stay up while the sheet is open.
        onOpenChanged: overlay.menuOpen = open
    }

    SilicaFlickable {
        id: info
        anchors { left: parent.left; right: parent.right; top: videoBox.bottom; bottom: parent.bottom }
        visible: !page.immersive
        clip: true
        contentHeight: column.height

        PullDownMenu {
            MenuItem {
                visible: page.failed
                //: Menu entry retrying a failed video
                text: qsTr("Retry")
                onClicked: playback.retry()
            }
            // Quality and audio only live in the player's settings sheet (gear on the video).
            // Library component (LIB-02, DL-01): at most 5 pulley items in all (PLT-06).
            MenuItem {
                visible: playback.videoId !== ""
                //: Menu entry adding the current video to a playlist
                text: qsTr("Add to playlist")
                onClicked: pageStack.push(Qt.resolvedUrl("AddToPlaylistDialog.qml"), { video: page.currentVideo() })
            }
            MenuItem {
                visible: playback.videoId !== "" && typeof downloads !== "undefined" && !downloadState.progressInfo.active
                //: Menu entry opening the download choice for the current video
                text: qsTr("Download")
                onClicked: pageStack.push(Qt.resolvedUrl("DownloadQualityDialog.qml"), { video: page.currentVideo() })
            }
        }

        Column {
            id: column
            width: info.width
            spacing: Theme.paddingSmall
            topPadding: Theme.paddingLarge

            Label {
                x: Theme.horizontalPageMargin
                width: parent.width - 2 * x
                text: playback.title
                wrapMode: Text.Wrap
                maximumLineCount: 3
                elide: Text.ElideRight
                font.pixelSize: Theme.fontSizeMedium
                color: Theme.highlightColor
            }
            // Views and age under the title, as on the official app; likes have their chip below.
            Label {
                x: Theme.horizontalPageMargin
                width: parent.width - 2 * x
                wrapMode: Text.Wrap
                font.pixelSize: Theme.fontSizeExtraSmall
                color: Theme.secondaryColor
                text: {
                    var parts = []
                    if (playback.live || watchInfo.live)
                        //: Shown for a live stream instead of a view count
                        parts.push(qsTr("Live now"))
                    // Watch-next / player values first (structured), the resolver's otherwise.
                    var views = watchInfo.viewCount >= 0 ? watchInfo.viewCount : playback.viewCount
                    if (views >= 0)
                        //: View count of a video, %1 is a compact number such as 1.2M
                        parts.push(qsTr("%1 views").arg(Format.count(views, Qt.locale())))
                    else if (watchInfo.viewText !== "")
                        parts.push(watchInfo.viewText)
                    var date = watchInfo.dateText !== "" ? watchInfo.dateText : playback.uploadDate
                    if (date !== "") parts.push(date)
                    return parts.join(" · ")
                }
            }
            // Owner row, as on the official app: the round avatar (the channel's initial while there
            // is none, as on the cards), the name with the subscriber count under it, and Subscribe
            // at the end (LIB-01). A tap on the row opens the channel page. Anchored throughout, so
            // it mirrors in right-to-left layouts (docs/DESIGN.md §7.8).
            BackgroundItem {
                id: ownerRow
                objectName: "ownerRow"
                readonly property string name: playback.channel !== "" ? playback.channel : watchInfo.owner
                readonly property string subscribers: {
                    // YouTube's localized text first, else generated in the same language.
                    if (watchInfo.subscriberText !== "") return watchInfo.subscriberText
                    if (watchInfo.subscriberCount >= 0 && typeof catalogText !== "undefined")
                        return catalogText.subscribers(watchInfo.subscriberCount)
                    if (watchInfo.subscriberCount >= 0)
                        //: Subscriber count of a channel, %1 is a compact number such as 1.2M
                        return qsTr("%1 subscribers").arg(Format.count(watchInfo.subscriberCount, Qt.locale()))
                    return ""
                }
                // A property of its own: an item's `visible` reads false while its parent is hidden.
                readonly property bool canSubscribe: playback.channelId.length === 24 && playback.channelId.indexOf("UC") === 0
                width: parent.width
                height: Math.max(ownerAvatar.height, ownerTexts.height,
                                 canSubscribe ? subscribeButton.height : 0) + Theme.paddingSmall
                visible: name !== "" || canSubscribe
                enabled: watchInfo.ownerId !== "" || playback.channelId !== ""
                onClicked: pageStack.push(Qt.resolvedUrl("ChannelPage.qml"),
                                          { reference: playback.channelId !== "" ? playback.channelId : watchInfo.ownerId })
                Item {
                    id: ownerAvatar
                    anchors { left: parent.left; leftMargin: Theme.horizontalPageMargin; verticalCenter: parent.verticalCenter }
                    width: Theme.iconSizeMedium
                    height: width
                    Rectangle {
                        anchors.fill: parent
                        visible: watchInfo.avatar === ""
                        radius: width / 2
                        color: Theme.rgba(Theme.highlightBackgroundColor, 0.25)
                        Label {
                            anchors.centerIn: parent
                            text: Rows.initial(ownerRow.name)
                            font.pixelSize: Theme.fontSizeMedium
                            color: Theme.highlightColor
                        }
                    }
                    // DSC-19: the 176 px source decoded at display size.
                    Avatar {
                        anchors.fill: parent
                        visible: watchInfo.avatar !== ""
                        source: watchInfo.avatar
                    }
                }
                Column {
                    id: ownerTexts
                    anchors {
                        left: ownerAvatar.right; leftMargin: Theme.paddingMedium
                        right: ownerRow.canSubscribe ? subscribeButton.left : parent.right
                        rightMargin: ownerRow.canSubscribe ? Theme.paddingMedium : Theme.horizontalPageMargin
                        verticalCenter: parent.verticalCenter
                    }
                    Label {
                        objectName: "ownerName"
                        width: parent.width
                        text: ownerRow.name
                        truncationMode: TruncationMode.Fade
                        font.pixelSize: Theme.fontSizeSmall
                        color: ownerRow.highlighted ? Theme.highlightColor : Theme.primaryColor
                    }
                    Label {
                        width: parent.width
                        visible: text !== ""
                        text: ownerRow.subscribers
                        truncationMode: TruncationMode.Fade
                        font.pixelSize: Theme.fontSizeExtraSmall
                        color: ownerRow.highlighted ? Theme.secondaryHighlightColor : Theme.secondaryColor
                    }
                }
                // The one Subscribe button of the app (texts, remorse): also on the channel page
                // and on channel rows.
                SubscribeButton {
                    id: subscribeButton
                    objectName: "watchSubscribe"
                    anchors { right: parent.right; rightMargin: Theme.horizontalPageMargin; verticalCenter: parent.verticalCenter }
                    visible: ownerRow.canSubscribe
                    channelId: playback.channelId
                    channelName: ownerRow.name
                    avatarUrl: watchInfo.avatar
                }
            }
            // PLY-26: the downloaded file is what plays; one tap streams it instead.
            Item {
                visible: page.localPlay
                x: Theme.horizontalPageMargin
                width: parent.width - 2 * x
                height: visible ? Math.max(localLabel.implicitHeight, streamButton.height) : 0
                Label {
                    id: localLabel
                    anchors { left: parent.left; right: streamButton.left; rightMargin: Theme.paddingMedium; verticalCenter: parent.verticalCenter }
                    wrapMode: Text.Wrap
                    font.pixelSize: Theme.fontSizeExtraSmall
                    color: Theme.highlightColor
                    //: Watch-page note while the video plays from its downloaded file
                    text: qsTr("Playing downloaded file")
                }
                Button {
                    id: streamButton
                    anchors { right: parent.right; verticalCenter: parent.verticalCenter }
                    preferredWidth: Theme.buttonWidthSmall
                    //: Button on the watch page that streams a video instead of playing its downloaded file
                    text: qsTr("Stream instead")
                    // PLY-26: the session's own switch, from the current position; the page,
                    // the cover and the mini-player stay. Off while a switch is already under way.
                    enabled: playback.canStreamInstead
                    onClicked: playback.streamInstead()
                }
            }
            // A non-fatal message while playback goes on (for example a stream switch that could not
            // start: the file keeps playing); the failed state has its own box over the picture.
            Label {
                x: Theme.horizontalPageMargin
                width: parent.width - 2 * x
                visible: !page.failed && playback.errorText !== ""
                wrapMode: Text.Wrap
                font.pixelSize: Theme.fontSizeExtraSmall
                color: Theme.errorColor
                text: playback.errorText
            }
            // DL-04 watch-page indicator.
            Label {
                id: downloadState
                property var progressInfo: ({ active: false, done: false, progress: 0 })
                function refresh() {
                    if (typeof downloads !== "undefined" && playback.videoId !== "") downloadState.progressInfo = downloads.stateFor(playback.videoId)
                }
                Connections {
                    target: typeof downloads !== "undefined" ? downloads : null
                    onItemChanged: if (videoId === playback.videoId) downloadState.refresh()
                }
                Connections { target: playback; onDetailsChanged: downloadState.refresh() }
                Component.onCompleted: refresh()
                x: Theme.horizontalPageMargin
                width: parent.width - 2 * x
                visible: downloadState.progressInfo.active || downloadState.progressInfo.done
                font.pixelSize: Theme.fontSizeExtraSmall
                color: Theme.highlightColor
                text: downloadState.progressInfo.done
                      //: Watch-page note for a video that is downloaded
                      ? qsTr("Downloaded")
                      //: Watch-page note while a download runs, %1 is a percentage
                      : qsTr("Downloading %1 %").arg(W.downloadPercent(downloadState.progressInfo.progress, false))
            }
            Label {
                x: Theme.horizontalPageMargin
                width: parent.width - 2 * x
                visible: text !== ""
                font.pixelSize: Theme.fontSizeTiny
                color: Theme.secondaryColor
                text: playback.state === PlaybackSession.Ready || playback.state === PlaybackSession.Preparing
                      ? playback.streamLabel : playback.statusText
            }

            // One-tap actions as a row of chips that scrolls sideways (docs/DESIGN.md §3.4): the like
            // count, share, download, save to a playlist, copy link (optionally at the current time),
            // open in the browser (DSC-14, LIB-02, DL-01).
            ChipStrip {
                id: chipRow
                objectName: "actionChips"
                visible: playback.videoId !== ""
                readonly property real likes: watchInfo.likeCount >= 0 ? watchInfo.likeCount : playback.likeCount
                ActionChip {
                    visible: chipRow.likes >= 0
                    passive: true
                    iconSource: "image://theme/icon-m-like"
                    text: Format.count(chipRow.likes, Qt.locale())
                    //: Like count of a video, %1 is a compact number
                    Accessible.name: qsTr("%1 likes").arg(Format.count(chipRow.likes, Qt.locale()))
                }
                // RYD-01: the community's dislike estimate, only when the user turned it on in
                // Settings (the catalog fills it then). Marked as an estimate with the "about" sign:
                // the official app shows no count at all.
                ActionChip {
                    objectName: "dislikeChip"
                    readonly property string count: watchInfo.dislikeText !== "" ? watchInfo.dislikeText
                                                    : Format.count(watchInfo.dislikeCount, Qt.locale())
                    visible: watchInfo.dislikeCount >= 0
                    passive: true
                    iconSource: "image://theme/icon-m-like"
                    iconRotation: 180
                    text: "\u2248 " + count // i18n-exempt: the "approximately" sign before a number
                    //: Estimated dislike count of a video, %1 is a compact number such as 1.2K
                    Accessible.name: qsTr("About %1 dislikes (estimate)").arg(count)
                }
                ActionChip {
                    iconSource: "image://theme/icon-m-share"
                    //: Button sharing the video link through the system share sheet
                    text: qsTr("Share")
                    onClicked: actions.share(watchInfo.shareUrl(atTime.checked, playback.position / 1000), playback.title)
                }
                ActionChip {
                    visible: typeof downloads !== "undefined" && !downloadState.progressInfo.active
                    iconSource: "image://theme/icon-m-cloud-download"
                    //: Menu entry opening the download choice for the current video
                    text: qsTr("Download")
                    onClicked: pageStack.push(Qt.resolvedUrl("DownloadQualityDialog.qml"), { video: page.currentVideo() })
                }
                ActionChip {
                    iconSource: "image://theme/icon-m-add"
                    //: Menu entry adding the current video to a playlist
                    text: qsTr("Add to playlist")
                    onClicked: pageStack.push(Qt.resolvedUrl("AddToPlaylistDialog.qml"), { video: page.currentVideo() })
                }
                ActionChip {
                    iconSource: "image://theme/icon-m-clipboard"
                    //: Button copying the video link to the clipboard
                    text: qsTr("Copy link")
                    onClicked: actions.copy(watchInfo.shareUrl(atTime.checked, playback.position / 1000))
                }
                ActionChip {
                    iconSource: "image://theme/icon-m-website"
                    //: Button opening the video on youtube.com in the web browser
                    text: qsTr("Open in browser")
                    onClicked: watchInfo.openInBrowser(atTime.checked, playback.position / 1000)
                }
            }
            TextSwitch {
                id: atTime
                visible: playback.videoId !== ""
                //: Switch adding the current playback time to shared and copied links
                text: qsTr("Link at the current time")
                description: checked ? Format.time(playback.position) : ""
            }
            Label {
                x: Theme.horizontalPageMargin
                width: parent.width - 2 * x
                visible: actions.message !== ""
                text: actions.message
                font.pixelSize: Theme.fontSizeExtraSmall
                color: Theme.highlightColor
            }

            // The description as a card (docs/DESIGN.md §3.4), as on the official app: collapsed to
            // five lines on a tinted box; a tap anywhere on it expands or collapses. Links open in
            // the browser, timestamps seek, hashtags search (the text takes only taps on links).
            BackgroundItem {
                id: descriptionCard
                objectName: "descriptionCard"
                visible: description.text !== ""
                width: parent.width
                height: descriptionBox.height + Theme.paddingMedium
                highlightedColor: "transparent" // theme-exempt: the box below shows the press
                readonly property bool expandable: !watchInfo.collapsed || watchInfo.descriptionTruncated || description.truncated
                enabled: expandable
                onClicked: watchInfo.toggleCollapsed()
                Rectangle {
                    id: descriptionBox
                    x: Theme.horizontalPageMargin
                    width: parent.width - 2 * x
                    height: descriptionColumn.height + 2 * Theme.paddingMedium
                    radius: Theme.paddingMedium
                    color: Theme.rgba(Theme.highlightBackgroundColor, descriptionCard.down ? 0.3 : 0.1)
                    Column {
                        id: descriptionColumn
                        x: Theme.paddingMedium
                        y: Theme.paddingMedium
                        width: parent.width - 2 * x
                        spacing: Theme.paddingSmall
                        Label {
                            id: description
                            readonly property bool rich: watchInfo.descriptionStyled !== ""
                            width: parent.width
                            visible: text !== ""
                            text: rich ? watchInfo.shownDescription : playback.description
                            textFormat: rich ? Text.StyledText : Text.PlainText
                            wrapMode: Text.Wrap
                            maximumLineCount: watchInfo.collapsed ? watchInfo.collapsedLines : 100000
                            elide: Text.ElideRight
                            font.pixelSize: Theme.fontSizeSmall
                            color: Theme.primaryColor
                            linkColor: Theme.highlightColor
                            onLinkActivated: watchInfo.activateLink(link)
                        }
                        Label {
                            id: showMoreLabel
                            visible: descriptionCard.expandable
                            text: watchInfo.collapsed
                                  //: Expands the collapsed video description
                                  ? qsTr("Show more")
                                  //: Collapses the video description to 5 lines
                                  : qsTr("Show less")
                            font.pixelSize: Theme.fontSizeExtraSmall
                            color: Theme.highlightColor
                        }
                    }
                }
            }

            SectionHeader {
                visible: watchInfo.chapters.count > 0
                //: Section header above the list of video chapters
                text: qsTr("Chapters")
            }
            BackgroundItem {
                width: parent.width
                visible: watchInfo.chapters.count > 0
                onClicked: pageStack.push(Qt.resolvedUrl("ChaptersPage.qml"), { chapterModel: watchInfo.chapters, seekable: true })
                Label {
                    x: Theme.horizontalPageMargin
                    width: parent.width - 2 * x
                    anchors.verticalCenter: parent.verticalCenter
                    truncationMode: TruncationMode.Fade
                    font.pixelSize: Theme.fontSizeSmall
                    text: watchInfo.chapters.currentTitle !== ""
                          //: Chapter entry on the watch page: chapter count and the chapter playing now
                          ? qsTr("%n chapter(s) · now: %1", "", watchInfo.chapters.count).arg(watchInfo.chapters.currentTitle)
                          : qsTr("%n chapter(s)", "", watchInfo.chapters.count)
                }
            }

            // Comments teaser (docs/DESIGN.md §3.4): with the related videos as cards the comment
            // list sits far down, so a card here opens the full comments page, as the official app's
            // teaser does. It shows the count and the first comment once they are loaded and never
            // starts a load by itself (DSC-16).
            BackgroundItem {
                id: commentsTeaser
                objectName: "commentsTeaser"
                visible: playback.videoId !== ""
                width: parent.width
                height: teaserBox.height + Theme.paddingMedium
                highlightedColor: "transparent" // theme-exempt: the box below shows the press
                readonly property bool loaded: commentsModel.videoId === playback.videoId && commentsModel.count > 0
                onClicked: pageStack.push(Qt.resolvedUrl("CommentsPage.qml"),
                                          { videoId: playback.videoId, token: commentsAnchor.token })
                Rectangle {
                    id: teaserBox
                    x: Theme.horizontalPageMargin
                    width: parent.width - 2 * x
                    height: teaserColumn.height + 2 * Theme.paddingMedium
                    radius: Theme.paddingMedium
                    color: Theme.rgba(Theme.highlightBackgroundColor, commentsTeaser.down ? 0.3 : 0.1)
                    Column {
                        id: teaserColumn
                        x: Theme.paddingMedium
                        y: Theme.paddingMedium
                        width: parent.width - 2 * x
                        spacing: Theme.paddingSmall
                        Item {
                            width: parent.width
                            height: Math.max(teaserTitle.height, teaserArrow.height)
                            Label {
                                id: teaserTitle
                                anchors { left: parent.left; right: teaserArrow.left; rightMargin: Theme.paddingSmall; verticalCenter: parent.verticalCenter }
                                truncationMode: TruncationMode.Fade
                                font.pixelSize: Theme.fontSizeSmall
                                color: Theme.highlightColor
                                text: commentsModel.videoId === playback.videoId && commentsModel.totalText !== ""
                                      //: Comments section header with the total count, e.g. "Comments 7.7K"
                                      ? qsTranslate("CommentsSection", "Comments %1").arg(commentsModel.totalText)
                                      //: Comments section header without a known count
                                      : qsTranslate("CommentsSection", "Comments")
                            }
                            Icon {
                                id: teaserArrow
                                anchors { right: parent.right; verticalCenter: parent.verticalCenter }
                                source: "image://theme/icon-m-right"
                                width: Theme.iconSizeSmall
                                height: width
                                sourceSize.width: width
                                sourceSize.height: height
                                // Points forward in right-to-left layouts too.
                                mirror: commentsTeaser.LayoutMirroring.enabled
                                highlighted: commentsTeaser.down
                            }
                        }
                        // The first comment: one delegate of the shared model, not a second list.
                        ListView {
                            id: teaserPreview
                            readonly property real rowHeight: Math.max(Theme.iconSizeSmall, Math.ceil(Theme.fontSizeExtraSmall * 2.8))
                            width: parent.width
                            height: commentsTeaser.loaded ? rowHeight : 0
                            visible: commentsTeaser.loaded
                            interactive: false
                            clip: true
                            cacheBuffer: 0
                            model: commentsTeaser.loaded ? commentsModel : null
                            delegate: Item {
                                width: teaserPreview.width
                                height: teaserPreview.rowHeight
                                Avatar {
                                    id: teaserAvatar
                                    anchors { left: parent.left; verticalCenter: parent.verticalCenter }
                                    width: Theme.iconSizeSmall
                                    height: width
                                    source: model.avatar || ""
                                }
                                Label {
                                    anchors { left: teaserAvatar.right; leftMargin: Theme.paddingMedium; right: parent.right; verticalCenter: parent.verticalCenter }
                                    text: model.text || ""
                                    textFormat: Text.PlainText
                                    wrapMode: Text.Wrap
                                    maximumLineCount: 2
                                    elide: Text.ElideRight
                                    font.pixelSize: Theme.fontSizeExtraSmall
                                    color: Theme.secondaryColor
                                }
                            }
                        }
                    }
                }
            }

            SectionHeader {
                visible: watchInfo.related.count > 0
                //: Section header above the related videos
                text: qsTr("Related videos")
            }
            // The first five inline; the rest on the related page.
            Repeater {
                model: Math.min(5, watchInfo.related.count)
                VideoListItem {
                    readonly property var row: { watchInfo.related.count; return watchInfo.related.get(index) }
                    width: column.width
                    // Badges and the shared menu like every list; Details first (DSC-14).
                    // Cards in portrait, as under the official player (§4.1).
                    cardLayout: true
                    avatarUrl: row.channelAvatar || ""
                    largeThumbnailUrl: row.thumbnailLarge || ""
                    videoId: row.videoId || ""
                    menu: Component { VideoMenuItems { video: row; showDetails: true } }
                    videoTitle: row.title || ""
                    channelName: row.channel || ""
                    thumbnailUrl: row.thumbnail || ""
                    durationText: row.duration || ""
                    views: row.viewCount !== undefined ? row.viewCount : -1
                    isLive: row.live === true
                    // Replaces the video in place on this watch page, through the shell (timing, PLY-25).
                    onClicked: window.openVideo(row.videoId, row)
                }
            }
            BackgroundItem {
                width: parent.width
                visible: watchInfo.related.count > 5 || watchInfo.related.canLoadMore
                onClicked: pageStack.push(Qt.resolvedUrl("RelatedPage.qml"), { info: watchInfo })
                Label {
                    // Anchored, not x:, so right-to-left layouts mirror it (I18N-03).
                    anchors { left: parent.left; leftMargin: Theme.horizontalPageMargin; verticalCenter: parent.verticalCenter }
                    //: Opens the full list of related videos
                    text: qsTr("More related videos")
                    font.pixelSize: Theme.fontSizeSmall
                    color: Theme.highlightColor
                }
            }

            // ---- Comments (DSC-16): read-only, loaded per the auto-load setting. ----
            CommentsSection {
                id: commentsAnchor
                width: parent.width
                flickable: info
                videoId: playback.videoId
                token: watchInfo.videoId === playback.videoId ? watchInfo.commentsContinuation : ""
            }

            Item { width: 1; height: Theme.paddingLarge }
        }
        VerticalScrollDecorator { }
    }
}
