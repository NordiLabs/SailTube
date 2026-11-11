// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

// yt-dlp fallback adapter: implements the shared playback::StreamResolver / StreamFetcher
// (Shared/Playback/StreamSource.h) over core's yt-dlp path (nordtile::video::resolver::
// YtDlpVideoService). It is the extractor that SharedStreamSource falls back to
// (ResolveOutcome.fallback, RES-08), and the whole resolver while SailTune's component is not
// pinned. It is not a second resolver: no InnerTube request, no proxy, no probe, no breaker. It
// names SailTube types, so it lives in nordtile::video::playeradapter, not nordtile::player.
//
// Behaviour:
// - resolve: core's fast extraction profile first (token-free: android_vr + visionos, RES-08 step
//   1); after a non-final failure or an answer that is not playable for the request, one pass with
//   the full JavaScript profile on yt-dlp's default clients (path ExtractorWithJavaScript). A bare
//   "video unavailable" is not final (one client may be told what another is not): it widens, and
//   its Unavailable error stays non-final. Rate limiting (TryLater) does not widen: a second pass
//   now meets the same limit. No cookies on any pass: core's runner has no cookie path (RES-20).
//   RES-14: StreamRequest::poProvider reaches each pass as DetailsOptions::poProvider (the
//   service adds the PO-token helper's arguments, FULL only); no pass forces a token.
// - nameFinal (RES-09): one token-free pass and its answer as it is; used to name a fast-path Final.
// - Strike rules (RES-07, RES-12; SailTune's, read from its committed StreamController and
//   resolvers): a video whose source was refused (reportRefused, or reportFault with Rejected) or
//   whose token-free pass just failed (with an answer: not for lack of network, time, a slot or
//   the runtime) starts with the full JavaScript profile for fullProfileMs: the client that
//   produced a refused address or no playable answer is not asked first again. A lost connection,
//   a stall or an abandoned opening (reportFault with Connection, Stalled, Abandoned), an
//   unexplained failure (Unspecified) and a plain invalidate() only drop the cached answers.
//   nameFinal is always the token-free pass.
// - Completions: asynchronous, on the owning thread, exactly once per operation, cancellation
//   included (class Cancelled); none after destruction.
// - RES-10 cache and single flight through player::ResolveCache (24 entries, freshness rules,
//   bounded join; failures and live answers never stored), keyed by playback::streamCacheKey.
// - RES-11 prefetch: at most 2 speculative resolves; a third cancels the oldest.
// - Answers are tagged with the requested video id; an extraction whose identity differs is a
//   failure (InvalidResponse), never another video's streams.

#include "../../../Shared/Playback/StreamSource.h"
#include "../../Resolver/Infrastructure/YtDlpVideoService.h"
#include "../Domain/ResolveCache.h"
#include <QObject>
#include <deque>
#include <functional>
#include <map>

namespace nordtile { namespace video { namespace playeradapter {

class YtDlpStreamResolver final : public QObject, public playback::StreamResolver, public player::SourceFaultSink {
public:
    using Clock = std::function<qint64()>; // Unix ms; default: the system clock
    // How long a recently refused or failed video starts with the full profile (SailTune: 5 min).
    static const qint64 fullProfileMs = 5 * 60 * 1000;
    // The service must outlive the resolver.
    explicit YtDlpStreamResolver(resolver::YtDlpVideoService *service, QObject *parent = nullptr,
                                 const player::CachePolicy &policy = player::CachePolicy(), Clock clock = Clock());
    ~YtDlpStreamResolver() override;

    quint64 resolve(const playback::StreamRequest &, Completion) override;
    // RES-09: why a video the fast path called final cannot be played: the token-free pass only
    // (never the widened pass, whatever it answers), under its own cache key.
    quint64 nameFinal(const playback::StreamRequest &, Completion) override;
    void prefetch(const playback::StreamRequest &) override;
    void cancel(quint64 operation) override;
    void invalidate(const QString &videoId) override;
    // RES-07 / RES-12: a source of the video was refused at byte 0: its cached answers go and its
    // next extractions start with the full profile for fullProfileMs.
    void reportRefused(const QString &videoId, const playback::StreamFormat &) override;
    // The same with what was observed: Rejected as reportRefused; every other fault as
    // invalidate (never a penalty for a connection loss, a stall or an abandoned opening).
    void reportFault(const std::string &videoId, player::SourceFault fault) override;
    // The cached answer for a request, if there is a valid one: no run, no flight, no counter;
    // nothing else is touched (the session's speculation and prefetch reuse).
    bool peekCached(const playback::StreamRequest &request, playback::StreamSet *out) const;
    // Keys (numbers only): "extractor", "extractorWithJavaScript", "failed", "cacheHits",
    // "cancelled", "medianExtractorMs" (-1 none), "cacheEntries", "startedFull" (flights that
    // skipped the token-free pass), "fullProfileVideos" (videos that would skip it now).
    QVariantMap diagnostics() const override;

