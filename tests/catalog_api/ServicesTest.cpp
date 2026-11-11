// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Catalog API coverage (NFR-TST-06), service level: transports, client identity, the work lane
// and the discovery services over FakeTransport and the recorded fixtures. Offline.
#include "tests/catalog_common/TestSupport.h"

#include "Core/Catalog/Domain/SearchFilter.h"
#include "Core/Catalog/Domain/Subscriptions.h"
#include "Core/Catalog/Domain/TextParse.h"
#include "Core/Catalog/Infrastructure/ChannelService.h"
#include "Core/Catalog/Infrastructure/ClientIdentity.h"
#include "Core/Catalog/Infrastructure/CommentsService.h"
#include "Core/Catalog/Infrastructure/FeedCacheStore.h"
#include "Core/Catalog/Infrastructure/FeedService.h"
#include "Core/Catalog/Infrastructure/HttpTransport.h"
#include "Core/Catalog/Infrastructure/InnerTubeClient.h"
#include "Core/Catalog/Infrastructure/Json.h"
#include "Core/Catalog/Infrastructure/SearchService.h"
#include "Core/Catalog/Infrastructure/ShortDetailService.h"
#include "Core/Catalog/Infrastructure/ShortsStore.h"
#include "Core/Catalog/Infrastructure/SponsorBlockService.h"
#include "Core/Catalog/Infrastructure/SuggestService.h"
#include "Core/Catalog/Infrastructure/WatchNextService.h"
#include "Core/Catalog/Infrastructure/WorkLane.h"

#include <QFile>
#include <QStringList>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <atomic>

using namespace nordtile::video;
using namespace nordtile::video::catalog;
using catalog_test::FakeTransport;
using catalog_test::fixture;
using catalog_test::fixtureJson;
using catalog_test::waitFor;

namespace {

const char *const kNasa = "UCLA_DiR1FfKNvjuUpBHmylQ";
const QString kVideo = QStringLiteral("9zCH37330f8");
const std::int64_t kFeedNow = 1794124800;   // 2026-11-08T08:00:00Z, just after the recording

QStringList &capturedLog() { static QStringList l; return l; }
void captureLog(QtMsgType, const QMessageLogContext &, const QString &message) { capturedLog().append(message); }

QJsonObject sentBody(const FakeTransport &net) {
    return net.requests.isEmpty() ? QJsonObject() : QJsonDocument::fromJson(net.requests.last().body).object();
}

bool containsId(const std::vector<Item> &rows, const std::string &id) {
    for (const Item &it : rows) if (it.id == id) return true;
    return false;
}

// The InnerTube stack every service test builds on.
struct Stack {
    FakeTransport net;
    ClientIdentity identity;
    WorkLane lane;
    InnerTubeClient client;
    explicit Stack(const QString &name, int threads = 1)
        : identity(nullptr, QString()), lane(name, threads), client(&net, &identity, &lane, DiscoveryLocale()) {}
};

}  // namespace

// ---------------------------------------------------------------- transport and InnerTube client

static void transportAndClient() {
    const QByteArray ua = webUserAgent();
    CHECK(ua.startsWith("Mozilla/5.0 ("));
    CHECK(ua.contains("Chrome/"));
    CHECK(!ua.contains("SailTube"));                // a plain desktop browser UA (DSC-02)
    const HttpRequest suggest = SuggestService::buildRequest(QStringLiteral("nasa"), QStringLiteral("en"));
    QByteArray sentUa;
    for (const auto &h : suggest.headers) if (h.first == "User-Agent") sentUa = h.second;
    CHECK_EQ(sentUa, ua);

    capturedLog().clear();
    QtMessageHandler previous = qInstallMessageHandler(captureLog);
    InnerTubeClient::logStage("search", 120, 15, 18, 51234, 200);
    qInstallMessageHandler(previous);
    CHECK_EQ(capturedLog().size(), 1);
    CHECK_EQ(capturedLog().value(0), QStringLiteral("sailtube-timing innertube search net=120 parse=15 ready=18 bytes=51234 status=200"));

    Stack s(QStringLiteral("api-client"));
    CHECK_EQ(s.client.locale().hl, QStringLiteral("en"));
    DiscoveryLocale de;
    de.hl = QStringLiteral("de");
    de.gl = QStringLiteral("AT");
    s.client.setLocale(de);
    CHECK_EQ(s.client.locale().hl, QStringLiteral("de"));
    CHECK_EQ(s.client.locale().gl, QStringLiteral("AT"));
    const QJsonObject body = QJsonDocument::fromJson(s.client.buildRequest(QStringLiteral("browse"), QJsonObject(), 5000).body).object();
    CHECK_EQ(json::at(body, QStringLiteral("context.client.hl")).toString(), QStringLiteral("de"));
    CHECK_EQ(json::at(body, QStringLiteral("context.client.gl")).toString(), QStringLiteral("AT"));

    // Every delivered post logs one timing line for its endpoint.
    s.net.on(QStringLiteral("youtubei/v1/browse"), QByteArray(), 200, "{\"contents\":{}}");
    bool done = false;
    capturedLog().clear();
    previous = qInstallMessageHandler(captureLog);
    s.client.post<int>(QStringLiteral("browse"), QJsonObject(), &s.net,
                       [](const InnerTubeReply &r) { return r.httpStatus; }, [&done](int) { done = true; });
    CHECK(waitFor([&done] { return done; }));
    qInstallMessageHandler(previous);
    bool timing = false;
    for (const QString &line : capturedLog())
        if (line.startsWith(QLatin1String("sailtube-timing innertube browse net=")) && line.endsWith(QLatin1String("status=200"))) timing = true;
    CHECK(timing);
}

