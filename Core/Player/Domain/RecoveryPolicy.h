// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

// PLY-18 recovery UX as a time-injected state machine (milliseconds from any
// monotonic clock; never read here).
//
//   fault (foreground, tries left) -> Waiting (1.5 s backoff) -> onTimer ->
//   Recover (tear down, re-resolve, resume) -> Recovering -> playback progress
//   -> Idle; 5 s of clean playback past the recovery point restores the budget.
//   fault with no tries left -> Failed: show reason() and a Retry button;
//   retry() resets the budget and recovers at once.
//   fault in the background -> Deferred, no budget spent; on foreground the
//   budget is fresh and recovery starts at once.
//
// The caller re-checks ownership (SessionArbiter::mayAct) before executing a
// Recover action, because the page may have lost the player during the backoff.

#include <cstdint>
#include <string>

namespace nordtile { namespace player {

enum class RecoveryState { Idle, Waiting, Recovering, Deferred, Failed };

struct RecoveryAction {
    enum Kind { None, Wait, Recover, Fail };
    Kind kind = None;
    std::int64_t deadlineMs = -1;      // Wait: when to call onTimer()
    std::int64_t resumePositionMs = 0; // Recover: where to resume (VOD)
    bool resumeAtLive = false;         // Recover: live stream, resume at the live edge
};

class RecoveryPolicy {
public:
    static const int kMaxTries = 3;          // PLY-18: up to 3 automatic recoveries
    static const int kBackoffMs = 1500;      // PLY-18: 1.5 s backoff
    static const int kCleanPlaybackMs = 5000; // PLY-18: budget restored after 5 s clean

    // New video: Idle, full budget.
    void reset();

    // PLY-18: playback failed. positionMs is the current (or pending resume)
    // position; live marks a live stream (resume at the live position).
    // A fault while a recovery is already Waiting/Recovering keeps the
    // position captured by the first fault (the engine reports 0 meanwhile).
    RecoveryAction onFault(std::int64_t nowMs, std::int64_t positionMs, bool live, const std::string &reason);

    // Call at nextDeadlineMs(). Waiting -> Recover once the backoff elapsed.
    RecoveryAction onTimer(std::int64_t nowMs);

    // PLY-18: report every position update while playing. The first update
    // after a recovery marks the recovery point; reaching recovery point + 5 s
    // of media time restores the full budget (pauses and stalls do not count).
    void onPlaybackProgress(std::int64_t positionMs);

    // PLY-18: background defers recovery without spending budget; returning
    // to the foreground recovers a Deferred fault at once with a fresh budget.
    RecoveryAction setForeground(bool foreground, std::int64_t nowMs);

    // PLY-18: the Retry button. Failed -> Recover with a reset budget.
    RecoveryAction retry(std::int64_t nowMs);

    std::int64_t nextDeadlineMs() const { return m_state == RecoveryState::Waiting ? m_deadline : -1; }
    RecoveryState state() const { return m_state; }
    int triesUsed() const { return m_tries; }
    int triesLeft() const { return kMaxTries - m_tries; }
    bool retryAvailable() const { return m_state == RecoveryState::Failed; } // show the Retry button
    const std::string &reason() const { return m_reason; } // shown with the Retry button
    bool foreground() const { return m_foreground; }

private:
    RecoveryAction recoverNow();

    RecoveryState m_state = RecoveryState::Idle;
    int m_tries = 0;
    std::int64_t m_deadline = -1;
    std::int64_t m_resumeMs = 0;
    bool m_live = false;
    bool m_foreground = true;
    bool m_haveRecoveryPoint = false;
    std::int64_t m_recoveryPointMs = 0;
    std::string m_reason;
};

} } // namespace nordtile::player
