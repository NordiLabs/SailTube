// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Subscriptions feed and home (DSC-04/05/06/07) on recorded fixtures (NFR-TST-01): RSS parsing,
// the refresh schedule, merge order with live pinning, id-sequence replacement, persistence
// from a copy, tier fallbacks with one dead channel, the 10-channel cap, depth through
// continuations, and valid-empty home versus drift.
//
// Derived fixtures are built in code from the recorded ones: video ids rewritten per channel
// and page (so many channels and pages stay distinct), RSS entries trimmed or re-dated, a tab
// list with altered Videos-tab params, and containers removed to simulate drift.
#include "tests/catalog_common/TestSupport.h"
#include "Clients/Sailfish/Presentation/LargeThumbnails.h"
#include "Core/Catalog/Domain/ThumbnailBook.h"
#include "Clients/Sailfish/Presentation/ChannelAvatars.h"
#include "Core/Catalog/Domain/AvatarBook.h"

#include "Core/Catalog/Domain/FeedSchedule.h"
#include "Core/Catalog/Domain/Subscriptions.h"
#include "Core/Catalog/Infrastructure/ClientIdentity.h"
#include "Core/Catalog/Infrastructure/FeedCacheStore.h"
#include "Core/Catalog/Infrastructure/FeedService.h"
#include "Core/Catalog/Infrastructure/HomeService.h"
#include "Core/Catalog/Infrastructure/InnerTubeClient.h"
#include "Core/Catalog/Infrastructure/Json.h"
#include "Core/Catalog/Infrastructure/RssParser.h"
#include "Core/Catalog/Infrastructure/WorkLane.h"
#include "Clients/Sailfish/Presentation/ContentFilters.h"
#include "Clients/Sailfish/Presentation/FeedModel.h"
#include "Clients/Sailfish/Presentation/HomeSettings.h"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QRegularExpression>
#include <QTemporaryDir>
#include <QUrlQuery>
#include <map>
#include <memory>
#include <set>

using namespace nordtile::video;
using namespace nordtile::video::catalog;
using media::LiveStatus;

namespace {

const std::int64_t kNow = 1794124800;   // 2026-11-08T08:00:00Z, just after the recording
const char *const kNasa = "UCLA_DiR1FfKNvjuUpBHmylQ";

std::string chan(int n) { return QStringLiteral("UCtestchannel%1").arg(n, 11, 10, QLatin1Char('0')).toStdString(); }
QString makeId(int a, int b, int c) {   // 11 characters
    return QStringLiteral("c%1p%2i%3").arg(a, 3, 10, QLatin1Char('0')).arg(b, 3, 10, QLatin1Char('0')).arg(c, 2, 10, QLatin1Char('0'));
}
std::int64_t iso(const char *text) { return QDateTime::fromString(QString::fromLatin1(text), Qt::ISODate).toMSecsSinceEpoch() / 1000; }

// Rewrites every video id matched by `re` (capture 1) to newId(index) everywhere in `body`.
QByteArray rewriteIds(QByteArray body, const QRegularExpression &re, const std::function<QString(int)> &newId) {
    QStringList ids;
    QRegularExpressionMatchIterator it = re.globalMatch(QString::fromUtf8(body));
    while (it.hasNext()) {
        const QString id = it.next().captured(1);
        if (!ids.contains(id)) ids.append(id);
    }
    for (int i = 0; i < ids.size(); ++i) body.replace(ids[i].toLatin1(), newId(i).toLatin1());
    return body;
}
const QRegularExpression &rssIdRe() { static const QRegularExpression re(QStringLiteral("<yt:videoId>([A-Za-z0-9_-]{11})</yt:videoId>")); return re; }
const QRegularExpression &jsonIdRe() { static const QRegularExpression re(QStringLiteral("\"contentId\":\"([A-Za-z0-9_-]{11})\"")); return re; }

// The NASA feed re-owned by `channel`, with ids rewritten by `tag`, the first `keep` entries.
QByteArray rssFor(const std::string &channel, int tag, int keep = -1) {
    QByteArray body = catalog_test::fixture(QStringLiteral("rss_channel.xml"));
    if (tag >= 0) body = rewriteIds(body, rssIdRe(), [tag](int i) { return makeId(tag, 0, i); });
    body.replace(kNasa, QByteArray(channel.c_str()));
    body.replace("LA_DiR1FfKNvjuUpBHmylQ", QByteArray(channel.c_str() + 2));
    if (keep >= 0) {
        int pos = -1;
        for (int i = 0; i <= keep; ++i) { pos = body.indexOf("<entry>", pos + 1); if (pos < 0) break; }
        if (pos >= 0) body = body.left(pos) + "</feed>\n";
    }
    return body;
}

QByteArray videosFor(int tag) {
    return rewriteIds(catalog_test::fixture(QStringLiteral("channel_videos.json")), jsonIdRe(), [tag](int i) { return makeId(tag, 1, i); });
}

QByteArray continuationPage(int serial) {
    return rewriteIds(catalog_test::fixture(QStringLiteral("channel_videos_p2.json")), jsonIdRe(), [serial](int i) {
        return QStringLiteral("k%1i%2").arg(serial, 6, 10, QLatin1Char('0')).arg(i, 3, 10, QLatin1Char('0'));
    });
}

InnerTubeReply replyOf(int status, const QByteArray &body) {
    HttpReply http;
    http.status = status;
    http.body = body;
    return InnerTubeClient::toReply(http);
}

Item video(const std::string &id, std::int64_t epoch = -1, std::int64_t age = -1, LiveStatus live = LiveStatus::NotLive) {
    Item it;
    it.id = id;
    it.title = "t " + id;
    it.publishedEpoch = epoch;
    it.publishedAgeSeconds = age;
    it.liveStatus = live;
    return it;
}

// A transport whose answers come from a function, with in-flight accounting.
class ScriptedTransport final : public QObject, public HttpTransport {
public:
    struct Answer {
        Answer(int s = 0, const QByteArray &b = QByteArray(), int d = 0) : status(s), body(b), delayMs(d) {}
        int status;
        QByteArray body;
        int delayMs;
    };
    std::function<Answer(const HttpRequest &)> handler;
    QList<HttpRequest> requests;
    int outstanding = 0;
    int peak = 0;

    quint64 send(const HttpRequest &request, QObject *context, HttpCallback done) override {
        requests.append(request);
        const Answer a = handler ? handler(request) : Answer();
        const quint64 id = ++next_;
        ++outstanding;
        if (outstanding > peak) peak = outstanding;
        QPointer<QObject> guard(context);
        const bool hasContext = context != nullptr;
        const QUrl url = request.url;
        QTimer::singleShot(a.delayMs, this, [this, id, a, done, guard, hasContext, url] {
            --outstanding;
            if (cancelled_.contains(id) || (hasContext && !guard)) return;
            HttpReply r;
            r.status = a.status;
            r.body = a.body;
            r.finalUrl = url;
            if (!a.status) r.errorText = QStringLiteral("offline");
            done(r);
        });
        return id;
    }
    void cancel(quint64 id) override { cancelled_.append(id); }
    QStringList contactedHosts() const override { return QStringList(); }

private:
    quint64 next_ = 0;
    QList<quint64> cancelled_;
};

struct Req {
    bool rss = false;
    bool browse = false;
    QString channel;
    QString params;
    QString continuation;
    bool videosTab() const { return browse && continuation.isEmpty() && params.startsWith(QLatin1String("EgZ2aWRlb3")); }
    bool liveTab() const { return browse && params.startsWith(QLatin1String("EgdzdHJlYW1z")); }
};

Req parseReq(const HttpRequest &r) {
    Req q;
    const QString url = r.url.toString();
    if (url.contains(QLatin1String("feeds/videos.xml"))) {
        q.rss = true;
        q.channel = QUrlQuery(r.url).queryItemValue(QStringLiteral("channel_id"));
    } else if (url.contains(QLatin1String("youtubei/v1/browse"))) {
        q.browse = true;
        const QJsonObject body = QJsonDocument::fromJson(r.body).object();
        q.channel = body.value(QStringLiteral("browseId")).toString();
        q.params = body.value(QStringLiteral("params")).toString();
        q.continuation = body.value(QStringLiteral("continuation")).toString();
    }
    return q;
}

int countReq(const ScriptedTransport &net, const std::function<bool(const Req &)> &pred) {
    int n = 0;
    for (const HttpRequest &r : net.requests) if (pred(parseReq(r))) ++n;
    return n;
}

struct Rig {
    ScriptedTransport net;
    ClientIdentity identity;
    WorkLane lane;
    InnerTubeClient client;
    std::int64_t now = kNow;
    Rig() : identity(nullptr, QString()), lane(QStringLiteral("feed-test"), 2), client(&net, &identity, &lane, DiscoveryLocale()) {}
    FeedSchedule::Clock clock() { return [this] { return now; }; }
};

bool waitIdle(FeedService &s, int ms = 8000) {
    return catalog_test::waitFor([&s] { return s.cacheLoaded() && !s.refreshing() && !s.extending(); }, ms);
}

int rowsOf(const MergedFeed &m, const std::string &channelId) {
    int n = 0;
    for (const Item &it : m.rows) if (it.channelId == channelId) ++n;
    return n;
}

}  // namespace

// --- RSS (DSC-05 tier 1) ---------------------------------------------------------------------

