// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

// SponsorBlock data (SB-01): the hash-prefix privacy rule, the category list and the segment
// filter. The skip decision (SB-02) belongs to the playback component; it consumes these segments.
// Qt-free; SHA-256 is implemented here so the prefix rule is testable without Qt.

#include <string>
#include <vector>

namespace nordtile { namespace video { namespace catalog {

// Lower-case hex SHA-256 (FIPS 180-4) of the bytes of `data`.
std::string sha256Hex(const std::string &data);

// The first 4 hex characters of sha256(videoId): the only part of the id the request carries.
std::string sponsorHashPrefix(const std::string &videoId);

// The eight skip categories, always requested (SB-02 order).
const std::vector<std::string> &sponsorCategories();
// "poi_highlight": a point segment offered as "jump to highlight" (SB-02 SHOULD).
extern const char *const kSponsorHighlightCategory;
// The JSON array for the categories query value, e.g. ["sponsor","selfpromo",...].
std::string sponsorCategoriesJson(bool withHighlight);

struct SponsorSegment {
    double startSeconds;
    double endSeconds;
    std::string category;
    std::string actionType;   // "skip", "mute", "poi", ...
    std::string uuid;
    SponsorSegment() : startSeconds(0), endSeconds(0) {}
    SponsorSegment(double s, double e, const std::string &c) : startSeconds(s), endSeconds(e), category(c) {}
};

// One entry of the prefix answer: all segments of one video whose id shares the prefix.
struct SponsorPrefixEntry {
    std::string videoId;
    std::vector<SponsorSegment> segments;
};

struct SponsorSegmentList {
    std::vector<SponsorSegment> segments;   // end > start, sorted by start then end
    bool hasHighlight;
    double highlightSeconds;                // first poi_highlight of the video, when present
    SponsorSegmentList() : hasHighlight(false), highlightSeconds(0) {}
};

// Keeps the entries for exactly `videoId`: skip-category segments with end > start, sorted;
// a poi_highlight (a point, start == end) is reported separately.
SponsorSegmentList filterSponsorSegments(const std::vector<SponsorPrefixEntry> &answer, const std::string &videoId);

}}}
