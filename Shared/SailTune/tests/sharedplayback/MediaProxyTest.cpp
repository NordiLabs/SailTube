// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// The loopback media proxy against a local TLS server that plays the media host: Range semantics
// towards the player, bounded upstream chunks, flow control, resume after a refusal, expiry or a
// dropped connection, identity checks. Qt Core and Network only.
#include <cstdio>
static int failures = 0, checks = 0;
#define CHECK(cond) do { ++checks; if (!(cond)) { ++failures; std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)
#include "../../Core/SharedPlayback/Infrastructure/MediaProxy.h"
#include "FakeProvider.h"
#include "ProxyClient.h"

using namespace nordtile::music::sharedplayback;
using infrastructure::MediaProxy;

struct Rig {
    FakeProvider server;
    std::int64_t clock = 1700000000000LL;
    std::vector<std::string> logs;
    std::unique_ptr<MediaProxy> proxy;
    QByteArray media;
    int refreshCalls = 0;
    int refreshDelayMs = 0;
    std::function<void(const MediaProxy::Source &)> onRefresh;   // lets a test see the old source

    explicit Rig(int mediaSize = 40000, qint64 chunk = 4096, std::function<void(MediaProxy::Options &)> tweak = nullptr) {
        CHECK(server.listen(QHostAddress::LocalHost, 0));
        media = makeMedia(mediaSize);
        server.mediaData = media;
        MediaProxy::Options options;
        options.ipv4Only = true;
        options.chunkBytes = chunk;
        options.prebufferBytes = 0;          // the older cases count upstream requests; the pre-buffer and the ladder have cases of their own
        options.firstChunkBytes = 0;
        options.tailBytes = 0;               // the tail has a case of its own
        options.ignoreSslErrorsForTesting = true;
        options.allowAnyHostForTesting = false;
        options.earlyRetryIntervalMs = 20;
        options.earlyRetryWindowMs = 200;
        options.upstreamTimeoutMs = 2000;
        options.clock = [this]() { return clock; };
        options.log = [this](const std::string &line) { logs.push_back(line); };
        options.endpointOverride = [this](const QString &, QString *host, quint16 *port) {
            *host = QStringLiteral("127.0.0.1"); *port = server.serverPort(); return true;
        };
        if (tweak) tweak(options);
        proxy.reset(new MediaProxy(options));
    }
    // ageMs: how long ago the address was obtained; a young one is retried in place when refused.
    MediaProxy::Source source(int expiresInMs = 3600 * 1000, int ageMs = 60000) const {
        MediaProxy::Source s;
        s.url = "https://rr1---sn-test.googlevideo.com/videoplayback?expire=4102444800&itag=251";
        s.userAgent = "Test-UA/1.0";
        s.contentLength = media.size();
        s.mimeType = "audio/webm";
        s.expiresAtUnixMs = clock + expiresInMs;
        s.issuedAtUnixMs = clock - ageMs;
        return s;
    }
    MediaProxy::Refresher refresher(bool ok = true, std::function<void(MediaProxy::Source *)> change = nullptr) {
        return [this, ok, change](const std::function<void(const MediaProxy::FreshSource &)> &deliver) {
            ++refreshCalls;
            MediaProxy::FreshSource fresh; fresh.ok = ok; fresh.source = source();
            fresh.source.url += "&refresh=" + std::to_string(refreshCalls);          // never the address that was just refused
            fresh.source.issuedAtUnixMs = clock - 60000;       // not a "fresh" address: no in-place retries for the next refusal
            if (change) change(&fresh.source);
            QTimer::singleShot(refreshDelayMs, [deliver, fresh]() { deliver(fresh); });
        };
    }
    QByteArray pathOf(const QUrl &url) const { return url.path().toLatin1(); }
    bool logged(const char *needle) const {
        for (std::size_t i = 0; i < logs.size(); ++i) if (logs[i].find(needle) != std::string::npos) return true;
        return false;
    }
    int mediaRequests() const { return server.count("/videoplayback"); }
    // The Range headers of the upstream requests, in order.
    QList<QByteArray> upstreamRanges() const {
        QList<QByteArray> ranges;
        for (std::size_t i = 0; i < server.requests.size(); ++i)
            if (server.requests[i].path.startsWith("/videoplayback")) ranges << server.requests[i].headers.value("range");
        return ranges;
    }
};