static void rssParse() {
    const RssFeed f = RssParser::parse(catalog_test::fixture(QStringLiteral("rss_channel.xml")), 200);
    CHECK(f.outcome == Outcome::Ok);
    CHECK_EQ(f.items.size(), std::size_t(15));
    CHECK_EQ(f.channelId, QString::fromLatin1(kNasa));
    int shorts = 0, withViews = 0;
    for (const Item &it : f.items) {
        CHECK_EQ(it.id.size(), std::size_t(11));
        CHECK(!it.title.empty());
        CHECK(it.publishedEpoch > iso("2020-01-01T00:00:00Z"));
        CHECK_EQ(it.channelId, std::string(kNasa));
        CHECK(!it.channel.empty());
        CHECK(it.thumbnailUrl.find(it.id) != std::string::npos);
        CHECK(it.liveStatus == LiveStatus::Unknown);   // RSS carries no live state
        if (it.viewCount > 0) ++withViews;
        if (it.kind == ItemKind::Short) {
            ++shorts;
            CHECK(it.shortEvidence == ShortEvidence::RssLink);
            CHECK_EQ(it.id, std::string("k5Z7sFwFF7Q"));
        }
    }
    CHECK_EQ(shorts, 1);
    CHECK(withViews >= 10);
    CHECK_EQ(f.items[0].id, std::string("MRg4uIpJ3xE"));
    CHECK_EQ(f.items[0].publishedEpoch, iso("2026-11-07T23:39:59Z"));
    CHECK_EQ(RssParser::feedUrl(QString::fromLatin1(kNasa)).toString(),
             QStringLiteral("https://www.youtube.com/feeds/videos.xml?channel_id=UCLA_DiR1FfKNvjuUpBHmylQ"));

    // A dead channel answers 404 with an HTML page: an HTTP failure, never drift.
    const RssFeed dead = RssParser::parse(catalog_test::fixture(QStringLiteral("rss_dead_channel.xml")), 404);
    CHECK(dead.outcome == Outcome::Http4xx);
    CHECK(dead.items.empty());
    // The same HTML with 200 is not a feed: drift.
    CHECK(RssParser::parse(catalog_test::fixture(QStringLiteral("rss_dead_channel.xml")), 200).outcome == Outcome::ShapeDrift);
    CHECK(RssParser::parse(QByteArray(), 0).outcome == Outcome::Network);
    // Derived: a feed without entries is valid-empty.
    const RssFeed empty = RssParser::parse(rssFor(kNasa, -1, 0), 200);
    CHECK(empty.outcome == Outcome::ValidEmpty);
    CHECK_EQ(empty.channelId, QString::fromLatin1(kNasa));
    // Derived: an entry without a video id is skipped, the rest kept.
    QByteArray broken = catalog_test::fixture(QStringLiteral("rss_channel.xml"));
    broken.replace("<yt:videoId>MRg4uIpJ3xE</yt:videoId>", "");
    const RssFeed partial = RssParser::parse(broken, 200);
    CHECK(partial.outcome == Outcome::Ok);
    CHECK_EQ(partial.items.size(), std::size_t(14));
    CHECK_EQ(partial.skipped, 1);
}

// --- schedule (DSC-06) -----------------------------------------------------------------------

static void scheduleTable() {
    const std::int64_t d = kFeedDay;
    CHECK_EQ(refreshIntervalForAge(0), 10 * kFeedMinute);
    CHECK_EQ(refreshIntervalForAge(2 * d - 1), 10 * kFeedMinute);
    CHECK_EQ(refreshIntervalForAge(2 * d), kFeedHour);            // exactly 2 days
    CHECK_EQ(refreshIntervalForAge(14 * d - 1), kFeedHour);
    CHECK_EQ(refreshIntervalForAge(14 * d), 6 * kFeedHour);       // exactly 14 days
    CHECK_EQ(refreshIntervalForAge(60 * d - 1), 6 * kFeedHour);
    CHECK_EQ(refreshIntervalForAge(60 * d), 24 * kFeedHour);      // exactly 60 days
    CHECK_EQ(refreshIntervalForAge(400 * d), 24 * kFeedHour);

    std::int64_t now = kNow;
    FeedSchedule s([&now] { return now; });
    ChannelList list;
    list.push_back(ChannelRef(chan(1), "A"));
    list.push_back(ChannelRef(chan(2), "B"));
    list.push_back(ChannelRef(chan(3), "C"));
    list.push_back(ChannelRef(chan(1), "A again"));        // duplicate: dropped
    list.push_back(ChannelRef("notachannel", "bad"));       // not a UC id: dropped
    list.push_back(ChannelRef("UC123", "short"));
    CHECK(s.setChannels(list));
    CHECK_EQ(s.size(), std::size_t(3));
    CHECK_EQ(s.due(false).size(), std::size_t(3));           // new channels are due at once

    const std::int64_t t0 = now;
    std::vector<Item> a; a.push_back(video("aaaaaaaaaa1", t0 - kFeedHour));
    std::vector<Item> c; c.push_back(video("ccccccccccc", t0 - 20 * d));
    CHECK(s.applySuccess(chan(1), a, FeedSource::Rss, "", false));
    CHECK(s.applySuccess(chan(2), std::vector<Item>(), FeedSource::InnerTube, "", true, Outcome::ValidEmpty));
    CHECK(s.applySuccess(chan(3), c, FeedSource::Rss, "", false));
    CHECK_EQ(refreshInterval(*s.find(chan(1)), now), 10 * kFeedMinute);
    CHECK_EQ(refreshInterval(*s.find(chan(2)), now), kEmptyChannelRetrySeconds);   // empty: hourly
    CHECK_EQ(refreshInterval(*s.find(chan(3)), now), 6 * kFeedHour);
    CHECK(s.find(chan(2))->lastOutcome == Outcome::ValidEmpty);
    CHECK(s.due(false).empty());
    CHECK_EQ(s.due(true).size(), std::size_t(3));            // forced: every channel
    CHECK_EQ(s.nextDueAt(), t0 + 10 * kFeedMinute);

    now = t0 + 9 * kFeedMinute;
    CHECK(s.due(false).empty());
    now = t0 + 10 * kFeedMinute;
    CHECK_EQ(s.due(false).size(), std::size_t(1));
    CHECK_EQ(s.due(false)[0].id, chan(1));
    now = t0 + kFeedHour;
    CHECK_EQ(s.due(false).size(), std::size_t(2));           // + the empty channel, hourly
    now = t0 + 6 * kFeedHour;
    CHECK_EQ(s.due(false).size(), std::size_t(3));

    // A failed fetch keeps the last good rows and waits its interval again.
    CHECK(s.applyFailure(chan(1), Outcome::Http5xx));
    CHECK_EQ(s.find(chan(1))->rows.size(), std::size_t(1));
    CHECK(s.find(chan(1))->lastOutcome == Outcome::Http5xx);
    CHECK(!isDue(*s.find(chan(1)), now));
    // A clock that went backwards makes the channel due rather than stuck.
    CHECK(isDue(*s.find(chan(1)), t0 - 100));

    // Unsubscribed channels are pruned; kept ones keep their rows; new ones are due at once.
    ChannelList next;
    next.push_back(ChannelRef(chan(1), "A renamed"));
    next.push_back(ChannelRef(chan(4), "D"));
    CHECK(s.setChannels(next));
    CHECK_EQ(s.size(), std::size_t(2));
    CHECK(s.find(chan(2)) == nullptr);
    CHECK_EQ(s.find(chan(1))->rows.size(), std::size_t(1));
    CHECK_EQ(s.find(chan(1))->channel.name, std::string("A renamed"));
    CHECK(isDue(*s.find(chan(4)), now));
    CHECK(!s.setChannels(next));                             // unchanged set

    // A running stream counts as an upload at fetch time.
    ChannelFeedState live(ChannelRef(chan(5), "E"));
    live.fetchedAt = now;
    live.rows.push_back(video("old00000000", now - 300 * d));
    live.rows.push_back(video("live0000000", -1, -1, LiveStatus::IsLive));
    CHECK_EQ(refreshInterval(live, now), 10 * kFeedMinute);
    // Upcoming streams do not count; an old channel with one stays at 24 h.
    live.rows[1].liveStatus = LiveStatus::Upcoming;
    CHECK_EQ(refreshInterval(live, now), 24 * kFeedHour);

    CHECK(isFeedChannelId(kNasa));
    CHECK(!isFeedChannelId("UCLA_DiR1FfKNvjuUpBHmyl!"));
}

// --- merge, live pinning, mixed timestamps (DSC-07) -----------------------------------------

static void mergeOrder() {
    const std::int64_t now = kNow;
    std::vector<ChannelFeedState> states;
    ChannelFeedState a(ChannelRef(chan(1), "A"));        // RSS: absolute dates
    a.fetchedAt = now;
    a.rows.push_back(video("a1000000000", now - 1 * kFeedHour));
    a.rows.push_back(video("a2000000000", now - 30 * kFeedHour));
    Item shortRow = video("aS000000000", now - 2 * kFeedHour);
    shortRow.kind = ItemKind::Short;
    shortRow.shortEvidence = ShortEvidence::RssLink;
    a.rows.push_back(shortRow);
    a.rows.push_back(video("", now));                    // malformed: no id
    a.rows.push_back(video("undated0000"));              // malformed: no timestamp
    ChannelFeedState b(ChannelRef(chan(2), "B"));        // InnerTube: relative ages
    b.fetchedAt = now - 600;
    std::vector<Item> bRows;
    bRows.push_back(video("b1000000000", -1, 2 * kFeedHour - 600));
    bRows.push_back(video("b2000000000", -1, 3 * kFeedDay));
    bRows.push_back(video("bLive000000", -1, -1, LiveStatus::IsLive));
    Item upcoming = video("bUp00000000", now + 2 * kFeedDay, -1, LiveStatus::Upcoming);
    bRows.push_back(upcoming);
    bRows.push_back(video("a1000000000", now - 1 * kFeedHour));   // duplicate of A's row
    Item channelRow = video("UCxxxxxxxxxxxxxxxxxxxxxx", now);
    channelRow.kind = ItemKind::Channel;
    bRows.push_back(channelRow);                         // malformed: not a video
    normalizeTimestamps(bRows, b.fetchedAt);
    CHECK_EQ(bRows[0].publishedEpoch, now - 2 * kFeedHour);
    CHECK_EQ(bRows[2].publishedEpoch, std::int64_t(-1));  // live rows are left alone
    b.rows = bRows;
    states.push_back(a);
    states.push_back(b);

    RowFilterSettings show;
    const MergedFeed m = mergeFeed(states, now, show);
    CHECK_EQ(m.malformed, 3);
    CHECK_EQ(m.duplicates, 1);
    CHECK_EQ(m.pinnedLive, 1);
    CHECK_EQ(m.rows.size(), std::size_t(7));
    CHECK_EQ(m.rows[0].id, std::string("bLive000000"));  // running stream pinned on top
    CHECK_EQ(m.rows[1].id, std::string("bUp00000000"));  // upcoming keyed to its fetch time, not +2 d
    CHECK_EQ(m.rows[2].id, std::string("a1000000000"));
    CHECK_EQ(m.rows[3].id, std::string("aS000000000"));  // RSS date and InnerTube age interleave
    CHECK_EQ(m.rows[4].id, std::string("b1000000000"));
    CHECK_EQ(m.rows[5].id, std::string("a2000000000"));
    CHECK_EQ(m.rows[6].id, std::string("b2000000000"));
    CHECK_EQ(m.rows[4].channel, std::string("B"));       // labels filled from the subscription
    CHECK_EQ(m.rows[4].channelId, chan(2));

    RowFilterSettings hide;
    hide.hideShorts = true;
    const MergedFeed h = mergeFeed(states, now, hide);
    CHECK_EQ(h.hiddenShorts, 1);
    CHECK_EQ(h.hiddenTotal(), 1);
    CHECK_EQ(h.rows.size(), std::size_t(6));

    // A cached LIVE state older than the trust window is not pinned any more.
    const MergedFeed later = mergeFeed(states, now + kLiveTrustSeconds + kFeedHour, show);
    CHECK_EQ(later.pinnedLive, 0);
    bool sawPast = false;
    for (const Item &it : later.rows) if (it.id == "bLive000000") sawPast = it.liveStatus == LiveStatus::WasLive;
    CHECK(sawPast);

    // Live-tab enrichment of RSS rows, and the carry-over when enrichment was unavailable.
    std::vector<Item> rss;
    rss.push_back(video("r1000000000", now - 100)); rss[0].liveStatus = LiveStatus::Unknown;
    rss.push_back(video("r2000000000", now - 200)); rss[1].liveStatus = LiveStatus::Unknown;
    std::vector<Item> liveTab;
    Item l1 = video("r1000000000", -1, -1, LiveStatus::IsLive); l1.viewCount = 81; liveTab.push_back(l1);
    liveTab.push_back(video("r2000000000", -1, 8 * kFeedHour, LiveStatus::NotLive));
    liveTab.push_back(video("extraLive00", -1, -1, LiveStatus::IsLive));
    liveTab.push_back(video("pastStream0", -1, 9 * kFeedHour, LiveStatus::NotLive));   // past, not in RSS: not added
    std::vector<Item> enriched = rss;
    CHECK_EQ(applyLiveState(enriched, liveTab), 3);
    CHECK_EQ(enriched.size(), std::size_t(3));
    CHECK(enriched[0].liveStatus == LiveStatus::IsLive);
    CHECK_EQ(enriched[0].viewCount, std::int64_t(81));
    CHECK(enriched[1].liveStatus == LiveStatus::WasLive);
    CHECK_EQ(enriched[2].id, std::string("extraLive00"));
    std::vector<Item> carried = rss;
    carryLiveState(carried, enriched);
    CHECK(carried[0].liveStatus == LiveStatus::IsLive);
}

