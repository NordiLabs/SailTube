// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Channels (DSC-12), channel headers (DSC-03), the one Shorts rule and its stores (DSC-17),
// members-only / restricted rows (DSC-18) and the remote playlist preview (LIB-03 / LNK-04),
// on the recorded @NASA fixtures. Variants derived in code are marked "derived:".
#include "tests/catalog_common/TestSupport.h"
#include "Core/Catalog/Domain/AvatarBook.h"

#include "Clients/Sailfish/Presentation/ChannelModel.h"
#include "Clients/Sailfish/Presentation/PlaylistModel.h"
#include "Clients/Sailfish/Presentation/QueueSeeder.h"
#include "Core/Catalog/Domain/ChannelReference.h"
#include "Core/Catalog/Domain/Images.h"
#include "Core/Catalog/Domain/LruStore.h"
#include "Core/Catalog/Domain/RowFilter.h"
#include "Core/Catalog/Domain/ShortsKnowledge.h"
#include "Core/Catalog/Infrastructure/ChannelService.h"
#include "Core/Catalog/Infrastructure/ClientIdentity.h"
#include "Core/Catalog/Infrastructure/InnerTubeClient.h"
#include "Core/Catalog/Infrastructure/Json.h"
#include "Core/Catalog/Infrastructure/PlaylistService.h"
#include "Core/Catalog/Infrastructure/ShortsStore.h"

#include <QDateTime>
#include <QFileInfo>
#include <QJsonArray>
#include <QTemporaryDir>
#include <QXmlStreamReader>
#include <algorithm>
#include <set>

using namespace nordtile::video;
using namespace nordtile::video::catalog;
using catalog_test::FakeTransport;
using sailfish::ChannelModel;
using sailfish::PlaylistModel;

namespace {

const char *const kNasa = "UCLA_DiR1FfKNvjuUpBHmylQ";
const char *const kVideosParams = "EgZ2aWRlb3PyBgQKAjoA";
const char *const kShortsParams = "EgZzaG9ydHPyBgUKA5oBAA==";
const char *const kStreamsParams = "EgdzdHJlYW1z8gYECgJ6AA==";
const char *const kPlaylistsParams = "EglwbGF5bGlzdHPyBgoKCEIGCgIQaCIA";

QJsonObject fx(const char *name) { return catalog_test::fixtureJson(QString::fromLatin1(name) + QStringLiteral(".json")); }
QByteArray bytes(const QJsonObject &o) { return QJsonDocument(o).toJson(QJsonDocument::Compact); }

InnerTubeReply okReply(const QJsonObject &json) {
    InnerTubeReply r;
    r.outcome = Outcome::Ok;
    r.httpStatus = 200;
    r.json = json;
    return r;
}

// ---- JSON surgery for derived variants -------------------------------------------------------

QJsonValue setPath(const QJsonValue &root, const QStringList &path, int i, const QJsonValue &value, bool remove) {
    if (i == path.size()) return value;
    const QString &seg = path.at(i);
    bool isIndex = false;
    const int idx = seg.toInt(&isIndex);
    const bool last = i == path.size() - 1;
    if (isIndex && root.isArray()) {
        QJsonArray a = root.toArray();
        if (remove && last) { a.removeAt(idx); return a; }
        a.replace(idx, setPath(a.at(idx), path, i + 1, value, remove));
        return a;
    }
    QJsonObject o = root.toObject();
    if (remove && last) { o.remove(seg); return o; }
    o.insert(seg, setPath(o.value(seg), path, i + 1, value, remove));
    return o;
}

QJsonObject withValue(const QJsonObject &root, const QString &dotted, const QJsonValue &value) {
    return setPath(root, dotted.split(QLatin1Char('.')), 0, value, false).toObject();
}

QJsonObject without(const QJsonObject &root, const QString &dotted) {
    return setPath(root, dotted.split(QLatin1Char('.')), 0, QJsonValue(), true).toObject();
}

// Rewrites every object stored under `key` below `v`.
QJsonValue mapKey(const QJsonValue &v, const QString &key, const std::function<QJsonObject(QJsonObject)> &f) {
    if (v.isArray()) {
        QJsonArray a = v.toArray();
        for (int i = 0; i < a.size(); ++i) a.replace(i, mapKey(a.at(i), key, f));
        return a;
    }
    if (!v.isObject()) return v;
    QJsonObject o = v.toObject();
    for (auto it = o.begin(); it != o.end(); ++it) {
        QJsonValue child = mapKey(it.value(), key, f);
        if (it.key() == key && child.isObject()) child = f(child.toObject());
        it.value() = child;
    }
    return o;
}

int selectedTabIndex(const QJsonObject &root) {
    const QJsonArray tabs = json::at(root, QStringLiteral("contents.twoColumnBrowseResultsRenderer.tabs")).toArray();
    for (int i = 0; i < tabs.size(); ++i)
        if (tabs.at(i).toObject().value(QStringLiteral("tabRenderer")).toObject().value(QStringLiteral("selected")).toBool()) return i;
    return -1;
}

QString gridPath(const QJsonObject &root) {
    return QStringLiteral("contents.twoColumnBrowseResultsRenderer.tabs.%1.tabRenderer.content.richGridRenderer.contents")
        .arg(selectedTabIndex(root));
}

// derived: the lockup of grid row `row` rewritten by `f`.
QJsonObject withLockup(const QJsonObject &root, int row, const std::function<QJsonObject(QJsonObject)> &f) {
    const QString path = gridPath(root) + QStringLiteral(".%1.richItemRenderer.content.lockupViewModel").arg(row);
    return withValue(root, path, f(json::at(root, path).toObject()));
}

QJsonObject withBadgeText(const QJsonObject &lockup, const QString &text) {
    return mapKey(lockup, QStringLiteral("thumbnailBadgeViewModel"), [text](QJsonObject b) {
        b.insert(QStringLiteral("text"), text);
        return b;
    }).toObject();
}

QString lockupId(const QJsonObject &root, int row) {
    return json::at(root, gridPath(root) + QStringLiteral(".%1.richItemRenderer.content.lockupViewModel.contentId").arg(row)).toString();
}

// derived: an answer with one tab ("/videos", "/shorts") removed from the channel's tab list.
QJsonObject withoutTab(QJsonObject root, const char *suffix) {
    const QJsonArray tabs = json::at(root, QStringLiteral("contents.twoColumnBrowseResultsRenderer.tabs")).toArray();
    QJsonArray kept;
    for (const QJsonValue &t : tabs) {
        const QString url = json::at(t, QStringLiteral("tabRenderer.endpoint.commandMetadata.webCommandMetadata.url")).toString();
        if (url.endsWith(QLatin1String(suffix))) continue;
        kept.append(t);
    }
    return withValue(root, QStringLiteral("contents.twoColumnBrowseResultsRenderer.tabs"), kept);
}

// derived: channel_home with the Videos tab removed from the tab list.
QJsonObject homeWithoutVideos() { return withoutTab(fx("channel_home"), "/videos"); }

// A small RSS reader for the fake RSS hook (the feed package owns the real parser).
std::vector<Item> readRss(const QByteArray &xml) {
    std::vector<Item> rows;
    QXmlStreamReader r(xml);
    Item cur;
    bool inEntry = false;
    while (!r.atEnd()) {
        r.readNext();
        if (r.isStartElement()) {
            const QString name = r.qualifiedName().toString();
            if (name == QLatin1String("entry")) { cur = Item(); inEntry = true; }
            else if (!inEntry) continue;
            else if (name == QLatin1String("yt:videoId")) cur.id = r.readElementText().toStdString();
            else if (name == QLatin1String("yt:channelId")) cur.channelId = r.readElementText().toStdString();
            else if (name == QLatin1String("title")) cur.title = r.readElementText().toStdString();
            else if (name == QLatin1String("published"))
                cur.publishedEpoch = QDateTime::fromString(r.readElementText(), Qt::ISODate).toMSecsSinceEpoch() / 1000;
            else if (name == QLatin1String("link") && r.attributes().value(QStringLiteral("rel")) == QLatin1String("alternate")) {
                if (r.attributes().value(QStringLiteral("href")).toString().contains(QLatin1String("/shorts/"))) {
                    cur.kind = ItemKind::Short;
                    cur.shortEvidence = ShortEvidence::RssLink;
                }
            } else if (name == QLatin1String("media:statistics"))
                cur.viewCount = r.attributes().value(QStringLiteral("views")).toString().toLongLong();
        } else if (r.isEndElement() && r.qualifiedName() == QLatin1String("entry")) {
            cur.thumbnailUrl = listThumbnailUrl(cur.id);
            rows.push_back(cur);
            inEntry = false;
        }
    }
    return rows;
}

ChannelService::RssFetch fakeRss(int *calls) {
    return [calls](const QString &channelId, QObject *context, ChannelService::RssDone done) {
        ++*calls;
        const std::vector<Item> rows = channelId == QLatin1String(kNasa)
                                           ? readRss(catalog_test::fixture(QStringLiteral("rss_channel.xml"))) : std::vector<Item>();
        QTimer::singleShot(0, context, [done, rows] { done(rows.empty() ? Outcome::Unavailable : Outcome::Ok, rows); });
    };
}

// Serves the @NASA channel: resolve, every tab, page 2 of Videos. InnerTube answers are
// delayed so the RSS paint comes first.
void serveNasa(FakeTransport &net, const QJsonObject &videos, int delayMs = 30) {
    const QJsonObject p1 = fx("channel_videos");
    const QString token = json::continuationToken(json::findFirst(p1, QStringLiteral("continuationItemRenderer")));
    net.on(QStringLiteral("navigation/resolve_url"), QByteArray(), 200, catalog_test::fixture(QStringLiteral("resolve_handle.json")), delayMs);
    net.on(QStringLiteral("browse"), token.toLatin1(), 200, catalog_test::fixture(QStringLiteral("channel_videos_p2.json")), delayMs);
    net.on(QStringLiteral("browse"), kVideosParams, 200, bytes(videos), delayMs);
    net.on(QStringLiteral("browse"), kShortsParams, 200, catalog_test::fixture(QStringLiteral("channel_shorts.json")), delayMs);
    net.on(QStringLiteral("browse"), kStreamsParams, 200, catalog_test::fixture(QStringLiteral("channel_live.json")), delayMs);
    net.on(QStringLiteral("browse"), kPlaylistsParams, 200, catalog_test::fixture(QStringLiteral("channel_playlists.json")), delayMs);
}

std::vector<std::string> ids(const std::vector<Item> &rows) {
    std::vector<std::string> out;
    for (const Item &r : rows) out.push_back(r.id);
    return out;
}

int countWhere(const std::vector<Item> &rows, const std::function<bool(const Item &)> &p) {
    int n = 0;
    for (const Item &r : rows) if (p(r)) ++n;
    return n;
}

bool unique(const std::vector<Item> &rows) {
    std::set<std::string> s;
    for (const Item &r : rows) if (!s.insert(r.id).second) return false;
    return true;
}

}  // namespace

