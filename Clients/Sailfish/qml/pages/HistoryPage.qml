// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
import QtQuick 2.6
import Sailfish.Silica 1.0
import "../components"
import "../components/Format.js" as Format

// LIB-04: newest first, 40 per page; tap resumes; clear with remorse (resume points stay).
Page {
    id: page

    RemorsePopup { id: remorse }

    SilicaListView {
        id: list
        anchors.fill: parent
        model: historyModel

        PullDownMenu {
            // The destructive entry sits on top, away from the finger.
            MenuItem {
                visible: historyModel.total > 0
                //: Menu entry deleting the whole watch history
                text: qsTr("Clear history")
                //: Remorse text while the history is being cleared
                onClicked: remorse.execute(qsTr("Clearing history"), function() { historyModel.clear() })
            }
            MenuItem {
                text: historyModel.paused
                      //: Menu entry turning history recording back on
                      ? qsTr("Resume history")
                      //: Menu entry stopping history recording
                      : qsTr("Pause history")
                onClicked: historyModel.paused = !historyModel.paused
            }
        }

        header: PageHeader {
            //: Title of the history page
            title: qsTr("History")
            //: Shown under the history title while recording is paused
            description: historyModel.paused ? qsTr("Paused") : ""
        }

        delegate: VideoListItem {
            id: row
            videoId: model.videoId
            videoTitle: model.title
            channelName: model.channel
            thumbnailUrl: model.thumbnail
            durationText: model.durationSeconds > 0 ? Format.time(model.durationSeconds * 1000) : ""
            // The shared row menu (watch later, playlists, watched mark, subscribe, downloads), then
            // this page's destructive entry last (docs/DESIGN.md §4.5).
            menu: Component {
                VideoMenuItems {
                    video: historyModel.get(index)
                    MenuItem {
                        //: Context menu entry removing one video from the history
                        text: qsTr("Remove from history")
                        onClicked: {
                            // The menu is gone when the remorse ends: capture what the callback needs.
                            var history = historyModel, id = model.videoId
                            //: Remorse text while a history entry is being removed
                            row.remorseAction(qsTr("Removing"), function() { history.remove(id) })
                        }
                    }
                }
            }
            onClicked: {
                window.openVideo(model.videoId, historyModel.get(index))
            }
        }

        ViewPlaceholder {
            enabled: historyModel.count === 0
            //: Empty history page
            text: qsTr("No history")
            hintText: historyModel.paused ? qsTr("History is paused.") : ""
        }
        VerticalScrollDecorator { }
    }
}
