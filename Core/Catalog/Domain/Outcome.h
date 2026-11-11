// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

// Result classes for every discovery call (DSC-01) and the counters behind DIA-01.
// Only ShapeDrift means "the parser needs an update"; HTTP, 429, 5xx, network and
// empty-but-valid answers are reported as what they are.

#include <cstdint>
#include <map>
#include <mutex>
#include <string>

namespace nordtile { namespace video { namespace catalog {

enum class Outcome {
    Ok,          // parsed with content
    ValidEmpty,  // the expected containers are present and hold nothing (zero hits, no uploads)
    ShapeDrift,  // the expected container markers are missing
    Http4xx,     // other 4xx (400 also triggers a client-identity re-scrape, DSC-02)
    RateLimited, // 429
    Http5xx,
    Network,     // DNS, TLS, timeout, connection reset
    Unavailable  // final answer for this item (deleted, private, terminated channel)
};

enum class Area { Search, Suggest, Home, Feed, Channel, WatchNext, Comments, Playlist, Captions, SponsorBlock, Dislikes, Images };

inline const char *outcomeName(Outcome o) {
    switch (o) {
    case Outcome::Ok: return "ok";
    case Outcome::ValidEmpty: return "empty";
    case Outcome::ShapeDrift: return "drift";
    case Outcome::Http4xx: return "http4xx";
    case Outcome::RateLimited: return "http429";
    case Outcome::Http5xx: return "http5xx";
    case Outcome::Network: return "network";
    case Outcome::Unavailable: return "unavailable";
    }
    return "?";
}

inline const char *areaName(Area a) {
    switch (a) {
    case Area::Search: return "search";
    case Area::Suggest: return "suggest";
    case Area::Home: return "home";
    case Area::Feed: return "feed";
    case Area::Channel: return "channel";
    case Area::WatchNext: return "watch-next";
    case Area::Comments: return "comments";
    case Area::Playlist: return "playlists";
    case Area::Captions: return "captions";
    case Area::SponsorBlock: return "sponsorblock";
    case Area::Dislikes: return "dislikes";
    case Area::Images: return "images";
    }
    return "?";
}

inline bool isFailure(Outcome o) { return o != Outcome::Ok && o != Outcome::ValidEmpty; }
inline bool countsAsDrift(Outcome o) { return o == Outcome::ShapeDrift; }

// Maps an HTTP status (0 = transport error) to a class. 2xx is Ok until a parser says otherwise.
inline Outcome outcomeForHttp(int status) {
    if (status <= 0) return Outcome::Network;
    if (status == 429) return Outcome::RateLimited;
    if (status >= 500) return Outcome::Http5xx;
    if (status >= 400) return Outcome::Http4xx;
    return Outcome::Ok;
}

// Thread-safe counters per (area, outcome) plus unknown renderer names (DSC-03). The
// diagnostics report (DIA-01/02, core component) reads a snapshot.
class CatalogCounters {
public:
    void count(Area area, Outcome outcome) {
        std::lock_guard<std::mutex> lock(mutex_);
        ++counts_[std::make_pair(area, outcome)];
    }
    void unknownRenderer(const std::string &name, int n = 1) {
        std::lock_guard<std::mutex> lock(mutex_);
        unknown_[name] += n;
    }
    std::int64_t value(Area area, Outcome outcome) const {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = counts_.find(std::make_pair(area, outcome));
        return it == counts_.end() ? 0 : it->second;
    }
    std::int64_t driftTotal() const {
        std::lock_guard<std::mutex> lock(mutex_);
        std::int64_t n = 0;
        for (const auto &kv : counts_) if (kv.first.second == Outcome::ShapeDrift) n += kv.second;
        return n;
    }
    std::map<std::pair<Area, Outcome>, std::int64_t> snapshot() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return counts_;
    }
    std::map<std::string, std::int64_t> unknownRenderers() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return unknown_;
    }
private:
    mutable std::mutex mutex_;
    std::map<std::pair<Area, Outcome>, std::int64_t> counts_;
    std::map<std::string, std::int64_t> unknown_;
};

}}}
