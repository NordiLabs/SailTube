// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "SplitSeekGuard.h"

namespace nordtile { namespace player {

const int64_t SplitSeekGuard::kResendDelayMs;

void SplitSeekGuard::reset() {
    splits_ = 0;
    rebuildUsed_ = false;
    gaveUp_ = false;
}

void SplitSeekGuard::onUserSeek() {
    reset();
}

void SplitSeekGuard::onSourcesSet(const std::string &sourceKey) {
    if (sourceKey == sourceKey_) return;
    sourceKey_ = sourceKey;
    reset();
}

SplitSeekAction SplitSeekGuard::onSeekResult(bool videoAccepted, bool audioAccepted) {
    if (videoAccepted && audioAccepted) {
        reset();
        return SplitSeekAction::None;
    }
    if (!videoAccepted && !audioAccepted) return SplitSeekAction::None;

    ++splits_;
    if (gaveUp_) return SplitSeekAction::GiveUpKeepPlaying;
    if (splits_ == 1 && !rebuildUsed_) return SplitSeekAction::ResendAfter250ms;
    if (!rebuildUsed_) {
        rebuildUsed_ = true;
        return SplitSeekAction::RebuildOnceWithDeferredSeek;
    }
    gaveUp_ = true;
    return SplitSeekAction::GiveUpKeepPlaying;
}

} } // namespace nordtile::player
