// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// The catalog side of the library hooks (CatalogLibraryBridge) over a real LibraryStore on a
// temporary database and recorded fixtures: feed subscriptions and write-back (DSC-05, LIB-01),
// hide watched (LIB-07), remote playlist mirrors (LIB-03) and import handles (IMP-01).
#include "tests/catalog_common/TestSupport.h"

#include "Clients/Sailfish/Presentation/CatalogLibraryBridge.h"
#include "Clients/Sailfish/Presentation/ChannelModel.h"
#include "Clients/Sailfish/Presentation/LibraryModels.h"
#include "Clients/Sailfish/Presentation/WatchedRows.h"
#include "Core/Catalog/Domain/AvatarBook.h"
#include "Core/Catalog/Domain/RowFilter.h"
#include "Core/Catalog/Infrastructure/ChannelService.h"
#include "Core/Catalog/Infrastructure/ClientIdentity.h"
#include "Core/Catalog/Infrastructure/FeedService.h"
#include "Core/Catalog/Infrastructure/InnerTubeClient.h"
#include "Core/Catalog/Infrastructure/PlaylistService.h"
#include "Core/Catalog/Infrastructure/RssParser.h"
#include "Core/Catalog/Infrastructure/WorkLane.h"
#include "Core/Library/Infrastructure/LibraryStore.h"

#include <QDir>
#include <QJsonArray>
#include <QTemporaryDir>
#include <algorithm>
#include <cstdio>

using namespace nordtile::video;
using namespace nordtile::video::catalog;
using catalog_test::FakeTransport;
using sailfish::CatalogLibraryBridge;
using sailfish::WatchStateIndex;
using sailfish::WatchedRows;

namespace {

const char *const kNasa = "UCLA_DiR1FfKNvjuUpBHmylQ";
const char *const kList = "PLF98rxtslw6s";
qint64 kNowMs = 1762600000000LL;

int sequence = 0;
QString dbPath(const QTemporaryDir &dir) {
    return dir.path() + QStringLiteral("/library-%1.db").arg(++sequence);
}

library::ChannelRecord channel(const std::string &id, const std::string &name) {
    library::ChannelRecord c;
    c.channelId = id;
    c.name = name;
    return c;
}

std::string chan(int n) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "UC%022d", n);
    return buf;
}

struct Rig {
    FakeTransport net;
    ClientIdentity identity;
    WorkLane lane;
    InnerTubeClient client;
    Rig() : identity(nullptr, QString()), lane(QStringLiteral("library-test"), 1), client(&net, &identity, &lane, DiscoveryLocale()) {}
};

bool feedIdle(FeedService &s, int ms = 8000) {
    return catalog_test::waitFor([&s] { return s.cacheLoaded() && !s.refreshing() && !s.extending(); }, ms);
}

// derived: a synthetic continuation page of playlistVideoRenderer rows.
QByteArray chainPage(int firstIndex, int perPage, const QString &next) {
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
        r.insert(QStringLiteral("shortBylineText"), QJsonObject{{QStringLiteral("runs"), QJsonArray{QJsonObject{{QStringLiteral("text"), QStringLiteral("NASA")}}}}});
        items.append(QJsonObject{{QStringLiteral("playlistVideoRenderer"), r}});
    }
    if (!next.isEmpty())
        items.append(QJsonObject{{QStringLiteral("continuationItemRenderer"), QJsonObject{{QStringLiteral("continuationEndpoint"),
            QJsonObject{{QStringLiteral("continuationCommand"), QJsonObject{{QStringLiteral("token"), next}}}}}}}});
    QJsonObject action{{QStringLiteral("appendContinuationItemsAction"), QJsonObject{{QStringLiteral("continuationItems"), items}}}};
    return QJsonDocument(QJsonObject{{QStringLiteral("onResponseReceivedActions"), QJsonArray{action}}}).toJson(QJsonDocument::Compact);
}

}  // namespace

