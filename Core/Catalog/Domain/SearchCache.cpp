// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "SearchCache.h"

#include <chrono>

namespace nordtile { namespace video { namespace catalog {

const std::int64_t SearchCache::kLifetimeMs;
const std::int64_t SearchCache::kPageOneReuseMs;
const std::size_t SearchCache::kMaxQueries;
const std::int64_t SuggestCache::kLifetimeMs;
const std::size_t SuggestCache::kMaxEntries;
const int SearchPaging::kMaxEmptyPagesInARow;

std::int64_t steadyNowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

SearchCache::SearchCache(CatalogClock now) : entries_(kMaxQueries, kLifetimeMs, now) {}

std::string SearchCache::key(const std::string &query, const SearchFilter &filter, const std::string &kind) {
    return kind + '\x1f' + filter.key() + '\x1f' + query;
}

const CachedSearchPage *SearchCache::page(const std::string &key, std::size_t index) {
    Entry *e = entries_.find(key);
    if (!e || index >= e->pages.size()) return nullptr;
    const CachedSearchPage &p = e->pages[index];
    if (index == 0 && entries_.now() - p.fetchedAtMs >= kPageOneReuseMs) return nullptr;
    return &p;
}

void SearchCache::store(const std::string &key, std::size_t index, const CachedSearchPage &page) {
    CachedSearchPage p = page;
    if (p.fetchedAtMs < 0) p.fetchedAtMs = entries_.now();
    if (index == 0) {
        Entry fresh;
        fresh.pages.push_back(p);
        entries_.put(key, fresh);
        return;
    }
    Entry *e = entries_.find(key);
    if (!e || index != e->pages.size()) return;   // not an extension of the cached chain
    e->pages.push_back(p);
}

std::size_t SearchCache::pageCount(const std::string &key) {
    Entry *e = entries_.find(key);
    return e ? e->pages.size() : 0;
}

bool SearchCache::contains(const std::string &key) { return entries_.find(key) != nullptr; }

SuggestCache::SuggestCache(CatalogClock now) : entries_(kMaxEntries, kLifetimeMs, now) {}

bool SuggestCache::lookup(const std::string &query, std::vector<std::string> *out) {
    std::vector<std::string> *v = entries_.find(query);
    if (!v) return false;
    if (out) *out = *v;
    return true;
}

void SuggestCache::store(const std::string &query, const std::vector<std::string> &suggestions) {
    entries_.put(query, suggestions);
}

PagingStep SearchPaging::afterPage(int visibleRowsAdded, bool hasContinuation) {
    if (!hasContinuation) return PagingStep::Stop;
    if (visibleRowsAdded > 0) { streak_ = 0; return PagingStep::Wait; }
    // Each empty page triggers one automatic fetch, at most kMaxEmptyPagesInARow in a row.
    if (streak_ >= kMaxEmptyPagesInARow) return PagingStep::Wait;
    ++streak_;
    return PagingStep::FetchNext;
}

}}}
