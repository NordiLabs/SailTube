// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "StallPolicy.h"

namespace nordtile { namespace player {

void StallPolicy::closeEpisode() {
    episode_ = false;
    step_ = StallStep::None;
}

void StallPolicy::suspend() {
    armed_ = false;
    reloadArmed_ = false;
    seekHeld_ = false;
    deadline_ = -1;
}

void StallPolicy::onInternalSeek() {
    armed_ = false;
    seekHeld_ = false;  // a new seek: its own wait
}

// A seek in flight is waited for, for at most noProgressMs.
bool StallPolicy::seekHold(std::int64_t nowMs) {
    if (!seekHeld_) {
        seekHeld_ = true;
        seekSinceMs_ = nowMs;
    }
    return nowMs - seekSinceMs_ < config_.noProgressMs;
}

void StallPolicy::onUserSeek() {
    episodes_.clear();
    closeEpisode();
    suspend();
}

void StallPolicy::reset() {
    onUserSeek();
    frozen_ = false;
    frozenProven_ = false;
    seekable_ = true;
    resolvable_ = true;
    lastPositionMs_ = 0;
    lastBufferedMs_ = 0;
    lastPercent_ = 100;
}

std::int64_t StallPolicy::window(bool buffering) const {
    if (episode_) return config_.stepGraceMs;
    return buffering ? config_.starvedMs : config_.noProgressMs;
}

void StallPolicy::arm(std::int64_t nowMs, const StallFacts &f) {
    armed_ = true;
    sinceMs_ = nowMs;
    lastPositionMs_ = f.positionMs;
    lastBufferedMs_ = f.bufferedMs;
    lastPercent_ = f.bufferingPercent;
    if (episode_) recoverBaseMs_ = f.positionMs;
    windowMs_ = window(f.buffering);
    deadline_ = sinceMs_ + windowMs_;
}

StallDecision StallPolicy::observe(std::int64_t nowMs, const StallFacts &f) {
    StallDecision d;
    // A check long after its deadline (the process was stopped, the device slept) says nothing
    // about the time in between: count again from here instead of calling it a stall.
    const bool late = deadline_ >= 0 && nowMs > deadline_ + config_.lateSlackMs;
    seekable_ = f.seekable;
    resolvable_ = f.resolvable;
    const bool reloading = episode_ && f.reloading && !f.started;
    if (!f.wantsPlay || f.ended || (!f.started && !reloading)) {
        suspend();
        return d;
    }
    if (reloading) {  // a step's reload on its way to the preroll: only the time counts
        armed_ = false;
        if (!reloadArmed_ || late) {
            reloadArmed_ = true;
            sinceMs_ = nowMs;
        }
        if (nowMs - sinceMs_ >= config_.reloadGraceMs) return nextStep(nowMs, d);
        deadline_ = sinceMs_ + config_.reloadGraceMs;
        return d;
    }
    reloadArmed_ = false;
    if (!f.seeking) seekHeld_ = false;
    if (f.liveEdgeWaiting || (f.seeking && seekHold(nowMs))) {
        // Their end is an event the caller observes; this deadline is only the net below it.
        armed_ = false;
        deadline_ = nowMs + config_.noProgressMs;
        return d;
    }
    if (f.videoFrozen != frozen_) {  // PLY-09: judge the frozen plan by what it does from here on
        frozen_ = f.videoFrozen;
        frozenProven_ = false;
        armed_ = false;
    }
    if (!armed_) {
        arm(nowMs, f);
        return d;
    }
    const bool advanced = f.positionMs > lastPositionMs_;
    const bool jumpedBack = f.positionMs < lastPositionMs_;
    // Buffering with progress: the level rose or bytes arrived since the last look.
    const bool filled = f.buffering && (f.bufferingPercent > lastPercent_ || f.bufferedMs > lastBufferedMs_);
    lastPositionMs_ = f.positionMs;
    lastBufferedMs_ = f.bufferedMs;
    lastPercent_ = f.bufferingPercent;
    if (advanced && frozen_) frozenProven_ = true;
    if (jumpedBack && episode_) recoverBaseMs_ = f.positionMs;
    if (episode_ && advanced && f.positionMs - recoverBaseMs_ >= config_.recoveredAdvanceMs) return recovered(nowMs, d);
    // A frozen picture whose position never advanced since the freeze has no audio clock to judge by.
    if (advanced || jumpedBack || filled || late || (frozen_ && !frozenProven_)) sinceMs_ = nowMs;
    windowMs_ = window(f.buffering);
    if (nowMs - sinceMs_ < windowMs_) {
        deadline_ = sinceMs_ + windowMs_;
        return d;
    }
    if (!episode_) {
        d.detected = true;
        d.reason = f.buffering ? "starved" : "frozen";
        d.ms = nowMs - sinceMs_;
        ++stalls_;
        std::size_t kept = 0;
        for (std::size_t i = 0; i < episodes_.size(); ++i)
            if (nowMs - episodes_[i] < config_.budgetWindowMs) episodes_[kept++] = episodes_[i];
        episodes_.resize(kept);
        if (static_cast<int>(episodes_.size()) >= config_.maxEpisodes) return failed(d, "budget");
        episodes_.push_back(nowMs);
        episode_ = true;
        step_ = StallStep::None;
        episodeStartMs_ = nowMs;
    }
    return nextStep(nowMs, d);
}

StallDecision StallPolicy::onPosition(std::int64_t nowMs, std::int64_t positionMs) {
    StallDecision d;
    if (!armed_ || positionMs == lastPositionMs_) return d;
    const bool advanced = positionMs > lastPositionMs_;
    lastPositionMs_ = positionMs;
    if (advanced && frozen_) frozenProven_ = true;
    if (!advanced && episode_) recoverBaseMs_ = positionMs;
    if (episode_ && advanced && positionMs - recoverBaseMs_ >= config_.recoveredAdvanceMs) return recovered(nowMs, d);
    sinceMs_ = nowMs;
    deadline_ = sinceMs_ + windowMs_;
    return d;
}

StallDecision StallPolicy::onStepFailed(std::int64_t nowMs) {
    StallDecision d;
    if (!episode_) return d;
    return nextStep(nowMs, d);
}

StallDecision StallPolicy::nextStep(std::int64_t nowMs, StallDecision d) {
    StallStep next = StallStep::None;
    if (step_ == StallStep::None && seekable_) next = StallStep::Seek;
    else if (step_ == StallStep::None || step_ == StallStep::Seek) next = StallStep::Reopen;
    else if (step_ == StallStep::Reopen && resolvable_) next = StallStep::Reresolve;
    if (next == StallStep::None) return failed(d, "exhausted");
    step_ = next;
    lastStep_ = static_cast<int>(next);
    armed_ = false;
    reloadArmed_ = false;
    // The caller observes again right after running the step; this is only the net below that.
    deadline_ = nowMs + config_.stepGraceMs;
    d.kind = StallDecision::Recover;
    d.step = next;
    return d;
}

StallDecision StallPolicy::recovered(std::int64_t nowMs, StallDecision d) {
    d.kind = StallDecision::Recovered;
    d.step = step_;
    d.ms = nowMs - episodeStartMs_;
    ++recoveries_;
    closeEpisode();
    sinceMs_ = nowMs;
    windowMs_ = config_.noProgressMs;
    deadline_ = sinceMs_ + windowMs_;
    return d;
}

StallDecision StallPolicy::failed(StallDecision d, const char *why) {
    d.kind = StallDecision::Fail;
    d.failure = why;
    d.step = step_;
    closeEpisode();
    suspend();
    return d;
}

} } // namespace nordtile::player
