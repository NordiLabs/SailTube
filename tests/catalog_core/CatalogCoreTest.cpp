// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Catalog foundation: text tables (I18N-02/04), image rules (DSC-19), locale (I18N-01),
// client identity (DSC-02), InnerTube request shape and outcome classes (DSC-01, DIA-01 input),
// and the shape-based renderer parser (DSC-03) on recorded fixtures (NFR-TST-01).
#include "tests/catalog_common/TestSupport.h"

#include "Core/Catalog/Domain/AvatarBook.h"
#include "Core/Catalog/Domain/Images.h"
#include "Core/Catalog/Domain/ThumbnailBook.h"
#include "Core/Catalog/Domain/Outcome.h"
#include "Core/Catalog/Domain/RowFilter.h"
#include "Core/Catalog/Domain/Video.h"
#include "Core/Catalog/Domain/TextParse.h"
#include "Core/Catalog/Infrastructure/ClientIdentity.h"
#include "Core/Catalog/Infrastructure/InnerTubeClient.h"
#include "Core/Catalog/Infrastructure/Json.h"
#include "Core/Catalog/Infrastructure/RendererParser.h"

#include <QDir>
#include <QJsonArray>
#include <QTemporaryDir>
#include <QThread>
#include <QPointer>
#include <QTimer>

using namespace nordtile::video;
using namespace nordtile::video::catalog;
using catalog_test::FakeTransport;

static void textTables() {
    CHECK_EQ(parseDurationText("14:54"), 894);
    CHECK_EQ(parseDurationText("1:05:56"), 3956);
    CHECK_EQ(parseDurationText("0:59"), 59);
    CHECK_EQ(parseDurationText("14 minutes, 54 seconds"), 894);
    CHECK_EQ(parseDurationText("LIVE"), -1);
    CHECK_EQ(parseDurationText("27 videos"), -1);

    CHECK_EQ(parseCountText("7,898,556 views", "en"), 7898556);
    CHECK_EQ(parseCountText("7.8M views", "en"), 7800000);
    CHECK_EQ(parseCountText("3.6M", "en"), 3600000);
    CHECK_EQ(parseCountText("694K views", "en"), 694000);
    CHECK_EQ(parseCountText("No views", "en"), 0);
    CHECK_EQ(parseCountText("1 view", "en"), 1);
    CHECK_EQ(parseCountText("84 watching", "en"), 84);
    CHECK_EQ(parseCountText("11\xC2\xA0Mio.", "de"), 11000000);
    CHECK_EQ(parseCountText("1,2\xC2\xA0Mio. Aufrufe", "de"), 1200000);
    CHECK_EQ(parseCountText("37.519 Aufrufe", "de"), 37519);
    CHECK_EQ(parseCountText("363\xE4\xB8\x87", "ja"), 3630000);          // 363万
    CHECK_EQ(parseCountText("1162\xE4\xB8\x87 \xE5\x9B\x9E\xE8\xA6\x96\xE8\x81\xB4", "ja"), 11620000);  // 1162万 回視聴
    CHECK_EQ(parseCountText("1,2\xC2\xA0M de visualizaciones", "es"), 1200000);
    CHECK_EQ(parseCountText("abc", "en"), -1);

    CHECK_EQ(parseAgeSeconds("3 weeks ago", "en"), 3 * 7 * 86400);
    CHECK_EQ(parseAgeSeconds("1y ago", "en"), 365 * 86400);
    CHECK_EQ(parseAgeSeconds("3d ago", "en"), 3 * 86400);
    CHECK_EQ(parseAgeSeconds("1mo ago", "en"), 30 * 86400);
    CHECK_EQ(parseAgeSeconds("Streamed 8 hours ago", "en"), 8 * 3600);
    CHECK_EQ(parseAgeSeconds("vor 2 Jahren", "de"), 2 * 365 * 86400);
    CHECK_EQ(parseAgeSeconds("vor 1 Jahr", "de"), 365 * 86400);
    CHECK_EQ(parseAgeSeconds("1 \xE5\xB9\xB4\xE5\x89\x8D", "ja"), 365 * 86400);   // 1 年前
    CHECK_EQ(parseAgeSeconds("hace 3 d\xC3\xAD" "as", "es"), 3 * 86400);
    CHECK_EQ(parseAgeSeconds("14:54", "en"), -1);
    CHECK_EQ(parseAgeSeconds("7.8M views", "en"), -1);
    // Process locale independence: the tables are keyed by hl only.
    CHECK_EQ(parseAgeSeconds("2 days ago", "de"), 2 * 86400);

    CHECK_EQ(parseAbsoluteDate("Oct 24, 2009", "en"), 1256342400);
    CHECK_EQ(parseAbsoluteDate("24.10.2009", "de"), 1256342400);
    CHECK_EQ(parseAbsoluteDate("2009/10/24", "ja"), 1256342400);
    CHECK_EQ(parseAbsoluteDate("2009-10-24", "en"), 1256342400);
    CHECK_EQ(parseAbsoluteDate("24 oct 2009", "es"), 1256342400);

    CHECK(classifyMetaPart("694K views", "en") == MetaPart::Views);
    CHECK(classifyMetaPart("3.6M", "en") == MetaPart::Views);
    CHECK(classifyMetaPart("1y ago", "en") == MetaPart::Age);
    CHECK(classifyMetaPart("84 watching", "en") == MetaPart::Watching);
    CHECK(classifyMetaPart("Kurzgesagt \xE2\x80\x93 In a Nutshell", "en") == MetaPart::Unknown);
    CHECK(classifyMetaPart("vor 2 Jahren", "de") == MetaPart::Age);
    CHECK(classifyMetaPart("11\xC2\xA0Mio.", "de") == MetaPart::Views);
    CHECK(classifyMetaPart("1.2M subscribers", "en") == MetaPart::Subscribers);
    CHECK_EQ(baseLanguage("pt-BR"), std::string("pt"));
    CHECK_EQ(baseLanguage("zh_CN"), std::string("zh"));
}

