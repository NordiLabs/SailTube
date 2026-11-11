// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
import QtQuick 2.0
import Sailfish.Silica 1.0

// Caption choice (SEL-06): Off, the real tracks, then the searchable translation list.
// Every tap here is an explicit pick and persists; the auto-pick never shows up as a choice.
Page {
    id: page
    allowedOrientations: Orientation.All

    // A CaptionTracksModel; defaults to the "captionTracks" context property.
    property QtObject tracks: typeof captionTracks !== "undefined" ? captionTracks : null
    // A streamed video's list is asked for with its watch data: on its way, or it failed.
    readonly property bool listLoading: !!tracks && tracks.loading === true
    readonly property bool listFailed: !!tracks && tracks.failed === true
    // The chosen track's text could not be loaded (the provider refused it).
    readonly property bool cuesFailed: !!tracks && !tracks.off && typeof captionCues !== "undefined" && !!captionCues
                                       && captionCues.failed === true && captionCues.url === tracks.currentUrl

    function choose(key) {
        if (key === "") tracks.pickOff()
        else tracks.pick(key)
        pageStack.pop()
    }

    SilicaListView {
        id: list
        anchors.fill: parent
        model: page.tracks ? page.tracks.translations : null

        PullDownMenu {
            visible: page.listFailed
            MenuItem {
                //: Menu entry asking again for the caption list after it failed to load
                text: qsTr("Retry")
                onClicked: page.tracks.reload()
            }
        }

        header: Column {
            width: list.width

            PageHeader {
                //: Title of the page choosing the caption track
                title: qsTr("Captions")
                //: Shown under the title when the chosen caption track could not be loaded
                description: page.cuesFailed ? qsTr("This track could not be loaded")
                                             : (page.tracks && !page.tracks.off ? page.tracks.currentName : "")
            }

            ListItem {
                contentHeight: Math.max(Theme.itemSizeSmall, captionLabel1.implicitHeight + 2 * Theme.paddingMedium)
                onClicked: page.choose("")
                Label {
                    id: captionLabel1
                    x: Theme.horizontalPageMargin
                    width: parent.width - 2 * x
                    anchors.verticalCenter: parent.verticalCenter
                    truncationMode: TruncationMode.Fade
                    //: Caption choice that turns captions off
                    text: qsTr("Off")
                    color: !page.tracks || page.tracks.off || highlighted ? Theme.highlightColor : Theme.primaryColor
                }
            }

            Repeater {
                model: page.tracks
                delegate: ListItem {
                    contentHeight: Math.max(Theme.itemSizeSmall, captionLabel2.implicitHeight + 2 * Theme.paddingMedium)
                    onClicked: page.choose(model.key)
                    Label {
                        id: captionLabel2
                        x: Theme.horizontalPageMargin
                        width: parent.width - 2 * x
                        anchors.verticalCenter: parent.verticalCenter
                        truncationMode: TruncationMode.Fade
                        text: model.name
                        color: model.current || highlighted ? Theme.highlightColor : Theme.primaryColor
                    }
                }
            }

            SectionHeader {
                visible: page.tracks && page.tracks.translations.total > 0
                //: Section of machine-translated caption languages
                text: qsTr("Translations")
            }

            SearchField {
                id: search
                width: parent.width
                visible: page.tracks && page.tracks.translations.total > 0
                //: Placeholder of the field filtering caption translations by name or code
                placeholderText: qsTr("Search languages")
                inputMethodHints: Qt.ImhNoPredictiveText
                onTextChanged: if (page.tracks) page.tracks.translations.filter = text
                EnterKey.iconSource: "image://theme/icon-m-enter-close"
                EnterKey.onClicked: focus = false
            }
        }

        delegate: ListItem {
            contentHeight: Math.max(Theme.itemSizeSmall, captionLabel3.implicitHeight + 2 * Theme.paddingMedium)
            onClicked: page.choose(model.key)
            Label {
                id: captionLabel3
                x: Theme.horizontalPageMargin
                width: parent.width - 2 * x
                anchors.verticalCenter: parent.verticalCenter
                truncationMode: TruncationMode.Fade
                text: model.name
                color: model.current || highlighted ? Theme.highlightColor : Theme.primaryColor
            }
        }

        ViewPlaceholder {
            enabled: page.tracks && !page.tracks.available
            text: page.listLoading
                  //: Shown while the caption list of a video is being loaded
                  ? qsTr("Loading captions")
                  : page.listFailed
                    //: Shown when the caption list of a video could not be loaded
                    ? qsTr("Captions could not be loaded")
                    //: Shown when a video has no captions
                    : qsTr("No captions for this video")
            //: Hint under the message that the caption list could not be loaded
            hintText: page.listFailed ? qsTr("Pull down to try again.") : ""
        }

        VerticalScrollDecorator { }
    }

    Component.onDestruction: if (page.tracks) page.tracks.translations.filter = ""
}
