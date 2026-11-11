// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Qt Core + Network checks of SharedStreamSource, the adapter from SailTune's pinned component
// (Shared/SailTune/Core/SharedPlayback, read-only) to Shared/Playback/StreamSource.h:
// - against a fake fast resolver (the component's real domain types, real MediaProxy): mapping,
//   probing a delivered rung through the component, fallback, Final without fallback, video and
//   audio through the proxy (which follows a googlevideo redirect itself), HLS unproxied,
//   refresher, cancel, prefetch cap, warmUp; SailTune's strike rules (a refused fast address is a
//   strike and sends the video to the extractor; a connection loss or a stall is neither; an
//   unexplained failure retries the fast path once); peekCached and prefetch reuse;
// - against the real FastStreamResolver and SailTune's FakeProvider through the component's test
//   seams: its probe follows a 302 to an allow-listed host, refuses a foreign one, stops a redirect
//   chain at 3 hops; and end to end.
// A local TLS server plays the media hosts for the proxy (every *.googlevideo.com host is routed to it).
// Build: tests/shared_stream_source/run.sh
#include <cstdio>
static int failures = 0, checks = 0;
#define CHECK(c) do { ++checks; if (!(c)) { ++failures; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

#include "Core/Player/Infrastructure/SharedStreamSourceSailTune.h"
#include "Shared/SailTune/tests/sharedplayback/FakeProvider.h" // read-only SailTune test helper (pinned)

#include <QCoreApplication>
#include <QHostAddress>
#include <QTcpSocket>
#include <map>
#include <memory>

using namespace nordtile;
namespace sp = nordtile::music::sharedplayback;
using playback::StreamError;
using playback::StreamErrorClass;
using playback::StreamRequest;
using playback::StreamSet;

static const char *const vid = "AAAAAAAAAAA";
static const int mediaSize = 20000;
static const char *const userAgent = "Test-UA/1.0";

// ---- the media host ---------------------------------------------------------------------------
// Routes by the "k" query parameter: hop (302 to another googlevideo host), forbid (403); anything
// else is served. Range is honoured against `media`.
struct MediaRequest { QByteArray target, range, userAgent, secFetchMode; };

class MediaHost : public QTcpServer {
public:
    QByteArray media;
    std::vector<MediaRequest> requests;
    std::map<QByteArray, int> seen;

    int count(const char *needle) const {
        int n = 0;
        for (const MediaRequest &r : requests) if (r.target.contains(needle)) ++n;
        return n;
    }

protected:
    void incomingConnection(qintptr fd) override {
        QSslSocket *socket = new QSslSocket(this);
        socket->setSocketDescriptor(fd);
        socket->setLocalCertificate(QSslCertificate(QByteArray(kTestCertPem)));
        socket->setPrivateKey(QSslKey(QByteArray(kTestKeyPem), QSsl::Rsa));
        const std::shared_ptr<QByteArray> buffer(new QByteArray);
        connect(socket, &QIODevice::readyRead, this, [this, socket, buffer]() {
            buffer->append(socket->readAll());
            const int end = buffer->indexOf("\r\n\r\n");
            if (end < 0) return;
            const QList<QByteArray> lines = buffer->left(end).split('\n');
            buffer->remove(0, end + 4);
            MediaRequest r;
            r.target = lines.value(0).trimmed().split(' ').value(1);
            for (int i = 1; i < lines.size(); ++i) {
                const int colon = lines.at(i).indexOf(':');
                if (colon <= 0) continue;
                const QByteArray name = lines.at(i).left(colon).trimmed().toLower(), value = lines.at(i).mid(colon + 1).trimmed();
                if (name == "range") r.range = value;
                else if (name == "user-agent") r.userAgent = value;
                else if (name == "sec-fetch-mode") r.secFetchMode = value;
            }
            requests.push_back(r);
            respond(socket, r);
        });
        connect(socket, &QAbstractSocket::disconnected, this, [socket]() { socket->deleteLater(); });
        socket->startServerEncryption();
    }

private:
    static QByteArray param(const QByteArray &target, const char *name) {
        const int q = target.indexOf('?');
        if (q < 0) return QByteArray();
        for (const QByteArray &kv : target.mid(q + 1).split('&'))
            if (kv.startsWith(QByteArray(name) + '=')) return kv.mid(int(qstrlen(name)) + 1);
        return QByteArray();
    }
    void redirect(QSslSocket *socket, const QByteArray &location) {
        socket->write("HTTP/1.1 302 Found\r\nLocation: " + location + "\r\nContent-Length: 0\r\n\r\n");
    }
    void respond(QSslSocket *socket, const MediaRequest &r) {
        const QByteArray k = param(r.target, "k");
        ++seen[k];
        if (k == "hop") { redirect(socket, "https://rr3---sn-other.googlevideo.com/videoplayback?k=ok&from=hop&itag=" + param(r.target, "itag")); return; }
        if (k == "forbid") { socket->write("HTTP/1.1 403 Forbidden\r\nContent-Length: 0\r\n\r\n"); return; }
        qint64 from = 0, to = media.size() - 1;
        if (r.range.startsWith("bytes=")) {
            const QList<QByteArray> parts = r.range.mid(6).split('-');
            from = parts.value(0).toLongLong();
            if (!parts.value(1).isEmpty()) to = qMin<qint64>(parts.value(1).toLongLong(), media.size() - 1);
        }
        const QByteArray body = media.mid(int(from), int(to - from + 1));
        socket->write("HTTP/1.1 206 Partial Content\r\nContent-Type: application/octet-stream\r\nContent-Range: bytes " +
                      QByteArray::number(from) + "-" + QByteArray::number(to) + "/" + QByteArray::number(media.size()) +
                      "\r\nContent-Length: " + QByteArray::number(body.size()) + "\r\n\r\n" + body);
    }
};

// A player: one plain request to the loopback proxy, read until it closes.
struct Reply { int status = 0; QByteArray body; };
static Reply fetchLocal(const QUrl &url, const QByteArray &range = QByteArray()) {
    Reply reply;
    QTcpSocket socket;
    socket.connectToHost(QHostAddress::LocalHost, quint16(url.port()));
    if (!socket.waitForConnected(3000)) return reply;
    QByteArray request = "GET " + url.path(QUrl::FullyEncoded).toLatin1() + " HTTP/1.1\r\nHost: 127.0.0.1\r\n";
    if (!range.isEmpty()) request += "Range: " + range + "\r\n";
    socket.write(request + "\r\n");
    QByteArray all;
    QElapsedTimer timer;
    timer.start();
    while (socket.state() == QAbstractSocket::ConnectedState && timer.elapsed() < 10000) {
        socket.waitForReadyRead(20);
        all += socket.readAll();
        QCoreApplication::processEvents();
    }
    all += socket.readAll();
    const int end = all.indexOf("\r\n\r\n");
    if (end < 0) return reply;
    reply.status = all.left(end).split(' ').value(1).toInt();
    reply.body = all.mid(end + 4);
    return reply;
}

// ---- fakes ------------------------------------------------------------------------------------

// The fast resolver's shape (SharedStreamSource.h, Api::Resolver) over the component's real types.
class FakeFast : public QObject {
public:
    typedef unsigned long long RequestId;
    typedef std::function<void(const sp::domain::ResolveOutcome &)> Callback;
    struct Counters {
        int fast = 3, cache = 1, fallbacks = 0, botWalls = 0, probeRejected = 0, network = 0;
        int medianTotalMs() const { return 42; }
    };
    std::function<sp::domain::ResolveOutcome(const std::string &, const sp::domain::StreamRequest &)> answer;
    int warmUps = 0;
    std::vector<std::string> resolved, invalidated;
    std::vector<sp::domain::StreamRequest> requests;
    std::vector<RequestId> cancels;
    std::vector<std::pair<std::string, std::int64_t> > refused;

