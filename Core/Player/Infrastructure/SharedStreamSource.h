// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

// Adapter from SailTune's shared playback component (pinned verbatim under
// Shared/SailTune/Core/SharedPlayback: FastStreamResolver + MediaProxy, namespace
// nordtile::music::sharedplayback) to the player-side interface playback::StreamResolver /
// playback::StreamFetcher (Shared/Playback/StreamSource.h). Qt Core + Network only, no SailTube
// application types (namespace nordtile::player).
//
// The component's classes come in through `Api`, a traits struct (the seam): the app uses
// SailTuneSharedPlayback (SharedStreamSourceSailTune.h); tests may replace the resolver with a
// fake of the same shape. Api provides:
//   Resolver    warmUp(); RequestId resolve(std::string videoId, Request, Callback); cancel(RequestId);
//               invalidate(std::string); reportRefused(std::string, int64 msLeft); allowed();
//               hasVisitor(); counters() {fast, cache, fallbacks, botWalls, probeRejected, network,
//               medianTotalMs()}; breaker().openUntilMs();
//               probeAddress(url, userAgent, contentLength, ProbeDone(ProbeVerdict, status, ms, finalUrl));
//               optional: bool peekCached(std::string videoId, Request, Set *out) (the pinned
//               component since 5611210; a resolver without it never answers a peek)
//   Proxy       Source, FreshSource, Refresher; QUrl open(videoId, Source, Refresher);
//               closeVideo(videoId); static allowedAddress(url); counters(); activeJobs(); port()
//   Request, Format, Set, Outcome, FailureKind, Path  (the component's domain types)
//
// Behaviour:
// - resolve (RES-03, RES-08, RES-09): the fast path first with a video or audio-only request
//   (ceiling, audio codecs, dub, hardware decode). ResolveOutcome.fallback, a fast answer without
//   a split pair for the request, or a delivered rung the probe refused goes to the injected
//   extractor (`fallback`, the yt-dlp adapter); a Final outcome never falls back (it is only named,
//   see final reasons). Every answer is re-ranked by SailTube's ladder (SharedStreamSourceLadder:
//   SEL-01/02 video, codec filters), so both paths rank alike.
// - final reasons (RES-09, RES-19): a Final outcome whose reason label names the case (S-14 labels:
//   removed, terminated, upcoming, geo, private, invalid_id) maps to that class. The pinned
//   component says only "final"; then (nameFinalReasons, not for prefetches) the extractor is asked
//   once to name it (StreamResolver::nameFinal: one extraction pass, no widening, never retried):
//   its Removed / Terminated / GeoBlocked / Private / Upcoming (with the scheduled
//   start) / StillProcessing / SignInRequired / InvalidId answer is delivered as final, anything
//   else as the final Unavailable; streams it finds after all are delivered (the component's
//   phrase list was wrong for this video).
// - probe (RES-06, RES-22): the component probes its own top audio and video (its probe is on and
//   follows up to 3 googlevideo redirects, handing back the address that answered). When
//   SailTube's ladder delivers another top rung, the adapter asks the component to probe that one
//   (FastStreamResolver::probeAddress, "a host that delivers another rung probes it itself") and
//   uses the address that answered; Refused or Unusable sends the resolve to the extractor (the
//   component counts the strike), no verdict is accepted but unverified. A fast-path format opened
//   later that was never probed (a quality switch) is probed the same way before it is proxied.
//   No probe or redirect handling of its own.
// - Completions: asynchronous, on the owning thread, exactly once per operation, cancellation
//   included (class Cancelled); none after destruction.
// - prefetch (RES-11): at most 2 speculative resolves; the newest replaces the oldest. A request
//   whose answer is already there (peekCached) starts nothing and takes no slot; a video that is
//   with the extractor under the strike rules is not speculated on.
// - peekCached: the answer resolve() would deliver with no network use at all (the component's
//   cached answer, re-ranked, its delivered top rungs already probed), or false. Reads only.
// - Strike rules (RES-07, RES-12; SailTune's, from its committed StreamController and
//   FastFirstStreamResolver, kept by player::FaultLedger):
//     * reportRefused(video, format): a byte-0 refusal. Of a fast-path address: a strike (the
//       component counts it when the address had a minute or more to live) and the video is the
//       extractor's for five minutes, starting with its full (JavaScript) profile, so the client
//       that produced a refused address is not asked again at once. Of any other address: no
//       strike; the extractor still starts with its full profile.
//     * reportFault(video, Connection | Stalled | Abandoned): a lost connection, a stall or an
//       opening that was abandoned is no verdict on the source: the cached answers go, the fast
//       path stays, nothing is counted (the session's stall watchdog reports these).
//     * invalidate(video) = reportFault(video, Unspecified), a failure nobody explained: within
//       five minutes of a fast answer for the video, the first one retries through the fast path
//       (its cache dropped); the second one within five minutes sends the video to the
//       extractor's full profile for five minutes. Never a strike.
//     * reportFault(video, Rejected) is reportRefused without the format: judged by whether the
//       fast path delivered the video in the last five minutes.
//   The adapter's own refreshes (MediaProxy asks for a fresh address) and probe refusals only
//   drop cached answers; the component counts the strikes of its own probes.
// - warmUp (RES-05, RES-21): forwards to the component, which mints and caches the visitor id
//   (6 h) and opens the pooled connection. Call once the first screen is up, off the UI thread
//   (ThreadedStreamSource, SharedStreamSourceThreaded.h).
// - fetcher (RES-16..RES-19): a non-HLS format of either path whose URL MediaProxy::allowedAddress
//   accepts and whose length and user agent are known is served through MediaProxy (loopback URL
//   with a per-stream token; MediaProxy follows googlevideo redirects itself). The Refresher
//   re-resolves the same video and returns the same format id, or fails (RES-12). HLS is returned
//   unproxied (RES-19). Anything else is the direct https URL with its headers
//   (StreamFormat::headers) for the player's own source.
//
// The resolver, proxy and fallback must outlive this object.

#include "../../../Shared/Playback/StreamSource.h"
#include "../Domain/ResolveCache.h"
#include "SharedStreamSourceLadder.h"
#include <QByteArray>
#include <QDateTime>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QTimer>
#include <QUrl>
#include <QVariantMap>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace nordtile { namespace player {

namespace sharedsource {
// Non-template helpers (SharedStreamSource.cpp).
bool validVideoId(const QString &);                                  // 11 chars of [A-Za-z0-9_-]
QString mimeTypeOf(const playback::StreamFormat &);                  // "video/webm", "audio/mp4", ...
QByteArray headerValue(const playback::StreamFormat &, const char *name);
std::vector<std::string> componentAudioCodecs(const playback::StreamRequest &); // "opus", "aac"; empty = default
QUrl directRemoteUrl(const playback::StreamFormat &, playback::StreamError *);  // https only

// Api::Resolver::peekCached is optional: used when the resolver has it, "not cached" otherwise.
template <class Resolver, class Request, class Set>
auto peekFast(Resolver *fast, const std::string &videoId, const Request &request, Set *out, int)
    -> decltype(fast->peekCached(videoId, request, out)) {
    return fast->peekCached(videoId, request, out);
}
template <class Resolver, class Request, class Set>
bool peekFast(Resolver *, const std::string &, const Request &, Set *, long) { return false; }
}

