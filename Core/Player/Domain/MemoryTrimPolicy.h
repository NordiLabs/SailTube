// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

// Memory: glibc keeps freed memory in its per-thread arenas (every opened video builds a GStreamer
// pipeline with ~10 threads), so the resident size of the main process only grows. Two measures,
// both switched here:
//   * kMemoryTrimEnabled      one switch for all (false = the old behaviour, no allocator calls);
//   * kTrimAfterTeardownEnabled  the teardown trim only (the arena cap and the idle trim stay on);
//   * kMallocArenaMax         mallopt(M_ARENA_MAX) at the top of main(), before any thread exists
//                             (an explicit MALLOC_ARENA_MAX in the environment wins);
//   * the trim policy below   malloc_trim(0) once the pipeline is gone and nothing is playing.
// Header-only, Qt-free. Pure: the owner thread feeds it events and asks "trim now?"; the timers
// live in the caller (Clients/Sailfish/Presentation/MemoryTrimScheduler). It never answers yes
// while a pipeline is loading or playing, so a trim cannot contend with frame delivery.

namespace nordtile { namespace player {

const bool kMemoryTrimEnabled = true;
// The teardown trim is off: a phone run showed opens right after a teardown trim with a median first
// frame of 7.7 s vs 1.8 s (confounded), so its cost on the owner thread is unproven. Flip to true
// once the "malloc-trim ... ms=" log lines show it is cheap (see docs/PERF_PLAYBACK.md).
const bool kTrimAfterTeardownEnabled = false;
const int kMallocArenaMax = 2;
const int kTrimSettleMs = 1500;   // after "teardown-done": let the dying pipeline's threads finish
const int kTrimIdleMs = 15000;    // after the app has nothing to play

class MemoryTrimPolicy {
public:
    explicit MemoryTrimPolicy(bool teardownTrimEnabled = kTrimAfterTeardownEnabled)
        : teardownTrimEnabled_(teardownTrimEnabled) {}
    bool teardownTrimEnabled() const { return kMemoryTrimEnabled && teardownTrimEnabled_; }
    // busy: a pipeline is loading or playing (paused, ended, failed and idle are not busy).
    void setBusy(bool busy) {
        busy_ = busy;
        if (busy) trimmedSinceBusy_ = false;
    }
    bool busy() const { return busy_; }
    // The old pipeline reached NULL and was freed.
    void teardownDone() { teardownPending_ = teardownTrimEnabled(); }
    bool teardownPending() const { return teardownPending_; }
    // The idle timer is worth (re)starting.
    bool idleTimerWanted() const { return kMemoryTrimEnabled && !busy_ && !trimmedSinceBusy_; }
    // The settle timer fired. True at most once per teardown, never while busy.
    bool shouldTrimAfterTeardown() {
        if (!teardownTrimEnabled() || !teardownPending_) return false;
        teardownPending_ = false;
        if (busy_) return false;  // the next video is starting; the idle timer will cover it
        trimmedSinceBusy_ = true;
        return true;
    }
    // The idle timer fired. True once per idle period, never while busy.
    bool shouldTrimIdle() {
        if (!kMemoryTrimEnabled || busy_ || trimmedSinceBusy_) return false;
        trimmedSinceBusy_ = true;
        return true;
    }

private:
    bool teardownTrimEnabled_;
    bool busy_ = false;
    bool teardownPending_ = false;
    bool trimmedSinceBusy_ = false;
};

} } // namespace nordtile::player
