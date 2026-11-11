// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Qt-free checks of the RES-10 resolve cache (Core/Player/Domain/ResolveCache): freshness,
// capacity, never failures or live answers, single flight with a bounded join, invalidate, peek;
// and of the RES-07 / RES-12 fault bookkeeping kept beside it (SailTune's strike rules:
// SourceFault, VideoDeadlines, FaultLedger).
// The interface, key registry, RES-09 table and the yt-dlp mapping are Qt Core and live in
// MappingTest.cpp of this suite. Build: tests/resolve_cache/run.sh
#include "Core/Player/Domain/ResolveCache.h"

#include <cstdio>
#include <string>
#include <utility>
#include <vector>

using namespace nordtile;

static int checks = 0;
static int failures = 0;
#define CHECK(c) do { ++checks; if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

static const std::int64_t T0 = 1790000000000LL; // ms
static const char *const vid = "aBcDeFgHiJk";

// A stand-in for the stored answer: the cache only copies it.
struct Answer {
    std::string videoId;
    int formats = 0;
};
typedef player::ResolveCache<Answer> Cache;

static player::CacheFacts facts(const std::string &id, std::int64_t expireSeconds) {
    player::CacheFacts f;
    f.videoId = id;
    f.expiresAtUnixMs = expireSeconds * 1000;
    return f;
}

static Answer answer(const std::string &id) {
    Answer a;
    a.videoId = id;
    a.formats = 1;
    return a;
}

static void res10_freshness() {
    const std::int64_t now = T0;
    // Far expire: capped at now + 20 min.
    CHECK(player::cacheableUntilMs(facts(vid, now / 1000 + 6 * 3600), now) == now + 20 * 60 * 1000);
    // Near expire: expire - 120 s.
    CHECK(player::cacheableUntilMs(facts(vid, now / 1000 + 600), now) == now + 480 * 1000);
    // Stored only if valid for more than 5 s.
    CHECK(player::cacheableUntilMs(facts(vid, now / 1000 + 126), now) == now + 6000);
    CHECK(player::cacheableUntilMs(facts(vid, now / 1000 + 125), now) == 0);
    CHECK(player::cacheableUntilMs(facts(vid, now / 1000 + 60), now) == 0);
    // No expire known: 5 min.
    CHECK(player::cacheableUntilMs(facts(vid, 0), now) == now + 5 * 60 * 1000);
    // HLS: 5 min even with a far expire.
    player::CacheFacts hls = facts(vid, now / 1000 + 6 * 3600);
    hls.hls = true;
    CHECK(player::cacheableUntilMs(hls, now) == now + 5 * 60 * 1000);
    // Live answers are never cached.
    player::CacheFacts live = hls;
    live.live = true;
    CHECK(player::cacheableUntilMs(live, now) == 0);
}

static void res10_neverCachesFailuresOrLive() {
    Cache cache;
    Answer out;
    CHECK(cache.acquire("k", 1, T0, &out) == Cache::Outcome::Lead);
    CHECK(cache.finish("k", nullptr, facts(vid, 0), T0 + 10) == std::vector<std::uint64_t>{1});
    CHECK(cache.size() == 0);
    CHECK(cache.acquire("k", 2, T0 + 20, &out) == Cache::Outcome::Lead);
    player::CacheFacts live = facts(vid, 0);
    live.live = true;
    const Answer a = answer(vid);
    cache.finish("k", &a, live, T0 + 30);
    CHECK(cache.size() == 0);
    CHECK(!cache.inFlight("k"));
}

static void res10_capacity24() {
    Cache cache;
    for (int i = 0; i < 30; ++i) {
        const std::string key = "k" + std::to_string(i);
        cache.acquire(key, static_cast<std::uint64_t>(i + 1), T0 + i, nullptr);
        const Answer a = answer(vid);
        cache.finish(key, &a, facts(vid, 0), T0 + i);
    }
    CHECK(cache.size() == 24);
    Answer out;
    // The oldest entries went first.
    CHECK(cache.acquire("k0", 100, T0 + 40, &out) == Cache::Outcome::Lead);
    CHECK(cache.acquire("k29", 101, T0 + 40, &out) == Cache::Outcome::Hit);
}

