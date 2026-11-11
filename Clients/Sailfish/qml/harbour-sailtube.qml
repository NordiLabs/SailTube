// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
import QtQuick 2.6
import Sailfish.Silica 1.0
import "pages"
import "cover"
import "components"
import "logic/Shell.js" as Shell
import "logic/SettingsInventory.js" as Inventory

ApplicationWindow {
    id: window

    // Set by the watch page while its picture is on screen.
    property bool videoVisible: false
    // The watch page is the current page (the mini-player hides).
    readonly property bool watchPageOnTop: !!pageStack.currentPage && pageStack.currentPage.isWatchPage === true
    // Pages with their own bottom controls declare `property bool miniPlayerOptOut: true` (PLY-11).
    readonly property bool miniPlayerOptOut: !!pageStack.currentPage && pageStack.currentPage.miniPlayerOptOut === true
    // The user closed the mini-player for the current video.
    property bool miniPlayerDismissed: false
    readonly property bool rightToLeft: Qt.application.layoutDirection === Qt.RightToLeft
    // Qt takes the application's layout direction from this entry of the loaded catalog, on
    // start and on every language change; it keeps the string in the catalogs.
    //: Layout direction of the language. Translate exactly as RTL for right-to-left languages (Arabic), LTR otherwise.
    readonly property string _layoutDirection: qsTranslate("QGuiApplication", "QT_LAYOUT_DIRECTION")

    // Right-to-left locales (Arabic) mirror every anchor and positioner below.
    LayoutMirroring.enabled: rightToLeft
    LayoutMirroring.childrenInherit: true

    initialPage: Component { MainPage { } }
    cover: Component { CoverPage { } }
    allowedOrientations: defaultAllowedOrientations
    _defaultPageOrientations: Orientation.All
    // The page area ends above the docked mini-player (visibleSize would loop through parent.height).
    // Pages end above the bottom navigation bar and the mini-player docked above it.
    bottomMargin: (navigationShown ? navigationBar.height : 0)
                  + (miniPlayer.item && miniPlayer.item.open ? miniPlayer.item.height : 0)

    // ---- Bottom navigation (docs/DESIGN.md §2.2) ----
    // Set by MainPage when it is created.
    property Item mainPage: null
    readonly property bool mainPageOnTop: !!pageStack.currentPage && pageStack.currentPage.isMainPage === true
    // Pages with forms or their own bottom controls (settings) and Dialogs hide the bar.
    readonly property bool navigationOptOut: !!pageStack.currentPage
        && (pageStack.currentPage.navigationBarOptOut === true || window.miniPlayerOptOut
            || pageStack.currentPage.hasOwnProperty("acceptDestination"))
    readonly property bool navigationShown: Shell.navigationBarVisible({
        watchPageOnTop: window.watchPageOnTop,
        pageOptOut: window.navigationOptOut,
        keyboardOpen: Qt.inputMethod.visible,
        immersive: false
    })
    readonly property var navigationSections: [
        //: Main page section with recommended or trending videos
        { id: "home", label: qsTranslate("MainPage", "Home"), icon: "image://theme/icon-m-home" },
        //: Channel tab with the channel's Shorts
        { id: "shorts", label: qsTranslate("ChannelPage", "Shorts"), icon: "image://theme/icon-m-video" },
        //: Main page section with the feed of subscribed channels
        { id: "subscriptions", label: qsTranslate("MainPage", "Subscriptions"), icon: "image://theme/icon-m-media-playlists" },
        //: Main page section with playlists, history, watch later and downloads
        { id: "library", label: qsTranslate("MainPage", "Library"), icon: "image://theme/icon-m-folder" },
        //: Main page section for searching videos
        { id: "search", label: qsTranslate("MainPage", "Search"), icon: "image://theme/icon-m-search" },
        //: Title of the downloads page
        { id: "downloads", label: qsTranslate("DownloadsPage", "Downloads"), icon: "image://theme/icon-m-cloud-download",
          activity: true }
    ]
    // A tap on the bar: switch the section, or return to the main page first. A tap on the
    // current section scrolls it back to the top (as the official apps do).
    function selectSection(id) {
        if (!window.mainPage) return
        var target = Shell.barTarget(id, window.mainPageOnTop)
        if (target.kind === "section") {
            if (window.mainPage.current === id) window.mainPage.scrollToTop()
            else window.mainPage.show(id)
        } else if (target.kind === "return") {
            window.mainPage.show(id)
            pageStack.pop(window.mainPage, pageStack.busy ? PageStackAction.Immediate : PageStackAction.Animated)
        }
    }

    // Opens a video on the watch page. Re-opening the video that is already loaded re-attaches
    // to the live player instead of reloading it (PLY-10/11).
    function openVideo(videoId, hints) {
        miniPlayerDismissed = false
        if (videoId !== playback.videoId) {
            // NFR-PERF-07: tap to details, playing and the first advancing frame.
            if (typeof startupTiming !== "undefined") { startupTiming.begin("video"); _timingVideo = true }
            // E12: playback keys its speculative-resolve reuse on this, right before open().
            if (typeof tapIntent !== "undefined") tapIntent.opening(videoId)
            playback.open(videoId, hints || {})
        }
        showWatchPage()
    }

    // Plays a list of rows ({ videoId, title, ... }) as the queue (PLY-25), from row `index`:
    // that row opens on the watch page, the others follow through next and previous, the system
    // media controls and the end of each video. Playlists call this for Play all and Shuffle.
    function playQueue(list, index) {
        var plan = Shell.queuePlan(list, index)
        if (!plan.first) return
        openVideo(plan.first.videoId, plan.first)
        playback.setQueue(plan.ids, plan.at)
    }

    // Brings the watch page to the top: a watch page further down the stack is reused rather
    // than stacked twice. For sessions opened another way (a downloaded file, PLY-26).
    function showWatchPage() {
        var existing = pageStack.find(function (page) { return page.isWatchPage === true })
        if (existing) pageStack.pop(existing)
        else pageStack.push(Qt.resolvedUrl("pages/VideoPage.qml"))
    }

    // Opens the page that holds a setting, with its control highlighted (SET-02). Error
    // messages use this instead of naming a page in their text.
    function openSetting(key) {
        var where = Inventory.locate(key)
        if (!where) return
        if (where.page === Inventory.PAGE_SETTINGS)
            pageStack.push(Qt.resolvedUrl("pages/SettingsPage.qml"), { focusKey: key })
        else if (where.page === Inventory.PAGE_DIAGNOSTICS)
            pageStack.push(Qt.resolvedUrl("pages/SettingsPage.qml"), { focusKey: key })
    }

    // True when the settings page holds a control for this key (an error's "Open setting" shows
    // only then; SET-02).
    function canOpenSetting(key) {
        return !!key && !!Inventory.locate(key)
    }

    function settingValue(key) {
        return appSettings.value(key, Inventory.defaultValue(key))
    }

    // A new video brings a closed mini-player back. PlaybackSession notifies videoId through
    // detailsChanged, so compare with the last id seen.
    property string _lastVideoId: ""
    property bool _timingVideo: false
    Connections {
        target: playback
        onPlayerChanged: if (window._timingVideo && playback.playing) startupTiming.end("video", "playing")
        onPositionChanged: {
            if (!window._timingVideo || playback.position <= 0) return
            startupTiming.end("video", "first-frame")
            window._timingVideo = false
        }
        onDetailsChanged: {
            if (window._timingVideo) startupTiming.end("video", "details")
            if (playback.videoId === window._lastVideoId) return
            window._lastVideoId = playback.videoId
            window.miniPlayerDismissed = false
        }
    }

    // Opened links (LNK-03/LNK-04). linkRouter routes first and asks for the raise after.
    readonly property var links: typeof linkRouter !== "undefined" ? linkRouter : null

    function pushLinkPage(url, properties) {
        return pageStack.push(url, properties, pageStack.busy ? PageStackAction.Immediate : PageStackAction.Animated)
    }

    // Library component hook: the subscriptions page opens channels through this.
    function openChannel(channelId, name) {
        return pageStack.push(Qt.resolvedUrl("pages/ChannelPage.qml"), { reference: channelId })
    }

    Connections {
        target: window.links
        onOpenVideo: {
            // A video opens on the watch page; a list seeds the queue at the video.
            var same = playback.videoId === videoId
            // LNK-02: a link's t= wins over the resume point (library component, LIB-05).
            if (startSeconds >= 0 && typeof libraryPlayback !== "undefined")
                libraryPlayback.setLinkStart(videoId, startSeconds * 1000)
            // index= is 1-based in the URL; the open() hint is 0-based (ARCHITECTURE.md section 8).
            var position = index > 0 ? index - 1 : -1
            window.openVideo(videoId, { startSeconds: startSeconds, playlistId: playlistId, index: position })
            if (same && startSeconds >= 0) playback.seek(startSeconds * 1000)
            // LNK-04: a list= link plays on through the playlist from the linked video.
            if (playlistId !== "" && typeof queueSeeder !== "undefined") queueSeeder.seed(playlistId, videoId, position)
        }
        onOpenChannel: window.pushLinkPage(Qt.resolvedUrl("pages/ChannelPage.qml"), { reference: reference })
        onOpenPlaylist: window.pushLinkPage(Qt.resolvedUrl("pages/PlaylistPreviewPage.qml"), { playlistId: listId })
        onRaiseRequested: {
            window.activate()
            raiseAgain.restart()
        }
    }

    // LNK-04: the playlist's ids, fetched by the catalog, become playback's queue (only while
    // the linked video is still the one playing).
    Connections {
        target: typeof queueSeeder !== "undefined" ? queueSeeder : null
        onQueueReady: if (playback.videoId === videoId) playback.setQueue(videoIds, currentIndex)
    }

    // Links that arrived during start-up are queued until the initial page exists.
    // Cold start (L1): only once the initial page is on the stack and no transition runs, so the
    // routed page lands on top of it instead of being replaced by it.
    Timer {
        interval: 0
        running: window.links !== null && pageStack.depth > 0 && !pageStack.busy
        onTriggered: window.links.setReady(true)
    }
    // A raise during start-up can come before the window is mapped: ask once more shortly after.
    Timer {
        id: raiseAgain
        interval: 600
        onTriggered: if (Qt.application.state !== Qt.ApplicationActive) window.activate()
    }

    // SET-01 "Hide Shorts" is stored in the app settings; catalog's search model keeps a live
    // copy (its filter page can still change it for the session).
    function applyHideShorts() {
        if (searchModel.hideShorts !== undefined) searchModel.hideShorts = settingValue("content/hideShorts") === true
    }
    Connections {
        target: appSettings
        onValueChanged: if (key === "content/hideShorts") window.applyHideShorts()
    }
    Component.onCompleted: applyHideShorts()

    // NFR-PERF-01: the first frame pays only for the main page and its landing section. Work it
    // does not need (MPRIS, download notifications, the keep-alive helpers) is created once the
    // first frame is on screen, or after 1.5 s at the latest, or as soon as a video opens.
    property bool deferredReady: startupTiming.hasMark("first-frame")
    Connections {
        target: startupTiming
        onMarked: if (stage === "first-frame") window.deferredReady = true
    }
    Timer {
        interval: 1500
        running: !window.deferredReady
        onTriggered: window.deferredReady = true
    }
    // Latched: once a video was opened the controls stay registered.
    property bool hadSession: false
    readonly property bool sessionLoaded: playback.videoId !== ""
    onSessionLoadedChanged: if (sessionLoaded) { hadSession = true; deferredReady = true }

    // Time-limited beta (core, docs/BETA.md): the dismissible end-of-beta warning; only in beta builds.
    Loader {
        active: typeof beta !== "undefined" && beta.bannerVisible
        anchors { top: parent.top; left: parent.left; right: parent.right }
        source: "components/BetaBanner.qml"
    }

    // Library component: download notifications (DL-04/06) and the FULL-only keep-alive (DL-09).
    Loader {
        active: window.deferredReady && typeof downloads !== "undefined"
        source: "components/DownloadNotifier.qml"
    }
    Loader {
        active: window.deferredReady && typeof downloads !== "undefined" && !storeVariant
        source: "components/DownloadKeepAlive.qml"
    }

    // MPRIS (PLT-10, MPR-01): registered on the bus with the first video, not at start.
    Loader {
        id: mediaControls
        active: window.hadSession
        source: "components/MediaControls.qml"
    }

    // PLY-12: keep the display on while a video plays in the foreground (FULL only:
    // the keep-alive plugin is not allowed in Harbour, PLT-11).
    Loader {
        id: displayKeepOn
        active: window.hadSession && !storeVariant
        source: "components/DisplayKeepOn.qml"
        // KeepOnPolicy (session): playing video, foreground, not parked, not
        // listen-only, setting on (PLY-12); and the watch page shows the picture.
        onLoaded: item.wanted = Qt.binding(function() { return window.videoVisible && playback.keepDisplayOn })
    }

    // The mini-player (PLY-11) is compiled and created with the first video, not at start
    // (NFR-PERF-01). The loaded bar is moved next to the Loader so it docks to the
    // window exactly as a direct child did (a sized Loader would stretch the panel).
    // The mini-player docks above the bottom bar: its panel lives in this area, which ends where
    // the bar begins.
    Item {
        id: dockArea
        // The window's bottomMargin already shortens this parent by the bar and the mini-player;
        // undo it so the dock ends exactly at the top of the bar, not one margin higher.
        anchors { left: parent.left; right: parent.right; top: parent.top; bottom: parent.bottom
                  bottomMargin: (window.navigationShown ? navigationBar.height : 0) - window.bottomMargin }
    }
    Loader {
        id: miniPlayer
        active: window.hadSession
        source: "components/MiniPlayer.qml"
        onLoaded: item.parent = dockArea
    }

    NavigationBar {
        id: navigationBar
        // Flush with the screen's bottom edge: the parent area ends window.bottomMargin above it
        // (device check: the bar floated one bar height above the edge).
        anchors.bottom: parent.bottom
        anchors.bottomMargin: -window.bottomMargin
        visible: window.navigationShown
        sections: window.navigationSections
        current: window.mainPage ? window.mainPage.current : ""
        // A dot on Downloads while downloads run (DL-04).
        activity: typeof downloads !== "undefined" && downloads.activeCount > 0
        onSelected: window.selectSection(id)
    }
    Binding {
        target: miniPlayer.item
        when: miniPlayer.status === Loader.Ready
        property: "shown"
        value: Shell.miniPlayerVisible({
            // A Short is never parked: it plays in the Shorts pager or is held there, and has no
            // mini-player (it would show the same video twice).
            hasSession: playback.videoId !== "" && !playback.shortActive,
            dismissed: window.miniPlayerDismissed,
            watchPageOnTop: window.watchPageOnTop,
            pageOptOut: window.miniPlayerOptOut,
            keyboardOpen: Qt.inputMethod.visible
        })
    }
    Connections {
        target: miniPlayer.item
        onOpenRequested: window.openVideo(playback.videoId)
        onCloseRequested: {
            window.miniPlayerDismissed = true
            playback.stop()
        }
    }
}