static void testServing() {
    Rig rig;
    const QUrl url = rig.proxy->open("AAAAAAAAAAA", rig.source(), rig.refresher());
    CHECK(url.isValid() && url.host() == "127.0.0.1" && url.path().startsWith("/s/") && url.port() == rig.proxy->port());
    // Whole file, no Range: 200 with the exact bytes, fetched in bounded chunks.
    Reply all = fetch(url.port(), rig.pathOf(url));
    CHECK(all.ok && all.status == 200 && all.body == rig.media);
    CHECK(all.headers.value("content-length") == QByteArray::number(rig.media.size()));
    CHECK(all.headers.value("accept-ranges") == "bytes" && all.headers.value("connection") == "close");
    CHECK(all.headers.value("content-type") == "application/octet-stream");
    QList<QByteArray> ranges = rig.upstreamRanges();
    CHECK(ranges.size() == 10);                                                      // 40000 bytes in chunks of 4096
    CHECK(!ranges.isEmpty() && ranges.first() == "bytes=0-4095" && ranges.last() == "bytes=36864-39999");
    // Every upstream request carries the address' user agent and the fixed header set; never cookies.
    for (std::size_t i = 0; i < rig.server.requests.size(); ++i) {
        const Recorded &r = rig.server.requests[i];
        if (!r.path.startsWith("/videoplayback")) continue;
        CHECK(r.headers.value("user-agent") == "Test-UA/1.0" && r.headers.value("sec-fetch-mode") == "navigate");
        CHECK(r.headers.value("accept-encoding") == "identity" && !r.headers.contains("cookie") && !r.headers.contains("authorization"));
    }
    // Ranges the way a player asks.
    Reply tail = fetch(url.port(), rig.pathOf(url), "bytes=30000-");
    CHECK(tail.status == 206 && tail.body == rig.media.mid(30000));
    CHECK(tail.headers.value("content-range") == "bytes 30000-39999/40000");
    Reply middle = fetch(url.port(), rig.pathOf(url), "bytes=5000-9999");
    CHECK(middle.status == 206 && middle.body == rig.media.mid(5000, 5000) && middle.headers.value("content-range") == "bytes 5000-9999/40000");
    Reply suffix = fetch(url.port(), rig.pathOf(url), "bytes=-1000");
    CHECK(suffix.status == 206 && suffix.body == rig.media.right(1000));
    Reply overshoot = fetch(url.port(), rig.pathOf(url), "bytes=39000-99999999");
    CHECK(overshoot.status == 206 && overshoot.body == rig.media.mid(39000));
    Reply past = fetch(url.port(), rig.pathOf(url), "bytes=40000-");
    CHECK(past.status == 416 && past.headers.value("content-range") == "bytes */40000" && past.body.isEmpty());
    // A Range header that is malformed, in another unit or asks for several ranges is ignored (RFC 7233): the whole body.
    Reply multi = fetch(url.port(), rig.pathOf(url), "bytes=0-1,5-6");
    CHECK(multi.status == 200 && multi.body == rig.media && !multi.headers.contains("content-range"));
    Reply pages = fetch(url.port(), rig.pathOf(url), "pages=1-2");
    CHECK(pages.status == 200 && pages.body == rig.media);
    CHECK(fetch(url.port(), rig.pathOf(url), "bytes=9-2").status == 200);
    Reply head = fetch(url.port(), rig.pathOf(url), "", "HEAD");
    CHECK(head.status == 200 && head.body.isEmpty() && head.headers.value("content-length") == "40000");
    const int before = rig.mediaRequests();
    CHECK(fetch(url.port(), rig.pathOf(url), "", "HEAD").status == 200);
    CHECK(rig.mediaRequests() == before);                                            // a HEAD never reaches the network
    // Everything else is refused without touching the network.
    CHECK(fetch(url.port(), "/s/unknowntoken").status == 404);
    CHECK(fetch(url.port(), "/other").status == 404);
    CHECK(fetch(url.port(), rig.pathOf(url), "", "POST").status == 405);
    CHECK(rig.mediaRequests() == before);
    CHECK(rig.proxy->activeJobs() == 0);
    for (std::size_t i = 0; i < rig.logs.size(); ++i)                                // no addresses, tokens or headers in the log
        CHECK(rig.logs[i].find("googlevideo") == std::string::npos && rig.logs[i].find("Test-UA") == std::string::npos &&
              rig.logs[i].find(url.path().mid(3).toStdString()) == std::string::npos);
}

static void testOpenValidation() {
    Rig rig;
    MediaProxy::Source bad = rig.source();
    bad.url = "http://rr1.googlevideo.com/videoplayback?x=1";
    CHECK(!rig.proxy->open("AAAAAAAAAAA", bad, rig.refresher()).isValid());
    bad.url = "https://evil.example.com/videoplayback?x=1";
    CHECK(!rig.proxy->open("AAAAAAAAAAA", bad, rig.refresher()).isValid());
    bad = rig.source(); bad.contentLength = 0;
    CHECK(!rig.proxy->open("AAAAAAAAAAA", bad, rig.refresher()).isValid());
    CHECK(!rig.proxy->open("bad id", rig.source(), rig.refresher()).isValid());
    CHECK(MediaProxy::allowedAddress("https://rr1---sn-x.googlevideo.com/videoplayback?a=1"));
    CHECK(!MediaProxy::allowedAddress("https://googlevideo.com.evil.com/videoplayback") && !MediaProxy::allowedAddress("https://user@rr1.googlevideo.com/x"));
    CHECK(!MediaProxy::allowedAddress("https://rr1.googlevideo.com:8443/x") && !MediaProxy::allowedAddress("https://rr1.googlevideo.com/x#f"));
    // Tokens are unguessable and distinct per open.
    const QUrl a = rig.proxy->open("AAAAAAAAAAA", rig.source(), rig.refresher());
    const QUrl b = rig.proxy->open("AAAAAAAAAAA", rig.source(), rig.refresher());
    CHECK(a != b && a.path().size() >= 3 + 64);
}

static void testSeekMidStream() {
    Rig rig;
    const QUrl url = rig.proxy->open("AAAAAAAAAAA", rig.source(), rig.refresher());
    // A player that reads a little, drops the connection and asks again further on (a seek).
    Reply partial = fetch(url.port(), rig.pathOf(url), "", "GET", 6000);
    CHECK(partial.ok && partial.body.size() >= 6000 && rig.media.startsWith(partial.body.left(6000)));
    CHECK(waitFor([&]() { return rig.proxy->activeJobs() == 0; }));
    waitFor([]() { return false; }, 100);                                          // let the dropped upstream request be recorded
    rig.server.requests.clear();
    Reply seek = fetch(url.port(), rig.pathOf(url), "bytes=20000-");
    CHECK(seek.status == 206 && seek.body == rig.media.mid(20000));
    CHECK(!rig.upstreamRanges().isEmpty() && rig.upstreamRanges().first() == "bytes=20000-24095");   // the new request starts there
    CHECK(waitFor([&]() { return rig.proxy->activeJobs() == 0; }));
}

static void testRefusalResumesAtTheSameByte() {
    Rig rig;
    const QUrl url = rig.proxy->open("AAAAAAAAAAA", rig.source(), rig.refresher());
    // Chunks 0..2 are served, the 4th is refused (an address that stopped being served), the new one works.
    rig.server.mediaActions = {0, 0, 0, 403, 0};
    Reply all = fetch(url.port(), rig.pathOf(url));
    CHECK(all.ok && all.status == 200 && all.body == rig.media);                     // byte-identical, nothing lost or repeated
    CHECK(rig.refreshCalls == 1 && rig.proxy->counters().refreshes == 1);
    CHECK(rig.logged("refresh why refused at 12288"));
    // The refused chunk was asked for again at the same offset.
    const QList<QByteArray> ranges = rig.upstreamRanges();
    int at = 0; for (int i = 0; i < ranges.size(); ++i) if (ranges[i] == "bytes=12288-16383") ++at;
    CHECK(at == 2);
}

static void testEarlyRefusalIsRetriedInPlace() {
    Rig rig;
    MediaProxy::Source fresh = rig.source(3600 * 1000, 0);    // just issued
    const QUrl url = rig.proxy->open("AAAAAAAAAAA", fresh, rig.refresher());
    rig.server.mediaActions = {403, 403, 0};                    // refused for a moment, then served
    Reply all = fetch(url.port(), rig.pathOf(url));
    CHECK(all.status == 200 && all.body == rig.media);
    CHECK(rig.refreshCalls == 0 && rig.proxy->counters().earlyRetries == 2);        // no re-resolve was needed
}

