// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
import QtQuick 2.6
import Sailfish.Silica 1.0
import "Format.js" as Format

// LIB-05 "Ask": resume at mm:ss or start over; the bridge auto-resumes after 5 s.
Rectangle {
    id: offer
    visible: libraryPlayback.resumeOfferVisible
    color: Qt.rgba(0, 0, 0, 0.7) // theme-exempt: scrim over the video
    radius: Theme.paddingSmall
    width: column.width + 2 * Theme.paddingLarge
    height: column.height + 2 * Theme.paddingMedium

    Column {
        id: column
        anchors.centerIn: parent
        spacing: Theme.paddingSmall
        Button {
            color: Theme.lightPrimaryColor   // on the scrim in every ambience

            anchors.horizontalCenter: parent.horizontalCenter
            //: Resume prompt; %1 is a time such as 12:34, %2 the seconds before it resumes by itself
            text: qsTr("Resume at %1 (%2)").arg(Format.time(libraryPlayback.resumeOfferPosition))
                                         .arg(libraryPlayback.resumeOfferSecondsLeft)
            onClicked: libraryPlayback.acceptResume()
        }
        Button {
            color: Theme.lightPrimaryColor   // on the scrim in every ambience

            anchors.horizontalCenter: parent.horizontalCenter
            //: Resume prompt button that plays the video from the beginning
            text: qsTr("Start over")
            onClicked: libraryPlayback.declineResume()
        }
    }
}