// ---- domain ----------------------------------------------------------------------------------

static void lruStore() {
    LruStore<std::string, int> lru(3);
    lru.put("a", 1); lru.put("b", 2); lru.put("c", 3);
    CHECK((lru.keys() == std::vector<std::string>{"c", "b", "a"}));
    CHECK(lru.get("a") && *lru.get("a") == 1);                         // touch: a becomes newest
    CHECK((lru.keys() == std::vector<std::string>{"a", "c", "b"}));
    CHECK_EQ(static_cast<int>(lru.put("d", 4)), 1);                    // evicts the least recent (b)
    CHECK(!lru.contains("b") && lru.contains("a") && lru.contains("d"));
    CHECK(lru.peek("c") && (lru.keys() == std::vector<std::string>{"d", "a", "c"}));   // peek keeps order
    lru.put("c", 30);                                                   // replace moves to front
    CHECK_EQ(*lru.peek("c"), 30);
    CHECK_EQ(lru.keys().front(), std::string("c"));
    CHECK_EQ(static_cast<int>(lru.size()), 3);

    // DSC-17 capacities: 3000 durations, 4000 Shorts, recency ordered.
    ShortsKnowledge k;
    CHECK_EQ(static_cast<int>(k.durations().capacity()), 3000);
    CHECK_EQ(static_cast<int>(k.shorts().capacity()), 4000);
    char id[12];
    for (int i = 0; i < 3001; ++i) {
        std::snprintf(id, sizeof id, "v%010d", i);
        k.recordDuration(id, 100 + i);
        if (i == 2998) k.recordDuration("v0000000000", 100);           // the oldest was used again
    }
    CHECK_EQ(static_cast<int>(k.durationCount()), 3000);
    CHECK_EQ(k.duration("v0000000000"), 100);                          // kept: recently used
    CHECK_EQ(k.duration("v0000000001"), -1);                           // evicted: least recently used
    for (int i = 0; i < 4001; ++i) {
        std::snprintf(id, sizeof id, "s%010d", i);
        k.markShort(id, ShortEvidence::Renderer);
    }
    CHECK_EQ(static_cast<int>(k.shortsCount()), 4000);
    CHECK(k.evidence("s0000000000") == ShortEvidence::None);
    CHECK(k.evidence("s0000004000") == ShortEvidence::Renderer);
    k.recordDuration("bad id", 10);                                     // not a video id
    k.recordDuration("v0000000000", 0);                                 // not a duration
    CHECK_EQ(k.duration("v0000000000"), 100);

    // Persistence round trip keeps recency order and evidence kinds.
    ShortsKnowledge a;
    a.recordDuration("aaaaaaaaaaa", 170); a.recordDuration("bbbbbbbbbbb", 900);
    a.markShort("ccccccccccc", ShortEvidence::RssLink); a.markShort("ddddddddddd", ShortEvidence::ShortsTab);
    a.markNotShort("aaaaaaaaaaa");
    ShortsKnowledge b;
    CHECK(b.deserialize(a.serialize()));
    CHECK((b.durations().keys() == a.durations().keys()));
    CHECK(b.evidence("ccccccccccc") == ShortEvidence::RssLink);
    CHECK(b.evidence("ddddddddddd") == ShortEvidence::ShortsTab);
    CHECK(b.evidence("aaaaaaaaaaa") == ShortEvidence::NotShort);
    CHECK_EQ(b.duration("bbbbbbbbbbb"), 900);
    CHECK(!b.deserialize("garbage\n"));
}

static void channelReferences() {
    struct Case { const char *in; ChannelReference::Kind kind; const char *value; const char *tab; const char *url; };
    using K = ChannelReference::Kind;
    const Case cases[] = {
        {"UCLA_DiR1FfKNvjuUpBHmylQ", K::ChannelId, kNasa, "", "https://www.youtube.com/channel/UCLA_DiR1FfKNvjuUpBHmylQ"},
        {"https://www.youtube.com/channel/UCLA_DiR1FfKNvjuUpBHmylQ/videos", K::ChannelId, kNasa, "videos", ""},
        {"/channel/UCLA_DiR1FfKNvjuUpBHmylQ", K::ChannelId, kNasa, "", ""},
        {"@NASA", K::Handle, "@NASA", "", "https://www.youtube.com/@NASA"},
        {"https://m.youtube.com/@NASA/shorts?si=abc", K::Handle, "@NASA", "shorts", "https://www.youtube.com/@NASA"},
        {"youtube.com/@NASA/live", K::Handle, "@NASA", "streams", ""},
        {"https://www.youtube.com/c/NASA/playlists", K::Custom, "NASA", "playlists", "https://www.youtube.com/c/NASA"},
        {"https://www.youtube.com/user/NASAtelevision", K::User, "NASAtelevision", "", "https://www.youtube.com/user/NASAtelevision"},
        {"https://www.youtube.com/NASA", K::Custom, "NASA", "", "https://www.youtube.com/NASA"},
        {"https://www.youtube.com/watch?v=9zCH37330f8", K::Invalid, "", "", ""},
        {"https://example.com/@NASA", K::Invalid, "", "", ""},
        {"https://www.youtube.com/channel/notAnId", K::Invalid, "", "", ""},
        {"https://www.youtube.com/@NASA/unknowntab", K::Invalid, "", "", ""},
        {"", K::Invalid, "", "", ""},
    };
    for (const Case &c : cases) {
        const ChannelReference r = parseChannelReference(c.in);
        CHECK(r.kind == c.kind);
        if (r.kind != c.kind) std::printf("  reference '%s'\n", c.in);
        CHECK_EQ(r.value, std::string(c.value));
        CHECK_EQ(r.tab, std::string(c.tab));
        if (*c.url) CHECK_EQ(r.resolveUrl(), std::string(c.url));
    }
    CHECK_EQ(parseChannelReference("@NASA").key(), parseChannelReference("https://youtube.com/@nasa/videos").key());
    CHECK(isChannelId(kNasa) && !isChannelId("UCshort") && !isChannelId("XXLA_DiR1FfKNvjuUpBHmylQ"));
}

// ---- headers and tabs (DSC-03, DSC-12) ----------------------------------------------------