struct SharedStreamSourceOptions {
    bool probeDeliveredRungs = true;   // RES-06 for rungs other than the component's own top
    bool nameFinalReasons = true;      // RES-09/19: ask the extractor which final case an unnamed Final is
    int probeJoinMs = 5000;            // audio + video join
    std::function<qint64()> clock;     // Unix ms; default: the system clock
    FaultPolicy faults;                // the strike rules' windows (SailTune: five minutes each)
};

template <class Api>
class SharedStreamSource final : public QObject, public playback::StreamResolver, public SourceFaultSink {
public:
    typedef typename Api::Resolver FastResolver;
    typedef typename Api::Proxy Proxy;
    typedef SharedStreamSourceOptions Options;

    static const std::size_t maximumPrefetches = 2;  // RES-11
    static const std::size_t maximumRemembered = 512; // URLs kept for open() / refresh / probe verdicts

    // fallback may be null (no extractor: fallback outcomes fail with the fast path's class).
    SharedStreamSource(FastResolver *fast, Proxy *proxy, playback::StreamResolver *fallback,
                       const Options &options = Options(), QObject *parent = nullptr)
        : QObject(parent), fast_(fast), proxy_(proxy), fallback_(fallback), options_(options), fetcher_(this),
          ledger_(options.faults) {}

    ~SharedStreamSource() override {
        // Callbacks of these operations (and of probes) are guarded and find this object gone.
        for (const auto &o : ops_) {
            if (o.second.onFast) fast_->cancel(o.second.fastId);
            if (o.second.onFallback && fallback_) fallback_->cancel(o.second.fallbackId);
        }
        // A Refresher calls back exactly once: the pending ones fail.
        const std::map<quint64, Deliver> pending = refreshes_;
        refreshes_.clear();
        for (const auto &r : pending) r.second(typename Proxy::FreshSource());
    }

    // RES-05 / RES-21: visitor id and connection, once the first screen is up.
    void warmUp() { fast_->warmUp(); }

    quint64 resolve(const playback::StreamRequest &request, Completion completion) override {
        const quint64 op = nextOp_++;
        Op o;
        o.request = request;
        o.completion = std::move(completion);
        ops_[op] = o;
        ++counters_.resolves;
        if (!sharedsource::validVideoId(request.videoId)) {
            finish(op, emptyAnswer(request.videoId), playback::streamError(playback::StreamErrorClass::InvalidId, request.signedIn), Source::None);
            return op;
        }
        // Strike rules: a video the fast path lost a moment ago is the extractor's for a while.
        if (avoidsFast(request.videoId)) {
            ++counters_.fastAvoided;
            toFallback(op, playback::streamError(playback::StreamErrorClass::Failed, request.signedIn));
            return op;
        }
        startFast(op);
        return op;
    }

    void prefetch(const playback::StreamRequest &request) override {
        if (!sharedsource::validVideoId(request.videoId)) return;
        // Reuse: an answer that is already there needs no operation and takes no prefetch slot.
        if (peekCached(request, nullptr)) { ++counters_.prefetchesCached; return; }
        // No speculation on a video that just failed: its next real resolve is the extractor's.
        if (avoidsFast(request.videoId)) { ++counters_.prefetchesSkipped; return; }
        const quint64 op = nextOp_++;
        Op o;
        o.request = request;
        o.request.speculative = true;
        o.speculative = true;
        ops_[op] = o;
        prefetches_.push_back(op);
        startFast(op);
        while (prefetches_.size() > maximumPrefetches) {
            ++counters_.prefetchesReplaced;
            cancel(prefetches_.front());
        }
    }

    void cancel(quint64 operation) override {
        auto it = ops_.find(operation);
        if (it == ops_.end()) return;
        const Op o = it->second;
        ops_.erase(it);
        forgetPrefetch(operation);
        if (o.onFast) fast_->cancel(o.fastId);
        if (o.onFallback && fallback_) fallback_->cancel(o.fallbackId);
        ++counters_.cancelled;
        if (!o.completion) return;
        const Completion completion = o.completion;
        const playback::StreamSet answer = emptyAnswer(o.request.videoId);
        QTimer::singleShot(0, this, [completion, answer]() {
            completion(answer, playback::streamError(playback::StreamErrorClass::Cancelled, false));
        });
    }

    // A failure nobody explained (a whole-set failure, a retry, a recovery): the cached answers
    // go; the strike rules above decide between one more try through the fast path and the
    // extractor. Never a strike. A caller that knows more uses reportFault or reportRefused.
    void invalidate(const QString &videoId) override { reportFault(videoId.toStdString(), SourceFault::Unspecified); }

    // RES-07 / RES-12: a strike on the fast path only for its own URLs (with the time they had
    // left), and such a video is the extractor's for a while; every cached answer of the video
    // goes in both paths, and the extractor starts with its full profile.
    void reportRefused(const QString &videoId, const playback::StreamFormat &format) override {
        ++counters_.refusedReports;
        forgetProbe(format.url.toEncoded());
        const std::string id = videoId.toStdString();
        const Origin *origin = findOrigin(format.url);
        if (origin && origin->fast) {
            strike(videoId, format.expiresAtUnixMs);
            ledger_.report(id, SourceFault::Rejected, now());  // this delivery is judged
            ledger_.avoidFast(id, now());
            ++counters_.faultStrikes;
        } else {
            fast_->invalidate(id);
        }
        if (fallback_) fallback_->reportRefused(videoId, format);
    }

    // SourceFaultSink: what the consumer observed when a delivered source of the video stopped
    // working (the strike rules above). Every fault drops the video's cached answers.
    void reportFault(const std::string &videoId, SourceFault fault) override {
        std::int64_t msLeft = 0;
        const FaultVerdict verdict = ledger_.report(videoId, fault, now(), &msLeft);
        if (verdict == FaultVerdict::Strike) {
            ++counters_.faultStrikes;
            fast_->reportRefused(videoId, msLeft); // a strike when >= 60 s were left; drops its cache
        } else {
            fast_->invalidate(videoId);
            if (verdict == FaultVerdict::RetryFast) ++counters_.faultRetries;
            else if (verdict == FaultVerdict::AvoidFast) ++counters_.faultAvoids;
            else if (notTheSource(fault)) ++counters_.faultsNotTheSource;
        }
        if (!fallback_) return;
        // The extractor learns what it can act on: a refused source, or a video that left the
        // fast path, starts with its full profile (Rejected); anything else only drops its cache.
        const SourceFault forwarded = verdict == FaultVerdict::Strike || verdict == FaultVerdict::AvoidFast
            ? SourceFault::Rejected : fault;
        const QString id = QString::fromStdString(videoId);
        if (SourceFaultSink *sink = dynamic_cast<SourceFaultSink *>(fallback_)) sink->reportFault(videoId, forwarded);
        else if (forwarded == SourceFault::Rejected) fallback_->reportRefused(id, playback::StreamFormat());
        else fallback_->invalidate(id);
    }