// A refresh keeps the rows that earlier extensions loaded below the fresh window (DSC-07).
static void depthSurvivesRefresh() {
    std::int64_t now = kNow;
    FeedSchedule s([&now] { return now; });
    s.setChannels(ChannelList(1, ChannelRef(chan(1), "A")));
    std::vector<Item> rss;
    for (int i = 0; i < 15; ++i) rss.push_back(video(makeId(1, 0, i).toStdString(), now - i * kFeedHour));
    s.applySuccess(chan(1), rss, FeedSource::Rss, "", false);
    CHECK_EQ(s.extendable(10).size(), std::size_t(1));      // RSS-only: the Videos tab extends it
    std::vector<Item> page1;
    for (int i = 0; i < 30; ++i) page1.push_back(video(makeId(1, 1, i).toStdString(), -1, (15 + i) * kFeedHour));
    CHECK(s.applyPage(chan(1), page1, "tok2"));
    CHECK_EQ(s.find(chan(1))->rows.size(), std::size_t(45));
    std::vector<Item> again = page1;                         // a page that adds nothing, same token: exhausted
    CHECK(!s.applyPage(chan(1), again, "tok2"));
    CHECK(s.find(chan(1))->exhausted);
    CHECK(!s.canExtend());
    now += kFeedHour;
    std::vector<Item> fresh = rss;
    fresh.insert(fresh.begin(), video("newupload01", now - 60));
    fresh.pop_back();
    s.applySuccess(chan(1), fresh, FeedSource::Rss, "", false);
    const ChannelFeedState *st = s.find(chan(1));
    // The fresh window, plus the older tail: the RSS row that fell out of the window and the
    // Videos-tab page.
    CHECK_EQ(st->rows.size(), std::size_t(15 + 1 + 30));
    CHECK_EQ(st->rows.front().id, std::string("newupload01"));
    CHECK_EQ(st->continuation, std::string("tok2"));
    // Extension failures stop after two attempts and drop an expired token.
    FeedSchedule f([&now] { return now; });
    f.setChannels(ChannelList(1, ChannelRef(chan(2), "B")));
    f.applySuccess(chan(2), page1, FeedSource::InnerTube, "tokA", true);
    CHECK(f.canExtend());
    f.applyExtendFailure(chan(2));
    CHECK(f.find(chan(2))->continuation.empty());
    CHECK(f.canExtend());                                   // retried from the first page
    f.applyExtendFailure(chan(2));
    CHECK(!f.canExtend());
}

// --- id-sequence replacement (DSC-06) --------------------------------------------------------

static void idSequence() {
    std::vector<Item> a, b;
    for (int i = 0; i < 5; ++i) a.push_back(video("v" + std::to_string(i)));
    b = a;
    b[2].title = "changed";
    CHECK(!idSequenceChanged(a, b));                       // contents only: no replacement
    b.push_back(video("v9"));
    SequenceDiff d = diffIdSequences(a, b);
    CHECK(d.changed);
    CHECK_EQ(d.prefix, std::size_t(5));
    CHECK_EQ(d.suffix, std::size_t(0));
    b = a;
    b.insert(b.begin(), video("new"));
    d = diffIdSequences(a, b);
    CHECK(d.changed);
    CHECK_EQ(d.prefix, std::size_t(0));
    CHECK_EQ(d.suffix, std::size_t(5));
    b = a;
    b[2] = video("x");
    d = diffIdSequences(a, b);
    CHECK(d.changed);
    CHECK_EQ(d.prefix, std::size_t(2));
    CHECK_EQ(d.suffix, std::size_t(2));
    b = a;
    std::swap(b[0], b[1]);
    CHECK(idSequenceChanged(a, b));                        // order is part of the sequence
    CHECK(idSequenceChanged(a, std::vector<Item>()));
    CHECK(!idSequenceChanged(std::vector<Item>(), std::vector<Item>()));
}

// --- persistence from a copy (DSC-06) --------------------------------------------------------

static void persistence() {
    QTemporaryDir dir;
    const QString path = dir.path() + QStringLiteral("/feed/cache.json");
    FeedCacheStore store(path);
    std::int64_t now = kNow;
    FeedSchedule s([&now] { return now; });
    ChannelList list;
    for (int c = 0; c < 40; ++c) list.push_back(ChannelRef(chan(c), "C" + std::to_string(c)));
    s.setChannels(list);
    for (int c = 0; c < 40; ++c) {
        std::vector<Item> rows;
        for (int i = 0; i < 50; ++i) rows.push_back(video(makeId(c, 0, i % 100).toStdString(), now - i * 3600));
        s.applySuccess(chan(c), rows, FeedSource::Rss, "", false);
    }
    WorkLane lane(QStringLiteral("save"), 1);
    QObject context;
    for (int round = 0; round < 3; ++round) {
        std::shared_ptr<std::vector<ChannelFeedState>> copy = std::make_shared<std::vector<ChannelFeedState>>(s.snapshot());
        std::set<std::string> expected;
        for (const ChannelFeedState &st : *copy) for (const Item &it : st.rows) expected.insert(st.channel.id + "/" + it.id);
        bool done = false, ok = false;
        std::function<bool()> work = [&store, copy] { return store.save(*copy); };
        std::function<void(bool)> finished = [&done, &ok](bool r) { ok = r; done = true; };
        lane.run<bool>(&context, work, finished);
        // A refresh mutates the live state 1000 times while the save runs.
        for (int i = 0; i < 1000; ++i) {
            now += 1;
            std::vector<Item> rows;
            for (int k = 0; k < 1 + i % 7; ++k) rows.push_back(video(makeId(round + 100, i % 1000, k).toStdString(), now - k));
            s.applySuccess(chan(i % 40), rows, FeedSource::InnerTube, "tok", true);
            if (i % 250 == 0) { ChannelList fewer(list.begin(), list.begin() + 30 + i % 10); s.setChannels(fewer); s.setChannels(list); }
        }
        CHECK(catalog_test::waitFor([&done] { return done; }));
        CHECK(ok);
        bool loadedOk = false;
        const std::vector<ChannelFeedState> back = store.load(&loadedOk);
        CHECK(loadedOk);
        std::set<std::string> got;
        for (const ChannelFeedState &st : back) for (const Item &it : st.rows) got.insert(st.channel.id + "/" + it.id);
        CHECK(got == expected);                              // the save holds exactly the copy
    }
    const QFile::Permissions perms = QFileInfo(path).permissions();
    CHECK((perms & QFileDevice::ReadOwner) && (perms & QFileDevice::WriteOwner));
    CHECK(!(perms & (QFileDevice::ReadGroup | QFileDevice::WriteGroup | QFileDevice::ReadOther | QFileDevice::WriteOther)));
    CHECK_EQ(QDir(dir.path() + QStringLiteral("/feed")).entryList(QDir::Files | QDir::Hidden).size(), 1);   // no temp leftovers

    // Round trip of the fields the feed needs.
    std::vector<ChannelFeedState> one(1, ChannelFeedState(ChannelRef(kNasa, "NASA")));
    Item it = video("ROboUHfPUrU", kNow - 5, 7, LiveStatus::IsLive);
    it.kind = ItemKind::Short; it.shortEvidence = ShortEvidence::RssLink; it.membersOnly = true;
    it.viewCount = 81; it.durationSeconds = 59; it.publishedText = "x"; it.thumbnailUrl = "https://i.ytimg.com/vi/ROboUHfPUrU/mqdefault.jpg";
    one[0].rows.push_back(it);
    one[0].continuation = "tok"; one[0].pagedFromVideosTab = true; one[0].fetchedAt = kNow; one[0].lastOutcome = Outcome::Http5xx;
    bool ok = false;
    const std::vector<ChannelFeedState> rt = FeedCacheStore::deserialize(FeedCacheStore::serialize(one), &ok);
    CHECK(ok);
    CHECK_EQ(rt.size(), std::size_t(1));
    CHECK_EQ(rt[0].rows.size(), std::size_t(1));
    const Item &r = rt[0].rows[0];
    CHECK(r.liveStatus == LiveStatus::IsLive && r.kind == ItemKind::Short && r.shortEvidence == ShortEvidence::RssLink && r.membersOnly);
    CHECK_EQ(r.viewCount, std::int64_t(81));
    CHECK_EQ(r.publishedEpoch, kNow - 5);
    CHECK_EQ(rt[0].continuation, std::string("tok"));
    CHECK(rt[0].lastOutcome == Outcome::Http5xx);
    // A damaged file loads as empty, never crashes.
    CHECK(FeedCacheStore::deserialize("{\"version\":1,\"chan", &ok).empty());
    CHECK(!ok);
    CHECK(FeedCacheStore(dir.path() + QStringLiteral("/missing.json")).load(&ok).empty());
    CHECK(ok);
}

