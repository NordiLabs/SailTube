// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "TapGestures.h"

namespace nordtile { namespace player {

const int TapGestures::kDoubleTapWindowMs;
const int TapGestures::kFlushDelayMs;
const int TapGestures::kGestureEndMs;
const int TapGestures::kDefaultStepSeconds;
const int ControlsVisibility::kAutoHideMs;

void TapGestures::setStepSeconds(int seconds) {
    m_stepSeconds = (seconds == 5 || seconds == 10 || seconds == 15) ? seconds : kDefaultStepSeconds;
}

std::int64_t TapGestures::clampTarget(std::int64_t ms) const {
    if (ms < 0) return 0;
    if (m_duration > 0 && ms > m_duration) return m_duration;
    return ms;
}

void TapGestures::addStep(TapSide side) {
    if (side != m_side) {
        m_side = side;
        m_displaySteps = 0; // displayed counter restarts; the target continues
    }
    ++m_displaySteps;
    std::int64_t step = static_cast<std::int64_t>(m_stepSeconds) * 1000;
    m_target = clampTarget(m_target + (side == TapSide::Left ? -step : step));
    m_flushDue = true;
}

TapOutput TapGestures::tap(std::int64_t nowMs, TapSide side, std::int64_t positionMs, std::int64_t durationMs) {
    TapOutput out = advance(nowMs);
    m_duration = durationMs;
    if (m_gesture) {
        addStep(side);
        m_lastTapAt = nowMs;
    } else if (m_pending && side == m_pendingSide) {
        m_pending = false;
        m_gesture = true;
        m_side = side;
        m_displaySteps = 0;
        m_target = clampTarget(positionMs);
        addStep(side);
        m_lastTapAt = nowMs;
    } else {
        m_pending = true; // first tap, or the other half inside the window
        m_pendingAt = nowMs;
        m_pendingSide = side;
    }
    return out;
}

TapOutput TapGestures::advance(std::int64_t nowMs) {
    TapOutput out;
    if (m_pending && nowMs >= m_pendingAt + kDoubleTapWindowMs) {
        m_pending = false;
        out.toggleControls = true;
    }
    if (m_gesture) {
        if (m_flushDue && nowMs >= m_lastTapAt + kFlushDelayMs) {
            m_flushDue = false;
            out.seek = true;
            out.seekTargetMs = m_target;
        }
        if (nowMs >= m_lastTapAt + kGestureEndMs) {
            m_gesture = false;
            m_displaySteps = 0;
            out.gestureEnded = true;
        }
    }
    return out;
}

std::int64_t TapGestures::nextDeadlineMs() const {
    std::int64_t best = -1;
    if (m_pending) best = m_pendingAt + kDoubleTapWindowMs;
    if (m_gesture) {
        std::int64_t d = m_flushDue ? m_lastTapAt + kFlushDelayMs : m_lastTapAt + kGestureEndMs;
        if (best < 0 || d < best) best = d;
    }
    return best;
}

void TapGestures::cancel() {
    m_pending = false;
    m_gesture = false;
    m_flushDue = false;
    m_displaySteps = 0;
}

int TapGestures::displayTotalSeconds() const {
    int total = m_displaySteps * m_stepSeconds;
    return m_side == TapSide::Left ? -total : total;
}

void ControlsVisibility::arm(std::int64_t nowMs) {
    m_deadline = (m_visible && m_playing && !m_menuOpen) ? nowMs + kAutoHideMs : -1;
}

void ControlsVisibility::setPlaying(std::int64_t nowMs, bool playing) {
    m_playing = playing;
    if (!playing) m_visible = true;
    arm(nowMs);
}

void ControlsVisibility::setEnded() {
    m_playing = false;
    m_visible = true;
    m_deadline = -1;
}

void ControlsVisibility::setMenuOpen(std::int64_t nowMs, bool open) {
    m_menuOpen = open;
    if (open) m_visible = true;
    arm(nowMs);
}

void ControlsVisibility::interact(std::int64_t nowMs) {
    m_visible = true;
    arm(nowMs);
}

void ControlsVisibility::toggle(std::int64_t nowMs) {
    m_visible = !m_visible;
    if (!m_visible) m_menuOpen = false;
    arm(nowMs);
}

bool ControlsVisibility::advance(std::int64_t nowMs) {
    if (m_deadline < 0 || nowMs < m_deadline) return false;
    m_visible = false;
    m_menuOpen = false;
    m_deadline = -1;
    return true;
}

} } // namespace nordtile::player