// ---- feed subscriptions and write-back (DSC-05, LIB-01) ----------------------------------------

static void feedSubscriptions() {
    QTemporaryDir dir;
    library::LibraryStore store(dbPath(dir), QStringLiteral("feed-subs"));
    CHECK(store.open());
    CHECK(store.subscribe(channel(kNasa, "NASA")));
    CHECK(store.subscribe(channel(chan(7), "Other")));

    Rig rig;
    const QByteArray rss = catalog_test::fixture(QStringLiteral("rss_channel.xml"));
    const RssFeed parsed = RssParser::parse(rss, 200);
    CHECK(parsed.outcome == Outcome::Ok && !parsed.items.empty());
    qint64 newest = -1;
    for (const Item &i : parsed.items) newest = std::max<qint64>(newest, i.publishedEpoch);
    CHECK(newest > 0);
    rig.net.on(QStringLiteral("channel_id=") + QLatin1String(kNasa), QByteArray(), 200, rss);
    FeedService feed(&rig.net, &rig.client, nullptr, nullptr, [newest] { return newest + 3600; });
    feed.start();
    CatalogLibraryBridge bridge(&feed, nullptr, nullptr, nullptr, [] { return kNowMs; });
    bridge.attach(&store, nullptr);
    CHECK(catalog_test::waitFor([&] { return feed.cacheLoaded(); }));
    CHECK_EQ(feed.channelCount(), 2);
    CHECK_EQ(bridge.channelPushes(), 1);

    // An unchanged list is not pushed again; a change is.
    emit store.subscriptionsChanged();
    CHECK_EQ(bridge.channelPushes(), 1);
    CHECK(store.subscribe(channel(chan(8), "Third")));
    CHECK_EQ(feed.channelCount(), 3);
    CHECK_EQ(bridge.channelPushes(), 2);
    CHECK(store.unsubscribe(QString::fromStdString(chan(8))));
    CHECK_EQ(feed.channelCount(), 2);

    // Write-back: NASA's newest upload goes to the library once RSS answered.
    feed.refresh(true);
    CHECK(feedIdle(feed));
    std::int64_t written = 0;
    for (const library::ChannelRecord &c : store.subscriptions(library::SubscriptionSort::Name))
        if (c.channelId == kNasa) written = c.latestUploadMs;
    CHECK_EQ(written, static_cast<std::int64_t>(newest * 1000));
    // Sorting by latest upload puts NASA first (the other channel has no uploads).
    const std::vector<library::ChannelRecord> latest = store.subscriptions(library::SubscriptionSort::LatestUpload);
    CHECK(!latest.empty() && latest.front().channelId == kNasa);

    // A store that did not open never prunes the feed (its list would be empty).
    library::LibraryStore closed(dir.path() + QStringLiteral("/missing/dir/x.db"), QStringLiteral("feed-closed"));
    FeedService other(&rig.net, &rig.client, nullptr, nullptr, [newest] { return newest + 3600; });
    ChannelList seeded;
    seeded.push_back(ChannelRef(kNasa, "NASA"));
    other.setChannels(seeded);
    other.start();
    CatalogLibraryBridge idle(&other, nullptr, nullptr, nullptr, [] { return kNowMs; });
    idle.attach(&closed, nullptr);
    CHECK(catalog_test::waitFor([&] { return other.cacheLoaded(); }));
    CHECK_EQ(other.channelCount(), 1);
    CHECK_EQ(idle.channelPushes(), 0);
}

// ---- hide watched (LIB-07) ---------------------------------------------------------------------