    void warmUp() { ++warmUps; }
    RequestId resolve(const std::string &videoId, const sp::domain::StreamRequest &request, const Callback &callback) {
        const RequestId id = next_++;
        resolved.push_back(videoId);
        requests.push_back(request);
        const sp::domain::ResolveOutcome outcome = answer ? answer(videoId, request) : sp::domain::ResolveOutcome();
        if (cacheAnswers && outcome.ok()) cached[request.cacheKey(videoId)] = outcome.streams;
        pending_[id] = callback;
        QTimer::singleShot(0, this, [this, id, outcome]() { deliver(id, outcome); });
        return id;
    }
    void cancel(RequestId id) {
        cancels.push_back(id);
        if (!pending_.count(id)) return;
        sp::domain::ResolveOutcome o;
        o.failure = sp::domain::FailureKind::Cancelled;
        QTimer::singleShot(0, this, [this, id, o]() { deliver(id, o); });
    }
    void invalidate(const std::string &videoId) { invalidated.push_back(videoId); forget(videoId); }
    // The component's cache as peekCached shows it (5611210): with cacheAnswers, every ok answer
    // is kept under the component's own key until the video is invalidated or refused.
    bool cacheAnswers = false;
    std::map<std::string, sp::domain::StreamSet> cached;
    mutable int peeks = 0;
    bool peekCached(const std::string &videoId, const sp::domain::StreamRequest &request, sp::domain::StreamSet *out) const {
        ++peeks;
        const auto it = cached.find(request.cacheKey(videoId));
        if (it == cached.end()) return false;
        if (out) { *out = it->second; out->path = sp::domain::ResolvePath::Cache; }
        return true;
    }
    // The component's probe of a rung the host delivers (fd49627): scripted verdict and final address.
    enum class ProbeVerdict { Accepted, NoVerdict, Refused, Unusable };
    typedef std::function<void(ProbeVerdict, int, int, const std::string &)> ProbeDone;
    std::function<std::pair<ProbeVerdict, std::string>(const std::string &url)> probe;
    std::vector<std::string> probed;
    void probeAddress(const std::string &url, const std::string &userAgent, std::int64_t contentLength, const ProbeDone &done) {
        probed.push_back(url);
        probeUserAgents.push_back(userAgent);
        probeLengths.push_back(contentLength);
        const std::pair<ProbeVerdict, std::string> v = probe ? probe(url) : std::make_pair(ProbeVerdict::Accepted, url);
        QTimer::singleShot(5, this, [done, v]() { done(v.first, v.first == ProbeVerdict::Accepted ? 206 : 403, 5, v.second); });
    }
    std::vector<std::string> probeUserAgents;
    std::vector<std::int64_t> probeLengths;
    void reportRefused(const std::string &videoId, std::int64_t msLeft) { refused.push_back(std::make_pair(videoId, msLeft)); forget(videoId); }
    bool allowed() const { return true; }
    bool hasVisitor() const { return warmUps > 0; }
    const Counters &counters() const { return counters_; }
    sp::domain::CircuitBreaker &breaker() { return breaker_; }

private:
    void forget(const std::string &videoId) {
        for (auto it = cached.begin(); it != cached.end();) {
            if (it->second.videoId == videoId) it = cached.erase(it);
            else ++it;
        }
    }
    void deliver(RequestId id, const sp::domain::ResolveOutcome &o) {
        auto it = pending_.find(id);
        if (it == pending_.end()) return;
        const Callback c = it->second;
        pending_.erase(it);
        c(o);
    }
    RequestId next_ = 1;
    std::map<RequestId, Callback> pending_;
    Counters counters_;
    sp::domain::CircuitBreaker breaker_;
};

// The injected extractor (the yt-dlp adapter's place).
class FakeExtractor : public QObject, public playback::StreamResolver {
public:
    StreamSet answer;
    StreamError error;
    std::vector<StreamRequest> resolved, prefetched;
    std::vector<quint64> cancels;
    std::vector<QString> invalidated;
    quint64 resolve(const StreamRequest &request, Completion completion) override {
        const quint64 op = next_++;
        resolved.push_back(request);
        StreamSet s = answer;
        s.videoId = request.videoId;
        const StreamError e = error;
        QTimer::singleShot(0, this, [completion, s, e]() { completion(s, e); });
        return op;
    }
    void prefetch(const StreamRequest &request) override { prefetched.push_back(request); }
    void cancel(quint64 op) override { cancels.push_back(op); }
    void invalidate(const QString &videoId) override { invalidated.push_back(videoId); }
    // A refused source: the cached answers go too (as the default reportRefused does).
    std::vector<QString> refused;
    void reportRefused(const QString &videoId, const playback::StreamFormat &) override {
        refused.push_back(videoId);
        invalidated.push_back(videoId);
    }
    QVariantMap diagnostics() const override {
        QVariantMap d;
        d.insert(QStringLiteral("extractor"), static_cast<qint64>(resolved.size()));
        return d;
    }
private:
    quint64 next_ = 1;
};

// An extractor that takes typed faults, as the yt-dlp adapter does (player::SourceFaultSink).
class SinkExtractor : public FakeExtractor, public player::SourceFaultSink {
public:
    std::vector<std::pair<std::string, player::SourceFault> > faults;
    void reportFault(const std::string &videoId, player::SourceFault fault) override { faults.push_back(std::make_pair(videoId, fault)); }
};

struct TestApi : player::SailTuneSharedPlayback {
    typedef FakeFast Resolver; // the rest is the pinned component
};
typedef player::SharedStreamSource<TestApi> Source;

// ---- helpers ----------------------------------------------------------------------------------

static std::int64_t nowMs() { return QDateTime::currentMSecsSinceEpoch(); }

// The adapter's clock for the strike-rule tests: the system clock plus an offset the test moves.
static qint64 clockOffsetMs = 0;
static player::SharedStreamSourceOptions clocked() {
    player::SharedStreamSourceOptions o;
    o.clock = []() { return static_cast<qint64>(QDateTime::currentMSecsSinceEpoch()) + clockOffsetMs; };
    return o;
}
static const qint64 minuteMs = 60 * 1000;

static std::string mediaUrl(const char *k, int itag, const char *host = "rr1---sn-test.googlevideo.com") {
    return std::string("https://") + host + "/videoplayback?k=" + k + "&itag=" + std::to_string(itag) + "&expire=4102444800";
}

static sp::domain::StreamFormat audioFormat(int itag, const char *k, const char *codec = "opus") {
    sp::domain::StreamFormat f;
    f.itag = itag;
    f.url = mediaUrl(k, itag);
    f.mimeType = std::string(codec) == "opus" ? "audio/webm" : "audio/mp4";
    f.codec = codec;
    f.hasAudio = true;
    f.bitrate = 128000;
    f.contentLength = mediaSize;
    f.audioLanguage = "en";
    f.expiresAtUnixMs = nowMs() + 6LL * 3600 * 1000;
    return f;
}

static sp::domain::StreamFormat videoFormat(int itag, const char *k, int height, const char *codec, int fps = 30) {
    sp::domain::StreamFormat f;
    f.itag = itag;
    f.url = mediaUrl(k, itag);
    f.mimeType = std::string(codec).compare(0, 3, "vp9") == 0 ? "video/webm" : "video/mp4";
    f.codec = codec;
    f.hasVideo = true;
    f.bitrate = height * 2000;
    f.contentLength = mediaSize;
    f.width = height * 16 / 9;
    f.height = height;
    f.fps = fps;
    f.expiresAtUnixMs = nowMs() + 6LL * 3600 * 1000;
    return f;
}

static sp::domain::ResolveOutcome okOutcome(const char *audioK = "ok", const char *videoK = "ok") {
    sp::domain::ResolveOutcome o;
    sp::domain::StreamSet &s = o.streams;
    s.videoId = vid;
    s.audio.push_back(audioFormat(251, audioK));
    s.audio.push_back(audioFormat(140, "ok", "mp4a.40.2"));
    s.video.push_back(videoFormat(248, videoK, 1080, "vp9"));
    s.video.push_back(videoFormat(137, videoK, 1080, "avc1.640028"));
    s.video.push_back(videoFormat(299, "ok", 1080, "avc1.64002a", 60));
    s.video.push_back(videoFormat(247, "ok", 720, "vp9"));
    s.userAgent = userAgent;
    s.expiresAtUnixMs = nowMs() + 6LL * 3600 * 1000;
    s.hasLoudness = true;
    s.loudnessDb = 3.5;
    s.path = sp::domain::ResolvePath::Fast;
    s.timings.totalMs = 120;
    s.timings.visitorMs = 30;
    s.timings.playerMs = 80;
    return o;
}

static sp::domain::ResolveOutcome failed(sp::domain::FailureKind kind, bool fallback, const char *reason = "x") {
    sp::domain::ResolveOutcome o;
    o.failure = kind;
    o.fallback = fallback;
    o.reason = reason;
    return o;
}

static StreamRequest request(playback::StreamPurpose purpose = playback::StreamPurpose::Video) {
    StreamRequest r;
    r.videoId = QString::fromLatin1(vid);
    r.purpose = purpose;
    return r;
}

struct Answer { StreamSet streams; StreamError error; };
typedef std::vector<Answer> Answers;
static playback::StreamResolver::Completion into(Answers *list) {
    return [list](const StreamSet &s, const StreamError &e) { Answer a; a.streams = s; a.error = e; list->push_back(a); };
}

struct Opened { QUrl url; StreamError error; };
typedef std::vector<Opened> OpenedList;
static playback::StreamFetcher::Opened openedInto(OpenedList *list) {
    return [list](const QUrl &u, const StreamError &e) { Opened o; o.url = u; o.error = e; list->push_back(o); };
}

static void settle(int ms) { waitFor([]() { return false; }, ms); }
static qint64 number(const QVariantMap &d, const char *key) { return d.value(QLatin1String(key)).toLongLong(); }
static QByteArray header(const playback::StreamFormat &f, const char *name) {
    for (const auto &h : f.headers) if (h.first == name) return h.second;
    return QByteArray();
}
static bool loopback(const QUrl &u) {
    return u.scheme() == QLatin1String("http") && u.host() == QLatin1String("127.0.0.1") && u.path().startsWith(QLatin1String("/s/"));
}

