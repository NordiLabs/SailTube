// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

// Catalog records shown in lists and on the video page. Display data only:
// a channel label does not establish identity; counts are provider estimates.

#include "../../Media/Domain/MediaFormat.h"
#include "../../Media/Domain/StreamSelection.h"
#include <cstdint>
#include <string>
#include <vector>

namespace nordtile { namespace video { namespace catalog {

struct VideoSummary {
    std::string videoId;
    std::string title;
    std::string channel;
    std::string channelId;
    std::int64_t durationSeconds = -1; // -1 unknown
    std::int64_t viewCount = -1;       // -1 unknown
    std::string thumbnailUrl;
    media::LiveStatus liveStatus = media::LiveStatus::Unknown;
};

struct Chapter {
    double startSeconds = 0;
    double endSeconds = 0;
    std::string title;
};

struct VideoDetails {
    VideoSummary summary;
    std::string description;
    std::string uploadDate;            // YYYYMMDD as supplied
    std::int64_t likeCount = -1;
    std::int64_t channelFollowerCount = -1;
    std::vector<Chapter> chapters;
    std::vector<media::MediaFormat> formats;
    std::string manifestUrl;           // HLS (live) when supplied
    std::vector<std::string> tags;
};

// YouTube video identities are 11 characters of [A-Za-z0-9_-].
inline bool validVideoId(const std::string &id) {
    if (id.size() != 11) return false;
    for (char c : id) {
        const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
        if (!ok) return false;
    }
    return true;
}

inline bool validChannelId(const std::string &id) {
    if (id.size() != 24 || id.compare(0, 2, "UC") != 0) return false;
    for (char c : id) {
        const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
        if (!ok) return false;
    }
    return true;
}

}}}