    // The answer resolve(request) would deliver without using the network, when there is one:
    // the component's cached answer for the request, re-ranked, with its delivered top rungs
    // already probed (by the component, or through it by an earlier resolve or prefetch). Reads
    // only: no request, probe, counter, strike or breaker is touched. False: resolve() has to
    // ask (nothing cached, a rung to probe, or the video is with the extractor).
    bool peekCached(const playback::StreamRequest &request, playback::StreamSet *out) {
        if (!sharedsource::validVideoId(request.videoId) || avoidsFast(request.videoId)) return false;
        typename Api::Set set;
        if (!sharedsource::peekFast(fast_, request.videoId.toStdString(), componentRequest(request), &set, 0)) return false;
        playback::StreamSet s = mapSet(set, request);
        if (s.videoId != request.videoId || !hasSplitPair(s, request)) return false;
        s.path = playback::ResolvePath::Cache;
        s.resolveMs = 0;
        s.phases = playback::ResolvePhases();
        const std::vector<QByteArray> tops = componentTops(set, request);
        const bool video = request.purpose == playback::StreamPurpose::Video && !s.video.isEmpty();
        for (int k = 0; k < (video ? 2 : 1); ++k) {
            playback::StreamFormat &f = k == 1 ? s.video[0] : s.audio[0];
            const QByteArray key = f.url.toEncoded();
            bool componentTop = false;
            for (const QByteArray &t : tops) componentTop = componentTop || t == key;
            if (componentTop) { f.probe = playback::ProbeState::NoVerdict; continue; }
            if (!options_.probeDeliveredRungs) continue;
            const auto known = probed_.find(key);
            if (known == probed_.end()) return false;   // resolve() would probe this rung first
            f.url = QUrl::fromEncoded(known->second, QUrl::StrictMode);
            f.probe = playback::ProbeState::Accepted;
        }
        if (out) *out = s;
        return true;
    }

    // Fixed keys, numbers only (DIA-02): the component's counters and breaker, this adapter's
    // counters, and the extractor's own keys prefixed "extractor.".
    QVariantMap diagnostics() const override {
        QVariantMap d;
        const auto &c = fast_->counters();
        d.insert(QStringLiteral("fastPath"), static_cast<qint64>(c.fast));
        d.insert(QStringLiteral("fastCacheHits"), static_cast<qint64>(c.cache));
        d.insert(QStringLiteral("fastFallbacks"), static_cast<qint64>(c.fallbacks));
        d.insert(QStringLiteral("botWalls"), static_cast<qint64>(c.botWalls));
        d.insert(QStringLiteral("fastNetworkFailures"), static_cast<qint64>(c.network));
        d.insert(QStringLiteral("medianFastPathMs"), static_cast<qint64>(c.medianTotalMs()));
        d.insert(QStringLiteral("breakerOpen"), static_cast<qint64>(fast_->allowed() ? 0 : 1));
        d.insert(QStringLiteral("breakerEndsAtUnixMs"), static_cast<qint64>(fast_->allowed() ? 0 : fast_->breaker().openUntilMs()));
        d.insert(QStringLiteral("visitorReady"), static_cast<qint64>(fast_->hasVisitor() ? 1 : 0));
        d.insert(QStringLiteral("resolves"), counters_.resolves);
        d.insert(QStringLiteral("fastAnswers"), counters_.fastAnswers);
        d.insert(QStringLiteral("fastUnpaired"), counters_.fastUnpaired);
        d.insert(QStringLiteral("fallbacks"), counters_.fallbacks);
        d.insert(QStringLiteral("extractorAnswers"), counters_.extractorAnswers);
        d.insert(QStringLiteral("finalFailures"), counters_.finalFailures);
        d.insert(QStringLiteral("finalLookups"), counters_.finalLookups);
        d.insert(QStringLiteral("failed"), counters_.failed);
        d.insert(QStringLiteral("cancelled"), counters_.cancelled);
        d.insert(QStringLiteral("prefetchesReplaced"), counters_.prefetchesReplaced);
        d.insert(QStringLiteral("refusedReports"), counters_.refusedReports);
        d.insert(QStringLiteral("probes"), counters_.probes);
        d.insert(QStringLiteral("probeAccepted"), counters_.probeAccepted);
        d.insert(QStringLiteral("probeRefused"), counters_.probeRefused);
        d.insert(QStringLiteral("probeUnusable"), counters_.probeUnusable);
        d.insert(QStringLiteral("probeNoVerdict"), counters_.probeNoVerdict);
        // Strike rules: resolves sent straight to the extractor, verdicts, videos off the fast path now.
        d.insert(QStringLiteral("fastAvoided"), counters_.fastAvoided);
        d.insert(QStringLiteral("faultStrikes"), counters_.faultStrikes);
        d.insert(QStringLiteral("faultRetries"), counters_.faultRetries);
        d.insert(QStringLiteral("faultAvoids"), counters_.faultAvoids);
        d.insert(QStringLiteral("faultsNotTheSource"), counters_.faultsNotTheSource);
        d.insert(QStringLiteral("avoidedVideos"), static_cast<qint64>(ledger_.avoidedCount(now())));
        d.insert(QStringLiteral("prefetchesCached"), counters_.prefetchesCached);
        d.insert(QStringLiteral("prefetchesSkipped"), counters_.prefetchesSkipped);
        if (fallback_) {
            const QVariantMap e = fallback_->diagnostics();
            for (auto it = e.constBegin(); it != e.constEnd(); ++it) d.insert(QStringLiteral("extractor.") + it.key(), it.value());
        }
        return d;
    }

    // The fetcher half (RES-16..RES-19); owned by this object.
    playback::StreamFetcher *fetcher() { return &fetcher_; }

    // The component's answer as a StreamSet for the request, re-ranked by SailTube's ladder.
    static playback::StreamSet mapSet(const typename Api::Set &set, const playback::StreamRequest &request) {
        playback::StreamSet s;
        s.videoId = QString::fromStdString(set.videoId);
        for (const auto &f : set.video) s.video << mapFormat(f, set.userAgent);
        for (const auto &f : set.audio) s.audio << mapFormat(f, set.userAgent);
        for (const auto &f : set.muxed) s.muxed << mapFormat(f, set.userAgent);
        rankVideo(&s.video, request);
        keepAllowedAudio(&s.audio, request); // the component's audio ladder is SEL-04 already
        rankMuxed(&s.muxed, request);
        s.live = set.metadata.isLive;
        s.liveState = s.live ? playback::LiveState::Live : playback::LiveState::NotLive;
        if (s.live && !set.hlsUrl.empty()) s.hlsManifest = QUrl::fromEncoded(QByteArray::fromStdString(set.hlsUrl), QUrl::StrictMode);
        s.expiresAtUnixMs = set.expiresAtUnixMs > 0 ? set.expiresAtUnixMs : earliestExpiryMs(s);
        s.hasLoudness = set.hasLoudness;
        s.loudnessDb = set.loudnessDb;
        s.path = mapPath(set.path);
        s.resolveMs = set.timings.totalMs;
        s.phases.visitorMs = set.timings.visitorMs;
        s.phases.requestMs = set.timings.playerMs;
        s.phases.probeMs = set.timings.probeMs;
        return s;
    }

