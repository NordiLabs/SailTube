// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

// What one Short's details answer says about it (the Shorts section's on-picture data): like and
// comment counts, the channel, the description, the related-video chip and the sound line. A
// list answer (popular search, a channel's Shorts tab, the reel sequence) carries none of it for
// most rows, so one request per Short fills it in (ShortDetailService). Qt-free.
// Unknown numbers are -1, unknown texts "".

#include <cstdint>
#include <cstddef>
#include <string>

namespace nordtile { namespace video { namespace catalog {

struct ShortDetail {
    std::string videoId;
    std::string title;            // the line drawn over the picture (hashtags included)
    std::string channelName;      // display name, else the handle
    std::string channelHandle;    // "@name"
    std::string channelId;        // "UC" + 22 characters
    std::string channelAvatar;    // https address, rewritten to the catalog's avatar size
    std::int64_t likeCount = -1;
    std::string likeText;         // YouTube's compact text in the display language; "" when hidden
    std::int64_t commentCount = -1;
    std::string commentText;      // "115", "2,211"; "" when unknown or comments are off
    bool commentsDisabled = false; // the answer has no comment button (comments off, made for kids)
    std::int64_t viewCount = -1;
    std::string viewText;         // the description header's text ("155,072 views")
    std::string publishedText;    // "18 hours ago"
    std::string description;      // full plain text the creator wrote; "" when there is none
    std::string relatedVideoId;   // the "related video" chip, when it links to a video or a Short
    std::string relatedVideoTitle;
    bool relatedVideoIsShort = false;
    std::string soundText;        // "ouuuuuu · @jaquelinearagonn"

    // The description's first line, at most `limit` characters (counted in code points), with an
    // ellipsis when something was cut. "" when there is no description.
    std::string descriptionShort(std::size_t limit = 100) const;
    // The picture of the related video: the vertical frame for a Short, else the list thumbnail
    // (derived from the id; every video has one). "" when there is no related video.
    std::string relatedVideoThumbnail() const;
};

// The switch for the Return YouTube Dislike estimate of a Short. kShortsDislikeEstimate is the
// shipped value, and it is false: YouTube stopped reporting dislikes for Shorts, so the service
// extrapolates from almost nothing (checked 2026-11-11: 13 dislikes next to 4,987 likes, 14 next
// to 62,274, with 0 raw dislikes). With it off the Shorts model asks the service for nothing and
// dislikeCount stays -1 (the rail shows no number). The watch page's estimate is not affected.
extern const bool kShortsDislikeEstimate;
bool shortsDislikeEstimate();
void setShortsDislikeEstimate(bool on);   // tests; starts as kShortsDislikeEstimate

}}}