// ---------------------------------------------------------------- client identity (DSC-02, I18N-01)

static void identity() {
    const DiscoveryLocale sys = DiscoveryLocale::fromSystem();
    const DiscoveryLocale expected = DiscoveryLocale::fromLocale(QLocale::system());
    CHECK_EQ(sys.hl, expected.hl);
    CHECK_EQ(sys.gl, expected.gl);
    CHECK(!sys.hl.isEmpty());
    CHECK_EQ(sys.gl.size(), 2);

    qint64 now = 1000LL * 3600 * 1000;
    FakeTransport net;
    net.on(QStringLiteral("https://www.youtube.com/"), QByteArray(), 200, "x\"INNERTUBE_CLIENT_VERSION\":\"2.20261201.01.00\"y");
    {
        ClientIdentity id(&net, QString(), nullptr, [&now] { return now; });
        CHECK_EQ(id.scrapeCount(), 0);
        id.refreshIfStale();                        // never fetched: background scrape
        CHECK_EQ(id.scrapeCount(), 1);
        CHECK_EQ(id.clientVersion(), QString::fromLatin1(ClientIdentity::builtInVersion()));   // never blocks
        CHECK(waitFor([&id] { return !id.scrapeInFlight(); }));
        CHECK_EQ(id.clientVersion(), QStringLiteral("2.20261201.01.00"));
        CHECK_EQ(id.source(), QStringLiteral("scrape"));
        id.refreshIfStale();                        // fresh: nothing sent
        now += 11LL * 3600 * 1000;
        id.refreshIfStale();
        CHECK_EQ(id.scrapeCount(), 1);
        CHECK_EQ(net.count(QStringLiteral("https://www.youtube.com/")), 1);
        now += 2LL * 3600 * 1000;                   // 13 h: stale
        id.refreshIfStale();
        CHECK_EQ(id.scrapeCount(), 2);
        CHECK(waitFor([&id] { return !id.scrapeInFlight(); }));
        CHECK_EQ(net.count(QStringLiteral("https://www.youtube.com/")), 2);
    }
    {
        FakeTransport down;
        down.on(QStringLiteral("https://www.youtube.com/"), QByteArray(), 503, "");
        ClientIdentity id(&down, QString(), nullptr, [&now] { return now; });
        id.refreshIfStale();
        CHECK(waitFor([&id] { return !id.scrapeInFlight(); }));
        CHECK_EQ(id.source(), QStringLiteral("built-in"));   // a failed scrape commits nothing
        now += 5LL * 60 * 1000;
        id.refreshIfStale();                        // still stale, but rate limited (10 min)
        CHECK_EQ(id.scrapeCount(), 1);
        now += 6LL * 60 * 1000;
        id.refreshIfStale();
        CHECK_EQ(id.scrapeCount(), 2);
        CHECK(waitFor([&id] { return !id.scrapeInFlight(); }));
    }
    {
        ClientIdentity id(&net, QString(), nullptr, [&now] { return now; });
        QStringList changes;
        QObject::connect(&id, &ClientIdentity::versionChanged, [&changes](const QString &v) { changes.append(v); });
        id.setExtractorVersion(QStringLiteral("not-a-version"));   // implausible: ignored
        CHECK_EQ(id.source(), QStringLiteral("built-in"));
        CHECK(changes.isEmpty());
        id.setExtractorVersion(QStringLiteral("2.20270115.01.00"));
        CHECK_EQ(id.source(), QStringLiteral("extractor"));
        CHECK_EQ(id.clientVersion(), QStringLiteral("2.20270115.01.00"));
        CHECK(changes == QStringList() << QStringLiteral("2.20270115.01.00"));
        id.refreshIfStale();                        // the extractor's value is never re-scraped
        now += 48LL * 3600 * 1000;
        id.refreshIfStale();
        CHECK_EQ(id.scrapeCount(), 0);
        CHECK_EQ(id.clientVersion(), QStringLiteral("2.20270115.01.00"));
    }
}

// ---------------------------------------------------------------- work lane (RES-02)

static void workLane() {
    WorkLane lane(QStringLiteral("api-lane"), 2);
    CHECK_EQ(lane.name(), QStringLiteral("api-lane"));
    CHECK_EQ(lane.activeThreads(), 0);
    QObject context;
    std::atomic<int> started(0), finished(0);
    std::atomic<bool> release(false);
    int delivered = 0;
    for (int i = 0; i < 2; ++i) {
        lane.run<int>(&context, [&started, &finished, &release] {
            ++started;
            QElapsedTimer t;
            t.start();
            while (!release.load() && t.elapsed() < 5000) QThread::msleep(2);
            ++finished;
            return 7;
        }, [&delivered](int v) { delivered += v; });
    }
    QElapsedTimer t;
    t.start();
    while (started.load() < 2 && t.elapsed() < 5000) QThread::msleep(2);
    CHECK_EQ(started.load(), 2);
    CHECK_EQ(lane.activeThreads(), 2);              // both jobs run in parallel on the lane
    release = true;
    lane.drain();                                   // blocks until the queued work finished
    CHECK_EQ(finished.load(), 2);
    CHECK_EQ(lane.activeThreads(), 0);
    CHECK_EQ(delivered, 0);                         // results arrive on the context's thread...
    CHECK(waitFor([&delivered] { return delivered == 14; }));   // ...once its event loop runs
    lane.drain();                                   // idle lane: returns at once
    CHECK_EQ(lane.activeThreads(), 0);
}

// ---------------------------------------------------------------- channels (DSC-12)

