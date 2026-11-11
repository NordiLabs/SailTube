// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once
// Switches the fast path off when the provider keeps refusing it, so that a refusing network or a
// changed protocol costs two attempts, not every track. State lives in memory only; time is
// passed in, so the policy is testable.
//
// A strike: the chosen address was rejected by the probe or refused at byte 0 while it was still
// valid for at least a minute. Two strikes within ten minutes, or two bot walls in a row, open the
// breaker for thirty minutes. A network failure is never a strike. Any success resets the bot
// counter.
#include <cstdint>
#include <vector>

namespace nordtile { namespace music { namespace sharedplayback { namespace domain {

class CircuitBreaker {
public:
    static const std::int64_t strikeWindowMs = 10 * 60 * 1000;
    static const std::int64_t openForMs = 30 * 60 * 1000;
    static const int strikesToOpen = 2;
    static const int botWallsToOpen = 2;

    bool allows(std::int64_t nowMs) const { return nowMs >= openUntilMs_; }
    std::int64_t openUntilMs() const { return openUntilMs_; }
    int botWalls() const { return botWalls_; }

    void recordStrike(std::int64_t nowMs) {
        pruneStrikes(nowMs);
        strikes_.push_back(nowMs);
        if (int(strikes_.size()) >= strikesToOpen) open(nowMs);
    }
    void recordBotWall(std::int64_t nowMs) {
        if (++botWalls_ >= botWallsToOpen) open(nowMs);
    }
    void recordSuccess() { botWalls_ = 0; }
    void reset() { strikes_.clear(); botWalls_ = 0; openUntilMs_ = 0; }

private:
    void open(std::int64_t nowMs) { openUntilMs_ = nowMs + openForMs; strikes_.clear(); botWalls_ = 0; }
    void pruneStrikes(std::int64_t nowMs) {
        std::vector<std::int64_t> kept;
        for (std::size_t i = 0; i < strikes_.size(); ++i) if (nowMs - strikes_[i] < strikeWindowMs) kept.push_back(strikes_[i]);
        strikes_.swap(kept);
    }
    std::vector<std::int64_t> strikes_;
    int botWalls_ = 0;
    std::int64_t openUntilMs_ = 0;
};

}}}}
