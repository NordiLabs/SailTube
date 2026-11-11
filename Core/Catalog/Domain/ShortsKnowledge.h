// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

// The one Shorts rule (DSC-17) and its persistent knowledge. Qt-free.
//
//  * A video is a Short only on structural evidence: a Shorts renderer, an RSS /shorts/ link,
//    or membership of its channel's Shorts tab.
//  * Without evidence, a video of 180 s or less is a candidate only (still shown). A background
//    Shorts-tab check of its channel settles it: listed -> Short, absent -> "checked, not a Short".
//  * Stores (recency-ordered LRU, persisted by the infrastructure): video id -> duration
//    (3000 entries), known Shorts ids with their evidence (4000), checked-not-a-Short ids (4000).
//  * Planning: which channels need a Shorts-tab check (rows with candidates, not checked in the
//    last 30 min, at most 8 per plan).
//
// Every list feeds observe() so the stores learn durations and evidence from any source; the
// row filter (RowFilter.h) consults lookup().

#include "Item.h"
#include "LruStore.h"
#include "RowFilter.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace nordtile { namespace video { namespace catalog {

class ShortsKnowledge {
public:
    static const std::size_t kDurationCapacity = 3000;
    static const std::size_t kShortsCapacity = 4000;
    static const std::size_t kNotShortCapacity = 4000;
    static const std::size_t kChannelMemory = 512;
    static const std::int64_t kRecheckIntervalMs = 30LL * 60 * 1000;
    static const std::size_t kMaxChecksPerPlan = 8;

    // `nowMs` is injectable for tests (defaults to the steady clock).
    explicit ShortsKnowledge(std::function<std::int64_t()> nowMs = std::function<std::int64_t()>());

    // Durations.
    void recordDuration(const std::string &videoId, std::int64_t seconds);
    std::int64_t duration(const std::string &videoId) const;   // -1 when unknown

    // Evidence. markShort() takes confirmed evidence only (Renderer, RssLink, ShortsTab) and
    // overrides an earlier "not a Short". markNotShort() never overrides a known Short.
    void markShort(const std::string &videoId, ShortEvidence evidence);
    void markNotShort(const std::string &videoId);
    // Confirmed evidence, NotShort, or None.
    ShortEvidence evidence(const std::string &videoId) const;
    // A lookup for filterRows(); it refers to this object, which must outlive it.
    ShortsLookup lookup() const;

    // Learns durations and structural evidence from a listing. `fromShortsTab` marks every
    // video row as listed on its channel's Shorts tab. Returns true when anything changed.
    bool observe(const std::vector<Item> &rows, bool fromShortsTab = false);
    // Fills unknown durations of rows from the store (RSS rows carry none).
    void fillDurations(std::vector<Item> &rows) const;
    // The evidence a row gets everywhere: row evidence, then the stores, then the 180 s
    // candidate rule (using the stored duration when the row has none).
    ShortEvidence classify(const Item &row) const;

    // Channels that need a Shorts-tab check: channels of rows classified Candidate that were
    // not checked within kRecheckIntervalMs, in first-seen order, at most `limit`.
    std::vector<std::string> channelsNeedingCheck(const std::vector<Item> &rows,
                                                  std::size_t limit = kMaxChecksPerPlan) const;
    // The candidate ids of one channel among `rows`.
    std::vector<std::string> candidatesOf(const std::string &channelId, const std::vector<Item> &rows) const;
    void noteChecked(const std::string &channelId);
    bool recentlyChecked(const std::string &channelId) const;
    // Settles a channel's candidates from its Shorts tab: listed ids become Shorts (ShortsTab
    // evidence), candidates absent from the list become "checked, not a Short". Returns the
    // number of candidates settled either way.
    int applyShortsTab(const std::string &channelId, const std::vector<std::string> &shortsTabIds,
                       const std::vector<std::string> &candidateIds);

    std::size_t durationCount() const { return durations_.size(); }
    std::size_t shortsCount() const { return shorts_.size(); }
    std::size_t notShortCount() const { return notShorts_.size(); }
    const LruStore<std::string, std::int64_t> &durations() const { return durations_; }
    const LruStore<std::string, int> &shorts() const { return shorts_; }
    const LruStore<std::string, bool> &notShorts() const { return notShorts_; }

    // Persistence format (line based, oldest entry first so a reload keeps recency order):
    //   sailtube-shorts 1
    //   d <videoId> <seconds>
    //   s <videoId> <evidence: r|l|t>
    //   n <videoId>
    std::string serialize() const;
    // Replaces the stores; malformed lines are skipped. False when the header is wrong.
    bool deserialize(const std::string &data);

    static bool isVideoId(const std::string &id);

private:
    std::int64_t now() const;
    std::function<std::int64_t()> nowMs_;
    LruStore<std::string, std::int64_t> durations_;
    LruStore<std::string, int> shorts_;          // value: ShortEvidence as int
    LruStore<std::string, bool> notShorts_;
    LruStore<std::string, std::int64_t> checkedAt_;   // channel -> last check (session only)
};

}}}
