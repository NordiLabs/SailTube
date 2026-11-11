// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// NFR-ROB-01 fault injection, discovery side: the real catalog services (search, channel tabs,
// feed refresh, comments, watch-next / related) driven through a scripted transport and a real
// NetworkTransport against misbehaving local servers. Faults: malformed JSON / XML, a missing
// renderer container, HTTP 404 / 429 / 5xx, a network error, a timeout, one bad item among
// healthy ones. Every case asserts that the healthy items survive and that the failure carries a
// fixed class (Outcome), never "drift" for a transport failure. Fixtures are synthetic
// (tests/fault_injection/fixtures). Build: tests/fault_injection/run.sh
#include "tests/fault_injection/FaultSupport.h"

#include "Core/Catalog/Domain/Outcome.h"
#include "Core/Catalog/Domain/SearchFilter.h"
#include "Core/Catalog/Domain/Subscriptions.h"
#include "Core/Catalog/Infrastructure/ChannelService.h"
#include "Core/Catalog/Infrastructure/CommentsService.h"
#include "Core/Catalog/Infrastructure/FeedService.h"
#include "Core/Catalog/Infrastructure/InnerTubeClient.h"
#include "Core/Catalog/Infrastructure/SearchService.h"
#include "Core/Catalog/Infrastructure/WatchNextService.h"
#include "Core/Catalog/Infrastructure/WorkLane.h"
#include "Clients/Sailfish/Presentation/CommentsModel.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <map>
#include <memory>

using namespace nordtile::video;
using namespace nordtile::video::catalog;
using fault::Answer;
using fault::FaultTransport;
using fault::fixture;

