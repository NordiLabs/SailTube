// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "PrerollBarrier.h"

namespace nordtile { namespace player {

namespace {
double saneRate(double r) { return r > 0.0 ? r : 1.0; }
int64_t sanePosition(int64_t ms) { return ms > 0 ? ms : 0; }
} // namespace

void PrerollBarrier::arm(int64_t positionMs, double rate) {
    armed_ = true;
    prerolled_ = false;
    applied_ = false;
    positionMs_ = sanePosition(positionMs);
    rate_ = saneRate(rate);
}

bool PrerollBarrier::deferSeek(int64_t positionMs) {
    if (!armed_ || prerolled_) return false;
    positionMs_ = sanePosition(positionMs);
    return true;
}

bool PrerollBarrier::deferRate(double rate) {
    if (!armed_ || prerolled_) return false;
    rate_ = saneRate(rate);
    return true;
}

bool PrerollBarrier::wantsStartSeek() const {
    return armed_ && !prerolled_ && !applied_ && (positionMs_ != 0 || rate_ != 1.0);
}

void PrerollBarrier::onStartSeekApplied(int64_t positionMs, double rate) {
    if (!armed_ || prerolled_) return;
    applied_ = true;
    appliedPositionMs_ = sanePosition(positionMs);
    appliedRate_ = saneRate(rate);
}

bool PrerollBarrier::onAsyncDone(DeferredSeek *out) {
    if (!armed_ || prerolled_) return false;
    prerolled_ = true;
    if (positionMs_ == 0 && rate_ == 1.0 && !applied_) return false;
    if (applied_ && positionMs_ == appliedPositionMs_ && rate_ == appliedRate_) return false;
    if (out) {
        out->positionMs = positionMs_;
        out->rate = rate_;
    }
    return true;
}

bool startSeekBeforePreroll(PlayMode mode) { return mode != PlayMode::Hls; }

} } // namespace nordtile::player
