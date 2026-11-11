// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

// Seek planning per mode and container (PLY-03), the byte-seekable upstream
// rule for ISO-BMFF (PLY-04) and the sink sync model (PLY-02). Pure functions.
//
// Engine contract (GStreamer):
//   * Not prerolled yet? Do not plan; store the target in PrerollBarrier
//     (PLY-06) and plan at the first async-done.
//   * planSeek() -> for every SeekCommand in `now`, send
//       gst_event_new_seek(rate, GST_FORMAT_TIME, flags, GST_SEEK_TYPE_SET,
//                          positionNs, GST_SEEK_TYPE_NONE, -1)
//     with gst_element_send_event() to THAT branch's sink (Video/Audio), or
//     to the pipeline for Branch::Muxed. A pipeline-level seek in Dual mode
//     reaches only one branch, so never do that.
//   * Report each command's accepted/refused result to SplitSeekGuard
//     (PLY-05) and the target to PositionPolicy::onSeekRequested (PLY-24).
//   * If plan.alignAudioAfterAsyncDone: on the next pipeline async-done,
//     query the VIDEO sink position (the landed keyframe) and send
//     planAudioAlign(plan, ok, landedMs) to the audio sink. The landed
//     position is what the scrubber shows (PositionPolicy::onSeekLanded).
//     Otherwise query the position after async-done and report it as landed.

#include "PlayerTypes.h"

#include <cstdint>
#include <vector>

namespace nordtile { namespace player {

struct SeekCommand {
    Branch branch = Branch::Muxed;  // which sink receives the event
    SeekFlags flags;
    int64_t positionNs = 0;          // GST_SEEK_TYPE_SET start, nanoseconds
    double rate = 1.0;               // the current playback rate, always set
};

struct SeekRequest {
    PlayMode mode = PlayMode::Dual;
    Container video = Container::Unknown;  // container of the video branch (Dual)
    Container audio = Container::Unknown;  // container of the audio branch (Dual, AudioOnly)
    int64_t targetMs = 0;
    int64_t durationMs = 0;                // 0 = unknown; known -> target clamped to it
    double rate = 1.0;                     // current rate; <= 0 or NaN -> 1.0
};

struct SeekPlan {
    std::vector<SeekCommand> now;          // send immediately, one per sink
    // ISO-BMFF video in Dual mode: audio is not seeked now; after async-done
    // it is aligned to where the video landed (planAudioAlign).
    bool alignAudioAfterAsyncDone = false;
    SeekFlags audioAlignFlags;             // flags for that later audio seek
    int64_t targetMs = 0;                  // clamped target, shown until landed
    double rate = 1.0;                     // sanitised rate used by every command
};

// PLY-03: plan one user/deferred seek.
//   Matroska branch: FLUSH|ACCURATE.
//   ISO-BMFF branch: FLUSH|KEY_UNIT, never ACCURATE (push-mode qtdemux).
//   Unknown container: treated like ISO-BMFF (the safe flags; accurate on
//     push-mode mp4 costs 10-50 s, key-unit costs at most one GOP).
//   Dual: one command per sink; ISO-BMFF/Unknown video -> video only now and
//     audio aligned after async-done.
//   Muxed / Hls / Local: one FLUSH|KEY_UNIT command to the pipeline.
//   AudioOnly: one command to the audio sink with the audio container flags.
SeekPlan planSeek(const SeekRequest &request);

// PLY-03 phase two: the audio seek to the video's landed position. When the
// position query failed (landedKnown == false) the plan target is used (at
// most one GOP of error). Only meaningful if plan.alignAudioAfterAsyncDone.
SeekCommand planAudioAlign(const SeekPlan &plan, bool landedKnown, int64_t landedMs);

// Flags for a branch of the given container (PLY-03 table above).
SeekFlags seekFlagsFor(Container container);

// PLY-04: uridecodebin buffering parameters for one source.
struct SourceBufferParams {
    bool download = false;        // uridecodebin "download" (disk-backed downloadbuffer)
    int64_t bufferDurationMs = 0; // "buffer-duration"; 0 = leave the element default
    int64_t bufferSizeBytes = 0;  // "buffer-size";     0 = leave the element default
};

// Container hint the engine must set on a source BEFORE its URL (PLY-04).
// Muxed mode clears hints (returns Unknown): a stale "mp4" hint gave the
// single source a wrong disk buffer. Hls and Local carry no hint either.
Container containerHintFor(PlayMode mode, Container container);

// PLY-04: does this source need the disk-backed (byte-seekable) buffer?
// True for an ISO-BMFF network source in Dual or AudioOnly mode.
bool needsDiskBackedBuffer(PlayMode mode, Container container);

// PLY-04: parameters for one network source. Disk-backed ISO-BMFF branch:
// download=true, 1 s, 1.5 MiB (one ~1.5 MB refill per seek). Lean branch
// (Matroska/Unknown in Dual/AudioOnly, and the Muxed source): 3 s of
// buffer-duration, no disk buffer. Hls / Local: defaults (all zero).
SourceBufferParams sourceBufferFor(PlayMode mode, Container container);

const int64_t kDiskBufferDurationMs = 1000;
const int64_t kDiskBufferSizeBytes = 1536 * 1024;  // 1.5 MiB
const int64_t kLeanBufferDurationMs = 3000;

// PLY-02: sync model. The audio sink provides the pipeline clock; the video
// sink syncs with QoS and a 2-buffer leaky queue (late frames are dropped,
// never drifted). For appsink: sync, qos, max-buffers, drop.
struct VideoSinkSync {
    bool sync = true;
    bool qos = true;
    int maxBuffers = 2;
    bool drop = true;
};
inline VideoSinkSync videoSinkSync() { return VideoSinkSync(); }

// PLY-02 / PLY-03 acceptance budget: |video - audio| position offset.
const int64_t kMaxAvOffsetMs = 80;
inline bool avOffsetWithinBudget(int64_t videoMs, int64_t audioMs) {
    int64_t d = videoMs - audioMs;
    return (d < 0 ? -d : d) <= kMaxAvOffsetMs;
}

} } // namespace nordtile::player