static void channelService() {
    CHECK_EQ(ChannelService::normalizeTab(QStringLiteral("Live")), QStringLiteral("streams"));
    CHECK_EQ(ChannelService::normalizeTab(QString()), QStringLiteral("videos"));
    CHECK_EQ(ChannelService::normalizeTab(QStringLiteral("uploads")), QStringLiteral("videos"));
    CHECK_EQ(ChannelService::normalizeTab(QStringLiteral("home")), QStringLiteral("featured"));
    CHECK_EQ(ChannelService::normalizeTab(QStringLiteral(" Shorts ")), QStringLiteral("shorts"));
    CHECK_EQ(ChannelService::normalizeTab(QStringLiteral("playlists")), QStringLiteral("playlists"));

    InnerTubeReply ok;
    ok.outcome = Outcome::Ok;
    ok.httpStatus = 200;
    ok.json = fixtureJson(QStringLiteral("resolve_handle.json"));
    QString id;
    CHECK(ChannelService::parseResolve(ok, &id) == Outcome::Ok);
    CHECK_EQ(id, QString::fromLatin1(kNasa));
    CHECK(ChannelService::parseResolve(ok, nullptr) == Outcome::Ok);
    InnerTubeReply notFound;
    notFound.outcome = Outcome::Http4xx;
    notFound.httpStatus = 404;
    CHECK(ChannelService::parseResolve(notFound, &id) == Outcome::Unavailable);
    InnerTubeReply network;
    CHECK(ChannelService::parseResolve(network, &id) == Outcome::Network);
    InnerTubeReply drift = ok;
    drift.json = QJsonObject();
    CHECK(ChannelService::parseResolve(drift, &id) == Outcome::ShapeDrift);
    InnerTubeReply video = ok;
    video.json = QJsonDocument::fromJson("{\"endpoint\":{\"watchEndpoint\":{\"videoId\":\"9zCH37330f8\"}}}").object();
    QString untouched = QStringLiteral("unchanged");
    CHECK(ChannelService::parseResolve(video, &untouched) == Outcome::Unavailable);   // not a channel
    CHECK_EQ(untouched, QStringLiteral("unchanged"));

    Stack s(QStringLiteral("api-channel"));
    ChannelService service(&s.client);
    CHECK(!service.hasRss());
    service.setRssFetch([](const QString &, QObject *, ChannelService::RssDone done) { done(Outcome::ValidEmpty, std::vector<Item>()); });
    CHECK(service.hasRss());

    CHECK(service.knownTabs(QString::fromLatin1(kNasa)).isEmpty());
    s.net.on(QStringLiteral("youtubei/v1/browse"), QByteArray(), 200, fixture(QStringLiteral("channel_videos.json")));
    ChannelPage page;
    bool done = false;
    service.browseTab(QString::fromLatin1(kNasa), QStringLiteral("videos"), &s.net, [&](ChannelPage p) { page = p; done = true; });
    CHECK(waitFor([&done] { return done; }));
    CHECK(page.outcome == Outcome::Ok);
    const QList<ChannelTab> tabs = service.knownTabs(QString::fromLatin1(kNasa));
    CHECK_EQ(tabs.size(), page.tabs.size());
    QStringList keys;
    for (const ChannelTab &t : tabs) {
        keys.append(t.key);
        if (!t.params.isEmpty()) CHECK_EQ(service.tabParams(QString::fromLatin1(kNasa), t.key), t.params);
        if (t.key == QLatin1String("videos")) CHECK(t.selected);
    }
    CHECK(keys.contains(QStringLiteral("videos")) && keys.contains(QStringLiteral("shorts")));
    CHECK(keys.contains(QStringLiteral("streams")) && keys.contains(QStringLiteral("playlists")));
    CHECK(service.knownTabs(QStringLiteral("UCsXVk37bltHxD1rDPwtNM8Q")).isEmpty());   // another channel
}

// ---------------------------------------------------------------- comments (DSC-16)