static void hideWatched() {
    // The shared row filter: watched videos are hidden and counted; channels never are.
    std::vector<Item> rows(3);
    rows[0].id = "aaaaaaaaaaa"; rows[1].id = "bbbbbbbbbbb";
    rows[2].kind = ItemKind::Channel; rows[2].id = "bbbbbbbbbbb";
    RowFilterSettings s;
    s.hideWatched = [](const std::string &id) { return id == "bbbbbbbbbbb"; };
    const RowFilterResult r = filterRows(rows, s);
    CHECK_EQ(static_cast<int>(r.visible.size()), 2);
    CHECK_EQ(r.hiddenWatched, 1);
    CHECK_EQ(r.hiddenTotal(), 1);

    QTemporaryDir dir;
    library::LibraryStore store(dbPath(dir), QStringLiteral("hide-watched"));
    CHECK(store.open());
    CHECK(store.subscribe(channel(kNasa, "NASA")));
    WatchStateIndex watch(&store);
    Rig rig;
    const QByteArray rss = catalog_test::fixture(QStringLiteral("rss_channel.xml"));
    const RssFeed parsed = RssParser::parse(rss, 200);
    qint64 newest = -1;
    for (const Item &i : parsed.items) newest = std::max<qint64>(newest, i.publishedEpoch);
    rig.net.on(QStringLiteral("channel_id=") + QLatin1String(kNasa), QByteArray(), 200, rss);
    FeedService feed(&rig.net, &rig.client, nullptr, nullptr, [newest] { return newest + 3600; });
    feed.start();
    CatalogLibraryBridge bridge(&feed, nullptr, nullptr, nullptr, [] { return kNowMs; });
    bridge.attach(&store, &watch);
    CHECK(WatchedRows::instance()->active());
    CHECK(catalog_test::waitFor([&] { return feed.cacheLoaded(); }));
    feed.refresh(true);
    CHECK(feedIdle(feed));
    const int all = static_cast<int>(feed.merged().rows.size());
    CHECK(all > 1);
    const Item first = feed.merged().rows.front();

    library::VideoEntry entry;
    entry.videoId = first.id;
    entry.title = first.title;
    store.markWatched(QString::fromStdString(first.id), entry);
    // Off by default: nothing hidden.
    catalog_test::waitFor([] { return false; }, 50);
    CHECK_EQ(static_cast<int>(feed.merged().rows.size()), all);
    watch.setHideWatched(true);
    CHECK(catalog_test::waitFor([&] { return feed.merged().hiddenWatched == 1; }));
    CHECK_EQ(static_cast<int>(feed.merged().rows.size()), all - 1);
    for (const Item &i : feed.merged().rows) CHECK(i.id != first.id);
    // Unmarking shows it again without a refresh.
    store.unmarkWatched(QString::fromStdString(first.id));
    CHECK(catalog_test::waitFor([&] { return feed.merged().hiddenWatched == 0; }));
    CHECK_EQ(static_cast<int>(feed.merged().rows.size()), all);
    // The search context follows its own option (library rule).
    store.markWatched(QString::fromStdString(first.id), entry);
    const auto search = WatchedRows::instance()->forContext(QStringLiteral("search"));
    const auto channelList = WatchedRows::instance()->forContext(QStringLiteral("channel"));
    CHECK(channelList && channelList(first.id));
    CHECK(search && !search(first.id));
    watch.setHideWatchedInSearch(true);
    CHECK(search(first.id));
    WatchedRows::instance()->setPredicate(WatchedRows::Predicate());
}