struct Rig {
    MediaHost host;
    FakeFast fast;
    FakeExtractor extractor;
    SinkExtractor sink;          // the fallback instead of `extractor` with typedFallback
    std::unique_ptr<sp::infrastructure::MediaProxy> proxy;
    std::unique_ptr<Source> source;

    explicit Rig(bool withFallback = true, const player::SharedStreamSourceOptions &options = player::SharedStreamSourceOptions(),
                 bool typedFallback = false) {
        CHECK(host.listen(QHostAddress::LocalHost, 0));
        host.media = QByteArray(mediaSize, '\0');
        for (int i = 0; i < mediaSize; ++i) host.media[i] = char((i * 7 + 3) & 0xff);
        const quint16 port = host.serverPort();
        const auto route = [port](const QString &, QString *connectHost, quint16 *connectPort) {
            *connectHost = QStringLiteral("127.0.0.1");
            *connectPort = port;
            return true;
        };
        sp::infrastructure::MediaProxy::Options po;
        po.ignoreSslErrorsForTesting = true;
        po.endpointOverride = route;
        po.earlyRetryWindowMs = 0;       // a refusal goes straight to the refresher
        po.upstreamTimeoutMs = 3000;
        proxy.reset(new sp::infrastructure::MediaProxy(po));
        playback::StreamResolver *fallback = !withFallback ? nullptr : typedFallback ? static_cast<playback::StreamResolver *>(&sink) : &extractor;
        source.reset(new Source(&fast, proxy.get(), fallback, options));
        // The answer is for the video that was asked for.
        fast.answer = [](const std::string &id, const sp::domain::StreamRequest &) {
            sp::domain::ResolveOutcome o = okOutcome();
            o.streams.videoId = id;
            return o;
        };
    }
    Answers resolve(const StreamRequest &r) {
        Answers got;
        source->resolve(r, into(&got));
        waitFor([&]() { return !got.empty(); }, 10000);
        return got;
    }
    Opened open(const playback::StreamFormat &f) {
        OpenedList got;
        source->fetcher()->open(f, openedInto(&got));
        waitFor([&]() { return !got.empty(); }, 10000);
        return got.empty() ? Opened() : got.front();
    }
};

static std::vector<std::string> ids(const QList<playback::StreamFormat> &list) {
    std::vector<std::string> out;
    for (const playback::StreamFormat &f : list) out.push_back(f.formatId.toStdString());
    return out;
}

// ---- tests ------------------------------------------------------------------------------------

// RES-03/RES-05: fast path answer mapped to a StreamSet, re-ranked by SailTube's ladder; the
// component request carries the mapped fields; warmUp forwarded. RES-06: the component's own top
// rungs are not probed again; a top rung SailTube delivers instead is probed through the component.
static void res03_fastPathMappedAndProbed() {
    Rig rig;
    rig.source->warmUp();
    CHECK(rig.fast.warmUps == 1);
    StreamRequest r = request();
    r.dubLanguage = QStringLiteral("pl");
    r.hardwareDecode = true;
    const Answers got = rig.resolve(r);
    CHECK(got.size() == 1);
    if (got.empty()) return;
    const StreamSet &s = got[0].streams;
    CHECK(got[0].error.ok() && s.videoId == QLatin1String(vid) && s.path == playback::ResolvePath::FastPath);
    // SEL-02: VP9 first with hardware decode, 60 fps after 30 fps at the same height.
    CHECK((ids(s.video) == std::vector<std::string>{"248", "137", "299", "247"}));
    CHECK((ids(s.audio) == std::vector<std::string>{"251", "140"}));
    CHECK(s.resolveMs == 120 && s.phases.visitorMs == 30 && s.phases.requestMs == 80);
    CHECK(s.hasLoudness && s.loudnessDb == 3.5 && s.expiresAtUnixMs > nowMs());
    CHECK(s.audio[0].container == QLatin1String("webm") && s.audio[0].audioCodec == QLatin1String("opus")
          && s.audio[0].videoCodec == QLatin1String("none") && s.audio[0].contentLength == mediaSize && s.audio[0].originalAudio);
    CHECK(header(s.audio[0], "User-Agent") == userAgent && header(s.video[0], "User-Agent") == userAgent);
    // The delivered tops are the component's (251, 248): probed by it, nothing probed here.
    CHECK(rig.fast.probed.empty());
    CHECK(s.audio[0].probe == playback::ProbeState::NoVerdict && s.video[0].probe == playback::ProbeState::NoVerdict);
    CHECK(s.audio[1].probe == playback::ProbeState::NotProbed);
    CHECK(rig.fast.requests.size() == 1);
    if (!rig.fast.requests.empty()) {
        const sp::domain::StreamRequest &c = rig.fast.requests[0];
        CHECK(!c.audioOnly && c.maxVideoHeight == 1080 && c.dubLanguage == "pl" && c.hardwareDecode && c.codecPreference.empty());
    }

    // Without hardware decode SailTube delivers H.264 137 first, not the component's VP9 248:
    // 137 is probed through the component, with its user agent and length.
    const Answers sw = rig.resolve(request());
    CHECK(!sw.empty() && sw[0].error.ok() && !sw[0].streams.video.isEmpty());
    if (!sw.empty() && !sw[0].streams.video.isEmpty()) {
        CHECK(sw[0].streams.video[0].formatId == QLatin1String("137") && sw[0].streams.video[0].probe == playback::ProbeState::Accepted);
        CHECK(sw[0].streams.phases.probeMs >= 0);
    }
    CHECK(rig.fast.probed.size() == 1 && rig.fast.probed[0] == mediaUrl("ok", 137));
    CHECK(!rig.fast.probeUserAgents.empty() && rig.fast.probeUserAgents[0] == userAgent && rig.fast.probeLengths[0] == mediaSize);

    // Audio-only request: no video; AAC preference passed as the component's family.
    StreamRequest a = request(playback::StreamPurpose::AudioOnly);
    a.audioCodecs = QStringList{QStringLiteral("mp4a")};
    const Answers audio = rig.resolve(a);
    CHECK(!audio.empty() && audio[0].error.ok() && audio[0].streams.video.isEmpty());
    CHECK(!audio.empty() && (ids(audio[0].streams.audio) == std::vector<std::string>{"140"}));
    CHECK(rig.fast.requests.size() == 3 && rig.fast.requests[2].audioOnly
          && rig.fast.requests[2].codecPreference == std::vector<std::string>{"aac"});
    CHECK(rig.fast.probed.size() == 2); // 140 is not the component's top (251): probed

    const QVariantMap d = rig.source->diagnostics();
    CHECK(number(d, "fastAnswers") == 3 && number(d, "medianFastPathMs") == 42 && number(d, "probes") == 2
          && number(d, "probeAccepted") == 2);
    CHECK(number(d, "visitorReady") == 1 && number(d, "breakerOpen") == 0);
    for (auto it = d.constBegin(); it != d.constEnd(); ++it) {
        bool isNumber = false;
        it.value().toLongLong(&isNumber);
        CHECK(isNumber); // fixed keys, numbers only
    }
}

// RES-06: the component's probe of a delivered rung answered from another googlevideo node: that
// address is the one in the StreamSet and the one the proxy fetches. Video and audio both proxied.
static void res06_deliveredRungRedirectedAndProxied() {
    Rig rig;
    rig.fast.probe = [](const std::string &url) {
        std::string final = url;
        if (url.find("itag=137") != std::string::npos)
            final = "https://rr3---sn-other.googlevideo.com/videoplayback?k=ok&from=hop&itag=137";
        return std::make_pair(FakeFast::ProbeVerdict::Accepted, final);
    };
    const Answers got = rig.resolve(request());
    CHECK(got.size() == 1 && got[0].error.ok());
    if (got.empty() || got[0].streams.audio.isEmpty() || got[0].streams.video.isEmpty()) return;
    const StreamSet &s = got[0].streams;
    CHECK(s.video[0].formatId == QLatin1String("137") && s.video[0].url.host() == QLatin1String("rr3---sn-other.googlevideo.com"));
    const Opened audio = rig.open(s.audio[0]);
    const Opened video = rig.open(s.video[0]);
    CHECK(audio.error.ok() && loopback(audio.url));
    CHECK(video.error.ok() && loopback(video.url) && video.url != audio.url);
    CHECK(rig.fast.probed.size() == 1); // already probed: not again at open
    const Reply part = fetchLocal(audio.url, "bytes=0-99");
    CHECK(part.status == 206 && part.body == rig.host.media.left(100));
    const Reply whole = fetchLocal(video.url);
    CHECK(whole.status == 200 && whole.body == rig.host.media);
    CHECK(rig.host.count("from=hop") >= 1);
    const QVariantMap d = rig.source->fetcher()->diagnostics();
    CHECK(number(d, "proxied") == 2 && number(d, "direct") == 0 && number(d, "proxyListening") == 1);
    // 5611210: the proxy fetched the start of each stream at open(); both requests above began in it.
    CHECK(number(d, "proxy.prebufferHits") >= 2 && number(d, "proxy.prebufferFailures") == 0);
    CHECK(d.contains(QStringLiteral("proxy.tailHits")) && d.contains(QStringLiteral("proxy.tailFailures"))
          && d.contains(QStringLiteral("proxy.upstreamReused")) && d.contains(QStringLiteral("proxy.redirects")));
}

