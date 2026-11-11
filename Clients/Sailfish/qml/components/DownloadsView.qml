// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
import QtQuick 2.6
import Sailfish.Silica 1.0
import "../logic/Rows.js" as Rows
import "Format.js" as Format

// DL-04 downloads: live progress, pause/resume/cancel, tap to play offline (DL-07). Shared by
// pages/DownloadsPage.qml and the Downloads section of the bottom navigation bar.
Item {
    id: page
    property bool standalone: false

    // A second tap on the section's bar entry (docs/DESIGN.md §2.2).
    function scrollToTop() { list.scrollToTop() }

    function playLocal(videoId, row) {
        var media = downloads.localMedia(videoId)
        if (!media.available) return
        // The playback component's local-file mode (PLY-26) takes the record when it exists.
        if (typeof playback.openLocal === "function") {
            playback.openLocal(media)
            // One watch page, reused (docs/DESIGN.md §2.6); scenes without the shell push it.
            if (typeof window !== "undefined" && typeof window.showWatchPage === "function") window.showWatchPage()
            else pageStack.push(Qt.resolvedUrl("../pages/VideoPage.qml"))
        } else if (media.layout !== "pair") {
            // Until PLY-26 lands: hand single files to the system player.
            Qt.openUrlExternally("file://" + media.filePath)
        }
    }

    SilicaListView {
        id: list
        anchors.fill: parent
        model: downloads

        header: Column {
            width: list.width
            PageHeader {
                //: Title of the downloads page
                title: qsTr("Downloads")
            }
            Label {
                x: Theme.horizontalPageMargin
                width: parent.width - 2 * x
                visible: !downloads.backgroundCapable
                wrapMode: Text.Wrap
                font.pixelSize: Theme.fontSizeExtraSmall
                color: Theme.secondaryColor
                //: Note on the downloads page of the store build (DL-09)
                text: qsTr("Downloads run only while SailTube is open.")
            }
        }

        delegate: ListItem {
            id: item
            // Grows with the system text size (docs/DESIGN.md §7.9).
            // The shared row geometry (docs/DESIGN.md §4.1): 42 % thumbnail, text column beside it.
            contentHeight: Math.max(thumb.height, rowTexts.height) + 2 * Theme.paddingMedium
            readonly property bool running: model.state <= 2 || model.state === 5
            menu: ContextMenu {
                MenuItem {
                    visible: item.running
                    //: Context menu entry pausing a download
                    text: qsTr("Pause")
                    onClicked: downloads.pause(model.downloadId)
                }
                MenuItem {
                    visible: model.state === 3 || model.state === 4
                    //: Context menu entry resuming a paused download
                    text: qsTr("Resume")
                    onClicked: downloads.resume(model.downloadId)
                }
                MenuItem {
                    visible: model.state === 7
                    //: Context menu entry retrying a failed download
                    text: qsTr("Retry")
                    onClicked: downloads.retry(model.downloadId)
                }
                MenuItem {
                    text: model.state === 6
                          //: Context menu entry deleting a downloaded file
                          ? qsTr("Delete")
                          //: Context menu entry cancelling a download
                          : qsTr("Cancel")
                    onClicked: {
                        var id = model.downloadId, done = model.state === 6
                        //: Remorse text while a download is removed
                        item.remorseAction(qsTr("Removing"), function() {
                            if (done) downloads.remove(id, true); else downloads.cancel(id)
                        })
                    }
                }
            }
            Image {
                id: thumb
                // Anchored, not x:, so right-to-left layouts mirror it (I18N-03).
                anchors.left: parent.left
                anchors.leftMargin: Theme.horizontalPageMargin
                y: Theme.paddingMedium
                width: Rows.thumbWidth(item.width, Screen.width)
                height: Rows.thumbHeight(width)
                source: model.thumbnail
                fillMode: Image.PreserveAspectCrop
                clip: true
                asynchronous: true
                sourceSize.width: width
            }
            Column {
                id: rowTexts
                anchors {
                    left: thumb.right; leftMargin: Theme.paddingMedium
                    right: parent.right; rightMargin: Theme.horizontalPageMargin
                    top: thumb.top
                }
                Label {
                    width: parent.width
                    text: model.title
                    wrapMode: Text.Wrap
                    maximumLineCount: 2
                    elide: Text.ElideRight
                    font.pixelSize: Theme.fontSizeSmall
                    color: item.highlighted ? Theme.highlightColor : Theme.primaryColor
                }
                Label {
                    width: parent.width
                    truncationMode: TruncationMode.Fade
                    font.pixelSize: Theme.fontSizeExtraSmall
                    color: model.state === 7 ? Theme.errorColor
                         : item.highlighted ? Theme.secondaryHighlightColor : Theme.secondaryColor
                    text: {
                        //: Kind of a download: the audio track only
                        var kind = model.kind === 0 ? qsTr("Audio")
                                 : (model.qualityHeight > 0 ? model.qualityHeight + "p" : qsTr("Video"))
                        var s = downloads.stateText(model.state, model.error)
                        if (item.running && model.bytesTotal > 0) s += " " + Math.round(model.fraction * 100) + " %"
                        return kind + " · " + s
                    }
                }
                ProgressBar {
                    width: parent.width
                    visible: item.running || model.state === 3 || model.state === 4
                    leftMargin: 0
                    rightMargin: 0
                    height: Theme.paddingLarge
                    minimumValue: 0
                    maximumValue: 1
                    value: model.fraction
                }
            }
            onClicked: if (model.state === 6) page.playLocal(model.videoId, model)
        }

        ViewPlaceholder {
            enabled: downloads.count === 0
            //: Empty downloads page
            text: qsTr("No downloads")
            //: Hint on the empty downloads page
            hintText: qsTr("Download from a video's pull-down menu or by long pressing a video.")
        }
        VerticalScrollDecorator { }
    }
}
