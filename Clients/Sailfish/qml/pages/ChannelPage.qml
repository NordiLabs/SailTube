// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Channel page (DSC-12, DSC-17, DSC-18): header, tabs Videos / Shorts / Live / Playlists,
// badged rows and the hidden-row count. Logic lives in ChannelModel.
// Push with pageStack.push(Qt.resolvedUrl("ChannelPage.qml"), {reference: "@handle" or UC id or URL}).
import QtQuick 2.0
import Sailfish.Silica 1.0
import org.nordilabs.sailtube 1.0
import "../components"
import "../components/Format.js" as Format
import "../logic/Rows.js" as Rows
import "../logic/ShareLinks.js" as Links

Page {
    id: page
    property string reference
    allowedOrientations: Orientation.All
    // The address to share; empty until the channel's id or handle is known (no Share entry then).
    readonly property string shareLink: Links.channel(channelModel.channelId, channelModel.handle)

    WatchActions { id: actions }

    ChannelModel {
        id: channelModel
        // The shared discovery settings (ux/core component) drive these once they exist.
        hideShorts: window.settingValue("content/hideShorts") === true   // SET-01, set once per page
        hideMembersOnly: typeof appSettings.hideMembersOnly === "boolean" ? appSettings.hideMembersOnly : false
    }

    Component.onCompleted: channelModel.open(page.reference)

    // Rows, the loading skeleton or the placeholder; never blank (NFR-PERF-01).
    readonly property string listState: Rows.emptyState(channelModel.count, channelModel.busy, false)

    // A channel row for the shared menu; rows of the channel's own lists may omit the channel.
    function menuRow(index) {
        var row = channelModel.get(index)
        if (!row.channel) row.channel = channelModel.title
        return row
    }

    function tabTitle(key) {
        //: Channel tab with the regular uploads
        if (key === "videos") return qsTr("Videos")
        //: Channel tab with the channel's Shorts
        if (key === "shorts") return qsTr("Shorts")
        //: Channel tab with live and past streams
        if (key === "streams") return qsTr("Live")
        //: Channel tab with the channel's playlists
        if (key === "playlists") return qsTr("Playlists")
        //: Channel tab with albums and releases (music channels)
        if (key === "releases") return qsTr("Releases")
        return key
    }

    SilicaListView {
        id: list
        VisibleRows { view: list }   // E12: rows on screen for tapIntent
        anchors.fill: parent
        model: channelModel
        PullDownMenu {
            MenuItem {
                objectName: "shareChannel"
                visible: page.shareLink !== ""
                //: Button sharing the video link through the system share sheet
                text: qsTranslate("VideoPage", "Share")
                onClicked: actions.share(page.shareLink, channelModel.title)
            }
            MenuItem {
                text: channelModel.hideMembersOnly
                      //: Menu entry showing members-only videos again
                      ? qsTr("Show members-only videos")
                      //: Menu entry hiding members-only videos
                      : qsTr("Hide members-only videos")
                onClicked: channelModel.hideMembersOnly = !channelModel.hideMembersOnly
            }
            MenuItem {
                text: channelModel.hideShorts
                      //: Menu entry showing Shorts again in the channel lists
                      ? qsTr("Show Shorts")
                      //: Menu entry hiding confirmed Shorts from the channel lists
                      : qsTr("Hide Shorts")
                onClicked: channelModel.hideShorts = !channelModel.hideShorts
            }
            MenuItem {
                //: Menu entry reloading the channel page
                text: qsTr("Refresh")
                onClicked: channelModel.refresh()
            }
        }

        header: Column {
            width: list.width
            spacing: Theme.paddingMedium

            PageHeader {
                //: Fallback title of a channel page while it loads
                title: channelModel.title !== "" ? channelModel.title : qsTr("Channel")
                description: channelModel.handle
            }

            Row {
                x: Theme.horizontalPageMargin
                width: parent.width - 2 * Theme.horizontalPageMargin
                spacing: Theme.paddingLarge
                visible: channelModel.title !== ""

                // Round, like every channel picture (docs/DESIGN.md §4.2).
                Avatar {
                    width: Theme.itemSizeLarge
                    height: width
                    source: channelModel.avatarUrl
                }
                Column {
                    anchors.verticalCenter: parent.verticalCenter
                    width: parent.width - Theme.itemSizeLarge - Theme.paddingLarge
                    Label {
                        width: parent.width
                        visible: text !== ""
                        // YouTube's localized text first, else generated in the same language.
                        text: channelModel.subscriberText !== "" ? channelModel.subscriberText
                              : channelModel.subscriberCount >= 0 && typeof catalogText !== "undefined"
                                ? catalogText.subscribers(channelModel.subscriberCount)
                                : channelModel.subscriberCount >= 0
                                  //: Subscriber count on the channel header, %1 is a compact number such as 1.2M
                                  ? qsTr("%1 subscribers").arg(Format.count(channelModel.subscriberCount, Qt.locale())) : ""
                        truncationMode: TruncationMode.Fade
                        color: Theme.highlightColor
                    }
                    Label {
                        width: parent.width
                        visible: text !== ""
                        text: channelModel.videoCountText !== "" ? channelModel.videoCountText
                              : channelModel.videoCount >= 0 && typeof catalogText !== "undefined"
                                ? catalogText.videos(channelModel.videoCount)
                                : channelModel.videoCount >= 0
                                  //: Video count on the channel header, %1 is a compact number such as 6.2K
                                  ? qsTr("%1 videos").arg(Format.count(channelModel.videoCount, Qt.locale())) : ""
                        truncationMode: TruncationMode.Fade
                        font.pixelSize: Theme.fontSizeSmall
                        color: Theme.secondaryHighlightColor
                    }
                }
            }

            // About (docs/DESIGN.md §4.8), as on the official app: the channel's description on a
            // tinted card, two lines until a tap opens it in place.
            BackgroundItem {
                id: aboutCard
                objectName: "channelAbout"
                property bool expanded: false
                visible: channelModel.description !== ""
                width: parent.width
                height: aboutBox.height
                highlightedColor: "transparent" // theme-exempt: the box below shows the press
                enabled: expanded || aboutText.truncated
                onClicked: expanded = !expanded
                Rectangle {
                    id: aboutBox
                    anchors {
                        left: parent.left; leftMargin: Theme.horizontalPageMargin
                        right: parent.right; rightMargin: Theme.horizontalPageMargin
                    }
                    height: aboutColumn.height + 2 * Theme.paddingMedium
                    radius: Theme.paddingMedium
                    color: Theme.rgba(Theme.highlightBackgroundColor, aboutCard.down ? 0.3 : 0.1)
                    Column {
                        id: aboutColumn
                        anchors {
                            left: parent.left; right: parent.right; top: parent.top
                            margins: Theme.paddingMedium
                        }
                        spacing: Theme.paddingSmall
                        Label {
                            id: aboutText
                            width: parent.width
                            text: channelModel.description
                            textFormat: Text.PlainText
                            wrapMode: Text.Wrap
                            maximumLineCount: aboutCard.expanded ? 100000 : 2
                            elide: Text.ElideRight
                            font.pixelSize: Theme.fontSizeSmall
                            color: Theme.primaryColor
                        }
                        Label {
                            visible: aboutCard.enabled
                            text: aboutCard.expanded
                                  //: Collapses the video description to 5 lines
                                  ? qsTranslate("VideoPage", "Show less")
                                  //: Expands the collapsed video description
                                  : qsTranslate("VideoPage", "Show more")
                            font.pixelSize: Theme.fontSizeExtraSmall
                            color: Theme.highlightColor
                        }
                    }
                }
            }

            // LIB-01: subscribe from the channel page with the app's one Subscribe button (the
            // watch page's and the channel rows': same texts, remorse before unsubscribing).
            SubscribeButton {
                id: subscribeButton
                objectName: "channelPageSubscribe"
                anchors.horizontalCenter: parent.horizontalCenter
                visible: channelModel.channelId.length === 24 && channelModel.channelId.indexOf("UC") === 0
                preferredWidth: Theme.buttonWidthMedium
                channelId: channelModel.channelId
                channelName: channelModel.title
                avatarUrl: channelModel.avatarUrl
            }

            // Feedback of Share when the system share sheet is missing ("link copied").
            Label {
                x: Theme.horizontalPageMargin
                width: parent.width - 2 * Theme.horizontalPageMargin
                visible: text !== ""
                text: actions.message
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.Wrap
                font.pixelSize: Theme.fontSizeExtraSmall
                color: Theme.highlightColor
            }

            // The tabs. Each takes an equal share of the row while its title fits there; a longer
            // title ("Listas de reproducción" on a small phone) widens its tab, and the row then
            // scrolls sideways, as the official app's does: no title is ever cut (I18N-03). The
            // row rests on the current tab: at its leading end for the first one, which is the
            // right end in right-to-left layouts (docs/DESIGN.md §7.8).
            SilicaFlickable {
                id: tabStrip
                objectName: "channelTabs"
                readonly property bool mirrored: tabStrip.LayoutMirroring.enabled
                readonly property string current: channelModel.tab
                width: parent.width
                height: tabRow.height
                contentWidth: Math.max(width, tabRow.width + 2 * Theme.horizontalPageMargin)
                flickableDirection: Flickable.HorizontalFlick
                boundsBehavior: Flickable.StopAtBounds
                clip: true

                // Brings the current tab, with the page margin beside it, into view. Never while
                // the user is moving the row.
                function revealCurrent() {
                    if (moving) return
                    for (var i = 0; i < tabRow.children.length; ++i) {
                        var tab = tabRow.children[i]
                        // By key: a tab's own `current` may not have followed the model yet.
                        if (tab.tabKey !== current) continue
                        var left = tabRow.x + tab.x - Theme.horizontalPageMargin
                        var right = left + tab.width + 2 * Theme.horizontalPageMargin
                        if (left < contentX) contentX = Math.max(0, left)
                        else if (right > contentX + width) contentX = Math.max(0, Math.min(contentWidth - width, right - width))
                        return
                    }
                }
                onCurrentChanged: revealCurrent()
                onMirroredChanged: revealCurrent()
                onContentWidthChanged: revealCurrent()
                onWidthChanged: revealCurrent()
                Component.onCompleted: revealCurrent()

                Row {
                    id: tabRow
                    // Anchored, so mirroring moves it to the right end of the content.
                    anchors { left: parent.left; leftMargin: Theme.horizontalPageMargin }
                    property var keys: channelModel.tabs.length > 0 ? channelModel.tabs : ["videos", "shorts", "streams", "playlists"]
                    readonly property real share: (tabStrip.width - 2 * Theme.horizontalPageMargin) / keys.length
                    Repeater {
                        model: tabRow.keys
                        BackgroundItem {
                            readonly property string tabKey: modelData
                            readonly property bool current: channelModel.tab === modelData
                            width: Math.max(tabRow.share, tabLabel.implicitWidth + 2 * Theme.paddingMedium)
                            height: Math.max(Theme.itemSizeSmall, tabLabel.implicitHeight + 2 * Theme.paddingMedium)
                            highlighted: down || current
                            onClicked: channelModel.tab = modelData
                            Accessible.role: Accessible.PageTab
                            Accessible.name: tabLabel.text
                            Label {
                                id: tabLabel
                                anchors.centerIn: parent
                                text: page.tabTitle(modelData)
                                font.pixelSize: Theme.fontSizeSmall
                                color: parent.current ? Theme.highlightColor : Theme.primaryColor
                            }
                        }
                    }
                }
            }

            Label {
                x: Theme.horizontalPageMargin
                width: parent.width - 2 * Theme.horizontalPageMargin
                visible: channelModel.fallbackTab
                wrapMode: Text.Wrap
                font.pixelSize: Theme.fontSizeExtraSmall
                color: Theme.secondaryColor
                //: Note on a channel page that opened its playlists because the channel has no Videos tab
                text: qsTr("This channel has no Videos tab.")
            }
            // NFR-PERF-01: the loading skeleton under the header and tabs (docs/DESIGN.md §5.1).
            SkeletonList {
                shown: page.listState === "skeleton"
                viewHeight: list.height
            }
        }

        // The shared row (docs/DESIGN.md §4.1): watched and download badges, the row menu.
        delegate: VideoListItem {
            id: row
            readonly property bool isPlaylist: model.kind === "playlist"
            videoId: isPlaylist ? "" : model.videoId
            menu: isPlaylist ? null : rowMenu
            videoTitle: model.title
            // The channel is the page itself, so the second line carries views and age.
            channelName: {
                var parts = []
                // YouTube's localized text first, else generated in the same language.
                if (model.viewText !== "") parts.push(model.viewText)
                else if (model.viewCount >= 0 && typeof catalogText !== "undefined") parts.push(catalogText.views(model.viewCount))
                //: Number of views of a video, %1 is a compact number such as 1.2M
                else if (model.viewCount >= 0) parts.push(qsTr("%1 views").arg(Format.count(model.viewCount, Qt.locale())))
                if (model.published !== "") parts.push(model.published)
                return parts.join(" · ")
            }
            thumbnailUrl: model.thumbnail
            isLive: model.live
            durationText: isPlaylist
                          ? (model.videoCount >= 0
                             //: Number of videos in a playlist on its thumbnail
                             ? qsTr("%n videos", "", model.videoCount) : model.videoCountText)
                          : model.duration
            badgeText: {
                var badges = []
                //: Badge of a video only channel members can watch
                if (model.membersOnly) badges.push(qsTr("Members only"))
                //: Badge of an age-restricted or otherwise restricted video
                if (model.restricted) badges.push(qsTr("Restricted"))
                //: Badge of a scheduled live stream or premiere
                if (model.upcoming) badges.push(qsTr("Upcoming"))
                //: Badge of a confirmed YouTube Short
                if (model.isShort && channelModel.tab !== "shorts") badges.push(qsTr("Short"))
                return badges.join(" · ")
            }

            onClicked: {
                if (isPlaylist) {
                    pageStack.push(Qt.resolvedUrl("PlaylistPreviewPage.qml"), {playlistId: model.playlistId})
                } else {
                    window.openVideo(model.videoId, channelModel.get(index))
                }
            }
            Component {
                id: rowMenu
                VideoMenuItems { video: page.menuRow(index) }
            }
        }

        footer: Column {
            width: list.width
            spacing: Theme.paddingSmall
            Item { width: 1; height: Theme.paddingLarge }
            BusyIndicator {
                anchors.horizontalCenter: parent.horizontalCenter
                running: channelModel.busy && channelModel.count > 0
                visible: running
                size: BusyIndicatorSize.Medium
            }
            Label {
                x: Theme.horizontalPageMargin
                width: parent.width - 2 * Theme.horizontalPageMargin
                visible: channelModel.hiddenCount > 0
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.Wrap
                font.pixelSize: Theme.fontSizeExtraSmall
                color: Theme.secondaryColor
                //: Footer of a list: how many rows the Shorts / members-only filters hid
                text: qsTr("%n hidden by filters", "", channelModel.hiddenCount)
            }
            // The next page failed: the error with a tap to retry, as in every list (§5.3).
            RetryRow {
                visible: channelModel.errorText !== "" && channelModel.count > 0
                errorText: channelModel.errorText
                onClicked: channelModel.loadMore()
            }
            Item { width: 1; height: Theme.paddingLarge }
        }

        // Errors never retry on scroll: only a tap on the inline error does.
        onAtYEndChanged: if (atYEnd && channelModel.canLoadMore && channelModel.errorText === "") channelModel.loadMore()

        ViewPlaceholder {
            enabled: page.listState === "placeholder"
            text: channelModel.errorText !== "" ? channelModel.errorText
                  : channelModel.hiddenCount > 0
                    //: Shown when every row of a channel tab was hidden by the filters
                    ? qsTr("Everything here is hidden by filters")
                    : channelModel.tab === "playlists" || channelModel.tab === "releases"
                      //: Shown on a channel tab without playlists
                      ? qsTr("No playlists")
                      //: Shown on a channel tab without videos
                      : qsTr("No videos")
            hintText: channelModel.errorText !== ""
                      //: Hint under a channel page error
                      ? qsTr("Pull down to refresh.") : ""
        }

        VerticalScrollDecorator { }
    }
}