static void res10_hitUntilFreshnessEnds() {
    Cache cache;
    const Answer a = answer(vid);
    cache.acquire("k", 1, T0, nullptr);
    cache.finish("k", &a, facts(vid, T0 / 1000 + 600), T0); // valid until T0 + 480 s
    Answer out;
    CHECK(cache.acquire("k", 2, T0 + 479 * 1000, &out) == Cache::Outcome::Hit);
    CHECK(out.videoId == vid && out.formats == 1);
    CHECK(cache.acquire("k", 3, T0 + 480 * 1000, &out) == Cache::Outcome::Lead);
    CHECK(cache.size() == 0);
}

static void res10_singleFlightBoundedJoin() {
    player::CachePolicy p;
    p.joinLimitMs = 1000;
    Cache cache(p);
    Answer out;
    CHECK(cache.acquire("k", 1, T0, &out) == Cache::Outcome::Lead);
    CHECK(cache.acquire("k", 2, T0 + 100, &out) == Cache::Outcome::Join);
    CHECK(cache.acquire("k", 3, T0 + 900, &out) == Cache::Outcome::Join);
    CHECK(cache.acquire("other", 4, T0 + 900, &out) == Cache::Outcome::Lead);
    // Joiner 2 waited past the bound; the leader never times out here.
    CHECK(cache.overdueJoiners(T0 + 1101) == std::vector<std::uint64_t>{2});
    CHECK(cache.overdueJoiners(T0 + 1101).empty());
    const Answer a = answer(vid);
    CHECK((cache.finish("k", &a, facts(vid, 0), T0 + 1200) == std::vector<std::uint64_t>{1, 3}));
    CHECK(cache.acquire("k", 5, T0 + 1300, &out) == Cache::Outcome::Hit);
    CHECK(cache.finish("k", &a, facts(vid, 0), T0 + 1300).empty()); // no flight: nothing to answer

    // Releasing the leader keeps the flight for its joiner; releasing the last waiter closes it.
    CHECK(cache.acquire("j", 10, T0, &out) == Cache::Outcome::Lead);
    CHECK(cache.acquire("j", 11, T0, &out) == Cache::Outcome::Join);
    CHECK(!cache.release(10));
    CHECK(cache.inFlight("j"));
    CHECK(cache.release(11));
    CHECK(!cache.inFlight("j"));
    CHECK(!cache.release(11));
    CHECK(cache.acquire("j", 12, T0, &out) == Cache::Outcome::Lead);
}

static void res10_invalidateDropsVideo() {
    Cache cache;
    const Answer a = answer(vid), b = answer("zzzzzzzzzzz");
    cache.acquire("a1", 1, T0, nullptr); cache.finish("a1", &a, facts(vid, 0), T0);
    cache.acquire("a2", 2, T0, nullptr); cache.finish("a2", &a, facts(vid, 0), T0);
    cache.acquire("b1", 3, T0, nullptr); cache.finish("b1", &b, facts("zzzzzzzzzzz", 0), T0);
    CHECK(cache.size() == 3);
    cache.invalidate(vid);
    CHECK(cache.size() == 1);
    Answer out;
    CHECK(cache.acquire("b1", 4, T0, &out) == Cache::Outcome::Hit);
    cache.clear();
    CHECK(cache.size() == 0);
}

// RES-10 / RES-11: peek is a read. A valid entry is returned; an expired or missing one is not;
// nothing is pruned, no flight is opened and no waiter is registered.
static void res10_peekReadsWithoutChanging() {
    Cache cache;
    const Answer a = answer(vid);
    cache.acquire("k", 1, T0, nullptr);
    cache.finish("k", &a, facts(vid, T0 / 1000 + 600), T0); // valid until T0 + 480 s
    Answer out;
    CHECK(cache.peek("k", T0 + 479 * 1000, &out) && out.videoId == vid && out.formats == 1);
    CHECK(cache.peek("k", T0 + 479 * 1000, nullptr));
    CHECK(!cache.peek("k", T0 + 480 * 1000, &out));   // expired: not returned ...
    CHECK(cache.size() == 1);                          // ... and not pruned
    CHECK(!cache.peek("other", T0, &out));
    CHECK(!cache.inFlight("k") && !cache.inFlight("other"));   // no flight was opened
    // The entry is still what acquire() finds; a key that is only being resolved has no answer yet.
    CHECK(cache.acquire("k", 2, T0 + 100, &out) == Cache::Outcome::Hit);
    CHECK(cache.acquire("flying", 3, T0, &out) == Cache::Outcome::Lead);
    CHECK(!cache.peek("flying", T0, &out) && cache.inFlight("flying"));
}

