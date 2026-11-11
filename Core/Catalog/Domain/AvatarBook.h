// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

// Channel avatars the app has already seen, by channel id (DSC-19; the YouTube-style list cards
// show a round avatar beside every row). Rows that carry no avatar of their own (RSS feed rows,
// a channel's own tab, playlist panels) look it up here. It is filled only from answers the app
// requested anyway (list rows that carry an owner avatar, channel headers, the watch page owner)
// and from the stored subscription records: no request is ever made for an avatar address.
// Images themselves load through the shared image cache, which accepts image hosts only.
//
// Process-wide, bounded (oldest entries leave first) and thread-safe: parsers learn on worker
// lanes, models read on the main thread. Header-only and Qt-free.

#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>

namespace nordtile { namespace video { namespace catalog {

class AvatarBook {
public:
    static const std::size_t kCapacity = 4000;

    static AvatarBook &instance() {
        static AvatarBook book;
        return book;
    }

    // "UC" + 22 characters, and an https address of sane length.
    static bool acceptable(const std::string &channelId, const std::string &avatarUrl) {
        return channelId.size() == 24 && channelId.compare(0, 2, "UC") == 0 &&
               avatarUrl.size() > 8 && avatarUrl.size() < 2048 && avatarUrl.compare(0, 8, "https://") == 0;
    }

    // Remembers an avatar. The first address seen for a channel stays (rows, the watch page owner
    // and feed answers name the same picture through different addresses, and flipping between
    // them would reload it); `replace` is for the channel's own page, which is authoritative.
    // Returns true when the entry is new or was replaced; the listener (if any) is then called on
    // the caller's thread, outside the lock.
    bool learn(const std::string &channelId, const std::string &avatarUrl, bool replace = false) {
        if (!acceptable(channelId, avatarUrl)) return false;
        std::function<void()> notify;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = entries_.find(channelId);
            if (it != entries_.end()) {
                if (!replace || it->second == avatarUrl) return false;
                it->second = avatarUrl;
            } else {
                entries_.emplace(channelId, avatarUrl);
                order_.push_back(channelId);
                while (order_.size() > kCapacity) {
                    entries_.erase(order_.front());
                    order_.pop_front();
                }
            }
            ++revision_;
            notify = listener_;
        }
        if (notify) notify();
        return true;
    }

    // "" when unknown.
    std::string lookup(const std::string &channelId) const {
        if (channelId.empty()) return std::string();
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = entries_.find(channelId);
        return it == entries_.end() ? std::string() : it->second;
    }

    std::size_t size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return entries_.size();
    }

    // Grows with every new or changed entry.
    std::uint64_t revision() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return revision_;
    }

    // Called after every change, on the thread that learned (may be a worker lane).
    void setListener(const std::function<void()> &listener) {
        std::lock_guard<std::mutex> lock(mutex_);
        listener_ = listener;
    }

    void clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        entries_.clear();
        order_.clear();
        ++revision_;
    }

private:
    AvatarBook() {}
    mutable std::mutex mutex_;
    std::unordered_map<std::string, std::string> entries_;
    std::deque<std::string> order_;
    std::uint64_t revision_ = 0;
    std::function<void()> listener_;
};

}}}