static void commentsService() {
    Stack s(QStringLiteral("api-comments"));
    CatalogCounters counters;
    CommentsService service(&s.client, &counters);
    s.net.on(QStringLiteral("youtubei/v1/next"), QByteArray(), 200, fixture(QStringLiteral("next_en.json")));

    const CommentsEntry expectedEntry = CommentsService::parseEntry(fixtureJson(QStringLiteral("next_en.json")), "en");
    CommentsEntry entry;
    bool got = false;
    CHECK(service.fetchEntry(kVideo, &s.net, [&](const CommentsEntry &e) { entry = e; got = true; }) > 0);
    CHECK(waitFor([&got] { return got; }));
    CHECK(entry.outcome == Outcome::Ok);
    CHECK(!entry.token.empty());
    CHECK_EQ(entry.token, expectedEntry.token);
    CHECK_EQ(entry.total, expectedEntry.total);
    CHECK_EQ(entry.sorts.size(), expectedEntry.sorts.size());
    QJsonObject sent = sentBody(s.net);
    CHECK_EQ(sent.value(QStringLiteral("videoId")).toString(), kVideo);
    CHECK(sent.value(QStringLiteral("contentCheckOk")).toBool() && sent.value(QStringLiteral("racyCheckOk")).toBool());
    CHECK(s.net.requests.last().url.path().endsWith(QLatin1String("/youtubei/v1/next")));

    s.net.rules.clear();
    s.net.on(QStringLiteral("youtubei/v1/next"), QByteArray(), 200, fixture(QStringLiteral("comments_p1.json")));
    const CommentsPage expectedThreads = CommentsService::parsePage(fixtureJson(QStringLiteral("comments_p1.json")), "en", false);
    CommentsPage threads;
    got = false;
    service.fetchThreads(QString::fromStdString(entry.token), &s.net, [&](const CommentsPage &p) { threads = p; got = true; });
    CHECK(waitFor([&got] { return got; }));
    CHECK(threads.outcome == Outcome::Ok);
    CHECK(threads.comments.size() >= 15);
    CHECK_EQ(threads.comments.size(), expectedThreads.comments.size());
    CHECK_EQ(threads.continuation, expectedThreads.continuation);
    if (!threads.comments.empty() && !expectedThreads.comments.empty())
        CHECK_EQ(threads.comments.front().id, expectedThreads.comments.front().id);
    CHECK_EQ(sentBody(s.net).value(QStringLiteral("continuation")).toString(), QString::fromStdString(entry.token));
    std::string replyToken;
    for (const Comment &c : threads.comments) if (!c.replyContinuation.empty()) { replyToken = c.replyContinuation; break; }
    CHECK(!replyToken.empty());

    s.net.rules.clear();
    s.net.on(QStringLiteral("youtubei/v1/next"), QByteArray(), 200, fixture(QStringLiteral("comments_replies_p1.json")));
    const CommentsPage expectedReplies = CommentsService::parsePage(fixtureJson(QStringLiteral("comments_replies_p1.json")), "en", true);
    CommentsPage replies;
    got = false;
    service.fetchReplies(QString::fromStdString(replyToken), &s.net, [&](const CommentsPage &p) { replies = p; got = true; });
    CHECK(waitFor([&got] { return got; }));
    CHECK(replies.outcome == Outcome::Ok);
    CHECK(!replies.comments.empty());
    CHECK_EQ(replies.comments.size(), expectedReplies.comments.size());
    bool allReplies = true;
    for (const Comment &c : replies.comments) if (!c.isReply) allReplies = false;
    CHECK(allReplies);
    CHECK_EQ(sentBody(s.net).value(QStringLiteral("continuation")).toString(), QString::fromStdString(replyToken));
    CHECK_EQ(counters.value(Area::Comments, Outcome::Ok), std::int64_t(3));

    // HTTP failures stay what they are (never drift) and are counted.
    s.net.rules.clear();
    s.net.on(QStringLiteral("youtubei/v1/next"), QByteArray(), 503, "");
    CommentsPage failed;
    got = false;
    service.fetchThreads(QStringLiteral("token"), &s.net, [&](const CommentsPage &p) { failed = p; got = true; });
    CHECK(waitFor([&got] { return got; }));
    CHECK(failed.outcome == Outcome::Http5xx);
    CHECK_EQ(counters.value(Area::Comments, Outcome::Http5xx), std::int64_t(1));
    got = false;
    service.fetchReplies(QStringLiteral("token"), &s.net, [&](const CommentsPage &p) { failed = p; got = true; });
    CHECK(waitFor([&got] { return got; }));
    CHECK(failed.outcome == Outcome::Http5xx);
    got = false;
    service.fetchEntry(kVideo, &s.net, [&](const CommentsEntry &e) { entry = e; got = true; });
    CHECK(waitFor([&got] { return got; }));
    CHECK(entry.outcome == Outcome::Http5xx);
}

// ---------------------------------------------------------------- feed (DSC-05/06/07)

static bool feedIdle(FeedService &s) {
    return waitFor([&s] { return s.cacheLoaded() && !s.refreshing() && !s.extending(); }, 8000);
}

