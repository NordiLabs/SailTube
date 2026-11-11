// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once
// The fast path: one direct POST to the provider's player endpoint as a client whose formats come
// with plain addresses (no signature or "n" deciphering, no JavaScript), a ranked ladder, and a
// probe of the top address with the request playback will make. Qt Core + Network only.
//
// What it keeps warm: a visitor id (minted once, valid for six hours, discarded after a bot
// answer) and a keep-alive TLS connection to the player host. It deduplicates concurrent
// requests for the same key, caches answers until their expiry minus two minutes (at most twenty
// minutes), and stops using itself for thirty minutes when the provider keeps refusing it
// (CircuitBreaker). When it cannot answer, the outcome says so (`fallback`) and the host uses its
// slower extractor. Anonymous throughout: no key, no cookies, no account.
#include "../Domain/CircuitBreaker.h"
#include "../Domain/InnerTubeClient.h"
#include "../Domain/ResolveCache.h"
#include "../Domain/StreamTypes.h"
#include "HttpsConnection.h"
#include <QElapsedTimer>
#include <QHash>
#include <QObject>
#include <QString>
#include <functional>
#include <memory>
#include <vector>

namespace nordtile { namespace music { namespace sharedplayback { namespace infrastructure {

class FastStreamResolver final : public QObject {
    Q_OBJECT
public:
    typedef unsigned long long RequestId;
    typedef std::function<void(const domain::ResolveOutcome &)> Callback;

    struct Options {
        QString playerHost = QStringLiteral("www.youtube.com");
        quint16 playerPort = 443;
        bool ipv4Only = true;
        int visitorTimeoutMs = 8000;
        int playerTimeoutMs = 10000;
        int probeTimeoutMs = 3000;
        int visitorLifetimeMs = 6 * 60 * 60 * 1000;
        bool probe = true;
        int reuseFirstByteTimeoutMs = 3000;   // see HttpsConnection::setReuseFirstByteTimeoutMs
        domain::ClientDefinition client = domain::visionosClient();
        // Unix milliseconds; replaceable for tests.
        std::function<std::int64_t()> clock;
        // One line of fixed numbers and class names per phase; empty: no logging.
        std::function<void(const std::string &)> log;
        // Tests only: where the media probe connects (the allow-listed media host is kept in the
        // address) and whether certificate errors are ignored.
        std::function<bool(const QString &host, QString *connectHost, quint16 *connectPort)> mediaEndpointOverride;
        bool ignoreSslErrorsForTesting = false;
    };

    struct Counters {
        int fast = 0, cache = 0, fallbacks = 0, botWalls = 0, probeRejected = 0, network = 0;
        std::vector<int> recentTotalsMs;   // last 32 fast resolves, for medians
        int medianTotalMs() const;
    };

    explicit FastStreamResolver(const Options &options, QObject *parent = nullptr);
    ~FastStreamResolver() override;

    // Mints the visitor id and opens the connection. Cheap; call once the first screen is up.
    void warmUp();
    // The callback runs asynchronously, exactly once (also for refusals and cancellation).
    RequestId resolve(const std::string &videoId, const domain::StreamRequest &request, const Callback &callback);
    void cancel(RequestId id);

    // A source from this component was refused downstream (probe, byte 0 of the playback fetch,
    // or the player). `msLeft` is how long the address still had to live: a refusal of an address
    // with a minute or more left is a strike.
    void reportRefused(const std::string &videoId, std::int64_t msLeft);
    // The device changed networks (Wi-Fi <-> cellular, a new address): addresses and the visitor id are bound to the
    // old one, pooled connections are dead. Forget them all; the next request starts clean. No strike is involved.
    void networkChanged();
    // The cached answer for a request, if there is a valid one: no network, no breaker, no counters, nothing else touched.
    bool peekCached(const std::string &videoId, const domain::StreamRequest &request, domain::StreamSet *out) const;
    // The first playback request for an address the host is about to deliver (a rung other than the one the
    // resolver probed). Refused (an HTTP refusal) counts a strike like the resolver's own probe; Unusable
    // (a redirect, an answer that ignored Range or has another total) and NoVerdict (network, server error) do not.
    enum class ProbeVerdict { Accepted, NoVerdict, Refused, Unusable };
    void probeAddress(const std::string &url, const std::string &userAgent, std::int64_t contentLength,
                      const std::function<void(ProbeVerdict verdict, int status, int ms)> &done);
    // The same, also telling the address that finally answered: the media host may redirect to another node of its CDN
    // (followed up to three hops, only to the CDN's own hosts over HTTPS); that address is the one to play.
    typedef std::function<void(ProbeVerdict verdict, int status, int ms, const std::string &finalUrl)> ProbeDone;
    void probeAddress(const std::string &url, const std::string &userAgent, std::int64_t contentLength, const ProbeDone &done);
    void invalidate(const std::string &videoId);

    bool allowed() const;
    const Counters &counters() const { return counters_; }
    bool hasVisitor() const { return !visitor_.empty(); }
    domain::CircuitBreaker &breaker() { return breaker_; }

private:
    struct Waiter { RequestId id; Callback callback; };
    struct Job;
    typedef std::function<void(int visitorMs)> StreamTimingsHook;
    std::int64_t now() const;
    std::int64_t mono() const;
    void logLine(const std::string &line) const;
    HttpsConnection *idleConnection();
    void begin(const std::shared_ptr<Job> &job);
    void ensureVisitor(const std::function<void()> &then, StreamTimingsHook hook);
    void requestPlayer(const std::shared_ptr<Job> &job);
    void probeTop(const std::shared_ptr<Job> &job, domain::StreamSet set);
    void probeUrl(const std::string &url, const std::string &userAgent, std::int64_t contentLength, int timeoutMs,
                  const ProbeDone &done, int hopsLeft, int elapsedSoFar);
    void complete(const std::shared_ptr<Job> &job, domain::ResolveOutcome outcome);
    void deliver(const std::vector<Waiter> &waiters, const domain::ResolveOutcome &outcome);

    Options options_;
    std::function<std::int64_t()> monotonic_;   // empty: options_.clock
    domain::CircuitBreaker breaker_;
    domain::ResolveCache cache_;
    Counters counters_;
    std::string visitor_;
    std::int64_t visitorMintedAt_ = 0;
    std::vector<HttpsConnection *> connections_;
    QHash<QString, std::shared_ptr<Job> > inFlight_;
    RequestId nextId_ = 1;
    bool minting_ = false;
    int visitorGeneration_ = 0;          // changes whenever the visitor id is minted or discarded: one verdict counts once
    int visitorFailures_ = 0;            // failed mints in a row
    std::int64_t visitorRetryAt_ = 0;    // no new mint before this (back-off); requests go to the extractor meanwhile
    int noFormatStreak_ = 0;             // answers in a row with no direct audio at all (the client may have stopped working)
    std::vector<std::function<void()> > afterVisitor_;
};

}}}}
