// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Full-page comments (DSC-16). Loads at once (the auto-load setting is for the embedded
// section). Shares the commentsModel with the video page, so an already loaded list is kept.
//
//   pageStack.push(Qt.resolvedUrl("CommentsPage.qml"), { videoId: playback.videoId, token: "" })
import QtQuick 2.0
import Sailfish.Silica 1.0
import "../components"

Page {
    id: page
    property string videoId: commentsModel.videoId
    property string token
    allowedOrientations: Orientation.All

    SilicaFlickable {
        id: flick
        anchors.fill: parent
        contentHeight: column.height

        PullDownMenu {
            MenuItem {
                visible: commentsModel.errorText !== ""
                //: Menu entry retrying the comments after an error
                text: qsTr("Retry")
                onClicked: commentsModel.retry()
            }
            MenuItem {
                visible: commentsModel.canSort && commentsModel.started && commentsModel.sort !== "top"
                //: Menu entry sorting comments by YouTube's top comments
                text: qsTr("Sort by top")
                onClicked: commentsModel.sort = "top"
            }
            MenuItem {
                visible: commentsModel.canSort && commentsModel.started && commentsModel.sort !== "newest"
                //: Menu entry sorting comments newest first
                text: qsTr("Sort by newest")
                onClicked: commentsModel.sort = "newest"
            }
        }

        Column {
            id: column
            width: flick.width

            PageHeader {
                //: Title of the comments page
                title: qsTr("Comments")
                description: commentsModel.totalText !== ""
                             //: Subtitle of the comments page: the sort and the total
                             ? qsTr("%1 · %2").arg(commentsModel.sort === "newest" ? qsTr("Newest") : qsTr("Top")).arg(commentsModel.totalText)
                             : (commentsModel.sort === "newest" ? qsTr("Newest") : qsTr("Top"))
            }

            CommentsSection {
                width: parent.width
                flickable: flick
                videoId: page.videoId
                token: page.token
                autoStart: true
                showHeader: false
                showSortSwitch: false
                followPlayback: false
            }
        }

        VerticalScrollDecorator { }
    }
}
