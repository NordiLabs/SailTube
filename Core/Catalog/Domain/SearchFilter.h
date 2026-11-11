// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

// Search filters and their public `sp` encoding (DSC-08). Qt-free.
//
// `sp` is a small protobuf message, base64 encoded, then percent-encoded for URLs:
//   field 1 (varint)  sort: 1 rating, 2 upload date, 3 views; relevance is the default and omitted
//   field 2 (message) filters, fields in ascending order:
//       1 upload date: 1 last hour, 2 today, 3 week, 4 month, 5 year
//       2 type: 1 video, 2 channel, 3 playlist
//       3 duration: 1 under 4 min, 2 over 20 min, 3 4-20 min
//       8 live: 1
// The InnerTube body carries the raw base64 (`params`); the /results URL the percent-encoded form.

#include <cstdint>
#include <string>
#include <vector>

namespace nordtile { namespace video { namespace catalog {

enum class SearchSort { Relevance = 0, Rating = 1, UploadDate = 2, Views = 3 };
enum class UploadDateFilter { Any = 0, LastHour = 1, Today = 2, Week = 3, Month = 4, Year = 5 };
enum class ResultTypeFilter { Any = 0, Video = 1, Channel = 2, Playlist = 3 };
enum class DurationFilter { Any = 0, Under4Minutes = 1, Over20Minutes = 2, From4To20Minutes = 3 };

struct SearchFilter {
    SearchFilter() : sort(SearchSort::Relevance), uploadDate(UploadDateFilter::Any),
                     type(ResultTypeFilter::Any), duration(DurationFilter::Any), live(false) {}
    SearchSort sort;
    UploadDateFilter uploadDate;
    ResultTypeFilter type;
    DurationFilter duration;
    bool live;

    // True when nothing narrows or reorders the results.
    bool isDefault() const {
        return sort == SearchSort::Relevance && uploadDate == UploadDateFilter::Any &&
               type == ResultTypeFilter::Any && duration == DurationFilter::Any && !live;
    }
    // Stable text form for cache keys ("s0d1t1l0v0").
    std::string key() const;
    bool operator==(const SearchFilter &o) const {
        return sort == o.sort && uploadDate == o.uploadDate && type == o.type && duration == o.duration && live == o.live;
    }
    bool operator!=(const SearchFilter &o) const { return !(*this == o); }
};

// Videos-only mode pins type=video when no type was chosen (matching ytsearch:); mixed mode
// (videos, channels and playlists together, DSC-08 SHOULD) leaves the type open.
SearchFilter effectiveSearchFilter(const SearchFilter &filter, bool mixed);

// Protobuf bytes of the filter (empty for the default filter).
std::vector<std::uint8_t> encodeSearchFilterBytes(const SearchFilter &filter);
// Raw base64 for the InnerTube `params` member ("" for the default filter).
std::string searchParams(const SearchFilter &filter);
// Base64, then percent-encoded, for the `sp=` URL parameter ("" for the default filter).
std::string searchSp(const SearchFilter &filter);

// Reads a base64 or percent-encoded `sp` back. Returns false for malformed input or fields this
// encoder does not know (the filter is left unspecified then).
bool decodeSearchParams(const std::string &spOrBase64, SearchFilter *out);

// Helpers (exposed for tests), namespaced to stay clear of other areas' URL helpers.
namespace sp {
std::string base64Encode(const std::vector<std::uint8_t> &bytes);
bool base64Decode(const std::string &text, std::vector<std::uint8_t> *out);
std::string percentEncode(const std::string &text);   // RFC 3986 unreserved characters kept
std::string percentDecode(const std::string &text);
}

}}}