static void testRefusalPersistsThenRefreshesAndGivesUp() {
    {   // Refused for longer than the window: one refresh, then it works.
        Rig rig;
        const QUrl url = rig.proxy->open("AAAAAAAAAAA", rig.source(3600 * 1000, 0), rig.refresher());
        rig.server.mediaActions = {403, 403, 403, 403, 403, 403, 403, 403, 403, 403, 403, 0};   // ten in-place retries (200 ms / 20 ms), then the re-resolve
        Reply all = fetch(url.port(), rig.pathOf(url));
        CHECK(all.status == 200 && all.body == rig.media);
        CHECK(rig.refreshCalls == 1);
    }
    {   // A refresher that fails ends the stream; the player sees a short body and a closed connection.
        Rig rig;
        const QUrl url = rig.proxy->open("AAAAAAAAAAA", rig.source(), rig.refresher(false));
        rig.server.mediaActions = {0, 403};
        Reply all = fetch(url.port(), rig.pathOf(url));
        CHECK(all.ok && all.body.size() < rig.media.size() && rig.media.startsWith(all.body));
        CHECK(rig.refreshCalls == 1);
    }
    {   // No more than eight re-resolves a minute.
        Rig rig(200000, 4096);
        const QUrl url = rig.proxy->open("AAAAAAAAAAA", rig.source(), rig.refresher());
        rig.server.mediaActions = {403};                                            // refused forever
        Reply all = fetch(url.port(), rig.pathOf(url));
        CHECK(all.ok && all.body.isEmpty());
        CHECK(rig.refreshCalls == 8);
    }
}

static void testDroppedConnections() {
    {   // The connection dies in the middle of a chunk: the part that arrived is kept, the rest is asked for.
        Rig rig;
        const QUrl url = rig.proxy->open("AAAAAAAAAAA", rig.source(), rig.refresher());
        rig.server.mediaActions = {0, -1500, 0};
        Reply all = fetch(url.port(), rig.pathOf(url));
        CHECK(all.status == 200 && all.body == rig.media);                          // nothing lost, nothing repeated
        CHECK(rig.refreshCalls == 0);                                               // one death is retried in place
        CHECK(rig.mediaRequests() >= 11 && rig.proxy->counters().resumed >= 1);
    }
    {   // Repeated deaths without any progress end the stream.
        Rig rig;
        const QUrl url = rig.proxy->open("AAAAAAAAAAA", rig.source(), rig.refresher());
        rig.server.mediaActions = {0, 1};                                           // then every connection is dropped at once
        Reply all = fetch(url.port(), rig.pathOf(url));
        CHECK(all.ok && all.body.size() == 4096 && rig.media.startsWith(all.body));
        CHECK(rig.refreshCalls == 2);                                               // the second death in a row asks for a new address, the fourth ends it
    }
}

static void testIdentityAndExpiry() {
    {   // A refresh that returns another length or type never splices two files.
        Rig rig;
        const QUrl url = rig.proxy->open("AAAAAAAAAAA", rig.source(), rig.refresher(true, [](MediaProxy::Source *s) { s->contentLength += 1; }));
        rig.server.mediaActions = {0, 403};
        Reply all = fetch(url.port(), rig.pathOf(url));
        CHECK(all.ok && all.body.size() == 4096);
        CHECK(rig.proxy->counters().identityChanged == 1);
        Rig other;
        const QUrl url2 = other.proxy->open("AAAAAAAAAAA", other.source(), other.refresher(true, [](MediaProxy::Source *s) { s->mimeType = "audio/mp4"; }));
        other.server.mediaActions = {0, 403};
        CHECK(fetch(url2.port(), other.pathOf(url2)).body.size() == 4096 && other.proxy->counters().identityChanged == 1);
    }
    {   // An address about to expire is replaced before the next chunk is requested.
        Rig rig;
        const QUrl url = rig.proxy->open("AAAAAAAAAAA", rig.source(20 * 1000), rig.refresher());   // expires in 20 s: inside the 30 s margin
        Reply all = fetch(url.port(), rig.pathOf(url));
        CHECK(all.status == 200 && all.body == rig.media);
        CHECK(rig.refreshCalls >= 1 && rig.logged("refresh why expiry"));
    }
    {   // The refreshed address replaces the old one for later requests too.
        Rig rig;
        const QUrl url = rig.proxy->open("AAAAAAAAAAA", rig.source(), rig.refresher(true, [](MediaProxy::Source *s) {
            s->url = "https://rr2---sn-other.googlevideo.com/videoplayback?expire=4102444800&itag=251"; }));
        rig.server.mediaActions = {0, 403, 0};
        CHECK(fetch(url.port(), rig.pathOf(url)).body == rig.media);
        const int refreshes = rig.refreshCalls;
        CHECK(fetch(url.port(), rig.pathOf(url), "bytes=100-").body == rig.media.mid(100) && rig.refreshCalls == refreshes);
    }
}