    // RES-09: the final class a component reason label names (docs/SHARED_REQUESTS.md S-14); None
    // for a label that names no case (the pinned component's "final").
    static playback::StreamErrorClass finalClassFor(const std::string &reason) {
        using playback::StreamErrorClass;
        if (reason == "removed" || reason == "no_longer_available") return StreamErrorClass::Removed;
        if (reason == "terminated") return StreamErrorClass::Terminated;
        if (reason == "upcoming" || reason == "premiere") return StreamErrorClass::Upcoming;
        if (reason == "geo" || reason == "geo_blocked" || reason == "country") return StreamErrorClass::GeoBlocked;
        if (reason == "private") return StreamErrorClass::Private;
        if (reason == "invalid_id") return StreamErrorClass::InvalidId;
        return StreamErrorClass::None;
    }

    // RES-09 class of an outcome (the answer when there is no extractor to fall back to). Final:
    // the named case, else the final class Unavailable. The component's fixed labels otherwise:
    // login_required -> SignInRequired, unplayable / not_ok -> the non-final Unavailable, no direct
    // URLs (no_direct_*) -> DeliveryChanged, HTTP 429 -> TryLater, a refused probe -> Failed.
    static playback::StreamError errorFor(const typename Api::Outcome &o, bool signedIn) {
        using playback::StreamErrorClass;
        StreamErrorClass e = StreamErrorClass::Failed;
        bool bare = false;
        if (o.failure == Api::FailureKind::Final) {
            e = finalClassFor(o.reason);
            if (e == StreamErrorClass::None) e = StreamErrorClass::Unavailable;
        }
        else if (o.failure == Api::FailureKind::BadResponse && o.reason == "invalid_id") e = StreamErrorClass::InvalidId;
        else if (o.failure == Api::FailureKind::BadResponse && o.reason == "http_429") e = StreamErrorClass::TryLater;
        else if (o.failure == Api::FailureKind::BadResponse) e = StreamErrorClass::InvalidResponse;
        else if (o.failure == Api::FailureKind::BreakerOpen) e = StreamErrorClass::TryLater;
        else if (o.failure == Api::FailureKind::BotWall) e = StreamErrorClass::BotCheck;
        else if (o.failure == Api::FailureKind::Network) e = StreamErrorClass::Network;
        else if (o.failure == Api::FailureKind::Cancelled) e = StreamErrorClass::Cancelled;
        else if (o.failure == Api::FailureKind::Unplayable && o.reason == "login_required") e = StreamErrorClass::SignInRequired;
        else if (o.failure == Api::FailureKind::Unplayable && (o.reason == "unplayable" || o.reason == "not_ok")) {
            e = StreamErrorClass::Unavailable;
            bare = true;
        }
        else if (o.failure == Api::FailureKind::NoUsableFormat) e = StreamErrorClass::DeliveryChanged;
        else if (o.failure == Api::FailureKind::ProbeRejected) e = StreamErrorClass::Failed;
        else if (o.failure == Api::FailureKind::Unplayable) e = StreamErrorClass::NoPlayableStream;
        playback::StreamError err = playback::streamError(e, signedIn);
        if (o.failure == Api::FailureKind::Final && e != StreamErrorClass::Private) err.final = true;
        if (bare) err.final = false;  // one client's "unavailable" (RES-09)
        return err;
    }

private:
    enum class Source { None, Fast, Extractor };
    typedef std::function<void(const typename Proxy::FreshSource &)> Deliver;
    enum class Verdict { Accepted, NoVerdict, Refused, Unusable };
    // Verdict and the URL to use (the address that answered on Accepted, the probed URL otherwise).
    typedef std::function<void(Verdict, const std::string &finalUrl)> ProbeDone;

    struct Op {
        playback::StreamRequest request;
        Completion completion;           // empty for a prefetch
        bool speculative = false;
        bool onFast = false;
        typename FastResolver::RequestId fastId = 0;
        bool onFallback = false;
        quint64 fallbackId = 0;
        bool namingFinal = false;        // the extractor names an unnamed Final (RES-09)
        playback::StreamError componentFinal;
    };
    struct Origin {
        QString videoId;
        playback::StreamRequest request;
        bool fast = false;
        qint64 issuedAtMs = 0;
    };
    struct Stream {
        playback::StreamFetcher::Opened opened;
        QUrl url;
        playback::StreamError error;
        bool ready = false;              // url / error decided
        bool delivered = false;
        bool proxied = false;
        QString videoId;
    };
    struct Join {                        // RES-06 probe of the top audio + video of one answer
        playback::StreamSet set;
        int pending = 0;
        bool done = false;
        bool refused = false;            // Refused or Unusable
        qint64 startedMs = 0;
        bool resolvedAudio = true, resolvedVideo = true;
    };

    class Fetcher final : public playback::StreamFetcher {
    public:
        explicit Fetcher(SharedStreamSource *owner) : owner_(owner) {}
        quint64 open(const playback::StreamFormat &f, Opened opened) override { return owner_->openStream(f, std::move(opened)); }
        void close(quint64 stream) override { owner_->closeStream(stream); }
        // Keys: "opened", "proxied", "direct", "hls", "proxyUnavailable", "refusedAtOpen", "open",
        // "refreshes", "refreshFailed", "proxyListening", "proxyJobs" and "proxy.*" counters
        // (MediaProxy::Counters, with the pre-buffer, tail and pooled-connection ones).
        QVariantMap diagnostics() const override { return owner_->fetcherDiagnostics(); }
    private:
        SharedStreamSource *owner_;
    };

    qint64 now() const { return options_.clock ? options_.clock() : QDateTime::currentMSecsSinceEpoch(); }

    static playback::StreamSet emptyAnswer(const QString &videoId) {
        playback::StreamSet s;
        s.videoId = videoId;
        return s;
    }

    static typename Api::Request componentRequest(const playback::StreamRequest &r) {
        typename Api::Request c;
        c.audioOnly = r.purpose == playback::StreamPurpose::AudioOnly;
        c.maxVideoHeight = r.maxHeight;              // 0: no ceiling in both
        c.codecPreference = sharedsource::componentAudioCodecs(r);
        c.dubLanguage = r.dubLanguage.toStdString();
        c.hardwareDecode = r.hardwareDecode;
        return c;
    }