static void images() {
    CHECK_EQ(listThumbnailUrl("5_Fayfx2NUM"), std::string("https://i.ytimg.com/vi/5_Fayfx2NUM/mqdefault.jpg"));
    CHECK_EQ(detailThumbnailUrl("5_Fayfx2NUM"), std::string("https://i.ytimg.com/vi/5_Fayfx2NUM/hqdefault.jpg"));
    CHECK_EQ(rewriteAvatarUrl("//yt3.ggpht.com/ytc/abc=s68-c-k-c0x00ffffff-no-rj"),
             std::string("https://yt3.ggpht.com/ytc/abc=s176-c-k-c0x00ffffff-no-rj"));
    CHECK_EQ(rewriteAvatarUrl("https://yt3.ggpht.com/abc=s88"), std::string("https://yt3.ggpht.com/abc=s176"));
    CHECK_EQ(rewriteAvatarUrl("https://yt3.ggpht.com/a/s88-c-k-no/photo.jpg"), std::string("https://yt3.ggpht.com/a/s176-c-k-no/photo.jpg"));
    CHECK_EQ(chooseAvatar({{"https://a/x=s48", 48, 48}, {"https://a/y=s900", 900, 900}, {"https://a/z=s200", 200, 200}}),
             std::string("https://a/z=s176"));
    CHECK_EQ(chooseAvatar({{"https://a/banner=s1060", 1060, 175}}), std::string());        // not square
    CHECK_EQ(chooseAvatar({{"https://a/x=s88", 88, 89}, {"https://a/y=s48", 48, 48}}), std::string("https://a/x=s176"));
}

static void locale() {
    CHECK_EQ(DiscoveryLocale::fromLocale(QLocale(QStringLiteral("de_DE"))).hl, QStringLiteral("de"));
    CHECK_EQ(DiscoveryLocale::fromLocale(QLocale(QStringLiteral("de_DE"))).gl, QStringLiteral("DE"));
    CHECK_EQ(DiscoveryLocale::fromLocale(QLocale(QStringLiteral("pt_PT"))).hl, QStringLiteral("pt-PT"));
    CHECK_EQ(DiscoveryLocale::fromLocale(QLocale(QStringLiteral("pt_BR"))).hl, QStringLiteral("pt"));
    CHECK_EQ(DiscoveryLocale::fromLocale(QLocale(QStringLiteral("zh_CN"))).hl, QStringLiteral("zh-CN"));
    CHECK_EQ(DiscoveryLocale::fromLocale(QLocale(QStringLiteral("es_MX"))).hl, QStringLiteral("es-419"));
    CHECK_EQ(DiscoveryLocale::fromLocale(QLocale(QStringLiteral("ja_JP"))).gl, QStringLiteral("JP"));
    CHECK_EQ(DiscoveryLocale::fromLocale(QLocale::c()).hl, QStringLiteral("en"));
    CHECK_EQ(DiscoveryLocale::fromLocale(QLocale::c()).gl, QStringLiteral("US"));
}