static void testLimitsAndClose() {
    {   // At most maxJobs streams at once; the extra client is told to come back later. Sockets that have asked for
        // nothing do not count: a local process cannot crowd the player out by opening connections.
        Rig rig(8 * 1024 * 1024, 1024 * 1024, [](MediaProxy::Options &o) { o.maxJobs = 1; });
        const QUrl url = rig.proxy->open("AAAAAAAAAAA", rig.source(), rig.refresher());
        std::vector<std::unique_ptr<QTcpSocket> > idle;
        for (int i = 0; i < 2; ++i) {
            idle.emplace_back(new QTcpSocket);
            idle.back()->connectToHost(QHostAddress::LocalHost, url.port());
            CHECK(idle.back()->waitForConnected(2000));
        }
        waitFor([&]() { return rig.proxy->activeJobs() == 2; }, 1000);
        CHECK(fetch(url.port(), rig.pathOf(url), "bytes=0-9").status == 206);       // served although two sockets are open
        QTcpSocket held;
        held.setSocketOption(QAbstractSocket::ReceiveBufferSizeSocketOption, 4096);
        held.setReadBufferSize(1);                                                  // takes nothing: the stream stays in progress
        held.connectToHost(QHostAddress::LocalHost, url.port());
        CHECK(held.waitForConnected(2000));
        held.write("GET " + rig.pathOf(url) + " HTTP/1.1\r\n\r\n");               // a stream that is being served and not read
        held.flush();
        waitFor([&]() { return rig.proxy->counters().served >= 2; }, 2000);
        const Reply busy = fetch(url.port(), rig.pathOf(url));
        CHECK(busy.status == 503);
        held.abort();
        CHECK(waitFor([&]() { return fetch(url.port(), rig.pathOf(url), "bytes=0-9").status == 206; }));
    }
    {   // Idle sockets are dropped after a few seconds; more than four times maxJobs of them are refused outright.
        Rig rig(400000, 4096, [](MediaProxy::Options &o) { o.maxJobs = 1; });
        const QUrl url = rig.proxy->open("AAAAAAAAAAA", rig.source(), rig.refresher());
        std::vector<std::unique_ptr<QTcpSocket> > idle;
        for (int i = 0; i < 6; ++i) { idle.emplace_back(new QTcpSocket); idle.back()->connectToHost(QHostAddress::LocalHost, url.port()); }
        waitFor([&]() { return rig.proxy->activeJobs() >= 4; }, 1000);
        CHECK(rig.proxy->activeJobs() <= 4);
        CHECK(waitFor([&]() { return rig.proxy->activeJobs() == 0; }, 8000));
    }
    {   // closeVideo ends what is running and forgets the tokens.
        Rig rig(400000, 4096);
        const QUrl url = rig.proxy->open("AAAAAAAAAAA", rig.source(), rig.refresher());
        const QUrl other = rig.proxy->open("BBBBBBBBBBB", rig.source(), rig.refresher());
        QTcpSocket client; client.connectToHost(QHostAddress::LocalHost, url.port());
        CHECK(client.waitForConnected(2000));
        client.write("GET " + rig.pathOf(url) + " HTTP/1.1\r\n\r\n");
        waitFor([&]() { return rig.proxy->activeJobs() == 1; }, 2000);
        rig.proxy->closeVideo("AAAAAAAAAAA");
        CHECK(waitFor([&]() { return rig.proxy->activeJobs() == 0; }));
        CHECK(fetch(url.port(), rig.pathOf(url)).status == 404);
        CHECK(fetch(other.port(), rig.pathOf(other), "bytes=0-99").status == 206);
    }
}

static void testTheTailOfATrackIsNotCutOff() {
    // The player reads at playback speed; after the last upstream byte the queued tail must still be delivered in full.
    Rig rig(700000, 65536, [](MediaProxy::Options &o) { o.clientBufferHighBytes = 1024 * 1024; o.clientBufferLowBytes = 256 * 1024; });
    const QUrl url = rig.proxy->open("AAAAAAAAAAA", rig.source(), rig.refresher());
    QTcpSocket slow;
    slow.setSocketOption(QAbstractSocket::ReceiveBufferSizeSocketOption, 4096);
    slow.connectToHost(QHostAddress::LocalHost, url.port());
    CHECK(slow.waitForConnected(2000));
    slow.write("GET " + rig.pathOf(url) + " HTTP/1.1\r\n\r\n");
    QByteArray got;
    QElapsedTimer total; total.start();
    while (total.elapsed() < 25000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        got += slow.read(8192);                                    // 80 KB/s: the 700 kB take about nine seconds
        waitFor([]() { return false; }, 100);
        if (slow.state() == QAbstractSocket::UnconnectedState && slow.bytesAvailable() == 0) break;
    }
    got += slow.readAll();
    const int end = got.indexOf("\r\n\r\n");
    CHECK(end > 0 && got.mid(end + 4) == rig.media);
}

static void testClockAheadOfTheAddressLifetimeDoesNotLoop() {
    // The address looks expired against this device's clock, and so does every replacement: one refresh, then play on.
    Rig rig;
    const QUrl url = rig.proxy->open("AAAAAAAAAAA", rig.source(20 * 1000), rig.refresher(true, [&rig](MediaProxy::Source *s) { s->expiresAtUnixMs = rig.clock + 10 * 1000; }));
    Reply all = fetch(url.port(), rig.pathOf(url));
    CHECK(all.status == 200 && all.body == rig.media);
    CHECK(rig.refreshCalls == 1);
}

static void testStreamEvictionSparesTheOneBeingRead() {
    Rig rig(400000, 4096);
    const QUrl playing = rig.proxy->open("AAAAAAAAAAA", rig.source(), rig.refresher());
    QTcpSocket held; held.connectToHost(QHostAddress::LocalHost, playing.port());
    CHECK(held.waitForConnected(2000));
    held.write("GET " + rig.pathOf(playing) + " HTTP/1.1\r\n\r\n");                 // being read (and slowly)
    waitFor([&]() { return rig.proxy->counters().served >= 1; }, 2000);
    std::vector<QUrl> others;
    for (int i = 0; i < 17; ++i) {
        rig.clock += 1000;
        others.push_back(rig.proxy->open(std::string("BBBBBBBBBB") + char('A' + i), rig.source(), rig.refresher()));
        CHECK(others.back().isValid());
    }
    CHECK(fetch(others.back().port(), rig.pathOf(others.back()), "bytes=0-9").status == 206);   // the newest is served
    CHECK(fetch(playing.port(), rig.pathOf(playing), "bytes=0-9").status == 206);               // the one being read was not evicted
    int gone = 0;
    for (std::size_t i = 0; i < others.size(); ++i) if (fetch(others[i].port(), rig.pathOf(others[i]), "bytes=0-9").status == 404) ++gone;
    CHECK(gone >= 1);                                                                             // and the least recently used idle one was
    held.abort();
}

