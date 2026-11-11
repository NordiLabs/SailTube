// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once
// In-memory cache of resolved sets (usually filled by a prefetch). Time is passed in.
//
// An entry is valid until min(earliest expiry - 120 s, now + 20 min); with no known expiry it lasts
// five minutes; it is stored only if it is still valid for more than five seconds. Failures and
// live answers are never stored (the caller does not offer them). At most 24 entries, least
// recently used out first. Invalidating a video drops every key of it.
#include "StreamTypes.h"
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace nordtile { namespace music { namespace sharedplayback { namespace domain {

class ResolveCache {
public:
    static const std::size_t capacity = 24;
    static const std::int64_t expiryMarginMs = 120 * 1000;
    static const std::int64_t maximumLifeMs = 20 * 60 * 1000;
    static const std::int64_t unknownExpiryLifeMs = 5 * 60 * 1000;
    static const std::int64_t minimumLifeMs = 5 * 1000;

    // Valid-until time for a set resolved at `nowMs`.
    static std::int64_t validUntil(const StreamSet &set, std::int64_t nowMs) {
        std::int64_t until = nowMs + maximumLifeMs;
        if (set.expiresAtUnixMs > 0) {
            const std::int64_t byExpiry = set.expiresAtUnixMs - expiryMarginMs;
            if (byExpiry < until) until = byExpiry;
        } else {
            until = nowMs + unknownExpiryLifeMs;
        }
        return until;
    }

    bool put(const std::string &key, const StreamSet &set, std::int64_t nowMs) {
        const std::int64_t until = validUntil(set, nowMs);
        if (until - nowMs <= minimumLifeMs) return false;
        erase(key);
        Entry entry; entry.key = key; entry.videoId = set.videoId; entry.set = set; entry.until = until;
        entries_.push_back(entry);
        while (entries_.size() > capacity) entries_.erase(entries_.begin());
        return true;
    }

    // Returns the stored set (most recently used afterwards) or nullptr.
    const StreamSet *get(const std::string &key, std::int64_t nowMs) {
        prune(nowMs);
        for (std::size_t i = 0; i < entries_.size(); ++i) {
            if (entries_[i].key != key) continue;
            Entry hit = entries_[i];
            entries_.erase(entries_.begin() + long(i));
            entries_.push_back(hit);
            return &entries_.back().set;
        }
        return nullptr;
    }

    // A read that changes nothing (no pruning, no use-order change): the stored set when it is still valid, else false.
    bool peek(const std::string &key, std::int64_t nowMs, StreamSet *out) const {
        for (std::size_t i = 0; i < entries_.size(); ++i) {
            if (entries_[i].key != key || entries_[i].until <= nowMs) continue;
            if (out) *out = entries_[i].set;
            return true;
        }
        return false;
    }

    void invalidateVideo(const std::string &videoId) {
        for (std::size_t i = entries_.size(); i-- > 0;) if (entries_[i].videoId == videoId) entries_.erase(entries_.begin() + long(i));
    }
    void clear() { entries_.clear(); }
    std::size_t size() const { return entries_.size(); }

private:
    struct Entry { std::string key, videoId; StreamSet set; std::int64_t until = 0; };
    void erase(const std::string &key) {
        for (std::size_t i = entries_.size(); i-- > 0;) if (entries_[i].key == key) entries_.erase(entries_.begin() + long(i));
    }
    void prune(std::int64_t nowMs) {
        for (std::size_t i = entries_.size(); i-- > 0;) if (entries_[i].until <= nowMs) entries_.erase(entries_.begin() + long(i));
    }
    std::vector<Entry> entries_;
};

}}}}