static void feedService() {
    Stack s(QStringLiteral("api-feed"), 2);
    s.net.on(QStringLiteral("feeds/videos.xml"), QByteArray(), 200, fixture(QStringLiteral("rss_channel.xml")));
    s.net.on(QStringLiteral("youtubei/v1/browse"), QByteArray(), 200, fixture(QStringLiteral("channel_videos.json")));
    QTemporaryDir dir;
    const QString path = dir.path() + QStringLiteral("/feed.json");
    FeedCacheStore store(path);
    std::int64_t now = kFeedNow;
    FeedService service(&s.net, &s.client, &store, nullptr, [&now] { return now; });
    service.setLiveEnrichment(false);

    // The in-process auto-refresh timer.
    QTimer *timer = service.findChild<QTimer *>();
    CHECK(timer != nullptr);
    if (timer) {
        CHECK(!timer->isActive());
        service.setAutoRefresh(true);
        CHECK(timer->isActive());
        CHECK_EQ(timer->interval(), FeedService::kAutoRefreshCheckMs);
        service.setAutoRefresh(false);
        CHECK(!timer->isActive());
    }

    ChannelList list;
    list.push_back(ChannelRef(kNasa, "NASA"));
    service.setChannels(list);
    service.start();
    CHECK(feedIdle(service));
    const ChannelFeedState *state = service.schedule().find(kNasa);
    CHECK(state != nullptr);
    if (!state) return;
    CHECK(state->source == FeedSource::Rss);
    const std::size_t rssRows = state->rows.size();
    CHECK_EQ(rssRows, std::size_t(15));
    CHECK_EQ(s.net.count(QStringLiteral("youtubei/v1/browse")), 0);

    // Depth: one Videos-tab page per extendable channel.
    CHECK(service.canExtend());
    service.extend();
    CHECK(service.extending());
    CHECK(feedIdle(service));
    CHECK(!service.extending());
    state = service.schedule().find(kNasa);
    CHECK(state && state->pagedFromVideosTab);
    CHECK(state && state->rows.size() > rssRows);
    CHECK_EQ(s.net.count(QStringLiteral("youtubei/v1/browse")), 1);
    const QJsonObject sent = sentBody(s.net);
    CHECK_EQ(sent.value(QStringLiteral("browseId")).toString(), QString::fromLatin1(kNasa));
    CHECK(!sent.value(QStringLiteral("params")).toString().isEmpty());

    // A Shorts lookup re-applies the filter offline.
    RowFilterSettings filter;
    filter.hideShorts = true;
    service.setRowFilter(filter);
    std::string target;
    for (const Item &it : service.merged().rows)
        if (it.kind == ItemKind::Video && !isConfirmedShort(it.shortEvidence)) { target = it.id; break; }
    CHECK(!target.empty());
    const int hiddenBefore = service.merged().hiddenShorts;
    const std::size_t shownBefore = service.merged().rows.size();
    const int requestsBefore = s.net.requests.size();
    service.setShortsLookup([target](const std::string &id) { return id == target ? ShortEvidence::Renderer : ShortEvidence::None; });
    CHECK_EQ(service.merged().hiddenShorts, hiddenBefore + 1);
    CHECK_EQ(service.merged().rows.size(), shownBefore - 1);
    CHECK(!containsId(service.merged().rows, target));
    CHECK_EQ(s.net.requests.size(), requestsBefore);    // no network for a filter change
    service.setShortsLookup(ShortsLookup());
    CHECK_EQ(service.merged().hiddenShorts, hiddenBefore);
    CHECK(containsId(service.merged().rows, target));

    // saveNow: a copy is written on the lane and reported by saved(). The saves the rounds asked
    // for have to be over first, however long the lane takes: a save asked for while one runs is
    // coalesced, and the count would then move for the older snapshot. (A fixed 300 ms wait here
    // failed once on a loaded machine: the file still held the 15 RSS rows.)
    CHECK(waitFor([&] { return !service.saving(); }, 15000));
    const int savesBefore = service.savesCompleted();
    QList<bool> results;
    QObject::connect(&service, &FeedService::saved, [&results](bool ok) { results.append(ok); });
    service.saveNow();
    CHECK(service.saving());
    CHECK(waitFor([&] { return service.savesCompleted() == savesBefore + 1; }, 15000));
    CHECK(!service.saving());
    CHECK_EQ(results.size(), 1);
    CHECK(!results.isEmpty() && results.first());
    bool ok = false;
    std::vector<ChannelFeedState> loaded = store.load(&ok);
    CHECK(ok);
    CHECK_EQ(loaded.size(), std::size_t(1));
    if (!loaded.empty()) {
        CHECK_EQ(loaded[0].channel.id, std::string(kNasa));
        CHECK_EQ(loaded[0].rows.size(), state->rows.size());
    }

    // Two calls in a row: the second waits for the first and is then taken from the state as it
    // is at that moment; whenever no save is running or waiting, the file is the current state.
    const std::uint64_t orderBefore = store.lastWrittenOrder();
    service.saveNow();
    service.saveNow();
    service.saveNow();
    CHECK(waitFor([&] { return !service.saving(); }, 15000));
    CHECK_EQ(service.savesCompleted(), savesBefore + 3);      // one running, the rest coalesced into one
    CHECK_EQ(store.lastWrittenOrder(), orderBefore + 2);
    loaded = store.load(&ok);
    CHECK(ok && !loaded.empty() && loaded[0].rows.size() == state->rows.size());

    // saveSync: written before it returns, and numbered after every earlier snapshot.
    CHECK(QFile::remove(path));
    CHECK(service.saveSync());
    CHECK(QFile::exists(path));
    CHECK_EQ(store.lastWrittenOrder(), orderBefore + 3);
    loaded = store.load(&ok);
    CHECK(ok);
    CHECK(!loaded.empty() && loaded[0].rows.size() == state->rows.size());

    // The shutdown case: a lane save is still on its way when saveSync writes. Whichever of the
    // two finishes last, the file holds saveSync's (newer) snapshot.
    service.saveNow();
    CHECK(service.saveSync());
    CHECK(waitFor([&] { return !service.saving(); }, 15000));
    CHECK_EQ(store.lastWrittenOrder(), orderBefore + 5);
    loaded = store.load(&ok);
    CHECK(ok && !loaded.empty() && loaded[0].rows.size() == state->rows.size());

    // The store itself: an older snapshot never replaces a newer one, in whatever order the
    // writes arrive; a skipped write is not an error.
    {
        QTemporaryDir orderDir;
        FeedCacheStore ordered(orderDir.path() + QStringLiteral("/feed.json"));
        CHECK_EQ(ordered.lastWrittenOrder(), std::uint64_t(0));
        std::vector<ChannelFeedState> older(1), newer(1);
        older[0].channel = ChannelRef(kNasa, "NASA");
        newer[0].channel = ChannelRef(kNasa, "NASA");
        older[0].rows.assign(state->rows.begin(), state->rows.begin() + 3);
        newer[0].rows = state->rows;
        CHECK(ordered.save(newer, 7));
        CHECK_EQ(ordered.lastWrittenOrder(), std::uint64_t(7));
        CHECK(ordered.save(older, 6));                       // arrives late: skipped
        CHECK_EQ(ordered.lastWrittenOrder(), std::uint64_t(7));
        std::vector<ChannelFeedState> back = ordered.load(&ok);
        CHECK(ok && back.size() == 1 && back[0].rows.size() == newer[0].rows.size());
        CHECK(ordered.save(older, 8));                       // a later snapshot always wins, whatever it holds
        back = ordered.load(&ok);
        CHECK(ok && back.size() == 1 && back[0].rows.size() == std::size_t(3));
        CHECK(ordered.save(newer));                          // unnumbered: the newest so far
        CHECK_EQ(ordered.lastWrittenOrder(), std::uint64_t(9));
        back = ordered.load(&ok);
        CHECK(ok && back.size() == 1 && back[0].rows.size() == newer[0].rows.size());
        // A second service on a store that has been written to numbers above it.
        FeedService second(&s.net, &s.client, &ordered, nullptr, [&now] { return now; });
        CHECK(second.saveSync());
        CHECK_EQ(ordered.lastWrittenOrder(), std::uint64_t(10));
    }

    // Without a store neither save does anything.
    FeedService bare(&s.net, &s.client, nullptr, nullptr, [&now] { return now; });
    CHECK(!bare.saveSync());
    bare.saveNow();
    waitFor([] { return false; }, 50);
    CHECK_EQ(bare.savesCompleted(), 0);
    bare.extend();                                  // before the cache is loaded: nothing starts
    CHECK(!bare.extending());
}

