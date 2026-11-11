// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

// Stall watchdog after the first frame (the half PLY-18 lacks: its recovery starts from an error,
// and a stream that just stops delivering raises none). Time-injected state machine
// (milliseconds from any monotonic clock; never read here), Qt-free.
//
// Detect: while the session wants to play and its load has prerolled, the playing position has
//   not advanced for noProgressMs (8 s), or for starvedMs (4 s) while the player is buffering and
//   its buffer makes no progress either (the level does not rise and no bytes arrive).
//   Not a stall: paused (by the user or an audio loss), not started yet (start, switch, resolve),
//   ended, a seek in flight (for at most noProgressMs: the player bounds a seek's landing itself,
//   one that still shows after that is as stuck as a standing position), a live stream waiting at
//   its edge, buffering that makes progress (a slow link is not a dead one), and the PLY-09
//   background freeze: the picture stands still there while the audio position advances. A
//   frozen plan whose position was never seen advancing since the freeze (no audio clock to judge
//   by) is left alone.
// Recover, one step at a time, each at most once per episode:
//   Seek       a seek to the current position on the same streams (a stuck sink, a lost segment)
//   Reopen     the same candidate through new stream jobs, reloaded at the position
//   Reresolve  one fresh resolve, reloaded at the position
//   then Fail: the caller shows a retryable error.
//   A step that cannot run is skipped (Seek when not seekable, Reresolve for a local file).
//   After a step the position must advance recoveredAdvanceMs (0.5 s) within stepGraceMs (5 s) of
//   playing again (buffer progress extends the wait), and a step's reload must preroll within
//   reloadGraceMs (12 s); otherwise the next step runs.
// Budget: maxEpisodes (3) episodes per budgetWindowMs (10 min); the next stall fails at once. A
//   user seek, Retry and a new video clear it.
//
// Driving it: observe(now, facts) with fresh facts at nextDeadlineMs() and after every event that
// may have changed a guard; onPosition(now, ms) for every position report in between (the
// position poll); suspend() when nothing can be watched and no time is at hand. There is one
// deadline and no ticker: while the position keeps advancing the deadline keeps moving and never
// fires (NFR-PERF-06).

#include <cstdint>
#include <vector>

namespace nordtile { namespace player {

struct StallConfig {
    std::int64_t noProgressMs = 8000;       // the position stands still this long: a stall
    std::int64_t starvedMs = 4000;          // ... or this long while buffering without progress
    std::int64_t stepGraceMs = 5000;        // after a step: the position advances within this, else the next step
    std::int64_t reloadGraceMs = 12000;     // a step's reload prerolls within this, else the next step
    std::int64_t recoveredAdvanceMs = 500;  // playback advanced this far after a step: recovered
    std::int64_t lateSlackMs = 2000;        // a check this late (process stopped, device asleep) counts again from then
    int maxEpisodes = 3;                    // per budgetWindowMs
    std::int64_t budgetWindowMs = 600000;   // 10 min
};

// What the session and the player say right now.
struct StallFacts {
    // Guards.
    bool wantsPlay = false;        // the session wants to play: no user pause, the audio resource is held
    bool started = false;          // the current load prerolled and is not being replaced
    bool reloading = false;        // a recovery step's reload was issued and has not prerolled yet
    bool seeking = false;          // a seek is in flight (the reported position is its target)
    bool ended = false;            // end of stream
    bool liveEdgeWaiting = false;  // live: the player waits at the edge for the next segment
    bool videoFrozen = false;      // PLY-09: the picture is frozen in the background
    // Which steps can run.
    bool seekable = true;
    bool resolvable = true;        // false for a local file
    // Progress.
    std::int64_t positionMs = 0;   // the furthest position a sink reached
    bool buffering = false;        // PLY-08: playback is held for data
    int bufferingPercent = 100;
    std::int64_t bufferedMs = 0;   // how far the media is buffered (0 unknown); rises while bytes arrive
};

enum class StallStep { None = 0, Seek = 1, Reopen = 2, Reresolve = 3 };

struct StallDecision {
    enum Kind { None, Recover, Recovered, Fail };
    Kind kind = None;
    // Recover: run this step now. Recovered: the step that did it. Fail: the last step tried.
    StallStep step = StallStep::None;
    bool detected = false;      // this decision opened a new episode (Recover, or Fail by the budget)
    const char *reason = "";    // detected: "frozen" (no buffering reported) or "starved"
    const char *failure = "";   // Fail: "budget" or "exhausted"
    std::int64_t ms = 0;        // detected: time without progress; Recovered: detection -> recovered
};

class StallPolicy {
public:
    // Host tests shorten the thresholds; applies from the next observation.
    void setConfig(const StallConfig &config) { config_ = config; }
    const StallConfig &config() const { return config_; }