    static playback::StreamFormat mapFormat(const typename Api::Format &f, const std::string &userAgent) {
        playback::StreamFormat o;
        o.formatId = QString::number(f.itag);
        o.container = QString::fromStdString(f.container());
        const QString codec = QString::fromStdString(f.codec);
        o.videoCodec = f.hasVideo ? codec : QStringLiteral("none");
        if (!f.hasAudio) o.audioCodec = QStringLiteral("none");
        else if (!f.hasVideo) o.audioCodec = codec;
        // A progressive format names its video codec only; the mp4 ones carry AAC-LC.
        else o.audioCodec = o.container == QLatin1String("mp4") ? QStringLiteral("mp4a.40.2") : QString();
        o.width = f.width;
        o.height = f.height;
        o.fps = f.fps;
        o.bitrateKbps = f.bitrate / 1000.0;
        o.contentLength = f.contentLength > 0 ? f.contentLength : -1;
        if (f.hasAudio && !f.hasVideo) {
            o.language = QString::fromStdString(f.audioLanguage);
            o.originalAudio = f.isDefaultAudio && !f.isDescriptive;
            o.languagePreference = f.isDescriptive ? -1 : f.isDefaultAudio ? 10 : 0;
            o.descriptive = f.isDescriptive;
        }
        o.dynamicRangeCompressed = f.isDrc;
        o.isHls = f.isHls;
        o.url = QUrl::fromEncoded(QByteArray::fromStdString(f.url), QUrl::StrictMode);
        if (!userAgent.empty()) o.headers << qMakePair(QByteArray("User-Agent"), QByteArray::fromStdString(userAgent));
        o.expiresAtUnixMs = f.expiresAtUnixMs;
        return o;
    }

    static playback::ResolvePath mapPath(typename Api::Path p) {
        if (p == Api::Path::Cache) return playback::ResolvePath::Cache;
        if (p == Api::Path::Fast) return playback::ResolvePath::FastPath;
        if (p == Api::Path::YtDlp) return playback::ResolvePath::Extractor;
        return playback::ResolvePath::Unknown;
    }

    void strike(const QString &videoId, qint64 expiresAtUnixMs) {
        // A byte-0 refusal reported by the player (the component's own probes count their strikes).
        const qint64 msLeft = expiresAtUnixMs > 0 ? expiresAtUnixMs - now() : 0;
        fast_->reportRefused(videoId.toStdString(), msLeft); // a strike when >= 60 s were left; drops its cache
    }

    // Strike rules: true while the video is the extractor's. Without an extractor the fast path
    // is the only path and is never avoided.
    bool avoidsFast(const QString &videoId) { return fallback_ && ledger_.avoidsFast(videoId.toStdString(), now()); }

    // The cached answers of the video in both paths, and nothing else (the adapter's own
    // refreshes and probe refusals: no verdict of the consumer).
    void dropCaches(const QString &videoId) {
        fast_->invalidate(videoId.toStdString());
        if (fallback_) fallback_->invalidate(videoId);
    }

    // ---- resolve ------------------------------------------------------------------------------

    void startFast(quint64 op) {
        auto it = ops_.find(op);
        if (it == ops_.end()) return;
        it->second.onFast = true;
        const playback::StreamRequest request = it->second.request;
        QPointer<QObject> guard(this);
        const typename FastResolver::RequestId id = fast_->resolve(request.videoId.toStdString(), componentRequest(request),
            [this, guard, op](const typename Api::Outcome &outcome) {
            if (!guard) return;
            onFast(op, outcome);
        });
        it = ops_.find(op);
        if (it != ops_.end() && it->second.onFast) it->second.fastId = id;
    }

    void onFast(quint64 op, const typename Api::Outcome &outcome) {
        auto it = ops_.find(op);
        if (it == ops_.end() || !it->second.onFast) return; // cancelled meanwhile
        it->second.onFast = false;
        const playback::StreamRequest request = it->second.request;
        if (outcome.ok()) {
            const playback::StreamSet s = mapSet(outcome.streams, request);
            if (s.videoId == request.videoId && hasSplitPair(s, request)) {
                probePair(op, markComponentProbed(s, outcome.streams, request));
                return;
            }
            // RES-08: no usable split pair for this request (codec policy, video wanted).
            ++counters_.fastUnpaired;
            toFallback(op, playback::streamError(playback::StreamErrorClass::NoPlayableStream, request.signedIn));
            return;
        }
        const playback::StreamError error = errorFor(outcome, request.signedIn);
        if (outcome.fallback) { toFallback(op, error); return; }
        // RES-09 / RES-19: a Final the component did not name: the extractor names it once.
        if (outcome.failure == Api::FailureKind::Final && finalClassFor(outcome.reason) == playback::StreamErrorClass::None
                && options_.nameFinalReasons && fallback_ && !it->second.speculative) {
            askFinalReason(op, error);
            return;
        }
        if (error.final) ++counters_.finalFailures; // RES-09: never falls back
        else ++counters_.failed;
        finish(op, emptyAnswer(request.videoId), error, Source::None);
    }

    void accept(quint64 op, const playback::StreamSet &s) {
        if (!ops_.count(op)) return;
        ++counters_.fastAnswers;
        finish(op, s, playback::StreamError(), Source::Fast);
    }

    // The addresses the component probed itself: its top audio (or muxed) and, for video, its
    // top video.
    static std::vector<QByteArray> componentTops(const typename Api::Set &set, const playback::StreamRequest &request) {
        std::vector<QByteArray> tops;
        if (const typename Api::Format *top = set.top()) tops.push_back(QByteArray::fromStdString(top->url));
        if (request.purpose == playback::StreamPurpose::Video && !set.video.empty())
            tops.push_back(QByteArray::fromStdString(set.video.front().url));
        return tops;
    }

    // The component probed its own top audio and video (and rewrote them to the addresses that
    // answered); remember them, and mark them accepted but not verified here (the component does
    // not report the verdict, docs/SHARED_REQUESTS.md S-18).
    playback::StreamSet markComponentProbed(playback::StreamSet s, const typename Api::Set &set,
                                            const playback::StreamRequest &request) {
        const std::vector<QByteArray> tops = componentTops(set, request);
        for (QList<playback::StreamFormat> *list : {&s.video, &s.audio, &s.muxed})
            for (playback::StreamFormat &f : *list)
                for (const QByteArray &t : tops)
                    if (f.url.toEncoded() == t) {
                        f.probe = playback::ProbeState::NoVerdict;
                        rememberProbe(t, t);
                    }
        return s;
    }

