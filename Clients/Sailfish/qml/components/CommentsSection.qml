// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Comments for one video (DSC-16), embeddable in a page's Column under a SilicaFlickable:
//
//   CommentsSection { width: parent.width; flickable: flick; videoId: playback.videoId; token: "" }
//
// The section never scrolls by itself: it watches `flickable` to call scrolledNear() (auto-load
// "When I scroll near") and loadMore() (reveal the next rows). Errors are shown inline and are
// repeated only by a tap. Logic lives in the C++ CommentsModel (context property commentsModel).
import QtQuick 2.0
import Sailfish.Silica 1.0

Column {
    id: section

    property QtObject comments: commentsModel
    property Flickable flickable
    property string videoId
    property string token
    // Load at once instead of following the auto-load setting (the full comments page).
    property bool autoStart: false
    property bool showHeader: true
    property bool showSortSwitch: true
    // Drive the "Shortly after playback starts" setting from the shared playback session.
    property bool followPlayback: true

    function arm() {
        if (!comments || videoId === "") return
        comments.setVideo(videoId, token)
        if (autoStart) comments.start()
        else if (followPlayback && typeof playback !== "undefined" && playback.playing && playback.videoId === videoId)
            comments.playbackStarted()
        viewportTimer.restart()
    }

    function checkViewport() {
        if (!comments || !flickable || !visible || videoId === "") return
        var top = section.mapToItem(flickable.contentItem, 0, 0).y
        var viewBottom = flickable.contentY + flickable.height
        // Within one screen of the viewport: the auto-load hook (the model applies the setting).
        if (top - viewBottom < flickable.height) comments.scrolledNear()
        // The end of the revealed rows is near: reveal more (never retries after an error).
        if (top + section.height - viewBottom < flickable.height && comments.canLoadMore && !comments.busy)
            comments.loadMore()
    }

    onVideoIdChanged: arm()
    onTokenChanged: if (comments && videoId !== "" && !comments.started) comments.setVideo(videoId, token)
    onHeightChanged: viewportTimer.restart()
    Component.onCompleted: arm()

    Timer {
        id: viewportTimer
        interval: 100
        onTriggered: section.checkViewport()
    }

    Connections {
        target: section.flickable
        onContentYChanged: viewportTimer.restart()
        onHeightChanged: viewportTimer.restart()
    }

    Connections {
        target: section.followPlayback && typeof playback !== "undefined" ? playback : null
        onPlayerChanged: {
            if (playback.playing && playback.videoId === section.videoId) section.comments.playbackStarted()
        }
    }

    // Header with the count. Tapping it loads (auto-load off or not yet triggered) or retries.
    BackgroundItem {
        id: header
        visible: section.showHeader
        width: parent.width
        // Grows with the system text size (docs/DESIGN.md §7.9).
        height: Math.max(Theme.itemSizeSmall, headerLabel.implicitHeight + 2 * Theme.paddingMedium)
        enabled: comments && (!comments.started || comments.errorText !== "")
        onClicked: {
            if (!comments.started) comments.start()
            else comments.retry()
        }
        // Styled like every other watch-page block's SectionHeader (end-aligned, small, highlight),
        // but tappable: it loads or retries (docs/DESIGN.md §3.4).
        Label {
            id: headerLabel
            anchors {
                left: headerBusy.right; leftMargin: Theme.paddingMedium
                right: parent.right; rightMargin: Theme.horizontalPageMargin
                verticalCenter: parent.verticalCenter
            }
            horizontalAlignment: Text.AlignRight
            color: Theme.highlightColor
            font.pixelSize: Theme.fontSizeSmall
            truncationMode: TruncationMode.Fade
            text: comments && comments.totalText !== ""
                  //: Comments section header with the total count, e.g. "Comments 7.7K"
                  ? qsTr("Comments %1").arg(comments.totalText)
                  //: Comments section header without a known count
                  : qsTr("Comments")
        }
        BusyIndicator {
            id: headerBusy
            anchors { left: parent.left; leftMargin: Theme.horizontalPageMargin; verticalCenter: parent.verticalCenter }
            size: BusyIndicatorSize.ExtraSmall
            running: comments && comments.busy && comments.count === 0
        }
    }

    // Not loaded yet (auto-load off, or waiting for the trigger): an explicit load.
    Button {
        visible: comments && !comments.started && section.videoId !== ""
        anchors.horizontalCenter: parent.horizontalCenter
        //: Button loading the comments of the video
        text: qsTr("Show comments")
        onClicked: comments.start()
    }

    ComboBox {
        visible: section.showSortSwitch && comments && comments.started && comments.canSort && comments.emptyText === ""
        width: parent.width
        //: Label of the comment sort choice
        label: qsTr("Sort")
        currentIndex: comments && comments.sort === "newest" ? 1 : 0
        menu: ContextMenu {
            //: Comment sort: YouTube's top comments
            MenuItem { text: qsTr("Top") }
            //: Comment sort: newest first
            MenuItem { text: qsTr("Newest") }
        }
        onCurrentIndexChanged: if (comments) comments.sort = currentIndex === 1 ? "newest" : "top"
    }

    // Inline list error: tap to retry. Scrolling never retries.
    BackgroundItem {
        visible: comments && comments.errorText !== ""
        width: parent.width
        height: errorLabel.height + 2 * Theme.paddingMedium
        onClicked: comments.retry()
        Label {
            id: errorLabel
            x: Theme.horizontalPageMargin
            width: parent.width - 2 * Theme.horizontalPageMargin
            anchors.verticalCenter: parent.verticalCenter
            wrapMode: Text.Wrap
            color: Theme.secondaryHighlightColor
            //: Inline comments error followed by the retry hint
            text: comments ? qsTr("%1 Tap to retry.").arg(comments.errorText) : ""
        }
    }

    Label {
        visible: comments && comments.emptyText !== "" && comments.errorText === ""
        x: Theme.horizontalPageMargin
        width: parent.width - 2 * Theme.horizontalPageMargin
        wrapMode: Text.Wrap
        color: Theme.secondaryColor
        text: comments ? comments.emptyText : ""
    }

    Repeater {
        model: section.comments

        ListItem {
            id: row
            readonly property bool moreRow: model.rowType === "more"
            readonly property bool threadRow: model.rowType === "thread"
            readonly property real indent: model.isReply ? Theme.paddingLarge * 2 : 0
            width: section.width
            contentHeight: moreRow ? Theme.itemSizeSmall
                                   : Math.max(avatar.height, body.height) + 2 * Theme.paddingMedium
            enabled: moreRow ? !model.repliesBusy
                             : threadRow && (model.repliesExpanded || model.canLoadMoreReplies)
            onClicked: {
                if (moreRow) section.comments.loadMoreReplies(index)
                else section.comments.toggleReplies(index)
            }

            // --- "more replies" / loading / replies error row
            Label {
                visible: row.moreRow
                anchors {
                    left: parent.left; leftMargin: Theme.horizontalPageMargin + Theme.paddingLarge * 2 + Theme.iconSizeSmall
                    right: moreBusy.left; rightMargin: Theme.paddingMedium
                    verticalCenter: parent.verticalCenter
                }
                truncationMode: TruncationMode.Fade
                font.pixelSize: Theme.fontSizeSmall
                color: model.replyErrorText !== "" ? Theme.secondaryHighlightColor : Theme.highlightColor
                text: model.repliesBusy
                      //: Row under a comment's replies while they load
                      ? qsTr("Loading replies…")
                      : model.replyErrorText !== ""
                        ? qsTr("%1 Tap to retry.").arg(model.replyErrorText)
                        //: Row loading further replies of a comment
                        : qsTr("More replies")
            }
            BusyIndicator {
                id: moreBusy
                visible: row.moreRow
                anchors { right: parent.right; rightMargin: Theme.horizontalPageMargin; verticalCenter: parent.verticalCenter }
                size: BusyIndicatorSize.ExtraSmall
                running: row.moreRow && model.repliesBusy
            }

            // --- comment row (thread or reply)
            Avatar {
                id: avatar
                visible: !row.moreRow
                // Anchored, not x:, so right-to-left layouts mirror it (I18N-03).
                anchors.left: parent.left
                anchors.leftMargin: Theme.horizontalPageMargin + row.indent
                y: Theme.paddingMedium
                width: model.isReply ? Theme.iconSizeSmall : Theme.iconSizeMedium
                height: width
                source: row.moreRow ? "" : model.avatar
            }
            Column {
                id: body
                visible: !row.moreRow
                anchors {
                    left: avatar.right; leftMargin: Theme.paddingMedium
                    right: parent.right; rightMargin: Theme.horizontalPageMargin
                    top: avatar.top
                }
                spacing: Theme.paddingSmall / 2
                Label {
                    width: parent.width
                    truncationMode: TruncationMode.Fade
                    font.pixelSize: Theme.fontSizeExtraSmall
                    color: model.isCreator ? Theme.highlightColor : Theme.secondaryColor
                    text: row.moreRow ? "" : [
                        model.author,
                        model.published,
                        //: Marks the comment the uploader pinned
                        model.pinned ? qsTr("Pinned") : ""
                    ].filter(function (s) { return s !== "" }).join(" · ")
                }
                Label {
                    width: parent.width
                    wrapMode: Text.Wrap
                    textFormat: Text.PlainText
                    font.pixelSize: Theme.fontSizeSmall
                    color: row.highlighted ? Theme.highlightColor : Theme.primaryColor
                    text: row.moreRow ? "" : model.text
                }
                Label {
                    width: parent.width
                    truncationMode: TruncationMode.Fade
                    font.pixelSize: Theme.fontSizeExtraSmall
                    color: row.threadRow && model.replyCount > 0 ? Theme.highlightColor : Theme.secondaryColor
                    visible: text !== ""
                    text: row.moreRow ? "" : [
                        //: Like count of a comment, e.g. "1.3K likes"
                        model.likes !== "" ? qsTr("%1 likes").arg(model.likes) : "",
                        row.threadRow && model.replyCount > 0
                            ? (model.repliesExpanded
                               //: Toggle hiding a comment's replies
                               ? qsTr("Hide replies")
                               //: Toggle showing a comment's replies, e.g. "66 replies"
                               : qsTr("%n replies", "", model.replyCount))
                            : ""
                    ].filter(function (s) { return s !== "" }).join(" · ")
                }
            }
        }
    }

    // Footer: shown of total, a busy spinner while the next request runs.
    Item {
        visible: comments && comments.started && (comments.count > 0 || comments.busy)
        width: parent.width
        height: Math.max(Theme.itemSizeSmall, footerLabel.implicitHeight + 2 * Theme.paddingMedium)
        Label {
            id: footerLabel
            anchors.centerIn: parent
            visible: !footerBusy.running
            font.pixelSize: Theme.fontSizeExtraSmall
            color: Theme.secondaryColor
            text: comments ? comments.footerText : ""
        }
        BusyIndicator {
            id: footerBusy
            anchors.centerIn: parent
            size: BusyIndicatorSize.Small
            running: comments && comments.busy && comments.count > 0
        }
    }
}