static void testRefreshAddressRules() {
    {   // The address that was just refused is no remedy: the stream ends instead of looping on it.
        Rig rig;
        const QUrl url = rig.proxy->open("AAAAAAAAAAA", rig.source(), [&rig](const std::function<void(const MediaProxy::FreshSource &)> &deliver) {
            ++rig.refreshCalls; MediaProxy::FreshSource same; same.ok = true; same.source = rig.source(); deliver(same);
        });
        rig.server.mediaActions = {0, 403};
        Reply all = fetch(url.port(), rig.pathOf(url));
        CHECK(all.ok && all.body.size() == 4096 && rig.refreshCalls == 1);
        CHECK(rig.logged("refresh result same_address"));
    }
    {   // An answer that arrives after its request is gone still replaces the address for the next request.
        Rig rig;
        rig.refreshDelayMs = 300;
        const QUrl url = rig.proxy->open("AAAAAAAAAAA", rig.source(), rig.refresher());
        rig.server.mediaActions = {0, 403, 0};
        QTcpSocket leaving;
        leaving.connectToHost(QHostAddress::LocalHost, url.port());
        CHECK(leaving.waitForConnected(2000));
        leaving.write("GET " + rig.pathOf(url) + " HTTP/1.1\r\n\r\n");
        CHECK(waitFor([&]() { return rig.refreshCalls == 1; }));                       // the second chunk was refused: the refresh is pending
        leaving.abort();                                                              // the player leaves while it is
        CHECK(waitFor([&]() { return rig.proxy->activeJobs() == 0; }));
        waitFor([]() { return false; }, 600);                                         // the answer arrives to nobody
        rig.server.requests.clear();
        Reply next = fetch(url.port(), rig.pathOf(url), "bytes=0-99");
        CHECK(next.status == 206 && next.body == rig.media.left(100));
        CHECK(!rig.server.requests.empty() && rig.server.requests[0].path.contains("refresh=1"));   // on the new address
    }
    {   // A refresher that never answers does not hold the connection for ever.
        Rig rig(40000, 4096, [](MediaProxy::Options &o) { o.refreshTimeoutMs = 300; });
        const QUrl url = rig.proxy->open("AAAAAAAAAAA", rig.source(), [&rig](const std::function<void(const MediaProxy::FreshSource &)> &) { ++rig.refreshCalls; });
        rig.server.mediaActions = {0, 403};
        Reply all = fetch(url.port(), rig.pathOf(url));
        CHECK(all.ok && all.body.size() == 4096 && rig.refreshCalls == 1);
        CHECK(rig.logged("refresh result timeout"));
    }
}

static void testContentRangeIsHeldToWhatWasAsked() {
    // An answer with another start, a longer range or another total is not spliced into the stream.
    Rig rig;
    const QUrl url = rig.proxy->open("AAAAAAAAAAA", rig.source(), rig.refresher());
    rig.server.mediaTotalOverride = rig.media.size() + 5;                 // another length than the one this stream was opened for
    Reply all = fetch(url.port(), rig.pathOf(url));
    CHECK(all.ok && all.body.isEmpty());
}

// A ranged request may be answered with a redirect to another node of the CDN: followed (a few hops, CDN hosts only), the new
// address is kept for the rest of the stream, nothing is lost or repeated.
static void testRedirectsWithinTheCdnAreFollowed() {
    const QByteArray other = "https://rr2---sn-other.googlevideo.com/videoplayback?expire=4102444800&itag=251&redirect_counter=1";
    {
        Rig rig;
        rig.server.redirectLocation = other;
        const QUrl url = rig.proxy->open("AAAAAAAAAAA", rig.source(), rig.refresher());
        rig.server.mediaActions = {0, 302, 0};
        Reply all = fetch(url.port(), rig.pathOf(url));
        CHECK(all.status == 200 && all.body == rig.media);
        CHECK(rig.proxy->counters().redirects == 1 && rig.refreshCalls == 0);
        bool onNewAddress = false;
        for (std::size_t i = 0; i < rig.server.requests.size(); ++i) if (rig.server.requests[i].path.contains("redirect_counter=1")) onNewAddress = true;
        CHECK(onNewAddress);                                                          // the rest of the stream went to the new node
        rig.server.requests.clear();
        CHECK(fetch(url.port(), rig.pathOf(url), "bytes=100-199").body == rig.media.mid(100, 100));
        CHECK(!rig.server.requests.empty() && rig.server.requests[0].path.contains("redirect_counter=1"));   // later requests too
    }
    {   // Not followed (not the CDN, or too many hops): the address is replaced through the host instead, and nothing is spliced.
        Rig rig;
        rig.server.redirectLocation = "https://evil.example.com/videoplayback?x=1";
        const QUrl url = rig.proxy->open("AAAAAAAAAAA", rig.source(), rig.refresher());
        rig.server.mediaActions = {0, 302, 0};
        Reply all = fetch(url.port(), rig.pathOf(url));
        CHECK(all.status == 200 && all.body == rig.media && rig.proxy->counters().redirects == 0 && rig.refreshCalls == 1);
        Rig loop;
        loop.server.redirectLocation = other;
        const QUrl url2 = loop.proxy->open("AAAAAAAAAAA", loop.source(), loop.refresher());
        loop.server.mediaActions = {0, 302, 302, 302, 302, 0};                         // four in a row
        Reply all2 = loop.pathOf(url2).isEmpty() ? Reply() : fetch(url2.port(), loop.pathOf(url2));
        CHECK(all2.status == 200 && all2.body == loop.media && loop.proxy->counters().redirects == 3 && loop.refreshCalls == 1);
    }
}

// The first upstream request is small and the next ones grow to the largest: the bytes that matter first come in one short
// exchange. A seek starts small again; a first chunk as large as the largest means no ladder.
static void testChunkLadder() {
    {
        Rig rig(40000, 16000, [](MediaProxy::Options &o) { o.firstChunkBytes = 1000; o.chunkGrowth = 4; });
        const QUrl url = rig.proxy->open("AAAAAAAAAAA", rig.source(), rig.refresher());
        Reply all = fetch(url.port(), rig.pathOf(url));
        CHECK(all.status == 200 && all.body == rig.media);
        const QList<QByteArray> ranges = rig.upstreamRanges();
        CHECK(ranges.size() == 5);
        CHECK(ranges.value(0) == "bytes=0-999" && ranges.value(1) == "bytes=1000-4999" && ranges.value(2) == "bytes=5000-20999");   // 1000, 4000, 16000
        CHECK(ranges.value(3) == "bytes=21000-36999" && ranges.value(4) == "bytes=37000-39999");                                      // capped at the largest
        rig.server.requests.clear();
        Reply seek = fetch(url.port(), rig.pathOf(url), "bytes=20000-");
        CHECK(seek.status == 206 && seek.body == rig.media.mid(20000));
        CHECK(!rig.upstreamRanges().isEmpty() && rig.upstreamRanges().first() == "bytes=20000-20999");                                // a seek starts small
    }
    {   // No ladder: the first chunk is as large as the others (or the feature is off).
        Rig rig(40000, 16000, [](MediaProxy::Options &o) { o.firstChunkBytes = 16000; });
        const QUrl url = rig.proxy->open("AAAAAAAAAAA", rig.source(), rig.refresher());
        CHECK(fetch(url.port(), rig.pathOf(url)).body == rig.media);
        CHECK(rig.upstreamRanges().first() == "bytes=0-15999" && rig.upstreamRanges().size() == 3);
    }
    {   // A chunk that dies halfway is resumed in place with the same size step, nothing lost.
        Rig rig(40000, 16000, [](MediaProxy::Options &o) { o.firstChunkBytes = 1000; o.chunkGrowth = 4; });
        const QUrl url = rig.proxy->open("AAAAAAAAAAA", rig.source(), rig.refresher());
        rig.server.mediaActions = {0, -2000, 0};
        CHECK(fetch(url.port(), rig.pathOf(url)).body == rig.media);
    }
}