// --- channel tab parsing (tier 2, depth, live) -----------------------------------------------

static void channelTabs() {
    const QString hl = QStringLiteral("en");
    FeedChannelPage v = FeedService::parseChannelTab(replyOf(200, catalog_test::fixture(QStringLiteral("channel_videos.json"))), hl, QStringLiteral("videos"));
    CHECK(v.outcome == Outcome::Ok);
    CHECK_EQ(v.items.size(), std::size_t(30));
    CHECK(!v.continuation.isEmpty());
    CHECK_EQ(v.videosParams, QString::fromLatin1(FeedService::kVideosTabParams));   // learned from the tab list
    CHECK_EQ(v.liveParams, QString::fromLatin1(FeedService::kLiveTabParams));
    int timed = 0, aged = 0;
    for (const Item &it : v.items) { if (it.durationSeconds > 0) ++timed; if (it.publishedAgeSeconds >= 0) ++aged; }
    CHECK(timed >= 25);
    CHECK_EQ(aged, 30);

    const FeedChannelPage p2 = FeedService::parseChannelTab(replyOf(200, catalog_test::fixture(QStringLiteral("channel_videos_p2.json"))), hl, QString());
    CHECK(p2.outcome == Outcome::Ok);
    CHECK_EQ(p2.items.size(), std::size_t(30));
    CHECK(!p2.continuation.isEmpty());

    const FeedChannelPage live = FeedService::parseChannelTab(replyOf(200, catalog_test::fixture(QStringLiteral("channel_live.json"))), hl, QStringLiteral("streams"));
    CHECK(live.outcome == Outcome::Ok);
    bool running = false;
    for (const Item &it : live.items) if (it.id == "ROboUHfPUrU") running = it.liveStatus == LiveStatus::IsLive;
    CHECK(running);

    const FeedChannelPage de = FeedService::parseChannelTab(replyOf(200, catalog_test::fixture(QStringLiteral("channel_videos_de.json"))), QStringLiteral("de"), QStringLiteral("videos"));
    CHECK(de.outcome == Outcome::Ok);
    const FeedChannelPage ja = FeedService::parseChannelTab(replyOf(200, catalog_test::fixture(QStringLiteral("channel_videos_ja.json"))), QStringLiteral("ja"), QStringLiteral("videos"));
    CHECK(ja.outcome == Outcome::Ok);

    // Home selected (the params were not understood) while Videos is listed: drift.
    const FeedChannelPage home = FeedService::parseChannelTab(replyOf(200, catalog_test::fixture(QStringLiteral("channel_home.json"))), hl, QStringLiteral("videos"));
    CHECK(home.outcome == Outcome::ShapeDrift);
    CHECK(home.requestedTabListed && !home.requestedTabSelected);
    // Derived: no tab list at all is drift; alerts without tabs is a dead channel.
    CHECK(FeedService::parseChannelTab(replyOf(200, "{\"contents\":{}}"), hl, QStringLiteral("videos")).outcome == Outcome::ShapeDrift);
    CHECK(FeedService::parseChannelTab(replyOf(200, "{\"alerts\":[{\"alertRenderer\":{\"type\":\"ERROR\"}}]}"), hl, QStringLiteral("videos")).outcome == Outcome::Unavailable);
    CHECK(FeedService::parseChannelTab(replyOf(503, ""), hl, QStringLiteral("videos")).outcome == Outcome::Http5xx);
    CHECK(FeedService::parseChannelTab(replyOf(200, "{\"responseContext\":{}}"), hl, QString()).outcome == Outcome::ShapeDrift);
    CHECK(FeedService::parseChannelTab(replyOf(200, "{\"onResponseReceivedActions\":[{\"appendContinuationItemsAction\":{\"continuationItems\":[]}}]}"), hl, QString()).outcome == Outcome::ValidEmpty);
    // Derived: a channel tab list without a Live tab: no streams, valid-empty.
    QByteArray noLive = catalog_test::fixture(QStringLiteral("channel_videos.json"));
    noLive.replace("/streams\"", "/other\"");
    CHECK(FeedService::parseChannelTab(replyOf(200, noLive), hl, QStringLiteral("streams")).outcome == Outcome::ValidEmpty);
}

// --- one dead channel (DSC-05 acceptance) ----------------------------------------------------

static void deadChannel() {
    Rig rig;
    const std::string dead = chan(66), other = chan(7);
    bool nasaDown = false;
    rig.net.handler = [&](const HttpRequest &r) {
        const Req q = parseReq(r);
        if (q.rss && q.channel == QLatin1String(kNasa))
            return nasaDown ? ScriptedTransport::Answer(503, "") : ScriptedTransport::Answer(200, catalog_test::fixture(QStringLiteral("rss_channel.xml")));
        if (q.rss && q.channel == QString::fromStdString(dead))
            return ScriptedTransport::Answer(404, catalog_test::fixture(QStringLiteral("rss_dead_channel.xml")));
        if (q.rss && q.channel == QString::fromStdString(other)) return ScriptedTransport::Answer(200, rssFor(other, 7));
        if (q.liveTab() && q.channel == QLatin1String(kNasa)) return ScriptedTransport::Answer(200, catalog_test::fixture(QStringLiteral("channel_live.json")));
        if (q.liveTab()) return ScriptedTransport::Answer(404, "");
        return ScriptedTransport::Answer(500, "");   // every Videos-tab call fails (dead channel's tier 2)
    };
    FeedService service(&rig.net, &rig.client, nullptr, nullptr, rig.clock());
    QStringList failed;
    QObject::connect(&service, &FeedService::channelFailed, [&failed](const QString &id, int) { failed.append(id); });
    ChannelList list;
    list.push_back(ChannelRef(kNasa, "NASA"));
    list.push_back(ChannelRef(dead, "Terminated"));
    list.push_back(ChannelRef(other, "Other"));
    service.setChannels(list);
    service.start();
    CHECK(waitIdle(service));
    const MergedFeed &m = service.merged();
    CHECK(rowsOf(m, kNasa) >= 15);
    CHECK_EQ(rowsOf(m, other), 15);
    CHECK_EQ(rowsOf(m, dead), 0);
    CHECK_EQ(failed.count(), 1);
    CHECK_EQ(failed.value(0), QString::fromStdString(dead));
    CHECK(service.schedule().find(dead)->lastOutcome == Outcome::Http5xx);
    CHECK_EQ(service.lastRoundFailures(), 1);
    CHECK(!service.lastRoundAllFailed());

    // Next round NASA's RSS and InnerTube fail too: its last good rows stay painted.
    const int nasaRows = rowsOf(service.merged(), kNasa);
    nasaDown = true;
    service.refresh(true);
    CHECK(waitIdle(service));
    CHECK_EQ(rowsOf(service.merged(), kNasa), nasaRows);
    CHECK_EQ(rowsOf(service.merged(), other), 15);
    CHECK_EQ(service.lastRoundFailures(), 2);
}

// --- tier fallbacks (DSC-05) -----------------------------------------------------------------

static void tierFallbacks() {
    // RSS 404 -> InnerTube Videos tab; the extractor is not asked.
    {
        Rig rig;
        rig.net.handler = [](const HttpRequest &r) {
            const Req q = parseReq(r);
            if (q.rss) return ScriptedTransport::Answer(404, catalog_test::fixture(QStringLiteral("rss_dead_channel.xml")));
            if (q.videosTab()) return ScriptedTransport::Answer(200, catalog_test::fixture(QStringLiteral("channel_videos.json")));
            return ScriptedTransport::Answer(500, "");
        };
        FeedService service(&rig.net, &rig.client, nullptr, nullptr, rig.clock());
        int extractorCalls = 0;
        service.setExtractor([&extractorCalls](const ChannelRef &, std::function<void(const ExtractorFeedResult &)> done) {
            ++extractorCalls;
            done(ExtractorFeedResult());
        });
        service.setChannels(ChannelList(1, ChannelRef(kNasa, "NASA")));
        service.start();
        CHECK(waitIdle(service));
        const ChannelFeedState *s = service.schedule().find(kNasa);
        CHECK(s->source == FeedSource::InnerTube);
        CHECK_EQ(s->rows.size(), std::size_t(30));
        CHECK(!s->continuation.empty());
        CHECK_EQ(extractorCalls, 0);
        CHECK_EQ(service.merged().rows.size(), std::size_t(30));
        for (const Item &it : service.merged().rows) CHECK(it.publishedEpoch > 0);   // ages converted with the fetch time
        CHECK_EQ(countReq(rig.net, [](const Req &q) { return q.liveTab(); }), 0);      // tier 2 carries live flags inline
    }
    // Both fail -> the extractor hook, once; its rows are used.
    {
        Rig rig;
        rig.net.handler = [](const HttpRequest &r) {
            return parseReq(r).rss ? ScriptedTransport::Answer(404, "") : ScriptedTransport::Answer(500, "");
        };
        FeedService service(&rig.net, &rig.client, nullptr, nullptr, rig.clock());
        int extractorCalls = 0;
        service.setExtractor([&extractorCalls](const ChannelRef &c, std::function<void(const ExtractorFeedResult &)> done) {
            ++extractorCalls;
            ExtractorFeedResult r;
            r.outcome = Outcome::Ok;
            r.rows.push_back(video("xtr00000001", kNow - 3600));
            r.rows.push_back(video("xtr00000002", kNow - 7200));
            r.rows[0].channelId = c.id;
            done(r);
        });
        service.setChannels(ChannelList(1, ChannelRef(kNasa, "NASA")));
        service.start();
        CHECK(waitIdle(service));
        CHECK_EQ(extractorCalls, 1);
        CHECK(service.schedule().find(kNasa)->source == FeedSource::Extractor);
        CHECK_EQ(service.merged().rows.size(), std::size_t(2));
    }
    // Both fail without an extractor: the refresh completes, reports, and does not hang.
    {
        Rig rig;
        rig.net.handler = [](const HttpRequest &r) {
            return parseReq(r).rss ? ScriptedTransport::Answer(404, "") : ScriptedTransport::Answer(500, "");
        };
        FeedService service(&rig.net, &rig.client, nullptr, nullptr, rig.clock());
        sailfish::FeedModel model(&service);
        service.setChannels(ChannelList(1, ChannelRef(kNasa, "NASA")));
        service.start();
        CHECK(waitIdle(service));
        CHECK(service.lastRoundAllFailed());
        CHECK(service.schedule().find(kNasa)->lastOutcome == Outcome::Http5xx);
        CHECK(!model.errorText().isEmpty());
        CHECK(!model.busy());
    }
    // RSS works without any extractor (DSC-05: refresh must not require it).
    {
        Rig rig;
        rig.net.handler = [](const HttpRequest &r) {
            const Req q = parseReq(r);
            if (q.rss) return ScriptedTransport::Answer(200, catalog_test::fixture(QStringLiteral("rss_channel.xml")));
            return ScriptedTransport::Answer(404, "");
        };
        FeedService service(&rig.net, &rig.client, nullptr, nullptr, rig.clock());
        service.setChannels(ChannelList(1, ChannelRef(kNasa, "NASA")));
        service.start();
        CHECK(waitIdle(service));
        CHECK(service.schedule().find(kNasa)->source == FeedSource::Rss);
        CHECK_EQ(service.merged().rows.size(), std::size_t(15));
    }
    // An empty RSS answer asks the Videos tab; stale built-in params are replaced by the ones
    // learned from the tab list (derived: the Videos tab's params altered in channel_home).
    {
        Rig rig;
        QByteArray home = catalog_test::fixture(QStringLiteral("channel_home.json"));
        home.replace("EgZ2aWRlb3PyBgQKAjoA", "TEARNEDPARAMS");
        rig.net.handler = [&home](const HttpRequest &r) {
            const Req q = parseReq(r);
            if (q.rss) return ScriptedTransport::Answer(200, rssFor(kNasa, -1, 0));
            if (q.browse && q.params == QLatin1String("TEARNEDPARAMS"))
                return ScriptedTransport::Answer(200, catalog_test::fixture(QStringLiteral("channel_videos.json")));
            if (q.videosTab()) return ScriptedTransport::Answer(200, home);
            return ScriptedTransport::Answer(500, "");
        };
        FeedService service(&rig.net, &rig.client, nullptr, nullptr, rig.clock());
        service.setChannels(ChannelList(1, ChannelRef(kNasa, "NASA")));
        service.start();
        CHECK(waitIdle(service));
        CHECK(service.schedule().find(kNasa)->source == FeedSource::InnerTube);
        CHECK_EQ(service.merged().rows.size(), std::size_t(30));
        CHECK_EQ(countReq(rig.net, [](const Req &q) { return q.browse; }), 2);
    }
}