    // RES-06: the delivered top audio and (for video) top video must have been probed; a rung the
    // component did not probe is probed through it (probeAddress), both in parallel.
    void probePair(quint64 op, const playback::StreamSet &s) {
        auto it = ops_.find(op);
        if (it == ops_.end()) return;
        const bool video = it->second.request.purpose == playback::StreamPurpose::Video && !s.video.isEmpty();
        std::shared_ptr<Join> join = std::make_shared<Join>();
        join->set = s;
        join->startedMs = now();
        const auto known = [this](playback::StreamFormat *f) {
            if (!options_.probeDeliveredRungs) return true;
            const auto p = probed_.find(f->url.toEncoded());
            if (p == probed_.end()) return false;
            f->url = QUrl::fromEncoded(p->second, QUrl::StrictMode);
            if (f->probe == playback::ProbeState::NotProbed) f->probe = playback::ProbeState::Accepted;
            return true;
        };
        join->resolvedAudio = known(&join->set.audio[0]);
        join->resolvedVideo = !video || known(&join->set.video[0]);
        const int targets = (join->resolvedAudio ? 0 : 1) + (join->resolvedVideo ? 0 : 1);
        if (!targets) { accept(op, join->set); return; }
        join->pending = targets;
        QPointer<QObject> guard(this);
        for (int k = 0; k < 2; ++k) {
            const bool isVideo = k == 1;
            if (isVideo ? join->resolvedVideo : join->resolvedAudio) continue;
            const playback::StreamFormat f = isVideo ? join->set.video[0] : join->set.audio[0];
            probeFormat(f, [this, guard, op, join, isVideo](Verdict verdict, const std::string &finalUrl) {
                if (!guard || join->done) return;
                playback::StreamFormat &target = isVideo ? join->set.video[0] : join->set.audio[0];
                (isVideo ? join->resolvedVideo : join->resolvedAudio) = true;
                if (verdict == Verdict::Accepted) {
                    target.url = QUrl::fromEncoded(QByteArray::fromStdString(finalUrl), QUrl::StrictMode);
                    target.probe = playback::ProbeState::Accepted;
                } else if (verdict == Verdict::Refused || verdict == Verdict::Unusable) {
                    join->refused = true;
                } else {
                    target.probe = playback::ProbeState::NoVerdict;
                }
                if (--join->pending == 0) completeProbe(op, join);
            });
        }
        QTimer::singleShot(options_.probeJoinMs, this, [this, op, join]() {
            if (join->done) return;
            if (!join->resolvedAudio) join->set.audio[0].probe = playback::ProbeState::NoVerdict;
            if (!join->resolvedVideo) join->set.video[0].probe = playback::ProbeState::NoVerdict;
            completeProbe(op, join);
        });
    }

    void completeProbe(quint64 op, const std::shared_ptr<Join> &join) {
        join->done = true;
        join->set.phases.probeMs = now() - join->startedMs;
        if (!join->refused) { accept(op, join->set); return; }
        // Refused (the component counted the strike) or unusable: drop the cached answer, then the
        // extractor (RES-08).
        fast_->invalidate(join->set.videoId.toStdString());
        auto it = ops_.find(op);
        if (it == ops_.end()) return;
        toFallback(op, playback::streamError(playback::StreamErrorClass::NoPlayableStream, it->second.request.signedIn));
    }

    void toFallback(quint64 op, const playback::StreamError &withoutFallback) {
        auto it = ops_.find(op);
        if (it == ops_.end()) return;
        const playback::StreamRequest request = it->second.request;
        if (!fallback_) {
            ++counters_.failed;
            finish(op, emptyAnswer(request.videoId), withoutFallback, Source::None);
            return;
        }
        ++counters_.fallbacks;
        if (it->second.speculative) {
            ops_.erase(it);
            forgetPrefetch(op);
            fallback_->prefetch(request);
            return;
        }
        it->second.onFallback = true;
        QPointer<QObject> guard(this);
        const quint64 id = fallback_->resolve(request, [this, guard, op](const playback::StreamSet &s, const playback::StreamError &e) {
            if (!guard) return;
            onFallback(op, s, e);
        });
        it = ops_.find(op);
        if (it != ops_.end() && it->second.onFallback) it->second.fallbackId = id;
    }

    void askFinalReason(quint64 op, const playback::StreamError &componentFinal) {
        auto it = ops_.find(op);
        if (it == ops_.end()) return;
        ++counters_.finalLookups;
        it->second.onFallback = true;
        it->second.namingFinal = true;
        it->second.componentFinal = componentFinal;
        QPointer<QObject> guard(this);
        // At most one extraction pass: the extractor does not widen or retry here.
        const quint64 id = fallback_->nameFinal(it->second.request, [this, guard, op](const playback::StreamSet &s, const playback::StreamError &e) {
            if (!guard) return;
            onFinalNamed(op, s, e);
        });
        it = ops_.find(op);
        if (it != ops_.end() && it->second.onFallback) it->second.fallbackId = id;
    }

    void onFinalNamed(quint64 op, const playback::StreamSet &s, const playback::StreamError &e) {
        using playback::StreamErrorClass;
        auto it = ops_.find(op);
        if (it == ops_.end() || !it->second.onFallback) return;
        it->second.onFallback = false;
        const playback::StreamError component = it->second.componentFinal;
        const bool anyStream = !s.video.isEmpty() || !s.audio.isEmpty() || !s.muxed.isEmpty() || !s.hlsManifest.isEmpty();
        if (e.ok() && anyStream) {  // the component's phrase list was wrong for this video
            ++counters_.extractorAnswers;
            finish(op, s, e, Source::Extractor);
            return;
        }
        const bool named = e.kind == StreamErrorClass::Removed || e.kind == StreamErrorClass::Terminated
            || e.kind == StreamErrorClass::GeoBlocked || e.kind == StreamErrorClass::Private
            || e.kind == StreamErrorClass::Upcoming || e.kind == StreamErrorClass::StillProcessing
            || e.kind == StreamErrorClass::SignInRequired || e.kind == StreamErrorClass::InvalidId;
        ++counters_.finalFailures;
        if (!named) { finish(op, emptyAnswer(it->second.request.videoId), component, Source::None); return; }
        playback::StreamError err = e;
        err.final = true;  // the component's verdict: no retry, widening or re-resolve
        playback::StreamSet answer = emptyAnswer(it->second.request.videoId);
        if (e.kind == StreamErrorClass::Upcoming) {  // RES-19: the start, when the extractor knows it
            answer.liveState = playback::LiveState::Upcoming;
            answer.scheduledStartUnixMs = s.scheduledStartUnixMs;
        }
        finish(op, answer, err, Source::None);
    }

    void onFallback(quint64 op, const playback::StreamSet &s, const playback::StreamError &e) {
        auto it = ops_.find(op);
        if (it == ops_.end() || !it->second.onFallback) return;
        it->second.onFallback = false;
        if (e.ok()) ++counters_.extractorAnswers;
        else if (e.final) ++counters_.finalFailures;
        else ++counters_.failed;
        finish(op, s, e, e.ok() ? Source::Extractor : Source::None);
    }

    // Exactly once: the operation leaves the table here and its completion is queued.
    void finish(quint64 op, const playback::StreamSet &s, const playback::StreamError &e, Source source) {
        auto it = ops_.find(op);
        if (it == ops_.end()) return;
        const Completion completion = it->second.completion;
        const playback::StreamRequest request = it->second.request;
        ops_.erase(it);
        forgetPrefetch(op);
        if (e.ok() && source != Source::None) remember(s, request, source == Source::Fast);
        if (!completion) return;
        // Strike rules: whose answer the consumer holds now (a prefetch delivers nothing).
        if (e.ok() && source == Source::Fast) ledger_.fastDelivered(s.videoId.toStdString(), now(), s.expiresAtUnixMs);
        else if (e.ok() && source == Source::Extractor) ledger_.extractorDelivered(s.videoId.toStdString());
        QTimer::singleShot(0, this, [completion, s, e]() { completion(s, e); });
    }

    void forgetPrefetch(quint64 op) {
        for (auto it = prefetches_.begin(); it != prefetches_.end();) {
            if (*it == op) it = prefetches_.erase(it);
            else ++it;
        }
    }

    // ---- remembered URLs ----------------------------------------------------------------------