static void identity() {
    CHECK(ClientIdentity::plausibleVersion(QStringLiteral("2.20261105.05.01")));
    CHECK(ClientIdentity::plausibleVersion(QStringLiteral("2.20261105")));
    CHECK(!ClientIdentity::plausibleVersion(QStringLiteral("2.2026110")));
    CHECK(!ClientIdentity::plausibleVersion(QStringLiteral("garbage")));
    CHECK(!ClientIdentity::plausibleVersion(QStringLiteral("2.20261340.01.00")));
    CHECK_EQ(ClientIdentity::extractVersion("x\"INNERTUBE_CLIENT_VERSION\":\"2.20261201.01.00\",y"), QStringLiteral("2.20261201.01.00"));

    QTemporaryDir dir;
    const QString cache = dir.path() + QStringLiteral("/client.json");
    qint64 now = 1000LL * 3600 * 1000;
    FakeTransport net;
    net.on(QStringLiteral("https://www.youtube.com/"), QByteArray(), 200, "<script>ytcfg.set({\"INNERTUBE_CLIENT_VERSION\":\"2.20261201.01.00\"})</script>");
    {
        ClientIdentity id(&net, cache, nullptr, [&now] { return now; });
        id.start();                                      // no cache: scrape in the background
        CHECK_EQ(id.clientVersion(), QString::fromLatin1(ClientIdentity::builtInVersion()));   // never blocks
        CHECK(catalog_test::waitFor([&] { return !id.scrapeInFlight(); }));
        CHECK_EQ(id.clientVersion(), QStringLiteral("2.20261201.01.00"));
        CHECK_EQ(net.count(QStringLiteral("https://www.youtube.com/")), 1);
        bool cookie = false;
        for (const auto &h : net.requests.last().headers) if (h.first == "Cookie" && h.second == "SOCS=CAI") cookie = true;
        CHECK(cookie);
    }
    {
        now += 11LL * 3600 * 1000;                       // 11 h later: the disk value is fresh
        ClientIdentity id(&net, cache, nullptr, [&now] { return now; });
        id.start();
        CHECK_EQ(id.source(), QStringLiteral("cache"));
        CHECK_EQ(id.clientVersion(), QStringLiteral("2.20261201.01.00"));
        CHECK_EQ(net.count(QStringLiteral("https://www.youtube.com/")), 1);
        id.reportHttp400();                              // 400: background re-scrape
        CHECK(catalog_test::waitFor([&] { return !id.scrapeInFlight(); }));
        CHECK_EQ(net.count(QStringLiteral("https://www.youtube.com/")), 2);
        now += 5LL * 60 * 1000;
        id.reportHttp400();                              // within 10 min: rate limited
        CHECK_EQ(net.count(QStringLiteral("https://www.youtube.com/")), 2);
        now += 6LL * 60 * 1000;
        id.reportHttp400();
        CHECK(catalog_test::waitFor([&] { return !id.scrapeInFlight(); }));
        CHECK_EQ(net.count(QStringLiteral("https://www.youtube.com/")), 3);
    }
    {
        now += 13LL * 3600 * 1000;                       // stale after 12 h
        FakeTransport bad;
        bad.on(QStringLiteral("https://www.youtube.com/"), QByteArray(), 200, "\"INNERTUBE_CLIENT_VERSION\":\"not-a-version\"");
        ClientIdentity id(&bad, cache, nullptr, [&now] { return now; });
        id.start();
        CHECK(catalog_test::waitFor([&] { return !id.scrapeInFlight(); }));
        CHECK_EQ(bad.count(QStringLiteral("https://www.youtube.com/")), 1);
        CHECK_EQ(id.clientVersion(), QStringLiteral("2.20261201.01.00"));   // implausible value not committed
    }
}

