// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "AudioResourcePolicy.h"

namespace nordtile { namespace player {

AudioResourceAction AudioResourcePolicy::requestPlay() {
    AudioResourceAction a;
    m_wantPlay = true;
    m_autoResume = false;
    if (m_state == State::Granted) {
        a.play = true;
    } else if (m_state != State::Acquiring) {
        m_state = State::Acquiring;
        a.acquire = true;
    }
    return a;
}

AudioResourceAction AudioResourcePolicy::requestPause() {
    AudioResourceAction a;
    m_wantPlay = false;
    m_autoResume = false;
    a.pause = true;
    return a;
}

AudioResourceAction AudioResourcePolicy::stop() {
    AudioResourceAction a;
    m_wantPlay = false;
    m_autoResume = false;
    a.pause = true;
    if (m_state != State::Released) a.release = true;
    m_state = State::Released;
    return a;
}

AudioResourceAction AudioResourcePolicy::onGranted() {
    AudioResourceAction a;
    if (m_state == State::Released) return a; // late grant after stop: ignore
    State before = m_state;
    m_state = State::Granted;
    if (before == State::Lost) {
        if (m_autoResume && m_resumeSetting) {
            m_wantPlay = true;
            a.play = true;
        }
        m_autoResume = false;
    } else if (m_wantPlay) {
        a.play = true;
    }
    return a;
}

AudioResourceAction AudioResourcePolicy::onLost(bool wasPlaying) {
    AudioResourceAction a;
    if (m_state == State::Released) return a;
    bool pendingStart = m_state == State::Acquiring && m_wantPlay;
    m_state = State::Lost;
    a.pause = true;
    m_autoResume = m_resumeSetting && (wasPlaying || pendingStart);
    m_wantPlay = false;
    return a;
}

} } // namespace nordtile::player
