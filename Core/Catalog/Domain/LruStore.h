// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

// A recency-ordered map with a fixed capacity (DSC-17 stores, resolve cache). Writing or
// touching an entry makes it the most recent one; inserting past the capacity evicts the
// least recently used entry. Iteration order is most recent first. Qt-free, not thread-safe
// (owners serialise access).

#include <cstddef>
#include <list>
#include <unordered_map>
#include <utility>
#include <vector>

namespace nordtile { namespace video { namespace catalog {

template <typename K, typename V>
class LruStore {
public:
    using Entry = std::pair<K, V>;

    explicit LruStore(std::size_t capacity) : capacity_(capacity ? capacity : 1) {}

    std::size_t capacity() const { return capacity_; }
    std::size_t size() const { return order_.size(); }
    bool empty() const { return order_.empty(); }
    bool contains(const K &key) const { return index_.find(key) != index_.end(); }

    // Inserts or replaces, and makes the entry the most recent one. Returns the number of
    // entries evicted (0 or 1).
    std::size_t put(const K &key, const V &value) {
        auto it = index_.find(key);
        if (it != index_.end()) {
            it->second->second = value;
            order_.splice(order_.begin(), order_, it->second);
            return 0;
        }
        order_.emplace_front(key, value);
        index_[key] = order_.begin();
        std::size_t evicted = 0;
        while (order_.size() > capacity_) {
            index_.erase(order_.back().first);
            order_.pop_back();
            ++evicted;
        }
        return evicted;
    }

    // Reads and makes the entry the most recent one; nullptr when absent.
    const V *get(const K &key) {
        auto it = index_.find(key);
        if (it == index_.end()) return nullptr;
        order_.splice(order_.begin(), order_, it->second);
        return &it->second->second;
    }

    // Reads without changing the order; nullptr when absent.
    const V *peek(const K &key) const {
        auto it = index_.find(key);
        return it == index_.end() ? nullptr : &it->second->second;
    }

    bool erase(const K &key) {
        auto it = index_.find(key);
        if (it == index_.end()) return false;
        order_.erase(it->second);
        index_.erase(it);
        return true;
    }

    void clear() { order_.clear(); index_.clear(); }

    // Most recent first.
    typename std::list<Entry>::const_iterator begin() const { return order_.begin(); }
    typename std::list<Entry>::const_iterator end() const { return order_.end(); }
    std::vector<K> keys() const {
        std::vector<K> out;
        out.reserve(order_.size());
        for (const Entry &e : order_) out.push_back(e.first);
        return out;
    }

private:
    std::size_t capacity_;
    std::list<Entry> order_;
    std::unordered_map<K, typename std::list<Entry>::iterator> index_;
};

}}}
