// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
import QtQuick 2.6
import Sailfish.Silica 1.0
import Nemo.Notifications 1.0

// DL-04 system notification (SHOULD) and DL-06 visible notices.
Item {
    Notification {
        id: progressNote
        appName: "SailTube"
        //: System notification summary while downloads run, %n is the number of active downloads
        summary: qsTr("Downloading %n video(s)", "", downloads.activeCount)
        isTransient: false
    }
    Notification {
        id: doneNote
        appName: "SailTube"
        isTransient: true
    }
    Notification {
        id: notice
        appName: "SailTube"
        isTransient: true
    }

    Connections {
        target: downloads
        onActiveChanged: {
            if (downloads.activeCount > 0) progressNote.publish()
            else progressNote.close()
        }
        onFinished: {
            doneNote.summary = ok
                    //: System notification when a download completed
                    ? qsTr("Download complete")
                    //: System notification when a download failed
                    : qsTr("Download failed")
            doneNote.body = title
            doneNote.publish()
        }
        onNotice: {
            notice.summary = text
            notice.publish()
        }
    }
}