static void innerTube() {
    FakeTransport net;
    ClientIdentity id(nullptr, QString());
    WorkLane lane(QStringLiteral("test"), 1);
    DiscoveryLocale loc; loc.hl = QStringLiteral("de"); loc.gl = QStringLiteral("DE");
    InnerTubeClient client(&net, &id, &lane, loc);
    QJsonObject body; body.insert(QStringLiteral("query"), QStringLiteral("x"));
    const HttpRequest req = client.buildRequest(QStringLiteral("search"), body, 10000);
    CHECK_EQ(req.url.toString(), QStringLiteral("https://www.youtube.com/youtubei/v1/search?prettyPrint=false"));
    CHECK(!req.url.toString().contains(QLatin1String("key=")));
    CHECK_EQ(req.method, QByteArray("POST"));
    const QJsonObject sent = QJsonDocument::fromJson(req.body).object();
    CHECK_EQ(json::at(sent, QStringLiteral("context.client.clientName")).toString(), QStringLiteral("WEB"));
    CHECK_EQ(json::at(sent, QStringLiteral("context.client.hl")).toString(), QStringLiteral("de"));
    CHECK_EQ(json::at(sent, QStringLiteral("context.client.gl")).toString(), QStringLiteral("DE"));
    CHECK(!json::at(sent, QStringLiteral("context.client.visitorData")).isString());
    bool cookie = false, name = false;
    for (const auto &h : req.headers) { if (h.first == "Cookie") cookie = true; if (h.first == "X-YouTube-Client-Name" && h.second == "1") name = true; }
    CHECK(!cookie);
    CHECK(name);

    struct Case { int status; QByteArray body; Outcome expected; };
    const QList<Case> cases = {
        {200, "{\"contents\":{}}", Outcome::Ok}, {200, "<html>", Outcome::ShapeDrift}, {404, "", Outcome::Http4xx},
        {429, "", Outcome::RateLimited}, {503, "", Outcome::Http5xx}, {0, "", Outcome::Network}};
    for (const Case &c : cases) {
        FakeTransport n;
        if (c.status) n.on(QStringLiteral("youtubei"), QByteArray(), c.status, c.body);
        InnerTubeClient cl(&n, &id, &lane, loc);
        bool got = false; Outcome outcome = Outcome::Ok; bool offMain = false;
        QThread *main = QThread::currentThread();
        cl.post<Outcome>(QStringLiteral("browse"), QJsonObject(), &n,
            [main, &offMain](const InnerTubeReply &r) { offMain = QThread::currentThread() != main; return r.outcome; },
            [&](Outcome o) { outcome = o; got = true; });
        CHECK(catalog_test::waitFor([&] { return got; }));
        CHECK(outcome == c.expected);
        CHECK(offMain);                                  // parsing ran on the lane
    }
    // 400 asks the identity for a background re-scrape (identity without transport: no-op).
    CHECK(!countsAsDrift(Outcome::RateLimited) && !countsAsDrift(Outcome::Http5xx) && countsAsDrift(Outcome::ShapeDrift));
    CHECK(outcomeForHttp(429) == Outcome::RateLimited && outcomeForHttp(500) == Outcome::Http5xx && outcomeForHttp(0) == Outcome::Network);
}

static int countKind(const ParsedItems &p, ItemKind k) {
    int n = 0;
    for (const Item &i : p.items) if (i.kind == k) ++n;
    return n;
}

