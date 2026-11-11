// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

// SEL-07 quality / dub switch that keeps the position. The engine executes
// plan.steps in order:
//   CapturePosition    position = captureSwitchPosition(...)  (stop resets it)
//   Stop               pipeline to NULL (FreezePolicy unlock first, PLY-09)
//   SetContainerHints  plan.videoHint / plan.audioHint, BEFORE the URLs (PLY-04)
//   SetUrls            new source URLs (SplitSeekGuard::onSourcesSet)
//   ArmPrerollBarrier  barrier.arm(plan.resumeMs, plan.rate)  (PLY-06)
//   Play               pipeline to PLAYING; the barrier seeks at async-done
//   ReleaseOldJobs     release plan.releaseJobs, keep plan.keepJobs
// Format ids are opaque strings here (e.g. yt-dlp format ids); a "job" is
// whatever fetches one format (proxy / range job).

#include "PlayerTypes.h"

#include <cstdint>
#include <string>
#include <vector>

namespace nordtile { namespace player {

enum class SwitchStep {
    CapturePosition,
    Stop,
    SetContainerHints,
    SetUrls,
    ArmPrerollBarrier,
    Play,
    ReleaseOldJobs
};

struct SwitchRequest {
    PlayMode mode = PlayMode::Dual;        // mode of the NEW selection
    Container videoContainer = Container::Unknown;
    Container audioContainer = Container::Unknown;
    int64_t capturedPositionMs = 0;        // from captureSwitchPosition()
    double rate = 1.0;                     // current rate (kept across the switch)
    std::vector<std::string> oldJobFormatIds;  // formats with live jobs now
    std::string newVideoFormatId;          // empty when the new mode has none
    std::string newAudioFormatId;          // empty in Muxed/Hls/Local
};

struct SwitchPlan {
    std::vector<SwitchStep> steps;
    int64_t resumeMs = 0;
    double rate = 1.0;
    Container videoHint = Container::Unknown;  // cleared outside Dual/AudioOnly
    Container audioHint = Container::Unknown;
    std::vector<std::string> keepJobs;     // old jobs the new selection re-uses (the kept audio)
    std::vector<std::string> releaseJobs;  // every other old job
};

// SEL-07 step 1. Position to carry over, read BEFORE stopping:
//   * pipeline not prerolled yet (a switch during the previous switch's
//     preroll): the barrier's pending position (the query would say 0);
//   * a seek in flight: the reported target (PositionPolicy::positionMs());
//   * otherwise the reported position.
// Pass PositionPolicy::positionMs() as reportedMs.
int64_t captureSwitchPosition(bool prerolled, int64_t barrierPendingMs, int64_t reportedMs);

// SEL-07: steps, hints and job split. Old jobs whose format id is re-used by
// the new selection (normally the audio on a quality switch, the video on a
// dub switch) are kept; the rest are released. Duplicates are collapsed.
SwitchPlan planSwitch(const SwitchRequest &request);

} } // namespace nordtile::player
