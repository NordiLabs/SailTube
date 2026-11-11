// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "MediaProxy.h"
#include "../Domain/InnerTubeClient.h"
#include "MediaAddress.h"
#include <QDateTime>
#include <QFile>
#include <QPointer>
#include <QTcpSocket>
#include <QTimer>
#include <algorithm>

namespace nordtile { namespace music { namespace sharedplayback { namespace infrastructure {
using namespace domain;

struct MediaProxy::Prebuffer {
    QByteArray data;                       // the first bytes of the stream, as they arrive
    std::int64_t wanted = 0;
    bool done = false, failed = false;
    int redirects = 0;
    QPointer<HttpsConnection> connection;  // idle and warm once done: the first job that needs the network takes it over
    std::vector<std::weak_ptr<Job> > waiters;
};

struct MediaProxy::Tail {
    QByteArray data;                       // the last bytes of the stream
    std::int64_t start = 0, wanted = 0;
    bool done = false, failed = false;
    QPointer<HttpsConnection> connection;
    std::vector<std::weak_ptr<Job> > waiters;
};

struct MediaProxy::Stream {
    ~Stream() { if (pre && pre->connection) pre->connection->deleteLater(); if (tail && tail->connection) tail->connection->deleteLater(); }
    std::string videoId;
    QString token;
    Source source;
    Refresher refresher;
    std::vector<std::int64_t> refreshTimes;
    std::int64_t issuedAt = 0;
    std::int64_t openedAt = 0;
    std::int64_t lastUsedAt = 0;
    int served = 0;
    int generation = 0;          // changes whenever the address is replaced
    std::shared_ptr<Prebuffer> pre;
    std::shared_ptr<Tail> tail;
};

struct MediaProxy::Job {
    QTcpSocket *client = nullptr;
    QByteArray request;
    std::shared_ptr<Stream> stream;
    bool head = false;
    std::int64_t start = 0, end = -1;      // inclusive byte range being served
    std::int64_t position = 0;             // next byte to hand to the client
    std::int64_t chunkFrom = 0, chunkTo = -1, chunkGot = 0;
    int deaths = 0;                        // consecutive early ends without progress
    int earlyElapsedMs = 0;
    bool finished = false;
    bool refreshing = false;
    int redirects = 0;                     // redirects followed for the chunk being fetched
    int chunkIndex = 0;                    // chunks completed over the network: the size ladder
    bool usedPrebuffer = false;
    int expiryHandledGeneration = -1;   // the address generation an expiry refresh was already started for
    QTimer *refreshWatchdog = nullptr;
    bool headersSent = false;
    bool firstByteLogged = false;
    HttpsConnection *upstream = nullptr;
    QString upstreamHost;
    QTimer *requestTimer = nullptr;
    std::int64_t startedAt = 0;
};

namespace {
const std::string tokenPrefix = "/s/";

// "bytes=a-b" | "bytes=a-" | "bytes=-n" against a known total. Returns false for a malformed or
// multi-range header (answered 416). `satisfiable` is false when the start lies past the end.
bool parseRange(const QByteArray &header, std::int64_t total, std::int64_t *start, std::int64_t *end, bool *satisfiable) {
    *satisfiable = true;
    if (!header.startsWith("bytes=")) return false;
    const QByteArray spec = header.mid(6).trimmed();
    if (spec.isEmpty() || spec.contains(',')) return false;
    const int dash = spec.indexOf('-');
    if (dash < 0) return false;
    const QByteArray first = spec.left(dash).trimmed(), last = spec.mid(dash + 1).trimmed();
    bool ok1 = true, ok2 = true;
    if (first.isEmpty()) {            // suffix
        const qlonglong n = last.toLongLong(&ok2);
        if (!ok2 || n <= 0) return false;
        *start = total - std::min<qlonglong>(n, total);
        *end = total - 1;
        return true;
    }
    const qlonglong a = first.toLongLong(&ok1);
    if (!ok1 || a < 0) return false;
    qlonglong b = total - 1;
    if (!last.isEmpty()) { b = last.toLongLong(&ok2); if (!ok2 || b < a) return false; }
    if (a >= total) { *satisfiable = false; return true; }
    *start = a;
    *end = std::min<qlonglong>(b, total - 1);
    return true;
}

bool readNumber(const std::string &value, std::size_t *at, std::int64_t *out) {
    std::int64_t v = 0; std::size_t i = *at; int digits = 0;
    for (; i < value.size() && value[i] >= '0' && value[i] <= '9'; ++i) {
        if (++digits > 15) return false;                 // far beyond any media length; no overflow
        v = v * 10 + (value[i] - '0');
    }
    if (digits == 0) return false;
    *at = i; *out = v;
    return true;
}

// "bytes 100-199/1000" -> 100, 199, 1000 (total -1 when "*")
bool parseContentRange(const std::string &value, std::int64_t *start, std::int64_t *end, std::int64_t *total) {
    if (value.compare(0, 6, "bytes ") != 0) return false;
    std::size_t i = 6;
    if (!readNumber(value, &i, start) || i >= value.size() || value[i] != '-') return false;
    ++i;
    if (!readNumber(value, &i, end) || i >= value.size() || value[i] != '/') return false;
    ++i;
    if (i < value.size() && value[i] == '*') { *total = -1; return i + 1 == value.size(); }
    return readNumber(value, &i, total) && i == value.size() && *end >= *start;
}

// 256 bits from the system's random source; empty when it cannot be read (the proxy then serves nothing).
QString randomToken() {
    QFile urandom(QStringLiteral("/dev/urandom"));
    if (!urandom.open(QIODevice::ReadOnly)) return QString();
    const QByteArray bytes = urandom.read(32);
    return bytes.size() == 32 ? QString::fromLatin1(bytes.toHex()) : QString();
}
}  // namespace

MediaProxy::MediaProxy(const Options &options, QObject *parent) : QObject(parent), options_(options), server_(this) {
    if (!options_.clock) options_.clock = []() { return std::int64_t(QDateTime::currentMSecsSinceEpoch()); };
    connect(&server_, &QTcpServer::newConnection, this, &MediaProxy::onNewConnection);
}

MediaProxy::~MediaProxy() { closeAll(); }

std::int64_t MediaProxy::now() const { return options_.clock(); }
void MediaProxy::logLine(const std::string &line) const { if (options_.log) options_.log(line); }

bool MediaProxy::allowedAddress(const std::string &url) { return isMediaCdnAddress(url); }

QUrl MediaProxy::open(const std::string &videoId, const Source &source, const Refresher &refresher) {
    if (!(options_.allowAnyHostForTesting || allowedAddress(source.url)) || source.contentLength <= 0 || !validVideoId(videoId)) return QUrl();
    if (!server_.isListening() && !server_.listen(QHostAddress::LocalHost, 0)) return QUrl();
    std::shared_ptr<Stream> stream(new Stream);
    stream->videoId = videoId;
    stream->token = randomToken();
    if (stream->token.isEmpty()) return QUrl();
    stream->source = source;
    stream->issuedAt = source.issuedAtUnixMs > 0 ? source.issuedAtUnixMs : now();
    stream->openedAt = now();
    stream->lastUsedAt = now();
    stream->refresher = refresher;
    streams_.insert(stream->token, stream);
    startPrebuffer(stream);
    startTail(stream);
    // Few streams are kept: beyond sixteen the one that was used longest ago and is not being read goes.
    if (streams_.size() > 16) {
        QString idle; std::int64_t at = 0;
        for (QHash<QString, std::shared_ptr<Stream> >::const_iterator it = streams_.constBegin(); it != streams_.constEnd(); ++it) {
            if (it.key() == stream->token) continue;
            bool busy = false;
            for (std::size_t i = 0; i < jobs_.size(); ++i) if (jobs_[i]->stream == it.value()) busy = true;
            if (busy) continue;
            if (idle.isEmpty() || it.value()->lastUsedAt < at) { idle = it.key(); at = it.value()->lastUsedAt; }
        }
        if (!idle.isEmpty()) streams_.remove(idle);
    }
    return QUrl(QStringLiteral("http://127.0.0.1:%1/s/%2").arg(server_.serverPort()).arg(stream->token));
}

QUrl MediaProxy::reuse(const std::string &videoId, std::int64_t contentLength, const std::string &mimeType, std::int64_t maxAgeMs,
                       std::int64_t minRemainingMs) {
    if (!server_.isListening() || contentLength <= 0) return QUrl();
    const std::int64_t at = now();
    std::shared_ptr<Stream> best;
    for (QHash<QString, std::shared_ptr<Stream> >::const_iterator it = streams_.constBegin(); it != streams_.constEnd(); ++it) {
        const std::shared_ptr<Stream> &s = it.value();
        if (s->videoId != videoId || s->source.contentLength != contentLength || s->source.mimeType != mimeType) continue;
        if (at - s->openedAt > maxAgeMs || at < s->openedAt) continue;
        if (s->source.expiresAtUnixMs > 0 && s->source.expiresAtUnixMs - at < minRemainingMs) continue;
        if (s->pre && s->pre->failed) continue;
        if (!best || s->lastUsedAt > best->lastUsedAt) best = s;
    }
    if (!best) return QUrl();
    best->lastUsedAt = at;
    ++counters_.reused;
    logLine(std::string("sailtune.media phase reuse prebuffer ") + (best->pre && best->pre->done ? "ready" : "loading") +
            " served " + std::to_string(best->served));
    return QUrl(QStringLiteral("http://127.0.0.1:%1/s/%2").arg(server_.serverPort()).arg(best->token));
}

bool MediaProxy::wasServed(const std::string &videoId) const {
    for (QHash<QString, std::shared_ptr<Stream> >::const_iterator it = streams_.constBegin(); it != streams_.constEnd(); ++it)
        if (it.value()->videoId == videoId && it.value()->served > 0) return true;
    return false;
}

void MediaProxy::closeVideo(const std::string &videoId) {
    std::vector<std::shared_ptr<Job> > doomed;
    for (std::size_t i = 0; i < jobs_.size(); ++i) if (jobs_[i]->stream && jobs_[i]->stream->videoId == videoId) doomed.push_back(jobs_[i]);
    for (std::size_t i = 0; i < doomed.size(); ++i) finishJob(doomed[i], false);
    for (QHash<QString, std::shared_ptr<Stream> >::iterator it = streams_.begin(); it != streams_.end();)
        if (it.value()->videoId == videoId) it = streams_.erase(it); else ++it;
}

void MediaProxy::closeAll() {
    const std::vector<std::shared_ptr<Job> > all(jobs_);
    for (std::size_t i = 0; i < all.size(); ++i) finishJob(all[i], false);
    streams_.clear();
    for (std::size_t i = 0; i < pool_.size(); ++i) if (pool_[i].connection) pool_[i].connection->deleteLater();
    pool_.clear();
    if (server_.isListening()) server_.close();
}

void MediaProxy::onNewConnection() {
    while (server_.hasPendingConnections()) {
        QTcpSocket *socket = server_.nextPendingConnection();
        if (!socket) return;
        std::shared_ptr<Job> job(new Job);
        job->client = socket;
        job->startedAt = now();
        if (int(jobs_.size()) >= 4 * options_.maxJobs) { socket->abort(); socket->deleteLater(); continue; }   // idle sockets never crowd out the backend
        socket->setParent(this);
        socket->setSocketOption(QAbstractSocket::LowDelayOption, 1);     // headers and the first bytes must not wait for each other
        jobs_.push_back(job);
        std::weak_ptr<Job> weak(job);
        connect(socket, &QIODevice::readyRead, this, [this, weak]() { if (std::shared_ptr<Job> j = weak.lock()) onClientData(j); });
        connect(socket, &QAbstractSocket::disconnected, this, [this, weak]() { if (std::shared_ptr<Job> j = weak.lock()) finishJob(j, false); });
        // Flow control: the network is read again once the client has taken most of what was queued for it.
        connect(socket, &QIODevice::bytesWritten, this, [this, weak](qint64) {
            std::shared_ptr<Job> j = weak.lock();
            if (!j || j->finished || !j->client || !j->upstream) return;
            if (j->upstream->paused() && j->client->bytesToWrite() < options_.clientBufferLowBytes) j->upstream->setPaused(false);
        });
        // A client that never asks is not kept.
        QTimer::singleShot(5000, this, [this, weak]() { if (std::shared_ptr<Job> j = weak.lock()) if (!j->stream && !j->finished) finishJob(j, false); });
    }
}

void MediaProxy::respondError(const std::shared_ptr<Job> &job, int status, const char *reason) {
    if (job->finished || !job->client) return;
    QByteArray out = "HTTP/1.1 " + QByteArray::number(status) + " " + reason + "\r\nContent-Length: 0\r\nConnection: close\r\n";
    if (status == 416 && job->stream) out += "Content-Range: bytes */" + QByteArray::number(qlonglong(job->stream->source.contentLength)) + "\r\n";
    out += "\r\n";
    job->client->write(out);
    job->headersSent = true;
    logLine("sailtune.media phase serve status " + std::to_string(status));
    finishJob(job, true);
}

void MediaProxy::onClientData(const std::shared_ptr<Job> &job) {
    if (job->finished || job->stream) { if (job->client) job->client->readAll(); return; }
    job->request.append(job->client->readAll());
    if (job->request.size() > 16 * 1024) { respondError(job, 400, "Bad Request"); return; }
    const int end = job->request.indexOf("\r\n\r\n");
    if (end < 0) return;
    const QList<QByteArray> lines = job->request.left(end).split('\n');
    const QList<QByteArray> first = lines.value(0).trimmed().split(' ');
    if (first.size() < 2) { respondError(job, 400, "Bad Request"); return; }
    const QByteArray method = first.at(0), path = first.at(1);
    QByteArray range;
    for (int i = 1; i < lines.size(); ++i) {
        const int colon = lines.at(i).indexOf(':');
        if (colon > 0 && lines.at(i).left(colon).trimmed().toLower() == "range") range = lines.at(i).mid(colon + 1).trimmed();
    }
    {   // Only streams being served count against the limit; connections that have not asked for anything do not.
        int serving = 0;
        for (std::size_t i = 0; i < jobs_.size(); ++i) if (jobs_[i]->stream && !jobs_[i]->finished) ++serving;
        if (serving >= options_.maxJobs) { respondError(job, 503, "Service Unavailable"); return; }
    }
    if (method != "GET" && method != "HEAD") { respondError(job, 405, "Method Not Allowed"); return; }
    if (!path.startsWith("/s/")) { respondError(job, 404, "Not Found"); return; }
    const QString token = QString::fromLatin1(path.mid(3).split('?').value(0));
    const std::shared_ptr<Stream> stream = streams_.value(token);
    if (!stream) { respondError(job, 404, "Not Found"); return; }
    job->stream = stream;
    stream->lastUsedAt = now();
    job->head = method == "HEAD";
    const std::int64_t total = stream->source.contentLength;
    bool satisfiable = true;
    job->start = 0; job->end = total - 1;
    // A Range header that is malformed, uses a unit other than bytes or asks for several ranges is ignored (RFC 7233): the
    // whole body is served. Only a first byte past the end is unsatisfiable.
    bool ranged = !range.isEmpty();
    if (ranged && !parseRange(range, total, &job->start, &job->end, &satisfiable)) { ranged = false; job->start = 0; job->end = total - 1; satisfiable = true; }
    if (!satisfiable) { respondError(job, 416, "Range Not Satisfiable"); return; }
    job->position = job->start;
    QByteArray head = ranged ? "HTTP/1.1 206 Partial Content\r\n" : "HTTP/1.1 200 OK\r\n";
    head += "Accept-Ranges: bytes\r\nContent-Type: application/octet-stream\r\nContent-Length: " +
            QByteArray::number(qlonglong(job->end - job->start + 1)) + "\r\n";
    if (ranged) head += "Content-Range: bytes " + QByteArray::number(qlonglong(job->start)) + "-" + QByteArray::number(qlonglong(job->end)) +
                        "/" + QByteArray::number(qlonglong(total)) + "\r\n";
    head += "Connection: close\r\n\r\n";
    job->client->write(head);
    job->headersSent = true;
    ++counters_.served;
    ++stream->served;
    logLine("sailtune.media phase serve status " + std::string(ranged ? "206" : "200") + " start " + std::to_string(job->start) +
            " bytes " + std::to_string(job->end - job->start + 1));
    if (job->head) { finishJob(job, true); return; }
    fetchNext(job);
}

void MediaProxy::fetchNext(const std::shared_ptr<Job> &job) {
    if (job->finished) return;
    if (job->position > job->end) { finishJob(job, true); return; }
    fetchChunk(job);
}

void MediaProxy::fetchChunk(const std::shared_ptr<Job> &job) {
    if (job->finished || job->refreshing) return;
    const std::shared_ptr<Stream> stream = job->stream;
    if (job->position > job->end) { finishJob(job, true); return; }     // nothing left (a last chunk that ended untidily)
    // An address about to expire is replaced before it is used - once per address: the expiry is the provider's clock
    // read against this device's, and a device whose clock is ahead must not be sent round the same loop for ever.
    if (stream->source.expiresAtUnixMs > 0 && now() >= stream->source.expiresAtUnixMs - 30000 &&
        job->expiryHandledGeneration != stream->generation) {
        job->expiryHandledGeneration = stream->generation + 1;
        refresh(job, "expiry");
        return;
    }
    if (serveTail(job)) return;
    if (job->finished) return;
    if (servePrebuffer(job)) return;
    if (job->finished) return;
    job->chunkFrom = job->position;
    job->chunkTo = std::min<std::int64_t>(job->position + chunkSizeFor(job) - 1, job->end);
    job->chunkGot = 0;
    const QUrl url(QString::fromStdString(stream->source.url), QUrl::StrictMode);
    QString connectHost = url.host(); quint16 connectPort = 443;
    if (options_.endpointOverride) options_.endpointOverride(url.host(), &connectHost, &connectPort);
    if (job->upstream && job->upstreamHost != url.host()) { job->upstream->deleteLater(); job->upstream = nullptr; }
    // The connection the pre-buffer warmed (TLS done, idle) goes to the first job that needs the network after it.
    if (!job->upstream && stream->pre && stream->pre->connection && stream->pre->done && !stream->pre->connection->busy() &&
        stream->pre->connection->connected()) {
        job->upstream = stream->pre->connection.data();
        stream->pre->connection = nullptr;
        job->upstreamHost = url.host();
    }
    if (!job->upstream) {
        job->upstream = takePooled(url.host());        // an idle warm connection of an earlier job: no new handshake
        if (job->upstream) { job->upstreamHost = url.host(); ++counters_.upstreamReused; }
    }
    if (!job->upstream) {
        job->upstream = new HttpsConnection(connectHost, connectPort, options_.ipv4Only, this);
        job->upstream->setIgnoreSslErrorsForTesting(options_.ignoreSslErrorsForTesting);
        job->upstreamHost = url.host();
    }
    HttpsRequest request;
    request.method = "GET";
    request.path = url.path(QUrl::FullyEncoded).toStdString() + (url.hasQuery() ? "?" + url.query(QUrl::FullyEncoded).toStdString() : std::string());
    const std::vector<Header> headers = mediaRequestHeaders(stream->source.userAgent);
    for (std::size_t i = 0; i < headers.size(); ++i) request.headers.push_back(headers[i]);
    request.headers.push_back(Header("Range", "bytes=" + std::to_string(job->chunkFrom) + "-" + std::to_string(job->chunkTo)));
    request.timeoutMs = options_.upstreamTimeoutMs;
    request.maxBodyBytes = 4 * 1024 * 1024;       // what may wait for the client; the chunk itself is streamed
    std::weak_ptr<Job> weak(job);
    request.onHeaders = [this, weak](int status, const std::map<std::string, std::string> &headers) {
        std::shared_ptr<Job> j = weak.lock();
        if (!j || j->finished) return false;
        if (status != 206) return false;
        std::map<std::string, std::string>::const_iterator range = headers.find("content-range");
        std::int64_t from = -1, to = -1, total = -1;
        // The bytes asked for, of the file expected: not another start, not more than was asked for, not another length.
        return range != headers.end() && parseContentRange(range->second, &from, &to, &total) && from == j->chunkFrom &&
               to <= j->chunkTo && (total < 0 || total == j->stream->source.contentLength);
    };
    request.onBody = [this, weak](const QByteArray &bytes) {
        std::shared_ptr<Job> j = weak.lock();
        if (!j || j->finished || !j->client) return false;
        if (!j->firstByteLogged) {
            j->firstByteLogged = true;
            // Milliseconds from the address being handed over to the first media byte reaching the player's connection.
            logLine("sailtune.media phase first_byte since_open_ms " + std::to_string(now() - j->stream->openedAt) +
                    " since_request_ms " + std::to_string(now() - j->startedAt));
        }
        const std::int64_t room = j->chunkTo - (j->chunkFrom + j->chunkGot) + 1;
        const QByteArray part = bytes.size() > room ? bytes.left(int(room)) : bytes;
        j->client->write(part);
        j->chunkGot += part.size();
        j->position += part.size();
        flowControl(j);
        return true;
    };
    job->upstream->send(request, [this, weak](const HttpsResult &result) {
        if (std::shared_ptr<Job> j = weak.lock()) chunkEnded(j, result);
    });
    // A new request starts unpaused: a client that is still behind keeps the network held back.
    if (job->client && job->client->bytesToWrite() > options_.clientBufferHighBytes) { job->upstream->setPaused(true); ++counters_.pauses; }
}

// The size of the next upstream request: small first, growing to the largest.
std::int64_t MediaProxy::chunkSizeFor(const std::shared_ptr<Job> &job) const {
    std::int64_t size = options_.chunkBytes;
    if (options_.firstChunkBytes > 0 && options_.firstChunkBytes < options_.chunkBytes) {
        size = options_.firstChunkBytes;
        const std::int64_t growth = std::max(2, options_.chunkGrowth);
        for (int i = 0; i < job->chunkIndex && size < options_.chunkBytes; ++i) size *= growth;
        size = std::min(size, options_.chunkBytes);
    }
    return size;
}

void MediaProxy::startPrebuffer(const std::shared_ptr<Stream> &stream) {
    if (options_.prebufferBytes <= 0 || stream->source.contentLength <= 0) return;
    stream->pre.reset(new Prebuffer);
    stream->pre->wanted = std::min<std::int64_t>(options_.prebufferBytes, stream->source.contentLength);
    requestPrebuffer(stream);
}

void MediaProxy::requestPrebuffer(const std::shared_ptr<Stream> &stream) {
    const std::shared_ptr<Prebuffer> pb = stream->pre;
    if (!pb) return;
    const QUrl url(QString::fromStdString(stream->source.url), QUrl::StrictMode);
    QString connectHost = url.host(); quint16 connectPort = 443;
    if (options_.endpointOverride) options_.endpointOverride(url.host(), &connectHost, &connectPort);
    if (pb->connection) { pb->connection->deleteLater(); pb->connection = nullptr; }
    pb->connection = new HttpsConnection(connectHost, connectPort, options_.ipv4Only, this);
    pb->connection->setIgnoreSslErrorsForTesting(options_.ignoreSslErrorsForTesting);
    pb->data.clear();
    HttpsRequest request;
    request.method = "GET";
    request.path = url.path(QUrl::FullyEncoded).toStdString() + (url.hasQuery() ? "?" + url.query(QUrl::FullyEncoded).toStdString() : std::string());
    const std::vector<Header> headers = mediaRequestHeaders(stream->source.userAgent);
    for (std::size_t i = 0; i < headers.size(); ++i) request.headers.push_back(headers[i]);
    request.headers.push_back(Header("Range", "bytes=0-" + std::to_string(pb->wanted - 1)));
    request.timeoutMs = options_.upstreamTimeoutMs;
    request.maxBodyBytes = std::size_t(pb->wanted) + 64 * 1024;
    std::weak_ptr<Stream> weak(stream);
    request.onHeaders = [weak](int status, const std::map<std::string, std::string> &headers) {
        const std::shared_ptr<Stream> s = weak.lock();
        if (!s || !s->pre || status != 206) return false;
        std::map<std::string, std::string>::const_iterator range = headers.find("content-range");
        std::int64_t from = -1, to = -1, total = -1;
        return range != headers.end() && parseContentRange(range->second, &from, &to, &total) && from == 0 &&
               to < s->pre->wanted && (total < 0 || total == s->source.contentLength);
    };
    request.onBody = [this, weak](const QByteArray &bytes) {
        const std::shared_ptr<Stream> s = weak.lock();
        if (!s || !s->pre) return false;
        const std::int64_t room = s->pre->wanted - s->pre->data.size();
        s->pre->data.append(bytes.size() > room ? bytes.left(int(room)) : bytes);
        wakePrebufferWaiters(s);
        return true;
    };
    pb->connection->send(request, [this, weak](const HttpsResult &result) {
        const std::shared_ptr<Stream> s = weak.lock();
        if (!s || !s->pre) return;
        Prebuffer &pre = *s->pre;
        // A redirect to another node of the CDN: follow it (it stays the stream's address).
        if (result.status >= 300 && result.status < 400 && pre.redirects < 3 && isMediaCdnAddress(result.location) && result.location != s->source.url) {
            ++pre.redirects; ++counters_.redirects;
            s->source.url = result.location;
            requestPrebuffer(s);
            return;
        }
        if (result.transportOk && result.status == 206 && pre.data.size() == pre.wanted) pre.done = true;
        else { pre.failed = true; ++counters_.prebufferFailures; logLine("sailtune.media phase prebuffer failed status " + std::to_string(result.status)); }
        wakePrebufferWaiters(s);
    });
}

void MediaProxy::poolUpstream(HttpsConnection *connection, const QString &host)
{
    if (!connection) return;
    // Entries whose connection went away or was closed by the idle timer are dropped first.
    for (std::size_t i = pool_.size(); i > 0; --i) {
        Pooled &p = pool_[i - 1];
        if (!p.connection || !p.connection->connected() || now() - p.at > 30000) {
            if (p.connection) p.connection->deleteLater();
            pool_.erase(pool_.begin() + long(i - 1));
        }
    }
    if (host.isEmpty() || connection->busy() || !connection->connected()) { connection->deleteLater(); return; }
    if (pool_.size() >= 3) {                         // the newest idle connection is the one most likely to still be alive
        if (pool_.front().connection) pool_.front().connection->deleteLater();
        pool_.erase(pool_.begin());
    }
    Pooled p; p.connection = connection; p.host = host; p.at = now();
    pool_.push_back(p);
}

HttpsConnection *MediaProxy::takePooled(const QString &host)
{
    for (std::size_t i = pool_.size(); i > 0; --i) {
        Pooled &p = pool_[i - 1];
        if (!p.connection || !p.connection->connected() || p.connection->busy() || now() - p.at > 30000) {
            if (p.connection) p.connection->deleteLater();
            pool_.erase(pool_.begin() + long(i - 1));
            continue;
        }
        if (p.host != host) continue;
        HttpsConnection *connection = p.connection.data();
        pool_.erase(pool_.begin() + long(i - 1));
        return connection;
    }
    return nullptr;
}

void MediaProxy::startTail(const std::shared_ptr<Stream> &stream)
{
    const std::int64_t length = stream->source.contentLength;
    if (options_.tailBytes <= 0 || length < 4 * options_.tailBytes + std::max<std::int64_t>(0, options_.prebufferBytes)) return;
    stream->tail.reset(new Tail);
    stream->tail->wanted = options_.tailBytes;
    stream->tail->start = length - options_.tailBytes;
    const QUrl url(QString::fromStdString(stream->source.url), QUrl::StrictMode);
    QString connectHost = url.host(); quint16 connectPort = 443;
    if (options_.endpointOverride) options_.endpointOverride(url.host(), &connectHost, &connectPort);
    stream->tail->connection = new HttpsConnection(connectHost, connectPort, options_.ipv4Only, this);
    stream->tail->connection->setIgnoreSslErrorsForTesting(options_.ignoreSslErrorsForTesting);
    HttpsRequest request;
    request.method = "GET";
    request.path = url.path(QUrl::FullyEncoded).toStdString() + (url.hasQuery() ? "?" + url.query(QUrl::FullyEncoded).toStdString() : std::string());
    const std::vector<Header> headers = mediaRequestHeaders(stream->source.userAgent);
    for (std::size_t i = 0; i < headers.size(); ++i) request.headers.push_back(headers[i]);
    request.headers.push_back(Header("Range", "bytes=" + std::to_string(stream->tail->start) + "-" + std::to_string(length - 1)));
    request.timeoutMs = options_.upstreamTimeoutMs;
    request.maxBodyBytes = std::size_t(stream->tail->wanted) + 64 * 1024;
    std::weak_ptr<Stream> weak(stream);
    request.onHeaders = [weak](int status, const std::map<std::string, std::string> &headers) {
        const std::shared_ptr<Stream> s = weak.lock();
        if (!s || !s->tail || status != 206) return false;
        std::map<std::string, std::string>::const_iterator range = headers.find("content-range");
        std::int64_t from = -1, to = -1, total = -1;
        return range != headers.end() && parseContentRange(range->second, &from, &to, &total) && from == s->tail->start &&
               to == s->source.contentLength - 1 && (total < 0 || total == s->source.contentLength);
    };
    request.onBody = [weak](const QByteArray &bytes) {
        const std::shared_ptr<Stream> s = weak.lock();
        if (!s || !s->tail) return false;
        const std::int64_t room = s->tail->wanted - s->tail->data.size();
        s->tail->data.append(bytes.size() > room ? bytes.left(int(room)) : bytes);
        return true;
    };
    stream->tail->connection->send(request, [this, weak](const HttpsResult &result) {
        const std::shared_ptr<Stream> s = weak.lock();
        if (!s || !s->tail) return;
        Tail &tail = *s->tail;
        if (result.transportOk && result.status == 206 && tail.data.size() == tail.wanted) {
            tail.done = true;
            // Its connection is warm and idle: the next job that needs this host starts on it.
            if (tail.connection) { HttpsConnection *connection = tail.connection.data(); tail.connection = nullptr;
                                   poolUpstream(connection, QUrl(QString::fromStdString(s->source.url), QUrl::StrictMode).host()); }
        } else {
            tail.failed = true; ++counters_.tailFailures;
            logLine("sailtune.media phase tail failed status " + std::to_string(result.status));
        }
        wakeTailWaiters(s);
    });
}

void MediaProxy::wakeTailWaiters(const std::shared_ptr<Stream> &stream)
{
    if (!stream->tail || stream->tail->waiters.empty()) return;
    std::vector<std::weak_ptr<Job> > waiting;
    waiting.swap(stream->tail->waiters);
    for (std::size_t i = 0; i < waiting.size(); ++i)
        if (std::shared_ptr<Job> job = waiting[i].lock()) if (!job->finished && !job->refreshing) fetchChunk(job);
}

bool MediaProxy::serveTail(const std::shared_ptr<Job> &job)
{
    const std::shared_ptr<Tail> tail = job->stream->tail;
    if (!tail || !job->client || job->position < tail->start || tail->failed) return false;
    if (!tail->done) {
        bool queued = false;
        for (std::size_t i = 0; i < tail->waiters.size(); ++i) if (tail->waiters[i].lock() == job) queued = true;
        if (!queued) tail->waiters.push_back(job);
        return true;
    }
    const std::int64_t upTo = std::min<std::int64_t>(tail->start + tail->data.size(), job->end + 1);
    const std::int64_t count = upTo - job->position;
    if (count <= 0) return false;
    if (!job->firstByteLogged) {
        job->firstByteLogged = true;
        ++counters_.tailHits;
        logLine("sailtune.media phase first_byte since_open_ms " + std::to_string(now() - job->stream->openedAt) +
                " since_request_ms " + std::to_string(now() - job->startedAt) + " from tail");
    }
    job->client->write(tail->data.constData() + (job->position - tail->start), qint64(count));
    job->position += count;
    if (job->position > job->end) { finishJob(job, true); return true; }
    return false;
}

void MediaProxy::wakePrebufferWaiters(const std::shared_ptr<Stream> &stream) {
    if (!stream->pre || stream->pre->waiters.empty()) return;
    std::vector<std::weak_ptr<Job> > waiting;
    waiting.swap(stream->pre->waiters);
    for (std::size_t i = 0; i < waiting.size(); ++i)
        if (std::shared_ptr<Job> job = waiting[i].lock()) if (!job->finished && !job->refreshing) fetchChunk(job);
}

bool MediaProxy::servePrebuffer(const std::shared_ptr<Job> &job) {
    const std::shared_ptr<Prebuffer> pb = job->stream->pre;
    if (!pb || !job->client) return false;
    if (job->position >= pb->wanted) {
        // Served out of the pre-buffer up to its end while its request is still finishing: a moment later its warm connection is free.
        if (job->usedPrebuffer && !pb->done && !pb->failed) {
            bool queued = false;
            for (std::size_t i = 0; i < pb->waiters.size(); ++i) if (pb->waiters[i].lock() == job) queued = true;
            if (!queued) pb->waiters.push_back(job);
            return true;
        }
        return false;
    }
    const std::int64_t have = pb->data.size();
    if (job->position < have) {
        const std::int64_t upTo = std::min<std::int64_t>(have, job->end + 1);
        const std::int64_t count = upTo - job->position;
        if (!job->firstByteLogged) {
            job->firstByteLogged = true;
            ++counters_.prebufferHits;
            logLine("sailtune.media phase first_byte since_open_ms " + std::to_string(now() - job->stream->openedAt) +
                    " since_request_ms " + std::to_string(now() - job->startedAt) + " from prebuffer");
        }
        job->client->write(pb->data.constData() + job->position, qint64(count));
        job->position += count;
        job->usedPrebuffer = true;
        if (job->position > job->end) { finishJob(job, true); return true; }
        if (job->position >= pb->wanted) { if (job->chunkIndex == 0) job->chunkIndex = 1; return servePrebuffer(job); }   // on to the network (once its warm connection is free)
    }
    // Everything that has arrived is out. Wait for more unless the pre-buffer is over.
    if (pb->done || pb->failed) return false;
    bool queued = false;
    for (std::size_t i = 0; i < pb->waiters.size(); ++i) if (pb->waiters[i].lock() == job) queued = true;
    if (!queued) pb->waiters.push_back(job);
    return true;
}

void MediaProxy::flowControl(const std::shared_ptr<Job> &job) {
    if (!job->upstream || !job->client) return;
    if (job->client->bytesToWrite() > options_.clientBufferHighBytes && !job->upstream->paused()) {
        job->upstream->setPaused(true);     // resumed from the bytesWritten handler of the client socket
        ++counters_.pauses;
    }
}

void MediaProxy::chunkEnded(const std::shared_ptr<Job> &job, const HttpsResult &result) {
    if (job->finished) return;
    const std::int64_t expected = job->chunkTo - job->chunkFrom + 1;
    if (result.transportOk && result.status == 206 && job->chunkGot == expected) {
        ++counters_.chunks;
        ++job->chunkIndex;
        job->redirects = 0;
        job->deaths = 0; job->earlyElapsedMs = 0;
        fetchNext(job);
        return;
    }
    // A request that was cut short or refused. What was delivered stays delivered: resume at the same byte.
    const bool progress = job->chunkGot > 0;
    if (progress) { job->deaths = 0; job->earlyElapsedMs = 0; }
    if (job->position > job->end) { finishJob(job, true); return; }      // everything was delivered; only the end of the answer was untidy
    const bool refused = result.status == 403 || result.status == 401 || result.status == 410 || result.status == 404;
    if (refused && !progress) ++counters_.refused;
    logLine("sailtune.media phase chunk_end status " + std::to_string(result.status) + " class " +
            (result.statusClass.empty() ? "none" : result.statusClass) + " got " + std::to_string(job->chunkGot) + " of " + std::to_string(expected));
    if (result.status == 416) { finishJob(job, false, true); return; }
    // The media host may answer a ranged request with a redirect to another host of its CDN (same file, another node):
    // follow it - a few hops, only to the CDN's own hosts over HTTPS - and keep that address for the rest of the stream.
    if (result.status >= 300 && result.status < 400 && !progress) {
        if (job->redirects < 3 && isMediaCdnAddress(result.location) && result.location != job->stream->source.url) {
            ++job->redirects; ++counters_.redirects;
            job->stream->source.url = result.location;
            logLine("sailtune.media phase redirect followed hop " + std::to_string(job->redirects));
            fetchChunk(job);
            return;
        }
        logLine("sailtune.media phase redirect refused");
        refresh(job, "redirect");
        return;
    }
    if (refused) {
        const std::int64_t age = now() - job->stream->issuedAt;
        // A freshly issued address can be refused for a few seconds: ask again in place before replacing it.
        if (!progress && age < options_.earlyRetryWindowMs && job->earlyElapsedMs < options_.earlyRetryWindowMs) {
            job->earlyElapsedMs += options_.earlyRetryIntervalMs;
            ++counters_.earlyRetries;
            std::weak_ptr<Job> weak(job);
            QTimer::singleShot(options_.earlyRetryIntervalMs, this, [this, weak]() { if (std::shared_ptr<Job> j = weak.lock()) fetchChunk(j); });
            return;
        }
        refresh(job, "refused");
        return;
    }
    if (!progress) ++job->deaths;
    if (job->deaths > options_.maxDeathsWithoutProgress) { logLine("sailtune.media phase give_up deaths " + std::to_string(job->deaths)); finishJob(job, false, true); return; }
    ++counters_.resumed;
    if (job->deaths >= 2) { refresh(job, "died"); return; }
    std::weak_ptr<Job> weak(job);
    QTimer::singleShot(job->deaths == 0 ? 0 : 250, this, [this, weak]() { if (std::shared_ptr<Job> j = weak.lock()) fetchChunk(j); });
}

void MediaProxy::refresh(const std::shared_ptr<Job> &job, const char *why) {
    if (job->finished || job->refreshing) return;
    const std::shared_ptr<Stream> stream = job->stream;
    const std::int64_t t = now();
    std::vector<std::int64_t> recent;
    for (std::size_t i = 0; i < stream->refreshTimes.size(); ++i) if (t - stream->refreshTimes[i] < 60000) recent.push_back(stream->refreshTimes[i]);
    stream->refreshTimes.swap(recent);
    if (!stream->refresher || int(stream->refreshTimes.size()) >= options_.maxRefreshesPerMinute) {
        logLine(std::string("sailtune.media phase refresh denied why ") + why);
        finishJob(job, false, true); return;
    }
    stream->refreshTimes.push_back(t);
    ++counters_.refreshes;
    job->refreshing = true;
    logLine(std::string("sailtune.media phase refresh why ") + why + " at " + std::to_string(job->position));
    std::weak_ptr<Job> weak(job);
    // A host that never answers does not hold the player's connection for ever.
    if (!job->refreshWatchdog) {
        job->refreshWatchdog = new QTimer(this);
        job->refreshWatchdog->setSingleShot(true);
    }
    job->refreshWatchdog->disconnect();
    connect(job->refreshWatchdog, &QTimer::timeout, this, [this, weak]() {
        std::shared_ptr<Job> j = weak.lock();
        if (!j || j->finished || !j->refreshing) return;
        logLine("sailtune.media phase refresh result timeout");
        finishJob(j, false, true);
    });
    job->refreshWatchdog->start(options_.refreshTimeoutMs);
    stream->refresher([this, weak, stream](const FreshSource &fresh) {
        // The same format byte for byte, a different address, or nothing: another length or type would splice two files,
        // and the address that was just refused is no remedy.
        const bool allowed = fresh.ok && (options_.allowAnyHostForTesting || allowedAddress(fresh.source.url));
        const bool same = allowed && fresh.source.contentLength == stream->source.contentLength &&
                          fresh.source.mimeType == stream->source.mimeType;
        const bool replaced = same && fresh.source.url != stream->source.url;
        // The shared stream takes the new address whether or not the request that asked is still there: the
        // next request of the player must not start on the address that was refused.
        if (replaced) {
            stream->source = fresh.source;
            stream->issuedAt = fresh.source.issuedAtUnixMs > 0 ? fresh.source.issuedAtUnixMs : now();
            ++stream->generation;
        }
        std::shared_ptr<Job> j = weak.lock();
        if (!j || j->finished || !j->refreshing) return;
        j->refreshing = false;
        if (j->refreshWatchdog) j->refreshWatchdog->stop();
        if (!replaced) {
            if (allowed && !same) ++counters_.identityChanged;
            logLine(std::string("sailtune.media phase refresh result ") + (!allowed ? "failed" : !same ? "identity_changed" : "same_address"));
            finishJob(j, false, true); return;
        }
        j->earlyElapsedMs = 0;
        logLine("sailtune.media phase refresh result ok");
        fetchChunk(j);
    });
}

void MediaProxy::finishJob(const std::shared_ptr<Job> &job, bool complete, bool linger) {
    if (job->finished) return;
    job->finished = true;
    if (job->refreshWatchdog) { job->refreshWatchdog->stop(); job->refreshWatchdog->deleteLater(); job->refreshWatchdog = nullptr; }
    if (job->upstream) { job->upstream->disconnect(this); HttpsConnection *upstream = job->upstream; job->upstream = nullptr; poolUpstream(upstream, job->upstreamHost); }
    if (job->client) {
        QTcpSocket *client = job->client;
        job->client = nullptr;
        client->disconnect(this);
        if ((complete || linger) && client->state() == QAbstractSocket::ConnectedState) {
            // Let the queued bytes go out, then close (FIN, not RST: what was sent stays readable). The tail of a
            // track can be minutes of audio and the player reads it at playback speed, so there is no deadline on
            // the whole: a client that takes nothing for ten seconds is dropped.
            connect(client, &QAbstractSocket::disconnected, client, &QObject::deleteLater);
            if (client->bytesToWrite() > 0) {
                QTimer *watch = new QTimer(client);
                std::shared_ptr<qint64> last(new qint64(client->bytesToWrite()));
                connect(watch, &QTimer::timeout, client, [client, last]() {
                    const qint64 left = client->bytesToWrite();
                    if (left > 0 && left >= *last) client->abort(); else *last = left;
                });
                watch->start(10000);
            }
            client->disconnectFromHost();
        } else {
            client->abort();
            client->deleteLater();
        }
    }
    for (std::size_t i = 0; i < jobs_.size(); ++i) if (jobs_[i] == job) { jobs_.erase(jobs_.begin() + long(i)); break; }
}

}}}}