    void remember(const playback::StreamSet &s, const playback::StreamRequest &request, bool fast) {
        Origin o;
        o.videoId = s.videoId;
        o.request = request;
        o.request.speculative = false;
        o.fast = fast;
        o.issuedAtMs = now();
        for (const QList<playback::StreamFormat> *list : {&s.video, &s.audio, &s.muxed}) {
            for (const playback::StreamFormat &f : *list) {
                const QByteArray key = f.url.toEncoded();
                if (!origins_.count(key)) originOrder_.push_back(key);
                origins_[key] = o;
            }
        }
        while (originOrder_.size() > maximumRemembered) {
            origins_.erase(originOrder_.front());
            originOrder_.pop_front();
        }
    }

    const Origin *findOrigin(const QUrl &url) const {
        auto it = origins_.find(url.toEncoded());
        return it == origins_.end() ? nullptr : &it->second;
    }

    void rememberProbe(const QByteArray &probedUrl, const QByteArray &finalUrl) {
        for (const QByteArray &key : {probedUrl, finalUrl}) {
            if (!probed_.count(key)) probedOrder_.push_back(key);
            probed_[key] = finalUrl;
        }
        while (probedOrder_.size() > maximumRemembered) {
            probed_.erase(probedOrder_.front());
            probedOrder_.pop_front();
        }
    }

    void forgetProbe(const QByteArray &url) {
        for (auto it = probed_.begin(); it != probed_.end();) {
            if (it->first == url || it->second == url) it = probed_.erase(it);
            else ++it;
        }
    }

    // ---- probe (RES-06): through the component ------------------------------------------------

    void probeFormat(const playback::StreamFormat &f, const ProbeDone &done) {
        const QByteArray startKey = f.url.toEncoded();
        const std::string ua = sharedsource::headerValue(f, "User-Agent").toStdString();
        ++counters_.probes;
        QPointer<QObject> guard(this);
        fast_->probeAddress(startKey.toStdString(), ua, f.contentLength > 0 ? f.contentLength : 0,
            [this, guard, startKey, done](typename FastResolver::ProbeVerdict v, int, int, const std::string &finalUrl) {
            if (!guard) return;
            Verdict verdict = Verdict::NoVerdict;
            if (v == FastResolver::ProbeVerdict::Accepted) verdict = Verdict::Accepted;
            else if (v == FastResolver::ProbeVerdict::Refused) verdict = Verdict::Refused;
            else if (v == FastResolver::ProbeVerdict::Unusable) verdict = Verdict::Unusable;
            const std::string answered = finalUrl.empty() ? startKey.toStdString() : finalUrl;
            switch (verdict) {
            case Verdict::Accepted: ++counters_.probeAccepted; rememberProbe(startKey, QByteArray::fromStdString(answered)); break;
            case Verdict::Refused: ++counters_.probeRefused; break;
            case Verdict::Unusable: ++counters_.probeUnusable; break;
            case Verdict::NoVerdict: ++counters_.probeNoVerdict; break;
            }
            done(verdict, verdict == Verdict::Accepted ? answered : startKey.toStdString());
        });
    }

    // ---- fetcher (RES-16..RES-19) --------------------------------------------------------------

    typename Proxy::Source proxySource(const playback::StreamFormat &f, const std::string &url, qint64 issuedAtMs) const {
        typename Proxy::Source s;
        s.url = url;
        s.userAgent = sharedsource::headerValue(f, "User-Agent").toStdString();
        s.contentLength = f.contentLength;
        s.mimeType = sharedsource::mimeTypeOf(f).toStdString();
        s.expiresAtUnixMs = f.expiresAtUnixMs;
        s.issuedAtUnixMs = issuedAtMs;
        return s;
    }

    // RES-12: a fresh address of the same format: the cached answers of the video are dropped,
    // the video is resolved again with the original request and the same format id is returned
    // (the proxy then checks length and type and follows redirects itself), or the refresh fails.
    typename Proxy::Refresher refresherFor(const Origin &origin, const QString &formatId) {
        QPointer<QObject> guard(this);
        const QString videoId = origin.videoId;
        const playback::StreamRequest request = origin.request;
        return [this, guard, videoId, request, formatId](const Deliver &deliver) {
            if (!guard) { deliver(typename Proxy::FreshSource()); return; }
            ++counters_.refreshes;
            const quint64 ticket = nextRefresh_++;
            refreshes_[ticket] = deliver;
            dropCaches(videoId);
            resolve(request, [this, ticket, formatId](const playback::StreamSet &s, const playback::StreamError &e) {
                const playback::StreamFormat *match = nullptr;
                if (e.ok())
                    for (const QList<playback::StreamFormat> *list : {&s.video, &s.audio, &s.muxed})
                        for (const playback::StreamFormat &f : *list)
                            if (!match && f.formatId == formatId && !f.isHls) match = &f;
                if (!match) { deliverRefresh(ticket, nullptr, std::string()); return; }
                deliverRefresh(ticket, match, match->url.toEncoded().toStdString());
            });
        };
    }

    void deliverRefresh(quint64 ticket, const playback::StreamFormat *f, const std::string &url) {
        auto it = refreshes_.find(ticket);
        if (it == refreshes_.end()) return;
        const Deliver d = it->second;
        refreshes_.erase(it);
        typename Proxy::FreshSource fresh;
        if (f) {
            fresh.ok = true;
            fresh.source = proxySource(*f, url, now());
        } else {
            ++counters_.refreshFailed;
        }
        d(fresh);
    }

    quint64 openStream(const playback::StreamFormat &format, playback::StreamFetcher::Opened opened) {
        const quint64 handle = nextStream_++;
        Stream st;
        st.opened = std::move(opened);
        streams_[handle] = st;
        ++counters_.opened;
        const QByteArray key = format.url.toEncoded();
        const Origin *origin = findOrigin(format.url);
        const bool proxyable = !format.isHls && proxy_ && origin && Proxy::allowedAddress(key.toStdString())
            && format.contentLength > 0 && !sharedsource::headerValue(format, "User-Agent").isEmpty();
        if (!proxyable) {
            // RES-19: HLS is never proxied; the rest goes to the player's own source.
            decideDirect(handle, format);
        } else {
            const Origin o = *origin;
            const auto known = probed_.find(key);
            // A fast-path rung nobody probed yet (e.g. a quality switch) is probed through the
            // component first; extractor rungs go to the proxy as they are.
            if (!o.fast || !options_.probeDeliveredRungs || known != probed_.end()) {
                proxyStream(handle, format, o, known != probed_.end() ? known->second.toStdString() : key.toStdString());
            } else {
                QPointer<QObject> guard(this);
                probeFormat(format, [this, guard, handle, format, o](Verdict verdict, const std::string &finalUrl) {
                    if (!guard || !streams_.count(handle)) return;
                    if (verdict == Verdict::Refused || verdict == Verdict::Unusable) {
                        ++counters_.refusedAtOpen;
                        dropCaches(o.videoId);
                        decide(handle, QUrl(), playback::streamError(playback::StreamErrorClass::NoPlayableStream, o.request.signedIn));
                        return;
                    }
                    proxyStream(handle, format, o, finalUrl);
                });
            }
        }
        return handle;
    }