static void headers() {
    const QJsonObject home = fx("channel_home");
    const ChannelHeader h = ChannelService::parseHeader(home, QStringLiteral("en"));
    CHECK_EQ(h.source, QStringLiteral("pageHeaderViewModel"));
    CHECK_EQ(h.channelId, QString::fromLatin1(kNasa));
    CHECK_EQ(h.title, QStringLiteral("NASA"));
    CHECK_EQ(h.handle, QStringLiteral("@NASA"));
    CHECK(h.subscriberCount > 1000000);
    CHECK(!h.subscriberText.isEmpty());
    CHECK(h.videoCount > 1000);
    CHECK(h.avatarUrl.startsWith(QLatin1String("https://")));
    CHECK(h.avatarUrl.contains(QLatin1String("=s176")));
    CHECK(h.rssUrl.contains(QLatin1String("channel_id=UCLA_DiR1FfKNvjuUpBHmylQ")));

    // derived: the same header in the older c4TabbedHeaderRenderer shape, without the
    // channelMetadataRenderer (the fixture suite has no recorded c4 answer any more).
    QJsonObject c4;
    c4.insert(QStringLiteral("channelId"), QString::fromLatin1(kNasa));
    c4.insert(QStringLiteral("title"), QJsonObject{{QStringLiteral("simpleText"), QStringLiteral("NASA")}});
    c4.insert(QStringLiteral("avatar"), QJsonObject{{QStringLiteral("thumbnails"), QJsonArray{
        QJsonObject{{QStringLiteral("url"), QStringLiteral("//yt3.ggpht.com/abc=s88-c-k")}, {QStringLiteral("width"), 88}, {QStringLiteral("height"), 88}},
        QJsonObject{{QStringLiteral("url"), QStringLiteral("//yt3.ggpht.com/abc=s176-c-k")}, {QStringLiteral("width"), 176}, {QStringLiteral("height"), 176}}}}});
    c4.insert(QStringLiteral("subscriberCountText"), QJsonObject{{QStringLiteral("simpleText"), QStringLiteral("15.1M subscribers")}});
    c4.insert(QStringLiteral("videosCountText"), QJsonObject{{QStringLiteral("runs"), QJsonArray{
        QJsonObject{{QStringLiteral("text"), QStringLiteral("6.2K")}}, QJsonObject{{QStringLiteral("text"), QStringLiteral(" videos")}}}}});
    c4.insert(QStringLiteral("channelHandleText"), QJsonObject{{QStringLiteral("runs"), QJsonArray{QJsonObject{{QStringLiteral("text"), QStringLiteral("@NASA")}}}}});
    QJsonObject old;
    old.insert(QStringLiteral("header"), QJsonObject{{QStringLiteral("c4TabbedHeaderRenderer"), c4}});
    const ChannelHeader o = ChannelService::parseHeader(old, QStringLiteral("en"));
    CHECK_EQ(o.source, QStringLiteral("c4TabbedHeaderRenderer"));
    CHECK_EQ(o.channelId, QString::fromLatin1(kNasa));
    CHECK_EQ(o.title, QStringLiteral("NASA"));
    CHECK_EQ(o.handle, QStringLiteral("@NASA"));
    CHECK_EQ(o.subscriberCount, static_cast<qint64>(15100000));
    CHECK_EQ(o.videoCount, static_cast<qint64>(6200));
    CHECK_EQ(o.avatarUrl, QStringLiteral("https://yt3.ggpht.com/abc=s176-c-k"));

    // The tab list: keys from the URL suffix, params decoded from the endpoints.
    const QList<ChannelTab> tabs = ChannelService::parseTabs(home);
    QStringList keys;
    for (const ChannelTab &t : tabs) keys << t.key;
    CHECK_EQ(keys.join(QLatin1Char(',')), QStringLiteral("featured,videos,shorts,streams,podcasts,playlists,posts"));
    for (const ChannelTab &t : tabs) {
        if (t.key == QLatin1String("shorts")) CHECK_EQ(t.params, QString::fromLatin1(kShortsParams));
        if (t.key == QLatin1String("featured")) CHECK(t.selected);
        CHECK(!t.title.isEmpty());
    }
    CHECK_EQ(ChannelService::fallbackTabParams(QStringLiteral("videos")), QString::fromLatin1(kVideosParams));
    CHECK_EQ(ChannelService::fallbackTabParams(QStringLiteral("live")), QString::fromLatin1(kStreamsParams));
    // The Home tab is the web app's "featured" tab (its recorded params).
    CHECK_EQ(ChannelService::fallbackTabParams(QStringLiteral("home")), QStringLiteral("EghmZWF0dXJlZPIGBAoCMgA="));
    CHECK_EQ(ChannelService::fallbackTabParams(QStringLiteral("releases")), QStringLiteral("EghyZWxlYXNlcw=="));   // {2: "releases"}
}

static void tabFixtures() {
    const QString en = QStringLiteral("en");
    const QString nasa = QString::fromLatin1(kNasa);
    // Videos.
    const ChannelPage v = ChannelService::parseBrowse(okReply(fx("channel_videos")), nasa, QStringLiteral("videos"), en);
    CHECK(v.outcome == Outcome::Ok);
    CHECK_EQ(v.selectedTab, QStringLiteral("videos"));
    CHECK(v.items.size() >= 25);
    CHECK(!v.continuation.isEmpty());
    CHECK(unique(v.items));
    CHECK_EQ(countWhere(v.items, [](const Item &i) { return i.kind == ItemKind::Video && i.id.size() == 11 && !i.title.empty(); }),
             static_cast<int>(v.items.size()));
    CHECK_EQ(countWhere(v.items, [](const Item &i) { return i.durationSeconds > 0 && i.viewCount >= 0 && i.publishedAgeSeconds >= 0; }),
             static_cast<int>(v.items.size()));
    CHECK_EQ(countWhere(v.items, [](const Item &i) { return i.channel == "NASA" && i.channelId == kNasa; }), static_cast<int>(v.items.size()));
    CHECK_EQ(v.header.title, QStringLiteral("NASA"));
    CHECK(v.hasTab(QStringLiteral("shorts")) && v.hasTab(QStringLiteral("streams")) && v.hasTab(QStringLiteral("playlists")));
    CHECK(v.unknown.isEmpty());
    // Videos page 2.
    const ChannelPage v2 = ChannelService::parseContinuation(okReply(fx("channel_videos_p2")), nasa, QStringLiteral("videos"), en);
    CHECK(v2.outcome == Outcome::Ok);
    CHECK(v2.items.size() >= 25);
    CHECK(!v2.continuation.isEmpty());
    std::vector<Item> both = v.items;
    both.insert(both.end(), v2.items.begin(), v2.items.end());
    CHECK(unique(both));
    CHECK_EQ(countWhere(v2.items, [](const Item &i) { return i.channelId == kNasa; }), static_cast<int>(v2.items.size()));
    // Shorts.
    const ChannelPage s = ChannelService::parseBrowse(okReply(fx("channel_shorts")), nasa, QStringLiteral("shorts"), en);
    CHECK(s.outcome == Outcome::Ok);
    CHECK(s.items.size() >= 20);
    CHECK_EQ(countWhere(s.items, [](const Item &i) { return i.kind == ItemKind::Short && i.shortEvidence == ShortEvidence::Renderer && i.id.size() == 11; }),
             static_cast<int>(s.items.size()));
    CHECK_EQ(countWhere(s.items, [](const Item &i) { return i.channelId == kNasa; }), static_cast<int>(s.items.size()));
    // Live: running streams and past streams with lengths.
    const ChannelPage l = ChannelService::parseBrowse(okReply(fx("channel_live")), nasa, QStringLiteral("live"), en);
    CHECK(l.outcome == Outcome::Ok);
    CHECK_EQ(l.selectedTab, QStringLiteral("streams"));
    CHECK(countWhere(l.items, [](const Item &i) { return i.liveStatus == media::LiveStatus::IsLive; }) >= 1);
    CHECK(countWhere(l.items, [](const Item &i) { return i.durationSeconds > 0; }) >= 20);
    CHECK(!l.continuation.isEmpty());
    // Playlists.
    const ChannelPage p = ChannelService::parseBrowse(okReply(fx("channel_playlists")), nasa, QStringLiteral("playlists"), en);
    CHECK(p.outcome == Outcome::Ok);
    CHECK(p.items.size() >= 20);
    CHECK_EQ(countWhere(p.items, [](const Item &i) { return i.kind == ItemKind::Playlist && !i.id.empty() && !i.title.empty(); }),
             static_cast<int>(p.items.size()));
    CHECK(countWhere(p.items, [](const Item &i) { return i.videoCount > 0; }) >= 20);
    // The channel's own playlists carry its name (a playlist curated by another channel keeps its owner).
    CHECK_EQ(countWhere(p.items, [](const Item &i) { return i.channelId == kNasa && i.channel == "NASA"; }),
             countWhere(p.items, [](const Item &i) { return i.channelId == kNasa; }));
    CHECK(countWhere(p.items, [](const Item &i) { return i.channelId == kNasa; }) >= 20);
    CHECK(!p.continuation.isEmpty());
    // Other languages parse into the same rows (counts from the hl tables).
    for (const char *lang : {"de", "ja"}) {
        const ChannelPage x = ChannelService::parseBrowse(okReply(fx(lang[0] == 'd' ? "channel_videos_de" : "channel_videos_ja")), nasa,
                                                          QStringLiteral("videos"), QString::fromLatin1(lang));
        CHECK(x.outcome == Outcome::Ok);
        CHECK(x.items.size() >= 25);
        CHECK(countWhere(x.items, [](const Item &i) { return i.viewCount >= 0 && i.durationSeconds > 0; }) >= 25);
        CHECK(x.header.subscriberCount > 1000000);
    }

    // DSC-01: valid empty versus shape drift versus HTTP.
    const QJsonObject videos = fx("channel_videos");
    // derived: a channel whose Videos tab exists but holds no uploads.
    const ChannelPage empty = ChannelService::parseBrowse(okReply(withValue(videos, gridPath(videos), QJsonArray())), nasa,
                                                          QStringLiteral("videos"), en);
    CHECK(empty.outcome == Outcome::ValidEmpty);
    CHECK(empty.items.empty() && !empty.header.isEmpty());
    // derived: the tab container is missing.
    const ChannelPage drift = ChannelService::parseBrowse(okReply(without(videos, QStringLiteral("contents.twoColumnBrowseResultsRenderer"))),
                                                          nasa, QStringLiteral("videos"), en);
    CHECK(drift.outcome == Outcome::ShapeDrift);
    // derived: the selected tab without a grid or section list.
    const QString content = QStringLiteral("contents.twoColumnBrowseResultsRenderer.tabs.%1.tabRenderer.content").arg(selectedTabIndex(videos));
    const ChannelPage drift2 = ChannelService::parseBrowse(okReply(withValue(videos, content, QJsonObject{{QStringLiteral("futureGrid"), QJsonObject()}})),
                                                           nasa, QStringLiteral("videos"), en);
    CHECK(drift2.outcome == Outcome::ShapeDrift);
    // derived: a terminated channel (ERROR alert, no tabs) is unavailable, not drift.
    QJsonObject gone;
    gone.insert(QStringLiteral("alerts"), QJsonArray{QJsonObject{{QStringLiteral("alertRenderer"), QJsonObject{
        {QStringLiteral("type"), QStringLiteral("ERROR")}, {QStringLiteral("text"), QJsonObject{{QStringLiteral("simpleText"), QStringLiteral("This channel does not exist.")}}}}}}});
    CHECK(ChannelService::parseBrowse(okReply(gone), nasa, QStringLiteral("videos"), en).outcome == Outcome::Unavailable);
    InnerTubeReply http;
    http.outcome = Outcome::Http5xx; http.httpStatus = 503;
    CHECK(ChannelService::parseBrowse(http, nasa, QStringLiteral("videos"), en).outcome == Outcome::Http5xx);
    http.outcome = Outcome::RateLimited; http.httpStatus = 429;
    CHECK(ChannelService::parseContinuation(http, nasa, QStringLiteral("videos"), en).outcome == Outcome::RateLimited);
    // derived: continuation answers.
    CHECK(ChannelService::parseContinuation(okReply(QJsonObject()), nasa, QStringLiteral("videos"), en).outcome == Outcome::ShapeDrift);
    const QJsonObject p2 = fx("channel_videos_p2");
    CHECK(ChannelService::parseContinuation(okReply(withValue(p2, QStringLiteral("onResponseReceivedActions.0.appendContinuationItemsAction.continuationItems"), QJsonArray())),
                                            nasa, QStringLiteral("videos"), en).outcome == Outcome::ValidEmpty);
    // derived: the Videos tab requested from a channel without one.
    const ChannelPage missing = ChannelService::parseBrowse(okReply(homeWithoutVideos()), nasa, QStringLiteral("videos"), en);
    CHECK(missing.tabMissing);
    CHECK(missing.items.empty());
    CHECK(missing.hasTab(QStringLiteral("playlists")));
}

