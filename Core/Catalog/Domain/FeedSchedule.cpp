// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "FeedSchedule.h"

#include <algorithm>
#include <chrono>
#include <map>
#include <set>

namespace nordtile { namespace video { namespace catalog {

using media::LiveStatus;

namespace {

std::int64_t systemNow() {
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

bool isRunning(const Item &item) { return item.liveStatus == LiveStatus::IsLive; }
bool isUpcoming(const Item &item) { return item.liveStatus == LiveStatus::Upcoming; }

// Newest first; equal times keep their order.
void sortChannelRows(std::vector<Item> &rows, std::int64_t fetchedAt) {
    std::stable_sort(rows.begin(), rows.end(), [fetchedAt](const Item &a, const Item &b) {
        return feedSortTime(a, fetchedAt) > feedSortTime(b, fetchedAt);
    });
}

void capRows(ChannelFeedState &state) {
    if (state.rows.size() > kFeedMaxRowsPerChannel) {
        state.rows.resize(kFeedMaxRowsPerChannel);
        state.exhausted = true;
        state.continuation.clear();
    }
}

// Fills what the existing row lacks from another answer for the same video (the InnerTube
// Videos tab has durations and live flags that RSS rows lack; RSS keeps its exact date).
void fillMissing(Item &row, const Item &other) {
    if (row.durationSeconds < 0 && other.durationSeconds >= 0) { row.durationSeconds = other.durationSeconds; row.durationText = other.durationText; }
    if (row.viewCount < 0 && other.viewCount >= 0) { row.viewCount = other.viewCount; row.viewText = other.viewText; }
    if (row.liveStatus == LiveStatus::Unknown || isRunning(other) || isUpcoming(other)) row.liveStatus = other.liveStatus;
    if (row.publishedText.empty()) row.publishedText = other.publishedText;
    if (!isConfirmedShort(row.shortEvidence) && isConfirmedShort(other.shortEvidence)) {
        row.shortEvidence = other.shortEvidence;
        row.kind = ItemKind::Short;
    }
    if (!row.membersOnly && other.membersOnly) row.membersOnly = true;
    if (row.thumbnailUrl.empty()) row.thumbnailUrl = other.thumbnailUrl;
}

}  // namespace

std::int64_t refreshIntervalForAge(std::int64_t age) {
    if (age < 0) return kEmptyChannelRetrySeconds;
    if (age < 2 * kFeedDay) return 10 * kFeedMinute;
    if (age < 14 * kFeedDay) return kFeedHour;
    if (age < 60 * kFeedDay) return 6 * kFeedHour;
    return 24 * kFeedHour;
}

std::int64_t feedSortTime(const Item &item, std::int64_t fetchedAt) {
    if (isUpcoming(item)) {
        // Keyed to "now" at the fetch, never to a far-future start time.
        if (item.publishedEpoch >= 0 && (fetchedAt < 0 || item.publishedEpoch < fetchedAt)) return item.publishedEpoch;
        return fetchedAt;
    }
    if (isRunning(item)) return item.publishedEpoch >= 0 ? item.publishedEpoch : fetchedAt;
    if (item.publishedEpoch >= 0) return item.publishedEpoch;
    if (item.publishedAgeSeconds >= 0 && fetchedAt >= 0) return fetchedAt - item.publishedAgeSeconds;
    return -1;
}

std::int64_t newestUploadEpoch(const ChannelFeedState &state) {
    std::int64_t newest = -1;
    for (const Item &row : state.rows) {
        if (isUpcoming(row)) continue;
        const std::int64_t t = isRunning(row) ? state.fetchedAt : feedSortTime(row, state.fetchedAt);
        if (t > newest) newest = t;
    }
    return newest;
}

std::int64_t refreshInterval(const ChannelFeedState &state, std::int64_t now) {
    if (state.rows.empty()) return kEmptyChannelRetrySeconds;
    const std::int64_t newest = newestUploadEpoch(state);
    if (newest < 0) return kEmptyChannelRetrySeconds;
    return refreshIntervalForAge(std::max<std::int64_t>(0, now - newest));
}

std::int64_t dueAt(const ChannelFeedState &state, std::int64_t now) {
    if (state.lastAttemptAt < 0 || now < state.lastAttemptAt) return now;   // new, or the clock went back
    return state.lastAttemptAt + refreshInterval(state, now);
}

bool isDue(const ChannelFeedState &state, std::int64_t now) { return dueAt(state, now) <= now; }

void normalizeTimestamps(std::vector<Item> &rows, std::int64_t fetchTime) {
    for (Item &row : rows) {
        if (isRunning(row) || isUpcoming(row)) continue;
        if (row.publishedEpoch < 0 && row.publishedAgeSeconds >= 0 && fetchTime >= 0)
            row.publishedEpoch = fetchTime - row.publishedAgeSeconds;
    }
}

bool isMalformedFeedRow(const Item &item, std::int64_t fetchedAt) {
    return item.id.empty() || !isVideoLike(item) || feedSortTime(item, fetchedAt) < 0;
}

int applyLiveState(std::vector<Item> &rows, const std::vector<Item> &liveTabRows) {
    std::map<std::string, const Item *> byId;
    for (const Item &l : liveTabRows) if (!l.id.empty() && isVideoLike(l)) byId[l.id] = &l;
    int changed = 0;
    std::set<std::string> present;
    for (Item &row : rows) {
        present.insert(row.id);
        auto it = byId.find(row.id);
        if (it == byId.end()) continue;
        const Item &l = *it->second;
        if (isRunning(l) || isUpcoming(l)) {
            if (row.liveStatus != l.liveStatus) ++changed;
            row.liveStatus = l.liveStatus;
            if (isRunning(l) && l.viewCount >= 0) { row.viewCount = l.viewCount; row.viewText = l.viewText; }
            if (isUpcoming(l) && l.publishedEpoch >= 0 && row.publishedEpoch < 0) row.publishedEpoch = l.publishedEpoch;
        } else if (row.liveStatus == LiveStatus::Unknown || row.liveStatus == LiveStatus::NotLive ||
                   row.liveStatus == LiveStatus::IsLive || row.liveStatus == LiveStatus::Upcoming) {
            // Listed on the Live tab and no longer running: a past stream.
            if (row.liveStatus != LiveStatus::WasLive) ++changed;
            row.liveStatus = LiveStatus::WasLive;
        }
        if (row.durationSeconds < 0 && l.durationSeconds >= 0) { row.durationSeconds = l.durationSeconds; row.durationText = l.durationText; }
    }
    for (const Item &l : liveTabRows) {
        if (l.id.empty() || !isVideoLike(l) || present.count(l.id)) continue;
        if (!isRunning(l) && !isUpcoming(l)) continue;
        rows.push_back(l);
        present.insert(l.id);
        ++changed;
    }
    return changed;
}

void carryLiveState(std::vector<Item> &fresh, const std::vector<Item> &previous) {
    std::map<std::string, LiveStatus> before;
    for (const Item &p : previous)
        if (isRunning(p) || isUpcoming(p) || p.liveStatus == LiveStatus::WasLive) before[p.id] = p.liveStatus;
    for (Item &row : fresh) {
        if (row.liveStatus != LiveStatus::Unknown) continue;
        auto it = before.find(row.id);
        if (it != before.end()) row.liveStatus = it->second;
    }
}

MergedFeed mergeFeed(const std::vector<ChannelFeedState> &states, std::int64_t now,
                     const RowFilterSettings &settings, const ShortsLookup &lookup) {
    struct Entry {
        Item item;
        std::int64_t key;
        bool pinned;
    };
    MergedFeed out;
    std::vector<Entry> entries;
    for (const ChannelFeedState &state : states) {
        for (const Item &row : state.rows) {
            if (isMalformedFeedRow(row, state.fetchedAt)) { ++out.malformed; continue; }
            Entry e;
            e.item = row;
            e.key = feedSortTime(row, state.fetchedAt);
            e.pinned = isRunning(row);
            if (e.pinned && (state.fetchedAt < 0 || now - state.fetchedAt > kLiveTrustSeconds)) {
                e.pinned = false;
                e.item.liveStatus = LiveStatus::WasLive;
            }
            if (e.item.channel.empty()) e.item.channel = state.channel.name;
            if (e.item.channelId.empty()) e.item.channelId = state.channel.id;
            entries.push_back(e);
        }
    }
    std::stable_sort(entries.begin(), entries.end(), [](const Entry &a, const Entry &b) {
        if (a.pinned != b.pinned) return a.pinned;
        return a.key > b.key;
    });
    std::vector<Item> rows;
    rows.reserve(entries.size());
    for (const Entry &e : entries) rows.push_back(e.item);
    std::set<std::string> seen;
    RowFilterResult filtered = filterRows(rows, settings, &seen, lookup);
    out.rows.swap(filtered.visible);
    out.hiddenShorts = filtered.hiddenShorts;
    out.hiddenMembersOnly = filtered.hiddenMembersOnly;
    out.hiddenRestricted = filtered.hiddenRestricted;
    out.hiddenWatched = filtered.hiddenWatched;
    out.duplicates = filtered.duplicates;
    for (const Item &row : out.rows) {
        if (!isRunning(row)) break;
        ++out.pinnedLive;
    }
    return out;
}

SequenceDiff diffIdSequences(const std::vector<Item> &shown, const std::vector<Item> &next) {
    SequenceDiff d;
    const std::size_t n = std::min(shown.size(), next.size());
    while (d.prefix < n && shown[d.prefix].id == next[d.prefix].id) ++d.prefix;
    d.changed = !(shown.size() == next.size() && d.prefix == n);
    if (!d.changed) return d;
    while (d.suffix < n - d.prefix && shown[shown.size() - 1 - d.suffix].id == next[next.size() - 1 - d.suffix].id) ++d.suffix;
    return d;
}

// ---------------------------------------------------------------------------------------------

FeedSchedule::FeedSchedule(Clock clock) : clock_(std::move(clock)) {}

std::int64_t FeedSchedule::now() const { return clock_ ? clock_() : systemNow(); }

bool FeedSchedule::setChannels(const ChannelList &channels) {
    const ChannelList wanted = feedChannels(channels);
    std::vector<ChannelFeedState> next;
    next.reserve(wanted.size());
    bool changed = wanted.size() != states_.size();
    for (const ChannelRef &c : wanted) {
        ChannelFeedState *existing = findMutable(c.id);
        if (existing) {
            ChannelFeedState s = *existing;
            if (!c.name.empty()) s.channel.name = c.name;
            next.push_back(s);
        } else {
            next.push_back(ChannelFeedState(c));
            changed = true;
        }
    }
    states_.swap(next);
    return changed;
}

const ChannelFeedState *FeedSchedule::find(const std::string &channelId) const {
    for (const ChannelFeedState &s : states_) if (s.channel.id == channelId) return &s;
    return nullptr;
}

ChannelFeedState *FeedSchedule::findMutable(const std::string &channelId) {
    for (ChannelFeedState &s : states_) if (s.channel.id == channelId) return &s;
    return nullptr;
}

ChannelList FeedSchedule::due(bool force) const {
    ChannelList out;
    const std::int64_t t = now();
    for (const ChannelFeedState &s : states_) if (force || isDue(s, t)) out.push_back(s.channel);
    return out;
}

std::int64_t FeedSchedule::nextDueAt() const {
    std::int64_t best = -1;
    const std::int64_t t = now();
    for (const ChannelFeedState &s : states_) {
        const std::int64_t d = dueAt(s, t);
        if (best < 0 || d < best) best = d;
    }
    return best;
}

bool FeedSchedule::anyDue() const {
    const std::int64_t t = now();
    for (const ChannelFeedState &s : states_) if (isDue(s, t)) return true;
    return false;
}

void FeedSchedule::markAttempt(const std::string &channelId) {
    if (ChannelFeedState *s = findMutable(channelId)) s->lastAttemptAt = now();
}

bool FeedSchedule::applySuccess(const std::string &channelId, std::vector<Item> rows, FeedSource source,
                                const std::string &continuation, bool fromVideosTab, Outcome outcome) {
    ChannelFeedState *s = findMutable(channelId);
    if (!s) return false;   // unsubscribed while the fetch ran
    const std::int64_t t = now();
    normalizeTimestamps(rows, t);
    std::vector<Item> fresh;
    std::set<std::string> ids;
    for (Item &row : rows) {
        if (row.id.empty() || !ids.insert(row.id).second) continue;
        if (row.channelId.empty()) row.channelId = s->channel.id;
        if (row.channel.empty()) row.channel = s->channel.name;
        fresh.push_back(row);
    }
    sortChannelRows(fresh, t);

    // Keep the older rows of earlier extensions below the fresh window (DSC-07 depth).
    std::int64_t oldest = -1;
    for (const Item &row : fresh) {
        if (isRunning(row) || isUpcoming(row)) continue;
        const std::int64_t k = feedSortTime(row, t);
        if (k >= 0 && (oldest < 0 || k < oldest)) oldest = k;
    }
    bool keptTail = false;
    if (oldest >= 0) {
        for (const Item &old : s->rows) {
            if (ids.count(old.id) || isRunning(old) || isUpcoming(old)) continue;
            const std::int64_t k = feedSortTime(old, s->fetchedAt);
            if (k >= 0 && k < oldest) { fresh.push_back(old); keptTail = true; }
        }
    }
    s->rows.swap(fresh);
    s->fetchedAt = t;
    s->lastAttemptAt = t;
    s->lastOutcome = s->rows.empty() ? Outcome::ValidEmpty : outcome;
    s->source = source;
    s->consecutiveFailures = 0;
    if (fromVideosTab && keptTail && s->pagedFromVideosTab && !s->continuation.empty()) {
        // The kept tail already reaches deeper: keep extending from its token.
    } else if (fromVideosTab) {
        s->continuation = continuation;
        s->pagedFromVideosTab = true;
        s->exhausted = continuation.empty();
        s->extendFailures = 0;
    } else if (!keptTail) {
        s->continuation.clear();
        s->pagedFromVideosTab = false;
        s->exhausted = false;
    }
    capRows(*s);
    return true;
}

bool FeedSchedule::applyFailure(const std::string &channelId, Outcome outcome) {
    ChannelFeedState *s = findMutable(channelId);
    if (!s) return false;
    s->lastAttemptAt = now();
    s->lastOutcome = outcome;
    ++s->consecutiveFailures;
    return true;
}

ChannelList FeedSchedule::extendable(int max) const {
    struct Candidate { ChannelRef channel; std::int64_t frontier; };
    std::vector<Candidate> c;
    for (const ChannelFeedState &s : states_) {
        if (s.rows.empty() || s.exhausted || s.extendFailures >= 2) continue;
        if (s.pagedFromVideosTab && s.continuation.empty()) continue;
        std::int64_t frontier = -1;
        for (const Item &row : s.rows) {
            const std::int64_t k = feedSortTime(row, s.fetchedAt);
            if (k >= 0 && (frontier < 0 || k < frontier)) frontier = k;
        }
        Candidate x;
        x.channel = s.channel;
        x.frontier = frontier;
        c.push_back(x);
    }
    std::stable_sort(c.begin(), c.end(), [](const Candidate &a, const Candidate &b) { return a.frontier > b.frontier; });
    ChannelList out;
    for (const Candidate &x : c) {
        if (static_cast<int>(out.size()) >= max) break;
        out.push_back(x.channel);
    }
    return out;
}

bool FeedSchedule::canExtend() const { return !extendable(1).empty(); }

bool FeedSchedule::applyPage(const std::string &channelId, std::vector<Item> rows, const std::string &continuation) {
    ChannelFeedState *s = findMutable(channelId);
    if (!s) return false;
    const std::int64_t t = now();
    normalizeTimestamps(rows, t);
    std::map<std::string, std::size_t> index;
    for (std::size_t i = 0; i < s->rows.size(); ++i) index[s->rows[i].id] = i;
    int added = 0;
    for (Item &row : rows) {
        if (row.id.empty()) continue;
        if (row.channelId.empty()) row.channelId = s->channel.id;
        if (row.channel.empty()) row.channel = s->channel.name;
        auto it = index.find(row.id);
        if (it != index.end()) { fillMissing(s->rows[it->second], row); continue; }
        index[row.id] = s->rows.size();
        s->rows.push_back(row);
        ++added;
    }
    sortChannelRows(s->rows, s->fetchedAt);
    const bool sameToken = !continuation.empty() && continuation == s->continuation;
    s->continuation = continuation;
    s->pagedFromVideosTab = true;
    s->exhausted = continuation.empty() || (added == 0 && sameToken);
    s->extendFailures = 0;
    capRows(*s);
    return added > 0;
}

bool FeedSchedule::applyExtendFailure(const std::string &channelId) {
    ChannelFeedState *s = findMutable(channelId);
    if (!s) return false;
    ++s->extendFailures;
    // An expired token: start again from the Videos tab's first page next time.
    if (!s->continuation.empty()) { s->continuation.clear(); s->pagedFromVideosTab = false; }
    return true;
}

MergedFeed FeedSchedule::merged(const RowFilterSettings &settings, const ShortsLookup &lookup) const {
    return mergeFeed(states_, now(), settings, lookup);
}

void FeedSchedule::restore(const std::vector<ChannelFeedState> &states) {
    std::vector<ChannelFeedState> next;
    std::set<std::string> seen;
    for (const ChannelFeedState &s : states) {
        if (!isFeedChannelId(s.channel.id) || !seen.insert(s.channel.id).second) continue;
        next.push_back(s);
    }
    states_.swap(next);
}

}}}