static void renderers() {
    // Search page 1: videoRenderer rows with structured-looking text, Shorts from
    // shortsLockupViewModel, a continuation.
    {
        RendererParser p(QStringLiteral("en"));
        const ParsedItems r = p.parse(catalog_test::fixtureJson(QStringLiteral("search_en_p1.json")));
        CHECK(countKind(r, ItemKind::Video) >= 10);
        CHECK(countKind(r, ItemKind::Short) >= 5);
        CHECK(!r.continuation.isEmpty());
        int withViews = 0, withDuration = 0, withAge = 0, withChannelId = 0;
        for (const Item &i : r.items) {
            CHECK(!i.title.empty());
            if (isVideoLike(i)) CHECK(validVideoId(i.id));
            if (i.kind == ItemKind::Channel) CHECK(validChannelId(i.id));
            if (i.kind != ItemKind::Video) continue;
            if (i.viewCount > 0) ++withViews;
            if (i.durationSeconds > 0) ++withDuration;
            if (i.publishedAgeSeconds > 0) ++withAge;
            if (validChannelId(i.channelId)) ++withChannelId;
            CHECK_EQ(i.thumbnailUrl, listThumbnailUrl(i.id));
        }
        CHECK(withViews >= 10 && withDuration >= 10 && withAge >= 10 && withChannelId >= 10);
        for (const Item &i : r.items) if (i.kind == ItemKind::Short) CHECK(i.shortEvidence == ShortEvidence::Renderer);
    }
    // The same query in German and Japanese parses to comparable rows (I18N-04).
    for (const char *f : {"search_de_p1.json", "search_ja_p1.json"}) {
        const QString hl = QString::fromLatin1(f).mid(7, 2);
        RendererParser p(hl);
        const ParsedItems r = p.parse(catalog_test::fixtureJson(QString::fromLatin1(f)));
        int ok = 0, total = 0;
        for (const Item &i : r.items) if (i.kind == ItemKind::Video) { ++total; if (i.viewCount > 0 && i.publishedAgeSeconds > 0) ++ok; }
        CHECK(total >= 10);
        CHECK(ok * 10 >= total * 8);                     // at least 80 % fully parsed, none dropped
    }
    // Channel Videos tab: lockupViewModel rows without owner, durations from badges.
    {
        RendererParser p(QStringLiteral("en"));
        const ParsedItems r = p.parse(catalog_test::fixtureJson(QStringLiteral("channel_videos.json")));
        CHECK_EQ(countKind(r, ItemKind::Video), 30);
        CHECK(!r.continuation.isEmpty());
        int good = 0;
        for (const Item &i : r.items) if (i.durationSeconds > 0 && i.viewCount >= 0 && i.publishedAgeSeconds >= 0) ++good;
        CHECK(good >= 28);
    }
    for (const char *f : {"channel_videos_de.json", "channel_videos_ja.json"}) {
        RendererParser p(QString::fromLatin1(f).mid(15, 2));
        const ParsedItems r = p.parse(catalog_test::fixtureJson(QString::fromLatin1(f)));
        int good = 0;
        for (const Item &i : r.items) if (i.durationSeconds > 0 && i.viewCount >= 0 && i.publishedAgeSeconds >= 0) ++good;
        CHECK(r.items.size() == 30);
        CHECK(good >= 28);
    }
    // Live tab: running streams carry the LIVE badge style and "watching" counts.
    {
        RendererParser p(QStringLiteral("en"));
        const ParsedItems r = p.parse(catalog_test::fixtureJson(QStringLiteral("channel_live.json")));
        int live = 0;
        for (const Item &i : r.items) if (i.liveStatus == media::LiveStatus::IsLive) { ++live; CHECK(i.viewCount > 0); }
        CHECK(live >= 1);
    }
    // Shorts tab.
    {
        RendererParser p(QStringLiteral("en"));
        const ParsedItems r = p.parse(catalog_test::fixtureJson(QStringLiteral("channel_shorts.json")));
        CHECK(countKind(r, ItemKind::Short) >= 20);   // page 1 of this tab carries no continuation
    }
    // Playlists (lockup PLAYLIST) and channels (channelRenderer) from filtered searches.
    {
        RendererParser p(QStringLiteral("en"));
        const ParsedItems pl = p.parse(catalog_test::fixtureJson(QStringLiteral("search_filter_playlist.json")));
        CHECK(countKind(pl, ItemKind::Playlist) >= 5);
        for (const Item &i : pl.items) if (i.kind == ItemKind::Playlist) { CHECK(i.videoCount > 0); CHECK(!i.thumbnailUrl.empty()); }
        const ParsedItems ch = p.parse(catalog_test::fixtureJson(QStringLiteral("search_filter_channel.json")));
        CHECK(countKind(ch, ItemKind::Channel) >= 3);
        int subs = 0;
        for (const Item &i : ch.items) if (i.kind == ItemKind::Channel) { CHECK(validChannelId(i.id)); if (i.subscriberCount > 0) ++subs; }
        CHECK(subs >= 1);
        const ParsedItems pb = p.parse(catalog_test::fixtureJson(QStringLiteral("playlist_browse.json")));
        CHECK(countKind(pb, ItemKind::Video) >= 3);       // the recorded list holds 3 videos
        CHECK(!pb.continuation.isEmpty());               // continuationItemViewModel holds the token
        CHECK_EQ(pb.unknown.value(QStringLiteral("continuationItemViewModel")), 0);
        const ParsedItems cp = p.parse(catalog_test::fixtureJson(QStringLiteral("channel_playlists.json")));
        CHECK(countKind(cp, ItemKind::Playlist) >= 5);
    }
    // Related lockups on watch-next: owner, compact views and ages.
    {
        RendererParser p(QStringLiteral("en"));
        const QJsonObject next = catalog_test::fixtureJson(QStringLiteral("next_en.json"));
        const ParsedItems r = p.parse(json::findFirst(next, QStringLiteral("secondaryResults")));
        CHECK(r.items.size() >= 15);
        int good = 0;
        for (const Item &i : r.items) if (!i.channel.empty() && validChannelId(i.channelId) && i.viewCount > 0 && i.publishedAgeSeconds > 0) ++good;
        CHECK(good * 10 >= static_cast<int>(r.items.size()) * 8);
        CHECK(!r.continuation.isEmpty());
    }
    // Unknown renderers are skipped and counted; garbage never crashes.
    {
        QJsonObject unknown; unknown.insert(QStringLiteral("futureRowViewModel"), QJsonObject());
        QJsonObject video; video.insert(QStringLiteral("videoRenderer"), QJsonObject{{QStringLiteral("videoId"), QStringLiteral("5_Fayfx2NUM")},
                                                                                  {QStringLiteral("title"), QJsonObject{{QStringLiteral("simpleText"), QStringLiteral("t")}}}});
        QJsonObject list; list.insert(QStringLiteral("contents"), QJsonArray{unknown, video, unknown, QJsonValue(42), QJsonValue()});
        RendererParser p;
        const ParsedItems r = p.parse(list);
        CHECK_EQ(static_cast<int>(r.items.size()), 1);
        CHECK_EQ(r.unknown.value(QStringLiteral("futureRowViewModel")), 2);
        QJsonObject broken; broken.insert(QStringLiteral("contents"), QJsonArray{QJsonObject{{QStringLiteral("videoRenderer"), QJsonObject()}}});
        const ParsedItems b = p.parse(broken);
        CHECK(b.items.empty());
        CHECK_EQ(b.unparsed, 1);
    }
}

