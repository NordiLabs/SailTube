// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "AutoplayPolicy.h"

namespace nordtile { namespace player {

const int AutoplayPolicy::kRelatedCountdownMs;
const int AutoplayPolicy::kPreviousRestartsAfterMs;

void AutoplayPolicy::setRelatedAutoplay(bool on) {
    m_related = on;
    if (!on) cancelCountdown();
}

void AutoplayPolicy::setQueue(const std::vector<std::string> &videoIds, int currentIndex) {
    m_queue = videoIds;
    m_index = (currentIndex >= 0 && currentIndex < static_cast<int>(m_queue.size())) ? currentIndex
                                                                                       : (m_queue.empty() ? -1 : 0);
    cancelCountdown();
}

void AutoplayPolicy::clearQueue() {
    m_queue.clear();
    m_index = -1;
    cancelCountdown();
}

bool AutoplayPolicy::canGoNext() const {
    return hasQueue() && m_index + 1 < static_cast<int>(m_queue.size());
}

AutoplayDecision AutoplayPolicy::playIndex(int index) {
    AutoplayDecision d;
    m_index = index;
    d.kind = AutoplayDecision::PlayItem;
    d.videoId = m_queue[index];
    d.queueIndex = index;
    cancelCountdown();
    return d;
}

AutoplayDecision AutoplayPolicy::onEnded(std::int64_t nowMs, bool owner) {
    AutoplayDecision d;
    if (!owner) return d; // PLY-10: a page lower in the stack never acts
    if (hasQueue()) {
        if (canGoNext()) return playIndex(m_index + 1);
        d.kind = AutoplayDecision::StopAtEnd;
        return d;
    }
    if (m_related && !m_candidate.empty()) {
        m_countdownVideo = m_candidate;
        m_countdownDeadline = nowMs + kRelatedCountdownMs;
        d.kind = AutoplayDecision::StartCountdown;
        d.videoId = m_countdownVideo;
        d.fromRelated = true;
        d.countdownDeadlineMs = m_countdownDeadline;
        return d;
    }
    d.kind = AutoplayDecision::StopAtEnd;
    return d;
}

AutoplayDecision AutoplayPolicy::advance(std::int64_t nowMs) {
    AutoplayDecision d;
    if (m_countdownDeadline < 0 || nowMs < m_countdownDeadline) return d;
    d.kind = AutoplayDecision::PlayItem;
    d.videoId = m_countdownVideo;
    d.fromRelated = true;
    cancelCountdown();
    return d;
}

void AutoplayPolicy::cancelCountdown() {
    m_countdownDeadline = -1;
    m_countdownVideo.clear();
}

std::int64_t AutoplayPolicy::countdownRemainingMs(std::int64_t nowMs) const {
    if (m_countdownDeadline < 0) return 0;
    std::int64_t left = m_countdownDeadline - nowMs;
    return left > 0 ? left : 0;
}

AutoplayDecision AutoplayPolicy::next() {
    if (!canGoNext()) return AutoplayDecision();
    return playIndex(m_index + 1);
}

AutoplayDecision AutoplayPolicy::previous(std::int64_t positionMs) {
    AutoplayDecision d;
    if (!hasQueue()) return d;
    if (positionMs > kPreviousRestartsAfterMs || m_index <= 0) {
        d.kind = AutoplayDecision::RestartCurrent;
        d.videoId = m_queue[m_index];
        d.queueIndex = m_index;
        d.replacePage = false;
        return d;
    }
    return playIndex(m_index - 1);
}

bool AutoplayPolicy::moveItem(int from, int to) {
    int n = static_cast<int>(m_queue.size());
    if (from < 0 || from >= n || to < 0 || to >= n) return false;
    if (from == to) return true;
    std::string item = m_queue[from];
    m_queue.erase(m_queue.begin() + from);
    m_queue.insert(m_queue.begin() + to, item);
    if (m_index == from) {
        m_index = to;
    } else if (from < m_index && to >= m_index) {
        --m_index;
    } else if (from > m_index && to <= m_index) {
        ++m_index;
    }
    return true;
}

bool AutoplayPolicy::removeItem(int index) {
    int n = static_cast<int>(m_queue.size());
    if (index < 0 || index >= n || index == m_index) return false;
    m_queue.erase(m_queue.begin() + index);
    if (index < m_index) --m_index;
    return true;
}

} } // namespace nordtile::player