// LIB-07 acceptance: 3 watched among 10 rows -> 7 shown, in the feed and in a channel list;
// channel paging still continues.
static void watchedAcceptance() {
    QTemporaryDir dir;
    library::LibraryStore store(dbPath(dir), QStringLiteral("watched-acceptance"));
    CHECK(store.open());
    CHECK(store.subscribe(channel(kNasa, "NASA")));
    WatchStateIndex watch(&store);
    watch.setHideWatched(true);

    // derived: the recorded RSS answer cut to its first 10 entries.
    const QByteArray full = catalog_test::fixture(QStringLiteral("rss_channel.xml"));
    QByteArray ten;
    int entries = 0;
    const int start = full.indexOf("<entry>");
    ten = full.left(start);
    int pos = start;
    while (entries < 10) {
        const int end = full.indexOf("</entry>", pos);
        if (end < 0) break;
        ten += full.mid(pos, end + 8 - pos);
        pos = end + 8;
        ++entries;
    }
    ten += "\n</feed>\n";
    const RssFeed parsed = RssParser::parse(ten, 200);
    CHECK_EQ(static_cast<int>(parsed.items.size()), 10);
    qint64 newest = -1;
    for (const Item &i : parsed.items) newest = std::max<qint64>(newest, i.publishedEpoch);

    Rig rig;
    rig.net.on(QStringLiteral("channel_id=") + QLatin1String(kNasa), QByteArray(), 200, ten);
    FeedService feed(&rig.net, &rig.client, nullptr, nullptr, [newest] { return newest + 3600; });
    feed.start();
    CatalogLibraryBridge bridge(&feed, nullptr, nullptr, nullptr, [] { return kNowMs; });
    bridge.attach(&store, &watch);
    CHECK(catalog_test::waitFor([&] { return feed.cacheLoaded(); }));
    feed.refresh(true);
    CHECK(feedIdle(feed));
    CHECK_EQ(static_cast<int>(feed.merged().rows.size() + feed.merged().hiddenTotal()), 10);
    const int shownBefore = static_cast<int>(feed.merged().rows.size());
    std::vector<std::string> watched;
    for (int i = 0; i < 3 && i < shownBefore; ++i) watched.push_back(feed.merged().rows[static_cast<std::size_t>(i * 2)].id);
    for (const std::string &id : watched) {
        library::VideoEntry e;
        e.videoId = id;
        store.markWatched(QString::fromStdString(id), e);
    }
    CHECK(catalog_test::waitFor([&] { return feed.merged().hiddenWatched == 3; }));
    CHECK_EQ(static_cast<int>(feed.merged().rows.size()), shownBefore - 3);
    if (shownBefore == 10) CHECK_EQ(static_cast<int>(feed.merged().rows.size()), 7);

    // A channel list (Videos tab, recorded page 1 and its continuation).
    WorkLane lane(QStringLiteral("watched-channel"), 1);
    FakeTransport net;
    const QByteArray videos = catalog_test::fixture(QStringLiteral("channel_videos.json"));
    const QJsonObject p1 = QJsonDocument::fromJson(videos).object();
    InnerTubeReply r1;
    r1.outcome = Outcome::Ok;
    r1.httpStatus = 200;
    r1.json = p1;
    const ChannelPage page1 = ChannelService::parseBrowse(r1, QLatin1String(kNasa), QStringLiteral("videos"), QStringLiteral("en"));
    CHECK(!page1.continuation.isEmpty());
    net.on(QStringLiteral("browse"), "\"" + page1.continuation.toLatin1() + "\"", 200, catalog_test::fixture(QStringLiteral("channel_videos_p2.json")));
    net.on(QStringLiteral("browse"), QByteArray("EgZ2aWRlb3PyBgQKAjoA"), 200, videos);
    InnerTubeClient client(&net, &rig.identity, &lane, DiscoveryLocale());
    ChannelService service(&client);
    sailfish::ChannelModel model(&service);
    model.open(QLatin1String(kNasa));
    CHECK(catalog_test::waitFor([&] { return !model.busy() && model.count() >= 10; }));
    const int listed = model.count();
    std::vector<std::string> chosen;
    for (int i = 0; i < 3; ++i) chosen.push_back(model.visibleRows()[static_cast<std::size_t>(i * 3)].id);
    for (const std::string &id : chosen) {
        library::VideoEntry e;
        e.videoId = id;
        store.markWatched(QString::fromStdString(id), e);
    }
    CHECK(catalog_test::waitFor([&] { return model.hiddenWatched() == 3; }));
    CHECK_EQ(model.count(), listed - 3);
    for (const Item &i : model.visibleRows())
        CHECK(std::find(chosen.begin(), chosen.end(), i.id) == chosen.end());
    // Paging continues past the hidden rows.
    CHECK(model.canLoadMore());
    model.loadMore();
    CHECK(catalog_test::waitFor([&] { return !model.busy() && model.count() > listed - 3; }));
    CHECK_EQ(model.hiddenWatched(), 3);
    // Setting off: everything back in place.
    watch.setHideWatched(false);
    CHECK(catalog_test::waitFor([&] { return model.hiddenWatched() == 0; }));
    WatchedRows::instance()->setPredicate(WatchedRows::Predicate());
}

