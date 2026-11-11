// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

// PLY-06 preroll barrier (also PLY-07 "speed applied before the first frame"
// and SEL-07 step 3). Seeks and a non-default rate requested before the
// pipeline has prerolled are held here and released at the FIRST pipeline
// async-done as ONE rate-carrying flush-seek. Later async-dones (from flush
// seeks) release nothing until the barrier is re-armed for a new URL or a
// rebuild. No timers are involved.
//
// Start seek before preroll (PLY-06, start-up performance): a non-zero start
// position or a rate != 1 can instead take effect BEFORE the first frame is
// decoded. The engine holds each branch's first compressed buffer at its
// decoder (the demuxer has parsed its headers by then), the caller sends the
// start seek from there (one flush inside the source bin, none at the sinks),
// and the first preroll is already at the start position. When every start
// seek was accepted, onStartSeekApplied() records it and the first
// async-done releases nothing (unless the user changed position or speed in
// the meantime). When one was refused or a branch could not be held, the
// barrier stays as it was and the first async-done releases the classic seek.
//
// Engine use:
//   building a pipeline:  barrier.arm(resumeOrCapturedMs, rememberedRate)
//                         if (barrier.wantsStartSeek() && startSeekBeforePreroll(mode))
//                             hold the decoders, send planSeek() per held branch,
//                             all accepted -> barrier.onStartSeekApplied(sentMs, sentRate)
//   user seek / speed:    if (!barrier.deferSeek(ms)) seek now (SeekPolicy)
//                         if (!barrier.deferRate(r))  rate-seek now at the
//                                                    current position
//   GST_MESSAGE_ASYNC_DONE on the pipeline bus:
//       DeferredSeek d;
//       if (barrier.onAsyncDone(&d)) execute planSeek({mode, ..., d.positionMs,
//                                                     duration, d.rate})
// The released seek is a normal seek: SeekPolicy picks per-sink flags, and in
// Dual mode its result goes to SplitSeekGuard like any other.

#include "PlayerTypes.h"

#include <cstdint>

namespace nordtile { namespace player {

struct DeferredSeek {
    int64_t positionMs = 0;
    double rate = 1.0;
};

class PrerollBarrier {
public:
    // New URL or rebuild: the next async-done is "first" again. A position
    // <= 0 means start; a rate <= 0 or NaN means 1.0.
    void arm(int64_t positionMs, double rate);

    // Before preroll: store (latest wins) and return true. After preroll:
    // return false and change nothing; the caller seeks immediately.
    bool deferSeek(int64_t positionMs);
    bool deferRate(double rate);

    // PLY-06 start seek: true while armed, not prerolled, nothing applied yet
    // and there is something to apply (position > 0 or rate != 1).
    bool wantsStartSeek() const;
    // Every branch accepted the start seek, which carried `positionMs` and
    // `rate`, before the first async-done. That async-done then releases
    // nothing, unless the pending values (deferSeek/deferRate) differ from them.
    void onStartSeekApplied(int64_t positionMs, double rate);
    bool startSeekApplied() const { return applied_; }

    // Pipeline async-done. Returns true exactly once per arm(), on the first
    // async-done, when there is something to apply (position > 0 or
    // rate != 1, and not already applied by the start seek); fills *out.
    // Returns false on later async-dones, when nothing is pending, or before
    // arm().
    bool onAsyncDone(DeferredSeek *out);

    bool armed() const { return armed_; }
    bool prerolled() const { return prerolled_; }
    // The position the barrier will apply (meaningful while !prerolled());
    // SwitchPlan uses it to capture a position mid-preroll.
    int64_t pendingPositionMs() const { return positionMs_; }
    double pendingRate() const { return rate_; }

private:
    bool armed_ = false;
    bool prerolled_ = false;
    bool applied_ = false;
    int64_t positionMs_ = 0;
    double rate_ = 1.0;
    int64_t appliedPositionMs_ = 0;
    double appliedRate_ = 1.0;
};

// PLY-06: may the start seek be sent before preroll in this mode? Dual,
// Muxed, Local and AudioOnly: yes (qtdemux / matroskademux behind a
// uridecodebin take a seek once they pushed their first buffer). Hls: no
// (the adaptive demuxer sits above the held decoder; the classic barrier
// seek at the first async-done applies).
bool startSeekBeforePreroll(PlayMode mode);

} } // namespace nordtile::player
