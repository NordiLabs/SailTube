// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
import QtQuick 2.6
import org.nordilabs.sailtube 1.0

// The live picture of the cover (user-approved cover video): the playback component's CoverVideoItem
// on the shared player's frames, fitted (PreserveAspectFit, its default). It draws at about
// 15 fps only while `active` and keeps the last frame otherwise. Kept in its own file so the
// cover compiles without the player module.
CoverVideoItem {
    source: playback.mediaObject
}