// ---- remote playlist mirrors (LIB-03) ----------------------------------------------------------

static void remotePlaylists() {
    QTemporaryDir dir;
    library::LibraryStore store(dbPath(dir), QStringLiteral("mirrors"));
    CHECK(store.open());
    library::RemotePlaylistRef ref;
    ref.remoteId = kList;
    ref.title = "Saved";
    const QString mirror = store.saveRemotePlaylist(ref);
    CHECK(!mirror.isEmpty());

    Rig rig;
    const QByteArray browse = catalog_test::fixture(QStringLiteral("playlist_browse.json"));
    rig.net.on(QStringLiteral("browse"), "\"browseId\":\"VLPLF98rxtslw6s\"", 200, browse);
    // The fixture's own continuation token leads into a synthetic chain: 50 pages x 100 rows.
    const QJsonObject browseJson = QJsonDocument::fromJson(browse).object();
    InnerTubeReply reply;
    reply.outcome = Outcome::Ok;
    reply.httpStatus = 200;
    reply.json = browseJson;
    const PlaylistPage page1 = PlaylistService::parseBrowse(reply, QLatin1String(kList), QStringLiteral("en"));
    CHECK(page1.outcome == Outcome::Ok);
    CHECK(!page1.continuation.isEmpty());
    const int firstRows = static_cast<int>(page1.items.size());
    QString token = page1.continuation;
    for (int p = 0; p < 50; ++p) {
        const QString next = QStringLiteral("chain-%1").arg(p + 1);
        // Quoted: "chain-1" must not match the request for "chain-10".
        rig.net.on(QStringLiteral("browse"), "\"" + token.toLatin1() + "\"", 200, chainPage(p * 100, 100, next));
        token = next;
    }
    PlaylistService service(&rig.client);
    CatalogLibraryBridge bridge(nullptr, &service, nullptr, nullptr, [] { return kNowMs; });
    bridge.attach(&store, nullptr);
    struct Finished { QString id; bool ok; int count; };
    QList<Finished> finished;
    QObject::connect(&bridge, &CatalogLibraryBridge::refreshFinished, [&finished](const QString &id, bool ok, int count) {
        Finished f; f.id = id; f.ok = ok; f.count = count; finished.append(f);
    });

    bridge.refresh(mirror);
    CHECK(bridge.busy() && bridge.isRefreshing(mirror));
    bridge.refresh(mirror);   // a second request for the running mirror is ignored
    CHECK(catalog_test::waitFor([&] { return finished.count() == 1; }, 20000));
    if (finished.count() != 1) return;
    CHECK_EQ(finished.at(0).id, mirror);
    CHECK(finished.at(0).ok);
    CHECK_EQ(finished.at(0).count, CatalogLibraryBridge::kMaxRemoteItems);
    const std::vector<library::VideoEntry> items = store.playlistItems(mirror);
    CHECK_EQ(static_cast<int>(items.size()), CatalogLibraryBridge::kMaxRemoteItems);
    for (int i = 0; i < firstRows && i < static_cast<int>(items.size()); ++i)
        CHECK_EQ(items[static_cast<std::size_t>(i)].videoId, page1.items[static_cast<std::size_t>(i)].id);
    CHECK_EQ(items[static_cast<std::size_t>(firstRows)].videoId, std::string("c0000000000"));
    // In order across pages, no gaps: row firstRows + k is chain item k.
    bool ordered = true;
    for (int k = 0; firstRows + k < static_cast<int>(items.size()); ++k) {
        char id[12];
        std::snprintf(id, sizeof id, "c%010d", k);
        ordered = ordered && items[static_cast<std::size_t>(firstRows + k)].videoId == id;
    }
    CHECK(ordered);
    CHECK(!items.front().thumbnailUrl.empty());
    library::PlaylistRecord record;
    CHECK(store.playlist(mirror, &record));
    CHECK_EQ(record.refreshedMs, static_cast<std::int64_t>(kNowMs));
    CHECK_EQ(record.title, page1.title.toStdString());   // the header follows the fetched list
    CHECK(!bridge.busy());

    // A failing continuation keeps the previous items (no partial replace).
    rig.net.rules.clear();
    rig.net.on(QStringLiteral("browse"), "\"browseId\":\"VLPLF98rxtslw6s\"", 200, browse);
    rig.net.on(QStringLiteral("browse"), page1.continuation.toLatin1(), 503, QByteArray());
    bridge.refresh(mirror);
    CHECK(catalog_test::waitFor([&] { return finished.count() == 2; }, 8000));
    if (finished.count() != 2) return;
    CHECK(!finished.at(1).ok);
    CHECK_EQ(static_cast<int>(store.playlistItems(mirror).size()), CatalogLibraryBridge::kMaxRemoteItems);

    // Not a mirror (watch later, unknown id): refused at once.
    bridge.refresh(QString::fromLatin1(library::watchLaterPlaylistId));
    bridge.refresh(QStringLiteral("no-such-list"));
    CHECK_EQ(finished.count(), 4);
    CHECK(finished.count() == 4 && !finished.at(2).ok && !finished.at(3).ok);
    CHECK(rig.net.hosts == QStringList() << QStringLiteral("www.youtube.com"));
}