static Item row(const char *id, std::int64_t seconds, ShortEvidence e = ShortEvidence::None) {
    Item i; i.id = id; i.title = id; i.durationSeconds = seconds; i.shortEvidence = e;
    i.kind = isConfirmedShort(e) ? ItemKind::Short : ItemKind::Video;
    return i;
}

static void rowFilter() {
    // DSC-17: a 2:50 Short (renderer evidence) and a 2:50 regular video.
    const std::vector<Item> rows = {row("shortAAAAAA", 170, ShortEvidence::Renderer), row("regularAAAA", 170),
                                    row("longAAAAAAA", 600), row("rssShortAAA", -1, ShortEvidence::RssLink)};
    RowFilterSettings hide; hide.hideShorts = true;
    RowFilterResult r = filterRows(rows, hide);
    CHECK_EQ(static_cast<int>(r.visible.size()), 2);
    CHECK_EQ(r.hiddenShorts, 2);
    CHECK_EQ(r.visible[0].id, std::string("regularAAAA"));
    CHECK(r.visible[0].shortEvidence == ShortEvidence::Candidate);      // candidate only, still shown
    // The store settles a candidate: the Shorts tab listed it.
    ShortsLookup store = [](const std::string &id) { return id == "regularAAAA" ? ShortEvidence::NotShort : ShortEvidence::None; };
    r = filterRows(rows, hide, nullptr, store);
    CHECK(r.visible[0].shortEvidence == ShortEvidence::NotShort);
    store = [](const std::string &id) { return id == "regularAAAA" ? ShortEvidence::ShortsTab : ShortEvidence::None; };
    r = filterRows(rows, hide, nullptr, store);
    CHECK_EQ(r.hiddenShorts, 3);
    // DSC-18: members-only rows are counted when hidden, kept (badged) otherwise.
    std::vector<Item> mixed = rows;
    mixed[2].membersOnly = true;
    RowFilterSettings members; members.hideMembersOnly = true;
    r = filterRows(mixed, members);
    CHECK_EQ(r.hiddenMembersOnly, 1);
    CHECK_EQ(static_cast<int>(r.visible.size()), 3);
    r = filterRows(mixed, RowFilterSettings());
    CHECK_EQ(static_cast<int>(r.visible.size()), 4);
    CHECK(r.visible[2].membersOnly);
    // Duplicates across pages.
    std::set<std::string> seen;
    filterRows(rows, RowFilterSettings(), &seen);
    r = filterRows(rows, RowFilterSettings(), &seen);
    CHECK_EQ(r.duplicates, 4);
    CHECK(r.visible.empty());
}

// NFR-ROB-01 / DIA-01: a 200 whose body was cut off is a network failure, sent once more, and
// never counted as shape drift.
namespace {
class TruncatingTransport final : public QObject, public HttpTransport {
public:
    int truncations = 0;   // how many leading replies come back cut off
    int sends = 0;
    QByteArray good = "{\"responseContext\":{},\"contents\":{}}";
    quint64 send(const HttpRequest &, QObject *context, HttpCallback done) override {
        ++sends;
        HttpReply r;
        if (sends <= truncations) { r.status = 0; r.truncated = true; r.body = "{\"contents\":{\"twoCol"; r.errorText = QStringLiteral("truncated body"); }
        else { r.status = 200; r.body = good; }
        QPointer<QObject> guard(context);
        QTimer::singleShot(0, this, [done, r, guard] { if (guard) done(r); });
        return static_cast<quint64>(sends);
    }
    void cancel(quint64) override {}
    QStringList contactedHosts() const override { return QStringList(); }
};
}