// --- due-only versus forced, and the 10-parallel cap (DSC-05/06) -----------------------------

static void dueAndForced() {
    Rig rig;
    const std::string old = chan(8);
    QByteArray oldFeed = rssFor(old, 8);
    oldFeed.replace("<published>2026-", "<published>2025-");   // derived: a quiet channel (24 h)
    rig.net.handler = [&](const HttpRequest &r) {
        const Req q = parseReq(r);
        if (q.rss && q.channel == QLatin1String(kNasa)) return ScriptedTransport::Answer(200, catalog_test::fixture(QStringLiteral("rss_channel.xml")));
        if (q.rss) return ScriptedTransport::Answer(200, oldFeed);
        return ScriptedTransport::Answer(404, "");
    };
    FeedService service(&rig.net, &rig.client, nullptr, nullptr, rig.clock());
    ChannelList list;
    list.push_back(ChannelRef(kNasa, "NASA"));
    list.push_back(ChannelRef(old, "Quiet"));
    service.setChannels(list);
    service.start();
    CHECK(waitIdle(service));
    auto rssCount = [&rig](const std::string &c) {
        return countReq(rig.net, [&c](const Req &q) { return q.rss && q.channel == QString::fromStdString(c); });
    };
    CHECK_EQ(rssCount(kNasa), 1);
    CHECK_EQ(rssCount(old), 1);
    CHECK_EQ(refreshInterval(*service.schedule().find(kNasa), rig.now), 10 * kFeedMinute);
    CHECK_EQ(refreshInterval(*service.schedule().find(old), rig.now), 24 * kFeedHour);

    rig.now += 5 * kFeedMinute;
    service.refresh(false);
    CHECK(!service.refreshing());                    // nothing due
    CHECK(waitIdle(service));
    CHECK_EQ(rssCount(kNasa), 1);
    rig.now += 6 * kFeedMinute;
    service.refresh(false);                          // only the active channel is due
    CHECK(waitIdle(service));
    CHECK_EQ(rssCount(kNasa), 2);
    CHECK_EQ(rssCount(old), 1);
    service.refresh(true);                           // pull-to-refresh: every channel
    service.refresh(true);                           // ignored while running
    CHECK(waitIdle(service));
    CHECK_EQ(rssCount(kNasa), 3);
    CHECK_EQ(rssCount(old), 2);
}

static void parallelCap() {
    Rig rig;
    rig.net.handler = [](const HttpRequest &r) {
        const Req q = parseReq(r);
        if (q.rss) return ScriptedTransport::Answer(200, rssFor(q.channel.toStdString(), q.channel.right(3).toInt()), 40);
        if (q.liveTab()) return ScriptedTransport::Answer(200, catalog_test::fixture(QStringLiteral("channel_live.json")), 20);
        return ScriptedTransport::Answer(404, "", 10);
    };
    FeedService service(&rig.net, &rig.client, nullptr, nullptr, rig.clock());
    ChannelList list;
    for (int c = 0; c < 25; ++c) list.push_back(ChannelRef(chan(c), "C"));
    service.setChannels(list);
    service.start();
    CHECK(waitIdle(service, 15000));
    CHECK_EQ(rig.net.peak, 10);
    CHECK_EQ(service.peakInFlight(), 10);
    CHECK_EQ(countReq(rig.net, [](const Req &q) { return q.rss; }), 25);
    CHECK_EQ(service.lastRoundAttempts(), 25);
    CHECK_EQ(service.lastRoundFailures(), 0);
}

// --- the model keeps the shown list unless the id sequence changed (DSC-06) ------------------

static void modelReplacement() {
    Rig rig;
    bool newUpload = false;
    rig.net.handler = [&newUpload](const HttpRequest &r) {
        const Req q = parseReq(r);
        if (!q.rss) return ScriptedTransport::Answer(404, "");
        QByteArray body = catalog_test::fixture(QStringLiteral("rss_channel.xml"));
        if (newUpload) {   // derived: one more entry, newer than all others
            const int first = body.indexOf("<entry>");
            const int end = body.indexOf("</entry>", first) + 8;
            QByteArray entry = body.mid(first, end - first);
            entry.replace("MRg4uIpJ3xE", "NEWupload01");
            entry.replace("2026-11-07T23:39:59+00:00", "2026-11-08T07:00:00+00:00");
            body.insert(first, entry);
        }
        return ScriptedTransport::Answer(200, body);
    };
    FeedService service(&rig.net, &rig.client, nullptr, nullptr, rig.clock());
    service.setLiveEnrichment(false);
    sailfish::FeedModel model(&service);
    model.setClock([&rig] { return rig.now; });
    service.setChannels(ChannelList(1, ChannelRef(kNasa, "NASA")));
    service.start();
    CHECK(waitIdle(service));
    CHECK(catalog_test::waitFor([&model] { return model.count() == 15; }));
    const int resets = model.resets();
    QList<QPair<int, int>> inserted;
    int removed = 0, reset = 0;
    QObject::connect(&model, &QAbstractItemModel::rowsInserted, [&inserted](const QModelIndex &, int a, int b) { inserted.append(qMakePair(a, b)); });
    QObject::connect(&model, &QAbstractItemModel::rowsRemoved, [&removed](const QModelIndex &, int, int) { ++removed; });
    QObject::connect(&model, &QAbstractItemModel::modelReset, [&reset] { ++reset; });

    service.refresh(true);                           // identical answers
    CHECK(waitIdle(service));
    QCoreApplication::processEvents();
    CHECK_EQ(model.resets(), resets);
    CHECK_EQ(inserted.count(), 0);
    CHECK_EQ(removed, 0);

    newUpload = true;
    service.refresh(true);
    CHECK(waitIdle(service));
    CHECK(catalog_test::waitFor([&model] { return model.count() == 16; }));
    CHECK_EQ(model.resets(), resets + 1);
    CHECK_EQ(inserted.count(), 1);
    CHECK_EQ(inserted.value(0).first, 0);            // spliced in at the top, rest untouched
    CHECK_EQ(inserted.value(0).second, 0);
    CHECK_EQ(removed, 0);
    CHECK_EQ(reset, 0);

    // Roles compatible with VideoListItem plus the feed's own.
    const QVariantMap row = model.get(0);
    CHECK_EQ(row.value(QStringLiteral("videoId")).toString(), QStringLiteral("NEWupload01"));
    static const char *const roles[] = { "videoId", "title", "channel", "channelId", "duration", "durationSeconds", "viewCount",
                                         "thumbnail", "live", "publishedText", "publishedEpoch", "short", "membersOnly", "upcoming" };
    for (const char *r : roles) CHECK(row.contains(QLatin1String(r)));
    CHECK_EQ(row.value(QStringLiteral("publishedText")).toString(), QStringLiteral("1 hour ago"));   // the discovery language (en)
    CHECK_EQ(row.value(QStringLiteral("channel")).toString(), QStringLiteral("NASA"));
    CHECK_EQ(sailfish::CatalogRowsModel::relativeAge(kNow - 30, kNow), QStringLiteral("1 minute ago"));
    sailfish::CatalogRowsModel::setLanguage(QStringLiteral("es"));
    CHECK_EQ(sailfish::CatalogRowsModel::relativeAge(kNow - 3 * 86400, kNow), QString::fromUtf8("hace 3 d\xC3\xAD" "as"));
    sailfish::CatalogRowsModel::setLanguage(QStringLiteral("en"));
}

// --- depth through continuations (DSC-07) ----------------------------------------------------