// ---------------------------------------------------------------- search (DSC-08/09)

static void searchService() {
    const QJsonObject plain = SearchService::firstPageBody(QStringLiteral("kurzgesagt"), SearchFilter());
    CHECK_EQ(plain.value(QStringLiteral("query")).toString(), QStringLiteral("kurzgesagt"));
    CHECK(!plain.contains(QStringLiteral("params")));   // the default filter sends no params
    CHECK_EQ(plain.size(), 1);
    SearchFilter videos;
    videos.type = ResultTypeFilter::Video;
    videos.uploadDate = UploadDateFilter::Week;
    const QJsonObject filtered = SearchService::firstPageBody(QStringLiteral("nasa"), videos);
    const QString params = filtered.value(QStringLiteral("params")).toString();
    CHECK_EQ(params, QString::fromStdString(searchParams(videos)));
    SearchFilter decoded;
    CHECK(decodeSearchParams(params.toStdString(), &decoded));
    CHECK(decoded == videos);

    CHECK_EQ(SearchService::fallbackToken(40), QStringLiteral("ytdlp:40"));
    CHECK(SearchService::isFallbackToken(SearchService::fallbackToken(40)));
    CHECK(SearchService::isFallbackToken(QStringLiteral("ytdlp:0")));
    CHECK(!SearchService::isFallbackToken(QStringLiteral("EpcDEgprdXJ6Z2VzYWd0")));
    CHECK(!SearchService::isFallbackToken(QString()));

    Stack s(QStringLiteral("api-search"));
    SearchService service(&s.client);
    int fallbackCalls = 0;
    SearchFallback fallback;
    fallback.search = [&fallbackCalls](const QString &, const SearchFilter &, int, int, QObject *, SearchFallback::Done) -> quint64 {
        ++fallbackCalls;
        return 0;   // declined: the first-tier answer stands
    };
    service.setFallback(fallback);

    s.net.on(QStringLiteral("youtubei/v1/search"), QByteArray("\"continuation\""), 200, fixture(QStringLiteral("search_en_p2.json")));
    const SearchResult expected = SearchService::parseContinuation(fixtureJson(QStringLiteral("search_en_p2.json")), QStringLiteral("en"));
    SearchResult result;
    bool got = false;
    service.prefetchPage(QStringLiteral("kurzgesagt"), SearchFilter(), QStringLiteral("TOKEN-P2"), [&](const SearchResult &r) { result = r; got = true; });
    CHECK(waitFor([&got] { return got; }));
    CHECK(result.outcome == Outcome::Ok);
    CHECK(!result.rows.empty());
    CHECK_EQ(result.rows.size(), expected.rows.size());
    CHECK(!result.fallback);
    CHECK_EQ(sentBody(s.net).value(QStringLiteral("continuation")).toString(), QStringLiteral("TOKEN-P2"));

    // Nothing to prefetch: no request at all.
    const int sentBefore = s.net.requests.size();
    got = false;
    service.prefetchPage(QStringLiteral("kurzgesagt"), SearchFilter(), QString(), [&](const SearchResult &r) { result = r; got = true; });
    CHECK(waitFor([&got] { return got; }));
    CHECK(result.outcome == Outcome::Unavailable);
    got = false;
    service.prefetchPage(QStringLiteral("kurzgesagt"), SearchFilter(), SearchService::fallbackToken(20), [&](const SearchResult &r) { result = r; got = true; });
    CHECK(waitFor([&got] { return got; }));
    CHECK(result.outcome == Outcome::Unavailable);
    CHECK_EQ(s.net.requests.size(), sentBefore);
    CHECK_EQ(fallbackCalls, 0);

    // A failed prefetch is reported as is and never starts the second tier...
    s.net.rules.clear();
    s.net.on(QStringLiteral("youtubei/v1/search"), QByteArray(), 503, "");
    got = false;
    service.prefetchPage(QStringLiteral("kurzgesagt"), SearchFilter(), QStringLiteral("TOKEN-P3"), [&](const SearchResult &r) { result = r; got = true; });
    CHECK(waitFor([&got] { return got; }));
    CHECK(result.outcome == Outcome::Http5xx);
    CHECK_EQ(fallbackCalls, 0);
    // ...while the same failure on a user-requested page asks it.
    got = false;
    service.nextPage(QStringLiteral("kurzgesagt"), SearchFilter(), QStringLiteral("TOKEN-P3"), 20, [&](const SearchResult &r) { result = r; got = true; });
    CHECK(waitFor([&got] { return got; }));
    CHECK_EQ(fallbackCalls, 1);
    CHECK(result.outcome == Outcome::Http5xx);
    CHECK_EQ(service.pending(), 0);
}

// ---------------------------------------------------------------- Shorts checks (DSC-17)