// ---- service flow ------------------------------------------------------------------------------

static void serviceFlow() {
    ClientIdentity identity(nullptr, QString());
    WorkLane lane(QStringLiteral("channel"), 1);
    DiscoveryLocale loc;
    CatalogCounters counters;
    {
        FakeTransport net;
        serveNasa(net, fx("channel_videos"), 0);
        InnerTubeClient client(&net, &identity, &lane, loc);
        ChannelService service(&client, nullptr, &counters);
        QString got; Outcome o = Outcome::Network; bool done = false;
        service.resolve(QStringLiteral("@NASA"), &net, [&](Outcome oo, QString id) { o = oo; got = id; done = true; });
        CHECK(catalog_test::waitFor([&] { return done; }));
        CHECK(o == Outcome::Ok);
        CHECK_EQ(got, QString::fromLatin1(kNasa));
        CHECK_EQ(net.count(QStringLiteral("navigation/resolve_url"), "\"url\":\"https://www.youtube.com/@NASA\""), 1);
        // Session cache: a tab URL of the same handle needs no second call.
        done = false;
        service.resolve(QStringLiteral("https://www.youtube.com/@nasa/videos"), &net, [&](Outcome oo, QString id) { o = oo; got = id; done = true; });
        CHECK(done && o == Outcome::Ok && got == QLatin1String(kNasa));
        CHECK_EQ(net.count(QStringLiteral("navigation/resolve_url")), 1);
        CHECK_EQ(service.cachedChannelId(QStringLiteral("@NASA")), QString::fromLatin1(kNasa));
        // UC ids need no call at all.
        done = false;
        service.resolve(QStringLiteral("https://www.youtube.com/channel/UCLA_DiR1FfKNvjuUpBHmylQ/streams"), &net,
                        [&](Outcome oo, QString id) { o = oo; got = id; done = true; });
        CHECK(done && got == QLatin1String(kNasa));
        CHECK_EQ(net.count(QStringLiteral("navigation/resolve_url")), 1);
        // An invalid reference fails without a request.
        done = false;
        service.resolve(QStringLiteral("https://www.youtube.com/watch?v=9zCH37330f8"), &net, [&](Outcome oo, QString) { o = oo; done = true; });
        CHECK(done && o == Outcome::Unavailable);

        // Videos tab, first with the recorded web params; afterwards the channel's own list.
        ChannelPage page; done = false;
        CHECK(!service.knowsTabs(QString::fromLatin1(kNasa)));
        service.browseTab(QString::fromLatin1(kNasa), QStringLiteral("videos"), &net, [&](ChannelPage p) { page = p; done = true; });
        CHECK(catalog_test::waitFor([&] { return done; }));
        CHECK(page.outcome == Outcome::Ok);
        CHECK(page.items.size() >= 25);
        CHECK(service.knowsTabs(QString::fromLatin1(kNasa)));
        CHECK_EQ(service.tabParams(QString::fromLatin1(kNasa), QStringLiteral("playlists")), QString::fromLatin1(kPlaylistsParams));
        CHECK_EQ(counters.value(Area::Channel, Outcome::Ok), static_cast<qint64>(2));   // resolve + browse
        // Continuation.
        ChannelPage more; done = false;
        service.browseMore(QString::fromLatin1(kNasa), QStringLiteral("videos"), page.continuation, &net, [&](ChannelPage p) { more = p; done = true; });
        CHECK(catalog_test::waitFor([&] { return done; }));
        CHECK(more.outcome == Outcome::Ok);
        CHECK(more.items.size() >= 25);
        CHECK_EQ(countWhere(more.items, [](const Item &i) { return i.channel == "NASA"; }), static_cast<int>(more.items.size()));
        CHECK(net.count(QStringLiteral("browse"), "\"continuation\"") == 1);
        // Shorts, Live and Playlists all go through InnerTube with the channel's params.
        for (const char *tab : {"shorts", "live", "playlists"}) {
            done = false;
            service.browseTab(QString::fromLatin1(kNasa), QString::fromLatin1(tab), &net, [&](ChannelPage p) { page = p; done = true; });
            CHECK(catalog_test::waitFor([&] { return done; }));
            CHECK(page.outcome == Outcome::Ok);
            CHECK(page.items.size() >= 20);
        }
        CHECK_EQ(net.count(QStringLiteral("browse"), kStreamsParams), 1);
        for (const QString &h : net.contactedHosts()) CHECK_EQ(h, QStringLiteral("www.youtube.com"));
    }
    {
        // derived: the channel's own Videos params differ from the recorded ones, and the
        // recorded ones are refused with HTTP 400. The service learns the tab list from the home
        // tab and asks again with the channel's own params.
        QJsonObject home = fx("channel_home");
        const QJsonArray tabs = json::at(home, QStringLiteral("contents.twoColumnBrowseResultsRenderer.tabs")).toArray();
        for (int i = 0; i < tabs.size(); ++i) {
            const QString url = json::at(tabs.at(i), QStringLiteral("tabRenderer.endpoint.commandMetadata.webCommandMetadata.url")).toString();
            if (url.endsWith(QLatin1String("/videos")))
                home = withValue(home, QStringLiteral("contents.twoColumnBrowseResultsRenderer.tabs.%1.tabRenderer.endpoint.browseEndpoint.params").arg(i),
                                 QStringLiteral("T3duVmlkZW9z"));
        }
        FakeTransport net;
        net.on(QStringLiteral("browse"), kVideosParams, 400, QByteArray());
        net.on(QStringLiteral("browse"), "T3duVmlkZW9z", 200, catalog_test::fixture(QStringLiteral("channel_videos.json")));
        net.on(QStringLiteral("browse"), QByteArray(), 200, bytes(home));
        InnerTubeClient client(&net, &identity, &lane, loc);
        ChannelService service(&client);
        ChannelPage page; bool done = false;
        service.browseTab(QString::fromLatin1(kNasa), QStringLiteral("videos"), &net, [&](ChannelPage p) { page = p; done = true; });
        CHECK(catalog_test::waitFor([&] { return done; }));
        CHECK(page.outcome == Outcome::Ok);
        CHECK(page.items.size() >= 25);
        CHECK_EQ(net.requests.size(), 3);
        if (net.requests.size() == 3) {
            CHECK(!net.requests.at(1).body.contains("\"params\""));      // the home tab, to learn the list
            CHECK(net.requests.at(2).body.contains("T3duVmlkZW9z"));        // the channel's own params
        }
    }
}

// ---- Shorts rule (DSC-17) ----------------------------------------------------------------------