// RES-06 / RES-08: a delivered rung the component refuses (or finds unusable) sends the resolve
// to the extractor; the component counted the strike itself, the adapter drops its cache.
static void res06_deliveredRungRefusedFallsBack() {
    for (FakeFast::ProbeVerdict verdict : {FakeFast::ProbeVerdict::Refused, FakeFast::ProbeVerdict::Unusable}) {
        Rig rig;
        rig.fast.probe = [verdict](const std::string &url) { return std::make_pair(verdict, url); };
        playback::StreamFormat x;
        x.formatId = QStringLiteral("251");
        x.audioCodec = QStringLiteral("opus");
        x.videoCodec = QStringLiteral("none");
        x.url = QUrl(QStringLiteral("https://media.example.com/a.webm"));
        rig.extractor.answer.audio << x;
        playback::StreamFormat v = x;
        v.formatId = QStringLiteral("137");
        v.videoCodec = QStringLiteral("avc1.640028");
        v.audioCodec = QStringLiteral("none");
        rig.extractor.answer.video << v;
        rig.extractor.answer.path = playback::ResolvePath::Extractor;
        const Answers got = rig.resolve(request()); // delivers 137, not the component's 248
        CHECK(got.size() == 1 && got[0].error.ok() && got[0].streams.path == playback::ResolvePath::Extractor);
        CHECK(rig.extractor.resolved.size() == 1 && rig.fast.probed.size() == 1);
        CHECK(rig.fast.refused.empty() && rig.fast.invalidated.size() == 1 && rig.fast.invalidated[0] == vid);
        const QVariantMap d = rig.source->diagnostics();
        CHECK(number(d, verdict == FakeFast::ProbeVerdict::Refused ? "probeRefused" : "probeUnusable") == 1);
        // A non-googlevideo extractor format is handed over directly, unproxied.
        if (!got.empty() && !got[0].streams.audio.isEmpty()) {
            const Opened o = rig.open(got[0].streams.audio[0]);
            CHECK(o.error.ok() && o.url == x.url);
        }
    }
    // A rung opened later that nobody probed (a quality switch) is probed first; refused -> error.
    Rig rig;
    const Answers got = rig.resolve(request(playback::StreamPurpose::Video));
    rig.fast.probe = [](const std::string &url) { return std::make_pair(FakeFast::ProbeVerdict::Refused, url); };
    if (got.empty() || got[0].streams.video.size() < 4) { CHECK(false); return; }
    const std::size_t before = rig.fast.probed.size();
    const Opened o = rig.open(got[0].streams.video[3]); // 247, never probed
    CHECK(rig.fast.probed.size() == before + 1);
    CHECK(o.url.isEmpty() && o.error.kind == StreamErrorClass::NoPlayableStream);
    CHECK(number(rig.source->fetcher()->diagnostics(), "refusedAtOpen") == 1);
}

// RES-08: a fallback outcome goes to the extractor with the same request; its googlevideo
// formats are probed when opened and proxied; RES-09: a Final outcome never falls back.
static void res08_fallbackAndFinal() {
    Rig rig;
    rig.fast.answer = [](const std::string &, const sp::domain::StreamRequest &) {
        return failed(sp::domain::FailureKind::BotWall, true, "bot_wall");
    };
    playback::StreamFormat a;
    a.formatId = QStringLiteral("251");
    a.audioCodec = QStringLiteral("opus");
    a.videoCodec = QStringLiteral("none");
    a.container = QStringLiteral("webm");
    a.contentLength = mediaSize;
    a.url = QUrl::fromEncoded(QByteArray::fromStdString(mediaUrl("hop", 251)));
    a.headers << qMakePair(QByteArray("User-Agent"), QByteArray("ytdlp-UA/1"));
    rig.extractor.answer.audio << a;
    rig.extractor.answer.path = playback::ResolvePath::Extractor;
    // The extractor's storyboard (scrub preview) comes through with its answer.
    playback::StoryboardSheet sheet;
    sheet.url = QUrl(QStringLiteral("https://i.ytimg.com/sb/x/storyboard3_L2/M0.jpg"));
    sheet.durationMs = 100000;
    rig.extractor.answer.storyboard.width = 160;
    rig.extractor.answer.storyboard.height = 90;
    rig.extractor.answer.storyboard.columns = 5;
    rig.extractor.answer.storyboard.rows = 5;
    rig.extractor.answer.storyboard.sheets << sheet;
    StreamRequest r = request(playback::StreamPurpose::AudioOnly);
    r.signedIn = true;
    const Answers got = rig.resolve(r);
    CHECK(got.size() == 1 && got[0].error.ok() && got[0].streams.path == playback::ResolvePath::Extractor);
    CHECK(got.size() == 1 && got[0].streams.storyboard.sheets.size() == 1 && got[0].streams.storyboard.width == 160
          && got[0].streams.storyboard.sheets[0].url == sheet.url);
    CHECK(rig.extractor.resolved.size() == 1 && rig.extractor.resolved[0].signedIn && rig.extractor.resolved[0].videoId == QLatin1String(vid));
    if (!got.empty() && !got[0].streams.audio.isEmpty()) {
        const Opened o = rig.open(got[0].streams.audio[0]);
        CHECK(o.error.ok() && loopback(o.url));            // extractor rung: proxied as it is
        CHECK(rig.fast.probed.empty());                    // no probe of its own
        // 5611210: the proxy asks for the start of the stream at open(); nothing else was sent.
        for (const MediaRequest &m : rig.host.requests) CHECK(m.range.startsWith("bytes=0-"));
        const Reply part = fetchLocal(o.url, "bytes=10-19");
        CHECK(part.status == 206 && part.body == rig.host.media.mid(10, 10));
        // The pinned MediaProxy followed the googlevideo redirect itself.
        CHECK(rig.host.count("k=hop") >= 1 && rig.host.count("from=hop") >= 1);
        CHECK(rig.proxy->counters().redirects >= 1);
        bool sawUa = false;
        for (const MediaRequest &m : rig.host.requests) sawUa = sawUa || m.userAgent == "ytdlp-UA/1";
        CHECK(sawUa);
    }
    CHECK(number(rig.source->diagnostics(), "fallbacks") == 1 && number(rig.source->diagnostics(), "extractor.extractor") == 1);

    Rig final;
    final.fast.answer = [](const std::string &, const sp::domain::StreamRequest &) {
        return failed(sp::domain::FailureKind::Final, false, "final");
    };
    const Answers f = final.resolve(request());
    CHECK(f.size() == 1 && f[0].error.kind == StreamErrorClass::Unavailable && f[0].error.final);
    // The unnamed Final is only named by the extractor (it answered nothing usable here); no
    // other fallback (res09_finalReasonsNamed).
    CHECK(final.extractor.resolved.size() == 1);
    CHECK(number(final.source->diagnostics(), "finalFailures") == 1 && number(final.source->diagnostics(), "finalLookups") == 1);

    // No split pair for a video request (only AV1 offered): the extractor.
    Rig unpaired;
    unpaired.fast.answer = [](const std::string &, const sp::domain::StreamRequest &) {
        sp::domain::ResolveOutcome o = okOutcome();
        o.streams.video.clear();
        o.streams.video.push_back(videoFormat(399, "ok", 1080, "av01.0.08M.08"));
        return o;
    };
    unpaired.resolve(request());
    CHECK(unpaired.extractor.resolved.size() == 1 && unpaired.fast.probed.empty());

    // Invalid ids never reach either path; no extractor: the fast path's class.
    Rig none(false);
    const Answers bad = none.resolve(request());
    CHECK(bad.size() == 1 && bad[0].error.ok()); // fast answer still fine without an extractor
    StreamRequest invalid = request();
    invalid.videoId = QStringLiteral("short");
    const Answers inv = none.resolve(invalid);
    CHECK(inv.size() == 1 && inv[0].error.kind == StreamErrorClass::InvalidId && inv[0].error.final);
    none.fast.answer = [](const std::string &, const sp::domain::StreamRequest &) {
        return failed(sp::domain::FailureKind::Network, true, "network");
    };
    const Answers net = none.resolve(request());
    CHECK(net.size() == 1 && net[0].error.kind == StreamErrorClass::Network && !net[0].error.final);
    CHECK(none.fast.resolved.size() == 2);
}

