// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Recommended (DSC-04): browse FEwhat_to_watch for the signed-in account. Signed out, the page
// explains that recommendations need an account.
import QtQuick 2.6
import Sailfish.Silica 1.0
import "../logic/Rows.js" as Rows

// Recommended view (DSC-04).
// Shared by pages/RecommendedPage.qml (standalone) and the main page section; `standalone` adds the
// page header and the navigation entries the section bar already offers.
Item {
    id: page
    property bool standalone: false
    // True while the view is on screen (page active, or the current section).
    property bool active: true
    // NFR-PERF-01: the first rows on screen end the start-up path ("startup content").
    readonly property bool hasRows: !!list.model && list.model.count > 0
    onHasRowsChanged: if (hasRows && active && typeof startupTiming !== "undefined") startupTiming.mark("content")

    // NFR-PERF-01: rows, the loading skeleton, or the placeholder; never blank.
    readonly property string listState: Rows.emptyState(recommendedModel.count, recommendedModel.busy, false)
    onListStateChanged: if (listState === "skeleton" && active && typeof startupTiming !== "undefined") startupTiming.mark("skeleton")

    // DSC-04a: signed out, Home shows the public "Popular" list; signed in, the recommendations.
    Component.onCompleted: {
        if (recommendedModel.count === 0) recommendedModel.reload()
        refreshContinue()
    }

    // A second tap on the section's bar entry (docs/DESIGN.md §2.2).
    function scrollToTop() { list.scrollToTop() }

    // Continue watching (docs/DESIGN.md §4.9): the unfinished videos among the newest history
    // rows. Read when the view comes on screen (a video watched meanwhile changes it), and handed
    // to the shelf only when the content changed.
    property var continueRows: []
    function refreshContinue() {
        var recent = []
        if (typeof historyModel !== "undefined" && historyModel) {
            var n = Math.min(historyModel.count, 40)
            for (var i = 0; i < n; ++i) recent.push(historyModel.get(i))
        }
        var next = Rows.continueWatching(recent, 10)
        if (Rows.continueKey(next) !== Rows.continueKey(continueRows)) continueRows = next
    }
    onActiveChanged: if (active) refreshContinue()

    SilicaListView {
        id: list
        VisibleRows { view: list }   // E12: rows on screen for tapIntent
        anchors.fill: parent
        model: recommendedModel

        PullDownMenu {
            busy: recommendedModel.busy
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
                visible: page.standalone
                //: Menu entry opening the subscriptions feed
                text: qsTr("Subscriptions")
                onClicked: pageStack.push(Qt.resolvedUrl("../pages/FeedPage.qml"))
            }
            MenuItem {
                visible: page.standalone
                //: Menu entry opening the search page
                text: qsTr("Search")
                onClicked: pageStack.push(Qt.resolvedUrl("../pages/SearchPage.qml"))
            }
            MenuItem {
                enabled: !recommendedModel.busy
                //: Menu entry reloading the recommendations
                text: qsTr("Refresh")
                onClicked: recommendedModel.reload()
            }
        }

        // The section bar is the header inside the main page; the skeleton sits under it while
        // the first page loads.
        header: Column {
            width: list.width
            // Every section has a header now that the navigation bar sits at the bottom.
            Item {
                width: parent.width
                height: pageHeader.height
                PageHeader {
                    id: pageHeader
                    title: recommendedModel.popular === true
                           //: Title of Home while signed out: the most viewed videos of this week in the user's region
                           ? qsTr("Popular")
                           //: Title of the recommendations page
                           : qsTr("Recommended")
                }
            }
            // Continue watching: its file is loaded only when a video was left unfinished.
            Loader {
                id: continueLoader
                objectName: "continueLoader"
                width: parent.width
                active: page.continueRows.length > 0
                source: active ? "ContinueShelf.qml" : ""
            }
            Binding {
                target: continueLoader.item
                when: continueLoader.status === Loader.Ready
                property: "rows"
                value: page.continueRows
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
            menu: Component { VideoMenuItems { video: recommendedModel.get(index) } }
            videoTitle: model.title
            channelName: model.publishedText !== "" ? model.channel + " · " + model.publishedText : model.channel
            thumbnailUrl: model.thumbnail
            durationText: model.duration
            views: model.live ? -1 : model.viewCount
            isLive: model.live
            onClicked: {
                window.openVideo(model.videoId, recommendedModel.get(index))
            }
        }

        footer: Column {
            width: list.width
            Item {
                width: parent.width
                height: recommendedModel.errorText === "" && (recommendedModel.canLoadMore || recommendedModel.busy) ? Theme.itemSizeLarge : Theme.paddingLarge
                BusyIndicator {
                    anchors.centerIn: parent
                    size: BusyIndicatorSize.Medium
                    running: recommendedModel.busy && recommendedModel.count > 0
                }
            }
            // The next page failed: the error with a tap to retry (never on scroll).
            RetryRow {
                visible: recommendedModel.errorText !== "" && recommendedModel.count > 0
                errorText: recommendedModel.errorText
                onClicked: recommendedModel.loadMore()
            }
        }

        // Errors never retry on scroll: only a tap on the inline error does (docs/DESIGN.md §5.3).
        onAtYEndChanged: if (atYEnd && recommendedModel.canLoadMore && recommendedModel.errorText === "") recommendedModel.loadMore()

        ViewPlaceholder {
            id: emptyPlaceholder
            enabled: page.listState === "placeholder"
            text: recommendedModel.errorText !== "" ? recommendedModel.errorText
                  : !recommendedModel.signedIn
                  //: Shown on the recommendations page while signed out
                  ? qsTr("Sign in for recommendations")
                  //: Shown when the account has no recommendations
                  : qsTr("No recommendations")
            hintText: !recommendedModel.signedIn
                      //: Hint on the recommendations page while signed out
                      ? qsTr("Recommendations come from a signed-in YouTube account.")
                      : qsTr("Pull down to refresh.")
        }

        PlaceholderActions {
            placeholder: emptyPlaceholder
            listState: page.listState
            errorText: recommendedModel.errorText
            canRetry: true
            onRetry: recommendedModel.reload()
        }

        VerticalScrollDecorator { }
    }
}