// RES-07: which faults are no verdict on the source.
static void res07_faultsThatAreNotTheSource() {
    using player::SourceFault;
    CHECK(player::notTheSource(SourceFault::Connection));
    CHECK(player::notTheSource(SourceFault::Stalled));
    CHECK(player::notTheSource(SourceFault::Abandoned));
    CHECK(!player::notTheSource(SourceFault::Rejected));
    CHECK(!player::notTheSource(SourceFault::Unspecified));
}

// "Treated differently for a while": deadlines per video, bounded, pruned when read.
static void res07_videoDeadlines() {
    player::VideoDeadlines d(2);
    CHECK(!d.active("a", T0) && d.activeCount(T0) == 0);
    d.set("a", T0 + 1000);
    d.set("b", T0 + 5000);
    CHECK(d.active("a", T0) && d.active("a", T0 + 999) && d.activeCount(T0) == 2);
    CHECK(!d.active("a", T0 + 1000));                 // the deadline itself is over
    CHECK(d.activeCount(T0 + 1000) == 1 && d.active("b", T0 + 1000));
    // Full: the entry that ends first goes; setting a known video only moves its deadline.
    d.set("a", T0 + 2000);
    d.set("a", T0 + 9000);
    CHECK(d.activeCount(T0) == 2);
    d.set("c", T0 + 7000);                            // "b" (ends at +5000) goes
    CHECK(!d.active("b", T0) && d.active("a", T0) && d.active("c", T0));
    d.erase("a");
    CHECK(!d.active("a", T0) && d.activeCount(T0) == 1);
    player::VideoDeadlines one(0);                    // never less than one entry
    one.set("x", T0 + 10);
    one.set("y", T0 + 10);
    CHECK(one.activeCount(T0) == 1 && one.active("y", T0));
}

// RES-07 / RES-12, SailTune's rule 1: a refused source within five minutes of a fast answer is a
// strike (with the time the addresses had left) and the video avoids the fast path for a while.
static void res07_refusedIsAStrikeAndAvoidsTheFastPath() {
    using player::FaultVerdict;
    using player::SourceFault;
    player::FaultLedger ledger;
    const std::int64_t minute = 60 * 1000;
    CHECK(ledger.policy().deliveryWindowMs == 5 * minute && ledger.policy().avoidMs == 5 * minute
          && ledger.policy().unexplainedWindowMs == 5 * minute && ledger.policy().maxVideos == 32);
    // Nothing delivered by the fast path: nothing to judge.
    std::int64_t left = -1;
    CHECK(ledger.report(vid, SourceFault::Rejected, T0, &left) == FaultVerdict::NotCounted && left == -1);
    CHECK(!ledger.avoidsFast(vid, T0));

    ledger.fastDelivered(vid, T0, T0 + 6 * 60 * minute);
    CHECK(ledger.report(vid, SourceFault::Rejected, T0 + 2 * minute, &left) == FaultVerdict::Strike);
    CHECK(left == 6 * 60 * minute - 2 * minute);
    CHECK(ledger.avoidsFast(vid, T0 + 2 * minute) && ledger.avoidedCount(T0 + 2 * minute) == 1);
    CHECK(ledger.avoidsFast(vid, T0 + 7 * minute - 1));
    CHECK(!ledger.avoidsFast(vid, T0 + 7 * minute) && ledger.avoidedCount(T0 + 7 * minute) == 0);
    // One delivered answer is judged once.
    CHECK(ledger.report(vid, SourceFault::Rejected, T0 + 3 * minute) == FaultVerdict::NotCounted);

    // Outside the delivery window, or delivered by the extractor last: not the fast path's.
    ledger.fastDelivered(vid, T0, 0);
    CHECK(ledger.report(vid, SourceFault::Rejected, T0 + 5 * minute, &left) == FaultVerdict::NotCounted);
    ledger.fastDelivered(vid, T0, 0);
    ledger.extractorDelivered(vid);
    CHECK(ledger.report(vid, SourceFault::Rejected, T0 + 1000) == FaultVerdict::NotCounted);
    // Unknown or past expiry: no time left; a clock that went back is not a verdict.
    ledger.fastDelivered(vid, T0, 0);
    left = -1;
    CHECK(ledger.report(vid, SourceFault::Rejected, T0 + 1000, &left) == FaultVerdict::Strike && left == 0);
    ledger.fastDelivered(vid, T0, T0 + 500);
    CHECK(ledger.report(vid, SourceFault::Rejected, T0 + 1000, &left) == FaultVerdict::Strike && left == 0);
    ledger.fastDelivered(vid, T0, 0);
    CHECK(ledger.report(vid, SourceFault::Rejected, T0 - 1000) == FaultVerdict::NotCounted);
    // A refusal the caller saw itself.
    player::FaultLedger own;
    own.avoidFast("other", T0);
    CHECK(own.avoidsFast("other", T0 + 5 * minute - 1) && !own.avoidsFast("other", T0 + 5 * minute));
}

