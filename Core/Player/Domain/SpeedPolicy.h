// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

// PLY-07 playback speed. Header-only, pure.
//   * Steps 0.25, 0.5, 0.75, 1, 1.25, 1.5, 1.75, 2 (pitch-corrected by
//     scaletempo in the audio chain; the rate rides on every seek).
//   * Remembered across videos: persist valueToRemember(); apply
//     rateToApply() through PrerollBarrier::arm() so it is engaged before the
//     first frame (PLY-06).
//   * Not changeable at the live edge (PLY-01): v1 has no DVR window, so the
//     engine passes atLiveEdge = (mode == Hls && stream is live). There the
//     applied rate is 1.0 and the remembered value is left untouched.

#include <cmath>

namespace nordtile { namespace player { namespace speed {

const int kStepCount = 8;

// Step i (clamped to 0..kStepCount-1): 0.25 * (i + 1).
inline double step(int index) {
    if (index < 0) index = 0;
    if (index >= kStepCount) index = kStepCount - 1;
    return 0.25 * (index + 1);
}

const double kMin = 0.25;
const double kMax = 2.0;
const double kNormal = 1.0;

// Index of the nearest step after clamping to [kMin, kMax]. Exact halfway
// values round up (0.375 -> 0.5). NaN / infinity -> the 1.0 step.
inline int indexOf(double rate) {
    if (!std::isfinite(rate)) return 3;
    if (rate <= kMin) return 0;
    if (rate >= kMax) return kStepCount - 1;
    return static_cast<int>(std::floor(rate / 0.25 - 1.0 + 0.5 + 1e-9));
}

// PLY-07: snap/clamp an arbitrary value to a step.
inline double snap(double rate) { return step(indexOf(rate)); }

// Next faster / slower step from the (snapped) current rate; saturates.
inline double next(double rate) { return step(indexOf(rate) + 1); }
inline double previous(double rate) { return step(indexOf(rate) - 1); }

// Rate the engine applies for a remembered value.
inline double rateToApply(double remembered, bool atLiveEdge) {
    return atLiveEdge ? kNormal : snap(remembered);
}

// Value to persist after the user picked `chosen`. At the live edge the menu
// is not offered; a stray choice there keeps the previous remembered value.
inline double valueToRemember(double previousRemembered, double chosen, bool atLiveEdge) {
    return atLiveEdge ? snap(previousRemembered) : snap(chosen);
}

// Whether the speed menu is offered.
inline bool speedChangeable(bool atLiveEdge) { return !atLiveEdge; }

} } } // namespace nordtile::player::speed
