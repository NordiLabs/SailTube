// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

// PLY-12 / PLT-11 display keep-on decision. Header-only, Qt-free.
// The QML side binds Nemo.KeepAlive DisplayBlanking.preventBlanking (FULL
// only, loaded so that a missing plugin degrades quietly) to preventBlanking().
// STORE compiles KeepAlive out: keepAliveAvailable is false there and the
// settings page shows keepOnLimitationNote() next to the setting.

namespace nordtile { namespace player {

// SET-01 / PLY-12: "Keep display on" defaults to on (FinTube defaults off).
const bool kKeepDisplayOnDefault = true;

struct KeepOnInputs {
    bool settingEnabled = kKeepDisplayOnDefault;
    bool playing = false;            // actually playing, not paused/buffering-paused
    bool foreground = false;         // application active (not minimised, display on)
    bool hasVideo = false;           // a video branch exists (not an audio file)
    bool listenOnly = false;         // SEL-08 listen-only mode
    bool keepAliveAvailable = false; // FULL build with the plugin loaded
};

// PLY-12: hold the display on only while a video plays in the foreground.
// Released when paused, in listen-only mode or in the background.
inline bool preventBlanking(const KeepOnInputs &in) {
    return in.settingEnabled && in.playing && in.foreground && in.hasVideo && !in.listenOnly
            && in.keepAliveAvailable;
}

// PLY-12: the STORE build (no KeepAlive) documents the limitation in settings.
inline bool keepOnLimitationNote(bool keepAliveAvailable) { return !keepAliveAvailable; }

// PLT-11: the KeepAlive background job is for downloads only. Playback, and
// in particular audio-only/background playback, never holds it: audio relies
// on the audio resource (PLY-23, AudioResourcePolicy.h).
inline bool holdKeepAliveBackgroundJob(bool downloadsRunning, bool keepAliveAvailable) {
    return downloadsRunning && keepAliveAvailable;
}

} } // namespace nordtile::player