    // RES-09 mapping of core's error codes (no provider text reaches the label). Without a reason,
    // VideoUnavailable is the bare (non-final) Unavailable.
    static playback::StreamError errorFor(resolver::ErrorCode, bool signedIn);
    // RES-09 with the run's finer reason: removed, terminated and country-blocked are final; a bare
    // unavailable is not; rate limiting is TryLater; the runtime's temporary-file budget is a
    // Failed with the label "storage_limit" (asking again meets the same budget); SABR-only /
    // failed-challenge answers are DeliveryChanged. Members-only is SignInRequired (final signed
    // out). code None gives no error (see unplayableError).
    static playback::StreamError errorFor(resolver::ErrorCode, resolver::FailureReason, bool signedIn);
    // RES-09 / RES-19: the class of a successful run whose set is not playable for the request:
    // Upcoming, StillProcessing, TryLater (rate limited), Failed (storage budget), SignInRequired
    // (members-only); formats with URLs that fit neither request nor device: NoPlayableStream; no
    // format with a URL: DeliveryChanged when the run shows it (the reason, or formatsWithoutUrl:
    // formats listed without a usable URL), else the bare (non-final) Unavailable.
    static playback::StreamError unplayableError(const playback::StreamSet &, resolver::FailureReason,
                                                 bool formatsWithoutUrl, bool signedIn);
    // The scrub-preview level of a details answer (yt-dlp's storyboard formats) as the stream set
    // carries it; an empty level gives an empty storyboard.
    static playback::StreamStoryboard storyboardOf(const resolver::StoryboardLevel &);
    static const std::size_t maximumPrefetches = 2;

private:
    struct Waiter {
        Completion completion; // empty for a prefetch
        QString videoId;
        std::string key;
        bool ready = false;
        playback::StreamSet streams;
        playback::StreamError error;
    };
    struct Flight {
        std::uint64_t token = 0;
        quint64 run = 0;
        bool fullProfile = false;
        bool singlePass = false;  // nameFinal: no widened pass
        playback::StreamRequest request;
        qint64 startedMs = 0;
    };
    qint64 now() const;
    quint64 begin(const playback::StreamRequest &, Completion, bool singlePass);
    void startFlight(const std::string &key, const playback::StreamRequest &, bool fullProfile, qint64 startedMs,
                     bool singlePass = false);
    void runFinished(const std::string &key, std::uint64_t token, const resolver::YtDlpVideoService::DetailsReport &,
                     const catalog::VideoDetails &);
    void post(quint64 operation, const playback::StreamSet &, const playback::StreamError &);
    void flush(quint64 operation);
    void checkJoins();
    void forgetPrefetch(quint64 operation);
    void dropWaiter(quint64 operation); // withdraws it from its flight, cancelling an orphaned run
    // True while the video's extractions start with the full profile (never for nameFinal).
    bool startsFull(const QString &videoId, qint64 nowMs, bool singlePass);

    resolver::YtDlpVideoService *service_;
    Clock clock_;
    player::ResolveCache<playback::StreamSet> cache_;
    quint64 nextOperation_ = 1;
    std::uint64_t nextToken_ = 1;
    std::map<quint64, Waiter> waiters_;
    std::map<std::string, Flight> flights_;
    std::deque<quint64> prefetches_;
    player::VideoDeadlines fullProfile_; // recently refused or failed videos: full profile first
    struct Counters {
        qint64 extractor = 0, extractorWithJavaScript = 0, failed = 0, cacheHits = 0, cancelled = 0, startedFull = 0;
    } counters_;
    std::deque<qint64> extractorMs_;
};

// Fetcher without a proxy: open() hands the remote https URL to the player, whose own HTTP source
// sends the format's headers (StreamFormat::headers). Used for the yt-dlp-only configuration and
// for formats SharedStreamSource cannot proxy. Completions are asynchronous and exactly once:
// close() before delivery completes with Cancelled.
class DirectStreamFetcher final : public QObject, public playback::StreamFetcher {
public:
    explicit DirectStreamFetcher(QObject *parent = nullptr) : QObject(parent) {}
    quint64 open(const playback::StreamFormat &, Opened) override;
    void close(quint64 stream) override;
    // Keys: "opened", "open" (currently open streams), "refused" (non-https formats).
    QVariantMap diagnostics() const override;

private:
    struct Pending {
        Opened opened;
        QUrl url;
        playback::StreamError error;
        bool delivered = false;
    };
    quint64 next_ = 1;
    std::map<quint64, Pending> open_;
    qint64 opened_ = 0, refused_ = 0;
};

}}}
