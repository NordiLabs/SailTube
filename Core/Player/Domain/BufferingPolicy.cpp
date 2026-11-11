// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "BufferingPolicy.h"

namespace nordtile { namespace player {

const int BufferingPolicy::kLiveEnterPercent;
const int BufferingPolicy::kLiveResumePercent;

void BufferingPolicy::reset(bool network, bool liveHls) {
    network_ = network;
    live_ = liveHls;
    buffering_ = false;
    startPhase_ = false;
    for (int i = 0; i < 3; ++i) {
        percent_[i] = 100;
        filled_[i] = true;
    }
}

void BufferingPolicy::beginStartPhase() {
    startPhase_ = true;
    for (int i = 0; i < 3; ++i) filled_[i] = false;
}

BufferingAction BufferingPolicy::current() const {
    BufferingAction a;
    a.showIndicator = buffering_ && wantsPlaying_;
    return a;
}

BufferingAction BufferingPolicy::onBufferingPercent(Branch branch, int percent) {
    if (!network_) return current();
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    percent_[slot(branch)] = percent;

    const int enterBelow = live_ ? kLiveEnterPercent : 100;
    const int leaveAt = live_ ? kLiveResumePercent : 100;
    if (percent >= leaveAt) filled_[slot(branch)] = true;

    int lowest = 100;
    for (int i = 0; i < 3; ++i) {
        // Start phase: a branch's first fill never holds or pauses playback.
        const int level = filled_[i] ? percent_[i] : 100;
        if (level < lowest) lowest = level;
    }

    BufferingAction a;
    if (!buffering_ && lowest < enterBelow) {
        buffering_ = true;
        a.pausePipeline = wantsPlaying_;  // already paused by the user otherwise
    } else if (buffering_ && lowest >= leaveAt) {
        buffering_ = false;
        a.resumePipeline = wantsPlaying_;
    }
    a.showIndicator = buffering_ && wantsPlaying_;
    return a;
}

BufferingAction BufferingPolicy::onUserIntent(bool wantsPlaying) {
    wantsPlaying_ = wantsPlaying;
    BufferingAction a;
    if (!wantsPlaying) a.pausePipeline = true;
    else if (!buffering_) a.resumePipeline = true;
    a.showIndicator = buffering_ && wantsPlaying_;
    return a;
}

} } // namespace nordtile::player