// At open() the proxy already asks for the start of the stream; the audio backend's first request is answered from memory and
// its next chunk goes over the connection the pre-buffer warmed.
static void testPrebuffer() {
    const auto settled = [](Rig &rig) { waitFor([&]() { return rig.mediaRequests() >= 1; }); waitFor([]() { return false; }, 300); };
    {   // The backend comes late: everything below the pre-buffer's end comes from memory, then the network takes over.
        Rig rig(40000, 4096, [](MediaProxy::Options &o) { o.prebufferBytes = 8000; });
        const QUrl url = rig.proxy->open("AAAAAAAAAAA", rig.source(), rig.refresher());
        settled(rig);
        CHECK(rig.mediaRequests() == 1 && rig.upstreamRanges().first() == "bytes=0-7999");               // the pre-buffer's own request
        Reply all = fetch(url.port(), rig.pathOf(url));
        CHECK(all.status == 200 && all.body == rig.media);
        CHECK(rig.proxy->counters().prebufferHits == 1 && rig.proxy->counters().prebufferFailures == 0);
        CHECK(rig.upstreamRanges().value(1) == "bytes=8000-12095");                                       // the network continues where the pre-buffer ended
        CHECK(rig.server.connectionsAccepted == 1);                                                        // ... on the connection the pre-buffer warmed
        rig.server.requests.clear();
        Reply inside = fetch(url.port(), rig.pathOf(url), "bytes=100-199");
        CHECK(inside.status == 206 && inside.body == rig.media.mid(100, 100) && rig.mediaRequests() == 0);   // wholly inside: no request at all
        Reply seek = fetch(url.port(), rig.pathOf(url), "bytes=20000-");
        CHECK(seek.status == 206 && seek.body == rig.media.mid(20000) && rig.upstreamRanges().first() == "bytes=20000-24095");   // a seek ignores it
        CHECK(rig.proxy->counters().prebufferHits == 2);
    }
    {   // The backend is early: it is served as the pre-buffer fills, nothing is lost or repeated.
        Rig rig(40000, 4096, [](MediaProxy::Options &o) { o.prebufferBytes = 30000; });
        const QUrl url = rig.proxy->open("AAAAAAAAAAA", rig.source(), rig.refresher());
        Reply all = fetch(url.port(), rig.pathOf(url));
        CHECK(all.status == 200 && all.body == rig.media);
        CHECK(rig.upstreamRanges().first() == "bytes=0-29999" && rig.upstreamRanges().value(1) == "bytes=30000-34095");
    }
    {   // A stream shorter than the pre-buffer: all of it is kept, and the request is answered whole from memory.
        Rig rig(5000, 4096, [](MediaProxy::Options &o) { o.prebufferBytes = 8000; });
        const QUrl url = rig.proxy->open("AAAAAAAAAAA", rig.source(), rig.refresher());
        settled(rig);
        Reply all = fetch(url.port(), rig.pathOf(url));
        CHECK(all.status == 200 && all.body == rig.media && rig.mediaRequests() == 1);
    }
    {   // Refused: the pre-buffer fails quietly and the normal path serves the stream.
        Rig rig(40000, 4096, [](MediaProxy::Options &o) { o.prebufferBytes = 8000; });
        rig.server.mediaActions = {403, 0};
        const QUrl url = rig.proxy->open("AAAAAAAAAAA", rig.source(3600 * 1000, 0), rig.refresher());
        settled(rig);
        CHECK(rig.proxy->counters().prebufferFailures == 1);
        Reply all = fetch(url.port(), rig.pathOf(url));
        CHECK(all.status == 200 && all.body == rig.media);
    }
    {   // Redirected to another node of the CDN: followed.
        Rig rig(40000, 4096, [](MediaProxy::Options &o) { o.prebufferBytes = 8000; });
        rig.server.redirectLocation = "https://rr2---sn-other.googlevideo.com/videoplayback?expire=4102444800&itag=251&redirect_counter=1";
        rig.server.mediaActions = {302, 0};
        const QUrl url = rig.proxy->open("AAAAAAAAAAA", rig.source(), rig.refresher());
        settled(rig);
        CHECK(rig.proxy->counters().redirects == 1 && rig.proxy->counters().prebufferFailures == 0);
        Reply all = fetch(url.port(), rig.pathOf(url));
        CHECK(all.status == 200 && all.body == rig.media);
    }
    {   // The stream is closed while the pre-buffer is in flight: nothing crashes, and the next stream works.
        Rig rig(40000, 4096, [](MediaProxy::Options &o) { o.prebufferBytes = 30000; });
        rig.proxy->open("AAAAAAAAAAA", rig.source(), rig.refresher());
        rig.proxy->closeVideo("AAAAAAAAAAA");
        waitFor([]() { return false; }, 300);
        const QUrl url = rig.proxy->open("BBBBBBBBBBB", rig.source(), rig.refresher());
        CHECK(fetch(url.port(), rig.pathOf(url)).body == rig.media);
    }
    {   // Off: no request before the backend asks.
        Rig rig;
        const QUrl url = rig.proxy->open("AAAAAAAAAAA", rig.source(), rig.refresher());
        waitFor([]() { return false; }, 250);
        CHECK(rig.mediaRequests() == 0);
        CHECK(fetch(url.port(), rig.pathOf(url), "bytes=0-9").status == 206);
    }
}