    void proxyStream(quint64 handle, const playback::StreamFormat &format, const Origin &origin, const std::string &url) {
        const QUrl local = proxy_->open(origin.videoId.toStdString(), proxySource(format, url, origin.issuedAtMs),
                                        refresherFor(origin, format.formatId));
        if (local.isValid() && !local.isEmpty()) {
            auto it = streams_.find(handle);
            if (it != streams_.end()) {
                it->second.proxied = true;
                it->second.videoId = origin.videoId;
            }
            ++counters_.proxied;
            decide(handle, local, playback::StreamError());
            return;
        }
        ++counters_.proxyUnavailable;
        playback::StreamFormat direct = format;
        direct.url = QUrl::fromEncoded(QByteArray::fromStdString(url), QUrl::StrictMode);
        decideDirect(handle, direct);
    }

    void decideDirect(quint64 handle, const playback::StreamFormat &format) {
        playback::StreamError error;
        const QUrl url = sharedsource::directRemoteUrl(format, &error);
        if (error.ok()) ++(format.isHls ? counters_.hls : counters_.direct);
        decide(handle, url, error);
    }

    // The stream's URL or error is known: deliver it from the event loop, exactly once.
    void decide(quint64 handle, const QUrl &url, const playback::StreamError &error) {
        auto it = streams_.find(handle);
        if (it == streams_.end()) return;
        it->second.url = url;
        it->second.error = error;
        it->second.ready = true;
        QTimer::singleShot(0, this, [this, handle]() {
            auto s = streams_.find(handle);
            if (s == streams_.end() || s->second.delivered) return;
            s->second.delivered = true;
            const playback::StreamFetcher::Opened cb = s->second.opened;
            const QUrl u = s->second.url;
            const playback::StreamError e = s->second.error;
            if (!e.ok()) streams_.erase(s);
            if (cb) cb(u, e);
        });
    }

    void closeStream(quint64 handle) {
        auto it = streams_.find(handle);
        if (it == streams_.end()) return;
        const Stream st = it->second;
        streams_.erase(it);
        if (st.proxied) {
            bool others = false;
            for (const auto &s : streams_) others = others || (s.second.proxied && s.second.videoId == st.videoId);
            if (!others) proxy_->closeVideo(st.videoId.toStdString());
        }
        if (st.delivered || !st.opened) return;
        const playback::StreamFetcher::Opened cb = st.opened;
        QTimer::singleShot(0, this, [cb]() { cb(QUrl(), playback::streamError(playback::StreamErrorClass::Cancelled, false)); });
    }

    QVariantMap fetcherDiagnostics() const {
        QVariantMap d;
        d.insert(QStringLiteral("opened"), counters_.opened);
        d.insert(QStringLiteral("proxied"), counters_.proxied);
        d.insert(QStringLiteral("direct"), counters_.direct);
        d.insert(QStringLiteral("hls"), counters_.hls);
        d.insert(QStringLiteral("proxyUnavailable"), counters_.proxyUnavailable);
        d.insert(QStringLiteral("refusedAtOpen"), counters_.refusedAtOpen);
        d.insert(QStringLiteral("open"), static_cast<qint64>(streams_.size()));
        d.insert(QStringLiteral("refreshes"), counters_.refreshes);
        d.insert(QStringLiteral("refreshFailed"), counters_.refreshFailed);
        if (proxy_) {
            const auto &c = proxy_->counters();
            d.insert(QStringLiteral("proxyListening"), static_cast<qint64>(proxy_->port() ? 1 : 0));
            d.insert(QStringLiteral("proxyJobs"), static_cast<qint64>(proxy_->activeJobs()));
            d.insert(QStringLiteral("proxy.served"), static_cast<qint64>(c.served));
            d.insert(QStringLiteral("proxy.chunks"), static_cast<qint64>(c.chunks));
            d.insert(QStringLiteral("proxy.resumed"), static_cast<qint64>(c.resumed));
            d.insert(QStringLiteral("proxy.refreshes"), static_cast<qint64>(c.refreshes));
            d.insert(QStringLiteral("proxy.earlyRetries"), static_cast<qint64>(c.earlyRetries));
            d.insert(QStringLiteral("proxy.identityChanged"), static_cast<qint64>(c.identityChanged));
            d.insert(QStringLiteral("proxy.refused"), static_cast<qint64>(c.refused));
            d.insert(QStringLiteral("proxy.pauses"), static_cast<qint64>(c.pauses));
            d.insert(QStringLiteral("proxy.redirects"), static_cast<qint64>(c.redirects));
            // Since 5611210: the start and the tail fetched at open(), pooled upstream connections.
            d.insert(QStringLiteral("proxy.prebufferHits"), static_cast<qint64>(c.prebufferHits));
            d.insert(QStringLiteral("proxy.prebufferFailures"), static_cast<qint64>(c.prebufferFailures));
            d.insert(QStringLiteral("proxy.tailHits"), static_cast<qint64>(c.tailHits));
            d.insert(QStringLiteral("proxy.tailFailures"), static_cast<qint64>(c.tailFailures));
            d.insert(QStringLiteral("proxy.upstreamReused"), static_cast<qint64>(c.upstreamReused));
        }
        return d;
    }

    FastResolver *fast_;
    Proxy *proxy_;
    playback::StreamResolver *fallback_;
    Options options_;
    Fetcher fetcher_;
    quint64 nextOp_ = 1;
    quint64 nextStream_ = 1;
    quint64 nextRefresh_ = 1;
    std::map<quint64, Op> ops_;
    std::deque<quint64> prefetches_;
    std::map<QByteArray, Origin> origins_;
    std::deque<QByteArray> originOrder_;
    std::map<QByteArray, QByteArray> probed_;   // accepted URL (as probed, and its final URL) -> final URL
    std::deque<QByteArray> probedOrder_;
    std::map<quint64, Stream> streams_;
    std::map<quint64, Deliver> refreshes_;
    FaultLedger ledger_;                         // the strike rules' memory
    struct Counters {
        qint64 resolves = 0, fastAnswers = 0, fastUnpaired = 0, fallbacks = 0, extractorAnswers = 0;
        qint64 finalFailures = 0, failed = 0, cancelled = 0, prefetchesReplaced = 0, refusedReports = 0, finalLookups = 0;
        qint64 probes = 0, probeAccepted = 0, probeRefused = 0, probeUnusable = 0, probeNoVerdict = 0;
        qint64 opened = 0, proxied = 0, direct = 0, hls = 0, proxyUnavailable = 0, refusedAtOpen = 0;
        qint64 refreshes = 0, refreshFailed = 0;
        qint64 fastAvoided = 0, faultStrikes = 0, faultRetries = 0, faultAvoids = 0, faultsNotTheSource = 0;
        qint64 prefetchesCached = 0, prefetchesSkipped = 0;
    } counters_;
};

template <class Api> const std::size_t SharedStreamSource<Api>::maximumPrefetches;
template <class Api> const std::size_t SharedStreamSource<Api>::maximumRemembered;

}}
