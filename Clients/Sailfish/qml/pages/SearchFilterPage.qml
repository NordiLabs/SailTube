// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
import QtQuick 2.6
import Sailfish.Silica 1.0

// Search filters (DSC-08): every `sp` filter, including "last hour", "playlist" and "live",
// plus the hide settings (DSC-18) and the search history switches (DSC-11). Accepting applies
// all filters at once, so the query runs once.
Dialog {
    id: dialog

    // Menu order -> model values (catalog::SearchSort / DurationFilter).
    readonly property var sortValues: [0, 2, 3, 1]
    readonly property var durationValues: [0, 1, 3, 2]

    onAccepted: {
        searchModel.setFilters(sortValues[sortBox.currentIndex], dateBox.currentIndex, typeBox.currentIndex,
                               durationValues[durationBox.currentIndex], liveSwitch.checked, mixedSwitch.checked)
        searchModel.hideShorts = shortsSwitch.checked
        searchModel.hideMembersOnly = membersSwitch.checked
    }

    SilicaFlickable {
        anchors.fill: parent
        contentHeight: column.height + Theme.paddingLarge

        PullDownMenu {
            MenuItem {
                //: Resets every search filter to its default
                text: qsTr("Reset filters")
                onClicked: {
                    sortBox.currentIndex = 0
                    dateBox.currentIndex = 0
                    typeBox.currentIndex = 0
                    durationBox.currentIndex = 0
                    liveSwitch.checked = false
                }
            }
        }

        Column {
            id: column
            width: parent.width

            DialogHeader {
                //: Accept button of the search filter dialog
                acceptText: qsTr("Apply")
            }

            ComboBox {
                id: sortBox
                //: Search filter: result order
                label: qsTr("Sort by")
                currentIndex: Math.max(0, dialog.sortValues.indexOf(searchModel.sort))
                menu: ContextMenu {
                    //: Search sort order: most relevant first
                    MenuItem { text: qsTr("Relevance") }
                    //: Search sort order: newest uploads first
                    MenuItem { text: qsTr("Upload date") }
                    //: Search sort order: most viewed first
                    MenuItem { text: qsTr("View count") }
                    //: Search sort order: best rated first
                    MenuItem { text: qsTr("Rating") }
                }
            }
            ComboBox {
                id: dateBox
                //: Search filter: when the video was uploaded
                label: qsTr("Uploaded")
                currentIndex: searchModel.uploadDate
                menu: ContextMenu {
                    //: Upload date filter: no limit
                    MenuItem { text: qsTr("Any time") }
                    //: Upload date filter: the last hour
                    MenuItem { text: qsTr("Last hour") }
                    //: Upload date filter: today
                    MenuItem { text: qsTr("Today") }
                    //: Upload date filter: this week
                    MenuItem { text: qsTr("This week") }
                    //: Upload date filter: this month
                    MenuItem { text: qsTr("This month") }
                    //: Upload date filter: this year
                    MenuItem { text: qsTr("This year") }
                }
            }
            ComboBox {
                id: typeBox
                //: Search filter: kind of result
                label: qsTr("Type")
                currentIndex: searchModel.type
                menu: ContextMenu {
                    //: Result type filter: any type
                    MenuItem { text: qsTr("Any") }
                    //: Result type filter: videos
                    MenuItem { text: qsTr("Videos") }
                    //: Result type filter: channels
                    MenuItem { text: qsTr("Channels") }
                    //: Result type filter: playlists
                    MenuItem { text: qsTr("Playlists") }
                }
            }
            ComboBox {
                id: durationBox
                //: Search filter: video length
                label: qsTr("Length")
                currentIndex: Math.max(0, dialog.durationValues.indexOf(searchModel.duration))
                menu: ContextMenu {
                    //: Duration filter: any length
                    MenuItem { text: qsTr("Any length") }
                    //: Duration filter: under 4 minutes
                    MenuItem { text: qsTr("Under 4 minutes") }
                    //: Duration filter: 4 to 20 minutes
                    MenuItem { text: qsTr("4-20 minutes") }
                    //: Duration filter: over 20 minutes
                    MenuItem { text: qsTr("Over 20 minutes") }
                }
            }
            TextSwitch {
                id: liveSwitch
                //: Search filter: only live streams
                text: qsTr("Live now")
                checked: searchModel.live
            }
            TextSwitch {
                id: mixedSwitch
                //: Search option: channels and playlists among the results when no type is chosen
                text: qsTr("Mixed results")
                description: qsTr("Show channels and playlists together with videos.")
                checked: searchModel.mixed
                enabled: typeBox.currentIndex === 0
            }

            SectionHeader {
                //: Section of the search filter dialog about hidden rows
                text: qsTr("Hide")
            }
            TextSwitch {
                id: shortsSwitch
                //: Search filter switch: leave Shorts out of the results
                text: qsTr("Hide Shorts")
                checked: searchModel.hideShorts
            }
            TextSwitch {
                id: membersSwitch
                //: Search filter switch: leave members-only videos out of the results
                text: qsTr("Hide members-only videos")
                checked: searchModel.hideMembersOnly
            }

            SectionHeader {
                visible: typeof suggestionModel !== "undefined"
                //: Section of the search filter dialog about the local search history
                text: qsTr("Search history")
            }
            TextSwitch {
                visible: typeof suggestionModel !== "undefined"
                //: Stops recording new searches in the local history
                text: qsTr("Pause search history")
                checked: typeof suggestionModel !== "undefined" && suggestionModel.historyPaused
                onClicked: suggestionModel.historyPaused = checked
            }
            Button {
                visible: typeof suggestionModel !== "undefined"
                anchors.horizontalCenter: parent.horizontalCenter
                enabled: typeof suggestionModel !== "undefined" && suggestionModel.historyCount > 0
                //: Deletes the local search history
                text: qsTr("Clear search history")
                onClicked: suggestionModel.clearHistory()
            }
        }
        VerticalScrollDecorator { }
    }
}
