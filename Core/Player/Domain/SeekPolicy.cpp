// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "SeekPolicy.h"

namespace nordtile { namespace player {

namespace {

double sanitiseRate(double rate) {
    // NaN fails every comparison, so it lands here too.
    if (!(rate > 0.0)) return 1.0;
    return rate;
}

int64_t clampTarget(int64_t targetMs, int64_t durationMs) {
    if (targetMs < 0) return 0;
    if (durationMs > 0 && targetMs > durationMs) return durationMs;
    return targetMs;
}

SeekCommand command(Branch branch, SeekFlags flags, int64_t ms, double rate) {
    SeekCommand c;
    c.branch = branch;
    c.flags = flags;
    c.positionNs = msToNs(ms);
    c.rate = rate;
    return c;
}

} // namespace

SeekFlags seekFlagsFor(Container container) {
    return container == Container::Matroska ? SeekFlags::flushAccurate() : SeekFlags::flushKeyUnit();
}

SeekPlan planSeek(const SeekRequest &request) {
    SeekPlan plan;
    plan.rate = sanitiseRate(request.rate);
    plan.targetMs = clampTarget(request.targetMs, request.durationMs);

    switch (request.mode) {
    case PlayMode::Dual:
        if (request.video == Container::Matroska) {
            plan.now.push_back(command(Branch::Video, seekFlagsFor(request.video), plan.targetMs, plan.rate));
            plan.now.push_back(command(Branch::Audio, seekFlagsFor(request.audio), plan.targetMs, plan.rate));
        } else {
            // ISO-BMFF (or unknown) video lands on a keyframe before the target;
            // the audio follows it after async-done.
            plan.now.push_back(command(Branch::Video, SeekFlags::flushKeyUnit(), plan.targetMs, plan.rate));
            plan.alignAudioAfterAsyncDone = true;
            plan.audioAlignFlags = seekFlagsFor(request.audio);
        }
        break;
    case PlayMode::AudioOnly:
        plan.now.push_back(command(Branch::Audio, seekFlagsFor(request.audio), plan.targetMs, plan.rate));
        break;
    case PlayMode::Muxed:
    case PlayMode::Hls:
    case PlayMode::Local:
        plan.now.push_back(command(Branch::Muxed, SeekFlags::flushKeyUnit(), plan.targetMs, plan.rate));
        break;
    }
    return plan;
}

SeekCommand planAudioAlign(const SeekPlan &plan, bool landedKnown, int64_t landedMs) {
    int64_t ms = landedKnown ? landedMs : plan.targetMs;
    if (ms < 0) ms = 0;
    return command(Branch::Audio, plan.audioAlignFlags, ms, plan.rate);
}

Container containerHintFor(PlayMode mode, Container container) {
    switch (mode) {
    case PlayMode::Dual:
    case PlayMode::AudioOnly:
        return container;
    case PlayMode::Muxed:
    case PlayMode::Hls:
    case PlayMode::Local:
        return Container::Unknown;
    }
    return Container::Unknown;
}

bool needsDiskBackedBuffer(PlayMode mode, Container container) {
    return containerHintFor(mode, container) == Container::IsoBmff;
}

SourceBufferParams sourceBufferFor(PlayMode mode, Container container) {
    SourceBufferParams p;
    if (mode == PlayMode::Hls || mode == PlayMode::Local) return p;
    if (needsDiskBackedBuffer(mode, container)) {
        p.download = true;
        p.bufferDurationMs = kDiskBufferDurationMs;
        p.bufferSizeBytes = kDiskBufferSizeBytes;
    } else {
        p.bufferDurationMs = kLeanBufferDurationMs;
    }
    return p;
}

} } // namespace nordtile::player
