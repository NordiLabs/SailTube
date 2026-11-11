// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

// Decisions behind the subscriptions feed (DSC-05/06/07). Qt-free, clock injectable.
//
//  * Per-channel state: the last good rows, when they were fetched, the last attempt and how it
//    ended, and the Videos-tab continuation used to extend the channel for depth.
//  * Refresh interval by the age of the channel's newest upload: under 2 days 10 min, under
//    14 days 1 h, under 60 days 6 h, older 24 h. Channels without rows retry hourly. A failed
//    fetch keeps the last good rows. New channels are due at once; unsubscribed ones are pruned.
//  * Merge: every channel's rows newest first by the best available timestamp (RSS absolute
//    dates; InnerTube relative ages converted with the fetch time when the rows arrived).
//    Running streams are pinned on top. Upcoming streams sort at their fetch time (never as a
//    far-future sentinel), so they sink once they are old. Malformed rows (no id, not a video,
//    no usable timestamp) are skipped and counted. The shared filterRows applies last.
//  * The shown list is replaced only when its id sequence changed (DSC-06): the diff gives the
//    common prefix and suffix so a list model can splice instead of resetting.

#include "Item.h"
#include "Outcome.h"
#include "RowFilter.h"
#include "Subscriptions.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace nordtile { namespace video { namespace catalog {

const std::int64_t kFeedMinute = 60;
const std::int64_t kFeedHour = 3600;
const std::int64_t kFeedDay = 86400;
const std::int64_t kEmptyChannelRetrySeconds = kFeedHour;
// A cached LIVE state older than this is not trusted for pinning (the stream may have ended
// while the app was closed); the row shows as a past stream until the next refresh.
const std::int64_t kLiveTrustSeconds = 6 * kFeedHour;
const int kFeedMaxParallel = 10;
const std::size_t kFeedMaxRowsPerChannel = 400;

enum class FeedSource { None, Rss, InnerTube, Extractor };

struct ChannelFeedState {
    ChannelFeedState() {}
    explicit ChannelFeedState(const ChannelRef &c) : channel(c) {}
    ChannelRef channel;
    std::vector<Item> rows;            // last good rows, newest first, timestamps normalised
    std::int64_t fetchedAt = -1;       // last successful fetch (epoch seconds)
    std::int64_t lastAttemptAt = -1;   // last attempt, successful or not
    Outcome lastOutcome = Outcome::Ok;
    FeedSource source = FeedSource::None;
    std::string continuation;          // Videos-tab token to extend the channel (DSC-07)
    bool pagedFromVideosTab = false;   // the Videos tab's first page has been merged in
    bool exhausted = false;            // the channel has no further pages
    int consecutiveFailures = 0;
    int extendFailures = 0;
};

// Interval for a channel whose newest upload is `newestUploadAgeSeconds` old.
std::int64_t refreshIntervalForAge(std::int64_t newestUploadAgeSeconds);

// Sort time of a feed row (epoch seconds), or -1 when the row has no usable timestamp.
// `fetchedAt` is when the row's channel answered (for live/upcoming rows without a date).
std::int64_t feedSortTime(const Item &item, std::int64_t fetchedAt);

// Newest upload of a channel (epoch seconds), -1 when it has no datable rows. A running stream
// counts as an upload at fetch time; upcoming streams do not count.
std::int64_t newestUploadEpoch(const ChannelFeedState &state);

// The channel's interval right now: hourly without rows, else by the newest upload's age.
std::int64_t refreshInterval(const ChannelFeedState &state, std::int64_t now);
bool isDue(const ChannelFeedState &state, std::int64_t now);
std::int64_t dueAt(const ChannelFeedState &state, std::int64_t now);

// Converts relative ages into estimated epochs with the time the rows were fetched. Rows that
// already carry an absolute date (RSS, microformat) keep it. Live/upcoming rows are left alone.
void normalizeTimestamps(std::vector<Item> &rows, std::int64_t fetchTime);

