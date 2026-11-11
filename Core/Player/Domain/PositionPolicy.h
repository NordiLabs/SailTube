// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

// PLY-24 position and duration reporting (background: FinTube spec 4.12).
//
//   * Poll interval (pollIntervalMs): 250 ms while the controls are visible,
//     1000 ms while hidden (never faster than 1 Hz, NFR-PERF-06), 0 = stop
//     the timer when nothing consumes the position. While paused the
//     position does not move: 0 once the duration is known.
//   * After a seek the target is reported straight away; position queries
//     are ignored until the landed position arrives (or the seek failed).
//   * At end of stream the position is unreliable: report the duration (or
//     the metadata duration, or the last good position) and ignore queries
//     until a new seek or source.
//   * The duration is re-polled until known (moov not parsed yet).
//
// The engine runs ONE timer at pollIntervalMs(); on each tick it queries
// position (and duration while needsDurationPoll()) and feeds the results.

#include <cstdint>

namespace nordtile { namespace player {

class PositionPolicy {
public:
    static const int64_t kVisibleIntervalMs = 250;
    static const int64_t kHiddenIntervalMs = 1000;

    // PLY-24 update interval. hasConsumer: a page, the cover or MPRIS shows
    // the position. Returns 0 for "no timer".
    int64_t pollIntervalMs(bool hasConsumer, bool controlsVisible, bool playing) const;

    // New source or rebuild: forget everything except the metadata duration
    // when `keepMetadataDuration`.
    void reset(bool keepMetadataDuration = false);

    // Duration from the video's metadata (fallback for EOS); 0 = unknown.
    void setMetadataDurationMs(int64_t ms);

    void onSeekRequested(int64_t targetMs);       // report the target now
    void onSeekLanded(bool known, int64_t landedMs); // after async-done; !known -> keep target, queries resume
    void onSeekRefused();                          // nothing moved; queries resume
    void onPositionQuery(bool ok, int64_t positionMs);
    void onDurationQuery(bool ok, int64_t durationMs);
    void onEos();

    int64_t positionMs() const;     // what the scrubber shows
    int64_t durationMs() const;     // best known duration (pipeline, else metadata), 0 = unknown
    bool durationKnown() const { return durationMs_ > 0; }
    bool needsDurationPoll() const { return durationMs_ <= 0; }
    bool seeking() const { return seeking_; }
    bool ended() const { return ended_; }

private:
    int64_t clamp(int64_t ms) const;

    int64_t positionMs_ = 0;
    int64_t durationMs_ = 0;
    int64_t metadataDurationMs_ = 0;
    bool seeking_ = false;
    bool ended_ = false;
};

} } // namespace nordtile::player
