// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

// PLY-05 split-seek recovery for Dual mode. A "split" is a dual seek that
// exactly one branch accepted: the branches now play different positions.
//
//   split #1 in an episode        -> ResendAfter250ms (same dual seek)
//   split again                   -> RebuildOnceWithDeferredSeek (stop + play
//                                    with the same URLs; seek via PrerollBarrier)
//   split after the rebuild       -> GiveUpKeepPlaying (do nothing more)
//
// Never loops: at most one rebuild per episode. The allowance resets ONLY on
// onUserSeek(), on a seek both branches accept, or on different sources
// (onSourcesSet with a new key). Teardown, including the one inside the
// rebuild, does not reset it; nor does re-setting the same URLs. Both
// branches refusing is not a desync (nothing moved) -> None. A refused
// audio-align (ISO-BMFF phase two) counts as a split.
//
// No timers inside: the engine arms one single-shot timer of kResendDelayMs
// when it gets ResendAfter250ms, and cancels it on onUserSeek()/onSourcesSet().

#include <cstdint>
#include <string>

namespace nordtile { namespace player {

enum class SplitSeekAction {
    None,                         // nothing to do
    ResendAfter250ms,             // re-send the same dual seek after kResendDelayMs
    RebuildOnceWithDeferredSeek,  // full stop+play, same URLs, seek at preroll
    GiveUpKeepPlaying             // still split after the rebuild: keep playing
};

class SplitSeekGuard {
public:
    static const int64_t kResendDelayMs = 250;

    // A user-initiated seek (scrubber, double-tap burst, chapter). Starts a
    // new episode. Do NOT call it for resends, deferred or align seeks.
    void onUserSeek();

    // The sources about to be played. A key different from the current one
    // (e.g. "videoUrl|audioUrl") is a new URL and resets the allowance; the
    // same key (the rebuild re-using its URLs) does not.
    void onSourcesSet(const std::string &sourceKey);

    // Teardown of the pipeline. Never resets the allowance (it runs inside
    // the rebuild). Provided so the engine can call it unconditionally.
    void onTeardown() {}

    // Result of a dual seek: did gst_element_send_event() on each sink return
    // TRUE? For the two-phase ISO-BMFF seek, a refused video phase (audio not
    // sent) is reported as (false, false): nothing moved.
    SplitSeekAction onSeekResult(bool videoAccepted, bool audioAccepted);

    // Result of the ISO-BMFF audio-align seek (video already accepted).
    SplitSeekAction onAudioAlignResult(bool audioAccepted) { return onSeekResult(true, audioAccepted); }

    bool rebuildUsed() const { return rebuildUsed_; }
    bool gaveUp() const { return gaveUp_; }
    int splitsThisEpisode() const { return splits_; }

private:
    void reset();

    std::string sourceKey_;
    int splits_ = 0;
    bool rebuildUsed_ = false;
    bool gaveUp_ = false;
};

} } // namespace nordtile::player
