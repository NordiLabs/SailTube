// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
import QtQuick 2.6

// Headless stand-in for the playback component's OverlayController: a tap toggles the controls,
// interaction shows them; no gesture timing.
QtObject {
    property bool controlsVisible: true
    property bool menuOpen: false
    property bool gestureActive: false
    property int gestureSide: 0
    property int gestureSeconds: 0
    property real gestureTargetMs: 0
    property int stepSeconds: 10
    signal controlsChanged()
    signal gestureChanged()
    signal seekRequested(real positionMs)
    onControlsVisibleChanged: controlsChanged()
    function tap(x, width, positionMs, durationMs) { controlsVisible = !controlsVisible }
    function interact() { controlsVisible = true }
    function setPlaying(playing) { }
    function setEnded() { controlsVisible = true }
    function cancelGesture() { }
}