namespace {

const std::int64_t kNow = 1794124800;   // 2026-11-08T08:00:00Z

// The discovery stack of the app over an injected transport.
struct Stack {
    FaultTransport net;
    WorkLane lane;
    InnerTubeClient client;
    CatalogCounters counters;
    QObject context;
    Stack() : lane(QStringLiteral("fault"), 2), client(&net, nullptr, &lane, DiscoveryLocale()) {}
};

template <typename T>
bool hasId(const std::vector<T> &rows, const std::string &id) {
    for (const T &r : rows) if (r.id == id) return true;
    return false;
}

// "a,b,c" of the row ids (comparable with CHECK_EQ).
std::string idsOf(const std::vector<Item> &rows) {
    std::string out;
    for (const Item &r : rows) out += (out.empty() ? "" : ",") + r.id;
    return out;
}

Item row(const char *id, const std::string &channelId = std::string()) {
    Item it;
    it.id = id;
    it.title = std::string("fallback ") + id;
    it.channelId = channelId;
    it.publishedEpoch = kNow - 3600;
    return it;
}

// Second search tier (the yt-dlp flat search in the app).
struct FakeSearchFallback {
    int calls = 0;
    Outcome outcome = Outcome::Ok;
    std::vector<Item> rows{row("fiFallbk001"), row("fiFallbk002")};
    SearchFallback hook() {
        SearchFallback f;
        f.search = [this](const QString &, const SearchFilter &, int, int, QObject *context, SearchFallback::Done done) -> quint64 {
            ++calls;
            const Outcome o = outcome;
            const std::vector<Item> r = rows;
            QTimer::singleShot(0, context, [done, o, r] { done(o, r); });
            return static_cast<quint64>(calls);
        };
        f.cancel = [](quint64) {};
        return f;
    }
};

SearchResult search(SearchService &service) {
    std::shared_ptr<SearchResult> out = std::make_shared<SearchResult>();
    std::shared_ptr<bool> got = std::make_shared<bool>(false);
    service.firstPage(QStringLiteral("fault query"), SearchFilter(), [out, got](const SearchResult &r) { *out = r; *got = true; });
    CHECK(catalog_test::waitFor([got] { return *got; }, 8000));
    return *out;
}

// ---- search (DSC-08, first tier + fallback) -------------------------------------------------

void searchOneBadItem() {
    Stack st;
    st.net.handler = [](const HttpRequest &) { return Answer::ok(fixture("search_mixed.json")); };
    SearchService service(&st.client, &st.counters);
    FakeSearchFallback fb;
    service.setFallback(fb.hook());
    const SearchResult r = search(service);
    CHECK(r.outcome == Outcome::Ok);
    CHECK_EQ(r.rows.size(), std::size_t(3));
    CHECK(hasId(r.rows, "fiSearch001"));
    CHECK(hasId(r.rows, "fiSearch002"));
    CHECK(hasId(r.rows, "UCfaultChannel0000000002"));
    CHECK(!hasId(r.rows, "fiSearch099"));                       // inside an unknown renderer: skipped
    CHECK(r.unknown.contains(QStringLiteral("futureShelfViewModel")));
    CHECK_EQ(r.continuation, QStringLiteral("FI-SEARCH-PAGE-2"));
    CHECK(!r.fallback);
    CHECK_EQ(fb.calls, 0);
    // A field that does not parse keeps its raw text; the row stays.
    for (const Item &it : r.rows)
        if (it.id == "fiSearch002") { CHECK_EQ(it.viewText, std::string("not a number views")); CHECK(it.viewCount < 0); }
    CHECK_EQ(st.counters.driftTotal(), std::int64_t(0));
}

// One first-tier fault -> the fallback answers, the first tier's class is kept for diagnostics.
void searchFaultFallsBack(const Answer &fault, Outcome expected) {
    Stack st;
    st.net.handler = [fault](const HttpRequest &) { return fault; };
    SearchService service(&st.client, &st.counters);
    FakeSearchFallback fb;
    service.setFallback(fb.hook());
    const SearchResult r = search(service);
    CHECK(r.outcome == Outcome::Ok);
    CHECK(r.fallback);
    CHECK(r.firstTierOutcome == expected);
    CHECK_EQ(fb.calls, 1);
    CHECK(hasId(r.rows, "fiFallbk001") && hasId(r.rows, "fiFallbk002"));
    CHECK_EQ(st.counters.value(Area::Search, expected), std::int64_t(1));
    CHECK_EQ(st.counters.driftTotal(), std::int64_t(expected == Outcome::ShapeDrift ? 1 : 0));
}

void searchNoFallbackClassified() {
    const struct { Answer answer; Outcome outcome; int status; } rows[] = {
        {Answer::http(503), Outcome::Http5xx, 503},
        {Answer::http(429), Outcome::RateLimited, 429},
        {Answer::http(403), Outcome::Http4xx, 403},
        {Answer::network(), Outcome::Network, 0},
        {Answer::timeout(), Outcome::Network, 0},
        {Answer::ok(fixture("malformed.json")), Outcome::ShapeDrift, 200},
    };
    for (const auto &row : rows) {
        Stack st;
        const Answer a = row.answer;
        st.net.handler = [a](const HttpRequest &) { return a; };
        SearchService service(&st.client, &st.counters);   // no second tier
        const SearchResult r = search(service);
        CHECK(r.outcome == row.outcome);
        CHECK_EQ(r.httpStatus, row.status);
        CHECK(r.rows.empty());
        CHECK(!r.fallback);
        CHECK_EQ(service.pending(), 0);
    }
}

void searchBothTiersFail() {
    Stack st;
    st.net.handler = [](const HttpRequest &) { return Answer::http(500); };
    SearchService service(&st.client, &st.counters);
    FakeSearchFallback fb;
    fb.outcome = Outcome::Network;
    service.setFallback(fb.hook());
    const SearchResult r = search(service);
    CHECK_EQ(fb.calls, 1);
    CHECK(r.outcome == Outcome::Http5xx);   // the first tier's fixed class, not the fallback's
    CHECK(r.rows.empty());
    CHECK(!r.fallback);
    CHECK_EQ(st.counters.driftTotal(), std::int64_t(0));
}

// ---- the real NetworkTransport against misbehaving servers ----------------------------------

struct Sent { bool done = false; HttpReply reply; qint64 ms = 0; };

std::shared_ptr<Sent> sendReal(HttpTransport &t, int timeoutMs) {
    std::shared_ptr<Sent> s = std::make_shared<Sent>();
    HttpRequest r;
    r.url = QUrl(QStringLiteral("https://www.youtube.com/youtubei/v1/search?prettyPrint=false"));
    r.method = "POST";
    r.body = "{}";
    r.timeoutMs = timeoutMs;
    std::shared_ptr<QElapsedTimer> clock = std::make_shared<QElapsedTimer>();
    clock->start();
    t.send(r, nullptr, [s, clock](const HttpReply &reply) { s->reply = reply; s->done = true; s->ms = clock->elapsed(); });
    return s;
}

// Search over the real transport: the first tier must fail with `expected` and the fallback answer.
void searchOverReal(quint16 port, Outcome expected) {
    fault::LoopbackTransport net(port, 1000);
    WorkLane lane(QStringLiteral("fault-real"));
    InnerTubeClient client(&net, nullptr, &lane, DiscoveryLocale());
    CatalogCounters counters;
    SearchService service(&client, &counters);
    FakeSearchFallback fb;
    service.setFallback(fb.hook());
    const SearchResult r = search(service);
    CHECK(r.fallback);
    CHECK(r.firstTierOutcome == expected);
    CHECK(hasId(r.rows, "fiFallbk001"));
}

void realTimeout() {
    fault::BadServer server;
    server.mode = fault::BadServer::Mode::Silent;
    CHECK(server.start());
    fault::LoopbackTransport net(server.port(), 1000);
    std::shared_ptr<Sent> s = sendReal(net, 1000);
    CHECK(catalog_test::waitFor([s] { return s->done; }, 6000));
    CHECK(s->reply.timedOut);
    CHECK_EQ(s->reply.status, 0);
    CHECK(s->ms >= 900 && s->ms < 5000);
    CHECK(outcomeForHttp(s->reply.status) == Outcome::Network);
    searchOverReal(server.port(), Outcome::Network);
}

void realConnectionRefused() {
    quint16 port = 0;
    {
        QTcpServer probe;
        CHECK(probe.listen(QHostAddress::LocalHost, 0));
        port = probe.serverPort();
    }   // closed again: nothing listens there
    fault::LoopbackTransport net(port, 1000);
    std::shared_ptr<Sent> s = sendReal(net, 1000);
    CHECK(catalog_test::waitFor([s] { return s->done; }, 6000));
    CHECK_EQ(s->reply.status, 0);
    CHECK(!s->reply.timedOut);
    CHECK(!s->reply.errorText.isEmpty());
    searchOverReal(port, Outcome::Network);
}

void realBadServer(fault::BadServer::Mode mode) {
    fault::BadServer server;
    server.mode = mode;
    CHECK(server.start());
    fault::LoopbackTransport net(server.port(), 3000);
    std::shared_ptr<Sent> s = sendReal(net, 3000);
    CHECK(catalog_test::waitFor([s] { return s->done; }, 8000));
    CHECK_EQ(s->reply.status, 0);
    CHECK(outcomeForHttp(s->reply.status) == Outcome::Network);
    searchOverReal(server.port(), Outcome::Network);
}

// A body cut short below a 200 status line. The transport keeps status 200 with the partial
// body, so the first tier reports ShapeDrift ("the parser needs an update") instead of Network.
// The fallback still answers (asserted); the classification is the known defect.
void realTruncatedBody() {
    fault::BadServer server;
    server.mode = fault::BadServer::Mode::Truncated;
    CHECK(server.start());
    fault::LoopbackTransport net(server.port(), 3000);
    WorkLane lane(QStringLiteral("fault-real"));
    InnerTubeClient client(&net, nullptr, &lane, DiscoveryLocale());
    CatalogCounters counters;
    SearchService service(&client, &counters);
    FakeSearchFallback fb;
    service.setFallback(fb.hook());
    const SearchResult r = search(service);
    CHECK(r.fallback);                       // degrade: the second tier still answers
    CHECK(hasId(r.rows, "fiFallbk001"));
    std::printf("  truncated body: first tier classified as %s (drift counter %lld)\n",
                outcomeName(r.firstTierOutcome), static_cast<long long>(counters.driftTotal()));
    // B1 fixed (catalog component): the transport marks the cut-off body truncated (status 0),
    // InnerTubeClient sends it once more, and the first tier reports Network, never drift.
    fault::run("transport_truncated_body_classified_network", [&] {
        CHECK(r.firstTierOutcome == Outcome::Network);
        CHECK_EQ(counters.driftTotal(), std::int64_t(0));
    });
}

// ---- channel tabs (DSC-12) ------------------------------------------------------------------

std::string channel(char tag) { return std::string("UCfaultFeedChannel00000") + tag; }

QByteArray videosTab(char tag) {
    return fixture("channel_videos_mixed.json", {{"CH", QByteArray::fromStdString(channel(tag))}, {"T", QByteArray(1, tag)}});
}

ChannelPage browse(ChannelService &service, QObject *context, const std::string &channelId) {
    std::shared_ptr<ChannelPage> out = std::make_shared<ChannelPage>();
    std::shared_ptr<bool> got = std::make_shared<bool>(false);
    service.browseTab(QString::fromStdString(channelId), QStringLiteral("videos"), context,
                      [out, got](ChannelPage p) { *out = p; *got = true; });
    CHECK(catalog_test::waitFor([got] { return *got; }, 8000));
    return *out;
}

void channelOneBadItem() {
    Stack st;
    st.net.handler = [](const HttpRequest &) { return Answer::ok(videosTab('A')); };
    ChannelService service(&st.client, nullptr, &st.counters);
    const ChannelPage p = browse(service, &st.context, channel('A'));
    CHECK(p.outcome == Outcome::Ok);
    CHECK_EQ(idsOf(p.items), std::string("fiAtab00001,fiAtab00002"));
    CHECK(p.unparsed >= 1);                                    // the row without an id
    CHECK(p.unknown.contains(QStringLiteral("futureGridItemViewModel")));
    CHECK_EQ(p.continuation, QStringLiteral("FI-TAB-A-2"));
    CHECK_EQ(st.counters.driftTotal(), std::int64_t(0));
}

void channelFaultsClassified() {
    const struct { Answer answer; Outcome outcome; bool drift; } rows[] = {
        {Answer::ok(fixture("malformed.json")), Outcome::ShapeDrift, true},
        {Answer::ok(fixture("channel_no_tabs.json")), Outcome::ShapeDrift, true},
        {Answer::ok(fixture("channel_terminated.json")), Outcome::Unavailable, false},
        {Answer::http(404, fixture("error_page.html")), Outcome::Unavailable, false},
        {Answer::http(429), Outcome::RateLimited, false},
        {Answer::http(503), Outcome::Http5xx, false},
        {Answer::network(), Outcome::Network, false},
        {Answer::timeout(), Outcome::Network, false},
    };
    for (const auto &row : rows) {
        Stack st;
        const Answer a = row.answer;
        st.net.handler = [a](const HttpRequest &) { return a; };
        ChannelService service(&st.client, nullptr, &st.counters);
        const ChannelPage p = browse(service, &st.context, channel('B'));
        CHECK(p.outcome == row.outcome);
        CHECK(p.items.empty());
        CHECK_EQ(st.counters.driftTotal(), std::int64_t(row.drift ? 1 : 0));
    }
}

void channelOneDeadAmongSeveral() {
    Stack st;
    st.net.handler = [](const HttpRequest &r) {
        const std::string id = fault::field(r, "browseId").toStdString();
        if (id == channel('B')) return Answer::http(503);
        return Answer::ok(videosTab(id.back()));
    };
    ChannelService service(&st.client, nullptr, &st.counters);
    std::map<char, ChannelPage> pages;
    int answered = 0;
    for (char tag : {'A', 'B', 'C'}) {
        service.browseTab(QString::fromStdString(channel(tag)), QStringLiteral("videos"), &st.context,
                          [&pages, &answered, tag](ChannelPage p) { pages[tag] = p; ++answered; });
    }
    CHECK(catalog_test::waitFor([&answered] { return answered == 3; }, 8000));
    CHECK(pages['A'].outcome == Outcome::Ok && pages['A'].items.size() == 2);
    CHECK(pages['B'].outcome == Outcome::Http5xx && pages['B'].items.empty());
    CHECK(pages['C'].outcome == Outcome::Ok && pages['C'].items.size() == 2);
    CHECK(hasId(pages['C'].items, "fiCtab00001"));
}

// ---- feed refresh with one dead channel among several (DSC-05) ------------------------------

bool feedIdle(const FeedService &s) { return s.cacheLoaded() && !s.refreshing() && !s.extending(); }

QByteArray rss(const char *name, char tag) {
    return fixture(name, {{"CH", QByteArray::fromStdString(channel(tag))}, {"T", QByteArray(1, tag)}});
}

void feedOneDeadChannel() {
    Stack st;
    bool aDown = false;
    st.net.handler = [&aDown](const HttpRequest &r) {
        const QString rssFor = fault::rssChannel(r);
        if (!rssFor.isEmpty()) {
            const char tag = rssFor.toStdString().back();
            switch (tag) {
            case 'A': return aDown ? Answer::http(503) : Answer::ok(rss("rss_ok.xml", 'A'));
            case 'B': return Answer::ok(rss("rss_malformed.xml", 'B'));               // XML cut short
            case 'C': return Answer::http(404, fixture("error_page.html"));            // dead channel
            case 'D': return Answer::network();
            case 'E': return Answer::ok(rss("rss_one_bad_entry.xml", 'E'));
            case 'F': return Answer::timeout();
            }
            return Answer::network();
        }
        if (fault::endpoint(r, "browse")) {
            const char tag = fault::field(r, "browseId").toStdString().back();
            switch (tag) {
            case 'A': return Answer::http(503);
            case 'B': return Answer::ok(videosTab('B'));
            case 'C': return Answer::http(503);
            case 'D': return Answer::http(429);
            case 'F': return Answer::ok(videosTab('F'));
            }
        }
        return Answer::http(500);
    };
    FeedService service(&st.net, &st.client, nullptr, nullptr, [] { return kNow; });
    service.setLiveEnrichment(false);
    service.setCounters(&st.counters);
    int extractorCalls = 0;
    service.setExtractor([&extractorCalls, &st](const ChannelRef &c, std::function<void(const ExtractorFeedResult &)> done) {
        ++extractorCalls;
        ExtractorFeedResult r;
        if (c.id == channel('D')) {
            r.outcome = Outcome::Ok;
            r.rows.push_back(row("fiDxtr00001", c.id));
        } else {
            r.outcome = Outcome::Network;
        }
        QTimer::singleShot(0, &st.context, [done, r] { done(r); });
    });
    QStringList failed;
    QObject::connect(&service, &FeedService::channelFailed, [&failed](const QString &id, int) { failed.append(id); });
    ChannelList list;
    for (char tag : {'A', 'B', 'C', 'D', 'E', 'F'}) list.push_back(ChannelRef(channel(tag), std::string("Fault ") + tag));
    service.setChannels(list);
    service.start();
    CHECK(catalog_test::waitFor([&service] { return feedIdle(service); }, 10000));

    const auto rowsOf = [&service](char tag) {
        const ChannelFeedState *s = service.schedule().find(channel(tag));
        return s ? static_cast<int>(s->rows.size()) : -1;
    };
    const auto sourceOf = [&service](char tag) {
        const ChannelFeedState *s = service.schedule().find(channel(tag));
        return s ? s->source : FeedSource::None;
    };
    CHECK_EQ(rowsOf('A'), 3);
    CHECK_EQ(rowsOf('B'), 2);   // RSS drift -> Videos tab, its bad rows skipped
    CHECK_EQ(rowsOf('C'), 0);   // the dead one
    CHECK_EQ(rowsOf('D'), 1);   // RSS network + tab 429 -> extractor
    CHECK_EQ(rowsOf('E'), 2);   // one bad RSS entry skipped
    CHECK_EQ(rowsOf('F'), 2);   // RSS timeout -> Videos tab
    CHECK(sourceOf('A') == FeedSource::Rss && sourceOf('E') == FeedSource::Rss);
    CHECK(sourceOf('B') == FeedSource::InnerTube && sourceOf('F') == FeedSource::InnerTube);
    CHECK(sourceOf('D') == FeedSource::Extractor);
    CHECK_EQ(extractorCalls, 2);   // C and D only
    CHECK(failed == QStringList() << QString::fromStdString(channel('C')));
    const ChannelFeedState *dead = service.schedule().find(channel('C'));
    CHECK(dead && isFailure(dead->lastOutcome));
    CHECK_EQ(service.lastRoundFailures(), 1);
    CHECK_EQ(service.lastRoundAttempts(), 6);
    CHECK(!service.lastRoundAllFailed());
    CHECK(hasId(service.merged().rows, "fiArss00001"));
    CHECK(hasId(service.merged().rows, "fiErss00002"));
    CHECK(hasId(service.merged().rows, "fiDxtr00001"));
    CHECK(st.counters.value(Area::Feed, Outcome::RateLimited) >= 1);
    CHECK(st.counters.value(Area::Feed, Outcome::Http4xx) >= 1);

    // Next round A fails on every tier: its last good rows stay painted, the rest unchanged.
    aDown = true;
    failed.clear();
    service.refresh(true);
    CHECK(catalog_test::waitFor([&service] { return feedIdle(service); }, 10000));
    CHECK_EQ(rowsOf('A'), 3);
    CHECK_EQ(rowsOf('E'), 2);
    CHECK_EQ(rowsOf('D'), 1);
    CHECK_EQ(service.lastRoundFailures(), 2);
    CHECK(failed.contains(QString::fromStdString(channel('A'))) && failed.contains(QString::fromStdString(channel('C'))));
    CHECK(hasId(service.merged().rows, "fiArss00001"));
}

// ---- comments (DSC-16): one broken thread among several -------------------------------------

void commentsOneBrokenThread() {
    const QJsonObject page = QJsonDocument::fromJson(fixture("comments_threads_mixed.json")).object();
    const CommentsPage p = CommentsService::parsePage(page, "en", false);
    CHECK(p.outcome == Outcome::Ok);
    std::string ids;
    for (const Comment &c : p.comments) ids += (ids.empty() ? "" : ",") + c.id;
    CHECK_EQ(ids, std::string("UgxFaultThreadA,UgxFaultThreadC,UgxFaultThreadE"));
    CHECK_EQ(p.missingBodies, 2);      // B (no entity) and D (entity without an id)
    CHECK_EQ(p.continuation, std::string("FI-THREADS-2"));
    CHECK_EQ(p.total, std::int64_t(5));
    for (const Comment &c : p.comments) {
        if (c.id == "UgxFaultThreadA") CHECK_EQ(c.replyContinuation, std::string("FI-REPLIES-A"));
        if (c.id == "UgxFaultThreadC") CHECK_EQ(c.likeText, std::string("not a count"));   // raw text kept
    }
}

CommentsPage fetchThreads(Stack &st, CommentsService &service) {
    std::shared_ptr<CommentsPage> out = std::make_shared<CommentsPage>();
    std::shared_ptr<bool> got = std::make_shared<bool>(false);
    service.fetchThreads(QStringLiteral("FI-THREADS-1"), &st.context, [out, got](const CommentsPage &p) { *out = p; *got = true; });
    CHECK(catalog_test::waitFor([got] { return *got; }, 8000));
    return *out;
}

void commentsPageFaults() {
    const struct { Answer answer; Outcome outcome; bool drift; } rows[] = {
        {Answer::ok(fixture("malformed.json")), Outcome::ShapeDrift, true},
        {Answer::ok("{\"responseContext\":{}}"), Outcome::ShapeDrift, true},
        {Answer::http(429), Outcome::RateLimited, false},
        {Answer::http(503), Outcome::Http5xx, false},
        {Answer::network(), Outcome::Network, false},
        {Answer::timeout(), Outcome::Network, false},
    };
    for (const auto &row : rows) {
        Stack st;
        const Answer a = row.answer;
        st.net.handler = [a](const HttpRequest &) { return a; };
        CommentsService service(&st.client, &st.counters);
        const CommentsPage p = fetchThreads(st, service);
        CHECK(p.outcome == row.outcome);
        CHECK(p.comments.empty());
        CHECK_EQ(st.counters.driftTotal(), std::int64_t(row.drift ? 1 : 0));
    }
}

int rowOfComment(const sailfish::CommentsModel &m, const char *id) {
    for (int i = 0; i < m.rowCount(); ++i)
        if (m.data(m.index(i, 0), sailfish::CommentsModel::CommentIdRole).toString() == QLatin1String(id)) return i;
    return -1;
}

bool repliesBusy(const sailfish::CommentsModel &m, int row) {
    return m.data(m.index(row, 0), sailfish::CommentsModel::RepliesBusyRole).toBool();
}

// A replies failure on one thread is inline on that thread; the list and the other threads are
// unaffected (and nothing is retried automatically).
void commentsRepliesFailureIsolated(const QString &settings) {
    Stack st;
    st.net.handler = [](const HttpRequest &r) {
        const QString token = fault::field(r, "continuation");
        if (token == QLatin1String("FI-THREADS-1")) return Answer::ok(fixture("comments_threads_mixed.json"));
        if (token == QLatin1String("FI-THREADS-2"))
            return Answer::ok("{\"onResponseReceivedEndpoints\":[{\"appendContinuationItemsAction\":{\"continuationItems\":[]}}]}");
        if (token == QLatin1String("FI-REPLIES-A")) return Answer::http(503);
        if (token == QLatin1String("FI-REPLIES-C")) return Answer::ok(fixture("comments_replies.json", {{"TH", "FaultThreadC"}}));
        return Answer::http(404);
    };
    CommentsService service(&st.client, &st.counters);
    sailfish::CommentsModel model(&service, nullptr, settings);
    model.load(QStringLiteral("fiSeedVideo"), QStringLiteral("FI-THREADS-1"));
    CHECK(catalog_test::waitFor([&model] { return !model.busy(); }, 8000));
    CHECK_EQ(model.errorText(), QString());
    CHECK_EQ(model.session().threadCount(), std::size_t(3));
    CHECK(rowOfComment(model, "UgxFaultThreadA") >= 0 && rowOfComment(model, "UgxFaultThreadE") >= 0);

    const int rowA = rowOfComment(model, "UgxFaultThreadA");
    model.toggleReplies(rowA);
    CHECK(catalog_test::waitFor([&model, rowA] { return !repliesBusy(model, rowA); }, 8000));
    const int rowC = rowOfComment(model, "UgxFaultThreadC");
    CHECK(rowC > rowA);
    model.toggleReplies(rowC);
    CHECK(catalog_test::waitFor([&model, rowC] { return !repliesBusy(model, rowC); }, 8000));

    CHECK(model.session().thread(0).replies.empty());              // A: failed
    CHECK_EQ(model.session().thread(1).replies.size(), std::size_t(1));   // C: its healthy reply
    CHECK(model.session().thread(2).replies.empty());              // E: untouched
    // A's inline error sits on its "more" row right below it; C shows no error.
    const QString errorA = model.data(model.index(rowA + 1, 0), sailfish::CommentsModel::ReplyErrorTextRole).toString();
    CHECK(!errorA.isEmpty());
    const int rowC2 = rowOfComment(model, "UgxFaultThreadC");
    CHECK(model.data(model.index(rowC2, 0), sailfish::CommentsModel::ReplyErrorTextRole).toString().isEmpty());
    CHECK_EQ(model.errorText(), QString());                         // the list itself is fine
    CHECK(rowOfComment(model, "UgxFaultThreadE") >= 0);
    CHECK_EQ(st.net.count([](const HttpRequest &r) { return fault::field(r, "continuation") == QLatin1String("FI-REPLIES-A"); }), 1);
    CHECK_EQ(st.counters.value(Area::Comments, Outcome::Http5xx), std::int64_t(1));
}

void commentsEntryRateLimited(const QString &settings) {
    Stack st;
    st.net.handler = [](const HttpRequest &) { return Answer::http(429); };
    CommentsService service(&st.client, &st.counters);
    sailfish::CommentsModel model(&service, nullptr, settings);
    model.load(QStringLiteral("fiSeedVideo"));     // no token: the model asks watch-next first
    CHECK(catalog_test::waitFor([&model] { return !model.busy(); }, 8000));
    CHECK(!model.errorText().isEmpty());
    CHECK_EQ(model.rowCount(), 0);
    CHECK_EQ(st.counters.value(Area::Comments, Outcome::RateLimited), std::int64_t(1));
    CHECK_EQ(st.counters.driftTotal(), std::int64_t(0));
}

// ---- watch-next and related rows (DSC-13/14) ------------------------------------------------

std::shared_ptr<const WatchPage> settledPage(WatchNextService &s, const char *id) {
    const QString vid = QLatin1String(id);
    catalog_test::waitFor([&s, vid] {
        const std::shared_ptr<const WatchPage> p = s.page(vid);
        return !p || (!p->loading() && !p->relatedPending);
    }, 8000);
    return s.page(vid);
}

void relatedOneBadRow() {
    Stack st;
    st.net.handler = [](const HttpRequest &r) {
        if (fault::endpoint(r, "next")) return Answer::ok(fixture("next_related_mixed.json"));
        return Answer::http(500);
    };
    WatchNextService service(&st.client, &st.counters);
    service.setPlayerEnabled(false);
    CHECK(service.request(QStringLiteral("fiSeedVideo")));
    const std::shared_ptr<const WatchPage> p = settledPage(service, "fiSeedVideo");
    CHECK(p != nullptr);
    if (!p) return;
    CHECK(p->next.outcome == Outcome::Ok);
    CHECK_EQ(idsOf(p->related), std::string("fiRelated01,fiRelated02"));
    CHECK_EQ(p->seedsExcluded, 1);
    CHECK_EQ(p->relatedContinuation, std::string("FI-RELATED-2"));
    CHECK_EQ(p->info.title, std::string("Fault seed video"));
    CHECK(st.counters.unknownRenderers().count("futureRelatedViewModel") == 1);
    CHECK_EQ(st.counters.driftTotal(), std::int64_t(0));
}

void relatedPageFailureKeepsRows() {
    Stack st;
    Answer page2 = Answer::http(503);
    st.net.handler = [&page2](const HttpRequest &r) {
        if (fault::field(r, "continuation") == QLatin1String("FI-RELATED-2")) return page2;
        if (fault::endpoint(r, "next")) return Answer::ok(fixture("next_related_mixed.json"));
        return Answer::http(500);
    };
    WatchNextService service(&st.client, &st.counters);
    service.setPlayerEnabled(false);
    service.request(QStringLiteral("fiSeedVideo"));
    settledPage(service, "fiSeedVideo");
    const struct { Answer answer; Outcome outcome; } faults[] = {
        {Answer::http(503), Outcome::Http5xx},
        {Answer::ok(fixture("malformed.json")), Outcome::ShapeDrift},
        {Answer::timeout(), Outcome::Network},
    };
    for (const auto &f : faults) {
        page2 = f.answer;
        CHECK(service.loadMoreRelated(QStringLiteral("fiSeedVideo")));
        const std::shared_ptr<const WatchPage> p = settledPage(service, "fiSeedVideo");
        CHECK(p != nullptr);
        if (!p) return;
        CHECK(p->relatedPageOutcome == f.outcome);
        CHECK_EQ(p->related.size(), std::size_t(2));                       // page 1 rows stay
        CHECK_EQ(p->relatedContinuation, std::string("FI-RELATED-2"));     // a retry stays possible
    }
    page2 = Answer::ok(fixture("related_page2.json"));
    CHECK(service.loadMoreRelated(QStringLiteral("fiSeedVideo")));
    const std::shared_ptr<const WatchPage> p = settledPage(service, "fiSeedVideo");
    CHECK(p && p->relatedPageOutcome == Outcome::Ok);
    CHECK(p && idsOf(p->related) == "fiRelated01,fiRelated02,fiRelated03");
}

void watchPlayerFailureInfoSurvives() {
    Stack st;
    st.net.handler = [](const HttpRequest &r) {
        if (fault::endpoint(r, "next")) return Answer::ok(fixture("next_related_mixed.json"));
        return Answer::http(500);   // the WEB player answer for the structured fields
    };
    WatchNextService service(&st.client, &st.counters);
    service.request(QStringLiteral("fiSeedVideo"));
    const std::shared_ptr<const WatchPage> p = settledPage(service, "fiSeedVideo");
    CHECK(p != nullptr);
    if (!p) return;
    CHECK(p->playerDone && p->player.outcome == Outcome::Http5xx);
    CHECK_EQ(p->info.title, std::string("Fault seed video"));
    CHECK_EQ(p->info.ownerId, std::string("UCfaultChannel0000000001"));
    CHECK_EQ(p->related.size(), std::size_t(2));
}

void watchNextFailureNotCached() {
    const struct { Answer answer; Outcome outcome; } faults[] = {
        {Answer::http(503), Outcome::Http5xx},
        {Answer::ok(fixture("malformed.json")), Outcome::ShapeDrift},
        {Answer::network(), Outcome::Network},
    };
    for (const auto &f : faults) {
        Stack st;
        bool healthy = false;
        const Answer fault = f.answer;
        st.net.handler = [&healthy, fault](const HttpRequest &r) {
            if (!healthy) return fault;
            if (fault::endpoint(r, "next")) return Answer::ok(fixture("next_related_mixed.json"));
            return Answer::http(500);
        };
        WatchNextService service(&st.client, &st.counters);
        service.request(QStringLiteral("fiSeedVideo"));
        CHECK(catalog_test::waitFor([&service] { return !service.page(QStringLiteral("fiSeedVideo")); }, 8000));   // failures are not cached
        CHECK(st.counters.value(Area::WatchNext, f.outcome) >= 1);
        healthy = true;                                  // the next open retries and works
        CHECK(service.request(QStringLiteral("fiSeedVideo")));
        const std::shared_ptr<const WatchPage> p = settledPage(service, "fiSeedVideo");
        CHECK(p && p->related.size() == 2 && p->info.title == "Fault seed video");
    }
}

}  // namespace

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    QTemporaryDir dir;
    CHECK(dir.isValid());
    const QString settings = dir.path() + QStringLiteral("/comments.conf");

    fault::run("search_one_bad_item_keeps_healthy_rows", searchOneBadItem);
    fault::run("search_malformed_json_falls_back", [] { searchFaultFallsBack(Answer::ok(fixture("malformed.json")), Outcome::ShapeDrift); });
    fault::run("search_missing_renderer_falls_back", [] { searchFaultFallsBack(Answer::ok(fixture("search_no_renderer.json")), Outcome::ShapeDrift); });
    fault::run("search_http429_falls_back", [] { searchFaultFallsBack(Answer::http(429), Outcome::RateLimited); });
    fault::run("search_http5xx_falls_back", [] { searchFaultFallsBack(Answer::http(502), Outcome::Http5xx); });
    fault::run("search_network_error_falls_back", [] { searchFaultFallsBack(Answer::network(), Outcome::Network); });
    fault::run("search_timeout_falls_back", [] { searchFaultFallsBack(Answer::timeout(), Outcome::Network); });
    fault::run("search_without_fallback_classified", searchNoFallbackClassified);
    fault::run("search_both_tiers_fail_fixed_class", searchBothTiersFail);

    fault::run("transport_timeout_real", realTimeout);
    fault::run("transport_connection_refused_real", realConnectionRefused);
    fault::run("transport_garbage_response_real", [] { realBadServer(fault::BadServer::Mode::Garbage); });
    fault::run("transport_connection_reset_real", [] { realBadServer(fault::BadServer::Mode::Reset); });
    fault::run("transport_truncated_body_degrades", realTruncatedBody);

    fault::run("channel_tab_one_bad_item_keeps_healthy_rows", channelOneBadItem);
    fault::run("channel_tab_faults_classified", channelFaultsClassified);
    fault::run("channel_one_dead_among_several", channelOneDeadAmongSeveral);

    fault::run("feed_one_dead_channel_among_several", feedOneDeadChannel);

    fault::run("comments_one_broken_thread_among_several", commentsOneBrokenThread);
    fault::run("comments_page_faults_classified", commentsPageFaults);
    fault::run("comments_replies_failure_isolated_to_thread", [&settings] { commentsRepliesFailureIsolated(settings); });
    fault::run("comments_entry_rate_limited_inline", [&settings] { commentsEntryRateLimited(settings); });

    fault::run("related_one_bad_row_keeps_healthy_rows", relatedOneBadRow);
    fault::run("related_page_failure_keeps_rows", relatedPageFailureKeepsRows);
    fault::run("watch_player_failure_info_survives", watchPlayerFailureInfoSurvives);
    fault::run("watch_next_failure_not_cached", watchNextFailureNotCached);

    return fault::finish("fault_injection catalog");
}