// Cause 5 (RES-09, RES-19): a Final outcome keeps its reason. A named label (S-14) maps without the
// extractor; the pinned component's bare "final" is named once by the extractor (never for a
// prefetch, never with nameFinalReasons off), and an upcoming answer carries its start.
static void res09_finalReasonsNamed() {
    using sp::domain::FailureKind;
    CHECK(Source::finalClassFor("removed") == StreamErrorClass::Removed);
    CHECK(Source::finalClassFor("terminated") == StreamErrorClass::Terminated);
    CHECK(Source::finalClassFor("upcoming") == StreamErrorClass::Upcoming);
    CHECK(Source::finalClassFor("geo") == StreamErrorClass::GeoBlocked);
    CHECK(Source::finalClassFor("private") == StreamErrorClass::Private);
    CHECK(Source::finalClassFor("invalid_id") == StreamErrorClass::InvalidId);
    CHECK(Source::finalClassFor("final") == StreamErrorClass::None);
    // The component's other fixed labels (the class used when no extractor is injected).
    struct Row { FailureKind kind; const char *reason; StreamErrorClass error; bool final; };
    const Row rows[] = {
        {FailureKind::Final, "final", StreamErrorClass::Unavailable, true},
        {FailureKind::Final, "removed", StreamErrorClass::Removed, true},
        {FailureKind::Final, "private", StreamErrorClass::Private, true},
        {FailureKind::Unplayable, "login_required", StreamErrorClass::SignInRequired, false},
        {FailureKind::Unplayable, "unplayable", StreamErrorClass::Unavailable, false},
        {FailureKind::Unplayable, "not_ok", StreamErrorClass::Unavailable, false},
        {FailureKind::Unplayable, "live", StreamErrorClass::NoPlayableStream, false},
        {FailureKind::NoUsableFormat, "no_direct_audio", StreamErrorClass::DeliveryChanged, false},
        {FailureKind::BadResponse, "http_429", StreamErrorClass::TryLater, false},
        {FailureKind::BadResponse, "http_500", StreamErrorClass::InvalidResponse, false},
        {FailureKind::ProbeRejected, "http_403", StreamErrorClass::Failed, false},
    };
    for (const Row &row : rows) {
        const StreamError e = Source::errorFor(failed(row.kind, false, row.reason), false);
        CHECK(e.kind == row.error && e.final == row.final);
    }
    CHECK(!Source::errorFor(failed(FailureKind::Final, false, "private"), true).final);  // private signed in

    const auto finalAnswer = [](const char *reason) {
        return [reason](const std::string &, const sp::domain::StreamRequest &) { return failed(FailureKind::Final, false, reason); };
    };
    {   // Named label: no extractor run.
        Rig rig;
        rig.fast.answer = finalAnswer("removed");
        const Answers a = rig.resolve(request());
        CHECK(a.size() == 1 && a[0].error.kind == StreamErrorClass::Removed && a[0].error.final);
        CHECK(rig.extractor.resolved.empty() && number(rig.source->diagnostics(), "finalLookups") == 0);
    }
    {   // Unnamed: the extractor names it.
        Rig rig;
        rig.fast.answer = finalAnswer("final");
        rig.extractor.error = playback::streamError(StreamErrorClass::GeoBlocked, false);
        const Answers a = rig.resolve(request());
        CHECK(a.size() == 1 && a[0].error.kind == StreamErrorClass::GeoBlocked && a[0].error.final);
        CHECK(rig.extractor.resolved.size() == 1 && number(rig.source->diagnostics(), "finalLookups") == 1);
    }
    {   // Upcoming: the start reaches the session (RES-19 "Scheduled to start").
        Rig rig;
        rig.fast.answer = finalAnswer("final");
        rig.extractor.error = playback::streamError(StreamErrorClass::Upcoming, false);
        rig.extractor.answer.liveState = playback::LiveState::Upcoming;
        rig.extractor.answer.scheduledStartUnixMs = 1790000000000LL;
        const Answers a = rig.resolve(request());
        CHECK(a.size() == 1 && a[0].error.kind == StreamErrorClass::Upcoming && a[0].error.final);
        CHECK(!a.empty() && a[0].streams.liveState == playback::LiveState::Upcoming
              && a[0].streams.scheduledStartUnixMs == 1790000000000LL);
    }
    {   // The extractor knows no more (a bare, non-final unavailable): the component's final verdict.
        Rig rig;
        rig.fast.answer = finalAnswer("final");
        StreamError bare = playback::streamError(StreamErrorClass::Unavailable, false);
        bare.final = false;
        rig.extractor.error = bare;
        const Answers a = rig.resolve(request());
        CHECK(a.size() == 1 && a[0].error.kind == StreamErrorClass::Unavailable && a[0].error.final);
    }
    {   // The extractor plays it after all: delivered.
        Rig rig;
        rig.fast.answer = finalAnswer("final");
        playback::StreamFormat muxed;
        muxed.formatId = QStringLiteral("18");
        muxed.videoCodec = QStringLiteral("avc1.42001E");
        muxed.audioCodec = QStringLiteral("mp4a.40.2");
        muxed.url = QUrl::fromEncoded(QByteArray::fromStdString(mediaUrl("ok", 18)));
        rig.extractor.answer.muxed << muxed;
        rig.extractor.answer.path = playback::ResolvePath::Extractor;
        const Answers a = rig.resolve(request());
        CHECK(a.size() == 1 && a[0].error.ok() && a[0].streams.muxed.size() == 1);
    }
    {   // Never for a prefetch, never when switched off.
        Rig rig;
        rig.fast.answer = finalAnswer("final");
        rig.source->prefetch(request());
        settle(100);
        CHECK(rig.extractor.resolved.empty() && rig.extractor.prefetched.empty());
        player::SharedStreamSourceOptions off;
        off.nameFinalReasons = false;
        Rig quiet(true, off);
        quiet.fast.answer = finalAnswer("final");
        const Answers a = quiet.resolve(request());
        CHECK(a.size() == 1 && a[0].error.kind == StreamErrorClass::Unavailable && a[0].error.final);
        CHECK(quiet.extractor.resolved.empty());
    }
}

// RES-19: HLS is handed over as it is, never proxied or probed.
static void res19_hlsUnproxied() {
    Rig rig;
    playback::StreamFormat hls;
    hls.formatId = QStringLiteral("95");
    hls.isHls = true;
    hls.videoCodec = QStringLiteral("avc1.4d401f");
    hls.audioCodec = QStringLiteral("mp4a.40.2");
    hls.contentLength = mediaSize;
    hls.url = QUrl(QStringLiteral("https://manifest.googlevideo.com/api/manifest/hls_variant/index.m3u8"));
    const Opened o = rig.open(hls);
    CHECK(o.error.ok() && o.url == hls.url);
    CHECK(rig.host.requests.empty());
    CHECK(number(rig.source->fetcher()->diagnostics(), "hls") == 1);
}

// RES-12: a refused stream asks the Refresher; it re-resolves the same video (cache dropped),
// probes the same format id and hands its URL to the proxy, which resumes.
static void res12_refresherReResolvesSameFormat() {
    Rig rig;
    int calls = 0;
    rig.fast.answer = [&calls](const std::string &, const sp::domain::StreamRequest &) {
        ++calls;
        return calls == 1 ? okOutcome("forbid", "ok") : okOutcome("fresh", "ok");
    };
    const Answers got = rig.resolve(request(playback::StreamPurpose::AudioOnly));
    CHECK(got.size() == 1 && got[0].error.ok());
    if (got.empty() || got[0].streams.audio.isEmpty()) return;
    const Opened o = rig.open(got[0].streams.audio[0]);
    CHECK(o.error.ok() && loopback(o.url));
    const Reply whole = fetchLocal(o.url);      // the first chunk is refused (403) -> refresh -> resumes
    CHECK(whole.status == 200 && whole.body == rig.host.media);
    CHECK(calls == 2 && rig.fast.invalidated.size() >= 1 && rig.extractor.invalidated.size() >= 1);
    CHECK(rig.host.count("k=fresh") >= 1 && rig.host.count("k=forbid") >= 1);
    CHECK(number(rig.source->fetcher()->diagnostics(), "refreshes") == 1);
    // The proxy's refresh only drops cached answers: no strike, nothing counted against the fast path.
    CHECK(rig.fast.refused.empty() && number(rig.source->diagnostics(), "faultRetries") == 0
          && number(rig.source->diagnostics(), "faultStrikes") == 0 && number(rig.source->diagnostics(), "avoidedVideos") == 0);

    // The refreshed answer lacks the format: the refresh fails and the stream ends.
    Rig gone;
    int n = 0;
    gone.fast.answer = [&n](const std::string &, const sp::domain::StreamRequest &) {
        ++n;
        sp::domain::ResolveOutcome out = okOutcome(n == 1 ? "forbid" : "ok", "ok");
        if (n > 1) out.streams.audio[0].itag = 250;
        return out;
    };
    const Answers g = gone.resolve(request(playback::StreamPurpose::AudioOnly));
    if (g.empty() || g[0].streams.audio.isEmpty()) { CHECK(false); return; }
    const Opened go = gone.open(g[0].streams.audio[0]);
    const Reply cut = fetchLocal(go.url);
    CHECK(cut.body.size() < mediaSize);
    CHECK(number(gone.source->fetcher()->diagnostics(), "refreshFailed") == 1);
}

