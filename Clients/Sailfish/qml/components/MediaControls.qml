// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
import QtQuick 2.6
import Amber.Mpris 1.0

// MPRIS (lock screen, headset, other controllers) through Amber.Mpris (PLT-10,
// MPR-01, MPR-03). Capabilities come from MediaControlsPolicy via the session:
// CanSeek only when seekable and not live, CanGoNext/CanGoPrevious only with a
// queue, Stop really stops, artUrl is a local cached file (never a remote URL).
MprisPlayer {
    serviceName: "sailtube"
    identity: "SailTube"
    desktopEntry: "harbour-sailtube"
    canControl: true
    canPlay: playback.canPlay
    canPause: playback.canPause
    canSeek: playback.canSeek
    canGoNext: playback.canGoNext
    canGoPrevious: playback.canGoPrevious
    playbackStatus: playback.playing ? Mpris.Playing
                    : playback.canPause ? Mpris.Paused : Mpris.Stopped
    // Amber.Mpris takes and hands back milliseconds, the session's own unit, and converts to
    // the microseconds of MPRIS (mpris:length, Position, Seek, SetPosition) itself. No scaling
    // here: tests/mpris reads the published values on a session bus.
    position: playback.position

    metaData {
        trackId: playback.videoId !== "" ? "/org/nordilabs/sailtube/video/" + playback.videoId.replace(/[^A-Za-z0-9_]/g, "_") : ""
        title: playback.title
        contributingArtist: playback.channel
        artUrl: playback.artUrl
        // mpris:length is omitted for live streams and unknown durations.
        duration: playback.live ? 0 : playback.duration
    }

    onPlayRequested: playback.canPause ? playback.play() : playback.continueWatching()
    onPauseRequested: playback.pause()
    onPlayPauseRequested: playback.canPause ? playback.togglePlayback() : playback.continueWatching()
    onStopRequested: playback.stop()
    onNextRequested: playback.next()
    onPreviousRequested: playback.previous()
    onSeekRequested: if (playback.canSeek) playback.seek(playback.position + offset)
    onSetPositionRequested: if (playback.canSeek) playback.seek(position)
}
