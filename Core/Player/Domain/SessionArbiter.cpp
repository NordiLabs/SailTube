// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "SessionArbiter.h"

namespace nordtile { namespace player {

SessionArbiter::Entry *SessionArbiter::find(int contextId) {
    for (std::size_t i = 0; i < m_stack.size(); ++i)
        if (m_stack[i].contextId == contextId) return &m_stack[i];
    return 0;
}

// The context becomes owner of the player for videoId.
SessionDecision SessionArbiter::takeOver(int contextId, const std::string &videoId) {
    SessionDecision d;
    m_owner = contextId;
    if (m_active && m_video == videoId) {
        d.reattach = true;
        d.videoId = m_video;
        d.generation = m_generation;
    } else {
        if (m_active) {
            d.savePosition = true;
            d.stop = true;
            d.stoppedVideoId = m_video;
        }
        m_video = videoId;
        m_active = true;
        ++m_generation;
        d.resolve = true;
        d.videoId = videoId;
        d.generation = m_generation;
    }
    d.owner = m_owner;
    return d;
}

SessionDecision SessionArbiter::open(int contextId, const std::string &videoId) {
    if (contextId == kNoContext || contextId == kAppContext || videoId.empty()) return SessionDecision();
    Entry *e = find(contextId);
    if (e) {
        e->videoId = videoId;
    } else {
        Entry n; n.contextId = contextId; n.videoId = videoId;
        m_stack.push_back(n);
    }
    return takeOver(contextId, videoId);
}

SessionDecision SessionArbiter::activate(int contextId) {
    Entry *e = find(contextId);
    if (!e || m_owner == contextId) {
        SessionDecision d; d.owner = m_owner; return d;
    }
    std::string video = e->videoId; // copy: takeOver does not touch the stack, but stay safe
    return takeOver(contextId, video);
}

SessionDecision SessionArbiter::deactivate(int contextId, const PlaybackSnapshot &s) {
    SessionDecision d;
    if (!mayAct(contextId) || contextId == kAppContext) { d.owner = m_owner; return d; }
    if (m_active && m_keepOnLeave && (s.playing || s.positionMs > 0)) {
        m_owner = kAppContext;
        d.park = true;
        d.videoId = m_video;
        d.generation = m_generation;
    } else {
        if (m_active) {
            d.savePosition = true;
            d.stop = true;
            d.stoppedVideoId = m_video;
            m_active = false;
        }
        m_owner = kNoContext;
    }
    d.owner = m_owner;
    return d;
}

SessionDecision SessionArbiter::close(int contextId, const PlaybackSnapshot &s) {
    SessionDecision d = deactivate(contextId, s);
    for (std::size_t i = 0; i < m_stack.size(); ++i) {
        if (m_stack[i].contextId == contextId) { m_stack.erase(m_stack.begin() + i); break; }
    }
    return d;
}

SessionDecision SessionArbiter::stopSession() {
    SessionDecision d;
    if (m_active) {
        d.savePosition = true;
        d.stop = true;
        d.stoppedVideoId = m_video;
        m_active = false;
        if (m_owner == kAppContext) m_owner = kNoContext;
    }
    d.owner = m_owner;
    return d;
}

void SessionArbiter::sessionEnded() {
    m_active = false;
    if (m_owner == kAppContext) m_owner = kNoContext;
}

SessionDecision SessionArbiter::reresolve(int contextId) {
    SessionDecision d;
    if (m_active && mayAct(contextId)) {
        ++m_generation;
        d.resolve = true;
        d.videoId = m_video;
        d.generation = m_generation;
    }
    d.owner = m_owner;
    return d;
}

bool SessionArbiter::acceptResolve(const std::string &videoId, unsigned generation) const {
    return m_active && videoId == m_video && generation == m_generation;
}

} } // namespace nordtile::player
