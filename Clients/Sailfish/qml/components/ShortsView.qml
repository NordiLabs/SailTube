// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
import QtQuick 2.6
import Sailfish.Silica 1.0
import org.nordilabs.sailtube 1.0
import "../logic/ShortsUi.js" as U
import "../logic/PlayerUi.js" as P
import "../logic/ShareLinks.js" as Links
import "../logic/Rows.js" as Rows

// Shorts section (DSC-17, docs/DESIGN.md §2.2 and §3.5), stage 2: a vertical pager, one Short per
// screen, over catalog's Shorts. The Short on screen plays by itself, full screen, upright and
// looping, in the shared player; a tap pauses or resumes it, a vertical swipe moves to the next
// one, which starts at once (the one after it is preloaded). There is no overlay and no watch page.
//
// Every page shows its poster; the page the pager has settled on adds the live picture over it
// once the player has a frame for that Short. The poster covers the switch, so a swipe never shows
// a black gap. What plays when is decided in logic/ShortsUi.js: only while this section is the
// current one with the main page on top, the app is in front and the user has not paused it.
Item {
    id: view
    objectName: "shortsView"
    property bool active: true
    // The app is in front: false while the screen is locked or another app is shown.
    property bool appActive: Qt.application.state === Qt.ApplicationActive
    // Catalog's Shorts model (subscriptions first, then popular Shorts, then YouTube's own sequence,
    // paged). Every build has it.
    readonly property bool busy: shortsModel.busy === true
    readonly property int count: shortsModel.count

    // `get()` hands out a snapshot of a row and tells nobody when the row changes later, and the
    // details of a Short (counts, channel, avatar, related video) arrive after its row exists. So
    // everything read from a row depends on this stamp, which changes whenever any row's data does:
    // the model's own `dataChanged`, and its `revision` when it has one. (A binding on a bare
    // `get()` was evaluated once per page, and the rail and the bottom block stayed empty.)
    property int _dataTick: 0
    readonly property int dataStamp: _dataTick + shortsModel.revision
    Connections {
        target: shortsModel
        onDataChanged: view._dataTick += 1
    }

    // The Short the pager has settled on (not the middle of a drag).
    property int shownIndex: -1
    readonly property var shownRow: dataStamp >= 0 && count >= 0 && shownIndex >= 0 ? row(shownIndex) : null
    readonly property string shownId: shownRow && shownRow.videoId ? shownRow.videoId : ""
    // The pager holds the Short (it is covered by another page, or the tab was left).
    property bool holding: false
    // The user paused it with a tap: the session's own pause, which a hold and a release keep.
    readonly property bool userPaused: shownId !== "" && playback.videoId === shownId && playback.shortActive
                                       && playback.state === PlaybackSession.Ready && !playback.playing && !playback.shortHeld
    // When the section was left (ms), to decide if the list is stale on the way back.
    property real leftAt: 0

    function scrollToTop() { pager.positionViewAtBeginning(); settle() }
    function row(i) { return shortsModel.get(i) }
    function refresh() { shortsModel.refresh() }
    // The shown Short changed: catalog prefetches the next ones and pages ahead.
    function settled(index) {
        if (index < 0) return
        shortsModel.setCurrent(index)
        if (shortsModel.canLoadMore && index >= shortsModel.count - 5) shortsModel.loadMore()
    }

    // The pager has settled on a page (or the list changed under it): that Short becomes the
    // shown one and the session is brought in line.
    function settle() {
        if (pager.moving) return
        var i = U.shownIndex(pager.currentIndex, count)
        if (i === shownIndex) return
        shownIndex = i
        if (i >= 0) settled(i)
        sync(false)
    }

    // The Shorts after the shown one, next first: the first is resolved and held, the second only
    // resolved. Asked on every settled page and whenever the list grows (the same ids start nothing).
    function askAhead() {
        if (shownIndex < 0) return
        var rows = []
        for (var i = shownIndex + 1; i < count && rows.length < 2; ++i) rows.push(row(i))
        var ids = U.aheadIds(rows, -1)
        if (ids.length > 0) playback.shortsAhead(ids)
    }

    // Tells the session what the pager wants (ShortsUi.sessionAction): open the Short on screen,
    // hold it while the section is covered, release it when it is back. The session's own events
    // (a call, a pause by the system) never reopen anything: only the user's own moves do.
    function sync(fromSession) {
        var action = U.sessionAction({ viewActive: active, currentId: shownId, sessionId: playback.videoId,
                                       sessionShort: playback.shortActive, holding: holding })
        if (fromSession && action === "open") return
        if (action === "open") {
            holding = false
            playback.openShort(shownId, U.hintsFor(shownRow))
            askAhead()
        } else if (action === "hold") {
            holding = true
            playback.holdShort("away")
        } else if (action === "release") {
            holding = false
            playback.releaseShort("away")
        }
    }
    // A tap is the user's own pause: the session keeps it across a hold and a release.
    function togglePause() { if (shownId !== "") playback.togglePlayback() }

    // The display stays on while a Short plays (the shell's keep-display-on wants the picture to
    // be visible). The position needs no fine cadence: the session sends one update a second
    // while the controls are hidden, and the progress line grows between them.
    function syncVisible() {
        if (active && appActive && playback.hasVideo) window.videoVisible = true
        else if (!window.watchPageOnTop) window.videoVisible = false
    }
    onActiveChanged: {
        syncVisible()
        if (active) {
            playback.setControlsVisible(false)
            if (U.shouldRefresh(count, busy, leftAt, new Date().getTime(), 600000)) refresh()
            settle()
        } else {
            leftAt = new Date().getTime()
        }
        sync(false)
    }
    onAppActiveChanged: syncVisible()
    onCountChanged: {
        if (shownIndex >= count) shownIndex = -1
        settle()
        askAhead()
    }
    Connections {
        target: playback
        onPlayerChanged: view.sync(true)
        onDetailsChanged: { view.syncVisible(); view.sync(true) }
    }
    Component.onDestruction: {
        if (!window.watchPageOnTop) window.videoVisible = false
    }

    // ---- What the rail and the bottom block do ----
    // A short line over the picture: the answer to a button that cannot act (like), or the share
    // sheet's own message. Silica has no notice component in this SDK.
    property string noticeText: ""
    function notice(text) { noticeText = text; noticeTimer.restart() }
    Timer { id: noticeTimer; interval: 3000; onTriggered: view.noticeText = "" }
    WatchActions { id: actions }
    Connections {
        target: actions
        onMessageChanged: if (actions.message !== "") view.notice(actions.message)
    }
    // The Short as a library row (the add-to-playlist dialog, the row menu).
    function videoMap(r) {
        var seconds = r.durationSeconds > 0 ? r.durationSeconds : -1
        if (playback.videoId === r.videoId && playback.duration > 0) seconds = Math.round(playback.duration / 1000)
        return { videoId: r.videoId, title: r.title || "", channel: r.channel || "", channelId: r.channelId || "",
                 thumbnail: r.thumbnail || "", durationSeconds: seconds }
    }
    function openComments(r) {
        // An empty token: the comments model fetches the video's watch data itself.
        pageStack.push(Qt.resolvedUrl("../pages/CommentsPage.qml"), { videoId: r.videoId, token: "" })
    }
    function openChannel(r) { pageStack.push(Qt.resolvedUrl("../pages/ChannelPage.qml"), { reference: r.channelId }) }

    // Catalog holds the details of the Short on screen and the next one until the first video bytes
    // arrive (2.5 s at most otherwise): tell it when the Short on screen plays.
    Binding {
        target: shortsModel
        property: "playbackRunning"
        value: view.shownId !== "" && playback.playing && playback.videoId === view.shownId
    }
    Component.onCompleted: {
        if (active && U.shouldRefresh(count, busy, 0, 0, 600000)) refresh()
        settle()
    }

    SilicaListView {
        id: pager
        objectName: "shortsPager"
        anchors.fill: parent
        model: shortsModel
        orientation: ListView.Vertical
        snapMode: ListView.SnapOneItem
        highlightRangeMode: ListView.StrictlyEnforceRange
        boundsBehavior: Flickable.StopAtBounds
        clip: true
        onMovementEnded: view.settle()
        onCurrentIndexChanged: view.settle()

        delegate: ListItem {
            id: page
            // The row's fields, read again whenever any row's data changes (see dataStamp).
            readonly property var entry: view.dataStamp >= 0 ? (view.row(index) || ({})) : ({})
            readonly property string vid: entry.videoId || ""
            readonly property bool isShown: index === view.shownIndex
            // The shared player holds this Short.
            readonly property bool sessionHere: vid !== "" && playback.videoId === vid
            readonly property bool failedHere: sessionHere && playback.state === PlaybackSession.Failed
            // A channel the app can open and subscribe to: a channel id (popular Shorts have none
            // until their details arrive).
            readonly property bool channelValid: (entry.channelId || "").length === 24 && (entry.channelId || "").indexOf("UC") === 0
            // Almost no Short has a description of its own; when one does, it opens under the title.
            readonly property bool hasDescription: !!entry.description
            property bool descExpanded: false
            onIsShownChanged: if (!isShown) descExpanded = false
            width: pager.width
            contentHeight: pager.height
            // A press must not flash over a playing video.
            highlightedColor: "transparent" // theme-exempt: no press flash over the picture
            menu: Component { VideoMenuItems { video: page.entry } }
            // A tap pauses or resumes the Short on screen; a long press opens the shared row menu.
            onClicked: if (page.isShown) view.togglePause()
            Accessible.role: Accessible.Button
            Accessible.name: view.userPaused
                             //: Accessible name of the play button
                             ? qsTranslate("PlayerControls", "Play")
                             //: Accessible name of the pause button
                             : qsTranslate("PlayerControls", "Pause")

            Rectangle {
                anchors.fill: parent
                color: "black" // theme-exempt: letterbox behind a Short's picture
            }
            Image {
                anchors.fill: parent
                source: page.entry.thumbnail || ""
                sourceSize.height: height
                fillMode: Image.PreserveAspectCrop
                asynchronous: true
                clip: true
            }
            // The live picture, only on the Short the pager has settled on.
            Loader {
                id: live
                objectName: "shortsLive"
                anchors.fill: parent
                active: page.isShown && view.active
                visible: U.surfaceShown({ sessionHere: page.sessionHere, sessionState: playback.state,
                                          hasFrame: live.item !== null && live.item.hasFrame === true })
                sourceComponent: Component {
                    VideoSurfaceItem {
                        objectName: "shortsSurface"
                        source: playback.mediaObject
                        // A vertical Short fills the screen; a wider one is shown whole.
                        fillMode: U.cropsToFill(videoAspect) ? VideoSurfaceItem.PreserveAspectCrop
                                                             : VideoSurfaceItem.PreserveAspectFit
                        // Presents frames while the pager is on screen; hidden, it keeps the last one.
                        active: view.active
                    }
                }
            }
            // Dark gradient under the text, over the picture, in every ambience.
            Rectangle {
                anchors { left: parent.left; right: parent.right; bottom: parent.bottom }
                height: parent.height * 0.35
                gradient: Gradient {
                    GradientStop { position: 0.0; color: "transparent" }
                    GradientStop { position: 1.0; color: Qt.rgba(0, 0, 0, 0.75) } // theme-exempt: scrim over a Short
                }
            }
            // Starting: the poster with a spinner until the first frame.
            BusyIndicator {
                objectName: "shortsBusy"
                anchors.centerIn: parent
                size: BusyIndicatorSize.Medium
                color: Theme.lightPrimaryColor
                running: page.isShown && view.active && !live.visible && !page.failedHere
                visible: running
            }
            // Paused by a tap.
            Icon {
                objectName: "shortsPauseMark"
                anchors.centerIn: parent
                visible: page.isShown && view.userPaused
                source: "image://theme/icon-l-play"
                color: Theme.lightPrimaryColor
            }
            // A Short that cannot play: the reason and a way to try again; the pager still moves on.
            Column {
                objectName: "shortsError"
                visible: page.failedHere
                anchors { left: parent.left; right: parent.right; verticalCenter: parent.verticalCenter
                          leftMargin: Theme.horizontalPageMargin; rightMargin: Theme.horizontalPageMargin }
                spacing: Theme.paddingMedium
                Label {
                    width: parent.width
                    text: playback.errorText
                    wrapMode: Text.Wrap
                    horizontalAlignment: Text.AlignHCenter
                    font.pixelSize: Theme.fontSizeSmall
                    color: Theme.lightPrimaryColor
                }
                Button {
                    anchors.horizontalCenter: parent.horizontalCenter
                    visible: playback.retryAvailable
                    color: Theme.lightPrimaryColor
                    //: Button under a playback error that tries the video again
                    text: qsTranslate("VideoPage", "Retry")
                    onClicked: playback.retry()
                }
            }
            // Everything over the picture draws light on dark in every ambience, Silica's own
            // controls (Subscribe) included.
            SilicaItem {
                anchors.fill: parent
                palette.colorScheme: Theme.LightOnDark

                // ---- Right rail: like, comments, save, share (docs/DESIGN.md §3.5) ----
                Column {
                    id: rail
                    objectName: "shortsRail"
                    anchors { right: parent.right; rightMargin: Theme.paddingSmall
                              bottom: parent.bottom; bottomMargin: Theme.paddingLarge * 2 }
                    spacing: Theme.paddingMedium
                    // Liking needs an account feature the app does not have: the count shows and a
                    // tap says so. The button stays, so the rail keeps its shape.
                    ShortsAction {
                        objectName: "shortsLike"
                        iconSource: "image://theme/icon-m-like"
                        count: page.entry.likeText || ""
                        label: count !== ""
                               //: Like count of a video, %1 is a compact number
                               ? qsTranslate("VideoPage", "%1 likes").arg(count)
                               //: Accessible name of the like button of a Short that shows no count
                               : qsTr("Like")
                        //: Shown over a Short when its like button is tapped: SailTube cannot like videos yet
                        onClicked: view.notice(qsTr("Liking videos is not available yet"))
                    }
                    // Comments: the existing page, which pauses the Short while it is open. Switched
                    // off for a Short whose creator turned them off.
                    ShortsAction {
                        objectName: "shortsComments"
                        iconSource: "image://theme/icon-m-chat"
                        count: page.entry.commentText || ""
                        inert: page.entry.commentsDisabled === true
                        //: Comments section header without a known count
                        label: qsTranslate("CommentsSection", "Comments")
                        onClicked: if (!inert) view.openComments(page.entry)
                    }
                    ShortsAction {
                        objectName: "shortsSave"
                        iconSource: "image://theme/icon-m-add"
                        //: Menu entry adding the current video to a playlist
                        label: qsTranslate("VideoPage", "Add to playlist")
                        onClicked: pageStack.push(Qt.resolvedUrl("../pages/AddToPlaylistDialog.qml"), { video: view.videoMap(page.entry) })
                    }
                    ShortsAction {
                        objectName: "shortsShare"
                        iconSource: "image://theme/icon-m-share"
                        //: Button sharing the video link through the system share sheet
                        label: qsTranslate("VideoPage", "Share")
                        onClicked: actions.share(Links.shortVideo(page.vid), page.entry.title || "")
                    }
                }

                // ---- Bottom left: channel and Subscribe, the description, the related video ----
                // Every part keeps its room whether or not its data has arrived (a popular Short
                // has no channel or description until its details load), so nothing jumps.
                Column {
                    id: info
                    objectName: "shortsInfo"
                    anchors {
                        left: parent.left; leftMargin: Theme.horizontalPageMargin
                        right: rail.left; rightMargin: Theme.paddingMedium
                        bottom: parent.bottom; bottomMargin: Theme.paddingLarge
                    }
                    spacing: Theme.paddingSmall

                    Item {
                        width: parent.width
                        height: Theme.itemSizeSmall
                        // Round avatar; a tinted disc while the channel is not known yet.
                        Item {
                            id: avatarSlot
                            anchors { left: parent.left; verticalCenter: parent.verticalCenter }
                            width: Theme.iconSizeMedium
                            height: width
                            Rectangle {
                                anchors.fill: parent
                                visible: !page.entry.channelAvatar
                                radius: width / 2
                                color: Qt.rgba(1, 1, 1, 0.2) // theme-exempt: avatar placeholder over a Short
                                Label {
                                    anchors.centerIn: parent
                                    text: Rows.initial(page.entry.channel || "")
                                    font.pixelSize: Theme.fontSizeMedium
                                    color: Theme.lightPrimaryColor
                                }
                            }
                            Loader {
                                id: avatarLoader
                                anchors.fill: parent
                                active: !!page.entry.channelAvatar
                                source: active ? "Avatar.qml" : ""
                            }
                            Binding {
                                target: avatarLoader.item
                                when: avatarLoader.status === Loader.Ready
                                property: "source"
                                value: page.entry.channelAvatar || ""
                            }
                        }
                        Label {
                            id: channelName
                            objectName: "shortsChannel"
                            anchors {
                                left: avatarSlot.right; leftMargin: Theme.paddingMedium
                                right: subscribe.visible ? subscribe.left : parent.right
                                rightMargin: subscribe.visible ? Theme.paddingMedium : 0
                                verticalCenter: parent.verticalCenter
                            }
                            text: page.entry.channel || ""
                            truncationMode: TruncationMode.Fade
                            font.pixelSize: Theme.fontSizeSmall
                            font.bold: true
                            color: Theme.lightPrimaryColor
                            style: Text.Outline
                            styleColor: Qt.rgba(0, 0, 0, 0.6) // theme-exempt: outline of text over the picture
                        }
                        // A tap on the avatar or the name opens the channel.
                        MouseArea {
                            objectName: "shortsChannelArea"
                            anchors { left: parent.left; right: channelName.right; top: parent.top; bottom: parent.bottom }
                            enabled: page.channelValid
                            onClicked: view.openChannel(page.entry)
                        }
                        SubscribeButton {
                            id: subscribe
                            objectName: "shortsSubscribe"
                            anchors { right: parent.right; verticalCenter: parent.verticalCenter }
                            visible: page.channelValid
                            channelId: page.entry.channelId || ""
                            channelName: page.entry.channel || ""
                            avatarUrl: page.entry.channelAvatar || ""
                            preferredWidth: Theme.buttonWidthExtraSmall
                        }
                    }

                    // The caption is the Short's title (almost no Short has a description of its own):
                    // two lines, a tap opens it in place, with the description under it when there is
                    // one. The tap area is a sibling of the text, not its child: it reads `truncated`,
                    // which lays the text out.
                    Item {
                        width: parent.width
                        height: description.height
                        Label {
                            id: description
                            objectName: "shortsDescription"
                            width: parent.width
                            text: page.descExpanded && page.hasDescription
                                  ? (page.entry.title || "") + "\n\n" + page.entry.description
                                  : (page.entry.title || "")
                            wrapMode: Text.Wrap
                            maximumLineCount: page.descExpanded ? 8 : 2
                            elide: Text.ElideRight
                            textFormat: Text.PlainText
                            font.pixelSize: Theme.fontSizeSmall
                            color: Theme.lightPrimaryColor
                            style: Text.Outline
                            styleColor: Qt.rgba(0, 0, 0, 0.6) // theme-exempt: outline of text over the picture
                        }
                        MouseArea {
                            objectName: "shortsDescriptionArea"
                            anchors.fill: parent
                            enabled: description.truncated || page.hasDescription || page.descExpanded
                            onClicked: page.descExpanded = !page.descExpanded
                        }
                    }

                    // The related video, when the Short has one; its room is kept either way.
                    Item {
                        width: parent.width
                        height: Theme.itemSizeExtraSmall
                        Rectangle {
                            objectName: "shortsRelated"
                            visible: !!page.entry.relatedVideoId
                            anchors { left: parent.left; verticalCenter: parent.verticalCenter }
                            width: Math.min(relatedRow.width + 2 * Theme.paddingMedium, parent.width)
                            height: Theme.itemSizeExtraSmall * 0.8
                            radius: height / 2
                            color: Qt.rgba(0, 0, 0, relatedArea.pressed ? 0.7 : 0.5) // theme-exempt: chip over a Short
                            Row {
                                id: relatedRow
                                anchors.centerIn: parent
                                spacing: Theme.paddingSmall
                                // The related video's picture when the row has one (a vertical frame when
                                // it is a Short, cropped to the chip), else the video icon.
                                Item {
                                    anchors.verticalCenter: parent.verticalCenter
                                    width: Theme.iconSizeMedium
                                    height: width
                                    Image {
                                        anchors.fill: parent
                                        visible: !!page.entry.relatedVideoThumbnail
                                        source: page.entry.relatedVideoThumbnail || ""
                                        sourceSize.width: width
                                        asynchronous: true
                                        fillMode: Image.PreserveAspectCrop
                                        clip: true
                                    }
                                    Icon {
                                        anchors.centerIn: parent
                                        visible: !page.entry.relatedVideoThumbnail
                                        source: "image://theme/icon-m-video"
                                        width: Theme.iconSizeSmall
                                        height: width
                                        sourceSize.width: width
                                        sourceSize.height: height
                                        color: Theme.lightPrimaryColor
                                    }
                                }
                                Label {
                                    anchors.verticalCenter: parent.verticalCenter
                                    width: Math.min(implicitWidth, info.width - Theme.iconSizeMedium - 4 * Theme.paddingMedium)
                                    text: page.entry.relatedVideoTitle || ""
                                    truncationMode: TruncationMode.Fade
                                    font.pixelSize: Theme.fontSizeExtraSmall
                                    color: Theme.lightPrimaryColor
                                }
                            }
                            MouseArea {
                                id: relatedArea
                                anchors.fill: parent
                                Accessible.role: Accessible.Button
                                Accessible.name: page.entry.relatedVideoTitle || ""
                                // The related video opens on the normal watch page.
                                onClicked: window.openVideo(page.entry.relatedVideoId, { title: page.entry.relatedVideoTitle || "" })
                            }
                        }
                    }
                }
            }
        }

        ViewPlaceholder {
            enabled: view.count === 0 && !view.busy
            //: Shown in the Shorts section when the subscribed channels have no Shorts
            text: shortsModel.errorText !== "" ? shortsModel.errorText : qsTr("No Shorts")
            //: Hint in the empty Shorts section
            hintText: qsTr("Shorts from your subscriptions appear here.")
        }
        BusyIndicator {
            anchors.centerIn: parent
            size: BusyIndicatorSize.Large
            running: view.count === 0 && view.busy
        }
    }

    // The notice: a short line over the picture.
    Rectangle {
        objectName: "shortsNotice"
        visible: view.noticeText !== ""
        anchors { horizontalCenter: parent.horizontalCenter; bottom: parent.bottom; bottomMargin: Theme.itemSizeLarge * 2 }
        width: Math.min(noticeLabel.implicitWidth, parent.width - 4 * Theme.paddingLarge) + 2 * Theme.paddingLarge
        height: noticeLabel.height + 2 * Theme.paddingSmall
        radius: height / 2
        color: Qt.rgba(0, 0, 0, 0.75) // theme-exempt: notice over a Short
        Label {
            id: noticeLabel
            anchors.centerIn: parent
            width: Math.min(implicitWidth, view.width - 4 * Theme.paddingLarge)
            text: view.noticeText
            wrapMode: Text.Wrap
            horizontalAlignment: Text.AlignHCenter
            font.pixelSize: Theme.fontSizeSmall
            color: Theme.lightPrimaryColor
        }
    }

    // The thin progress line along the bottom edge: it fills from the left in both directions, like
    // every progress in the app (docs/DESIGN.md §7.8). The session sends one position a second, and
    // the line grows linearly over the second between two of them; a looping Short's position jumps
    // back to 0, and the line jumps with it (ShortsUi.progressMove), never animating backwards.
    Rectangle {
        id: progressTrack
        objectName: "shortsProgress"
        visible: view.shownId !== "" && playback.videoId === view.shownId
        x: 0
        y: parent.height - height
        width: parent.width
        height: Math.max(2, Math.round(Theme.paddingSmall / 2))
        color: Qt.rgba(1, 1, 1, 0.3) // theme-exempt: track over a Short
        readonly property real fraction: U.progressFraction(playback.position, playback.duration,
                                                            view.shownRow ? view.shownRow.durationSeconds : -1)
        onFractionChanged: {
            var target = width * fraction
            if (U.progressMove(progressFill.width, target) === "jump") { progressGrow.stop(); progressFill.width = target }
            else { progressGrow.from = progressFill.width; progressGrow.to = target; progressGrow.restart() }
        }
        Rectangle {
            id: progressFill
            objectName: "shortsProgressFill"
            height: parent.height
            width: 0
            color: Theme.lightPrimaryColor
        }
        NumberAnimation { id: progressGrow; target: progressFill; property: "width"; duration: 1000; easing.type: Easing.Linear }
    }
}
