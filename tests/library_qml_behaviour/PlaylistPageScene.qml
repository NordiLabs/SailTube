// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// The real PlaylistPage.qml as the initial page of a Silica window (fakes as context properties).
import QtQuick 2.6
import Sailfish.Silica 1.0
import "../../Clients/Sailfish/qml/pages"

ApplicationWindow {
    cover: null
    initialPage: Component { PlaylistPage { playlistId: "p1" } }
}
