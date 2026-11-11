// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

// Search and suggestion caches and the infinite-scroll decision. Qt-free, injectable clock.
//
//  * SearchCache (DSC-10): per (query, filters, kind) for 10 min, at most 8 queries (least
//    recently used evicted). Page 1 is reused only while younger than 2 min; deeper pages stay
//    while the entry lives, so scrolling back over them sends no request and only a new
//    continuation is fetched.
//  * SuggestCache (DSC-11): 10 min per exact query, 128 entries. Callers never store failures.
//  * SearchPaging (DSC-09): paging stops only without a continuation token. A page that adds no
//    visible rows after filterRows (Shorts, members-only, duplicates) fetches the next one
//    automatically, up to 5 automatic fetches in a row; then it waits for the user (the list
//    keeps its continuation, so a scroll or "more" goes on).

#include "Item.h"
#include "SearchFilter.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <list>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace nordtile { namespace video { namespace catalog {

// Milliseconds on a monotonic clock. Default: std::chrono::steady_clock.
using CatalogClock = std::function<std::int64_t()>;
std::int64_t steadyNowMs();

// A small LRU map whose entries expire `lifetimeMs` after they were stored.
template <typename V>
class ExpiringLru {
public:
    ExpiringLru(std::size_t capacity, std::int64_t lifetimeMs, CatalogClock now)
        : capacity_(capacity), lifetimeMs_(lifetimeMs), now_(now ? now : CatalogClock(steadyNowMs)) {}

    // The live value (touching it), or nullptr.
    V *find(const std::string &key) {
        auto it = index_.find(key);
        if (it == index_.end()) return nullptr;
        if (now_() - it->second->storedMs >= lifetimeMs_) { order_.erase(it->second); index_.erase(it); return nullptr; }
        order_.splice(order_.begin(), order_, it->second);
        return &it->second->value;
    }
    // Inserts or replaces (restarting the lifetime) and evicts the least recently used entry.
    V &put(const std::string &key, const V &value) {
        auto it = index_.find(key);
        if (it != index_.end()) { order_.erase(it->second); index_.erase(it); }
        Node n;
        n.key = key; n.value = value; n.storedMs = now_();
        order_.push_front(n);
        index_[key] = order_.begin();
        while (order_.size() > capacity_) { index_.erase(order_.back().key); order_.pop_back(); }
        return order_.front().value;
    }
    bool contains(const std::string &key) const { return index_.count(key) != 0; }
    std::int64_t ageMs(const std::string &key) const {
        auto it = index_.find(key);
        return it == index_.end() ? -1 : now_() - it->second->storedMs;
    }
    std::size_t size() const { return order_.size(); }
    void clear() { order_.clear(); index_.clear(); }
    std::int64_t now() const { return now_(); }

private:
    struct Node { std::string key; V value; std::int64_t storedMs; };
    std::size_t capacity_;
    std::int64_t lifetimeMs_;
    CatalogClock now_;
    std::list<Node> order_;   // front = most recently used
    std::map<std::string, typename std::list<Node>::iterator> index_;
};

struct CachedSearchPage {
    CachedSearchPage() : fetchedAtMs(-1), fallback(false) {}
    std::vector<Item> rows;       // unfiltered rows as parsed
    std::string continuation;     // token for the following page ("" = last page)
    std::int64_t fetchedAtMs;
    bool fallback;                // rows came from the second tier
};

class SearchCache {
public:
    static const std::int64_t kLifetimeMs = 10LL * 60 * 1000;
    static const std::int64_t kPageOneReuseMs = 2LL * 60 * 1000;
    static const std::size_t kMaxQueries = 8;

    explicit SearchCache(CatalogClock now = CatalogClock());

    // `kind` is the result mode ("mixed" or "videos").
    static std::string key(const std::string &query, const SearchFilter &filter, const std::string &kind);

    // Page `index` (0 = page 1) of a live entry, or nullptr. Page 1 only while < 2 min old.
    const CachedSearchPage *page(const std::string &key, std::size_t index);
    // Page 1 starts a new entry (its older deeper pages are dropped). A deeper page is kept only
    // when it extends the cached chain (index == number of cached pages).
    void store(const std::string &key, std::size_t index, const CachedSearchPage &page);

    std::size_t pageCount(const std::string &key);
    bool contains(const std::string &key);
    std::size_t size() const { return entries_.size(); }
    void clear() { entries_.clear(); }

private:
    struct Entry { std::vector<CachedSearchPage> pages; };
    ExpiringLru<Entry> entries_;
};

class SuggestCache {
public:
    static const std::int64_t kLifetimeMs = 10LL * 60 * 1000;
    static const std::size_t kMaxEntries = 128;

    explicit SuggestCache(CatalogClock now = CatalogClock());
    bool lookup(const std::string &query, std::vector<std::string> *out);
    void store(const std::string &query, const std::vector<std::string> &suggestions);
    std::size_t size() const { return entries_.size(); }
    void clear() { entries_.clear(); }

private:
    ExpiringLru<std::vector<std::string>> entries_;
};

enum class PagingStep {
    Stop,       // no continuation token: the list is complete
    Wait,       // wait for the user to scroll (the page added rows, or the auto-fetch cap was hit)
    FetchNext   // the page added no visible rows: fetch the next one now
};

class SearchPaging {
public:
    static const int kMaxEmptyPagesInARow = 5;

    SearchPaging() : streak_(0) {}
    void reset() { streak_ = 0; }
    // A page the user asked for (scroll / "more"): the empty-page allowance starts again.
    void userRequested() { streak_ = 0; }
    PagingStep afterPage(int visibleRowsAdded, bool hasContinuation);
    // Automatic fetches made in a row because pages added nothing.
    int emptyStreak() const { return streak_; }
    bool capped() const { return streak_ >= kMaxEmptyPagesInARow; }

private:
    int streak_;
};

}}}
