// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// The real DownloadQualityDialog.qml, pushed over an empty page (the fake `downloads` as a context property).
import QtQuick 2.6
import Sailfish.Silica 1.0

ApplicationWindow {
    id: app
    cover: null
    property var dialog: null
    function openDialog() {
        dialog = pageStack.push(Qt.resolvedUrl("../../Clients/Sailfish/qml/pages/DownloadQualityDialog.qml"),
                                { video: { videoId: "dlgquality1", title: "Dialog" } }, PageStackAction.Immediate)
    }
    initialPage: Component { Page { } }
}