// SailTune's rule 2: a connection loss, a stall or an abandoned opening is never a strike and
// never counted; the fast path stays.
static void res07_connectionLossAndStallAreNotStrikes() {
    using player::FaultVerdict;
    using player::SourceFault;
    for (SourceFault fault : {SourceFault::Connection, SourceFault::Stalled, SourceFault::Abandoned}) {
        player::FaultLedger ledger;
        for (int i = 0; i < 6; ++i) {
            ledger.fastDelivered(vid, T0 + i * 1000, T0 + 3600 * 1000);
            CHECK(ledger.report(vid, fault, T0 + i * 1000 + 500) == FaultVerdict::NotCounted);
        }
        CHECK(!ledger.avoidsFast(vid, T0 + 10000) && ledger.avoidedCount(T0 + 10000) == 0);
        // And they do not count towards the unexplained pair either.
        ledger.fastDelivered(vid, T0 + 20000, 0);
        CHECK(ledger.report(vid, SourceFault::Unspecified, T0 + 21000) == FaultVerdict::RetryFast);
    }
}

// SailTune's rule 3: an unexplained failure retries through the fast path once; the second one
// within five minutes sends the video to the extractor for a while. Never a strike.
static void res07_unexplainedRetriesOnceThenAvoids() {
    using player::FaultVerdict;
    using player::SourceFault;
    const std::int64_t minute = 60 * 1000;
    player::FaultLedger ledger;
    ledger.fastDelivered(vid, T0, 0);
    CHECK(ledger.report(vid, SourceFault::Unspecified, T0 + 1000) == FaultVerdict::RetryFast);
    CHECK(!ledger.avoidsFast(vid, T0 + 1000));
    CHECK(ledger.report(vid, SourceFault::Unspecified, T0 + 2000) == FaultVerdict::NotCounted);  // nothing delivered since
    ledger.fastDelivered(vid, T0 + 3000, 0);                                                    // the retry was delivered
    CHECK(ledger.report(vid, SourceFault::Unspecified, T0 + 4 * minute) == FaultVerdict::AvoidFast);
    CHECK(ledger.avoidsFast(vid, T0 + 4 * minute) && !ledger.avoidsFast(vid, T0 + 9 * minute));
    // After the avoidance the count starts again.
    ledger.fastDelivered(vid, T0 + 10 * minute, 0);
    CHECK(ledger.report(vid, SourceFault::Unspecified, T0 + 10 * minute + 1) == FaultVerdict::RetryFast);
    // Two failures more than five minutes apart are two first failures.
    ledger.fastDelivered(vid, T0 + 16 * minute, 0);
    CHECK(ledger.report(vid, SourceFault::Unspecified, T0 + 16 * minute + 1) == FaultVerdict::RetryFast);
    CHECK(!ledger.avoidsFast(vid, T0 + 16 * minute + 1));
    // Other videos are judged on their own.
    ledger.fastDelivered("other", T0 + 16 * minute, 0);
    CHECK(ledger.report("other", SourceFault::Unspecified, T0 + 16 * minute + 2) == FaultVerdict::RetryFast);
}