// The next track was prepared ahead: its stream is handed out again for the real playback, with the pre-buffer it already
// holds and the connection it warmed - nothing is requested twice.
static void testStreamReuse() {
    const auto settled = [](Rig &rig) { waitFor([&]() { return rig.mediaRequests() >= 1; }); waitFor([]() { return false; }, 300); };
    Rig rig(40000, 4096, [](MediaProxy::Options &o) { o.prebufferBytes = 8000; });
    const MediaProxy::Source src = rig.source();
    const QUrl first = rig.proxy->open("AAAAAAAAAAA", src, rig.refresher());
    settled(rig);
    CHECK(rig.mediaRequests() == 1);
    const std::int64_t length = src.contentLength;
    CHECK(rig.proxy->reuse("AAAAAAAAAAA", length, "audio/webm", 600000, 120000) == first);
    CHECK(rig.proxy->counters().reused == 1);
    // Another video, another length or another type is another stream.
    CHECK(!rig.proxy->reuse("BBBBBBBBBBB", length, "audio/webm", 600000, 120000).isValid());
    CHECK(!rig.proxy->reuse("AAAAAAAAAAA", length + 1, "audio/webm", 600000, 120000).isValid());
    CHECK(!rig.proxy->reuse("AAAAAAAAAAA", length, "audio/mp4", 600000, 120000).isValid());
    CHECK(!rig.proxy->reuse("AAAAAAAAAAA", 0, "audio/webm", 600000, 120000).isValid());
    CHECK(rig.proxy->counters().reused == 1);
    // Playing it: the start comes from memory, and the rest continues on the warmed connection.
    rig.server.requests.clear();
    Reply all = fetch(first.port(), rig.pathOf(first));
    CHECK(all.status == 200 && all.body == rig.media);
    CHECK(rig.proxy->counters().prebufferHits == 1 && rig.server.connectionsAccepted == 1);
    CHECK(rig.upstreamRanges().first() == "bytes=8000-12095");                  // the start was not requested again
    // A replay of the same stream still has its start in memory.
    rig.server.requests.clear();
    CHECK(rig.proxy->reuse("AAAAAAAAAAA", length, "audio/webm", 600000, 120000) == first);
    Reply again = fetch(first.port(), rig.pathOf(first), "bytes=0-99");
    CHECK(again.status == 206 && again.body == rig.media.left(100) && rig.mediaRequests() == 0);
    // Age and the address's remaining life are held to: an old stream, or one whose address is about to expire, is not handed out.
    rig.clock += 700000;
    CHECK(!rig.proxy->reuse("AAAAAAAAAAA", length, "audio/webm", 600000, 120000).isValid());
    CHECK(rig.proxy->reuse("AAAAAAAAAAA", length, "audio/webm", 3600000, 120000).isValid());      // young enough for a longer limit
    CHECK(!rig.proxy->reuse("AAAAAAAAAAA", length, "audio/webm", 3600000, 3600 * 1000).isValid()); // expires within the asked-for time
    // A stream that was closed (refused downstream) is gone.
    rig.proxy->closeVideo("AAAAAAAAAAA");
    CHECK(!rig.proxy->reuse("AAAAAAAAAAA", length, "audio/webm", 3600000, 0).isValid());
    {   // A pre-buffer that failed is not worth handing out again.
        Rig bad(40000, 4096, [](MediaProxy::Options &o) { o.prebufferBytes = 8000; });
        bad.server.mediaActions = {403, 0};
        bad.proxy->open("AAAAAAAAAAA", bad.source(3600 * 1000, 0), bad.refresher());
        settled(bad);
        CHECK(bad.proxy->counters().prebufferFailures == 1);
        CHECK(!bad.proxy->reuse("AAAAAAAAAAA", bad.media.size(), "audio/webm", 600000, 120000).isValid());
    }
}

// A demuxer looks at the tail of a stream first (tag footers, an index): the proxy has fetched it at open() and answers from memory.
static void testTailAnsweredFromMemory() {
    const int total = 200000;
    Rig rig(total, 4096, [](MediaProxy::Options &o) { o.tailBytes = 8192; o.prebufferBytes = 0; });
    const QUrl url = rig.proxy->open("AAAAAAAAAAA", rig.source(), rig.refresher());
    waitFor([&]() { return rig.mediaRequests() >= 1; });
    waitFor([]() { return false; }, 300);
    CHECK(rig.mediaRequests() == 1 && rig.upstreamRanges().first() == "bytes=191808-199999");      // one request, for the last 8192 bytes
    // The last 32 bytes, then the last 128: both from memory, nothing more is asked of the provider.
    Reply last32 = fetch(url.port(), rig.pathOf(url), "bytes=199968-");
    CHECK(last32.status == 206 && last32.body == rig.media.right(32) && last32.headers.value("content-range") == "bytes 199968-199999/200000");
    Reply last128 = fetch(url.port(), rig.pathOf(url), "bytes=199872-199999");
    CHECK(last128.status == 206 && last128.body == rig.media.right(128));
    Reply whole = fetch(url.port(), rig.pathOf(url), "bytes=191808-");
    CHECK(whole.body == rig.media.mid(191808));
    CHECK(rig.mediaRequests() == 1 && rig.proxy->counters().tailHits == 3);
    // A range that starts before the tail is a network request as usual, and the bytes are right.
    Reply across = fetch(url.port(), rig.pathOf(url), "bytes=190000-");
    CHECK(across.status == 206 && across.body == rig.media.mid(190000) && rig.mediaRequests() > 1);
    // A HEAD never reaches the network, and whole-file reads are unchanged.
    Reply all = fetch(url.port(), rig.pathOf(url));
    CHECK(all.status == 200 && all.body == rig.media);
    {   // Small streams have no tail; with the switch off there is none either.
        Rig small(20000, 4096, [](MediaProxy::Options &o) { o.tailBytes = 8192; });
        const QUrl u = small.proxy->open("BBBBBBBBBBB", small.source(), small.refresher());
        waitFor([]() { return false; }, 200);
        CHECK(small.mediaRequests() == 0);
        Reply r = fetch(u.port(), small.pathOf(u));
        CHECK(r.body == small.media);
        Rig off(total, 4096);
        const QUrl u2 = off.proxy->open("CCCCCCCCCCC", off.source(), off.refresher());
        waitFor([]() { return false; }, 200);
        CHECK(off.mediaRequests() == 0);
    }
    {   // A request for the tail that arrives before the tail has (a slow origin): it waits for it, not for a second request.
        Rig slow(total, 4096, [](MediaProxy::Options &o) { o.tailBytes = 8192; o.prebufferBytes = 0; });
        slow.server.mediaDelayMs = 300;
        const QUrl u = slow.proxy->open("DDDDDDDDDDD", slow.source(), slow.refresher());
        Reply early = fetch(u.port(), slow.pathOf(u), "bytes=199968-");
        CHECK(early.status == 206 && early.body == slow.media.right(32) && slow.mediaRequests() == 1);
    }
    {   // The origin refuses the tail: the stream works without it.
        Rig refused(total, 4096, [](MediaProxy::Options &o) { o.tailBytes = 8192; o.prebufferBytes = 0; });
        refused.server.mediaActions = {403, 0};
        const QUrl u = refused.proxy->open("EEEEEEEEEEE", refused.source(3600 * 1000, 0), refused.refresher());
        waitFor([&]() { return refused.proxy->counters().tailFailures == 1; });
        Reply r = fetch(u.port(), refused.pathOf(u), "bytes=199968-");
        CHECK(r.status == 206 && r.body == refused.media.right(32));
    }
}

