// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
import QtQuick 2.6
import Sailfish.Silica 1.0
import org.nordilabs.sailtube 1.0
import "../components"
import "../components/Format.js" as Format

// Details-only view (DSC-14), opened by a long-press on a row. It has its own WatchInfo (metadata
// from watch-next and the WEB player answer, which works for unplayable videos) and never
// touches the current playback until "Play" replaces it with the watch page.
Page {
    id: page
    property string videoId
    property var hints: ({})
    allowedOrientations: Orientation.All   // every page works in landscape (docs/DESIGN.md §7.10)

    WatchInfo {
        id: info
        videoId: page.videoId
        onOpenUrlRequested: Qt.openUrlExternally(url)
    }
    WatchActions { id: actions }

    function play() {
        playback.open(page.videoId, page.hints)
        pageStack.replace(Qt.resolvedUrl("VideoPage.qml"))
    }

    SilicaFlickable {
        id: flick
        anchors.fill: parent
        contentHeight: column.height

        PullDownMenu {
            MenuItem {
                //: Menu entry opening the in-browser YouTube page of the video
                text: qsTr("Open in browser")
                onClicked: info.openInBrowser()
            }
            MenuItem {
                //: Button copying the video link to the clipboard
                text: qsTr("Copy link")
                onClicked: actions.copy(info.shareUrl())
            }
            MenuItem {
                //: Button sharing the video link through the system share sheet
                text: qsTr("Share")
                onClicked: actions.share(info.shareUrl(), info.title)
            }
            MenuItem {
                //: Menu entry starting playback of the video shown in the details view
                text: qsTr("Play")
                onClicked: page.play()
            }
        }

        Column {
            id: column
            width: flick.width
            spacing: Theme.paddingSmall

            PageHeader {
                title: info.title !== "" ? info.title : (page.hints.title || "")
            }

            Rectangle {
                width: parent.width
                height: Math.round(width * 9 / 16)
                color: "black" // theme-exempt: letterbox behind a thumbnail
                Image {
                    anchors.fill: parent
                    source: info.thumbnail
                    fillMode: Image.PreserveAspectFit
                    asynchronous: true
                    sourceSize.width: width
                }
                IconButton {
                    anchors.centerIn: parent
                    icon.source: "image://theme/icon-l-play"
                    //: Accessible name of the play button
                    Accessible.name: qsTr("Play")
                    onClicked: page.play()
                }
                BusyIndicator {
                    anchors { right: parent.right; bottom: parent.bottom; margins: Theme.paddingMedium }
                    size: BusyIndicatorSize.Small
                    running: info.loading
                }
            }

            Label {
                x: Theme.horizontalPageMargin
                width: parent.width - 2 * x
                text: {
                    var name = info.owner !== "" ? info.owner : (page.hints.channel || "")
                    if (info.subscriberCount >= 0)
                        //: Subscriber count of a channel, %1 is a compact number such as 1.2M
                        return name + " · " + qsTr("%1 subscribers").arg(Format.count(info.subscriberCount, Qt.locale()))
                    return info.subscriberText !== "" ? name + " · " + info.subscriberText : name
                }
                truncationMode: TruncationMode.Fade
                color: Theme.secondaryHighlightColor
            }
            Label {
                x: Theme.horizontalPageMargin
                width: parent.width - 2 * x
                wrapMode: Text.Wrap
                font.pixelSize: Theme.fontSizeExtraSmall
                color: Theme.secondaryColor
                text: {
                    var parts = []
                    //: Shown for a live stream instead of a view count
                    if (info.live) parts.push(qsTr("Live now"))
                    //: View count of a video, %1 is a compact number such as 1.2M
                    if (info.viewCount >= 0) parts.push(qsTr("%1 views").arg(Format.count(info.viewCount, Qt.locale())))
                    else if (info.viewText !== "") parts.push(info.viewText)
                    //: Like count of a video, %1 is a compact number
                    if (info.likeCount >= 0) parts.push(qsTr("%1 likes").arg(Format.count(info.likeCount, Qt.locale())))
                    if (info.dateText !== "") parts.push(info.dateText)
                    if (info.durationSeconds > 0) parts.push(Format.time(info.durationSeconds * 1000))
                    return parts.join(" · ")
                }
            }
            Label {
                x: Theme.horizontalPageMargin
                width: parent.width - 2 * x
                visible: text !== ""
                text: actions.message !== "" ? actions.message : info.errorText
                wrapMode: Text.Wrap
                font.pixelSize: Theme.fontSizeExtraSmall
                color: Theme.highlightColor
            }

            SectionHeader {
                visible: info.descriptionStyled !== ""
                //: Section header above the video description
                text: qsTr("Description")
            }
            Label {
                id: description
                x: Theme.horizontalPageMargin
                width: parent.width - 2 * x
                visible: info.descriptionStyled !== ""
                text: info.shownDescription
                textFormat: Text.StyledText
                wrapMode: Text.Wrap
                maximumLineCount: info.collapsed ? info.collapsedLines : 100000
                elide: Text.ElideRight
                font.pixelSize: Theme.fontSizeSmall
                linkColor: Theme.highlightColor
                // No player here: timestamps and hashtags stay inert, links open the browser.
                onLinkActivated: info.activateLink(link)
            }
            BackgroundItem {
                width: parent.width
                // Touch target that grows with the system text size (docs/DESIGN.md §7.7, §7.9).
                height: Math.max(Theme.itemSizeSmall, showMoreLabel.implicitHeight + 2 * Theme.paddingMedium)
                visible: description.visible && (!info.collapsed || info.descriptionTruncated || description.truncated)
                onClicked: info.toggleCollapsed()
                Label {
                    id: showMoreLabel
                    // Anchored, not x:, so right-to-left layouts mirror it (I18N-03).
                    anchors { left: parent.left; leftMargin: Theme.horizontalPageMargin; verticalCenter: parent.verticalCenter }
                    text: info.collapsed
                      //: Expands the collapsed video description
                      ? qsTr("Show more")
                      //: Collapses the video description to 5 lines
                      : qsTr("Show less")
                    font.pixelSize: Theme.fontSizeSmall
                    color: Theme.highlightColor
                }
            }

            BackgroundItem {
                width: parent.width
                visible: info.chapters.count > 0
                onClicked: pageStack.push(Qt.resolvedUrl("ChaptersPage.qml"), { chapterModel: info.chapters, seekable: false })
                Label {
                    // Anchored, not x:, so right-to-left layouts mirror it (I18N-03).
                    anchors { left: parent.left; leftMargin: Theme.horizontalPageMargin; verticalCenter: parent.verticalCenter }
                    //: Number of chapters of a video
                    text: qsTr("%n chapter(s)", "", info.chapters.count)
                    font.pixelSize: Theme.fontSizeSmall
                    color: Theme.highlightColor
                }
            }
            BackgroundItem {
                width: parent.width
                visible: info.related.count > 0
                onClicked: pageStack.push(Qt.resolvedUrl("RelatedPage.qml"), { info: info })
                Label {
                    // Anchored, not x:, so right-to-left layouts mirror it (I18N-03).
                    anchors { left: parent.left; leftMargin: Theme.horizontalPageMargin; verticalCenter: parent.verticalCenter }
                    //: Section header above the related videos
                    text: qsTr("Related videos")
                    font.pixelSize: Theme.fontSizeSmall
                    color: Theme.highlightColor
                }
            }

            // ---- Comments (DSC-16) go here; built by the comments package. ----
            Item { id: commentsAnchor; width: parent.width; height: 0 }

            Item { width: 1; height: Theme.paddingLarge }
        }
        VerticalScrollDecorator { }
    }
}