// ---- import handles (IMP-01) -------------------------------------------------------------------

static void importHandles() {
    QTemporaryDir dir;
    library::LibraryStore store(dbPath(dir), QStringLiteral("handles"));
    CHECK(store.open());
    Rig rig;
    rig.net.on(QStringLiteral("navigation/resolve_url"), "\"url\":\"https://www.youtube.com/@NASA\"", 200,
               catalog_test::fixture(QStringLiteral("resolve_handle.json")));
    rig.net.on(QStringLiteral("navigation/resolve_url"), QByteArray(), 404, QByteArray("{}"));
    ChannelService channels(&rig.client);
    CatalogLibraryBridge bridge(nullptr, nullptr, &channels, nullptr, [] { return kNowMs; });
    bridge.attach(&store, nullptr);
    QList<QPair<int, int> > done;
    QObject::connect(&bridge, &CatalogLibraryBridge::handlesResolved, [&done](int ok, int failed) { done.append(qMakePair(ok, failed)); });

    library::ImportBatch batch;
    batch.source = "newpipe";
    library::PendingHandle nasa;
    nasa.url = "https://www.youtube.com/@NASA";
    nasa.name = "NASA";
    library::PendingHandle gone;
    gone.url = "https://www.youtube.com/@doesnotexist000";
    gone.name = "Gone";
    batch.pendingHandles.push_back(nasa);
    batch.pendingHandles.push_back(gone);
    const library::ImportSummary summary = store.applyImport(batch);
    CHECK_EQ(summary.handlesPending, 2);
    // The import's subscriptionsChanged starts the run (or the start-up timer does).
    if (!bridge.resolvingHandles()) bridge.resolvePendingHandles(false);
    CHECK(catalog_test::waitFor([&] { return done.count() == 1; }, 8000));
    if (done.count() != 1) return;
    CHECK_EQ(done.at(0).first, 1);
    CHECK_EQ(done.at(0).second, 1);
    CHECK(store.isSubscribed(QLatin1String(kNasa)));
    const std::vector<library::PendingHandle> left = store.pendingHandles();
    CHECK_EQ(static_cast<int>(left.size()), 1);
    CHECK(!left.empty() && left.front().url == gone.url);
    // An automatic run does not retry a URL that failed this session; an explicit retry does.
    const int calls = rig.net.count(QStringLiteral("navigation/resolve_url"));
    bridge.resolvePendingHandles(false);
    CHECK(!bridge.resolvingHandles());
    bridge.resolvePendingHandles(true);
    CHECK(catalog_test::waitFor([&] { return done.count() == 2; }, 8000));
    CHECK_EQ(rig.net.count(QStringLiteral("navigation/resolve_url")), calls + 1);
    CHECK_EQ(static_cast<int>(store.pendingHandles().size()), 1);
}