// Completions exactly once, cancellation included; prefetch cap (RES-11); reportRefused routes
// the strike to the fast path only for its own URLs.
static void contract_cancelPrefetchRefused() {
    Rig rig;
    Answers got;
    const quint64 op = rig.source->resolve(request(), into(&got));
    rig.source->cancel(op);
    rig.source->cancel(op);
    CHECK(got.empty());
    settle(300);
    CHECK(got.size() == 1 && got[0].error.kind == StreamErrorClass::Cancelled && rig.fast.cancels.size() == 1);

    StreamRequest a = request(), b = request(), c = request();
    a.maxHeight = 360; b.maxHeight = 480; c.maxHeight = 720;
    rig.source->prefetch(a);
    rig.source->prefetch(b);
    rig.source->prefetch(c); // replaces a
    CHECK(rig.fast.cancels.size() == 2 && rig.fast.requests.size() == 4 && rig.fast.requests[3].maxVideoHeight == 720);
    settle(500);
    CHECK(number(rig.source->diagnostics(), "prefetchesReplaced") == 1);
    // A speculative resolve that falls back uses the extractor's prefetch, never its resolve.
    rig.fast.answer = [](const std::string &, const sp::domain::StreamRequest &) {
        return failed(sp::domain::FailureKind::Unplayable, true, "login_required");
    };
    rig.source->prefetch(a);
    settle(200);
    CHECK(rig.extractor.prefetched.size() == 1 && rig.extractor.prefetched[0].speculative && rig.extractor.resolved.empty());

    rig.fast.answer = [](const std::string &, const sp::domain::StreamRequest &) { return okOutcome(); };
    const Answers fast = rig.resolve(request());
    if (!fast.empty() && !fast[0].streams.audio.isEmpty()) {
        const std::size_t before = rig.fast.refused.size();
        rig.source->reportRefused(QString::fromLatin1(vid), fast[0].streams.audio[0]);
        CHECK(rig.fast.refused.size() == before + 1 && rig.fast.refused.back().second > 3600 * 1000);
        playback::StreamFormat foreign;
        foreign.url = QUrl(QStringLiteral("https://rr9.googlevideo.com/videoplayback?k=x"));
        rig.source->reportRefused(QString::fromLatin1(vid), foreign);
        CHECK(rig.fast.refused.size() == before + 1);  // not a fast-path URL: no strike
        CHECK(!rig.fast.invalidated.empty() && rig.extractor.invalidated.size() >= 2);
    } else {
        CHECK(false);
    }

    // Closing a stream before delivery completes it once, as Cancelled.
    OpenedList early;
    playback::StreamFormat f;
    f.url = QUrl(QStringLiteral("https://media.example.com/x"));
    const quint64 h = rig.source->fetcher()->open(f, openedInto(&early));
    rig.source->fetcher()->close(h);
    settle(100);
    CHECK(early.size() == 1 && early[0].error.kind == StreamErrorClass::Cancelled);

    // No completion after destruction.
    Answers late;
    {
        Rig gone;
        gone.source->resolve(request(), into(&late));
    }
    settle(300);
    CHECK(late.empty());
}

// RES-07 / RES-12, SailTune's rule 1: a byte-0 refusal of a fast-path address is a strike, and the
// video is the extractor's (full profile first) for five minutes; other videos are not touched;
// without an extractor the fast path is never avoided.
static void res07_refusedFastAddressIsAStrikeAndTheVideoGoesToTheExtractor() {
    clockOffsetMs = 0;
    const StreamRequest audio = request(playback::StreamPurpose::AudioOnly);
    {
        Rig rig(true, clocked(), true);
        const Answers first = rig.resolve(audio);
        CHECK(first.size() == 1 && first[0].error.ok() && first[0].streams.path == playback::ResolvePath::FastPath);
        if (first.empty() || first[0].streams.audio.isEmpty()) return;
        rig.source->reportRefused(QString::fromLatin1(vid), first[0].streams.audio[0]);
        CHECK(rig.fast.refused.size() == 1 && rig.fast.refused[0].first == vid && rig.fast.refused[0].second > 3600 * 1000);
        CHECK(rig.sink.refused.size() == 1 && rig.sink.faults.empty());   // the extractor: full profile first
        // The next resolve of the video is the extractor's; the fast path is not asked.
        const Answers second = rig.resolve(audio);
        CHECK(second.size() == 1 && second[0].error.ok());
        CHECK(rig.fast.resolved.size() == 1 && rig.sink.resolved.size() == 1);
        QVariantMap d = rig.source->diagnostics();
        CHECK(number(d, "faultStrikes") == 1 && number(d, "fastAvoided") == 1 && number(d, "avoidedVideos") == 1);
        // peekCached and prefetch leave such a video alone: nothing is asked on either path.
        CHECK(!rig.source->peekCached(audio, nullptr));
        rig.source->prefetch(audio);
        settle(50);
        CHECK(rig.fast.resolved.size() == 1 && rig.sink.prefetched.empty() && rig.sink.resolved.size() == 1);
        CHECK(number(rig.source->diagnostics(), "prefetchesSkipped") == 1);
        // Another video still takes the fast path.
        StreamRequest other = audio;
        other.videoId = QStringLiteral("BBBBBBBBBBB");
        const Answers b = rig.resolve(other);
        CHECK(b.size() == 1 && b[0].error.ok() && b[0].streams.path == playback::ResolvePath::FastPath && rig.fast.resolved.size() == 2);
        // Five minutes later the fast path is asked again.
        clockOffsetMs = 5 * minuteMs - 2000;
        rig.resolve(audio);
        CHECK(rig.fast.resolved.size() == 2 && rig.sink.resolved.size() == 2);
        clockOffsetMs = 5 * minuteMs + 2000;
        const Answers again = rig.resolve(audio);
        CHECK(again.size() == 1 && again[0].streams.path == playback::ResolvePath::FastPath && rig.fast.resolved.size() == 3);
        CHECK(number(rig.source->diagnostics(), "avoidedVideos") == 0);
        clockOffsetMs = 0;
    }
    {   // A refused address that is not the fast path's: no strike, the fast path stays in front,
        // and the extractor is still told (its token-free pass is not asked first again).
        Rig rig(true, clocked(), true);
        rig.resolve(audio);
        playback::StreamFormat foreign;
        foreign.url = QUrl(QStringLiteral("https://rr9.googlevideo.com/videoplayback?k=x"));
        rig.source->reportRefused(QString::fromLatin1(vid), foreign);
        CHECK(rig.fast.refused.empty() && rig.sink.refused.size() == 1);
        rig.resolve(audio);
        CHECK(rig.fast.resolved.size() == 2 && rig.sink.resolved.empty());
        CHECK(number(rig.source->diagnostics(), "faultStrikes") == 0 && number(rig.source->diagnostics(), "avoidedVideos") == 0);
    }
    {   // reportFault(Rejected): the same without the format, judged by the fast answer delivered
        // a moment ago (with the time it had left); a plain extractor gets reportRefused.
        Rig rig(true, clocked(), false);
        rig.resolve(audio);
        rig.source->reportFault(vid, player::SourceFault::Rejected);
        CHECK(rig.fast.refused.size() == 1 && rig.fast.refused[0].second > 3600 * 1000);
        CHECK(rig.extractor.refused.size() == 1);
        rig.resolve(audio);
        CHECK(rig.fast.resolved.size() == 1 && rig.extractor.resolved.size() == 1);
        // With nothing delivered by the fast path since, a second report is not a second strike.
        rig.source->reportFault(vid, player::SourceFault::Rejected);
        CHECK(rig.fast.refused.size() == 1 && rig.extractor.refused.size() == 2);
        CHECK(number(rig.source->diagnostics(), "faultStrikes") == 1);
    }
    {   // No extractor: the fast path is the only path and is never avoided.
        Rig rig(false, clocked());
        const Answers first = rig.resolve(audio);
        if (first.empty() || first[0].streams.audio.isEmpty()) { CHECK(false); return; }
        rig.source->reportRefused(QString::fromLatin1(vid), first[0].streams.audio[0]);
        CHECK(rig.fast.refused.size() == 1);
        const Answers second = rig.resolve(audio);
        CHECK(second.size() == 1 && second[0].error.ok() && rig.fast.resolved.size() == 2);
        CHECK(number(rig.source->diagnostics(), "fastAvoided") == 0);
    }
}