static void depth() {
    // Four channels whose RSS fails: their Videos tabs give 120 rows, continuations the rest.
    {
        Rig rig;
        int serial = 0;
        rig.net.handler = [&serial](const HttpRequest &r) {
            const Req q = parseReq(r);
            if (q.rss) return ScriptedTransport::Answer(404, "");
            if (q.browse && !q.continuation.isEmpty()) return ScriptedTransport::Answer(200, continuationPage(++serial));
            if (q.videosTab()) return ScriptedTransport::Answer(200, videosFor(q.channel.right(3).toInt()));
            return ScriptedTransport::Answer(500, "");
        };
        FeedService service(&rig.net, &rig.client, nullptr, nullptr, rig.clock());
        sailfish::FeedModel model(&service);
        ChannelList list;
        for (int c = 0; c < 4; ++c) list.push_back(ChannelRef(chan(c), "C"));
        service.setChannels(list);
        service.start();
        CHECK(waitIdle(service));
        CHECK(catalog_test::waitFor([&model] { return model.count() == 120; }));
        for (int round = 0; round < 10 && model.count() < 300; ++round) {
            CHECK(model.canLoadMore());
            model.loadMore();
            CHECK(waitIdle(service));
            QCoreApplication::processEvents();
        }
        CHECK(model.count() >= 300);
        CHECK(countReq(rig.net, [](const Req &q) { return !q.continuation.isEmpty(); }) >= 8);
        std::set<std::string> ids;
        std::int64_t last = -1;
        bool sorted = true;
        for (const Item &it : model.rows()) {
            ids.insert(it.id);
            if (last >= 0 && it.publishedEpoch > last) sorted = false;
            last = it.publishedEpoch;
        }
        CHECK_EQ(ids.size(), model.rows().size());
        CHECK(sorted);
    }
    // Forty RSS channels (derived: 3 entries each) scroll past 300 rows through extension.
    {
        Rig rig;
        int serial = 0;
        rig.net.handler = [&serial](const HttpRequest &r) {
            const Req q = parseReq(r);
            const int tag = q.channel.right(3).toInt();
            if (q.rss) return ScriptedTransport::Answer(200, rssFor(q.channel.toStdString(), tag, 3));
            if (q.browse && !q.continuation.isEmpty()) return ScriptedTransport::Answer(200, continuationPage(++serial));
            if (q.videosTab()) return ScriptedTransport::Answer(200, videosFor(tag));
            return ScriptedTransport::Answer(404, "");
        };
        FeedService service(&rig.net, &rig.client, nullptr, nullptr, rig.clock());
        service.setLiveEnrichment(false);
        sailfish::FeedModel model(&service);
        ChannelList list;
        for (int c = 0; c < 40; ++c) list.push_back(ChannelRef(chan(c), "C"));
        service.setChannels(list);
        service.start();
        CHECK(waitIdle(service, 15000));
        CHECK(catalog_test::waitFor([&model] { return model.count() == 120; }));
        for (int round = 0; round < 20 && model.count() < 1500; ++round) {
            if (!model.canLoadMore()) break;
            model.loadMore();
            CHECK(waitIdle(service, 15000));
            QCoreApplication::processEvents();
        }
        CHECK(model.count() >= 300);
        CHECK(countReq(rig.net, [](const Req &q) { return !q.continuation.isEmpty(); }) >= 1);
        CHECK(rig.net.peak <= 10);
    }
}

// --- live pinned on top; mixed sources sort by timestamp (DSC-07) ----------------------------

static void liveAndMixed() {
    Rig rig;
    const std::string other = chan(9);
    rig.net.handler = [&other](const HttpRequest &r) {
        const Req q = parseReq(r);
        if (q.rss && q.channel == QLatin1String(kNasa)) return ScriptedTransport::Answer(200, catalog_test::fixture(QStringLiteral("rss_channel.xml")));
        if (q.rss) return ScriptedTransport::Answer(404, "");
        if (q.liveTab() && q.channel == QLatin1String(kNasa)) return ScriptedTransport::Answer(200, catalog_test::fixture(QStringLiteral("channel_live.json")));
        if (q.videosTab() && q.channel == QString::fromStdString(other)) return ScriptedTransport::Answer(200, videosFor(9));
        return ScriptedTransport::Answer(500, "");
    };
    FeedService service(&rig.net, &rig.client, nullptr, nullptr, rig.clock());
    sailfish::FeedModel model(&service);
    ChannelList list;
    list.push_back(ChannelRef(kNasa, "NASA"));
    list.push_back(ChannelRef(other, "Other"));
    service.setChannels(list);
    service.start();
    CHECK(waitIdle(service));
    const MergedFeed &m = service.merged();
    CHECK_EQ(m.pinnedLive, 2);
    std::set<std::string> pinned;
    pinned.insert(m.rows[0].id);
    pinned.insert(m.rows[1].id);
    CHECK(pinned.count("ROboUHfPUrU") == 1);          // RSS row enriched to LIVE
    CHECK(pinned.count("M3HKLzjvKPc") == 1);          // running stream found only on the Live tab
    for (std::size_t i = 2; i < m.rows.size(); ++i) CHECK(m.rows[i].liveStatus != LiveStatus::IsLive);
    for (const Item &it : m.rows) {
        if (it.id == "ROboUHfPUrU") CHECK(it.viewCount > 0 && it.viewCount < 100000);   // watching now, not total views
        if (it.id == "MRg4uIpJ3xE") CHECK(it.liveStatus == LiveStatus::WasLive);
    }
    CHECK(catalog_test::waitFor([&model] { return model.count() > 2; }));
    CHECK(model.data(model.index(0), sailfish::CatalogRowsModel::LiveRole).toBool());
    CHECK(model.data(model.index(0), sailfish::CatalogRowsModel::DurationRole).toString().isEmpty());

    // Below the pinned rows: newest first by the best timestamp, RSS and InnerTube interleaved.
    std::int64_t last = -1;
    bool sorted = true;
    int firstOther = -1, lastNasa = -1;
    for (std::size_t i = 2; i < m.rows.size(); ++i) {
        const std::int64_t t = feedSortTime(m.rows[i], kNow);
        if (last >= 0 && t > last) sorted = false;
        last = t;
        if (m.rows[i].channelId == other && firstOther < 0) firstOther = static_cast<int>(i);
        if (m.rows[i].channelId == kNasa) lastNasa = static_cast<int>(i);
    }
    CHECK(sorted);
    CHECK(firstOther >= 0 && firstOther < lastNasa);

    // Hours later without a refresh, the cached LIVE state is no longer pinned.
    rig.now += kLiveTrustSeconds + kFeedHour;
    service.setRowFilter(RowFilterSettings());
    CHECK_EQ(service.merged().pinnedLive, 0);
}

// --- stale-while-revalidate from the cache (DSC-06) ------------------------------------------

static void cacheLaunch() {
    QTemporaryDir dir;
    FeedCacheStore store(dir.path() + QStringLiteral("/feed.json"));
    int painted = 0;
    {
        Rig rig;
        rig.net.handler = [](const HttpRequest &r) {
            return parseReq(r).rss ? ScriptedTransport::Answer(200, catalog_test::fixture(QStringLiteral("rss_channel.xml")))
                                   : ScriptedTransport::Answer(404, "");
        };
        FeedService service(&rig.net, &rig.client, &store, nullptr, rig.clock());
        service.setChannels(ChannelList(1, ChannelRef(kNasa, "NASA")));
        service.start();
        CHECK(waitIdle(service));
        // Until no save runs or waits, the file may still hold the snapshot taken before the
        // refresh (the first save is asked for when the channels are set).
        CHECK(catalog_test::waitFor([&service] { return service.savesCompleted() >= 1 && !service.saving(); }, 15000));
        painted = static_cast<int>(service.merged().rows.size());
        CHECK_EQ(painted, 15);
    }
    // Next launch, offline and 30 min later: the cache paints at once, the due channel
    // refreshes in the background, fails, and the cached rows stay.
    Rig rig;
    rig.now = kNow + 30 * kFeedMinute;
    rig.net.handler = [](const HttpRequest &) { return ScriptedTransport::Answer(0, "", 200); };
    FeedService service(&rig.net, &rig.client, &store, nullptr, rig.clock());
    sailfish::FeedModel model(&service);
    service.setChannels(ChannelList(1, ChannelRef(kNasa, "NASA")));
    CHECK(!model.loaded());
    service.start();
    CHECK(catalog_test::waitFor([&service] { return service.cacheLoaded(); }));
    CHECK_EQ(static_cast<int>(service.merged().rows.size()), painted);
    CHECK_EQ(model.count(), painted);
    CHECK(service.refreshing());
    CHECK(model.refreshing() && model.stale());
    CHECK(waitIdle(service));
    CHECK_EQ(model.count(), painted);
    CHECK(!model.stale());
    // A new subscription is due at once.
    const int before = static_cast<int>(rig.net.requests.size());
    ChannelList two;
    two.push_back(ChannelRef(kNasa, "NASA"));
    two.push_back(ChannelRef(chan(3), "New"));
    service.setChannels(two);
    CHECK(service.refreshing());
    CHECK(waitIdle(service));
    CHECK(countReq(rig.net, [](const Req &q) { return q.rss && q.channel == QString::fromStdString(chan(3)); }) >= 1);
    CHECK(static_cast<int>(rig.net.requests.size()) > before);
    CHECK_EQ(model.channelCount(), 2);
    // setChannels from QML.
    QVariantList qml;
    QVariantMap c; c.insert(QStringLiteral("id"), QString::fromLatin1(kNasa)); c.insert(QStringLiteral("name"), QStringLiteral("NASA"));
    qml.append(c);
    model.setChannels(qml);
    CHECK_EQ(model.channelCount(), 1);
    CHECK(waitIdle(service));
}

// --- home: valid-empty versus drift, landing page (DSC-04) -----------------------------------