static void shortsRule() {
    const QString en = QStringLiteral("en");
    const QString nasa = QString::fromLatin1(kNasa);
    std::int64_t clock = 1000000;
    ShortsKnowledge k([&clock] { return clock; });
    // A 2:50 Short: listed on the Shorts tab (renderer evidence); its length (170 s) is known
    // from the duration store, as a watch page or RSS enrichment would record it.
    const ChannelPage shortsTab = ChannelService::parseBrowse(okReply(fx("channel_shorts")), nasa, QStringLiteral("shorts"), en);
    CHECK(!shortsTab.items.empty());
    Item shortRow = shortsTab.items.front();
    k.recordDuration(shortRow.id, 170);
    // derived: a 2:50 regular video (the first Videos-tab row with its badge set to 2:50).
    const QJsonObject videos = withLockup(fx("channel_videos"), 0, [](QJsonObject l) { return withBadgeText(l, QStringLiteral("2:50")); });
    const ChannelPage videosTab = ChannelService::parseBrowse(okReply(videos), nasa, QStringLiteral("videos"), en);
    const Item regular = videosTab.items.front();
    CHECK_EQ(regular.durationSeconds, static_cast<std::int64_t>(170));
    CHECK(regular.kind == ItemKind::Video);
    shortRow.durationSeconds = -1;      // Shorts lockups carry no length
    CHECK(k.classify(shortRow) == ShortEvidence::Renderer);
    CHECK(k.classify(regular) == ShortEvidence::Candidate);        // 170 s without evidence: candidate only
    // The hide filter acts only on confirmed Shorts.
    RowFilterSettings hide; hide.hideShorts = true;
    std::vector<Item> mixed = {regular, shortRow};
    RowFilterResult r = filterRows(mixed, hide, nullptr, k.lookup());
    CHECK_EQ(r.hiddenShorts, 1);
    CHECK_EQ(static_cast<int>(r.visible.size()), 1);
    CHECK_EQ(r.visible.front().id, regular.id);
    CHECK(r.visible.front().shortEvidence == ShortEvidence::Candidate);
    // Planning: the channel of the candidate needs a Shorts-tab check, once per 30 min.
    k.observe(videosTab.items);
    const std::vector<std::string> plan = k.channelsNeedingCheck(videosTab.items);
    CHECK_EQ(static_cast<int>(plan.size()), 1);
    CHECK_EQ(plan.front(), std::string(kNasa));
    const std::vector<std::string> candidates = k.candidatesOf(kNasa, videosTab.items);
    CHECK(std::find(candidates.begin(), candidates.end(), regular.id) != candidates.end());
    CHECK(candidates.size() >= 10);     // NASA uploads many clips of 3 minutes or less
    // Settled by the Shorts-tab check: listed ids are Shorts, the rest are not.
    const int settled = k.applyShortsTab(kNasa, ids(shortsTab.items), candidates);
    CHECK_EQ(settled, static_cast<int>(candidates.size()));
    CHECK(k.classify(regular) == ShortEvidence::NotShort);
    CHECK(k.channelsNeedingCheck(videosTab.items).empty());
    r = filterRows(mixed, hide, nullptr, k.lookup());
    CHECK_EQ(r.hiddenShorts, 1);
    CHECK(r.visible.front().shortEvidence == ShortEvidence::NotShort);
    // A Shorts-tab listing settles a candidate as a Short; later evidence overrides "not a Short".
    k.markShort(regular.id, ShortEvidence::ShortsTab);
    CHECK(k.classify(regular) == ShortEvidence::ShortsTab);
    k.markNotShort(regular.id);                                   // never overrides a known Short
    CHECK(k.classify(regular) == ShortEvidence::ShortsTab);
    // RSS /shorts/ links confirm at once.
    const std::vector<Item> rss = readRss(catalog_test::fixture(QStringLiteral("rss_channel.xml")));
    CHECK(countWhere(rss, [](const Item &i) { return i.shortEvidence == ShortEvidence::RssLink; }) >= 1);
    ShortsKnowledge fresh([&clock] { return clock; });
    fresh.observe(rss);
    for (const Item &i : rss)
        if (i.shortEvidence == ShortEvidence::RssLink) CHECK(fresh.evidence(i.id) == ShortEvidence::RssLink);
    // Recheck spacing (30 min) with the injected clock.
    ShortsKnowledge spaced([&clock] { return clock; });
    std::vector<Item> one = {regular};
    spaced.noteChecked(kNasa);
    CHECK(spaced.channelsNeedingCheck(one).empty());
    clock += ShortsKnowledge::kRecheckIntervalMs - 1;
    CHECK(spaced.channelsNeedingCheck(one).empty());
    clock += 2;
    CHECK_EQ(static_cast<int>(spaced.channelsNeedingCheck(one).size()), 1);
    // Durations fill RSS rows (no lengths in the feed); live rows record nothing.
    std::vector<Item> rssRows = rss;
    fresh.recordDuration(rssRows.front().id, 4321);
    fresh.fillDurations(rssRows);
    CHECK_EQ(rssRows.front().durationSeconds, static_cast<std::int64_t>(4321));
}

// ---- Shorts store: persistence and the background check ------------------------------------

static void shortsStore() {
    ClientIdentity identity(nullptr, QString());
    WorkLane lane(QStringLiteral("channel"), 1);
    WorkLane backgroundLane(QStringLiteral("background"), 1);
    DiscoveryLocale loc;
    QTemporaryDir dir;
    const QString file = dir.path() + QStringLiteral("/sub/shorts.txt");
    const QJsonObject videos = withLockup(fx("channel_videos"), 0, [](QJsonObject l) { return withBadgeText(l, QStringLiteral("2:50")); });
    const QString regularId = lockupId(videos, 0);
    // derived: one Shorts-tab entry replaced by the second Videos-tab row, so the check confirms
    // that candidate as a Short while every other candidate is settled as not a Short.
    const QString promotedId = lockupId(videos, 1);
    QJsonObject shorts = fx("channel_shorts");
    const QString shortsGrid = gridPath(shorts) + QStringLiteral(".0.richItemRenderer.content.shortsLockupViewModel.onTap.innertubeCommand.reelWatchEndpoint.videoId");
    shorts = withValue(shorts, shortsGrid, promotedId);
    {
        FakeTransport net, background;
        serveNasa(net, videos, 0);
        background.on(QStringLiteral("browse"), kShortsParams, 200, bytes(shorts), 300);
        InnerTubeClient client(&net, &identity, &lane, loc);
        InnerTubeClient backgroundClient(&background, &identity, &backgroundLane, loc);
        ShortsStore store(&backgroundClient, file);
        ChannelService service(&client, &store);
        ChannelModel model(&service);
        model.setHideShorts(true);
        int finished = 0; int settled = 0;
        QObject::connect(&store, &ShortsStore::checkFinished, [&](const QString &, int n) { ++finished; settled = n; });
        model.open(QString::fromLatin1(kNasa));
        CHECK(catalog_test::waitFor([&] { return !model.busy() && model.count() > 0; }));
        // Before the check settles them: candidates stay visible, nothing is hidden as a Short.
        CHECK_EQ(finished, 0);
        CHECK_EQ(store.pendingChecks(), 1);
        const int all = model.count();
        CHECK(all >= 25);
        CHECK_EQ(model.hiddenShorts(), 0);
        CHECK(model.data(model.index(1, 0), ChannelModel::ShortCandidateRole).toBool());
        CHECK_EQ(model.data(model.index(1, 0), ChannelModel::ItemIdRole).toString(), promotedId);
        CHECK(catalog_test::waitFor([&] { return finished == 1; }));
        CHECK_EQ(model.count(), all - 1);
        CHECK(settled >= 10);
        CHECK_EQ(background.count(QStringLiteral("browse"), kShortsParams), 1);
        CHECK_EQ(net.count(QStringLiteral("browse"), kShortsParams), 0);     // never on the interactive transport
        CHECK(store.knowledge().evidence(regularId.toStdString()) == ShortEvidence::NotShort);
        CHECK(isConfirmedShort(store.knowledge().evidence(promotedId.toStdString())));
        // The model re-filtered in place: only the confirmed Short is hidden and counted.
        CHECK_EQ(model.hiddenShorts(), 1);
        CHECK_EQ(model.hiddenCount(), 1);
        bool promotedVisible = false;
        for (const Item &i : model.visibleRows()) if (i.id == promotedId.toStdString()) promotedVisible = true;
        CHECK(!promotedVisible);
        CHECK_EQ(model.data(model.index(0, 0), ChannelModel::ItemIdRole).toString(), regularId);
        CHECK(!model.data(model.index(0, 0), ChannelModel::ShortCandidateRole).toBool());   // settled: not a Short
        CHECK(!model.data(model.index(0, 0), ChannelModel::ShortRole).toBool());
        model.setHideShorts(false);
        CHECK_EQ(model.hiddenShorts(), 0);
        // The same rows do not plan another check within 30 min.
        CHECK_EQ(store.requestChecks(model.visibleRows()), 0);
        CHECK(store.save());
        CHECK(QFileInfo(file).exists());
        CHECK((QFileInfo(file).permissions() & (QFileDevice::ReadGroup | QFileDevice::ReadOther | QFileDevice::WriteGroup | QFileDevice::WriteOther)) == 0);
    }
    {
        ShortsStore reloaded(nullptr, file);
        CHECK(reloaded.load());
        CHECK(isConfirmedShort(reloaded.knowledge().evidence(promotedId.toStdString())));
        CHECK(reloaded.knowledge().evidence(regularId.toStdString()) == ShortEvidence::NotShort);
        CHECK_EQ(reloaded.knowledge().duration(regularId.toStdString()), static_cast<std::int64_t>(170));
        CHECK(reloaded.knowledge().shortsCount() >= 40);
    }
    {
        // derived: a channel without a Shorts tab settles its candidates as not Shorts.
        FakeTransport background;
        const QJsonObject noShorts = withoutTab(fx("channel_home"), "/shorts");
        background.on(QStringLiteral("browse"), QByteArray(), 200, bytes(noShorts));
        InnerTubeClient backgroundClient(&background, &identity, &backgroundLane, loc);
        ShortsStore store(&backgroundClient, QString());
        const ChannelPage page = ChannelService::parseBrowse(okReply(videos), QString::fromLatin1(kNasa), QStringLiteral("videos"), QStringLiteral("en"));
        int finished = 0;
        QObject::connect(&store, &ShortsStore::checkFinished, [&](const QString &, int) { ++finished; });
        CHECK_EQ(store.requestChecks(page.items), 1);
        CHECK(catalog_test::waitFor([&] { return finished == 1; }));
        CHECK(store.knowledge().evidence(regularId.toStdString()) == ShortEvidence::NotShort);
        // A failed check settles nothing.
        FakeTransport down;
        InnerTubeClient downClient(&down, &identity, &backgroundLane, loc);
        ShortsStore failing(&downClient, QString());
        finished = 0;
        QObject::connect(&failing, &ShortsStore::checkFinished, [&](const QString &, int) { ++finished; });
        CHECK_EQ(failing.requestChecks(page.items), 1);
        CHECK(catalog_test::waitFor([&] { return finished == 1; }));
        CHECK(failing.knowledge().classify(page.items.front()) == ShortEvidence::Candidate);
    }
}

