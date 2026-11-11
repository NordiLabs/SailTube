// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

// PLY-08 buffering. Owns the play/pause decision so a buffering pause and a
// user pause never fight. Feed it GST_MESSAGE_BUFFERING percentages per
// network branch and the user's play/pause intent; execute the action.
//
// VOD policy (Dual, Muxed, Hls VOD, AudioOnly over the network):
//   enter buffering when any branch reports < 100 %: pause the pipeline and
//   show the indicator; leave when every branch is back at 100 %, resuming
//   only if the user intent is "playing".
// Live HLS low-latency policy (pausing a live stream drifts it away from the
//   edge, so pause late and resume early):
//   enter only when a branch drops below kLiveEnterPercent (10 %); leave when
//   every branch reaches kLiveResumePercent (60 %). Between those, keep
//   playing without an indicator.
// Local files (network == false) never buffer: messages are ignored.
//
// Start phase (opt-in, beginStartPhase() after reset; start-up performance):
//   the first preroll never waits for a buffer level, and a branch's first
//   fill never pauses playback. A branch that has not yet reached its leave
//   threshold once (100 %, live 60 %) is ignored; once it has, the normal
//   rules above apply to it (an underrun pauses, resume at 100 %). Why not a
//   "ran dry" rule during the first fill: uridecodebin's queue2 reports its
//   own level, which stays near 0 % while decodebin's multiqueue drains it
//   (host run: 0-1 % on a 1.5x link while playback ran smoothly).
//
// A user pause during buffering is respected: no resume at 100 %. The
// indicator shows only while buffering AND the user wants to play (a paused
// player shows its paused state instead). A user play during buffering is
// remembered and applied when buffering ends.

#include "PlayerTypes.h"

namespace nordtile { namespace player {

struct BufferingAction {
    bool pausePipeline = false;   // set the pipeline to PAUSED now
    bool resumePipeline = false;  // set the pipeline to PLAYING now
    bool showIndicator = false;   // current indicator state (level, not edge)
};

class BufferingPolicy {
public:
    static const int kLiveEnterPercent = 10;
    static const int kLiveResumePercent = 60;

    // New source / rebuild. `network`: the source is fetched over the
    // network; `liveHls`: Hls mode on a live stream. Clears all levels and
    // keeps the user intent.
    void reset(bool network, bool liveHls);

    // Start-up performance (PLY-08 start phase, see above): call after reset()
    // for a new build.
    void beginStartPhase();
    bool inStartPhase() const { return startPhase_; }

    // GST_MESSAGE_BUFFERING from `branch` (percent 0..100, clamped).
    BufferingAction onBufferingPercent(Branch branch, int percent);

    // User pressed play (true) or pause (false). Play while buffering only
    // records the intent; play otherwise resumes; pause always pauses.
    BufferingAction onUserIntent(bool wantsPlaying);

    bool buffering() const { return buffering_; }
    bool wantsPlaying() const { return wantsPlaying_; }
    BufferingAction current() const;

private:
    static int slot(Branch b) { return b == Branch::Video ? 0 : b == Branch::Audio ? 1 : 2; }

    bool network_ = true;
    bool live_ = false;
    bool buffering_ = false;
    bool wantsPlaying_ = false;
    int percent_[3] = {100, 100, 100};
    bool startPhase_ = false;     // beginStartPhase() .. reset()
    bool filled_[3] = {true, true, true};  // normal rules apply (the branch filled once)
};

} } // namespace nordtile::player