// Bounded memory: at most maxVideos delivery records, unexplained counts and avoided videos.
static void res07_ledgerIsBounded() {
    using player::FaultVerdict;
    using player::SourceFault;
    player::FaultPolicy policy;
    policy.maxVideos = 2;
    policy.avoidMs = 1000;
    player::FaultLedger ledger(policy);
    ledger.fastDelivered("a", T0, 0);
    ledger.fastDelivered("b", T0 + 1, 0);
    ledger.fastDelivered("c", T0 + 2, 0);        // "a", the oldest delivery, is forgotten
    CHECK(ledger.report("a", SourceFault::Rejected, T0 + 10) == FaultVerdict::NotCounted);
    CHECK(ledger.report("b", SourceFault::Rejected, T0 + 10) == FaultVerdict::Strike);
    CHECK(ledger.report("c", SourceFault::Rejected, T0 + 20) == FaultVerdict::Strike);
    ledger.avoidFast("d", T0 + 30);              // "b" (ends first) leaves the avoided set
    CHECK(!ledger.avoidsFast("b", T0 + 40) && ledger.avoidsFast("c", T0 + 40) && ledger.avoidsFast("d", T0 + 40));
    CHECK(ledger.avoidedCount(T0 + 40) == 2 && ledger.avoidedCount(T0 + 2000) == 0);
    // Unexplained counts: the oldest goes, so its next failure is a first one again.
    player::FaultLedger counts(policy);
    const char *const ids[] = {"p", "q", "r"};
    for (int i = 0; i < 3; ++i) {
        counts.fastDelivered(ids[i], T0 + i, 0);
        CHECK(counts.report(ids[i], SourceFault::Unspecified, T0 + 10 + i) == FaultVerdict::RetryFast);
    }
    counts.fastDelivered("p", T0 + 100, 0);
    CHECK(counts.report("p", SourceFault::Unspecified, T0 + 101) == FaultVerdict::RetryFast);
    counts.fastDelivered("r", T0 + 100, 0);
    CHECK(counts.report("r", SourceFault::Unspecified, T0 + 102) == FaultVerdict::AvoidFast);
    player::FaultPolicy none;
    none.maxVideos = 0;                           // never less than one
    player::FaultLedger tiny(none);
    tiny.fastDelivered("a", T0, 0);
    CHECK(tiny.report("a", SourceFault::Rejected, T0 + 1) == FaultVerdict::Strike && tiny.policy().maxVideos == 1);
}

// The sink interface: a resolver that takes typed faults.
static void res07_sourceFaultSink() {
    struct Recorder : player::SourceFaultSink {
        std::vector<std::pair<std::string, player::SourceFault> > seen;
        void reportFault(const std::string &videoId, player::SourceFault fault) override { seen.push_back(std::make_pair(videoId, fault)); }
    } recorder;
    player::SourceFaultSink *sink = &recorder;
    sink->reportFault(vid, player::SourceFault::Stalled);
    CHECK(recorder.seen.size() == 1 && recorder.seen[0].first == vid && recorder.seen[0].second == player::SourceFault::Stalled);
}

int main() {
    res10_freshness();
    res10_neverCachesFailuresOrLive();
    res10_capacity24();
    res10_hitUntilFreshnessEnds();
    res10_singleFlightBoundedJoin();
    res10_invalidateDropsVideo();
    res10_peekReadsWithoutChanging();
    res07_faultsThatAreNotTheSource();
    res07_videoDeadlines();
    res07_refusedIsAStrikeAndAvoidsTheFastPath();
    res07_connectionLossAndStallAreNotStrikes();
    res07_unexplainedRetriesOnceThenAvoids();
    res07_ledgerIsBounded();
    res07_sourceFaultSink();
    std::printf("resolve cache tests: %d checks, %d failure%s\n", checks, failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
