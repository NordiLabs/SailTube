// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

// A small bounded cache with an optional time-to-live and an injectable clock, used by the
// caption cue cache (12 URLs for the session), SponsorBlock (10 min, 64 entries) and Return
// YouTube Dislike (10 min). Callers store successes only: failures are never put here.
// Eviction drops the least recently stored or read entry. Not thread-safe: services use it on
// their owner thread.

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <list>
#include <map>
#include <utility>

namespace nordtile { namespace video { namespace catalog {

using MillisClock = std::function<std::int64_t()>;

inline std::int64_t steadyMillis() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
}

template <typename K, typename V>
class ExpiringCache {
public:
    // ttlMs <= 0 keeps entries until they are evicted (session cache).
    ExpiringCache(std::size_t capacity, std::int64_t ttlMs, MillisClock clock = MillisClock())
        : capacity_(capacity ? capacity : 1), ttlMs_(ttlMs), clock_(clock ? clock : MillisClock(steadyMillis)) {}

    void setClock(MillisClock clock) { clock_ = clock ? clock : MillisClock(steadyMillis); }
    std::size_t capacity() const { return capacity_; }
    std::size_t size() const { return index_.size(); }

    // Returns true and fills `out` for a fresh entry; an expired entry is removed.
    bool get(const K &key, V *out) {
        auto it = index_.find(key);
        if (it == index_.end()) return false;
        if (ttlMs_ > 0 && clock_() - it->second->storedAt >= ttlMs_) {
            order_.erase(it->second);
            index_.erase(it);
            return false;
        }
        order_.splice(order_.end(), order_, it->second);
        if (out) *out = it->second->value;
        return true;
    }

    bool contains(const K &key) { return get(key, nullptr); }

    void put(const K &key, const V &value) {
        auto it = index_.find(key);
        if (it != index_.end()) {
            it->second->value = value;
            it->second->storedAt = clock_();
            order_.splice(order_.end(), order_, it->second);
            return;
        }
        while (index_.size() >= capacity_ && !order_.empty()) {
            index_.erase(order_.front().key);
            order_.pop_front();
        }
        Entry e;
        e.key = key;
        e.value = value;
        e.storedAt = clock_();
        order_.push_back(e);
        index_[key] = std::prev(order_.end());
    }

    void remove(const K &key) {
        auto it = index_.find(key);
        if (it == index_.end()) return;
        order_.erase(it->second);
        index_.erase(it);
    }

    void clear() { order_.clear(); index_.clear(); }

private:
    struct Entry {
        K key;
        V value;
        std::int64_t storedAt;
        Entry() : storedAt(0) {}
    };
    std::size_t capacity_;
    std::int64_t ttlMs_;
    MillisClock clock_;
    std::list<Entry> order_;
    std::map<K, typename std::list<Entry>::iterator> index_;
};

}}}
