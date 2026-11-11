// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

// Which videos have no 1280x720 thumbnail (hq720.jpg). The `thumbnailLarge` role of the list
// models names hq720.jpg for a video unless it is listed here, in which case it names
// hqdefault.jpg (480x360), which exists for every video. A video gets listed
//  * when the image loader was answered 404 for its hq720.jpg (the image request itself; the
//    loader remembers these on disk, so it happens once per video), or
//  * when an answer whose rows name hq720.jpg names only smaller pictures for it.
// No request is made to find out. Process-wide, bounded (oldest entries leave first),
// thread-safe (the loader and the parsers run off the main thread), header-only and Qt-free.

#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_set>
#include <vector>

namespace nordtile { namespace video { namespace catalog {

class ThumbnailBook {
public:
    static const std::size_t kCapacity = 4000;

    static ThumbnailBook &instance() {
        static ThumbnailBook book;
        return book;
    }

    static bool isVideoId(const std::string &id) {
        if (id.size() != 11) return false;
        for (char c : id) {
            const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
            if (!ok) return false;
        }
        return true;
    }

    // Returns true when the video was not listed yet; the listener (if any) is then called on
    // the caller's thread, outside the lock.
    bool markLargeMissing(const std::string &videoId) {
        if (!isVideoId(videoId)) return false;
        std::function<void()> notify;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!missing_.insert(videoId).second) return false;
            order_.push_back(videoId);
            while (order_.size() > kCapacity) {
                missing_.erase(order_.front());
                order_.pop_front();
            }
            ++revision_;
            notify = listener_;
        }
        if (notify) notify();
        return true;
    }

    bool largeMissing(const std::string &videoId) const {
        std::lock_guard<std::mutex> lock(mutex_);
        return missing_.count(videoId) != 0;
    }

    // Oldest first (what the loader keeps on disk).
    std::vector<std::string> missing() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return std::vector<std::string>(order_.begin(), order_.end());
    }

    std::size_t size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return missing_.size();
    }

    std::uint64_t revision() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return revision_;
    }

    void setListener(const std::function<void()> &listener) {
        std::lock_guard<std::mutex> lock(mutex_);
        listener_ = listener;
    }

    void clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        missing_.clear();
        order_.clear();
        ++revision_;
    }

private:
    ThumbnailBook() {}
    mutable std::mutex mutex_;
    std::unordered_set<std::string> missing_;
    std::deque<std::string> order_;
    std::uint64_t revision_ = 0;
    std::function<void()> listener_;
};

}}}