    // New video: no episode, a full budget. The totals (stalls(), recoveries(), lastStep()) stay.
    void reset();
    // The user sought, or pressed Retry: a full budget; an open episode is dropped (the next
    // observation starts a new window).
    void onUserSeek();
    // A jump the session made itself (SponsorBlock skip, loop restart): it is not progress; the
    // next observation takes a new baseline.
    void onInternalSeek();
    // Nothing can be watched right now (paused, media released, failed) and no time is at hand.
    // An open episode stays open: it goes on when the session is watched again.
    void suspend();

    // Fresh facts: at nextDeadlineMs(), and after any event that may have changed a guard.
    StallDecision observe(std::int64_t nowMs, const StallFacts &facts);
    // A position report between observations (the position poll). Only ever None or Recovered.
    StallDecision onPosition(std::int64_t nowMs, std::int64_t positionMs);
    // The step's reload failed before its preroll: the next step, or Fail.
    StallDecision onStepFailed(std::int64_t nowMs);

    // When to observe() again if nothing else happens; -1 = not watching.
    std::int64_t nextDeadlineMs() const { return deadline_; }

    bool armed() const { return armed_; }          // a baseline is held: onPosition() counts
    bool recovering() const { return episode_; }   // an episode is open
    StallStep step() const { return step_; }       // its last step (None: no episode)
    int episodesInBudget() const { return static_cast<int>(episodes_.size()); }

    // Diagnostics, totals of the session.
    int stalls() const { return stalls_; }          // episodes detected
    int recoveries() const { return recoveries_; }  // episodes that ended with playback advancing
    int lastStep() const { return lastStep_; }      // the last step run: 0 none, 1 seek, 2 reopen, 3 re-resolve

private:
    void arm(std::int64_t nowMs, const StallFacts &facts);
    bool seekHold(std::int64_t nowMs);
    std::int64_t window(bool buffering) const;
    void closeEpisode();
    StallDecision nextStep(std::int64_t nowMs, StallDecision decision);
    StallDecision recovered(std::int64_t nowMs, StallDecision decision);
    StallDecision failed(StallDecision decision, const char *why);

    StallConfig config_;
    bool armed_ = false;
    bool reloadArmed_ = false;            // the reload window of a step is running
    std::int64_t deadline_ = -1;
    std::int64_t sinceMs_ = 0;            // start of the current window: the last progress
    std::int64_t windowMs_ = 0;           // its length
    std::int64_t lastPositionMs_ = 0;
    std::int64_t lastBufferedMs_ = 0;
    int lastPercent_ = 100;
    bool frozen_ = false;
    bool frozenProven_ = false;           // the position advanced since the freeze began
    bool seekHeld_ = false;               // a seek in flight is being waited for ...
    std::int64_t seekSinceMs_ = 0;        // ... since then
    bool seekable_ = true;
    bool resolvable_ = true;

    bool episode_ = false;
    StallStep step_ = StallStep::None;
    std::int64_t episodeStartMs_ = 0;
    std::int64_t recoverBaseMs_ = 0;      // the position after the step; recovered = advanced past it
    std::vector<std::int64_t> episodes_;  // start times inside the budget window

    int stalls_ = 0;
    int recoveries_ = 0;
    int lastStep_ = 0;
};

} } // namespace nordtile::player
