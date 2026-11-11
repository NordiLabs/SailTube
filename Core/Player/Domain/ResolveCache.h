// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

// RES-10 resolve cache and single-flight bookkeeping for the yt-dlp fallback adapter
// (Core/Player/Infrastructure/YtDlpStreamResolver). Pure: no Qt, no I/O, time passed in as
// milliseconds since the Unix epoch. The stored answer is a template parameter (the adapter
// stores playback::StreamSet, which is Qt Core); the cache only reads the CacheFacts the caller
// passes with it. SailTune's fast path keeps its own cache (Core/SharedPlayback).
//
// Also here, because both resolvers keep them next to their caches (RES-07, RES-12): the source
// fault vocabulary (SourceFault, SourceFaultSink) and the bookkeeping of SailTune's strike rules
// (FaultLedger, VideoDeadlines). Header-only, so no build recipe changes with them.

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace nordtile { namespace player {

struct CachePolicy {
    std::size_t maxEntries = 24;
    std::int64_t expiryMarginMs = 120 * 1000;     // earliest expire - 120 s
    std::int64_t maxLifetimeMs = 20 * 60 * 1000;  // now + 20 min
    std::int64_t hlsLifetimeMs = 5 * 60 * 1000;   // HLS answers
    std::int64_t unknownExpiryLifetimeMs = 5 * 60 * 1000; // no expire in any URL
    std::int64_t minValidityMs = 5 * 1000;        // store only if valid for more than this
    std::int64_t joinLimitMs = 120 * 1000;        // bounded join (above the extractor's 90 s)
};

// What freshness depends on, read from an answer by the caller.
struct CacheFacts {
    std::string videoId;              // invalidate() drops every entry of this id
    bool live = false;                // running livestream: never cached
    bool hls = false;                 // carries an HLS manifest: 5 min
    std::int64_t expiresAtUnixMs = 0; // earliest URL expire, 0 unknown
};

// RES-10 freshness: absolute time (ms) until which the answer may be served, or 0 when it must
// not be stored (live answers, or valid for minValidityMs or less).
std::int64_t cacheableUntilMs(const CacheFacts &, std::int64_t nowMs, const CachePolicy & = CachePolicy());

// RES-10: at most maxEntries answers (expired first, then oldest stored out), never failures or
// live answers, single flight per key with a bounded join, invalidate per video.
template <class Answer>
class ResolveCache {
public:
    enum class Outcome { Hit, Lead, Join };

    explicit ResolveCache(const CachePolicy &policy = CachePolicy()) : policy_(policy) {
        if (policy_.maxEntries < 1) policy_.maxEntries = 1;
    }

    // RES-10 lookup. Hit: *out (when non-null) receives the stored answer, the waiter is not
    // registered. Lead: no flight existed; one is opened with the waiter as its first member and
    // the caller starts the work. Join: the waiter waits for the running flight (bounded).
    Outcome acquire(const std::string &key, std::uint64_t waiter, std::int64_t nowMs, Answer *out) {
        auto e = entries_.find(key);
        if (e != entries_.end()) {
            if (e->second.validUntilMs > nowMs) {
                if (out) *out = e->second.answer;
                return Outcome::Hit;
            }
            entries_.erase(e);
        }
        Member m;
        m.waiter = waiter;
        m.joinedMs = nowMs;
        auto f = flights_.find(key);
        if (f != flights_.end()) {
            f->second.members.push_back(m);
            return Outcome::Join;
        }
        flights_[key].members.push_back(m);
        return Outcome::Lead;
    }

    // A read that changes nothing (no pruning, no flight, no waiter): the stored answer when it
    // is still valid, else false. For "is this already resolved?" questions (prefetch reuse).
    bool peek(const std::string &key, std::int64_t nowMs, Answer *out) const {
        const auto e = entries_.find(key);
        if (e == entries_.end() || e->second.validUntilMs <= nowMs) return false;
        if (out) *out = e->second.answer;
        return true;
    }

    // The work for key ended. A non-null answer is stored when cacheableUntilMs(facts) allows
    // (failures are passed as null and never stored). Returns the flight's waiters, leader
    // first, and closes the flight; empty when no flight exists (abandoned work).
    std::vector<std::uint64_t> finish(const std::string &key, const Answer *answer, const CacheFacts &facts,
                                      std::int64_t nowMs) {
        std::vector<std::uint64_t> waiters;
        auto f = flights_.find(key);
        if (f == flights_.end()) return waiters;
        for (const Member &m : f->second.members) waiters.push_back(m.waiter);
        flights_.erase(f);
        if (answer) store(key, *answer, facts, nowMs);
        return waiters;
    }

    // Withdraws one waiter. Returns true when its flight has no waiter left; the flight is then
    // closed and the caller should cancel the work.
    bool release(std::uint64_t waiter) {
        for (auto f = flights_.begin(); f != flights_.end(); ++f) {
            std::vector<Member> &members = f->second.members;
            for (auto m = members.begin(); m != members.end(); ++m) {
                if (m->waiter != waiter) continue;
                members.erase(m);
                if (!members.empty()) return false;
                flights_.erase(f);
                return true;
            }
        }
        return false;
    }

    // Joiners that waited longer than joinLimitMs, removed from their flights (RES-10 bounded
    // join). The leader (first member) owns the work and is bounded by the work's own timeout.
    std::vector<std::uint64_t> overdueJoiners(std::int64_t nowMs) {
        std::vector<std::uint64_t> overdue;
        for (auto &f : flights_) {
            std::vector<Member> &members = f.second.members;
            for (std::size_t i = members.size(); i-- > 1;) {
                if (nowMs - members[i].joinedMs <= policy_.joinLimitMs) continue;
                overdue.push_back(members[i].waiter);
                members.erase(members.begin() + static_cast<std::ptrdiff_t>(i));
            }
        }
        return overdue;
    }

    // Drops every stored answer of the video (byte-0 refusal, RES-10).
    void invalidate(const std::string &videoId) {
        for (auto it = entries_.begin(); it != entries_.end();) {
            if (it->second.videoId == videoId) it = entries_.erase(it);
            else ++it;
        }
    }
    void clear() { entries_.clear(); }

    std::size_t size() const { return entries_.size(); }
    bool inFlight(const std::string &key) const { return flights_.count(key) != 0; }
    const CachePolicy &policy() const { return policy_; }

private:
    struct Entry {
        std::string videoId;
        Answer answer;
        std::int64_t storedMs = 0;
        std::int64_t validUntilMs = 0;
    };
    struct Member {
        std::uint64_t waiter;
        std::int64_t joinedMs;
    };
    struct Flight {
        std::vector<Member> members; // members[0] is the leader while present
    };

    void store(const std::string &key, const Answer &answer, const CacheFacts &facts, std::int64_t nowMs) {
        const std::int64_t until = cacheableUntilMs(facts, nowMs, policy_);
        if (!until) return;
        for (auto it = entries_.begin(); it != entries_.end();) {
            if (it->second.validUntilMs <= nowMs) it = entries_.erase(it);
            else ++it;
        }
        entries_.erase(key);
        while (entries_.size() >= policy_.maxEntries) {
            auto oldest = entries_.begin();
            for (auto it = entries_.begin(); it != entries_.end(); ++it)
                if (it->second.storedMs < oldest->second.storedMs) oldest = it;
            entries_.erase(oldest);
        }
        Entry entry;
        entry.videoId = facts.videoId;
        entry.answer = answer;
        entry.storedMs = nowMs;
        entry.validUntilMs = until;
        entries_[key] = entry;
    }

    CachePolicy policy_;
    std::map<std::string, Entry> entries_;
    std::map<std::string, Flight> flights_;
};

// ---- Source faults (RES-07, RES-12; SailTune's strike rules) -----------------------------------

// What the consumer observed when a delivered source stopped working (the vocabulary of
// SailTune's StreamResolverPort::SourceFault). Only Rejected says that the source itself was
// refused; Unspecified is a failure nobody could explain. A lost connection, a stall, or an
// opening that was abandoned or ran out of time say nothing about the resolver.
enum class SourceFault { Unspecified, Rejected, Connection, Stalled, Abandoned };

// True for the faults that are no verdict on the source: never a strike, never counted.
inline bool notTheSource(SourceFault fault) {
    return fault == SourceFault::Connection || fault == SourceFault::Stalled || fault == SourceFault::Abandoned;
}

// A resolver that takes typed faults. Every fault drops the video's cached answers (like
// StreamResolver::invalidate); what else happens is the implementation's strike rule. Call it on
// the implementation's owning thread.
class SourceFaultSink {
public:
    virtual ~SourceFaultSink() {}
    virtual void reportFault(const std::string &videoId, SourceFault fault) = 0;
};

// Videos remembered until a deadline (Unix ms): "this video is treated differently for a while".
// At most `capacity` videos; when full, the one whose deadline ends first goes.
class VideoDeadlines {
public:
    explicit VideoDeadlines(std::size_t capacity = 32) : capacity_(capacity < 1 ? 1 : capacity) {}

    void set(const std::string &videoId, std::int64_t untilMs) {
        if (!until_.count(videoId) && until_.size() >= capacity_) {
            auto first = until_.begin();
            for (auto it = until_.begin(); it != until_.end(); ++it)
                if (it->second < first->second) first = it;
            until_.erase(first);
        }
        until_[videoId] = untilMs;
    }
    // True while the video's deadline lies ahead; an entry that ended is dropped.
    bool active(const std::string &videoId, std::int64_t nowMs) {
        const auto it = until_.find(videoId);
        if (it == until_.end()) return false;
        if (it->second > nowMs) return true;
        until_.erase(it);
        return false;
    }
    void erase(const std::string &videoId) { until_.erase(videoId); }
    // Entries whose deadline lies ahead (diagnostics; changes nothing).
    std::size_t activeCount(std::int64_t nowMs) const {
        std::size_t n = 0;
        for (const auto &e : until_) if (e.second > nowMs) ++n;
        return n;
    }

private:
    std::size_t capacity_;
    std::map<std::string, std::int64_t> until_;
};

struct FaultPolicy {
    std::int64_t deliveryWindowMs = 5 * 60 * 1000;    // a fault counts only this long after the fast path delivered
    std::int64_t unexplainedWindowMs = 5 * 60 * 1000; // the second unexplained failure within this leaves the fast path
    std::int64_t avoidMs = 5 * 60 * 1000;             // how long the video then stays with the extractor
    std::size_t maxVideos = 32;
};

// What a reported fault means for the fast path.
enum class FaultVerdict {
    NotCounted,   // no verdict on the source, or the fast path did not deliver this video lately
    RetryFast,    // unexplained, the first time: one more try through the fast path; no strike
    AvoidFast,    // unexplained twice: the extractor answers this video for a while; no strike
    Strike        // refused: a strike against the fast path, and the extractor answers this video
};

// SailTune's strike rules as a state machine (its FastFirstStreamResolver::invalidate):
//  * Rejected within deliveryWindowMs of a fast answer: Strike, and the video avoids the fast
//    path for avoidMs (a refused address is never produced twice by the same client);
//  * Connection, Stalled, Abandoned: NotCounted, the fast path stays;
//  * Unspecified within the window: RetryFast the first time, AvoidFast the second time within
//    unexplainedWindowMs; never a strike.
// Every report consumes the delivery record: one delivered answer is judged once.
class FaultLedger {
public:
    explicit FaultLedger(const FaultPolicy &policy = FaultPolicy())
        : policy_(policy), avoid_(policy.maxVideos) {
        if (policy_.maxVideos < 1) policy_.maxVideos = 1;
    }

    // The fast path delivered an answer for the video; its addresses live until expiresAtUnixMs
    // (0 unknown).
    void fastDelivered(const std::string &videoId, std::int64_t nowMs, std::int64_t expiresAtUnixMs) {
        if (!delivered_.count(videoId) && delivered_.size() >= policy_.maxVideos) {
            auto oldest = delivered_.begin();
            for (auto it = delivered_.begin(); it != delivered_.end(); ++it)
                if (it->second.atMs < oldest->second.atMs) oldest = it;
            delivered_.erase(oldest);
        }
        Delivered d;
        d.atMs = nowMs;
        d.expiresAtMs = expiresAtUnixMs;
        delivered_[videoId] = d;
    }
    // The extractor delivered the video last: a later failure is not the fast path's.
    void extractorDelivered(const std::string &videoId) { delivered_.erase(videoId); }

    // A fault of the video's delivered source. On Strike, *msLeft (when non-null) is how long the
    // delivered addresses still had to live (0 when unknown or over).
    FaultVerdict report(const std::string &videoId, SourceFault fault, std::int64_t nowMs, std::int64_t *msLeft = nullptr) {
        FaultVerdict verdict = FaultVerdict::NotCounted;
        const auto it = delivered_.find(videoId);
        if (it == delivered_.end()) return verdict;
        const Delivered d = it->second;
        delivered_.erase(it);
        if (notTheSource(fault) || nowMs - d.atMs >= policy_.deliveryWindowMs || nowMs < d.atMs) return verdict;
        if (fault == SourceFault::Rejected) {
            if (msLeft) *msLeft = d.expiresAtMs > nowMs ? d.expiresAtMs - nowMs : 0;
            avoidFast(videoId, nowMs);
            return FaultVerdict::Strike;
        }
        Unexplained &u = unexplained_[videoId];
        u.count = u.count > 0 && nowMs - u.lastMs <= policy_.unexplainedWindowMs && nowMs >= u.lastMs ? u.count + 1 : 1;
        u.lastMs = nowMs;
        verdict = FaultVerdict::RetryFast;
        if (u.count >= 2) {
            avoidFast(videoId, nowMs);
            verdict = FaultVerdict::AvoidFast;
        }
        while (unexplained_.size() > policy_.maxVideos) {
            auto oldest = unexplained_.begin();
            for (auto e = unexplained_.begin(); e != unexplained_.end(); ++e)
                if (e->second.lastMs < oldest->second.lastMs) oldest = e;
            unexplained_.erase(oldest);
        }
        return verdict;
    }

    // The video goes to the extractor for avoidMs from now (a refusal the caller saw itself, e.g.
    // of an address it knows to be the fast path's).
    void avoidFast(const std::string &videoId, std::int64_t nowMs) {
        unexplained_.erase(videoId);
        avoid_.set(videoId, nowMs + policy_.avoidMs);
    }
    // True while the video must not be asked from the fast path.
    bool avoidsFast(const std::string &videoId, std::int64_t nowMs) { return avoid_.active(videoId, nowMs); }
    std::size_t avoidedCount(std::int64_t nowMs) const { return avoid_.activeCount(nowMs); }
    const FaultPolicy &policy() const { return policy_; }

private:
    struct Delivered { std::int64_t atMs = 0; std::int64_t expiresAtMs = 0; };
    struct Unexplained { int count = 0; std::int64_t lastMs = 0; };
    FaultPolicy policy_;
    std::map<std::string, Delivered> delivered_;
    std::map<std::string, Unexplained> unexplained_;
    VideoDeadlines avoid_;
};

}}
