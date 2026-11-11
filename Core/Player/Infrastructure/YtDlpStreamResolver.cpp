// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// yt-dlp fallback adapter; see YtDlpStreamResolver.h.
#include "YtDlpStreamResolver.h"
#include "SharedStreamSourceLadder.h"
#include "YtDlpStreamMapping.h"

#include <QDateTime>
#include <QPointer>
#include <QString>
#include <QTimer>
#include <algorithm>
#include <vector>

namespace nordtile { namespace video { namespace playeradapter {

using playback::StreamError;
using playback::StreamErrorClass;
using playback::StreamRequest;
using playback::StreamSet;

namespace {
const std::size_t timingSamples = 31;

qint64 median(const std::deque<qint64> &samples) {
    if (samples.empty()) return -1;
    std::vector<qint64> v(samples.begin(), samples.end());
    std::nth_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(v.size() / 2), v.end());
    return v[v.size() / 2];
}

player::CacheFacts factsOf(const StreamSet &s) {
    player::CacheFacts f;
    f.videoId = s.videoId.toStdString();
    f.live = s.liveState == playback::LiveState::Live || s.live;
    f.hls = !s.hlsManifest.isEmpty();
    f.expiresAtUnixMs = s.expiresAtUnixMs;
    return f;
}

StreamSet emptyAnswer(const QString &videoId) {
    StreamSet s;
    s.videoId = videoId;
    return s;
}

// The shared runtime's temporary-file budget (Shared/Runtime/Python/resolver.py): Failed, with a
// label of its own so that the session does not ask again by itself (the same budget is met).
StreamError storageLimitError(bool signedIn) {
    StreamError e = playback::streamError(StreamErrorClass::Failed, signedIn);
    e.label = QLatin1String(resolver::failureReasonLabel(resolver::FailureReason::StorageLimit));
    return e;
}
} // namespace

StreamError YtDlpStreamResolver::errorFor(resolver::ErrorCode code, bool signedIn) {
    return errorFor(code, resolver::FailureReason::None, signedIn);
}

StreamError YtDlpStreamResolver::errorFor(resolver::ErrorCode code, resolver::FailureReason reason, bool signedIn) {
    using resolver::ErrorCode;
    using resolver::FailureReason;
    StreamErrorClass e = StreamErrorClass::Failed;
    switch (code) {
    case ErrorCode::None: return StreamError();
    case ErrorCode::InvalidRequest: e = StreamErrorClass::InvalidId; break;
    case ErrorCode::RuntimeUnavailable: e = StreamErrorClass::RuntimeUnavailable; break;
    case ErrorCode::Busy: e = StreamErrorClass::Busy; break;
    case ErrorCode::Cancelled: e = StreamErrorClass::Cancelled; break;
    case ErrorCode::Timeout: e = StreamErrorClass::Timeout; break;
    case ErrorCode::Network:
        e = reason == FailureReason::RateLimited ? StreamErrorClass::TryLater : StreamErrorClass::Network;
        break;
    case ErrorCode::BotCheck: e = StreamErrorClass::BotCheck; break;
    case ErrorCode::AgeRestricted: e = StreamErrorClass::AgeRestricted; break;
    case ErrorCode::PrivateVideo: e = StreamErrorClass::Private; break;
    case ErrorCode::VideoUnavailable:
        // Core folds the final reasons into one code; the reason tells them apart (RES-09).
        if (reason == FailureReason::Removed) e = StreamErrorClass::Removed;
        else if (reason == FailureReason::Terminated) e = StreamErrorClass::Terminated;
        else if (reason == FailureReason::GeoBlocked) e = StreamErrorClass::GeoBlocked;
        else if (reason == FailureReason::MembersOnly) e = StreamErrorClass::SignInRequired;
        else if (reason == FailureReason::RateLimited) e = StreamErrorClass::TryLater;
        else {
            // A bare "video unavailable": the next pass or a fresh resolve may differ (not final).
            StreamError bare = playback::streamError(StreamErrorClass::Unavailable, signedIn);
            bare.final = false;
            return bare;
        }
        break;
    case ErrorCode::Upcoming: e = StreamErrorClass::Upcoming; break;
    case ErrorCode::InvalidResponse: e = StreamErrorClass::InvalidResponse; break;
    case ErrorCode::NoPlayableStream:
        e = reason == FailureReason::DeliveryChanged ? StreamErrorClass::DeliveryChanged : StreamErrorClass::NoPlayableStream;
        break;
    case ErrorCode::Failed:
        // The runtime's temporary-file budget stays Failed (never "reinstall"), under its own label.
        if (reason == FailureReason::StorageLimit) return storageLimitError(signedIn);
        e = reason == FailureReason::DeliveryChanged ? StreamErrorClass::DeliveryChanged
            : reason == FailureReason::RateLimited ? StreamErrorClass::TryLater
            : reason == FailureReason::MembersOnly ? StreamErrorClass::SignInRequired : StreamErrorClass::Failed;
        break;
    }
    StreamError err = playback::streamError(e, signedIn);
    // Members-only: widening over clients cannot change it; with cookies (signed in) it may.
    if (e == StreamErrorClass::SignInRequired) err.final = !signedIn;
    return err;
}

playback::StreamStoryboard YtDlpStreamResolver::storyboardOf(const resolver::StoryboardLevel &level) {
    playback::StreamStoryboard board;
    if (level.empty()) return board;
    board.width = level.width;
    board.height = level.height;
    board.columns = level.columns;
    board.rows = level.rows;
    for (const resolver::StoryboardSheet &sheet : level.sheets) {
        playback::StoryboardSheet s;
        s.url = QUrl(QString::fromStdString(sheet.url), QUrl::StrictMode);
        s.durationMs = sheet.durationMs;
        board.sheets.append(s);
    }
    return board;
}

StreamError YtDlpStreamResolver::unplayableError(const StreamSet &streams, resolver::FailureReason reason,
                                                 bool formatsWithoutUrl, bool signedIn) {
    using resolver::FailureReason;
    StreamErrorClass e = StreamErrorClass::NoPlayableStream;
    const bool anyUrl = !streams.video.isEmpty() || !streams.audio.isEmpty() || !streams.muxed.isEmpty()
        || !streams.hlsManifest.isEmpty();
    if (streams.liveState == playback::LiveState::Upcoming) e = StreamErrorClass::Upcoming;
    else if (streams.liveState == playback::LiveState::PostLive) e = StreamErrorClass::StillProcessing;
    else if (reason == FailureReason::RateLimited) e = StreamErrorClass::TryLater;
    else if (reason == FailureReason::StorageLimit) return storageLimitError(signedIn);
    else if (reason == FailureReason::MembersOnly) e = StreamErrorClass::SignInRequired;
    // Formats with URLs that do not fit this request or device are NoPlayableStream (a true device
    // limit). No format with a URL: the delivery changed when the run shows it (SABR-only, a failed
    // challenge, a missing PO token, formats listed without a usable URL); an answer that only
    // lacks formats says no more than "unavailable" (not final).
    else if (!anyUrl && (reason == FailureReason::DeliveryChanged || formatsWithoutUrl)) e = StreamErrorClass::DeliveryChanged;
    else if (!anyUrl) {
        StreamError bare = playback::streamError(StreamErrorClass::Unavailable, signedIn);
        bare.final = false;
        return bare;
    }
    StreamError err = playback::streamError(e, signedIn);
    if (e == StreamErrorClass::SignInRequired) err.final = !signedIn;
    return err;
}

YtDlpStreamResolver::YtDlpStreamResolver(resolver::YtDlpVideoService *service, QObject *parent,
                                         const player::CachePolicy &policy, Clock clock)
    : QObject(parent), service_(service), clock_(std::move(clock)), cache_(policy) {}

YtDlpStreamResolver::~YtDlpStreamResolver() {
    // Completions of these runs are guarded and will find this object gone; queued flushes are
    // bound to this object and are dropped with it.
    for (const auto &f : flights_) service_->cancel(f.second.run);
}

qint64 YtDlpStreamResolver::now() const {
    return clock_ ? clock_() : QDateTime::currentMSecsSinceEpoch();
}

quint64 YtDlpStreamResolver::resolve(const StreamRequest &request, Completion completion) {
    return begin(request, std::move(completion), false);
}

quint64 YtDlpStreamResolver::nameFinal(const StreamRequest &request, Completion completion) {
    return begin(request, std::move(completion), true);
}

quint64 YtDlpStreamResolver::begin(const StreamRequest &request, Completion completion, bool singlePass) {
    const quint64 op = nextOperation_++;
    Waiter w;
    w.completion = std::move(completion);
    w.videoId = request.videoId;
    w.key = playback::streamCacheKey(request).toStdString();
    // A single-pass flight never serves a resolve that would widen (and the other way round).
    if (singlePass) w.key += "|final";
    waiters_[op] = w;
    if (!catalog::validVideoId(request.videoId.toStdString())) {
        ++counters_.failed;
        post(op, emptyAnswer(request.videoId), playback::streamError(StreamErrorClass::InvalidId, request.signedIn));
        return op;
    }
    const qint64 t = now();
    StreamSet cached;
    switch (cache_.acquire(w.key, op, t, &cached)) {
    case player::ResolveCache<StreamSet>::Outcome::Hit:
        cached.path = playback::ResolvePath::Cache;
        cached.resolveMs = 0;
        cached.phases = playback::ResolvePhases();
        ++counters_.cacheHits;
        post(op, cached, StreamError());
        break;
    case player::ResolveCache<StreamSet>::Outcome::Lead:
        startFlight(w.key, request, startsFull(request.videoId, t, singlePass), t, singlePass);
        break;
    case player::ResolveCache<StreamSet>::Outcome::Join:
        QTimer::singleShot(static_cast<int>(std::min<qint64>(cache_.policy().joinLimitMs + 50, 600000)),
                           this, [this]() { checkJoins(); });
        break;
    }
    return op;
}

void YtDlpStreamResolver::startFlight(const std::string &key, const StreamRequest &request, bool fullProfile,
                                      qint64 startedMs, bool singlePass) {
    Flight f;
    f.token = nextToken_++;
    f.fullProfile = fullProfile;
    f.singlePass = singlePass;
    f.request = request;
    f.startedMs = startedMs;
    flights_[key] = f;
    const std::uint64_t token = f.token;
    QPointer<QObject> guard(this);
    // RES-20: details() builds its arguments from the runner's common set, which has no cookie,
    // login or browser-profile option; signedIn never changes the run.
    // RES-14: the provider's arguments only when the request says the helper is ready. This
    // adapter has two passes (token-free, then widened); it has no token-forced RES-08 step 4,
    // so it never forces a token. DetailsOptions changes nothing on STORE.
    resolver::YtDlpVideoService::DetailsOptions options;
    options.poProvider = request.poProvider;
    options.forcePoToken = false;
    const quint64 run = service_->detailsReport(request.videoId, fullProfile, options,
        [this, guard, key, token](const resolver::YtDlpVideoService::DetailsReport &report, const catalog::VideoDetails &details) {
        if (!guard) return;
        runFinished(key, token, report, details);
    });
    auto it = flights_.find(key);
    if (it != flights_.end() && it->second.token == token) it->second.run = run;
}

void YtDlpStreamResolver::runFinished(const std::string &key, std::uint64_t token,
                                      const resolver::YtDlpVideoService::DetailsReport &report,
                                      const catalog::VideoDetails &details) {
    auto it = flights_.find(key);
    if (it == flights_.end() || it->second.token != token) return; // abandoned or superseded run
    const Flight flight = it->second;
    const bool signedIn = flight.request.signedIn;
    StreamError error = errorFor(report.error, report.reason, signedIn);
    StreamSet streams;
    if (report.error == resolver::ErrorCode::None) {
        if (QString::fromStdString(details.summary.videoId) != flight.request.videoId) {
            error = playback::streamError(StreamErrorClass::InvalidResponse, signedIn);
        } else {
            streams = toStreamSet(details, flight.request);
            streams.storyboard = storyboardOf(report.storyboard);
            // RES-19: the scheduled start of an upcoming video (yt-dlp's release_timestamp).
            if (streams.liveState == playback::LiveState::Upcoming && report.scheduledStartUnix > 0)
                streams.scheduledStartUnixMs = report.scheduledStartUnix * 1000;
            if (!player::playable(streams, flight.request))
                error = unplayableError(streams, report.reason, report.formatsWithoutUrl > 0, signedIn);
        }
    }
    // RES-08 step 2: one widened pass after a non-final failure; not for rate limiting, and never
    // when only the reason of a final answer was asked for (nameFinal).
    if (!error.ok() && !flight.fullProfile && !flight.singlePass && !error.final
            && error.kind != StreamErrorClass::Cancelled && error.kind != StreamErrorClass::TryLater) {
        // The token-free pass failed for this video: its next extractions start with the full
        // profile. Not after a pass that never got an answer (no network, a timeout, no free
        // slot, no runtime): that says nothing about the clients it asked.
        if (error.kind != StreamErrorClass::Network && error.kind != StreamErrorClass::Timeout
                && error.kind != StreamErrorClass::Busy && error.kind != StreamErrorClass::RuntimeUnavailable)
            fullProfile_.set(flight.request.videoId.toStdString(), now() + fullProfileMs);
        startFlight(key, flight.request, true, flight.startedMs);
        return;
    }
    flights_.erase(it);
    const qint64 t = now();
    if (error.ok()) {
        streams.path = flight.fullProfile ? playback::ResolvePath::ExtractorWithJavaScript : playback::ResolvePath::Extractor;
        streams.resolveMs = t - flight.startedMs;
        streams.phases.requestMs = streams.resolveMs; // core reports no run/queue split (R-CORE-6)
        ++(flight.fullProfile ? counters_.extractorWithJavaScript : counters_.extractor);
        extractorMs_.push_back(streams.resolveMs);
        if (extractorMs_.size() > timingSamples) extractorMs_.pop_front();
    } else {
        // RES-19: an upcoming failure keeps the live state and the start for the session.
        const bool upcoming = error.kind == StreamErrorClass::Upcoming;
        const StreamSet answered = streams;
        streams = emptyAnswer(flight.request.videoId);
        if (upcoming) {
            streams.liveState = playback::LiveState::Upcoming;
            streams.scheduledStartUnixMs = answered.scheduledStartUnixMs > 0 ? answered.scheduledStartUnixMs
                                                                              : report.scheduledStartUnix * 1000;
        }
        ++counters_.failed;
    }
    // Live answers and failures are refused by the cache itself (cacheableUntilMs / null).
    const std::vector<std::uint64_t> ops = cache_.finish(key, error.ok() ? &streams : nullptr, factsOf(streams), t);
    for (std::uint64_t op : ops) {
        forgetPrefetch(op);
        post(op, streams, error);
    }
}

void YtDlpStreamResolver::post(quint64 op, const StreamSet &streams, const StreamError &error) {
    auto it = waiters_.find(op);
    if (it == waiters_.end()) return;
    if (!it->second.completion) { waiters_.erase(it); return; } // prefetch: nothing to deliver
    if (it->second.ready) return; // already answered (e.g. cancelled); exactly once
    it->second.ready = true;
    it->second.streams = streams;
    it->second.error = error;
    QTimer::singleShot(0, this, [this, op]() { flush(op); });
}

void YtDlpStreamResolver::flush(quint64 op) {
    auto it = waiters_.find(op);
    if (it == waiters_.end() || !it->second.ready) return;
    const Completion completion = std::move(it->second.completion);
    const StreamSet streams = it->second.streams;
    const StreamError error = it->second.error;
    waiters_.erase(it);
    if (completion) completion(streams, error);
}

void YtDlpStreamResolver::checkJoins() {
    for (std::uint64_t op : cache_.overdueJoiners(now())) {
        auto it = waiters_.find(op);
        if (it == waiters_.end()) continue;
        ++counters_.failed;
        post(op, emptyAnswer(it->second.videoId), playback::streamError(StreamErrorClass::Timeout, false));
    }
}

void YtDlpStreamResolver::dropWaiter(quint64 op) {
    auto it = waiters_.find(op);
    if (it == waiters_.end()) return;
    const std::string key = it->second.key;
    forgetPrefetch(op);
    if (!cache_.release(op)) return;
    // Nobody waits for this flight any more.
    auto f = flights_.find(key);
    if (f == flights_.end()) return;
    const quint64 run = f->second.run;
    flights_.erase(f);
    service_->cancel(run);
}

void YtDlpStreamResolver::cancel(quint64 op) {
    auto it = waiters_.find(op);
    if (it == waiters_.end() || it->second.ready) return; // unknown, or its answer is already queued
    const QString videoId = it->second.videoId;
    dropWaiter(op);
    ++counters_.cancelled;
    post(op, emptyAnswer(videoId), playback::streamError(StreamErrorClass::Cancelled, false));
}

void YtDlpStreamResolver::forgetPrefetch(quint64 op) {
    prefetches_.erase(std::remove(prefetches_.begin(), prefetches_.end(), op), prefetches_.end());
}

void YtDlpStreamResolver::prefetch(const StreamRequest &request) {
    if (!catalog::validVideoId(request.videoId.toStdString())) return;
    const quint64 op = nextOperation_++;
    Waiter w;
    w.videoId = request.videoId;
    w.key = playback::streamCacheKey(request).toStdString();
    waiters_[op] = w;
    const qint64 t = now();
    switch (cache_.acquire(w.key, op, t, nullptr)) {
    case player::ResolveCache<StreamSet>::Outcome::Hit:
        waiters_.erase(op);
        return;
    case player::ResolveCache<StreamSet>::Outcome::Join: // already being resolved
        cache_.release(op);
        waiters_.erase(op);
        return;
    case player::ResolveCache<StreamSet>::Outcome::Lead:
        break;
    }
    prefetches_.push_back(op);
    startFlight(w.key, request, startsFull(request.videoId, t, false), t);
    // RES-11: the newest speculative resolve replaces the oldest one.
    while (prefetches_.size() > maximumPrefetches) {
        const quint64 oldest = prefetches_.front();
        dropWaiter(oldest);
        waiters_.erase(oldest);
    }
}

void YtDlpStreamResolver::invalidate(const QString &videoId) { cache_.invalidate(videoId.toStdString()); }

void YtDlpStreamResolver::reportRefused(const QString &videoId, const playback::StreamFormat &) {
    reportFault(videoId.toStdString(), player::SourceFault::Rejected);
}

void YtDlpStreamResolver::reportFault(const std::string &videoId, player::SourceFault fault) {
    cache_.invalidate(videoId);
    // Only a refusal says something about the source: whatever produced it (possibly the
    // token-free pass) is not tried first again for a while.
    if (fault == player::SourceFault::Rejected && catalog::validVideoId(videoId))
        fullProfile_.set(videoId, now() + fullProfileMs);
}

bool YtDlpStreamResolver::startsFull(const QString &videoId, qint64 nowMs, bool singlePass) {
    if (singlePass || !fullProfile_.active(videoId.toStdString(), nowMs)) return false;
    ++counters_.startedFull;
    return true;
}

bool YtDlpStreamResolver::peekCached(const StreamRequest &request, StreamSet *out) const {
    StreamSet cached;
    if (!cache_.peek(playback::streamCacheKey(request).toStdString(), now(), &cached)) return false;
    if (out) {
        cached.path = playback::ResolvePath::Cache;
        cached.resolveMs = 0;
        cached.phases = playback::ResolvePhases();
        *out = cached;
    }
    return true;
}

QVariantMap YtDlpStreamResolver::diagnostics() const {
    QVariantMap d;
    d.insert(QStringLiteral("extractor"), counters_.extractor);
    d.insert(QStringLiteral("extractorWithJavaScript"), counters_.extractorWithJavaScript);
    d.insert(QStringLiteral("failed"), counters_.failed);
    d.insert(QStringLiteral("cacheHits"), counters_.cacheHits);
    d.insert(QStringLiteral("cancelled"), counters_.cancelled);
    d.insert(QStringLiteral("medianExtractorMs"), median(extractorMs_));
    d.insert(QStringLiteral("cacheEntries"), static_cast<qint64>(cache_.size()));
    d.insert(QStringLiteral("startedFull"), counters_.startedFull);
    d.insert(QStringLiteral("fullProfileVideos"), static_cast<qint64>(fullProfile_.activeCount(now())));
    return d; // no fast path and no breaker in this adapter
}

quint64 DirectStreamFetcher::open(const playback::StreamFormat &format, Opened opened) {
    const quint64 handle = next_++;
    Pending p;
    p.opened = std::move(opened);
    p.url = directUrl(format, &p.error);
    if (p.error.ok()) ++opened_;
    else ++refused_;
    open_[handle] = p;
    QTimer::singleShot(0, this, [this, handle]() {
        auto it = open_.find(handle);
        if (it == open_.end() || it->second.delivered) return;
        it->second.delivered = true;
        const Opened cb = it->second.opened;
        const QUrl url = it->second.url;
        const playback::StreamError error = it->second.error;
        if (!error.ok()) open_.erase(it);
        if (cb) cb(url, error);
    });
    return handle;
}

void DirectStreamFetcher::close(quint64 stream) {
    auto it = open_.find(stream);
    if (it == open_.end()) return;
    const bool delivered = it->second.delivered;
    const Opened cb = it->second.opened;
    open_.erase(it);
    if (delivered || !cb) return;
    // Closed before delivery: the completion still runs once, as Cancelled.
    QTimer::singleShot(0, this, [cb]() { cb(QUrl(), playback::streamError(StreamErrorClass::Cancelled, false)); });
}

QVariantMap DirectStreamFetcher::diagnostics() const {
    QVariantMap d;
    d.insert(QStringLiteral("opened"), opened_);
    d.insert(QStringLiteral("open"), static_cast<qint64>(open_.size()));
    d.insert(QStringLiteral("refused"), refused_);
    return d;
}

}}}