static void shortsStore() {
    Stack s(QStringLiteral("api-shorts"));
    s.net.on(QStringLiteral("youtubei/v1/browse"), QByteArray(), 200, fixture(QStringLiteral("channel_shorts.json")));
    ShortsStore store(&s.client, QString());
    QStringList asked;
    store.setTabParamsLookup([&asked](const QString &channelId) {
        asked.append(channelId);
        return QStringLiteral("CHANNELOWNSHORTSPARAMS");
    });
    Item candidate;
    candidate.id = "k5Z7sFwFF7Q";                   // listed first on the recorded Shorts tab
    candidate.kind = ItemKind::Video;
    candidate.durationSeconds = 45;
    candidate.channelId = kNasa;
    candidate.liveStatus = media::LiveStatus::NotLive;
    std::vector<Item> rows;
    rows.push_back(candidate);

    CHECK(store.checkedChannels().isEmpty());
    CHECK_EQ(store.runningChecks(), 0);
    CHECK_EQ(store.requestChecks(rows), 1);
    CHECK_EQ(store.runningChecks(), 1);             // started at once (at most 2 in parallel)
    CHECK(waitFor([&store] { return store.pendingChecks() == 0; }));
    CHECK_EQ(store.runningChecks(), 0);
    CHECK(store.checkedChannels() == QStringList() << QString::fromLatin1(kNasa));
    CHECK(asked == QStringList() << QString::fromLatin1(kNasa));
    const QJsonObject sent = sentBody(s.net);
    CHECK_EQ(sent.value(QStringLiteral("browseId")).toString(), QString::fromLatin1(kNasa));
    CHECK_EQ(sent.value(QStringLiteral("params")).toString(), QStringLiteral("CHANNELOWNSHORTSPARAMS"));
    CHECK(isConfirmedShort(store.knowledge().evidence("k5Z7sFwFF7Q")));
    CHECK_EQ(store.requestChecks(rows), 0);         // settled and recently checked
    CHECK_EQ(s.net.count(QStringLiteral("youtubei/v1/browse")), 1);

    // Without a lookup the recorded web params are used.
    Stack t(QStringLiteral("api-shorts-2"));
    t.net.on(QStringLiteral("youtubei/v1/browse"), QByteArray(), 200, fixture(QStringLiteral("channel_shorts.json")));
    ShortsStore plain(&t.client, QString());
    CHECK_EQ(plain.requestChecks(rows), 1);
    CHECK(waitFor([&plain] { return plain.pendingChecks() == 0; }));
    CHECK_EQ(sentBody(t.net).value(QStringLiteral("params")).toString(), ChannelService::fallbackTabParams(QStringLiteral("shorts")));
    CHECK_EQ(plain.checkedChannels().size(), 1);
}

// ---------------------------------------------------------------- SponsorBlock (SB-01/02)

static void sponsorBlock() {
    FakeTransport net;
    WorkLane lane(QStringLiteral("api-sponsor"));
    SponsorBlockService service(&net, &lane);
    CHECK(!service.highlightEnabled());
    const QString plainUrl = service.requestUrl(kVideo).toString();
    CHECK(!plainUrl.contains(QLatin1String("poi_highlight")));
    CHECK(!plainUrl.contains(kVideo));               // only the hash prefix leaves the device
    net.on(QStringLiteral("sponsor.ajay.app/api/skipSegments/"), QByteArray(), 200, fixture(QStringLiteral("sponsorblock_prefix.json")));

    SponsorResult result;
    bool got = false;
    auto fetch = [&] {
        got = false;
        service.fetch(kVideo, &net, [&](const SponsorResult &r) { result = r; got = true; });
        return waitFor([&got] { return got; });
    };
    CHECK(fetch());
    CHECK(!isFailure(result.outcome));
    CHECK(!result.fromCache);
    CHECK(fetch());
    CHECK(result.fromCache);
    CHECK_EQ(net.count(QStringLiteral("sponsor.ajay.app")), 1);

    service.setHighlightEnabled(true);               // changes the query: the cache is cleared
    CHECK(service.highlightEnabled());
    CHECK(service.requestUrl(kVideo).toString().contains(QLatin1String("poi_highlight")));
    CHECK(fetch());
    CHECK(!result.fromCache);
    CHECK_EQ(net.count(QStringLiteral("sponsor.ajay.app")), 2);
    CHECK(net.requests.last().url.toString().contains(QLatin1String("poi_highlight")));
    service.setHighlightEnabled(true);               // unchanged: the cache stays
    CHECK(fetch());
    CHECK(result.fromCache);
    service.setHighlightEnabled(false);
    CHECK(!service.highlightEnabled());
    CHECK(fetch());
    CHECK(!result.fromCache);
    CHECK_EQ(net.count(QStringLiteral("sponsor.ajay.app")), 3);
    CHECK(!net.requests.last().url.toString().contains(QLatin1String("poi_highlight")));
}

// ---------------------------------------------------------------- suggestions (DSC-11)

static void suggestService() {
    FakeTransport net;
    SuggestService service(&net, DiscoveryLocale());
    CHECK_EQ(service.locale().hl, QStringLiteral("en"));
    DiscoveryLocale de;
    de.hl = QStringLiteral("de");
    de.gl = QStringLiteral("DE");
    service.setLocale(de);
    CHECK_EQ(service.locale().hl, QStringLiteral("de"));
    CHECK_EQ(service.locale().gl, QStringLiteral("DE"));
    net.on(QStringLiteral("suggestqueries.google.com"), QByteArray(), 200, fixture(QStringLiteral("suggest_de.json")));
    SuggestResult result;
    bool got = false;
    service.fetch(QStringLiteral("kurzg"), [&](const SuggestResult &r) { result = r; got = true; });
    CHECK(waitFor([&got] { return got; }));
    CHECK(result.outcome == Outcome::Ok);
    CHECK_EQ(result.suggestions.size(), 10);
    CHECK_EQ(result.suggestions.value(0), QStringLiteral("kurz gesagt"));
    CHECK(net.requests.last().url.query().contains(QLatin1String("hl=de")));   // the new locale is used
}

