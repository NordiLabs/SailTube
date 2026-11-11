// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once
// A loopback HTTP source for the player: it serves a resolved media address on 127.0.0.1 and
// fetches the real thing itself. Qt Core + Network only.
//
// Why: the platform's audio backend can be refused by the media host where a plain client with
// the same headers is not (observed by other players on this platform), the address is bound to the
// address family and the user agent that obtained it, and a refused or expired address should cost
// one re-resolve and a resume at the same byte, not a failed track. So the backend is handed
// http://127.0.0.1:<port>/s/<token> and this class:
//
//  * answers GET / HEAD with 200, 206 (+Content-Range), 416 and 404/405/503 as appropriate; always
//    Connection: close, type application/octet-stream so that the backend typefinds the stream;
//  * fetches the media in bounded Range chunks (10 MiB), each from the position the client has
//    reached, over IPv4 on request, with the headers the address was issued for (no cookies, no
//    redirects), and forwards bytes as they arrive; it stops taking bytes from the network while the
//    client is slow (flow control), so memory stays bounded by one chunk;
//  * on a refusal (403), an expiry or a connection that died, asks the host for a fresh address of
//    the SAME format (same length and type, else the stream ends: IdentityChanged) and resumes at
//    the same byte; a refusal of a freshly issued address is retried in place every 500 ms for up to
//    6 s first (such an address can be refused for a few seconds). Limits: 3 deaths in a row without
//    progress end the stream; at most 8 re-resolves per 60 s.
//
// Only addresses registered through open() are served, under an unguessable token, and only
// https addresses on the host suffix allow-list are ever fetched.
#include "HttpsConnection.h"
#include <QByteArray>
#include <QHash>
#include <QObject>
#include <QString>
#include <QTcpServer>
#include <QUrl>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace nordtile { namespace music { namespace sharedplayback { namespace infrastructure {

class MediaProxy final : public QObject {
public:
    struct Source {
        std::string url;                 // the media address (https, allow-listed host)
        std::string userAgent;           // the client's own: the address is bound to it
        std::int64_t contentLength = 0;  // total bytes of the format; required (> 0)
        std::string mimeType;            // identity check on refresh ("audio/webm")
        std::int64_t expiresAtUnixMs = 0;
        std::int64_t issuedAtUnixMs = 0; // when the address was obtained (0: now)
    };
    struct FreshSource { bool ok = false; Source source; };
    // Asks the host for a new address of the same format; calls back exactly once.
    typedef std::function<void(const std::function<void(const FreshSource &)> &deliver)> Refresher;

    struct Options {
        bool ipv4Only = true;
        std::int64_t chunkBytes = 10 * 1024 * 1024;   // the largest upstream request
        // The first upstream request of a request chain is small and grows (x chunkGrowth per chunk, up to chunkBytes): the
        // bytes that matter first (container header, first seconds) come in one short exchange, and a seek starts small too.
        std::int64_t firstChunkBytes = 512 * 1024;     // 0: always chunkBytes
        int chunkGrowth = 4;
        // At open() the proxy already asks for the start of the stream over a connection of its own and keeps these bytes, so
        // that the first request of the audio backend (which takes a moment to appear) is answered from memory and its
        // connection is warm. 0: off. Bounded per stream; dropped with the stream.
        std::int64_t prebufferBytes = 256 * 1024;
        // The last bytes of the stream are also fetched at open(), over a connection of their own, and kept: the demuxers that open a
        // stream look at its tail first (tag footers, an index, the end of a fragmented file), each time over a new request, and every
        // such request that has to reach the provider costs a round trip and, on a new connection, a TLS handshake before the first
        // audio. A request that lies wholly in the tail is answered from memory. 0: off. Streams shorter than a few times it have none.
        std::int64_t tailBytes = 16 * 1024;
        int maxJobs = 8;
        int earlyRetryIntervalMs = 500;
        int earlyRetryWindowMs = 6000;
        int maxDeathsWithoutProgress = 3;
        int maxRefreshesPerMinute = 8;
        int upstreamTimeoutMs = 30000;
        int refreshTimeoutMs = 20000;      // a refresher that has not answered by then ends the request
        std::int64_t clientBufferHighBytes = 2 * 1024 * 1024;   // pause the network above this
        std::int64_t clientBufferLowBytes = 512 * 1024;
        std::function<std::int64_t()> clock;                      // unix ms; default: the system clock
        std::function<void(const std::string &)> log;             // fixed numbers and class names only
        // Tests only.
        std::function<bool(const QString &host, QString *connectHost, quint16 *connectPort)> endpointOverride;
        bool ignoreSslErrorsForTesting = false;
        bool allowAnyHostForTesting = false;
    };

