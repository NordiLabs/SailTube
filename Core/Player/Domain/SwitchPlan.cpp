// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "SwitchPlan.h"

#include "SeekPolicy.h"

#include <algorithm>

namespace nordtile { namespace player {

namespace {
bool contains(const std::vector<std::string> &v, const std::string &s) {
    return std::find(v.begin(), v.end(), s) != v.end();
}
} // namespace

int64_t captureSwitchPosition(bool prerolled, int64_t barrierPendingMs, int64_t reportedMs) {
    const int64_t ms = prerolled ? reportedMs : barrierPendingMs;
    return ms > 0 ? ms : 0;
}

SwitchPlan planSwitch(const SwitchRequest &request) {
    SwitchPlan plan;
    plan.steps.push_back(SwitchStep::CapturePosition);
    plan.steps.push_back(SwitchStep::Stop);
    plan.steps.push_back(SwitchStep::SetContainerHints);
    plan.steps.push_back(SwitchStep::SetUrls);
    plan.steps.push_back(SwitchStep::ArmPrerollBarrier);
    plan.steps.push_back(SwitchStep::Play);
    plan.steps.push_back(SwitchStep::ReleaseOldJobs);

    plan.resumeMs = request.capturedPositionMs > 0 ? request.capturedPositionMs : 0;
    plan.rate = request.rate > 0.0 ? request.rate : 1.0;
    plan.videoHint = containerHintFor(request.mode, request.videoContainer);
    plan.audioHint = containerHintFor(request.mode, request.audioContainer);

    for (std::vector<std::string>::const_iterator it = request.oldJobFormatIds.begin();
         it != request.oldJobFormatIds.end(); ++it) {
        const std::string &id = *it;
        if (id.empty() || contains(plan.keepJobs, id) || contains(plan.releaseJobs, id)) continue;
        const bool reused = id == request.newAudioFormatId || id == request.newVideoFormatId;
        (reused ? plan.keepJobs : plan.releaseJobs).push_back(id);
    }
    return plan;
}

} } // namespace nordtile::player