// ---- channel model: RSS first, merge, tabs, fallbacks -----------------------------------------

static void channelModel() {
    ClientIdentity identity(nullptr, QString());
    WorkLane lane(QStringLiteral("channel"), 1);
    DiscoveryLocale loc;
    {
        FakeTransport net;
        serveNasa(net, fx("channel_videos"), 40);
        InnerTubeClient client(&net, &identity, &lane, loc);
        ChannelService service(&client);
        int rssCalls = 0;
        service.setRssFetch(fakeRss(&rssCalls));
        ChannelModel model(&service);
        int resets = 0, removals = 0;
        model.open(QStringLiteral("@NASA"));
        CHECK_EQ(model.tab(), QStringLiteral("videos"));
        CHECK(model.busy());
        CHECK(catalog_test::waitFor([&] { return model.rssPainted(); }));
        CHECK_EQ(rssCalls, 1);
        // First paint: the RSS uploads (RSS Shorts belong on the Shorts tab), in feed order.
        const std::vector<Item> rss = readRss(catalog_test::fixture(QStringLiteral("rss_channel.xml")));
        std::vector<std::string> rssUploads;
        for (const Item &i : rss) if (i.shortEvidence != ShortEvidence::RssLink) rssUploads.push_back(i.id);
        CHECK(ids(model.visibleRows()) == rssUploads);
        CHECK(model.busy());                                        // InnerTube page 1 still pending
        QObject::connect(&model, &QAbstractItemModel::modelReset, [&] { ++resets; });
        QObject::connect(&model, &QAbstractItemModel::rowsRemoved, [&] { ++removals; });
        CHECK(catalog_test::waitFor([&] { return !model.busy(); }));
        // InnerTube page 1 merged by id: the painted rows keep their place, new ones follow.
        const std::vector<std::string> after = ids(model.visibleRows());
        CHECK(after.size() > rssUploads.size());
        CHECK(std::equal(rssUploads.begin(), rssUploads.end(), after.begin()));
        CHECK(unique(model.visibleRows()));
        CHECK_EQ(resets, 0);
        CHECK_EQ(removals, 0);
        const ChannelPage p1 = ChannelService::parseBrowse(okReply(fx("channel_videos")), QString::fromLatin1(kNasa), QStringLiteral("videos"), QStringLiteral("en"));
        std::set<std::string> union_(rssUploads.begin(), rssUploads.end());
        for (const Item &i : p1.items) union_.insert(i.id);
        CHECK_EQ(static_cast<int>(after.size()), static_cast<int>(union_.size()));
        // RSS rows that InnerTube also listed now carry its length; RSS keeps its exact date.
        for (const Item &i : model.visibleRows()) {
            if (i.id == "hQsM8aYR5mw") {
                CHECK_EQ(i.durationSeconds, static_cast<std::int64_t>(95));
                CHECK(i.publishedEpoch > 0);
            }
        }
        // Header.
        CHECK_EQ(model.title(), QStringLiteral("NASA"));
        CHECK_EQ(model.channelId(), QString::fromLatin1(kNasa));
        CHECK_EQ(model.handle(), QStringLiteral("@NASA"));
        CHECK(model.subscriberCount() > 1000000 && model.videoCount() > 1000);
        CHECK(model.avatarUrl().contains(QLatin1String("=s176")));
        CHECK_EQ(model.tabs().join(QLatin1Char(',')), QStringLiteral("videos,shorts,streams,playlists"));
        CHECK(model.canLoadMore());
        CHECK_EQ(model.outcome(), QStringLiteral("ok"));
        CHECK(model.errorText().isEmpty());
        // Page 2 appends.
        const int before = model.count();
        model.loadMore();
        CHECK(catalog_test::waitFor([&] { return !model.busy(); }));
        CHECK(model.count() >= before + 25);
        CHECK(unique(model.visibleRows()));
        CHECK_EQ(resets, 0);
        // Tabs.
        model.setTab(QStringLiteral("shorts"));
        CHECK(catalog_test::waitFor([&] { return !model.busy() && model.count() > 0; }));
        CHECK(model.count() >= 20);
        CHECK(model.data(model.index(0, 0), ChannelModel::ShortRole).toBool());
        CHECK_EQ(model.data(model.index(0, 0), ChannelModel::KindRole).toString(), QStringLiteral("short"));
        model.setHideShorts(true);                                   // never hides the Shorts tab itself
        CHECK(model.count() >= 20);
        CHECK_EQ(model.hiddenShorts(), 0);
        model.setTab(QStringLiteral("live"));
        CHECK_EQ(model.tab(), QStringLiteral("streams"));
        CHECK(catalog_test::waitFor([&] { return !model.busy() && model.count() > 0; }));
        CHECK(model.data(model.index(0, 0), ChannelModel::LiveRole).toBool());
        CHECK(model.data(model.index(0, 0), ChannelModel::DurationRole).toString().isEmpty());
        model.setTab(QStringLiteral("playlists"));
        CHECK(catalog_test::waitFor([&] { return !model.busy() && model.count() > 0; }));
        CHECK_EQ(model.data(model.index(0, 0), ChannelModel::KindRole).toString(), QStringLiteral("playlist"));
        CHECK(!model.data(model.index(0, 0), ChannelModel::PlaylistIdRole).toString().isEmpty());
        CHECK(model.get(0).value(QStringLiteral("videoCount")).toDouble() > 0);
        // Back to Videos: kept, no new request.
        const int browseCalls = net.count(QStringLiteral("browse"));
        model.setTab(QStringLiteral("videos"));
        CHECK(model.count() >= before + 25);
        CHECK_EQ(net.count(QStringLiteral("browse")), browseCalls);
        // Each tab was one InnerTube call (plus page 2 of Videos).
        CHECK_EQ(net.count(QStringLiteral("browse"), kShortsParams), 1);
        CHECK_EQ(net.count(QStringLiteral("browse"), kPlaylistsParams), 1);
    }
    {
        // A tab link opens that tab.
        FakeTransport net;
        serveNasa(net, fx("channel_videos"), 0);
        InnerTubeClient client(&net, &identity, &lane, loc);
        ChannelService service(&client);
        ChannelModel model(&service);
        model.open(QStringLiteral("https://www.youtube.com/@NASA/shorts"));
        CHECK_EQ(model.tab(), QStringLiteral("shorts"));
        CHECK(catalog_test::waitFor([&] { return !model.busy() && model.count() > 0; }));
        CHECK_EQ(net.count(QStringLiteral("browse"), kVideosParams), 0);
    }
    {
        // derived: a channel with no Videos tab (and no RSS rows) opens its Playlists tab.
        FakeTransport net;
        net.on(QStringLiteral("browse"), kVideosParams, 200, bytes(homeWithoutVideos()));
        net.on(QStringLiteral("browse"), kPlaylistsParams, 200, bytes(withoutTab(fx("channel_playlists"), "/videos")));
        InnerTubeClient client(&net, &identity, &lane, loc);
        ChannelService service(&client);
        ChannelModel model(&service);
        model.open(QString::fromLatin1(kNasa));
        CHECK(catalog_test::waitFor([&] { return !model.busy() && model.count() > 0; }));
        CHECK(model.fallbackTab());
        CHECK_EQ(model.tab(), QStringLiteral("playlists"));
        CHECK(model.count() >= 20);
        CHECK(!model.tabs().contains(QStringLiteral("videos")));
        CHECK_EQ(model.title(), QStringLiteral("NASA"));
    }
    {
        // derived: no uploads (valid empty) versus a changed layout (drift) versus no network.
        const QJsonObject videos = fx("channel_videos");
        struct Case { QJsonObject body; int status; const char *outcome; bool empty; };
        const QList<Case> cases = {
            {withValue(videos, gridPath(videos), QJsonArray()), 200, "empty", true},
            {without(videos, QStringLiteral("contents.twoColumnBrowseResultsRenderer")), 200, "drift", false},
            {QJsonObject(), 503, "http5xx", false}};
        for (const Case &c : cases) {
            FakeTransport net;
            net.on(QStringLiteral("browse"), kVideosParams, c.status, c.status == 200 ? bytes(c.body) : QByteArray());
            InnerTubeClient client(&net, &identity, &lane, loc);
            ChannelService service(&client);
            ChannelModel model(&service);
            model.open(QString::fromLatin1(kNasa));
            CHECK(catalog_test::waitFor([&] { return !model.busy(); }));
            CHECK_EQ(model.outcome(), QString::fromLatin1(c.outcome));
            CHECK_EQ(model.empty(), c.empty);
            CHECK_EQ(model.errorText().isEmpty(), c.empty);
            CHECK_EQ(model.count(), 0);
            CHECK(!model.canLoadMore());
        }
        // An unknown handle is unavailable.
        FakeTransport net;
        net.on(QStringLiteral("navigation/resolve_url"), QByteArray(), 404, QByteArray());
        InnerTubeClient client(&net, &identity, &lane, loc);
        ChannelService service(&client);
        ChannelModel model(&service);
        model.open(QStringLiteral("@doesnotexist000"));
        CHECK(catalog_test::waitFor([&] { return !model.busy(); }));
        CHECK_EQ(model.outcome(), QStringLiteral("unavailable"));
        CHECK(!model.errorText().isEmpty());
    }
}