static void truncatedBody() {
    // The classification itself: a truncated reply is Network, a complete garbage 200 is drift.
    HttpReply cut;
    cut.status = 0;
    cut.truncated = true;
    cut.body = "{\"contents\":{\"twoCol";
    CHECK(InnerTubeClient::toReply(cut).outcome == Outcome::Network);
    HttpReply garbage;
    garbage.status = 200;
    garbage.body = "{\"contents\":{\"twoCol";
    CHECK(InnerTubeClient::toReply(garbage).outcome == Outcome::ShapeDrift);

    ClientIdentity identity(nullptr, QString());
    WorkLane lane(QStringLiteral("truncation"), 1);
    QObject context;
    {
        // Cut off once: the retry answers, the caller sees Ok.
        TruncatingTransport net;
        net.truncations = 1;
        InnerTubeClient client(&net, &identity, &lane, DiscoveryLocale());
        Outcome got = Outcome::Unavailable;
        bool done = false;
        std::function<Outcome(const InnerTubeReply &)> parse = [](const InnerTubeReply &r) { return r.outcome; };
        std::function<void(Outcome)> finish = [&](Outcome o) { got = o; done = true; };
        client.post<Outcome>(QLatin1String(innertube::kEndpointSearch), QJsonObject(), &context, parse, finish);
        CHECK(catalog_test::waitFor([&] { return done; }));
        CHECK(got == Outcome::Ok);
        CHECK_EQ(net.sends, 2);
    }
    {
        // Cut off twice: a network failure (retried once only), the drift counter stays at 0.
        TruncatingTransport net;
        net.truncations = 5;
        InnerTubeClient client(&net, &identity, &lane, DiscoveryLocale());
        CatalogCounters counters;
        Outcome got = Outcome::Unavailable;
        bool done = false;
        std::function<Outcome(const InnerTubeReply &)> parse = [](const InnerTubeReply &r) { return r.outcome; };
        std::function<void(Outcome)> finish = [&](Outcome o) { got = o; counters.count(Area::Search, o); done = true; };
        client.post<Outcome>(QLatin1String(innertube::kEndpointSearch), QJsonObject(), &context, parse, finish);
        CHECK(catalog_test::waitFor([&] { return done; }));
        CHECK(got == Outcome::Network);
        CHECK_EQ(net.sends, 2);
        CHECK_EQ(counters.driftTotal(), static_cast<std::int64_t>(0));
        CHECK_EQ(counters.value(Area::Search, Outcome::Network), static_cast<std::int64_t>(1));
    }
}

// Channel avatars by id (AvatarBook): filled from rows that carry an owner avatar and from
// channel browse answers, read by rows that carry none. No request is involved.
static void avatarBook() {
    AvatarBook &book = AvatarBook::instance();
    book.clear();
    const std::string nasa = "UCLA_DiR1FfKNvjuUpBHmylQ";
    CHECK(AvatarBook::acceptable(nasa, "https://yt3.ggpht.com/a=s176"));
    CHECK(!AvatarBook::acceptable("PLx", "https://yt3.ggpht.com/a=s176"));           // not a channel id
    CHECK(!AvatarBook::acceptable(nasa, "http://yt3.ggpht.com/a=s176"));              // https only
    CHECK(!AvatarBook::acceptable(nasa, std::string()));
    int notified = 0;
    book.setListener([&notified] { ++notified; });
    const std::uint64_t before = book.revision();
    CHECK(book.learn(nasa, "https://yt3.ggpht.com/a=s176"));
    CHECK(!book.learn(nasa, "https://yt3.ggpht.com/a=s176"));                         // unchanged
    CHECK(!book.learn("bad", "https://yt3.ggpht.com/a=s176"));
    CHECK_EQ(book.lookup(nasa), std::string("https://yt3.ggpht.com/a=s176"));
    CHECK(book.lookup("UCnone0000000000000000000").empty());
    CHECK(book.lookup(std::string()).empty());
    CHECK(!book.learn(nasa, "https://yt3.ggpht.com/b=s176"));                         // the first address stays
    CHECK_EQ(book.lookup(nasa), std::string("https://yt3.ggpht.com/a=s176"));
    CHECK(book.learn(nasa, "https://yt3.ggpht.com/b=s176", true));                    // the channel's own page replaces it
    CHECK_EQ(book.lookup(nasa), std::string("https://yt3.ggpht.com/b=s176"));
    CHECK_EQ(static_cast<int>(book.size()), 1);
    CHECK_EQ(notified, 2);
    CHECK(book.revision() > before);
    // Bounded: the oldest entries leave first.
    for (std::size_t i = 0; i < AvatarBook::kCapacity + 10; ++i) {
        char id[32];
        std::snprintf(id, sizeof id, "UC%022zu", i);
        book.learn(id, "https://yt3.ggpht.com/x=s176");
    }
    CHECK_EQ(book.size(), AvatarBook::kCapacity);
    CHECK(book.lookup(nasa).empty());
    book.setListener(std::function<void()>());
    book.clear();
    CHECK_EQ(static_cast<int>(book.size()), 0);

    // Parsed rows teach it: search rows carry the owner's avatar.
    const RendererParser parser(QStringLiteral("en"));
    const ParsedItems search = parser.parse(catalog_test::fixtureJson(QStringLiteral("search_en_p1.json")));
    int withAvatar = 0;
    for (const Item &it : search.items) {
        if (it.avatarUrl.empty()) continue;
        ++withAvatar;
        // Known for the row's channel (the first address seen for a channel stays).
        CHECK(book.lookup(it.kind == ItemKind::Channel ? it.id : it.channelId).compare(0, 8, "https://") == 0);
    }
    CHECK(withAvatar >= 10);
    CHECK(book.size() > 0);
    // A channel browse answer names the channel's avatar; other answers do not.
    book.clear();
    CHECK(RendererParser::learnChannelAvatar(catalog_test::fixtureJson(QStringLiteral("channel_videos.json"))));
    const std::string learned = book.lookup(nasa);
    CHECK(learned.compare(0, 8, "https://") == 0);
    CHECK(learned.find("=s176") != std::string::npos);                                 // the 176 px size (DSC-19)
    CHECK(!RendererParser::learnChannelAvatar(catalog_test::fixtureJson(QStringLiteral("search_en_p1.json"))));
    book.clear();
}

