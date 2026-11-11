// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "FastStreamResolver.h"
#include "../Domain/Playability.h"
#include "MediaAddress.h"
#include "PlayerResponseParser.h"
#include <QDateTime>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QTimer>
#include <QUrl>
#include <algorithm>

namespace nordtile { namespace music { namespace sharedplayback { namespace infrastructure {
using namespace domain;

struct FastStreamResolver::Job {
    std::string videoId;
    StreamRequest request;
    QString key;
    std::vector<Waiter> waiters;
    QElapsedTimer clock;
    StreamTimings timings;
    int visitorGen = 0;      // the visitor id this job asked with
};

int FastStreamResolver::Counters::medianTotalMs() const {
    if (recentTotalsMs.empty()) return -1;
    std::vector<int> sorted(recentTotalsMs);
    std::sort(sorted.begin(), sorted.end());
    return sorted[sorted.size() / 2];
}

FastStreamResolver::FastStreamResolver(const Options &options, QObject *parent) : QObject(parent), options_(options) {
    if (!options_.clock) {
        options_.clock = []() { return std::int64_t(QDateTime::currentMSecsSinceEpoch()); };
        // The default also measures durations on the monotonic clock; an injected clock (tests) drives both.
        const std::shared_ptr<QElapsedTimer> started(new QElapsedTimer);
        started->start();
        monotonic_ = [started]() { return std::int64_t(started->elapsed()); };
    }
}

// The connections are children: destroying them ends their requests without running callbacks.
FastStreamResolver::~FastStreamResolver() {
    for (std::size_t i = 0; i < connections_.size(); ++i) delete connections_[i];
    connections_.clear();
}

std::int64_t FastStreamResolver::now() const { return options_.clock(); }
// Durations (the breaker's windows, the visitor id's age, back-offs) are measured on a clock that does not jump when
// the user or the network sets the time; `now()` (wall clock) is for comparing with the provider's expiry times.
std::int64_t FastStreamResolver::mono() const { return monotonic_ ? monotonic_() : options_.clock(); }

void FastStreamResolver::logLine(const std::string &line) const {
    if (options_.log) options_.log(line);
}

bool FastStreamResolver::allowed() const { return breaker_.allows(mono()); }

HttpsConnection *FastStreamResolver::idleConnection() {
    for (std::size_t i = 0; i < connections_.size(); ++i) if (!connections_[i]->busy()) return connections_[i];
    HttpsConnection *connection = new HttpsConnection(options_.playerHost, options_.playerPort, options_.ipv4Only, this);
    connection->setIgnoreSslErrorsForTesting(options_.ignoreSslErrorsForTesting);
    connection->setReuseFirstByteTimeoutMs(options_.reuseFirstByteTimeoutMs);
    connections_.push_back(connection);
    return connection;
}

void FastStreamResolver::warmUp() {
    ensureVisitor([]() {}, StreamTimingsHook());
}

// Mints the visitor id once and shares the work between callers that arrive meanwhile.
void FastStreamResolver::ensureVisitor(const std::function<void()> &then, StreamTimingsHook hook) {
    const bool fresh = !visitor_.empty() && mono() - visitorMintedAt_ < options_.visitorLifetimeMs;
    if (fresh) { if (hook) hook(-1); then(); return; }
    // A mint that just failed is not repeated on every tap: the caller finds no visitor id and goes elsewhere.
    if (!minting_ && mono() < visitorRetryAt_) { if (hook) hook(-1); then(); return; }
    QElapsedTimer *timer = new QElapsedTimer; timer->start();
    afterVisitor_.push_back([then, hook, timer]() {
        const int ms = int(timer->elapsed());
        delete timer;
        if (hook) hook(ms);
        then();
    });
    if (minting_) return;
    minting_ = true;
    HttpsRequest request;
    request.method = "POST";
    request.path = "/youtubei/v1/visitor_id?prettyPrint=false";
    request.headers.push_back(Header("Content-Type", "application/json"));
    request.headers.push_back(Header("User-Agent", options_.client.userAgent));
    request.headers.push_back(Header("Origin", "https://www.youtube.com"));
    request.headers.push_back(Header("Accept-Encoding", "identity"));
    request.body = QByteArray::fromStdString(visitorRequestBody());
    request.timeoutMs = options_.visitorTimeoutMs;
    request.maxBodyBytes = 64 * 1024;
    idleConnection()->send(request, [this](const HttpsResult &result) {
        if (result.transportOk && result.status == 200) {
            const QJsonObject root = QJsonDocument::fromJson(result.body).object();
            const QString data = root.value(QStringLiteral("responseContext")).toObject().value(QStringLiteral("visitorData")).toString();
            bool plain = !data.isEmpty() && data.size() <= 256;
            for (int i = 0; plain && i < data.size(); ++i) {
                const QChar c = data.at(i);
                plain = c.isLetterOrNumber() || c == QLatin1Char('%') || c == QLatin1Char('_') || c == QLatin1Char('-') || c == QLatin1Char('=');
            }
            if (plain) { visitor_ = data.toStdString(); visitorMintedAt_ = mono(); ++visitorGeneration_; visitorFailures_ = 0; visitorRetryAt_ = 0; }
        }
        if (visitor_.empty() || mono() - visitorMintedAt_ >= options_.visitorLifetimeMs) {
            // Back off: 15 s, one minute, five minutes. A refusal (403, 429) is also a strike; a network failure is not.
            if (result.transportOk && (result.status == 403 || result.status == 429)) breaker_.recordStrike(mono());
            if (visitor_.empty() || !(result.transportOk && result.status == 200)) {
                ++visitorFailures_;
                visitorRetryAt_ = mono() + (visitorFailures_ <= 1 ? 15000 : visitorFailures_ == 2 ? 60000 : 300000);
            }
        }
        logLine(std::string("sailtune.fast phase visitor ok ") + (visitor_.empty() ? "false" : "true") +
                " status " + std::to_string(result.status) + " class " + (result.statusClass.empty() ? "none" : result.statusClass));
        minting_ = false;
        std::vector<std::function<void()> > continuations;
        continuations.swap(afterVisitor_);
        for (std::size_t i = 0; i < continuations.size(); ++i) continuations[i]();
    });
}

FastStreamResolver::RequestId FastStreamResolver::resolve(const std::string &videoId, const StreamRequest &request,
                                                          const Callback &callback) {
    const RequestId id = nextId_++;
    const auto reply = [this, callback](const ResolveOutcome &outcome) {
        QTimer::singleShot(0, this, [callback, outcome]() { callback(outcome); });
    };
    ResolveOutcome refusal;
    if (!validVideoId(videoId)) {
        refusal.failure = FailureKind::BadResponse; refusal.reason = "invalid_id"; refusal.fallback = false;
        reply(refusal); return id;
    }
    if (!allowed()) {
        refusal.failure = FailureKind::BreakerOpen; refusal.reason = "breaker_open"; refusal.fallback = true;
        reply(refusal); return id;
    }
    const QString key = QString::fromStdString(request.cacheKey(videoId));
    if (const StreamSet *hit = cache_.get(key.toStdString(), now())) {
        ResolveOutcome outcome;
        outcome.streams = *hit;
        outcome.streams.path = ResolvePath::Cache;
        outcome.streams.timings = StreamTimings();
        outcome.streams.timings.totalMs = 0;
        ++counters_.cache;
        logLine("sailtune.fast phase resolve path cache total_ms 0");
        reply(outcome); return id;
    }
    std::shared_ptr<Job> job = inFlight_.value(key);
    Waiter waiter; waiter.id = id; waiter.callback = callback;
    if (job) { job->waiters.push_back(waiter); return id; }   // single flight
    job.reset(new Job);
    job->videoId = videoId; job->request = request; job->key = key; job->waiters.push_back(waiter);
    job->clock.start();
    inFlight_.insert(key, job);
    // Start from the event loop so that a callback of the caller never runs inside resolve().
    QTimer::singleShot(0, this, [this, job]() { begin(job); });
    return id;
}

void FastStreamResolver::cancel(RequestId id) {
    for (QHash<QString, std::shared_ptr<Job> >::iterator it = inFlight_.begin(); it != inFlight_.end(); ++it) {
        std::vector<Waiter> &waiters = it.value()->waiters;
        for (std::size_t i = 0; i < waiters.size(); ++i) {
            if (waiters[i].id != id) continue;
            const Callback callback = waiters[i].callback;
            waiters.erase(waiters.begin() + long(i));
            ResolveOutcome outcome; outcome.failure = FailureKind::Cancelled; outcome.reason = "cancelled"; outcome.fallback = false;
            QTimer::singleShot(0, this, [callback, outcome]() { callback(outcome); });
            return;
        }
    }
}

void FastStreamResolver::invalidate(const std::string &videoId) { cache_.invalidateVideo(videoId); }

void FastStreamResolver::reportRefused(const std::string &videoId, std::int64_t msLeft) {
    cache_.invalidateVideo(videoId);
    if (msLeft >= 60 * 1000) {
        breaker_.recordStrike(mono());
        logLine(std::string("sailtune.fast phase strike breaker_open ") + (breaker_.allows(mono()) ? "false" : "true"));
    }
}

void FastStreamResolver::begin(const std::shared_ptr<Job> &job) {
    ensureVisitor([this, job]() {
        if (visitor_.empty()) {
            // The provider gave none (or a mint failed a moment ago): asking for the video blind would only be a bot wall.
            ResolveOutcome outcome; outcome.failure = FailureKind::Network; outcome.reason = "no_visitor"; outcome.fallback = true;
            ++counters_.network;
            complete(job, outcome); return;
        }
        requestPlayer(job);
    }, [job](int ms) { job->timings.visitorMs = ms; });
}

void FastStreamResolver::requestPlayer(const std::shared_ptr<Job> &job) {
    HttpsRequest request;
    request.method = "POST";
    request.path = "/youtubei/v1/player?prettyPrint=false";
    const std::vector<Header> headers = playerRequestHeaders(options_.client, visitor_);
    for (std::size_t i = 0; i < headers.size(); ++i) request.headers.push_back(headers[i]);
    request.body = QByteArray::fromStdString(playerRequestBody(options_.client, job->videoId));
    request.timeoutMs = options_.playerTimeoutMs;
    request.maxBodyBytes = 4 * 1024 * 1024;
    if (request.body.isEmpty()) {
        ResolveOutcome outcome; outcome.failure = FailureKind::BadResponse; outcome.reason = "bad_client"; outcome.fallback = true;
        complete(job, outcome); return;
    }
    job->visitorGen = visitorGeneration_;
    QElapsedTimer *timer = new QElapsedTimer; timer->start();
    idleConnection()->send(request, [this, job, timer](const HttpsResult &result) {
        job->timings.playerMs = int(timer->elapsed());
        delete timer;
        job->timings.reusedConnection = result.connectionReused;
        ResolveOutcome outcome;
        if (!result.transportOk) {
            ++counters_.network;
            outcome.failure = FailureKind::Network; outcome.reason = result.statusClass.empty() ? "network" : result.statusClass; outcome.fallback = true;
            complete(job, outcome); return;
        }
        if (result.status != 200) {
            // The provider turning the client away (403) or rate limiting it (429) says something about the client, not the network.
            if (result.status == 403 || result.status == 429) {
                breaker_.recordStrike(mono());
                logLine("sailtune.fast phase strike player_http_" + std::to_string(result.status) + " breaker_open " + (breaker_.allows(mono()) ? "false" : "true"));
            }
            outcome.failure = FailureKind::BadResponse; outcome.reason = "http_" + std::to_string(result.status); outcome.fallback = true;
            complete(job, outcome); return;
        }
        QElapsedTimer parse; parse.start();
        const ParsedPlayer parsed = parsePlayerResponse(result.body, job->videoId, job->request, now());
        job->timings.parseMs = int(parse.elapsed());
        if (parsed.failure != FailureKind::None) {
            outcome.failure = parsed.failure; outcome.reason = parsed.reason;
            outcome.fallback = parsed.failure != FailureKind::Final;
            if (parsed.failure == FailureKind::BotWall) {
                // The visitor id is what the provider judged: start over with a new one. Requests that shared that id
                // and ran at the same time are one verdict, not several (and a late one never discards a newer id).
                if (job->visitorGen == visitorGeneration_) {
                    visitor_.clear();
                    ++visitorGeneration_;
                    breaker_.recordBotWall(mono());
                    ++counters_.botWalls;
                }
            }
            if (parsed.failure == FailureKind::NoUsableFormat) {
                // One video without plain addresses is normal; several in a row say the client no longer yields them.
                if (++noFormatStreak_ >= 3) breaker_.recordStrike(mono());
            }
            logLine("sailtune.fast phase player ok false class " + std::string(failureLabel(parsed.failure)) + " reason " + parsed.reason +
                    " kept " + std::to_string(parsed.keptFormats) + " dropped " + std::to_string(parsed.droppedFormats));
            complete(job, outcome); return;
        }
        StreamSet set = parsed.set;
        set.userAgent = options_.client.userAgent;
        set.path = ResolvePath::Fast;
        logLine("sailtune.fast phase player ok true kept " + std::to_string(parsed.keptFormats) + " dropped " + std::to_string(parsed.droppedFormats) +
                " audio " + std::to_string(set.audio.size()) + " muxed " + std::to_string(set.muxed.size()) + " top_itag " +
                std::to_string(set.top()->itag) + " top_codec " + set.top()->codecFamily() + " player_ms " + std::to_string(job->timings.playerMs) +
                " reused " + (result.connectionReused ? "true" : "false"));
        if (options_.probe) probeTop(job, set);
        else { ResolveOutcome ok; ok.streams = set; complete(job, ok); }
    });
}

namespace {
// "bytes 0-1023/10000000" -> 10000000; -1 when absent or malformed.
std::int64_t contentRangeTotal(const std::string &value) {
    const std::size_t slash = value.find('/');
    if (value.compare(0, 6, "bytes ") != 0 || slash == std::string::npos || slash + 1 >= value.size()) return -1;
    std::int64_t total = 0;
    for (std::size_t i = slash + 1; i < value.size(); ++i) {
        if (value[i] < '0' || value[i] > '9' || total > (std::int64_t(1) << 50)) return -1;
        total = total * 10 + (value[i] - '0');
    }
    return total;
}
}  // namespace

void FastStreamResolver::probeUrl(const std::string &url, const std::string &userAgent, std::int64_t contentLength, int timeoutMs,
                                  const ProbeDone &done, int hopsLeft, int elapsedSoFar) {
    const QUrl parsed(QString::fromStdString(url), QUrl::StrictMode);
    QString host = parsed.host(); quint16 port = 443;
    QString connectHost = host;
    if (options_.mediaEndpointOverride) options_.mediaEndpointOverride(host, &connectHost, &port);
    HttpsConnection *connection = new HttpsConnection(connectHost, port, options_.ipv4Only, this);
    connection->setIgnoreSslErrorsForTesting(options_.ignoreSslErrorsForTesting);
    HttpsRequest request;
    request.method = "GET";
    request.path = parsed.path(QUrl::FullyEncoded).toStdString() + (parsed.hasQuery() ? "?" + parsed.query(QUrl::FullyEncoded).toStdString() : std::string());
    const std::vector<Header> headers = mediaRequestHeaders(userAgent);
    for (std::size_t i = 0; i < headers.size(); ++i) request.headers.push_back(headers[i]);
    // Exactly the first request playback makes: one bounded chunk from byte 0.
    const std::int64_t chunk = 10 * 1024 * 1024;
    const std::int64_t last = (contentLength > 0 && contentLength < chunk ? contentLength : chunk) - 1;
    request.headers.push_back(Header("Range", "bytes=0-" + std::to_string(last)));
    request.timeoutMs = timeoutMs;
    request.headersOnly = true;
    request.maxBodyBytes = 16 * 1024;
    QElapsedTimer *timer = new QElapsedTimer; timer->start();
    QPointer<FastStreamResolver> guard(this);
    connection->send(request, [this, guard, connection, timer, done, contentLength, url, userAgent, timeoutMs, hopsLeft, elapsedSoFar](const HttpsResult &result) {
        const int ms = elapsedSoFar + int(timer->elapsed());
        delete timer;
        connection->deleteLater();
        // The media host may answer with a redirect to another node of its CDN ("redirector"): follow it, a few hops, only to the
        // CDN's own hosts over HTTPS (the address that finally answers is the one handed to playback).
        if (result.transportOk && result.status >= 300 && result.status < 400 && hopsLeft > 0 && guard &&
            isMediaCdnAddress(result.location) && result.location != url) {
            logLine("sailtune.fast phase probe redirect hop " + std::to_string(4 - hopsLeft));
            probeUrl(result.location, userAgent, contentLength, timeoutMs, done, hopsLeft - 1, ms);
            return;
        }
        // 206 with the expected total accepts. A client error (403, 404, 410, 429 ...) refuses: that is the verdict on
        // the address. A redirect that could not be followed, a 200 that ignored Range or another total cannot be played
        // the way playback fetches (ranged): unusable, but nothing the provider did wrong. A transport failure or a server
        // error is no verdict (a flaky network must not push every track onto the slow path).
        ProbeVerdict verdict = ProbeVerdict::NoVerdict;
        if (result.transportOk) {
            if (result.status == 206) {
                const std::int64_t total = contentRangeTotal(result.contentRange);
                verdict = (contentLength > 0 && total > 0 && total != contentLength) ? ProbeVerdict::Unusable : ProbeVerdict::Accepted;
            } else if (result.status >= 400 && result.status < 500) verdict = ProbeVerdict::Refused;
            else if (result.status >= 500 && result.status < 600) verdict = ProbeVerdict::NoVerdict;
            else verdict = ProbeVerdict::Unusable;
        }
        done(verdict, result.status, ms, url);
    });
}

void FastStreamResolver::probeAddress(const std::string &url, const std::string &userAgent, std::int64_t contentLength,
                                      const std::function<void(ProbeVerdict, int, int)> &done) {
    probeAddress(url, userAgent, contentLength, [done](ProbeVerdict verdict, int status, int ms, const std::string &) { done(verdict, status, ms); });
}

void FastStreamResolver::probeAddress(const std::string &url, const std::string &userAgent, std::int64_t contentLength,
                                      const ProbeDone &done) {
    probeUrl(url, userAgent, contentLength, options_.probeTimeoutMs, [this, done](ProbeVerdict verdict, int status, int ms, const std::string &finalUrl) {
        if (verdict == ProbeVerdict::Refused) {
            breaker_.recordStrike(mono());
            ++counters_.probeRejected;
        }
        logLine(std::string("sailtune.fast phase probe verdict ") + (verdict == ProbeVerdict::Accepted ? "accept" : verdict == ProbeVerdict::Refused ? "reject" :
                verdict == ProbeVerdict::Unusable ? "unusable" : "none") + " status " + std::to_string(status) + " ms " + std::to_string(ms) + " rung other");
        done(verdict, status, ms, finalUrl);
    }, 3, 0);
}

bool FastStreamResolver::peekCached(const std::string &videoId, const StreamRequest &request, StreamSet *out) const {
    if (!validVideoId(videoId)) return false;
    return cache_.peek(request.cacheKey(videoId), now(), out);
}

void FastStreamResolver::networkChanged() {
    cache_.clear();
    visitor_.clear();
    ++visitorGeneration_;
    visitorFailures_ = 0; visitorRetryAt_ = 0;
    for (std::size_t i = 0; i < connections_.size(); ++i) if (!connections_[i]->busy()) connections_[i]->abort();
    logLine("sailtune.fast phase network_changed");
}

void FastStreamResolver::probeTop(const std::shared_ptr<Job> &job, StreamSet set) {
    struct Join { int pending = 0; int refused = 0; int unusable = 0; int status = 0; int ms = 0; bool anyAccepted = false; std::string finalUrl[2]; };
    std::shared_ptr<Join> join(new Join);
    std::vector<std::pair<std::string, std::int64_t> > urls;
    urls.push_back(std::make_pair(set.top()->url, set.top()->contentLength));
    if (!job->request.audioOnly && !set.video.empty()) urls.push_back(std::make_pair(set.video.front().url, set.video.front().contentLength));
    set.probedUrl = set.top()->url;
    join->pending = int(urls.size());
    for (std::size_t i = 0; i < urls.size(); ++i) {
        probeUrl(urls[i].first, set.userAgent, urls[i].second, options_.probeTimeoutMs,
                 [this, job, set, join, i](ProbeVerdict verdict, int status, int ms, const std::string &finalUrl) {
            if (verdict == ProbeVerdict::Refused) { ++join->refused; join->status = status; }
            else if (verdict == ProbeVerdict::Unusable) { ++join->unusable; join->status = status; }
            else if (verdict == ProbeVerdict::Accepted) join->anyAccepted = true;
            if (verdict == ProbeVerdict::Accepted) join->finalUrl[i] = finalUrl;
            if (ms > join->ms) join->ms = ms;
            if (--join->pending > 0) return;
            job->timings.probeMs = join->ms;
            ResolveOutcome outcome;
            if (join->refused > 0) {
                breaker_.recordStrike(mono());
                ++counters_.probeRejected;
                outcome.failure = FailureKind::ProbeRejected; outcome.reason = "http_" + std::to_string(join->status); outcome.fallback = true;
                logLine("sailtune.fast phase probe verdict reject status " + std::to_string(join->status) + " ms " + std::to_string(join->ms));
            } else if (join->unusable > 0) {
                outcome.failure = FailureKind::ProbeRejected; outcome.reason = "unusable_" + std::to_string(join->status); outcome.fallback = true;
                logLine("sailtune.fast phase probe verdict unusable status " + std::to_string(join->status) + " ms " + std::to_string(join->ms));
            } else {
                outcome.streams = set;
                // A redirect that was followed: the address that answered replaces the one the player gave.
                StreamFormat *top = outcome.streams.audio.empty() ? (outcome.streams.muxed.empty() ? nullptr : &outcome.streams.muxed.front())
                                                                  : &outcome.streams.audio.front();
                if (top && !join->finalUrl[0].empty() && join->finalUrl[0] != top->url) { top->url = join->finalUrl[0]; outcome.streams.probedUrl = top->url; }
                if (join->finalUrl[1].size() && !outcome.streams.video.empty() && join->finalUrl[1] != outcome.streams.video.front().url)
                    outcome.streams.video.front().url = join->finalUrl[1];
                logLine("sailtune.fast phase probe verdict " + std::string(join->anyAccepted ? "accept" : "none") + " ms " + std::to_string(join->ms));
            }
            complete(job, outcome);
        }, 3, 0);
    }
}

void FastStreamResolver::complete(const std::shared_ptr<Job> &job, ResolveOutcome outcome) {
    job->timings.totalMs = int(job->clock.elapsed());
    if (outcome.ok()) {
        outcome.streams.timings = job->timings;
        outcome.streams.videoId = job->videoId;
        ++counters_.fast;
        counters_.recentTotalsMs.push_back(job->timings.totalMs);
        if (counters_.recentTotalsMs.size() > 32) counters_.recentTotalsMs.erase(counters_.recentTotalsMs.begin());
        breaker_.recordSuccess();
        noFormatStreak_ = 0;
        cache_.put(job->key.toStdString(), outcome.streams, now());
    } else if (outcome.fallback) {
        ++counters_.fallbacks;
    }
    logLine("sailtune.fast phase resolve path " + std::string(outcome.ok() ? "fast" : "none") + " result " + failureLabel(outcome.failure) +
            " total_ms " + std::to_string(job->timings.totalMs) + " visitor_ms " + std::to_string(job->timings.visitorMs) +
            " player_ms " + std::to_string(job->timings.playerMs) + " parse_ms " + std::to_string(job->timings.parseMs) +
            " probe_ms " + std::to_string(job->timings.probeMs));
    inFlight_.remove(job->key);
    const std::vector<Waiter> waiters = job->waiters;
    job->waiters.clear();
    deliver(waiters, outcome);
}

void FastStreamResolver::deliver(const std::vector<Waiter> &waiters, const ResolveOutcome &outcome) {
    for (std::size_t i = 0; i < waiters.size(); ++i) {
        const Callback callback = waiters[i].callback;
        QTimer::singleShot(0, this, [callback, outcome]() { callback(outcome); });
    }
}

}}}}
