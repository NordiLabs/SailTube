// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Subscriptions feed (DSC-05/06/07): the cache paints at once, due channels refresh in the
// background with a small indicator by the title, pull-down Refresh forces every channel,
// and scrolling to the end extends the feed.
import QtQuick 2.6
import Sailfish.Silica 1.0
import "../logic/Rows.js" as Rows

// Subscriptions feed view (DSC-05/06/07).
// Shared by pages/FeedPage.qml (standalone) and the main page section; `standalone` adds the
// page header and the navigation entries the section bar already offers.
Item {
    id: page
    property bool standalone: false
    // True while the view is on screen (page active, or the current section).
    property bool active: true
    // NFR-PERF-01: the first rows on screen end the start-up path ("startup content").
    readonly property bool hasRows: !!list.model && list.model.count > 0
    onHasRowsChanged: if (hasRows && active && typeof startupTiming !== "undefined") startupTiming.mark("content")

    // NFR-PERF-01: rows (cached or fresh), the loading skeleton, or the placeholder; never blank.
    readonly property string listState: Rows.emptyState(feedModel.count, feedModel.busy || feedModel.refreshing,
                                                        !refreshAsked && feedModel.channelCount > 0)
    property bool refreshAsked: false
    onListStateChanged: if (listState === "skeleton" && active && typeof startupTiming !== "undefined") startupTiming.mark("skeleton")

    onActiveChanged: if (active) { refreshAsked = true; feedModel.refresh(false) }
    Component.onCompleted: if (active) { refreshAsked = true; feedModel.refresh(false) }

    // A second tap on the section's bar entry (docs/DESIGN.md §2.2).
    function scrollToTop() { list.scrollToTop() }

    SilicaListView {
        id: list
        VisibleRows { view: list }   // E12: rows on screen for tapIntent
        anchors.fill: parent
        model: feedModel

        PullDownMenu {
            busy: feedModel.refreshing
            MenuItem {
                //: Menu entry opening the about page
                text: qsTr("About SailTube")
                onClicked: pageStack.push(Qt.resolvedUrl("../pages/AboutPage.qml"))
            }
            MenuItem {
                //: Menu entry opening the application settings
                text: qsTr("Settings")
                onClicked: pageStack.push(Qt.resolvedUrl("../pages/SettingsPage.qml"))
            }
            MenuItem {
                visible: page.standalone && (homeSettings.signedIn)
                //: Menu entry opening the recommendations of the signed-in account
                text: qsTr("Recommended")
                onClicked: pageStack.push(Qt.resolvedUrl("../pages/RecommendedPage.qml"))
            }
            MenuItem {
                visible: page.standalone
                //: Menu entry opening the search page
                text: qsTr("Search")
                onClicked: pageStack.push(Qt.resolvedUrl("../pages/SearchPage.qml"))
            }
            MenuItem {
                enabled: !feedModel.refreshing && feedModel.channelCount > 0
                //: Menu entry refreshing every subscribed channel
                text: qsTr("Refresh")
                onClicked: feedModel.refresh(true)
            }
        }

        // The section bar is the header inside the main page; the skeleton sits under it while
        // the first page loads, so it moves with the pulley.
        header: Column {
            width: list.width
            // Every section has a header now that the navigation bar sits at the bottom.
            Item {
                width: parent.width
                height: pageHeader.height
                PageHeader {
                    id: pageHeader
                    //: Title of the subscriptions feed page
                    title: qsTr("Subscriptions")
                    BusyIndicator {
                        anchors {
                            left: parent.left
                            leftMargin: Theme.horizontalPageMargin
                            verticalCenter: parent.verticalCenter
                        }
                        size: BusyIndicatorSize.ExtraSmall
                        running: feedModel.refreshing && feedModel.count > 0
                    }
                }
            }
            SkeletonList {
                cards: true
                shown: page.listState === "skeleton"
                viewHeight: list.height
            }
        }

        delegate: VideoListItem {
            // Watched progress, download badge (LIB-06, DL-04) and the shared row menu.
            // YouTube Android's cards in portrait (docs/DESIGN.md §4.1).
            cardLayout: true
            avatarUrl: model.channelAvatar !== undefined ? (model.channelAvatar || "") : ""
            largeThumbnailUrl: model.thumbnailLarge !== undefined ? (model.thumbnailLarge || "") : ""
            videoId: model.videoId
            menu: Component { VideoMenuItems { video: feedModel.get(index) } }
            videoTitle: model.title
            channelName: model.publishedText !== "" ? model.channel + " · " + model.publishedText : model.channel
            thumbnailUrl: model.thumbnail
            durationText: model.duration
            views: model.live ? -1 : model.viewCount
            isLive: model.live
            onClicked: {
                window.openVideo(model.videoId, feedModel.get(index))
            }
        }

        footer: Column {
            width: list.width
            Item {
                width: parent.width
                height: feedModel.errorText === "" && (feedModel.canLoadMore || feedModel.busy) ? Theme.itemSizeLarge : Theme.paddingLarge
                BusyIndicator {
                    anchors.centerIn: parent
                    size: BusyIndicatorSize.Medium
                    running: feedModel.busy && !feedModel.refreshing && feedModel.count > 0
                }
            }
            // The next page failed: the error with a tap to retry (never on scroll).
            RetryRow {
                visible: feedModel.errorText !== "" && feedModel.count > 0
                errorText: feedModel.errorText
                onClicked: feedModel.loadMore()
            }
        }

        // Errors never retry on scroll: only a tap on the inline error does (docs/DESIGN.md §5.3).
        onAtYEndChanged: if (atYEnd && feedModel.canLoadMore && feedModel.errorText === "") feedModel.loadMore()

        ViewPlaceholder {
            id: emptyPlaceholder
            enabled: page.listState === "placeholder"
            text: feedModel.channelCount === 0
                  //: Shown on the feed page without subscriptions
                  ? qsTr("No subscriptions")
                  : feedModel.errorText !== "" ? feedModel.errorText
                  //: Shown when the subscribed channels have no videos
                  : qsTr("No videos")
            hintText: feedModel.channelCount === 0
                      //: Hint on the feed page without subscriptions
                      ? qsTr("Subscribe to channels to see their new videos here.")
                      //: Hint on an empty or failed feed
                      : qsTr("Pull down to refresh.")
        }

        PlaceholderActions {
            placeholder: emptyPlaceholder
            listState: page.listState
            errorText: feedModel.errorText
            canRetry: feedModel.channelCount > 0
            onRetry: feedModel.refresh(true)
        }

        VerticalScrollDecorator { }
    }
}