// thumbnailLarge: the 1280x720 picture (hq720) unless the video is known to have none
// (ThumbnailBook), then the 480x360 one that exists for every video.
static void thumbnailBook() {
    ThumbnailBook &book = ThumbnailBook::instance();
    book.clear();
    CHECK_EQ(largeThumbnailUrl("9zCH37330f8"), std::string("https://i.ytimg.com/vi/9zCH37330f8/hq720.jpg"));
    CHECK(largeThumbnailUrl(std::string()).empty());
    CHECK_EQ(thumbnailVideoId(listThumbnailUrl("9zCH37330f8")), std::string("9zCH37330f8"));
    CHECK_EQ(thumbnailVideoId(detailThumbnailUrl("9zCH37330f8")), std::string("9zCH37330f8"));
    CHECK_EQ(thumbnailVideoId(largeThumbnailUrl("9zCH37330f8")), std::string("9zCH37330f8"));
    CHECK(thumbnailVideoId(shortsThumbnailUrl("9zCH37330f8")).empty());                // a vertical frame is not a card picture
    CHECK(thumbnailVideoId("https://yt3.ggpht.com/a=s176").empty());                   // an avatar
    CHECK(thumbnailVideoId("https://i.ytimg.com/vi/short/mqdefault.jpg").empty());
    CHECK(ThumbnailBook::isVideoId("9zCH37330f8") && !ThumbnailBook::isVideoId("9zCH37330f") && !ThumbnailBook::isVideoId("9zCH37330f/"));
    int notified = 0;
    book.setListener([&notified] { ++notified; });
    const std::uint64_t before = book.revision();
    CHECK(!book.largeMissing("9zCH37330f8"));
    CHECK(book.markLargeMissing("9zCH37330f8"));
    CHECK(!book.markLargeMissing("9zCH37330f8"));                                      // already listed
    CHECK(!book.markLargeMissing("not an id"));
    CHECK(book.largeMissing("9zCH37330f8"));
    CHECK_EQ(notified, 1);
    CHECK(book.revision() > before);
    CHECK(book.missing() == std::vector<std::string>(1, "9zCH37330f8"));
    for (std::size_t i = 0; i < ThumbnailBook::kCapacity + 5; ++i) {
        char id[16];
        std::snprintf(id, sizeof id, "v%010zu", i);
        book.markLargeMissing(id);
    }
    CHECK_EQ(book.size(), ThumbnailBook::kCapacity);
    CHECK(!book.largeMissing("9zCH37330f8"));                                          // the oldest left
    book.setListener(std::function<void()>());
    book.clear();

    // An answer whose rows name hq720 names only smaller pictures for a video without one.
    const RendererParser parser(QStringLiteral("es"));
    const ParsedItems home = parser.parse(catalog_test::fixtureJson(QStringLiteral("home_popular_es.json")));
    int large = 0, small = 0;
    for (const Item &it : home.items) {
        if (it.kind != ItemKind::Video) continue;
        if (it.largeThumbnailListed > 0) { ++large; CHECK(!book.largeMissing(it.id)); }
        if (it.largeThumbnailListed < 0) { ++small; CHECK(book.largeMissing(it.id)); }
    }
    CHECK(large >= 15);
    CHECK_EQ(small, static_cast<int>(book.size()));
    CHECK(small >= 1);
    // An answer that names no hq720 at all (a playlist) says nothing: nobody is listed.
    book.clear();
    parser.parse(catalog_test::fixtureJson(QStringLiteral("playlist_browse.json")));
    CHECK_EQ(static_cast<int>(book.size()), 0);
    book.clear();
}

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    textTables();
    images();
    locale();
    identity();
    innerTube();
    renderers();
    rowFilter();
    truncatedBody();
    avatarBook();
    thumbnailBook();
    return catalog_test::finish("catalog_core");
}
