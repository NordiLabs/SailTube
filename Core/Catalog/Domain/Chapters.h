// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

// Video chapters (DSC-15). Qt-free.
//
//  * Markers from the watch-next response win (multiMarkersPlayerBarRenderer /
//    macroMarkersListItemRenderer); they are only sorted and de-duplicated.
//  * Otherwise chapters are rebuilt from the description. A line counts when a `[h:]mm:ss`
//    timestamp sits at its start or its end, with at most 8 non-word characters between the
//    timestamp and that line edge (brackets, dashes, emoji, emoji keycaps with VS16). The
//    separators around the title are trimmed. The line's own indentation and trailing
//    whitespace are not counted as decorations.
//  * A rebuilt list is accepted only with at least 3 chapters, the first at 0:00, starts
//    strictly increasing by at least 10 s, and the last start before the duration (checked
//    when the duration is known, i.e. > 0).

#include <cstdint>
#include <string>
#include <vector>

namespace nordtile { namespace video { namespace catalog {

// Named ChapterMark: catalog::Chapter (Video.h) is the resolver's chapter record.
struct ChapterMark {
    ChapterMark() {}
    ChapterMark(const std::string &t, std::int64_t start) : title(t), startSeconds(start) {}
    std::string title;
    std::int64_t startSeconds = 0;
    std::int64_t endSeconds = -1;   // next start, or the duration for the last one; -1 unknown
};

const int kMinDescriptionChapters = 3;
const std::int64_t kMinChapterGapSeconds = 10;
const int kMaxChapterDecoration = 8;   // non-word code points between timestamp and line edge

// Parses "[h:]mm:ss" (seconds two digits, minutes < 60 when hours are given). -1 otherwise.
std::int64_t parseTimestamp(const std::string &text);

// Formats seconds as "m:ss" or "h:mm:ss".
std::string formatTimestamp(std::int64_t seconds);

// Every line of `description` (UTF-8) carrying a chapter timestamp, in order, unvalidated.
std::vector<ChapterMark> chapterCandidates(const std::string &description);

// Why a rebuilt list was refused (tests and diagnostics).
enum class ChapterRejection { None, TooFew, FirstNotZero, NotIncreasing, GapTooSmall, PastDuration };
ChapterRejection validateDescriptionChapters(const std::vector<ChapterMark> &chapters, std::int64_t durationSeconds);

// Rebuilt-from-description chapters, or an empty list when a rule fails.
std::vector<ChapterMark> chaptersFromDescription(const std::string &description, std::int64_t durationSeconds,
                                             ChapterRejection *why = nullptr);

// Sorts, drops duplicate starts and fills endSeconds (the last one ends at the duration when
// known).
std::vector<ChapterMark> finalizeChapters(std::vector<ChapterMark> chapters, std::int64_t durationSeconds);

// Response markers when there are any, else the description rebuild; always finalized.
std::vector<ChapterMark> chooseChapters(const std::vector<ChapterMark> &responseMarkers, const std::string &description,
                                    std::int64_t durationSeconds, bool *fromResponse = nullptr);

// Index of the chapter playing at `positionSeconds` (the last one starting at or before it), or
// -1 for an empty list. A position before the first start maps to 0.
int chapterAt(const std::vector<ChapterMark> &chapters, double positionSeconds);

}}}