static void home() {
    const QString hl = QStringLiteral("en");
    const HomePage anon = HomeService::parseHome(replyOf(200, catalog_test::fixture(QStringLiteral("home_anonymous.json"))), hl, false);
    CHECK(anon.outcome == Outcome::ValidEmpty);              // a rich grid with only a nudge
    CHECK(anon.items.empty());
    QByteArray drift = catalog_test::fixture(QStringLiteral("home_anonymous.json"));
    drift.replace("\"richGridRenderer\"", "\"someNewGridRenderer\"");   // derived: container gone
    CHECK(HomeService::parseHome(replyOf(200, drift), hl, false).outcome == Outcome::ShapeDrift);
    CHECK(HomeService::parseHome(replyOf(503, ""), hl, false).outcome == Outcome::Http5xx);   // never drift
    CHECK(HomeService::parseHome(replyOf(200, catalog_test::fixture(QStringLiteral("search_en_p1.json"))), hl, false).outcome == Outcome::ShapeDrift);
    // Derived signed-in shape: the anonymous home with a channel's video grid in its tab.
    QJsonObject doc = catalog_test::fixtureJson(QStringLiteral("home_anonymous.json"));
    const QJsonObject grid = json::findFirst(catalog_test::fixtureJson(QStringLiteral("channel_videos.json")), QStringLiteral("richGridRenderer"));
    QJsonObject content; content.insert(QStringLiteral("richGridRenderer"), grid);
    QJsonObject tabRenderer; tabRenderer.insert(QStringLiteral("selected"), true); tabRenderer.insert(QStringLiteral("content"), content);
    QJsonObject tab; tab.insert(QStringLiteral("tabRenderer"), tabRenderer);
    QJsonObject two; two.insert(QStringLiteral("tabs"), QJsonArray() << tab);
    QJsonObject contents; contents.insert(QStringLiteral("twoColumnBrowseResultsRenderer"), two);
    doc.insert(QStringLiteral("contents"), contents);
    const HomePage full = HomeService::parseHome(replyOf(200, QJsonDocument(doc).toJson()), hl, false);
    CHECK(full.outcome == Outcome::Ok);
    CHECK_EQ(full.items.size(), std::size_t(30));
    CHECK(!full.continuation.isEmpty());
    CHECK(HomeService::parseHome(replyOf(200, catalog_test::fixture(QStringLiteral("channel_videos_p2.json"))), hl, true).outcome == Outcome::Ok);
    CHECK(HomeService::parseHome(replyOf(200, "{}"), hl, true).outcome == Outcome::ShapeDrift);

    // DSC-04a (user-approved 2026-11-08): signed out, Home is the public "Popular" list from the
    // anonymous search endpoint (empty query, most viewed this week, videos only), recorded in
    // es/ES; page 1 cached for 10 minutes per language and region. Signed in: FEwhat_to_watch.
    {
        const QJsonObject body = HomeService::popularBody();
        CHECK_EQ(body.value(QStringLiteral("query")).toString(), QString());
        CHECK_EQ(body.value(QStringLiteral("params")).toString(), QStringLiteral("CAMSBAgDEAE="));
        Rig rig;
        int searches = 0, browses = 0;
        rig.net.handler = [&](const HttpRequest &r) {
            if (r.url.path().endsWith(QLatin1String("/search"))) {
                ++searches;
                if (r.body.contains("\"continuation\"")) return ScriptedTransport::Answer(200, catalog_test::fixture(QStringLiteral("home_popular_es_p2.json")));
                return ScriptedTransport::Answer(200, catalog_test::fixture(QStringLiteral("home_popular_es.json")));
            }
            ++browses;
            return ScriptedTransport::Answer(200, catalog_test::fixture(QStringLiteral("home_anonymous.json")));
        };
        qint64 now = 0;
        HomeService service(&rig.client, false);
        service.setClock([&now] { return now; });
        sailfish::RecommendedModel model(&service);
        CHECK(model.popular() && !model.signedIn());
        service.load();
        CHECK(catalog_test::waitFor([&service] { return !service.busy(); }));
        CHECK_EQ(searches, 1);
        CHECK_EQ(browses, 0);
        CHECK(service.outcome() == Outcome::Ok);
        const int page1 = model.count();
        CHECK(page1 >= 10);
        CHECK(rig.net.requests.at(0).body.contains("\"query\":\"\""));
        CHECK(rig.net.requests.at(0).body.contains("CAMSBAgDEAE="));
        for (const Item &it : service.items()) CHECK(isVideoLike(it) && !it.id.empty());
        // Paging continues through the search continuation.
        CHECK(service.canLoadMore());
        model.loadMore();
        CHECK(catalog_test::waitFor([&] { return !service.busy() && model.count() > page1; }));
        CHECK_EQ(searches, 2);
        // Within 10 minutes page 1 comes from the cache; after that it is asked again.
        now += 9 * 60 * 1000;
        service.load();
        CHECK(!service.busy());
        CHECK_EQ(model.count(), page1);
        CHECK_EQ(searches, 2);
        now += 2 * 60 * 1000;
        service.load();
        CHECK(catalog_test::waitFor([&service] { return !service.busy(); }));
        CHECK_EQ(searches, 3);
        // Another language or region is not served from the cache.
        DiscoveryLocale de;
        de.hl = QStringLiteral("de");
        de.gl = QStringLiteral("DE");
        rig.client.setLocale(de);
        service.load();
        CHECK(catalog_test::waitFor([&service] { return !service.busy(); }));
        CHECK_EQ(searches, 4);
        // Signed in: the recommended home instead, never the popular list.
        service.setClient(&rig.client, true);
        CHECK(model.signedIn() && !model.popular());
        model.reload();
        CHECK(catalog_test::waitFor([&service] { return !service.busy(); }));
        CHECK_EQ(browses, 1);
        CHECK_EQ(searches, 4);
        CHECK(rig.net.requests.last().body.contains("FEwhat_to_watch"));
        CHECK(service.outcome() == Outcome::ValidEmpty);
        CHECK_EQ(model.count(), 0);
    }

    // Landing page setting.
    QTemporaryDir dir;
    const QString file = dir.path() + QStringLiteral("/SailTube.conf");
    {
        sailfish::HomeSettings s(file, nullptr);
        CHECK_EQ(s.landingPage(), QStringLiteral("subscriptions"));       // default
        CHECK_EQ(s.initialPage(), QStringLiteral("pages/FeedPage.qml"));
        CHECK_EQ(s.landingChoices().size(), 2);                           // Recommended only when signed in
        CHECK(!sailfish::HomeSettings::landingPages(false).contains(QStringLiteral("recommended")));
        CHECK(sailfish::HomeSettings::landingPages(true).contains(QStringLiteral("recommended")));
        s.setLandingPage(QStringLiteral("recommended"));                  // refused while signed out
        CHECK_EQ(s.landingPage(), QStringLiteral("subscriptions"));
        s.setSignedIn(true);
        CHECK_EQ(s.landingChoices().size(), 3);
        s.setLandingPage(QStringLiteral("recommended"));
        CHECK_EQ(s.effectiveLandingPage(), QStringLiteral("recommended"));
        CHECK_EQ(s.initialPage(), QStringLiteral("pages/RecommendedPage.qml"));
        s.setSignedIn(false);
        CHECK_EQ(s.effectiveLandingPage(), QStringLiteral("subscriptions")); // falls back while signed out
        s.setLandingPage(QStringLiteral("bogus"));
        CHECK_EQ(s.landingPage(), QStringLiteral("recommended"));
        s.setLandingPage(QStringLiteral("search"));
    }
    {
        sailfish::HomeSettings s(file, nullptr);                          // survives a restart
        CHECK_EQ(s.landingPage(), QStringLiteral("search"));
        CHECK_EQ(s.initialPage(), QStringLiteral("pages/SearchPage.qml"));
    }
    {
        QSettings raw(file, QSettings::IniFormat);
        raw.setValue(QLatin1String(sailfish::HomeSettings::kKey), QStringLiteral("garbage"));
        raw.sync();
        sailfish::HomeSettings s(file, nullptr);
        CHECK_EQ(s.landingPage(), QStringLiteral("subscriptions"));
    }
}

// YouTube-style cards: every feed row has a channelAvatar. RSS rows carry none of their own, so
// it is the avatar known for the row's channel (AvatarBook): empty until something names it, and
// re-announced for the rows on screen when it becomes known. The live-tab answer the refresh
// fetches anyway names it, so no request is made for avatars.
static void channelAvatars() {
    AvatarBook::instance().clear();
    Rig rig;
    int browses = 0;
    rig.net.handler = [&](const HttpRequest &r) {
        const Req q = parseReq(r);
        if (q.rss) return ScriptedTransport::Answer(200, catalog_test::fixture(QStringLiteral("rss_channel.xml")));
        ++browses;
        if (q.liveTab()) return ScriptedTransport::Answer(200, catalog_test::fixture(QStringLiteral("channel_live.json")));
        return ScriptedTransport::Answer(404, "");
    };
    {
        // Without the live-tab answer (enrichment off): unknown, so empty; known later, announced.
        FeedService service(&rig.net, &rig.client, nullptr, nullptr, rig.clock());
        service.setLiveEnrichment(false);
        sailfish::FeedModel model(&service);
        service.setChannels(ChannelList(1, ChannelRef(kNasa, "NASA")));
        service.start();
        CHECK(waitIdle(service));
        CHECK(model.count() > 0);
        CHECK_EQ(browses, 0);
        CHECK(model.roleNames().values().contains("channelAvatar"));
        CHECK_EQ(model.get(0).value(QStringLiteral("channelAvatar")).toString(), QString());
        int announced = 0;
        QVector<int> roles;
        QObject::connect(&model, &QAbstractItemModel::dataChanged,
                         [&](const QModelIndex &, const QModelIndex &, const QVector<int> &r) { ++announced; roles = r; });
        CHECK(sailfish::ChannelAvatars::instance()->learn(QString::fromLatin1(kNasa), QStringLiteral("https://yt3.ggpht.com/nasa=s176")));
        CHECK(catalog_test::waitFor([&] { return announced > 0; }));
        CHECK(roles == QVector<int>() << sailfish::CatalogRowsModel::ChannelAvatarRole);
        for (int i = 0; i < model.count(); ++i)
            CHECK_EQ(model.get(i).value(QStringLiteral("channelAvatar")).toString(), QStringLiteral("https://yt3.ggpht.com/nasa=s176"));
        CHECK_EQ(sailfish::ChannelAvatars::instance()->get(QString::fromLatin1(kNasa)), QStringLiteral("https://yt3.ggpht.com/nasa=s176"));
        CHECK(sailfish::ChannelAvatars::instance()->revision() > 0);
    }
    {
        // With enrichment (the default): the live-tab answer of the refresh names the avatar.
        AvatarBook::instance().clear();
        FeedService service(&rig.net, &rig.client, nullptr, nullptr, rig.clock());
        sailfish::FeedModel model(&service);
        service.setChannels(ChannelList(1, ChannelRef(kNasa, "NASA")));
        service.start();
        CHECK(waitIdle(service));
        CHECK(browses >= 1);
        const QString avatar = model.get(0).value(QStringLiteral("channelAvatar")).toString();
        CHECK(avatar.startsWith(QLatin1String("https://")));
        CHECK(avatar.contains(QLatin1String("=s176")));
        CHECK_EQ(avatar, QString::fromStdString(AvatarBook::instance().lookup(kNasa)));
    }
    {
        // thumbnailLarge: the 1280x720 picture for full-width cards; the 480x360 one (it exists
        // for every video) as soon as the first is known to be missing, announced to the view.
        ThumbnailBook::instance().clear();
        FeedService service(&rig.net, &rig.client, nullptr, nullptr, rig.clock());
        service.setLiveEnrichment(false);
        sailfish::FeedModel model(&service);
        service.setChannels(ChannelList(1, ChannelRef(kNasa, "NASA")));
        service.start();
        CHECK(waitIdle(service));
        CHECK(model.count() > 1);
        const QString id = model.get(0).value(QStringLiteral("videoId")).toString();
        const QString other = model.get(1).value(QStringLiteral("videoId")).toString();
        CHECK_EQ(model.get(0).value(QStringLiteral("thumbnailLarge")).toString(), QStringLiteral("https://i.ytimg.com/vi/%1/hq720.jpg").arg(id));
        CHECK_EQ(model.get(0).value(QStringLiteral("thumbnail")).toString(), QStringLiteral("https://i.ytimg.com/vi/%1/mqdefault.jpg").arg(id));   // kept for compact rows
        QVector<int> roles;
        QObject::connect(&model, &QAbstractItemModel::dataChanged,
                         [&](const QModelIndex &, const QModelIndex &, const QVector<int> &r) { roles = r; });
        CHECK(ThumbnailBook::instance().markLargeMissing(id.toStdString()));   // what the image loader does on a 404
        CHECK(catalog_test::waitFor([&] { return !roles.isEmpty(); }));
        CHECK(roles == QVector<int>() << sailfish::CatalogRowsModel::ThumbnailLargeRole);
        CHECK_EQ(model.get(0).value(QStringLiteral("thumbnailLarge")).toString(), QStringLiteral("https://i.ytimg.com/vi/%1/hqdefault.jpg").arg(id));
        CHECK_EQ(model.get(1).value(QStringLiteral("thumbnailLarge")).toString(), QStringLiteral("https://i.ytimg.com/vi/%1/hq720.jpg").arg(other));
        CHECK_EQ(sailfish::LargeThumbnails::instance()->get(id), QStringLiteral("https://i.ytimg.com/vi/%1/hqdefault.jpg").arg(id));
        CHECK(sailfish::LargeThumbnails::instance()->get(QStringLiteral("nope")).isEmpty());
        CHECK(sailfish::LargeThumbnails::instance()->revision() > 0);
        // Rows whose picture is not a video's keep it (a channel avatar, a vertical frame).
        CHECK_EQ(sailfish::LargeThumbnails::forThumbnail("https://yt3.ggpht.com/a=s176"), QStringLiteral("https://yt3.ggpht.com/a=s176"));
        ThumbnailBook::instance().clear();
    }
    // A row's own avatar wins over the book, and survives the on-disk caches.
    Item own;
    own.id = "aaaaaaaaaaa";
    own.channelId = kNasa;
    own.avatarUrl = "https://yt3.ggpht.com/own=s176";
    CHECK_EQ(sailfish::ChannelAvatars::forRow(own), QStringLiteral("https://yt3.ggpht.com/own=s176"));
    CHECK_EQ(cachedItemFromJson(cachedItemToJson(own)).avatarUrl, own.avatarUrl);
    Item channelRow;
    channelRow.kind = ItemKind::Channel;
    channelRow.id = kNasa;
    CHECK_EQ(sailfish::ChannelAvatars::forRow(channelRow), QString::fromStdString(AvatarBook::instance().lookup(kNasa)));
    AvatarBook::instance().clear();
}