// ---------------------------------------------------------------- watch page (DSC-13)

static void watchNext() {
    Stack s(QStringLiteral("api-watch"));
    qint64 now = 1000;
    WatchNextService service(&s.client, nullptr, nullptr, [&now] { return now; });
    service.setPlayerEnabled(false);
    s.net.on(QStringLiteral("/youtubei/v1/next"), QByteArray("\"videoId\""), 200, fixture(QStringLiteral("next_en.json")));
    auto settled = [&service] {
        return waitFor([&service] {
            const std::shared_ptr<const WatchPage> p = service.page(kVideo);
            return p && !p->loading() && !p->relatedPending;
        }, 8000);
    };
    CHECK(service.request(kVideo));
    CHECK(settled());
    std::shared_ptr<const WatchPage> first = service.page(kVideo);
    CHECK(first != nullptr);
    if (!first) return;
    CHECK(first->related.size() >= 15);
    CHECK(!first->commentsContinuation.isEmpty());
    CHECK_EQ(service.commentsContinuation(kVideo), first->commentsContinuation);
    std::string target;
    for (const Item &it : first->related)
        if (isVideoLike(it) && !isConfirmedShort(it.shortEvidence)) { target = it.id; break; }
    CHECK(!target.empty());
    CHECK(service.request(kVideo));                 // fresh: served from the cache
    CHECK_EQ(s.net.count(QStringLiteral("/youtubei/v1/next")), 1);

    service.forget(kVideo);
    CHECK(service.page(kVideo) == nullptr);
    CHECK(service.commentsContinuation(kVideo).isEmpty());
    service.forget(QStringLiteral("aaaaaaaaaaa"));  // unknown: harmless

    // The next request is sent again and the Shorts lookup feeds the related filter.
    RowFilterSettings filter;
    filter.hideShorts = true;
    service.setRowFilterSettings(filter);
    service.setShortsLookup([target](const std::string &id) { return id == target ? ShortEvidence::Renderer : ShortEvidence::None; });
    CHECK(service.request(kVideo));
    CHECK_EQ(s.net.count(QStringLiteral("/youtubei/v1/next")), 2);
    CHECK(settled());
    const std::shared_ptr<const WatchPage> second = service.page(kVideo);
    CHECK(second != nullptr);
    if (!second) return;
    CHECK(!containsId(second->related, target));
    CHECK(second->hiddenShorts >= 1);
    CHECK_EQ(second->related.size() + static_cast<std::size_t>(second->hiddenShorts), first->related.size());
}

// A Short's watch-next answer (recorded for the same Short as shorts_item_en): what a watch info
// built for a Short would show, and whether the comments entry works for a Short id as for a video.
static void shortWatchNext() {
    const QJsonObject next = fixtureJson(QStringLiteral("shorts_next_en.json"));
    const QString id = QStringLiteral("AAAAAAAAAAA");
    const NextParse parsed = WatchNextService::parseNext(next, QStringLiteral("en"), id);
    CHECK(parsed.outcome == Outcome::Ok);
    const WatchInfoData &info = parsed.info;
    CHECK(!info.title.empty());
    CHECK(!info.ownerName.empty() && isFeedChannelId(info.ownerId));
    CHECK(info.ownerAvatar.compare(0, 8, "https://") == 0);
    CHECK(!info.subscriberText.empty() && info.subscriberCount > 0);   // "5.24 million subscribers"
    CHECK(!info.likeText.empty() && info.likeCount > 0);
    CHECK(info.viewCount > 0 || !info.viewText.empty());
    CHECK(!parsed.commentsContinuation.isEmpty());
    CHECK(!parsed.related.empty());                                    // related videos exist for a Short too

    // The comments entry (the first token and the total) from the same answer, as for a video.
    const CommentsEntry entry = CommentsService::parseEntry(next, "en");
    CHECK(entry.outcome == Outcome::Ok);
    CHECK(!entry.token.empty());
    CHECK(entry.total > 0 && !entry.totalText.empty());
    CHECK(!entry.commentsOff);

    // The same Short's item answer agrees with it: the exact like count behind the compact text.
    const ShortDetailResult item = ShortDetailService::parse(fixtureJson(QStringLiteral("shorts_item_en.json")), id, QStringLiteral("en"));
    CHECK(item.outcome == Outcome::Ok);
    CHECK(item.detail.likeCount > 0);
    const std::int64_t shown = parseCountText(info.likeText, "en");
    CHECK(shown > 0 && item.detail.likeCount * 100 >= shown * 90 && item.detail.likeCount * 100 <= shown * 110);
    CHECK(item.detail.commentCount > 0 && entry.total > 0 &&
          item.detail.commentCount * 100 >= entry.total * 80 && item.detail.commentCount * 100 <= entry.total * 120);   // seconds apart
    CHECK_EQ(item.detail.channelId, info.ownerId);
}

void runServiceApiTests() {
    transportAndClient();
    identity();
    workLane();
    channelService();
    commentsService();
    feedService();
    searchService();
    shortsStore();
    sponsorBlock();
    suggestService();
    watchNext();
    shortWatchNext();
}
