// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

// The watch-next cache policy (DSC-13/14/16 share one answer per video): entries live 10 min,
// at most 32 are kept (least recently used evicted first). Qt-free; the clock is injected so
// tests can age entries. Single-flight bookkeeping (which keys have a request in flight) lives
// here too, so the service can join a second caller to the first request.

#include <cstdint>
#include <functional>
#include <list>
#include <map>
#include <memory>
#include <set>
#include <string>

namespace nordtile { namespace video { namespace catalog {

const std::int64_t kWatchCacheTtlMs = 10LL * 60 * 1000;
const std::size_t kWatchCacheEntries = 32;

template <typename V>
class TtlLruCache {
public:
    using Clock = std::function<std::int64_t()>;
    TtlLruCache(Clock nowMs, std::int64_t ttlMs = kWatchCacheTtlMs, std::size_t capacity = kWatchCacheEntries)
        : now_(std::move(nowMs)), ttl_(ttlMs), capacity_(capacity) {}

    // The live value for `key` (fresh entries only), marked as recently used; null otherwise.
    std::shared_ptr<V> get(const std::string &key) {
        auto it = map_.find(key);
        if (it == map_.end()) return std::shared_ptr<V>();
        if (now_() - it->second.storedMs >= ttl_) { erase(key); return std::shared_ptr<V>(); }
        touch(key);
        return it->second.value;
    }
    // Present regardless of age (an in-flight entry is not aged out under its caller).
    std::shared_ptr<V> peek(const std::string &key) const {
        auto it = map_.find(key);
        return it == map_.end() ? std::shared_ptr<V>() : it->second.value;
    }
    bool fresh(const std::string &key) const {
        auto it = map_.find(key);
        return it != map_.end() && now_() - it->second.storedMs < ttl_;
    }
    void put(const std::string &key, std::shared_ptr<V> value) {
        erase(key);
        Entry e;
        e.value = std::move(value);
        e.storedMs = now_();
        map_[key] = e;
        order_.push_front(key);
        while (map_.size() > capacity_ && !order_.empty()) {
            const std::string victim = order_.back();
            if (pinned_.count(victim)) {   // never evict what is in flight; try the next oldest
                bool evicted = false;
                for (auto r = order_.rbegin(); r != order_.rend(); ++r) {
                    if (pinned_.count(*r)) continue;
                    const std::string k = *r;
                    erase(k);
                    evicted = true;
                    break;
                }
                if (!evicted) break;
                continue;
            }
            erase(victim);
        }
    }
    // Restarts an entry's life (a completed refresh).
    void restamp(const std::string &key) {
        auto it = map_.find(key);
        if (it != map_.end()) it->second.storedMs = now_();
    }
    void erase(const std::string &key) {
        if (!map_.erase(key)) return;
        order_.remove(key);
    }
    std::size_t size() const { return map_.size(); }

    // Single-flight bookkeeping.
    bool inFlight(const std::string &key) const { return pinned_.count(key) != 0; }
    void setInFlight(const std::string &key, bool on) { if (on) pinned_.insert(key); else pinned_.erase(key); }

private:
    struct Entry {
        std::shared_ptr<V> value;
        std::int64_t storedMs = 0;
    };
    void touch(const std::string &key) {
        order_.remove(key);
        order_.push_front(key);
    }
    Clock now_;
    std::int64_t ttl_;
    std::size_t capacity_;
    std::map<std::string, Entry> map_;
    std::list<std::string> order_;
    std::set<std::string> pinned_;
};

}}}