// SET-01 "Hide Shorts" (content/hideShorts) reaches the Subscriptions feed and Home through
// ContentFilters, the object the composition feeds from the application settings.
static void hideShortsSetting() {
    auto confirmedShorts = [](const std::vector<Item> &rows) {
        int n = 0;
        for (const Item &it : rows) if (it.kind == ItemKind::Short && isConfirmedShort(it.shortEvidence)) ++n;
        return n;
    };
    CHECK_EQ(QString::fromLatin1(sailfish::ContentFilters::kHideShortsKey), QStringLiteral("content/hideShorts"));

    // The feed. The recorded RSS answer has uploads whose link is a Shorts link.
    {
        Rig rig;
        rig.net.handler = [](const HttpRequest &r) {
            const Req q = parseReq(r);
            if (q.rss && q.channel == QLatin1String(kNasa)) return ScriptedTransport::Answer(200, catalog_test::fixture(QStringLiteral("rss_channel.xml")));
            if (q.liveTab()) return ScriptedTransport::Answer(200, catalog_test::fixture(QStringLiteral("channel_live.json")));
            return ScriptedTransport::Answer(500, "");
        };
        FeedService service(&rig.net, &rig.client, nullptr, nullptr, rig.clock());
        sailfish::FeedModel model(&service);
        ChannelList list;
        list.push_back(ChannelRef(kNasa, "NASA"));
        service.setChannels(list);
        service.start();
        CHECK(waitIdle(service));
        const int all = static_cast<int>(service.merged().rows.size());
        const int shorts = confirmedShorts(service.merged().rows);
        CHECK(shorts >= 1);
        CHECK(all > shorts);
        CHECK(catalog_test::waitFor([&] { return model.count() == all; }));
        CHECK_EQ(model.hiddenCount(), 0);

        // The library's hide-watched hook is in the same filter and has to survive.
        RowFilterSettings hooked = service.rowFilter();
        hooked.hideWatched = [](const std::string &) { return false; };
        service.setRowFilter(hooked);

        sailfish::ContentFilters filters;
        int changes = 0;
        QObject::connect(&filters, &sailfish::ContentFilters::changed, [&changes] { ++changes; });
        filters.addList([&service] { return service.rowFilter(); },
                        [&service](const RowFilterSettings &f) { service.setRowFilter(f); });
        CHECK_EQ(filters.lists(), 1);
        CHECK(!filters.hideShorts());
        CHECK(!service.rowFilter().hideShorts);          // off: nothing was written

        const int sent = rig.net.requests.size();
        filters.setHideShorts(true);
        CHECK(filters.hideShorts());
        CHECK_EQ(changes, 1);
        CHECK(service.rowFilter().hideShorts);
        CHECK(static_cast<bool>(service.rowFilter().hideWatched));
        CHECK_EQ(static_cast<int>(service.merged().rows.size()), all - shorts);
        CHECK_EQ(service.merged().hiddenShorts, shorts);
        CHECK_EQ(confirmedShorts(service.merged().rows), 0);
        CHECK_EQ(rig.net.requests.size(), sent);         // merged again from the cache, offline
        CHECK(catalog_test::waitFor([&] { return model.count() == all - shorts; }));
        CHECK_EQ(model.hiddenCount(), shorts);           // hidden rows are counted, never dropped silently
        filters.setHideShorts(true);                     // unchanged: no signal, no work
        CHECK_EQ(changes, 1);

        filters.setHideShorts(false);
        CHECK_EQ(static_cast<int>(service.merged().rows.size()), all);
        CHECK_EQ(service.merged().hiddenShorts, 0);
        CHECK(static_cast<bool>(service.rowFilter().hideWatched));
        CHECK(catalog_test::waitFor([&] { return model.count() == all; }));

        // A list registered while the switch is on takes it at once; empty accessors are ignored.
        filters.setHideShorts(true);
        RowFilterSettings late;
        filters.addList([&late] { return late; }, [&late](const RowFilterSettings &f) { late = f; });
        CHECK(late.hideShorts);
        filters.addList(sailfish::ContentFilters::Getter(), sailfish::ContentFilters::Setter());
        CHECK_EQ(filters.lists(), 2);
    }

    // Home (Popular). Derived from the recorded popular page: the first row's time overlay gets
    // the Shorts style, which is how a Short is marked among video rows.
    {
        QByteArray popular = catalog_test::fixture(QStringLiteral("home_popular_es.json"));
        const int overlay = popular.indexOf("thumbnailOverlayTimeStatusRenderer");
        const int style = popular.indexOf("\"DEFAULT\"", overlay);
        CHECK(overlay > 0 && style > overlay);
        if (style > 0) popular.replace(style, 9, "\"SHORTS\"");
        Rig rig;
        int searches = 0;
        rig.net.handler = [&](const HttpRequest &r) {
            if (r.url.path().endsWith(QLatin1String("/search"))) { ++searches; return ScriptedTransport::Answer(200, popular); }
            return ScriptedTransport::Answer(500, "");
        };
        qint64 now = 0;
        HomeService service(&rig.client, false);
        service.setClock([&now] { return now; });
        sailfish::RecommendedModel model(&service);
        sailfish::ContentFilters filters;
        filters.addList([&service] { return service.rowFilter(); },
                        [&service](const RowFilterSettings &f) { service.setRowFilter(f); });

        // Nothing loaded yet: the switch is stored and starts no request.
        filters.setHideShorts(true);
        CHECK(service.rowFilter().hideShorts);
        CHECK(!service.busy());
        CHECK_EQ(searches, 0);
        filters.setHideShorts(false);
        CHECK_EQ(searches, 0);

        service.load();
        CHECK(catalog_test::waitFor([&service] { return !service.busy(); }));
        CHECK_EQ(searches, 1);
        const int all = static_cast<int>(service.items().size());
        CHECK(all >= 10);
        CHECK_EQ(confirmedShorts(service.items()), 1);
        CHECK_EQ(service.hiddenCount(), 0);
        CHECK(catalog_test::waitFor([&] { return model.count() == all; }));

        // On: the list is loaded again with the filter, from the popular page 1 cache.
        filters.setHideShorts(true);
        CHECK(!service.busy());
        CHECK_EQ(searches, 1);
        CHECK_EQ(static_cast<int>(service.items().size()), all - 1);
        CHECK_EQ(confirmedShorts(service.items()), 0);
        CHECK_EQ(service.hiddenCount(), 1);
        CHECK(catalog_test::waitFor([&] { return model.count() == all - 1; }));
        // Off again: the Short is back.
        filters.setHideShorts(false);
        CHECK_EQ(static_cast<int>(service.items().size()), all);
        CHECK_EQ(service.hiddenCount(), 0);
        CHECK_EQ(searches, 1);

        // What the Shorts knowledge already says about a row counts too (rows without a marker).
        const std::string known = service.items().back().id;
        service.setShortsLookup([known](const std::string &id) { return id == known ? ShortEvidence::ShortsTab : ShortEvidence::None; });
        filters.setHideShorts(true);
        CHECK_EQ(static_cast<int>(service.items().size()), all - 2);
        CHECK_EQ(service.hiddenCount(), 2);

        // Other filter fields changing does not reload when the hiding switches stay the same.
        RowFilterSettings same = service.rowFilter();
        same.hideWatched = [](const std::string &) { return false; };
        const int before = static_cast<int>(service.items().size());
        service.setRowFilter(same);
        CHECK_EQ(static_cast<int>(service.items().size()), before);
        CHECK_EQ(searches, 1);
    }
}

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    rssParse();
    scheduleTable();
    mergeOrder();
    depthSurvivesRefresh();
    idSequence();
    persistence();
    channelTabs();
    deadChannel();
    tierFallbacks();
    dueAndForced();
    parallelCap();
    modelReplacement();
    depth();
    liveAndMixed();
    cacheLaunch();
    home();
    hideShortsSetting();
    channelAvatars();
    return catalog_test::finish("catalog_feed");
}
