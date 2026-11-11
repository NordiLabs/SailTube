// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

// One row in any discovery list (search, home, feed, channel tabs, related, playlists).
// Qt-free. Every list shares this shape so a presentation model can mix kinds (DSC-08 mixed
// results) and keep fixed roles. Values are display data from the provider: a channel label
// does not establish identity and counts are estimates.

#include "../../Media/Domain/StreamSelection.h"
#include <cstdint>
#include <string>
#include <vector>

namespace nordtile { namespace video { namespace catalog {

enum class ItemKind { Video, Short, Channel, Playlist };

// How we know a video is a Short (DSC-17). Only structural evidence confirms one.
enum class ShortEvidence {
    None,          // no evidence either way
    Candidate,     // 180 s or less without structural evidence: a candidate only
    Renderer,      // shortsLockupViewModel / reelItemRenderer
    RssLink,       // RSS alternate link contains /shorts/
    ShortsTab,     // listed on the channel's Shorts tab
    NotShort       // checked against the channel's Shorts tab and absent
};

inline bool isConfirmedShort(ShortEvidence e) {
    return e == ShortEvidence::Renderer || e == ShortEvidence::RssLink || e == ShortEvidence::ShortsTab;
}

struct Item {
    ItemKind kind = ItemKind::Video;
    std::string id;                 // video id, UC channel id or playlist id
    std::string title;
    std::string channel;            // uploader / owner label
    std::string channelId;          // UC id when the response carries one
    std::int64_t durationSeconds = -1;
    std::string durationText;       // raw badge text, shown when parsing failed (I18N-02)
    std::int64_t viewCount = -1;    // views, or current viewers while live
    std::string viewText;           // raw text kept for display when the count did not parse
    std::int64_t publishedEpoch = -1;   // absolute UTC seconds when known (RSS, microformat)
    std::int64_t publishedAgeSeconds = -1; // parsed from relative text ("3 days ago")
    std::string publishedText;      // raw relative/absolute text
    std::string thumbnailUrl;
    std::string avatarUrl;          // channel rows (and owners when supplied)
    // Parser hint, not stored: 1 the answer named the 1280x720 picture (hq720) for this video,
    // -1 it named only smaller ones, 0 it named none (see ThumbnailBook).
    int largeThumbnailListed = 0;
    media::LiveStatus liveStatus = media::LiveStatus::Unknown;
    bool membersOnly = false;       // DSC-18
    bool restricted = false;        // age-restricted / unavailable badge (DSC-18)
    ShortEvidence shortEvidence = ShortEvidence::None;
    // Channel rows.
    std::string handle;             // "@name" when known
    std::string subscriberText;
    std::int64_t subscriberCount = -1;
    // Playlist rows (and channels' video counts).
    std::int64_t videoCount = -1;
    std::string videoCountText;
    // A video row that belongs to a playlist (LNK-04 list= queue, playlist previews).
    std::string playlistId;
    int playlistIndex = -1;
};

inline bool isVideoLike(const Item &item) {
    return item.kind == ItemKind::Video || item.kind == ItemKind::Short;
}

}}}
