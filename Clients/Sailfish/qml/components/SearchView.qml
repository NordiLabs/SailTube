// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
import QtQuick 2.6
import Sailfish.Silica 1.0
import "../logic/Rows.js" as Rows

// Search view with suggestions and filters (DSC-01/02/11).
// Shared by pages/SearchPage.qml (standalone) and the main page section; `standalone` adds the
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
    readonly property string listState: Rows.emptyState(searchModel.count, searchModel.busy, false)

    // Suggestions (or the history, for an empty field) replace the results while typing.
    readonly property bool suggesting: field.activeFocus && typeof suggestionModel !== "undefined" && suggestionModel.count > 0

    // A search that starts elsewhere (a hashtag of a description) shows in the field. While the
    // user is typing, the field is theirs.
    readonly property string modelQuery: searchModel.query
    onModelQueryChanged: if (modelQuery !== "" && !field.activeFocus && field.text !== modelQuery) field.text = modelQuery

    function run(text) {
        if (text.trim().length === 0) return
        if (typeof suggestionModel !== "undefined") suggestionModel.submit(text)
        searchModel.search(text)
        field.focus = false
    }

    function filterSummary() {
        var parts = []
        var sorts = ["",
            //: Search filter summary: sorted by rating
            qsTr("Rating"),
            //: Search filter summary: sorted by upload date
            qsTr("Upload date"),
            //: Search filter summary: sorted by view count
            qsTr("View count")]
        var dates = ["",
            //: Search filter summary: uploaded in the last hour
            qsTr("Last hour"),
            //: Search filter summary: uploaded today
            qsTr("Today"),
            //: Search filter summary: uploaded this week
            qsTr("This week"),
            //: Search filter summary: uploaded this month
            qsTr("This month"),
            //: Search filter summary: uploaded this year
            qsTr("This year")]
        var types = ["",
            //: Search filter summary: only videos
            qsTr("Videos"),
            //: Search filter summary: only channels
            qsTr("Channels"),
            //: Search filter summary: only playlists
            qsTr("Playlists")]
        var lengths = ["",
            //: Search filter summary: videos under 4 minutes
            qsTr("Under 4 minutes"),
            //: Search filter summary: videos over 20 minutes
            qsTr("Over 20 minutes"),
            //: Search filter summary: videos of 4 to 20 minutes
            qsTr("4-20 minutes")]
        if (searchModel.sort > 0) parts.push(sorts[searchModel.sort])
        if (searchModel.uploadDate > 0) parts.push(dates[searchModel.uploadDate])
        if (searchModel.type > 0) parts.push(types[searchModel.type])
        if (searchModel.duration > 0) parts.push(lengths[searchModel.duration])
        //: Search filter summary: only live streams
        if (searchModel.live) parts.push(qsTr("Live"))
        return parts.join(" · ")
    }

    function openRow(index) {
        var row = searchModel.get(index)
        if (row.kind === "channel") {
            pageStack.push(Qt.resolvedUrl("../pages/ChannelPage.qml"), { "reference": row.channelId })
        } else if (row.kind === "playlist") {
            pageStack.push(Qt.resolvedUrl("../pages/PlaylistPreviewPage.qml"), { "playlistId": row.playlistId })
        } else {
            window.openVideo(row.videoId, row)
        }
    }

    // The field stays outside the result list, so arriving results never take its focus and
    // the keyboard stays open (DSC-11).
    Column {
        id: top
        width: parent.width
        PageHeader {
            visible: page.standalone
            //: Title of the search page
            title: qsTr("Search")
        }
        SearchField {
            id: field
            width: parent.width
            //: Placeholder of the search field
            placeholderText: qsTr("Search YouTube")
            inputMethodHints: Qt.ImhNoPredictiveText
            EnterKey.iconSource: "image://theme/icon-m-enter-accept"
            EnterKey.enabled: text.trim().length > 0
            EnterKey.onClicked: page.run(text)
            onTextChanged: {
                if (typeof suggestionModel !== "undefined") suggestionModel.query = text
                // NFR-PERF-09: the search request skips the TLS handshake (no-op if warmed lately).
                if (text.length === 1 && typeof searchModel.prewarm === "function") searchModel.prewarm()
            }
            Component.onCompleted: {
                // The last search is on screen at once (from disk) and refreshed in place.
                if (searchModel.restored === true) {
                    text = searchModel.query
                    searchModel.refreshInPlace()
                } else if (searchModel.query !== "") {
                    // A search that started elsewhere before this view existed.
                    text = searchModel.query
                } else if (searchModel.count === 0) {
                    forceActiveFocus()
                }
            }
        }
        // Filter chips under the field (docs/DESIGN.md §4.6), as the official app shows them: the
        // filter page first (it names the active filters), then the result type and Live as quick
        // choices. Each quick chip applies at once (DSC-08).
        ChipStrip {
            id: filterChips
            objectName: "filterChips"
            ActionChip {
                iconSource: "image://theme/icon-m-levels"
                selected: searchModel.filtersActive
                text: searchModel.filtersActive
                      //: Search filter entry while filters are active, %1 lists them
                      ? qsTr("Filters: %1").arg(page.filterSummary())
                      //: Search filter entry when no filter is active
                      : qsTr("Filters")
                onClicked: pageStack.push(Qt.resolvedUrl("../pages/SearchFilterPage.qml"))
            }
            ActionChip {
                selected: searchModel.type === 0 && !searchModel.live
                //: Search filter chip: every kind of result
                text: qsTr("All")
                onClicked: { searchModel.live = false; searchModel.type = 0 }
            }
            ActionChip {
                selected: searchModel.type === 1
                //: Search filter summary: only videos
                text: qsTr("Videos")
                onClicked: searchModel.type = 1
            }
            ActionChip {
                selected: searchModel.type === 2
                //: Search filter summary: only channels
                text: qsTr("Channels")
                onClicked: searchModel.type = 2
            }
            ActionChip {
                selected: searchModel.type === 3
                //: Search filter summary: only playlists
                text: qsTr("Playlists")
                onClicked: searchModel.type = 3
            }
            ActionChip {
                selected: searchModel.live
                //: Search filter summary: only live streams
                text: qsTr("Live")
                onClicked: searchModel.live = !searchModel.live
            }
        }
    }

    SilicaListView {
        id: suggestions
        visible: page.suggesting
        anchors { top: top.bottom; left: parent.left; right: parent.right; bottom: parent.bottom }
        clip: true
        model: typeof suggestionModel !== "undefined" ? suggestionModel : null
        delegate: ListItem {
            id: suggestion
            contentHeight: Math.max(Theme.itemSizeSmall, suggestionLabel.implicitHeight + 2 * Theme.paddingMedium)
            menu: model.history ? historyMenu : null
            onClicked: {
                field.text = model.text
                page.run(model.text)
            }
            // A clock for an earlier search, a magnifier for a suggestion, as the official app.
            Icon {
                id: suggestionIcon
                anchors { left: parent.left; leftMargin: Theme.horizontalPageMargin; verticalCenter: parent.verticalCenter }
                source: model.history ? "image://theme/icon-m-history" : "image://theme/icon-m-search"
                width: Theme.iconSizeSmall
                height: width
                sourceSize.width: width
                sourceSize.height: height
                highlighted: suggestion.highlighted
                opacity: Theme.opacityHigh
            }
            Label {
                id: suggestionLabel
                anchors {
                    left: suggestionIcon.right; leftMargin: Theme.paddingMedium
                    right: parent.right; rightMargin: Theme.horizontalPageMargin
                    verticalCenter: parent.verticalCenter
                }
                text: model.text
                truncationMode: TruncationMode.Fade
                color: suggestion.highlighted ? Theme.highlightColor
                                              : model.history ? Theme.secondaryColor : Theme.primaryColor
            }
            Component {
                id: historyMenu
                ContextMenu {
                    MenuItem {
                        //: Removes one entry from the search history
                        text: qsTr("Remove from history")
                        onClicked: suggestionModel.removeHistory(model.text)
                    }
                }
            }
        }
        VerticalScrollDecorator { }
    }

    // A second tap on the section's bar entry: back to the top of the results.
    function scrollToTop() { list.scrollToTop() }

    SilicaListView {
        id: list
        VisibleRows { view: list }   // E12: rows on screen for tapIntent
        visible: !page.suggesting
        anchors { top: top.bottom; left: parent.left; right: parent.right; bottom: parent.bottom }
        clip: true
        model: searchModel

        PullDownMenu {
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
                //: Menu entry opening the local library
                text: qsTr("Library")
                onClicked: pageStack.push(Qt.resolvedUrl("../pages/LibraryPage.qml"))
            }
            MenuItem {
                //: Menu entry opening the search filters
                text: qsTr("Search filters")
                onClicked: pageStack.push(Qt.resolvedUrl("../pages/SearchFilterPage.qml"))
            }
            MenuItem {
                visible: playback.videoId !== ""
                //: Menu entry returning to the video that is playing
                text: qsTr("Now playing")
                onClicked: window.openVideo(playback.videoId)
            }
        }

        header: SkeletonList {
            cards: true
            shown: page.listState === "skeleton"
            viewHeight: list.height
        }

        delegate: VideoListItem {
            id: row
            // Video and playlist results as cards in portrait; channels keep their round row.
            cardLayout: true
            // The channel's picture for a video or playlist result (catalog's channelAvatar).
            avatarUrl: model.kind !== "channel" ? (model.channelAvatar || model.avatar || "") : ""
            largeThumbnailUrl: model.thumbnailLarge !== undefined ? (model.thumbnailLarge || "") : ""
            // LIB-07: optional hide-watched in search.
            readonly property bool hiddenWatched: watchIndex.hideWatchedInSearch && watchIndex.hidden(model.videoId, "search")
            visible: !hiddenWatched
            contentHeight: hiddenWatched ? 0 : naturalHeight
            videoId: model.videoId
            channelRow: model.kind === "channel"
            // A channel result carries its Subscribe button, as on the official app (LIB-01).
            channelId: model.kind === "channel" ? (model.channelId || "") : ""
            // The channel page's badge text and its translations (one Shorts label everywhere, DSC-17).
            //: Badge of a confirmed YouTube Short
            badgeText: model.kind === "short" ? qsTranslate("ChannelPage", "Short") : ""
            menu: Component { VideoMenuItems { video: searchModel.get(index) } }
            videoTitle: model.title
            channelName: model.kind === "channel"
                         ? model.subscriberText
                         : (model.membersOnly
                            //: Prefix of a members-only row, %1 is the channel name
                            ? qsTr("Members only · %1").arg(model.channel) : model.channel)
            thumbnailUrl: model.kind === "channel" && model.avatar !== "" ? model.avatar : model.thumbnail
            durationText: model.kind === "playlist" ? model.videoCountText
                          //: Badge on the thumbnail of a scheduled live stream or premiere
                          : model.upcoming ? qsTr("UPCOMING") : model.duration
            views: model.kind === "video" || model.kind === "short" ? model.viewCount : -1
            isLive: model.live
            onClicked: page.openRow(index)
        }

        footer: Column {
            width: list.width
            Item {
                width: parent.width
                height: searchModel.busy && searchModel.count > 0 ? Theme.itemSizeLarge : 0
                BusyIndicator {
                    anchors.centerIn: parent
                    running: searchModel.busy && searchModel.count > 0
                    size: BusyIndicatorSize.Medium
                }
            }
            // The next page failed: the error with a tap to retry (never on scroll).
            RetryRow {
                visible: searchModel.errorText !== "" && searchModel.count > 0
                errorText: searchModel.errorText
                onClicked: searchModel.loadMore()
            }
            // For a first page shorter than the screen, where reaching the end never changes.
            Button {
                anchors.horizontalCenter: parent.horizontalCenter
                visible: searchModel.canLoadMore && searchModel.count > 0 && !searchModel.busy && searchModel.errorText === ""
                //: Loads the next page of search results
                text: qsTr("More results")
                onClicked: searchModel.loadMore()
            }
            Label {
                // DSC-18: hidden rows are never dropped silently.
                visible: searchModel.hiddenCount > 0
                x: Theme.horizontalPageMargin
                width: parent.width - 2 * Theme.horizontalPageMargin
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.Wrap
                font.pixelSize: Theme.fontSizeExtraSmall
                color: Theme.secondaryColor
                //: Footer of the search results, %n is the number of hidden Shorts and members-only rows
                text: qsTr("%n result(s) hidden (Shorts, members-only)", "", searchModel.hiddenCount)
            }
            Item { width: parent.width; height: Theme.paddingLarge }
        }

        // Errors never retry on scroll: only a tap on the inline error does (docs/DESIGN.md §5.3).
        onAtYEndChanged: if (atYEnd && searchModel.canLoadMore && searchModel.errorText === "") searchModel.loadMore()

        ViewPlaceholder {
            id: emptyPlaceholder
            enabled: page.listState === "placeholder"
            text: searchModel.errorText !== "" ? searchModel.errorText
                  : searchModel.query !== ""
                    ? (searchModel.hiddenCount > 0
                       //: Shown when every search result was hidden by the Shorts / members-only settings
                       ? qsTr("All results are hidden")
                       //: Shown when a search found nothing
                       : qsTr("No results found"))
                    //: Shown on the empty search page
                    : qsTr("Find videos")
            hintText: searchModel.errorText !== "" ? qsTr("Search again to retry.")
                      : searchModel.query === "" ? qsTr("Type a search above and press Enter.")
                      : searchModel.filtersActive
                        //: Hint when a filtered search found nothing
                        ? qsTr("Try fewer filters.") : ""
        }

        PlaceholderActions {
            placeholder: emptyPlaceholder
            listState: page.listState
            errorText: searchModel.errorText
            canRetry: searchModel.query !== ""
            onRetry: searchModel.search(searchModel.query)
        }

        VerticalScrollDecorator { }
    }
}
