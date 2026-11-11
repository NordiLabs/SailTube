// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

// Qt-free library decisions (SAILTUBE_REQUIREMENTS LIB-01..07, IMP-01). Every
// number here is the spec's; tests/library_domain checks each boundary.

#include "LibraryTypes.h"
#include <cstdint>
#include <string>
#include <vector>

namespace nordtile { namespace video { namespace library {

namespace limits {
const double watchedFraction = 0.80;            // LIB-04
const std::int64_t endMarginMs = 15000;         // LIB-04/05: "within 15 s of the end"
const std::int64_t resumeMinimumMs = 10000;     // LIB-05: under 10 s stores "none"
const std::int64_t resumeSaveIntervalMs = 30000;// LIB-05: save every 30 s while playing
const std::int64_t resumeExpiryMs = 30LL * 24 * 3600 * 1000; // LIB-05: 30 days
const int resumeCapacity = 1000;                // LIB-05
const int historyCapMinimum = 100;              // LIB-04 setting range
const int historyCapMaximum = 5000;
const int historyCapDefault = 500;
const double progressBarMinimumFraction = 0.005;// LIB-06: bar shown above 0.5 %
const int resumeAskAutoSeconds = 5;             // LIB-05 "Ask" auto-resumes after 5 s
const int playlistTitleMaxChars = 100;
}

// LIB-04: watched = played 80 % or more, or within 15 s of the end.
bool reachesWatched(std::int64_t positionMs, std::int64_t durationMs);

// Sticky merge of a new progress report into an existing record (or a fresh one
// when `existing.known` is false). Keeps the furthest fraction, never clears
// watched, bumps playCount only when `newSession` is true.
HistoryRecord mergeProgress(const HistoryRecord *existing, const PlaybackProgress &progress,
                            std::int64_t nowMs, bool newSession);

// LIB-04 manual mark: fraction 1, position = duration, watched. The caller also
// clears the resume point. Unmark clears flag, fraction and position only.
HistoryRecord markWatched(const HistoryRecord &record, std::int64_t nowMs);
HistoryRecord unmarkWatched(const HistoryRecord &record);

int clampHistoryCap(int requested); // 100..5000, invalid -> default 500
// The setting accepts only 100..5000; anything else is refused (spec v1.0).
bool validHistoryCap(int requested);

// LIB-05: the position to store, or -1 for "none" (delete the point): under
// 10 s, within 15 s of the end, ended, live, or unknown duration.
std::int64_t resumePositionToStore(const PlaybackProgress &progress);

// IMP-01: an imported point is kept only when > 10 s and not within 15 s of the end.
bool resumeEligibleForImport(std::int64_t positionMs, std::int64_t durationMs);

bool resumeExpired(const ResumeRecord &record, std::int64_t nowMs);

// LIB-05 behaviour setting and the start decision. A link `t=` (LNK-02) wins.
enum class ResumeBehaviour { Always = 0, Ask = 1, Never = 2 };
enum class ResumeAction { StartOver, ResumeAt, AskThenResume, StartAtLink };
struct ResumeDecision {
    ResumeAction action = ResumeAction::StartOver;
    std::int64_t positionMs = 0;  // where to start (ResumeAt/AskThenResume/StartAtLink)
    int autoResumeSeconds = 0;    // AskThenResume: 5
};
// savedMs < 0 = no point. linkStartMs < 0 = no t= in the link.
ResumeDecision decideResume(std::int64_t savedMs, ResumeBehaviour behaviour, std::int64_t linkStartMs);

// LIB-06: overlay flags for one thumbnail.
struct OverlayFlags { bool showProgress = false; double progress = 0; bool watchedBadge = false; };
OverlayFlags overlayFor(const WatchState &state);

// LIB-07: hide-watched filter for a list (feed, channel, optional search).
enum class ListContext { Feed, Channel, Search, Other };
struct HideWatchedSettings { bool enabled = false; bool includeSearch = false; };
bool hideFromList(const WatchState &state, ListContext context, const HideWatchedSettings &settings);

// LIB-01: ordering. Name = case-insensitive (ASCII folding plus byte order for the
// rest), ties by channel id. LatestUpload = newest first, unknown (0) last, then Name.
void sortSubscriptions(std::vector<ChannelRecord> &channels, SubscriptionSort sort);

// IMP-01 / IMP-03: identities from URLs.
// "https://www.youtube.com/channel/UC…" (any host of youtube.com, optional trailing
// path or query) -> UC id, else empty.
std::string channelIdFromUrl(const std::string &url);
// True for @handle, /c/ and /user/ URLs that need an online lookup.
bool isHandleUrl(const std::string &url);
// watch?v=, youtu.be/, /shorts/, /embed/, /live/, /v/ -> 11-character id, else empty.
std::string videoIdFromUrl(const std::string &url);
// YouTube list id from "…list=<id>" or a bare id; empty when invalid.
std::string playlistIdFromUrl(const std::string &url);

// LIB-02: local ids are 12 lowercase hex characters. `entropy` comes from the caller.
std::string localPlaylistId(std::uint64_t entropy);
// Trim, collapse inner whitespace runs, cap at 100 characters (UTF-8 aware);
// empty -> `fallback`.
std::string normalizePlaylistTitle(const std::string &title, const std::string &fallback);
// Case-insensitive name match used by the importer's "skip same-named list" rule.
bool sameListName(const std::string &a, const std::string &b);

// Shorts loop items never end; the player saves their end once as "no resume point" (-1). A stop in
// the first seconds saves -1 too, so it counts as the end (watched) only when the last position seen
// is within max(2 s, 10 % of the duration) of the end.
bool loopEndReached(std::int64_t savedMs, std::int64_t lastPositionMs, std::int64_t durationMs);

}}}
