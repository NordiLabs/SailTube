// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

// PLY-23 / PLT-12 audio role and Sailfish audio-resource policy. Qt-free
// state machine; the engine owns the resource client (e.g. AudioResource from
// libaudioresource / Amber) and the pulsesink, and executes the actions.
//
//   requestPlay -> acquire -> onGranted -> play
//   onLost (call, alarm) -> pause at once (well inside the 1 s budget);
//   onGranted later -> resume only if the setting allows AND it was playing
//   when lost. A user pause (or stop) during the loss cancels the auto-resume.

namespace nordtile { namespace player {

// PLT-12: PulseAudio media.role set on every audio sink (stream-properties of
// pulsesink, in dual, muxed, HLS, local and audio-only modes). "x-maemo" is
// the role the Sailfish policy treats as media playback. PROVISIONAL: it must
// be confirmed by experiment E8 (SAILTUBE_REQUIREMENTS.md §7) on R1 and R2;
// change it here only, every sink reads this constant.
const char *const kMediaRole = "x-maemo";

struct AudioResourceAction {
    bool acquire = false; // ask the policy for the audio resource
    bool play = false;    // set the pipeline to PLAYING
    bool pause = false;   // set the pipeline to PAUSED now
    bool release = false; // give the resource back
    bool any() const { return acquire || play || pause || release; }
};

class AudioResourcePolicy {
public:
    enum class State { Released, Acquiring, Granted, Lost };

    // PLY-23: "resume after an interruption" setting. Not in the SET-01 table;
    // default on (an interpretation).
    void setResumeAfterInterruption(bool on) { m_resumeSetting = on; if (!on) m_autoResume = false; }
    bool resumeAfterInterruption() const { return m_resumeSetting; }

    // PLY-23: the user (or autoplay) wants playback. Never PLAYING before the
    // resource is granted.
    AudioResourceAction requestPlay();
    // PLY-23: user pause. Keeps the resource, cancels any pending auto-resume.
    AudioResourceAction requestPause();
    // Stop / session end: pause and release.
    AudioResourceAction stop();
    // Resource callbacks from the policy daemon.
    AudioResourceAction onGranted();
    // PLY-23: lost to a call or alarm. wasPlaying: the pipeline was PLAYING.
    AudioResourceAction onLost(bool wasPlaying);

    State state() const { return m_state; }
    bool wantsPlay() const { return m_wantPlay; }
    bool autoResumePending() const { return m_autoResume; }

private:
    State m_state = State::Released;
    bool m_wantPlay = false;
    bool m_autoResume = false;
    bool m_resumeSetting = true;
};

} } // namespace nordtile::player