// SailTune's rule 2: a lost connection, a stall or an abandoned opening is no verdict on the
// source: the cached answers go, nothing is counted, the fast path stays and the extractor is
// told the same fault (never a refusal). This is what the session's stall watchdog reports.
static void res07_connectionLossAndStallAreNotStrikes() {
    clockOffsetMs = 0;
    const StreamRequest audio = request(playback::StreamPurpose::AudioOnly);
    const std::vector<player::SourceFault> faults = {player::SourceFault::Stalled, player::SourceFault::Connection,
                                                     player::SourceFault::Abandoned, player::SourceFault::Stalled};
    for (bool typed : {false, true}) {
        Rig rig(true, clocked(), typed);
        FakeExtractor &extractor = typed ? static_cast<FakeExtractor &>(rig.sink) : rig.extractor;
        for (player::SourceFault fault : faults) {
            const Answers a = rig.resolve(audio);
            CHECK(a.size() == 1 && a[0].error.ok() && a[0].streams.path == playback::ResolvePath::FastPath);
            rig.source->reportFault(vid, fault);
        }
        CHECK(rig.fast.refused.empty());                                   // never a strike
        CHECK(rig.fast.invalidated.size() == faults.size());               // the cached answers went each time
        CHECK(rig.fast.resolved.size() == faults.size() && extractor.resolved.empty());   // the fast path stayed
        const QVariantMap d = rig.source->diagnostics();
        CHECK(number(d, "faultsNotTheSource") == qint64(faults.size()) && number(d, "faultStrikes") == 0
              && number(d, "faultRetries") == 0 && number(d, "faultAvoids") == 0 && number(d, "fastAvoided") == 0
              && number(d, "avoidedVideos") == 0);
        CHECK(extractor.refused.empty());
        if (typed) {
            CHECK(rig.sink.faults.size() == faults.size() && rig.sink.invalidated.empty());
            for (std::size_t i = 0; i < rig.sink.faults.size() && i < faults.size(); ++i)
                CHECK(rig.sink.faults[i].first == vid && rig.sink.faults[i].second == faults[i]);
        } else {
            CHECK(extractor.invalidated.size() == faults.size());
        }
        // They do not count towards the unexplained pair either: the next unexplained failure is a first one.
        rig.resolve(audio);
        rig.source->invalidate(QString::fromLatin1(vid));
        rig.resolve(audio);
        CHECK(rig.fast.resolved.size() == faults.size() + 2 && extractor.resolved.empty());
        CHECK(number(rig.source->diagnostics(), "faultRetries") == 1 && number(rig.source->diagnostics(), "avoidedVideos") == 0);
    }
}

// SailTune's rule 3: a failure nobody explained (invalidate) retries through the fast path once;
// the second one within five minutes sends the video to the extractor's full profile for five
// minutes. Never a strike. Failures of an answer the extractor delivered are not the fast path's.
static void res07_unexplainedFailureRetriesTheFastPathOnceThenTheExtractor() {
    clockOffsetMs = 0;
    const StreamRequest audio = request(playback::StreamPurpose::AudioOnly);
    const QString id = QString::fromLatin1(vid);
    Rig rig(true, clocked(), true);
    // An invalidate before anything was delivered (a retry after a resolve error) counts nothing.
    rig.source->invalidate(id);
    CHECK(number(rig.source->diagnostics(), "faultRetries") == 0 && rig.sink.faults.size() == 1
          && rig.sink.faults[0].second == player::SourceFault::Unspecified);
    rig.resolve(audio);                                              // fast answer 1
    rig.source->invalidate(id);                                      // unexplained, the first time
    CHECK(number(rig.source->diagnostics(), "faultRetries") == 1 && rig.sink.faults.back().second == player::SourceFault::Unspecified);
    const Answers retry = rig.resolve(audio);                        // one more try through the fast path
    CHECK(retry.size() == 1 && retry[0].streams.path == playback::ResolvePath::FastPath);
    CHECK(rig.fast.resolved.size() == 2 && rig.sink.resolved.empty());
    clockOffsetMs = 4 * minuteMs;
    rig.source->invalidate(id);                                      // the second time within five minutes
    CHECK(number(rig.source->diagnostics(), "faultAvoids") == 1 && number(rig.source->diagnostics(), "avoidedVideos") == 1);
    CHECK(rig.sink.faults.back().second == player::SourceFault::Rejected);   // the extractor: full profile first
    const Answers third = rig.resolve(audio);
    CHECK(third.size() == 1 && third[0].error.ok() && rig.fast.resolved.size() == 2 && rig.sink.resolved.size() == 1);
    CHECK(number(rig.source->diagnostics(), "fastAvoided") == 1);
    CHECK(rig.fast.refused.empty() && rig.sink.refused.empty());     // never a strike
    // The extractor delivered last: more failures change nothing for the fast path.
    rig.source->invalidate(id);
    rig.source->invalidate(id);
    CHECK(number(rig.source->diagnostics(), "faultAvoids") == 1 && number(rig.source->diagnostics(), "faultRetries") == 1);
    // Five minutes after the second failure the fast path is back, with a clean count.
    clockOffsetMs = 9 * minuteMs + 2000;
    const Answers back = rig.resolve(audio);
    CHECK(back.size() == 1 && back[0].streams.path == playback::ResolvePath::FastPath && rig.fast.resolved.size() == 3);
    rig.source->invalidate(id);
    CHECK(number(rig.source->diagnostics(), "faultRetries") == 2 && number(rig.source->diagnostics(), "avoidedVideos") == 0);
    // Two unexplained failures more than five minutes apart are two first ones.
    rig.resolve(audio);
    clockOffsetMs = 15 * minuteMs;
    rig.resolve(audio);                                              // a delivery inside the window of the next failure
    rig.source->invalidate(id);
    CHECK(number(rig.source->diagnostics(), "faultRetries") == 3 && number(rig.source->diagnostics(), "faultAvoids") == 1);
    clockOffsetMs = 0;
}

// RES-11 reuse: peekCached answers from the component's cache without any network use, only
// when resolve() would not have to probe; a prefetch of such a request starts nothing and takes
// no slot. A resolver without peekCached never answers a peek.
static void res11_peekCachedAndPrefetchReuse() {
    clockOffsetMs = 0;
    Rig rig;
    rig.fast.cacheAnswers = true;
    const StreamRequest audio = request(playback::StreamPurpose::AudioOnly);
    StreamSet out;
    CHECK(!rig.source->peekCached(audio, &out) && rig.fast.peeks == 1);          // nothing cached
    StreamRequest invalid = audio;
    invalid.videoId = QStringLiteral("short");
    CHECK(!rig.source->peekCached(invalid, &out) && rig.fast.peeks == 1);        // never reaches the component
    rig.resolve(audio);                                                          // the component caches its answer
    const std::size_t asked = rig.fast.resolved.size();
    CHECK(rig.source->peekCached(audio, &out));
    CHECK(out.videoId == QLatin1String(vid) && out.path == playback::ResolvePath::Cache && out.resolveMs == 0
          && out.phases.requestMs == -1);
    CHECK((ids(out.audio) == std::vector<std::string>{"251", "140"}) && out.video.isEmpty());
    CHECK(!out.audio.isEmpty() && out.audio[0].probe == playback::ProbeState::NoVerdict);   // the component's own top
    CHECK(rig.source->peekCached(audio, nullptr));
    CHECK(rig.fast.resolved.size() == asked && rig.fast.probed.empty() && rig.host.requests.empty());   // a read

    // A video request delivers 137, not the component's 248: no peek until 137 was probed.
    const StreamRequest video = request();
    rig.fast.probe = [](const std::string &url) { return std::make_pair(FakeFast::ProbeVerdict::NoVerdict, url); };
    const Answers unverified = rig.resolve(video);
    CHECK(unverified.size() == 1 && unverified[0].error.ok() && rig.fast.probed.size() == 1);
    CHECK(!rig.source->peekCached(video, &out));                                 // resolve() would probe 137 again
    rig.fast.probe = [](const std::string &url) {
        return std::make_pair(FakeFast::ProbeVerdict::Accepted, url + "&node=2");
    };
    rig.resolve(video);
    CHECK(rig.fast.probed.size() == 2);
    const std::size_t probes = rig.fast.probed.size(), resolves = rig.fast.resolved.size();
    CHECK(rig.source->peekCached(video, &out));
    CHECK(!out.video.isEmpty() && out.video[0].formatId == QLatin1String("137")
          && out.video[0].probe == playback::ProbeState::Accepted
          && out.video[0].url.query().contains(QLatin1String("node=2")));       // the address that answered
    CHECK(!out.audio.isEmpty() && out.audio[0].probe == playback::ProbeState::NoVerdict);
    CHECK(rig.fast.probed.size() == probes && rig.fast.resolved.size() == resolves);

    // Prefetch reuse: an answer that is there starts nothing, and two running prefetches keep their slots.
    StreamRequest a = request(), b = request();
    a.videoId = QStringLiteral("CCCCCCCCCCC");
    b.videoId = QStringLiteral("DDDDDDDDDDD");
    rig.source->prefetch(a);
    rig.source->prefetch(b);
    rig.source->prefetch(audio);
    rig.source->prefetch(video);
    CHECK(rig.fast.resolved.size() == resolves + 2 && rig.fast.cancels.empty());
    settle(100);
    QVariantMap d = rig.source->diagnostics();
    CHECK(number(d, "prefetchesCached") == 2 && number(d, "prefetchesReplaced") == 0 && number(d, "prefetchesSkipped") == 0);
    // The prefetched videos are peekable now (their tops were probed ahead), the tap needs no network.
    CHECK(rig.source->peekCached(a, &out) && out.videoId == QLatin1String("CCCCCCCCCCC"));

    // A dropped answer is not peeked.
    rig.source->reportFault(vid, player::SourceFault::Stalled);
    CHECK(!rig.source->peekCached(audio, nullptr) && !rig.source->peekCached(video, nullptr));

    // The optional member: a resolver without peekCached is "not cached"; one with it is asked.
    struct NoPeek { } none;
    sp::domain::StreamSet set;
    CHECK(!player::sharedsource::peekFast(&none, std::string(vid), sp::domain::StreamRequest(), &set, 0));
    sp::domain::StreamRequest component;
    component.audioOnly = false;
    CHECK(player::sharedsource::peekFast(&rig.fast, std::string("CCCCCCCCCCC"), component, &set, 0) && set.videoId == "CCCCCCCCCCC");
}