bool isMalformedFeedRow(const Item &item, std::int64_t fetchedAt);

// Applies the live/upcoming state found on the channel's Live tab to `rows` by id (RSS rows
// carry none). Running or upcoming streams missing from `rows` are added. Returns the number
// of rows changed or added.
int applyLiveState(std::vector<Item> &rows, const std::vector<Item> &liveTabRows);

// When enrichment was unavailable, keeps the live/upcoming state the previous rows had.
void carryLiveState(std::vector<Item> &fresh, const std::vector<Item> &previous);

struct MergedFeed {
    std::vector<Item> rows;
    int hiddenShorts = 0;
    int hiddenMembersOnly = 0;
    int hiddenRestricted = 0;
    int hiddenWatched = 0;
    int duplicates = 0;
    int malformed = 0;
    int pinnedLive = 0;
    int hiddenTotal() const { return hiddenShorts + hiddenMembersOnly + hiddenRestricted + hiddenWatched; }
};

MergedFeed mergeFeed(const std::vector<ChannelFeedState> &states, std::int64_t now,
                     const RowFilterSettings &settings, const ShortsLookup &lookup = ShortsLookup());

// Shown list versus a new merge, compared by id only.
struct SequenceDiff {
    bool changed = false;
    std::size_t prefix = 0;   // rows equal at the start
    std::size_t suffix = 0;   // rows equal at the end (never overlapping the prefix)
};
SequenceDiff diffIdSequences(const std::vector<Item> &shown, const std::vector<Item> &next);
inline bool idSequenceChanged(const std::vector<Item> &shown, const std::vector<Item> &next) {
    return diffIdSequences(shown, next).changed;
}

// The feed's state across channels. Single-threaded: the service owns it on the UI thread and
// persists `snapshot()` copies (DSC-06: never serialise the live state).
class FeedSchedule {
public:
    using Clock = std::function<std::int64_t()>;   // epoch seconds
    explicit FeedSchedule(Clock clock = Clock());

    std::int64_t now() const;

    // Prunes unsubscribed channels, adds new ones (due at once), refreshes names, keeps order.
    // Returns true when the set of channels changed.
    bool setChannels(const ChannelList &channels);
    const std::vector<ChannelFeedState> &states() const { return states_; }
    const ChannelFeedState *find(const std::string &channelId) const;
    std::size_t size() const { return states_.size(); }

    // Channels to refresh: the due ones, or every channel when forced.
    ChannelList due(bool force) const;
    // Earliest time a channel becomes due (epoch seconds); -1 without channels.
    std::int64_t nextDueAt() const;
    bool anyDue() const;

    void markAttempt(const std::string &channelId);
    // A good answer (rows may be empty: a channel without uploads). Older rows from earlier
    // extensions are kept below the fresh ones, with their continuation.
    bool applySuccess(const std::string &channelId, std::vector<Item> rows, FeedSource source,
                      const std::string &continuation, bool fromVideosTab, Outcome outcome = Outcome::Ok);
    // A failed fetch: the last good rows stay.
    bool applyFailure(const std::string &channelId, Outcome outcome);

    // Depth (DSC-07): channels that can be extended, newest frontier first, at most `max`.
    ChannelList extendable(int max) const;
    bool canExtend() const;
    // Merges an extension page (the Videos tab's first page, or a continuation page).
    bool applyPage(const std::string &channelId, std::vector<Item> rows, const std::string &continuation);
    bool applyExtendFailure(const std::string &channelId);

    MergedFeed merged(const RowFilterSettings &settings, const ShortsLookup &lookup = ShortsLookup()) const;

    std::vector<ChannelFeedState> snapshot() const { return states_; }
    void restore(const std::vector<ChannelFeedState> &states);

private:
    ChannelFeedState *findMutable(const std::string &channelId);
    Clock clock_;
    std::vector<ChannelFeedState> states_;
};

}}}