// ---- members-only and restricted rows (DSC-18) --------------------------------------------------

static void membersOnly() {
    ClientIdentity identity(nullptr, QString());
    WorkLane lane(QStringLiteral("channel"), 1);
    DiscoveryLocale loc;
    // derived: a members badge added to row 2's lockup metadata, an age-restricted badge to row 3.
    QJsonObject videos = fx("channel_videos");
    videos = withLockup(videos, 2, [](QJsonObject l) {
        return withValue(l, QStringLiteral("metadata.lockupMetadataViewModel.badges"), QJsonArray{QJsonObject{{QStringLiteral("badgeViewModel"), QJsonObject{
            {QStringLiteral("badgeText"), QStringLiteral("Members only")}, {QStringLiteral("badgeStyle"), QStringLiteral("BADGE_MEMBERS_ONLY")}}}}});
    });
    videos = withLockup(videos, 3, [](QJsonObject l) {
        return withValue(l, QStringLiteral("metadata.lockupMetadataViewModel.badges"), QJsonArray{QJsonObject{{QStringLiteral("metadataBadgeRenderer"), QJsonObject{
            {QStringLiteral("style"), QStringLiteral("BADGE_STYLE_TYPE_AGE_RESTRICTED")}, {QStringLiteral("label"), QStringLiteral("Age-restricted")}}}}});
    });
    const QString membersId = lockupId(videos, 2);
    const QString restrictedId = lockupId(videos, 3);
    const ChannelPage parsed = ChannelService::parseBrowse(okReply(videos), QString::fromLatin1(kNasa), QStringLiteral("videos"), QStringLiteral("en"));
    CHECK(parsed.items.at(2).membersOnly && !parsed.items.at(2).restricted);
    CHECK(parsed.items.at(3).restricted && !parsed.items.at(3).membersOnly);
    const int all = static_cast<int>(parsed.items.size());

    FakeTransport net;
    serveNasa(net, videos, 0);
    InnerTubeClient client(&net, &identity, &lane, loc);
    ChannelService service(&client);
    ChannelModel model(&service);
    model.open(QString::fromLatin1(kNasa));
    CHECK(catalog_test::waitFor([&] { return !model.busy() && model.count() > 0; }));
    // Hide setting off: badged, nothing hidden.
    CHECK_EQ(model.count(), all);
    CHECK_EQ(model.hiddenCount(), 0);
    CHECK_EQ(model.data(model.index(2, 0), ChannelModel::ItemIdRole).toString(), membersId);
    CHECK(model.data(model.index(2, 0), ChannelModel::MembersOnlyRole).toBool());
    CHECK(model.data(model.index(3, 0), ChannelModel::RestrictedRole).toBool());
    CHECK(!model.data(model.index(4, 0), ChannelModel::MembersOnlyRole).toBool());
    // Hide setting on: hidden, and counted.
    model.setHideMembersOnly(true);
    CHECK_EQ(model.count(), all - 1);
    CHECK_EQ(model.hiddenMembersOnly(), 1);
    CHECK_EQ(model.hiddenCount(), 1);
    model.setHideRestricted(true);
    CHECK_EQ(model.count(), all - 2);
    CHECK_EQ(model.hiddenRestricted(), 1);
    CHECK_EQ(model.hiddenCount(), 2);
    CHECK_EQ(model.count() + model.hiddenCount(), all);           // no row disappears uncounted
    for (const Item &i : model.visibleRows()) CHECK(i.id != membersId.toStdString() && i.id != restrictedId.toStdString());
    model.setHideMembersOnly(false);
    model.setHideRestricted(false);
    CHECK_EQ(model.count(), all);
    CHECK_EQ(model.hiddenCount(), 0);
}

// ---- playlist preview (LIB-03 / LNK-04) ----------------------------------------------------------

namespace {
// derived: a synthetic continuation page of playlistVideoRenderer rows (structured length).
QJsonObject chainPage(int page, int perPage, int firstIndex, const QString &next) {
    QJsonArray items;
    for (int i = 0; i < perPage; ++i) {
        const int n = firstIndex + i;
        char id[12];
        std::snprintf(id, sizeof id, "c%010d", n);
        QJsonObject r;
        r.insert(QStringLiteral("videoId"), QString::fromLatin1(id));
        r.insert(QStringLiteral("title"), QJsonObject{{QStringLiteral("runs"), QJsonArray{QJsonObject{{QStringLiteral("text"), QStringLiteral("Item %1").arg(n)}}}}});
        r.insert(QStringLiteral("index"), QJsonObject{{QStringLiteral("simpleText"), QString::number(n + 1)}});
        r.insert(QStringLiteral("lengthSeconds"), QStringLiteral("170"));
        r.insert(QStringLiteral("lengthText"), QJsonObject{{QStringLiteral("simpleText"), QStringLiteral("2:50")}});
        r.insert(QStringLiteral("shortBylineText"), QJsonObject{{QStringLiteral("runs"), QJsonArray{QJsonObject{{QStringLiteral("text"), QStringLiteral("NASA")}}}}});
        items.append(QJsonObject{{QStringLiteral("playlistVideoRenderer"), r}});
    }
    if (!next.isEmpty())
        items.append(QJsonObject{{QStringLiteral("continuationItemRenderer"), QJsonObject{{QStringLiteral("continuationEndpoint"),
            QJsonObject{{QStringLiteral("continuationCommand"), QJsonObject{{QStringLiteral("token"), next}}}}}}}});
    Q_UNUSED(page);
    QJsonObject action{{QStringLiteral("appendContinuationItemsAction"), QJsonObject{{QStringLiteral("continuationItems"), items}}}};
    return QJsonObject{{QStringLiteral("onResponseReceivedActions"), QJsonArray{action}}};
}
}

