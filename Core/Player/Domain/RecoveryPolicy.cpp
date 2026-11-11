// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "RecoveryPolicy.h"

namespace nordtile { namespace player {

const int RecoveryPolicy::kMaxTries;
const int RecoveryPolicy::kBackoffMs;
const int RecoveryPolicy::kCleanPlaybackMs;

void RecoveryPolicy::reset() {
    bool fg = m_foreground;
    *this = RecoveryPolicy();
    m_foreground = fg;
}

RecoveryAction RecoveryPolicy::recoverNow() {
    m_state = RecoveryState::Recovering;
    m_deadline = -1;
    m_haveRecoveryPoint = false;
    RecoveryAction a;
    a.kind = RecoveryAction::Recover;
    a.resumePositionMs = m_resumeMs;
    a.resumeAtLive = m_live;
    return a;
}

RecoveryAction RecoveryPolicy::onFault(std::int64_t nowMs, std::int64_t positionMs, bool live,
                                       const std::string &reason) {
    RecoveryAction a;
    if (m_state == RecoveryState::Failed) return a; // already showing Retry
    bool midRecovery = m_state == RecoveryState::Waiting || m_state == RecoveryState::Recovering
            || m_state == RecoveryState::Deferred;
    if (!midRecovery) {
        m_resumeMs = positionMs < 0 ? 0 : positionMs;
        m_live = live;
    }
    m_reason = reason;
    if (!m_foreground) {
        m_state = RecoveryState::Deferred; // no budget spent
        m_deadline = -1;
        return a;
    }
    if (m_state == RecoveryState::Waiting) return a; // backoff already armed for this fault burst
    if (m_tries >= kMaxTries) {
        m_state = RecoveryState::Failed;
        m_deadline = -1;
        a.kind = RecoveryAction::Fail;
        return a;
    }
    ++m_tries;
    m_state = RecoveryState::Waiting;
    m_deadline = nowMs + kBackoffMs;
    a.kind = RecoveryAction::Wait;
    a.deadlineMs = m_deadline;
    return a;
}

RecoveryAction RecoveryPolicy::onTimer(std::int64_t nowMs) {
    if (m_state != RecoveryState::Waiting || nowMs < m_deadline) return RecoveryAction();
    return recoverNow();
}

void RecoveryPolicy::onPlaybackProgress(std::int64_t positionMs) {
    if (m_state == RecoveryState::Recovering) {
        m_state = RecoveryState::Idle;
        m_haveRecoveryPoint = true;
        m_recoveryPointMs = positionMs;
        return;
    }
    if (m_state != RecoveryState::Idle || !m_haveRecoveryPoint) return;
    if (positionMs < m_recoveryPointMs) {
        m_recoveryPointMs = positionMs; // user sought back: measure from here
    } else if (positionMs - m_recoveryPointMs >= kCleanPlaybackMs) {
        m_tries = 0;
        m_haveRecoveryPoint = false;
    }
}

RecoveryAction RecoveryPolicy::setForeground(bool foreground, std::int64_t nowMs) {
    (void)nowMs;
    m_foreground = foreground;
    if (!foreground) {
        if (m_state == RecoveryState::Waiting) {
            // Backoff interrupted by going to the background: give the try back
            // and defer, so background stalls never consume the budget.
            if (m_tries > 0) --m_tries;
            m_state = RecoveryState::Deferred;
            m_deadline = -1;
        }
        return RecoveryAction();
    }
    if (m_state != RecoveryState::Deferred) return RecoveryAction();
    m_tries = 1; // fresh budget, this recovery is its first try
    return recoverNow();
}

RecoveryAction RecoveryPolicy::retry(std::int64_t nowMs) {
    (void)nowMs;
    if (m_state != RecoveryState::Failed) return RecoveryAction();
    m_tries = 0; // Retry resets the automatic budget
    return recoverNow();
}

} } // namespace nordtile::player
