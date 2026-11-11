// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

// PLY-17 overlay tap gestures and controls auto-hide, time-injected
// (milliseconds from any monotonic clock).
//
// Timer model: after every call, arm ONE single-shot timer for
// nextDeadlineMs() (cancel it when -1) and call advance(now) when it fires.
// Both classes are independent; the overlay owns one of each.

#include <cstdint>

namespace nordtile { namespace player {

enum class TapSide { Left, Right };

// Left/right half of the tap area; x and width in the same units.
inline TapSide tapSideFor(double x, double width) { return x < width / 2 ? TapSide::Left : TapSide::Right; }

struct TapOutput {
    bool toggleControls = false;  // a single tap was confirmed
    bool seek = false;            // flush the burst: ONE seek to seekTargetMs
    std::int64_t seekTargetMs = 0;
    bool gestureEnded = false;    // hide the double-tap feedback
};

// PLY-17 / NFR-PERF-03 ("double-tap burst = 1 flush").
// - A single tap toggles the controls once the 250 ms double-tap window
//   passes without a second tap on the same half.
// - A second tap on the same half within 250 ms starts a seek gesture of
//   -/+ step. A tap on the other half inside the window restarts the window
//   for that half instead (the first tap is dropped, no toggle).
// - During the gesture every tap on either half adds a step at once. Same
//   half: the displayed total grows. Other half: the displayed total restarts
//   from that side's first step, but the target continues from where it is.
// - The target is clamped to [0, duration] (no upper clamp while duration <= 0).
// - The seek is applied 250 ms after the last tap, as one flush. The gesture
//   ends 700 ms after the last tap; taps between the flush and the end keep
//   stacking on the flushed target and flush again 250 ms after their last tap.
class TapGestures {
public:
    static const int kDoubleTapWindowMs = 250;
    static const int kFlushDelayMs = 250;
    static const int kGestureEndMs = 700;
    static const int kDefaultStepSeconds = 10;

    // PLY-17: step is 5, 10 or 15 s (SET-01 default 10). Other values -> 10.
    void setStepSeconds(int seconds);
    int stepSeconds() const { return m_stepSeconds; }

    // A tap on one half. positionMs/durationMs: current playback state.
    // Also processes deadlines that already passed (late timer).
    TapOutput tap(std::int64_t nowMs, TapSide side, std::int64_t positionMs, std::int64_t durationMs);
    // Call when the single-shot timer fires.
    TapOutput advance(std::int64_t nowMs);
    // Earliest pending deadline, -1 when nothing is pending.
    std::int64_t nextDeadlineMs() const;
    // Drop everything (gear opened, video changed). No output.
    void cancel();

    // Running display (PLY-17 "show a running total").
    bool gestureActive() const { return m_gesture; }
    TapSide displaySide() const { return m_side; }
    int displayTotalSeconds() const; // signed: negative = backwards
    std::int64_t targetMs() const { return m_target; }

private:
    std::int64_t clampTarget(std::int64_t ms) const;
    void addStep(TapSide side);

    int m_stepSeconds = kDefaultStepSeconds;
    bool m_pending = false;
    std::int64_t m_pendingAt = 0;
    TapSide m_pendingSide = TapSide::Left;
    bool m_gesture = false;
    bool m_flushDue = false;
    std::int64_t m_lastTapAt = 0;
    std::int64_t m_target = 0;
    std::int64_t m_duration = 0;
    TapSide m_side = TapSide::Left;
    int m_displaySteps = 0;
};

// PLY-17 controls visibility: start visible; auto-hide 3.5 s after showing
// while playing and with the gear menu closed; show again and stay on pause or
// end; any interaction restarts the timer.
class ControlsVisibility {
public:
    static const int kAutoHideMs = 3500;

    void setPlaying(std::int64_t nowMs, bool playing); // pause -> show and stay
    void setEnded();                                   // end -> show and stay
    void setMenuOpen(std::int64_t nowMs, bool open);   // open menu holds the controls
    void interact(std::int64_t nowMs);                 // show + restart the timer
    void toggle(std::int64_t nowMs);                   // confirmed single tap
    // Timer fired. Returns true when the controls just hid; hiding also
    // closes the gear menu (menuOpen() becomes false).
    bool advance(std::int64_t nowMs);

    bool visible() const { return m_visible; }
    bool menuOpen() const { return m_menuOpen; }
    std::int64_t nextDeadlineMs() const { return m_deadline; }

private:
    void arm(std::int64_t nowMs);

    bool m_visible = true;
    bool m_playing = false;
    bool m_menuOpen = false;
    std::int64_t m_deadline = -1;
};

} } // namespace nordtile::player
