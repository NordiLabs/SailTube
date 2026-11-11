// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

// LIB-04/05: when the player's progress is written to the library. The player
// adapter feeds events with explicit times; the tracker answers with the
// reports to store: every 30 s while playing, on pause, on leave (another video
// opened), on stop and at the end. Nothing is reported for a video that never
// started playing. Qt-free so the cadence is tested without a media backend.

#include "LibraryTypes.h"
#include <cstdint>
#include <vector>

namespace nordtile { namespace video { namespace library {

struct ProgressReport {
    PlaybackProgress progress;
    bool newSession = false;      // first report of this opened video
};

class ProgressTracker {
public:
    explicit ProgressTracker(std::int64_t saveIntervalMs = 30000);

    // A different video is opened (or the same one again after stop).
    std::vector<ProgressReport> open(const VideoEntry &video, bool live, std::int64_t nowMs);
    // Fills in details that arrive after open (title from the resolver, live flag).
    void updateVideo(const VideoEntry &video, bool live);
    std::vector<ProgressReport> position(std::int64_t positionMs, std::int64_t durationMs, std::int64_t nowMs);
    std::vector<ProgressReport> playingChanged(bool playing, std::int64_t nowMs);
    std::vector<ProgressReport> ended(std::int64_t nowMs);
    std::vector<ProgressReport> stop(std::int64_t nowMs);

    bool active() const { return open_; }
    const VideoEntry &video() const { return video_; }

private:
    std::vector<ProgressReport> flush(bool ended);

    std::int64_t interval_;
    bool open_ = false;
    bool live_ = false;
    bool playing_ = false;
    bool started_ = false;
    bool reported_ = false;
    bool dirty_ = false;
    VideoEntry video_;
    std::int64_t positionMs_ = 0;
    std::int64_t durationMs_ = 0;
    std::int64_t lastSaveMs_ = 0;
};

}}}
