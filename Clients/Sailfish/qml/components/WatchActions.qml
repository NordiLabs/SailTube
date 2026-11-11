// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
import QtQuick 2.6
import Sailfish.Silica 1.0

// One-tap watch page actions (DSC-14): share a link through the Sailfish share sheet
// (Sailfish.Share ShareAction, text/x-url) and copy a link. The share module is loaded at run
// time so a system without it still opens the page; sharing then copies the link instead and
// says so.
Item {
    id: root
    // Short feedback for the page to show ("Link copied"); cleared after a few seconds.
    property string message: ""
    property var shareAction: null   // null = not tried yet, false = unavailable

    function copy(url) {
        if (url === "") return
        Clipboard.text = url
        //: Feedback after the video link was copied to the clipboard
        show(qsTr("Link copied"))
    }

    function share(url, title) {
        if (url === "") return
        if (shareAction === null) {
            try {
                shareAction = Qt.createQmlObject('import Sailfish.Share 1.0; ShareAction { mimeType: "text/x-url" }',
                                                 root, "WatchShareAction")
            } catch (e) {
                shareAction = false
            }
        }
        if (shareAction) {
            //: Title of the system share sheet for a video link
            shareAction.title = qsTr("Share link")
            shareAction.resources = [{ "type": "text/x-url", "linkTitle": title, "status": url }]
            shareAction.trigger()
            return
        }
        Clipboard.text = url
        //: Feedback when the share sheet is unavailable and the link was copied instead
        show(qsTr("Sharing is not available; link copied"))
    }

    function show(text) {
        message = text
        clearTimer.restart()
    }

    Timer {
        id: clearTimer
        interval: 3000
        onTriggered: root.message = ""
    }
}
