// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
import QtQuick 2.6
import org.nordilabs.sailtube 1.0

// The mini-player's live frame (PLY-11): the playback component's surface on the shared player.
VideoSurfaceItem {
    source: playback.mediaObject
    fillMode: VideoSurfaceItem.PreserveAspectCrop
}