static void playlists() {
    CHECK_EQ(PlaylistService::normalizeListId(QStringLiteral("VLPLF98rxtslw6s")), QStringLiteral("PLF98rxtslw6s"));
    CHECK_EQ(PlaylistService::normalizeListId(QStringLiteral("https://www.youtube.com/watch?v=9wq3VHsL_bE&list=PLF98rxtslw6s&index=2")), QStringLiteral("PLF98rxtslw6s"));
    CHECK_EQ(PlaylistService::normalizeListId(QStringLiteral("https://www.youtube.com/playlist?list=PLF98rxtslw6s")), QStringLiteral("PLF98rxtslw6s"));
    CHECK(PlaylistService::normalizeListId(QStringLiteral("bad id")).isEmpty());

    const QJsonObject browse = fx("playlist_browse");
    const PlaylistPage p = PlaylistService::parseBrowse(okReply(browse), QStringLiteral("PLF98rxtslw6s"), QStringLiteral("en"));
    CHECK(p.outcome == Outcome::Ok);
    CHECK(!p.title.isEmpty());
    CHECK_EQ(p.owner, QStringLiteral("NASA"));
    CHECK_EQ(p.ownerId, QString::fromLatin1(kNasa));
    CHECK_EQ(p.videoCount, static_cast<qint64>(3));
    CHECK_EQ(static_cast<int>(p.items.size()), 3);
    CHECK(!p.continuation.isEmpty());
    CHECK(p.unknown.isEmpty());                                   // continuationItemViewModel is understood
    for (int i = 0; i < static_cast<int>(p.items.size()); ++i) {
        const Item &it = p.items[static_cast<std::size_t>(i)];
        CHECK_EQ(it.playlistIndex, i);
        CHECK_EQ(it.playlistId, std::string("PLF98rxtslw6s"));
        CHECK(it.id.size() == 11 && !it.title.empty() && it.durationSeconds > 0);
    }
    // DSC-01 for playlists.
    const QString sectionContents = QStringLiteral("contents.twoColumnBrowseResultsRenderer.tabs.0.tabRenderer.content.sectionListRenderer.contents");
    const PlaylistPage empty = PlaylistService::parseBrowse(okReply(withValue(browse, sectionContents, QJsonArray())), QStringLiteral("PLF98rxtslw6s"), QStringLiteral("en"));
    CHECK(empty.outcome == Outcome::ValidEmpty);
    CHECK(PlaylistService::parseBrowse(okReply(without(browse, QStringLiteral("contents"))), QStringLiteral("PLF98rxtslw6s"), QStringLiteral("en")).outcome == Outcome::ShapeDrift);
    QJsonObject gone;
    gone.insert(QStringLiteral("alerts"), QJsonArray{QJsonObject{{QStringLiteral("alertRenderer"), QJsonObject{{QStringLiteral("type"), QStringLiteral("ERROR")}}}}});
    CHECK(PlaylistService::parseBrowse(okReply(gone), QStringLiteral("PLx"), QStringLiteral("en")).outcome == Outcome::Unavailable);

    // The preview model: page 1 plus a synthetic continuation chain of 50 pages x 100 rows
    // (LIB-03: at least 5000 items, in order, no cap).
    ClientIdentity identity(nullptr, QString());
    WorkLane lane(QStringLiteral("playlist"), 1);
    DiscoveryLocale loc;
    FakeTransport net;
    net.on(QStringLiteral("browse"), "\"browseId\":\"VLPLF98rxtslw6s\"", 200, bytes(browse));
    const int pages = 50, perPage = 100;
    net.on(QStringLiteral("browse"), p.continuation.toLatin1(), 200,
           bytes(chainPage(0, perPage, 3, QStringLiteral("chain-01-end"))));
    for (int i = 1; i < pages; ++i) {
        const QString token = QStringLiteral("chain-%1-end").arg(i, 2, 10, QLatin1Char('0'));
        const QString next = i + 1 < pages ? QStringLiteral("chain-%1-end").arg(i + 1, 2, 10, QLatin1Char('0')) : QString();
        net.on(QStringLiteral("browse"), (QStringLiteral("\"") + token + QStringLiteral("\"")).toLatin1(), 200,
               bytes(chainPage(i, perPage, 3 + i * perPage, next)));
    }
    InnerTubeClient client(&net, &identity, &lane, loc);
    CatalogCounters counters;
    PlaylistService service(&client, &counters);
    PlaylistModel model(&service);
    model.open(QStringLiteral("https://www.youtube.com/playlist?list=PLF98rxtslw6s"));
    CHECK_EQ(model.playlistId(), QStringLiteral("PLF98rxtslw6s"));
    CHECK(catalog_test::waitFor([&] { return !model.busy(); }));
    CHECK_EQ(model.count(), 3);
    CHECK_EQ(model.owner(), QStringLiteral("NASA"));
    CHECK_EQ(model.videoCount(), 3.0);
    CHECK(!model.title().isEmpty());
    const QVariantMap hint = model.get(1);
    CHECK_EQ(hint.value(QStringLiteral("playlistId")).toString(), QStringLiteral("PLF98rxtslw6s"));
    CHECK_EQ(hint.value(QStringLiteral("index")).toInt(), 1);
    CHECK(!hint.value(QStringLiteral("videoId")).toString().isEmpty());
    int guard = 0;
    while (model.canLoadMore() && guard++ < 200) {
        model.loadMore();
        CHECK(catalog_test::waitFor([&] { return !model.busy(); }));
    }
    CHECK_EQ(model.count(), 3 + pages * perPage);
    bool ordered = true, structured = true;
    for (int i = 0; i < model.count(); ++i) {
        const Item &it = model.rows()[static_cast<std::size_t>(i)];
        if (it.playlistIndex != i) ordered = false;
        if (i >= 3 && it.durationSeconds != 170) structured = false;
    }
    CHECK(ordered);
    CHECK(structured);
    CHECK(unique(model.rows()));
    CHECK_EQ(model.data(model.index(4999, 0), PlaylistModel::IndexRole).toInt(), 4999);
    CHECK_EQ(counters.value(Area::Playlist, Outcome::Ok), static_cast<qint64>(1 + pages));
    // Nothing is saved by the preview: it only ever sent browse calls.
    for (const auto &r : net.requests) CHECK(r.url.toString().contains(QLatin1String("/youtubei/v1/browse")));
}

// LNK-04: a list= link seeds playback's queue from the playlist, at the linked video.
static void queueSeeder() {
    ClientIdentity identity(nullptr, QString());
    WorkLane lane(QStringLiteral("queue"), 1);
    DiscoveryLocale loc;
    const QJsonObject browse = fx("playlist_browse");
    const PlaylistPage page1 = PlaylistService::parseBrowse(okReply(browse), QStringLiteral("PLF98rxtslw6s"), QStringLiteral("en"));
    CHECK(page1.items.size() >= 3 && !page1.continuation.isEmpty());
    const QString second = QString::fromStdString(page1.items.at(1).id);
    // derived: one synthetic continuation page of 100 rows, then the end.
    FakeTransport net;
    net.on(QStringLiteral("browse"), "\"browseId\":\"VLPLF98rxtslw6s\"", 200, bytes(browse));
    net.on(QStringLiteral("browse"), "\"" + page1.continuation.toLatin1() + "\"", 200, bytes(chainPage(1, 100, 0, QString())));
    InnerTubeClient client(&net, &identity, &lane, loc);
    PlaylistService service(&client);
    sailfish::QueueSeeder seeder(&service);
    struct Ready { QString videoId; QStringList ids; int current; };
    QList<Ready> ready;
    QObject::connect(&seeder, &sailfish::QueueSeeder::queueReady, [&ready](const QString &v, const QStringList &ids, int c) {
        Ready r; r.videoId = v; r.ids = ids; r.current = c; ready.append(r);
    });

    // The linked video is the list's second: the queue is the whole walk, current = 1.
    seeder.seed(QStringLiteral("https://www.youtube.com/watch?v=x&list=PLF98rxtslw6s&index=2"), second, 1);
    CHECK(catalog_test::waitFor([&] { return ready.size() == 1; }));
    if (ready.size() != 1) return;
    CHECK_EQ(ready.at(0).videoId, second);
    CHECK_EQ(ready.at(0).current, 1);
    CHECK_EQ(ready.at(0).ids.value(1), second);
    CHECK_EQ(ready.at(0).ids.size(), static_cast<int>(page1.items.size()) + 100);   // page 1 + the continuation, in order
    CHECK_EQ(ready.at(0).ids.value(static_cast<int>(page1.items.size())), QStringLiteral("c0000000000"));
    CHECK(!seeder.busy());

    // A video that is not in the list goes in at its index hint.
    seeder.seed(QStringLiteral("PLF98rxtslw6s"), QStringLiteral("zzzzzzzzzzz"), 2);
    CHECK(catalog_test::waitFor([&] { return ready.size() == 2; }));
    if (ready.size() != 2) return;
    CHECK_EQ(ready.at(1).current, 2);
    CHECK_EQ(ready.at(1).ids.value(2), QStringLiteral("zzzzzzzzzzz"));

    // A newer link wins; the first walk is dropped.
    seeder.seed(QStringLiteral("PLF98rxtslw6s"), second, 1);
    seeder.seed(QStringLiteral("PLF98rxtslw6s"), QString::fromStdString(page1.items.at(0).id), 0);
    CHECK(catalog_test::waitFor([&] { return ready.size() == 3; }));
    catalog_test::waitFor([] { return false; }, 200);
    CHECK_EQ(ready.size(), 3);
    CHECK_EQ(ready.at(2).current, 0);

    // A list that cannot be fetched: nothing (the video plays alone).
    FakeTransport down;
    InnerTubeClient downClient(&down, &identity, &lane, loc);
    PlaylistService downService(&downClient);
    sailfish::QueueSeeder alone(&downService);
    int emitted = 0;
    QObject::connect(&alone, &sailfish::QueueSeeder::queueReady, [&emitted](const QString &, const QStringList &, int) { ++emitted; });
    alone.seed(QStringLiteral("PLF98rxtslw6s"), second, 1);
    CHECK(catalog_test::waitFor([&] { return !alone.busy(); }));
    CHECK_EQ(emitted, 0);
}

// YouTube-style cards: a channel page's rows carry no avatar of their own, so channelAvatar is
// the channel's header avatar; playlist rows carry their owner's.
static void channelAvatars() {
    AvatarBook::instance().clear();
    ClientIdentity identity(nullptr, QString());
    WorkLane lane(QStringLiteral("avatars"), 1);
    DiscoveryLocale loc;
    FakeTransport net;
    serveNasa(net, fx("channel_videos"), 0);
    const QJsonObject browse = fx("playlist_browse");
    net.on(QStringLiteral("browse"), "\"browseId\":\"VLPLF98rxtslw6s\"", 200, bytes(browse));
    InnerTubeClient client(&net, &identity, &lane, loc);
    ChannelService service(&client);
    ChannelModel model(&service);
    model.open(QString::fromLatin1(kNasa));
    CHECK(catalog_test::waitFor([&] { return !model.busy() && model.count() > 0; }));
    CHECK(!model.avatarUrl().isEmpty());
    for (int i = 0; i < model.count(); ++i)
        CHECK_EQ(model.get(i).value(QStringLiteral("channelAvatar")).toString(), model.avatarUrl());
    // The channel's own page is authoritative for the book.
    CHECK_EQ(QString::fromStdString(AvatarBook::instance().lookup(kNasa)), model.avatarUrl());

    PlaylistService playlists(&client);
    PlaylistModel list(&playlists);
    list.open(QStringLiteral("PLF98rxtslw6s"));
    CHECK(catalog_test::waitFor([&] { return list.count() >= 3; }));
    CHECK(list.roleNames().values().contains("channelAvatar"));
    for (int i = 0; i < list.count(); ++i)
        CHECK(list.get(i).value(QStringLiteral("channelAvatar")).toString().startsWith(QLatin1String("https://")));
    AvatarBook::instance().clear();
}

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    lruStore();
    channelReferences();
    headers();
    tabFixtures();
    serviceFlow();
    shortsRule();
    shortsStore();
    channelModel();
    membersOnly();
    playlists();
    queueSeeder();
    channelAvatars();
    return catalog_test::finish("catalog_channel");
}