    struct Counters { int served = 0, chunks = 0, resumed = 0, refreshes = 0, earlyRetries = 0, identityChanged = 0, refused = 0, pauses = 0, redirects = 0, prebufferHits = 0, prebufferFailures = 0, reused = 0, tailHits = 0, tailFailures = 0, upstreamReused = 0; };

    explicit MediaProxy(const Options &options, QObject *parent = nullptr);
    ~MediaProxy() override;

    // Starts listening on first use. Empty when the address is not allowed or listening failed.
    QUrl open(const std::string &videoId, const Source &source, const Refresher &refresher);
    // The address of a stream of this video that is still open and carries the same bytes (length and type), for a second
    // playback of what was already resolved - the next track that was prepared ahead, a replay. Its pre-buffer and its warm
    // connection come with it, so nothing is fetched again. Not a stream that is older than maxAgeMs, whose address has
    // less than minRemainingMs left (when it has an expiry), or whose pre-buffer failed. Empty when there is none.
    QUrl reuse(const std::string &videoId, std::int64_t contentLength, const std::string &mimeType, std::int64_t maxAgeMs,
               std::int64_t minRemainingMs);
    void closeVideo(const std::string &videoId);   // ends every stream and job of a video
    void closeAll();
    quint16 port() const { return server_.isListening() ? server_.serverPort() : 0; }
    int activeJobs() const { return int(jobs_.size()); }
    // True once a client has been served from any stream of the video (the backend did reach the proxy).
    bool wasServed(const std::string &videoId) const;
    const Counters &counters() const { return counters_; }

    // True for an address the proxy is willing to fetch.
    static bool allowedAddress(const std::string &url);

private:
    struct Stream;
    struct Job;
    std::int64_t now() const;
    void logLine(const std::string &line) const;
    void onNewConnection();
    void onClientData(const std::shared_ptr<Job> &job);
    void startServing(const std::shared_ptr<Job> &job);
    void respondError(const std::shared_ptr<Job> &job, int status, const char *reason);
    void fetchNext(const std::shared_ptr<Job> &job);
    void fetchChunk(const std::shared_ptr<Job> &job);
    void chunkEnded(const std::shared_ptr<Job> &job, const HttpsResult &result);
    void refresh(const std::shared_ptr<Job> &job, const char *why);
    // complete: the whole answer was handed over; linger: an answer that ends short is still closed gracefully
    // (what was sent is delivered) instead of reset. Otherwise the client is dropped at once.
    void finishJob(const std::shared_ptr<Job> &job, bool complete, bool linger = false);
    void flowControl(const std::shared_ptr<Job> &job);
    struct Tail;
    void startTail(const std::shared_ptr<Stream> &stream);
    void wakeTailWaiters(const std::shared_ptr<Stream> &stream);
    bool serveTail(const std::shared_ptr<Job> &job);       // true: the job is answered from the tail, waits for it, or is finished
    // Upstream connections of finished jobs that are idle and warm are kept for a short while and handed to the next job that needs
    // the same host (a seek, a probe): no new TLS handshake. A connection in the middle of an answer is never kept.
    struct Pooled { QPointer<HttpsConnection> connection; QString host; std::int64_t at = 0; };
    void poolUpstream(HttpsConnection *connection, const QString &host);
    HttpsConnection *takePooled(const QString &host);
    std::vector<Pooled> pool_;
    struct Prebuffer;
    void startPrebuffer(const std::shared_ptr<Stream> &stream);
    void requestPrebuffer(const std::shared_ptr<Stream> &stream);
    void wakePrebufferWaiters(const std::shared_ptr<Stream> &stream);
    bool servePrebuffer(const std::shared_ptr<Job> &job);   // true: the job waits for the pre-buffer or is finished; false: go on over the network
    std::int64_t chunkSizeFor(const std::shared_ptr<Job> &job) const;

    Options options_;
    QTcpServer server_;
    QHash<QString, std::shared_ptr<Stream> > streams_;
    std::vector<std::shared_ptr<Job> > jobs_;
    Counters counters_;
};

}}}}