// The real component through its test seams: SailTune's FastStreamResolver with componentOptions()
// (probe on), the player endpoint and the media host both played by SailTune's FakeProvider.
struct RealRig {
    FakeProvider provider;
    std::unique_ptr<sp::infrastructure::FastStreamResolver> fast;
    std::unique_ptr<sp::infrastructure::MediaProxy> proxy;
    FakeExtractor extractor;
    std::unique_ptr<player::SailTuneStreamSource> source;
    RealRig() {
        CHECK(provider.listen(QHostAddress::LocalHost, 0));
        provider.playerBodies.push_back(fixture("player_ok.json"));
        sp::infrastructure::FastStreamResolver::Options fo = player::componentOptions();
        CHECK(fo.probe);
        const quint16 port = provider.serverPort();
        fo.playerHost = QStringLiteral("127.0.0.1");
        fo.playerPort = port;
        fo.ignoreSslErrorsForTesting = true;
        fo.mediaEndpointOverride = [port](const QString &, QString *h, quint16 *p) { *h = QStringLiteral("127.0.0.1"); *p = port; return true; };
        fast.reset(new sp::infrastructure::FastStreamResolver(fo));
        // 5611210: the proxy fetches the start of a stream at open(); it is routed to the fake
        // provider too, so that the suite never reaches for a real media host.
        sp::infrastructure::MediaProxy::Options po;
        po.ignoreSslErrorsForTesting = true;
        po.endpointOverride = fo.mediaEndpointOverride;
        proxy.reset(new sp::infrastructure::MediaProxy(po));
        source.reset(new player::SailTuneStreamSource(fast.get(), proxy.get(), &extractor));
    }
    Answers resolve(const StreamRequest &r) {
        Answers got;
        source->resolve(r, into(&got));
        waitFor([&]() { return !got.empty(); }, 15000);
        return got;
    }
};

// RES-06 / RES-22 in the component: a 302 to another googlevideo node is followed and its address
// is the one delivered; a 302 to a foreign host is not followed (extractor); a redirect chain
// stops after 3 hops (extractor).
static void res06_componentRedirects() {
    {
        RealRig rig;
        rig.provider.mediaStatuses = {302, 206};
        rig.provider.redirectLocation = "https://rr7---sn-next.googlevideo.com/videoplayback?expire=4102444800&itag=251";
        const Answers got = rig.resolve(request(playback::StreamPurpose::AudioOnly));
        CHECK(got.size() == 1 && got[0].error.ok() && got[0].streams.path == playback::ResolvePath::FastPath);
        if (!got.empty() && !got[0].streams.audio.isEmpty()) {
            const playback::StreamFormat &top = got[0].streams.audio[0];
            CHECK(top.url.host() == QLatin1String("rr7---sn-next.googlevideo.com") && top.url.query().contains(QLatin1String("hop=1")));
            CHECK(top.probe == playback::ProbeState::NoVerdict);
        }
        CHECK(rig.provider.count("/videoplayback") == 2 && rig.extractor.resolved.empty());
    }
    {
        RealRig rig;
        rig.provider.mediaStatuses = {302};
        rig.provider.redirectLocation = "https://evil.example.com/videoplayback";
        const Answers got = rig.resolve(request(playback::StreamPurpose::AudioOnly));
        CHECK(got.size() == 1 && rig.extractor.resolved.size() == 1);
        CHECK(rig.provider.count("/videoplayback") == 1);
    }
    {
        RealRig rig;
        rig.provider.mediaStatuses = {302};
        rig.provider.redirectLocation = "https://rr8---sn-loop.googlevideo.com/videoplayback?itag=251";
        const Answers got = rig.resolve(request(playback::StreamPurpose::AudioOnly));
        CHECK(got.size() == 1 && rig.extractor.resolved.size() == 1);
        CHECK(rig.provider.count("/videoplayback") == 4); // the request and three hops
    }
}

// End to end with a video request: the component probes its tops, the adapter probes the H.264 top
// it delivers instead, all through FakeProvider; open gives a loopback URL.
static void realComponent_endToEnd() {
    RealRig rig;
    rig.source->warmUp();
    CHECK(waitFor([&]() { return rig.fast->hasVisitor(); }));
    const Answers got = rig.resolve(request());
    CHECK(got.size() == 1);
    if (got.empty()) return;
    const StreamSet &s = got[0].streams;
    CHECK(got[0].error.ok() && s.path == playback::ResolvePath::FastPath && s.videoId == QLatin1String(vid));
    CHECK(!s.audio.isEmpty() && !s.video.isEmpty());
    if (s.audio.isEmpty() || s.video.isEmpty()) return;
    CHECK(player::codecFamily(s.video[0].videoCodec) == QLatin1String("h264")); // SEL-02 default: H.264 first
    CHECK(s.video[0].probe != playback::ProbeState::NotProbed);
    for (const playback::StreamFormat &f : s.video) CHECK(player::codecFamily(f.videoCodec) != QLatin1String("av1"));
    CHECK(rig.provider.count("/videoplayback") >= 2 && rig.extractor.resolved.empty());
    OpenedList opened;
    rig.source->fetcher()->open(s.audio[0], openedInto(&opened));
    CHECK(waitFor([&]() { return !opened.empty(); }));
    CHECK(!opened.empty() && opened[0].error.ok() && loopback(opened[0].url));

    // 5611210: the pinned resolver's peekCached. The same answer, from its cache, with no request.
    const int playerRequests = rig.provider.count("/youtubei/v1/player");
    StreamSet peeked;
    CHECK(rig.source->peekCached(request(), &peeked));
    CHECK(peeked.path == playback::ResolvePath::Cache && peeked.videoId == QLatin1String(vid)
          && ids(peeked.video) == ids(s.video) && ids(peeked.audio) == ids(s.audio));
    CHECK(!peeked.video.isEmpty() && peeked.video[0].url == s.video[0].url && peeked.video[0].probe != playback::ProbeState::NotProbed);
    StreamRequest other = request();
    other.maxHeight = 360;
    CHECK(!rig.source->peekCached(other, nullptr));                  // another key: not cached
    rig.source->prefetch(request());                                 // reuse: nothing is asked
    settle(100);
    CHECK(rig.provider.count("/youtubei/v1/player") == playerRequests);
    CHECK(number(rig.source->diagnostics(), "prefetchesCached") == 1);
    // A stall drops the cached answer without a strike; the next resolve asks the fast path again.
    rig.source->reportFault(vid, player::SourceFault::Stalled);
    CHECK(!rig.source->peekCached(request(), nullptr) && rig.fast->allowed());
    const Answers again = rig.resolve(request());
    CHECK(again.size() == 1 && again[0].error.ok() && again[0].streams.path == playback::ResolvePath::FastPath);
    CHECK(rig.provider.count("/youtubei/v1/player") == playerRequests + 1 && rig.extractor.resolved.empty());
}

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    res03_fastPathMappedAndProbed();
    res06_deliveredRungRedirectedAndProxied();
    res06_deliveredRungRefusedFallsBack();
    res08_fallbackAndFinal();
    res09_finalReasonsNamed();
    res19_hlsUnproxied();
    res12_refresherReResolvesSameFormat();
    contract_cancelPrefetchRefused();
    res07_refusedFastAddressIsAStrikeAndTheVideoGoesToTheExtractor();
    res07_connectionLossAndStallAreNotStrikes();
    res07_unexplainedFailureRetriesTheFastPathOnceThenTheExtractor();
    res11_peekCachedAndPrefetchReuse();
    res06_componentRedirects();
    realComponent_endToEnd();
    std::printf("shared stream source tests: %d checks, %d failure%s\n", checks, failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
