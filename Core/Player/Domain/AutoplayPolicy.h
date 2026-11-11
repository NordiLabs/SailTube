// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

// PLY-25 autoplay plan, time-injected (milliseconds). Qt-free.
// - Playlists and the queue advance on natural end; playback stops after the
//   last item (no related autoplay after a playlist ends).
// - Without a queue, related-video autoplay (setting, default on) runs a 5 s
//   cancellable "up next" countdown, then plays the related candidate.
// - The next item REPLACES the watch page (replacePage), so Back returns to
//   the list.
// - Only the owner autoplays (PLY-10): pass SessionArbiter::mayAct().
// Also answers MPRIS CanGoNext / CanGoPrevious (MPR-01) and the cover's
// "next" action (MPR-02).

#include <cstdint>
#include <string>
#include <vector>

namespace nordtile { namespace player {

struct AutoplayDecision {
    enum Kind {
        None,
        PlayItem,       // open videoId (queueIndex >= 0 for a queue item)
        StartCountdown, // show "up next" for videoId until countdownDeadlineMs
        StopAtEnd,      // end of the list: stop, stay on the page
        RestartCurrent  // "previous" near the start / on the first item: seek to 0
    };
    Kind kind = None;
    std::string videoId;
    int queueIndex = -1;
    bool fromRelated = false;
    bool replacePage = true;            // PLY-25: the next item replaces the watch page
    std::int64_t countdownDeadlineMs = -1;
};

class AutoplayPolicy {
public:
    static const int kRelatedCountdownMs = 5000;   // PLY-25
    static const int kPreviousRestartsAfterMs = 3000; // "previous" past 3 s restarts the item

    // SET-01 "Autoplay related: On, with a countdown". Turning it off cancels
    // a running countdown.
    void setRelatedAutoplay(bool on);
    bool relatedAutoplay() const { return m_related; }

    // The queue (a playlist, or a user queue) and the item playing now.
    void setQueue(const std::vector<std::string> &videoIds, int currentIndex);
    void clearQueue();
    // The related video that would autoplay without a queue ("" for none).
    void setRelatedCandidate(const std::string &videoId) { m_candidate = videoId; }

    // MPR-01: a queue exists when it has at least two items.
    bool hasQueue() const { return m_queue.size() > 1; }
    // MPR-01 CanGoNext: a queue with an item after the current one.
    bool canGoNext() const;
    // MPR-01 CanGoPrevious: any queue (on the first item, previous restarts it).
    bool canGoPrevious() const { return hasQueue(); }

    // PLY-25: natural end of stream. owner = SessionArbiter::mayAct(page).
    AutoplayDecision onEnded(std::int64_t nowMs, bool owner);
    // Countdown timer fired (call at nextDeadlineMs()).
    AutoplayDecision advance(std::int64_t nowMs);
    // PLY-25: the user cancelled "up next".
    void cancelCountdown();
    bool countdownActive() const { return m_countdownDeadline >= 0; }
    std::int64_t countdownRemainingMs(std::int64_t nowMs) const;
    std::int64_t nextDeadlineMs() const { return m_countdownDeadline; }

    // MPRIS Next / cover next / mini-player next.
    AutoplayDecision next();
    // MPRIS Previous: restart when past 3 s or on the first item.
    AutoplayDecision previous(std::int64_t positionMs);

    // PLY-25 (MAY) queue panel editing. The playing item keeps playing:
    // moving follows it; removing it is refused.
    bool moveItem(int from, int to);
    bool removeItem(int index);
    const std::vector<std::string> &queue() const { return m_queue; }
    int currentIndex() const { return m_index; }

private:
    AutoplayDecision playIndex(int index);

    std::vector<std::string> m_queue;
    int m_index = -1;
    bool m_related = true;
    std::string m_candidate;
    std::string m_countdownVideo;
    std::int64_t m_countdownDeadline = -1;
};

} } // namespace nordtile::player