// ---- channel avatars: stored records <-> AvatarBook --------------------------------------------

static void avatarSync() {
    catalog::AvatarBook &book = catalog::AvatarBook::instance();
    book.clear();
    QTemporaryDir dir;
    library::LibraryStore store(dbPath(dir), QStringLiteral("avatars"));
    CHECK(store.open());
    library::ChannelRecord stored = channel(kNasa, "NASA");
    stored.avatarUrl = "https://yt3.ggpht.com/stored=s176";
    CHECK(store.subscribe(stored));
    CHECK(store.subscribe(channel(chan(7), "No avatar yet")));
    CatalogLibraryBridge bridge(nullptr, nullptr, nullptr, nullptr, [] { return kNowMs; });
    bridge.attach(&store, nullptr);
    // A stored avatar is known at once (before the feed's cached rows are painted).
    CHECK_EQ(book.lookup(kNasa), std::string("https://yt3.ggpht.com/stored=s176"));
    CHECK(book.lookup(chan(7)).empty());

    auto avatarOf = [&store](const std::string &id) {
        for (const library::ChannelRecord &c : store.subscriptions(library::SubscriptionSort::Name))
            if (c.channelId == id) return c.avatarUrl;
        return std::string("?");
    };
    // An avatar learned from an answer is written back to the record that had none.
    CHECK(book.learn(chan(7), "https://yt3.ggpht.com/learned=s176"));
    bridge.syncAvatars();
    CHECK_EQ(avatarOf(chan(7)), std::string("https://yt3.ggpht.com/learned=s176"));
    CHECK_EQ(avatarOf(kNasa), std::string("https://yt3.ggpht.com/stored=s176"));       // untouched
    // The channel's own page replaced the entry: the record follows; nothing ping-pongs.
    CHECK(book.learn(kNasa, "https://yt3.ggpht.com/fresh=s176", true));
    bridge.syncAvatars();
    CHECK_EQ(avatarOf(kNasa), std::string("https://yt3.ggpht.com/fresh=s176"));
    const std::uint64_t revision = book.revision();
    bridge.syncAvatars();
    CHECK_EQ(book.revision(), revision);
    // Channels that are not subscribed are never written.
    CHECK(book.learn(chan(9), "https://yt3.ggpht.com/other=s176"));
    bridge.syncAvatars();
    CHECK_EQ(static_cast<int>(store.subscriptions(library::SubscriptionSort::Name).size()), 2);
    book.clear();
}

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    std::setvbuf(stdout, nullptr, _IONBF, 0);   // keep the output of a test that aborts
    std::printf("feedSubscriptions\n");
    feedSubscriptions();
    std::printf("hideWatched\n");
    hideWatched();
    std::printf("watchedAcceptance\n");
    watchedAcceptance();
    std::printf("remotePlaylists\n");
    remotePlaylists();
    std::printf("avatarSync\n");
    avatarSync();
    std::printf("importHandles\n");
    importHandles();
    return catalog_test::finish("catalog_library");
}
