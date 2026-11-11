// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "PositionPolicy.h"

namespace nordtile { namespace player {

const int64_t PositionPolicy::kVisibleIntervalMs;
const int64_t PositionPolicy::kHiddenIntervalMs;

int64_t PositionPolicy::pollIntervalMs(bool hasConsumer, bool controlsVisible, bool playing) const {
    if (!hasConsumer) return 0;
    if (!playing && durationKnown()) return 0;
    return controlsVisible ? kVisibleIntervalMs : kHiddenIntervalMs;
}

void PositionPolicy::reset(bool keepMetadataDuration) {
    positionMs_ = 0;
    durationMs_ = 0;
    if (!keepMetadataDuration) metadataDurationMs_ = 0;
    seeking_ = false;
    ended_ = false;
}

void PositionPolicy::setMetadataDurationMs(int64_t ms) {
    metadataDurationMs_ = ms > 0 ? ms : 0;
}

int64_t PositionPolicy::durationMs() const {
    return durationMs_ > 0 ? durationMs_ : metadataDurationMs_;
}

int64_t PositionPolicy::clamp(int64_t ms) const {
    if (ms < 0) return 0;
    const int64_t d = durationMs();
    if (d > 0 && ms > d) return d;
    return ms;
}

void PositionPolicy::onSeekRequested(int64_t targetMs) {
    ended_ = false;
    seeking_ = true;
    positionMs_ = clamp(targetMs);
}

void PositionPolicy::onSeekLanded(bool known, int64_t landedMs) {
    seeking_ = false;
    if (known) positionMs_ = clamp(landedMs);
}

void PositionPolicy::onSeekRefused() {
    seeking_ = false;
}

void PositionPolicy::onPositionQuery(bool ok, int64_t positionMs) {
    if (!ok || seeking_ || ended_) return;
    positionMs_ = clamp(positionMs);
}

void PositionPolicy::onDurationQuery(bool ok, int64_t durationMs) {
    if (ok && durationMs > 0) durationMs_ = durationMs;
}

void PositionPolicy::onEos() {
    ended_ = true;
    seeking_ = false;
    const int64_t d = durationMs();
    if (d > 0) positionMs_ = d;
}

int64_t PositionPolicy::positionMs() const {
    return positionMs_;
}

} } // namespace nordtile::player
