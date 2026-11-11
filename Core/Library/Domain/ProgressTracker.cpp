// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "ProgressTracker.h"

namespace nordtile { namespace video { namespace library {

ProgressTracker::ProgressTracker(std::int64_t saveIntervalMs) : interval_(saveIntervalMs > 0 ? saveIntervalMs : 30000) {}

std::vector<ProgressReport> ProgressTracker::open(const VideoEntry &video, bool live, std::int64_t nowMs) {
    std::vector<ProgressReport> out = flush(false); // leaving the previous video
    open_ = true;
    live_ = live;
    playing_ = false;
    started_ = false;
    reported_ = false;
    dirty_ = false;
    video_ = video;
    positionMs_ = 0;
    durationMs_ = video.durationSeconds > 0 ? video.durationSeconds * 1000 : 0;
    lastSaveMs_ = nowMs;
    return out;
}

void ProgressTracker::updateVideo(const VideoEntry &video, bool live) {
    if (!open_ || video.videoId != video_.videoId) return;
    if (!video.title.empty()) video_.title = video.title;
    if (!video.channel.empty()) video_.channel = video.channel;
    if (!video.channelId.empty()) video_.channelId = video.channelId;
    if (!video.thumbnailUrl.empty()) video_.thumbnailUrl = video.thumbnailUrl;
    if (video.durationSeconds > 0) {
        video_.durationSeconds = video.durationSeconds;
        if (durationMs_ <= 0) durationMs_ = video.durationSeconds * 1000;
    }
    live_ = live;
}

std::vector<ProgressReport> ProgressTracker::position(std::int64_t positionMs, std::int64_t durationMs, std::int64_t nowMs) {
    if (!open_) return std::vector<ProgressReport>();
    if (durationMs > 0) durationMs_ = durationMs;
    if (positionMs > 0) {
        if (!started_) lastSaveMs_ = nowMs; // the first 30 s window starts with playback
        started_ = true;
        if (positionMs != positionMs_) dirty_ = true;
        positionMs_ = positionMs;
    }
    if (playing_ && started_ && nowMs - lastSaveMs_ >= interval_) {
        lastSaveMs_ = nowMs;
        return flush(false);
    }
    return std::vector<ProgressReport>();
}

std::vector<ProgressReport> ProgressTracker::playingChanged(bool playing, std::int64_t nowMs) {
    if (!open_) return std::vector<ProgressReport>();
    const bool paused = playing_ && !playing;
    playing_ = playing;
    if (playing) lastSaveMs_ = nowMs;
    return paused ? flush(false) : std::vector<ProgressReport>();
}

std::vector<ProgressReport> ProgressTracker::ended(std::int64_t nowMs) {
    if (!open_) return std::vector<ProgressReport>();
    lastSaveMs_ = nowMs;
    started_ = true;
    if (durationMs_ > 0) positionMs_ = durationMs_;
    dirty_ = true;
    playing_ = false;
    return flush(true);
}

std::vector<ProgressReport> ProgressTracker::stop(std::int64_t nowMs) {
    std::vector<ProgressReport> out = flush(false);
    lastSaveMs_ = nowMs;
    open_ = false;
    playing_ = false;
    return out;
}

std::vector<ProgressReport> ProgressTracker::flush(bool endedNow) {
    std::vector<ProgressReport> out;
    if (!open_ || !started_ || !dirty_) return out;
    ProgressReport r;
    r.progress.video = video_;
    r.progress.positionMs = positionMs_;
    r.progress.durationMs = live_ ? 0 : durationMs_;
    r.progress.ended = endedNow;
    r.progress.live = live_;
    r.newSession = !reported_;
    reported_ = true;
    dirty_ = false;
    out.push_back(r);
    return out;
}

}}}
