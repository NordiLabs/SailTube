// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "FreezePolicy.h"

namespace nordtile { namespace player {

const int64_t FreezePolicy::kFreezeGraceMs;
const int64_t FreezePolicy::kRepaintDelayMs;

FreezeTarget FreezePolicy::freezeTarget(PlayMode mode) {
    switch (mode) {
    case PlayMode::Dual: return FreezeTarget::LockAndPauseVideoSource;
    case PlayMode::Muxed:
    case PlayMode::Hls:
    case PlayMode::Local: return FreezeTarget::DropBeforeVideoDecoder;
    case PlayMode::AudioOnly: return FreezeTarget::None;
    }
    return FreezeTarget::None;
}

FreezeAction FreezePolicy::configure(PlayMode mode, bool hasVideo, int64_t nowMs) {
    FreezeAction a;
    // A target change while frozen: thaw the old form first.
    const bool canFreeze = hasVideo && freezeTarget(mode) != FreezeTarget::None;
    if (frozen_ && (!canFreeze || mode != mode_)) {
        frozen_ = false;
        a.thawVideo = true;
    }
    mode_ = mode;
    hasVideo_ = hasVideo;
    repaintAt_ = -1;
    freezeAt_ = (!pictureWanted() && canFreeze && !frozen_) ? nowMs + kFreezeGraceMs : -1;
    return a;
}

FreezeAction FreezePolicy::onAppActive(bool active, int64_t nowMs) {
    FreezeAction a;
    if (active == active_) return a;
    active_ = active;
    const bool canFreeze = target() != FreezeTarget::None;
    if (!active) {
        repaintAt_ = -1;
        if (canFreeze && !frozen_ && !coverVisible_) freezeAt_ = nowMs + kFreezeGraceMs;
        return a;
    }
    freezeAt_ = -1;
    if (frozen_) {
        frozen_ = false;
        a.thawVideo = true;
    }
    if (hasVideo_) repaintAt_ = nowMs + kRepaintDelayMs;
    return a;
}

FreezeAction FreezePolicy::onCoverVisible(bool visible, int64_t nowMs) {
    FreezeAction a;
    if (visible == coverVisible_) return a;
    coverVisible_ = visible;
    if (active_) return a;  // foreground: unchanged behaviour
    if (visible) {
        freezeAt_ = -1;
        if (frozen_) {
            frozen_ = false;
            a.thawVideo = true;  // no flush, as on activation (PLY-09)
        }
        return a;
    }
    if (target() != FreezeTarget::None && !frozen_) freezeAt_ = nowMs + kFreezeGraceMs;
    return a;
}

FreezeAction FreezePolicy::onTimer(int64_t nowMs) {
    FreezeAction a;
    if (freezeAt_ >= 0 && nowMs >= freezeAt_) {
        freezeAt_ = -1;
        if (!pictureWanted() && !frozen_ && target() != FreezeTarget::None) {
            frozen_ = true;
            a.freezeVideo = true;
        }
    }
    if (repaintAt_ >= 0 && nowMs >= repaintAt_) {
        repaintAt_ = -1;
        if (active_) a.repaint = true;
    }
    return a;
}

int64_t FreezePolicy::nextDeadlineMs() const {
    if (freezeAt_ < 0) return repaintAt_;
    if (repaintAt_ < 0) return freezeAt_;
    return freezeAt_ < repaintAt_ ? freezeAt_ : repaintAt_;
}

void FreezePolicy::onTeardown() {
    frozen_ = false;
    freezeAt_ = -1;
    repaintAt_ = -1;
    hasVideo_ = false;
}

} } // namespace nordtile::player