// Jobs that end cleanly leave their warm connection for the next one: a probe after a probe, a seek after a short read, costs no new handshake.
static void testUpstreamConnectionsAreReused() {
    Rig rig(200000, 4096, [](MediaProxy::Options &o) { o.prebufferBytes = 0; });
    const QUrl url = rig.proxy->open("AAAAAAAAAAA", rig.source(), rig.refresher());
    for (int i = 0; i < 4; ++i) {
        Reply r = fetch(url.port(), rig.pathOf(url), "bytes=" + QByteArray::number(i * 30000) + "-" + QByteArray::number(i * 30000 + 99));
        CHECK(r.status == 206 && r.body == rig.media.mid(i * 30000, 100));
    }
    CHECK(rig.server.connectionsAccepted == 1);                                    // one TLS connection for the four requests
    CHECK(rig.proxy->counters().upstreamReused == 3);
    // A job that is cut off in the middle of its answer leaves nothing behind that could be reused.
    Rig mid(500000, 4096, [](MediaProxy::Options &o) { o.prebufferBytes = 0; o.chunkBytes = 400000; });
    const QUrl u = mid.proxy->open("BBBBBBBBBBB", mid.source(), mid.refresher());
    mid.server.mediaDelayMs = 400;                                                 // the answer is still on its way when the client leaves
    {
        QTcpSocket c; c.connectToHost(QHostAddress::LocalHost, u.port()); c.waitForConnected(2000);
        c.write("GET " + mid.pathOf(u) + " HTTP/1.1\r\nHost: x\r\nRange: bytes=0-\r\n\r\n"); c.waitForBytesWritten(1000);
        waitFor([&]() { return mid.mediaRequests() >= 1; });
        c.abort();
    }
    waitFor([&]() { return mid.proxy->activeJobs() == 0; });
    mid.server.mediaDelayMs = 0;
    Reply after = fetch(u.port(), mid.pathOf(u), "bytes=100000-100099");
    CHECK(after.status == 206 && after.body == mid.media.mid(100000, 100));
    CHECK(mid.server.connectionsAccepted >= 2);                                    // the interrupted one was not reused
    // A dead pooled connection is replaced without the player noticing.
    Rig dead(200000, 4096, [](MediaProxy::Options &o) { o.prebufferBytes = 0; });
    const QUrl d = dead.proxy->open("CCCCCCCCCCC", dead.source(), dead.refresher());
    CHECK(fetch(d.port(), dead.pathOf(d), "bytes=0-99").status == 206);
    dead.server.closeAfterEveryAnswer = true;                                      // from now on the origin closes after each answer
    CHECK(fetch(d.port(), dead.pathOf(d), "bytes=1000-1099").body == dead.media.mid(1000, 100));
    CHECK(fetch(d.port(), dead.pathOf(d), "bytes=2000-2099").body == dead.media.mid(2000, 100));
}

static void testFlowControl() {
    // A slow reader: the proxy stops reading from the network while the client's queue is above the high mark.
    Rig rig(8 * 1024 * 1024, 1024 * 1024, [](MediaProxy::Options &o) {
        o.clientBufferHighBytes = 64 * 1024; o.clientBufferLowBytes = 16 * 1024; });
    const QUrl url = rig.proxy->open("AAAAAAAAAAA", rig.source(), rig.refresher());
    QTcpSocket slow;
    slow.setReadBufferSize(32 * 1024);                           // small receive buffer: the proxy's send queue backs up
    slow.connectToHost(QHostAddress::LocalHost, url.port());
    CHECK(slow.waitForConnected(2000));
    slow.write("GET " + rig.pathOf(url) + " HTTP/1.1\r\n\r\n");
    slow.flush();
    QElapsedTimer timer; timer.start();
    while (timer.elapsed() < 600) { QCoreApplication::processEvents(QEventLoop::AllEvents, 20); }   // do not read for a while
    CHECK(rig.proxy->activeJobs() == 1 && rig.proxy->counters().pauses >= 1);   // the network was held back while the client did not read
    QByteArray got;
    CHECK(waitFor([&]() { got += slow.readAll(); return got.size() >= int(rig.media.size()) + 100 || slow.state() == QAbstractSocket::UnconnectedState; }, 15000));
    got += slow.readAll();
    const int end = got.indexOf("\r\n\r\n");
    CHECK(end > 0 && got.mid(end + 4) == rig.media);                // everything arrived, in order, once
}

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    testServing();
    testOpenValidation();
    testSeekMidStream();
    testRefusalResumesAtTheSameByte();
    testEarlyRefusalIsRetriedInPlace();
    testRefusalPersistsThenRefreshesAndGivesUp();
    testDroppedConnections();
    testIdentityAndExpiry();
    testLimitsAndClose();
    testFlowControl();
    testChunkLadder();
    testPrebuffer();
    testStreamReuse();
    testTailAnsweredFromMemory();
    testUpstreamConnectionsAreReused();
    testRedirectsWithinTheCdnAreFollowed();
    testTheTailOfATrackIsNotCutOff();
    testClockAheadOfTheAddressLifetimeDoesNotLoop();
    testStreamEvictionSparesTheOneBeingRead();
    testRefreshAddressRules();
    testContentRangeIsHeldToWhatWasAsked();
    if (failures) { std::fprintf(stderr, "%d check(s) failed\n", failures); return 1; }
    std::printf("sharedplayback MediaProxyTest: %d checks passed\n", checks);
    return 0;
}
