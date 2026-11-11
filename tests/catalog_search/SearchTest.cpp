// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Search package tests: DSC-01 (valid empty vs drift, second tier), DSC-08 (sp encoder, filter
// fixtures, mixed vs videos-only), DSC-09 (paging, all-Shorts page, 5-page cap), DSC-10 (cache),
// DSC-11 (suggestions, debounce, stale replies, caches, history), NFR-PERF-09 (host timing).
// Offline: every answer comes from recorded fixtures or variants derived from them in code.

#include "tests/catalog_common/TestSupport.h"
#include "Core/Catalog/Domain/AvatarBook.h"
#include <QFile>
#include <QDateTime>
#include <QTemporaryDir>

#include "Clients/Sailfish/Presentation/DiscoverySearchModel.h"
#include "Clients/Sailfish/Presentation/TapIntent.h"
#include "Clients/Sailfish/Presentation/SuggestionModel.h"
#include "Core/Catalog/Domain/SearchCache.h"
#include "Core/Catalog/Domain/SearchFilter.h"
#include "Core/Catalog/Domain/Video.h"
#include "Core/Catalog/Infrastructure/ClientIdentity.h"
#include "Core/Catalog/Infrastructure/InnerTubeClient.h"
#include "Core/Catalog/Infrastructure/Json.h"
#include "Core/Catalog/Infrastructure/SearchService.h"
#include "Core/Catalog/Infrastructure/SuggestService.h"
#include "Core/Catalog/Infrastructure/YtDlpSearchFallback.h"
#include "Core/Resolver/Infrastructure/YtDlpRunner.h"
#include "Core/Resolver/Infrastructure/YtDlpVideoService.h"

#include <QDir>
#include <QJsonArray>
#include <QSet>
#include <QSettings>
#include <QUrlQuery>
#include <memory>

using namespace nordtile::video::catalog;
using nordtile::video::sailfish::DiscoverySearchModel;
using nordtile::video::sailfish::TapIntent;
using nordtile::video::sailfish::SuggestionModel;
using catalog_test::FakeTransport;

namespace {

const qint64 kMin = 60 * 1000;

QByteArray bytes(const QJsonObject &o) { return QJsonDocument(o).toJson(QJsonDocument::Compact); }

int countKind(const std::vector<Item> &rows, ItemKind k) {
    int n = 0;
    for (const Item &i : rows) if (i.kind == k) ++n;
    return n;
}

// Captures the NFR-PERF-09 timing lines.
QStringList &timingLines() { static QStringList l; return l; }
// The per-request stage lines of InnerTubeClient (kept apart from the page-1 line).
QStringList &stageLines() { static QStringList l; return l; }
QtMessageHandler previousHandler = nullptr;
void captureMessages(QtMsgType type, const QMessageLogContext &ctx, const QString &msg) {
    if (msg.startsWith(QLatin1String("sailtube-timing innertube "))) stageLines().append(msg);
    else if (msg.startsWith(QLatin1String("sailtube-timing"))) timingLines().append(msg);
    if (previousHandler) previousHandler(type, ctx, msg);
}

// One discovery stack over a fake transport.
struct Stack {
    FakeTransport net;
    ClientIdentity identity;
    WorkLane lane;
    InnerTubeClient client;
    CatalogCounters counters;
    SearchService service;
    Stack() : identity(nullptr, QString()), lane(QStringLiteral("search-test"), 1),
              client(&net, &identity, &lane, DiscoveryLocale()), service(&client, &counters) {}
};

QString tokenOfFirstPage(const QString &fixture) {
    return SearchService::parseFirstPage(catalog_test::fixtureJson(fixture), QStringLiteral("en")).continuation;
}
QString tokenOfContinuation(const QString &fixture) {
    return SearchService::parseContinuation(catalog_test::fixtureJson(fixture), QStringLiteral("en")).continuation;
}

// A continuation answer holding `rows` (renderer objects) and an optional next token.
QJsonObject continuationAnswer(const QJsonArray &rows, const QString &next) {
    QJsonObject section;
    section.insert(QStringLiteral("contents"), rows);
    QJsonObject sectionHolder;
    sectionHolder.insert(QStringLiteral("itemSectionRenderer"), section);
    QJsonArray items;
    items.append(sectionHolder);
    if (!next.isEmpty()) {
        QJsonObject cmd; cmd.insert(QStringLiteral("token"), next);
        QJsonObject ep; ep.insert(QStringLiteral("continuationCommand"), cmd);
        QJsonObject cir; cir.insert(QStringLiteral("continuationEndpoint"), ep);
        QJsonObject holder; holder.insert(QStringLiteral("continuationItemRenderer"), cir);
        items.append(holder);
    }
    QJsonObject action; action.insert(QStringLiteral("continuationItems"), items);
    QJsonObject command; command.insert(QStringLiteral("appendContinuationItemsAction"), action);
    QJsonArray commands; commands.append(command);
    QJsonObject answer; answer.insert(QStringLiteral("onResponseReceivedCommands"), commands);
    return answer;
}

QJsonObject shortsLockup(const QString &videoId, const QString &title) {
    QJsonObject reel; reel.insert(QStringLiteral("videoId"), videoId);
    QJsonObject cmd; cmd.insert(QStringLiteral("reelWatchEndpoint"), reel);
    QJsonObject onTap; onTap.insert(QStringLiteral("innertubeCommand"), cmd);
    QJsonObject primary; primary.insert(QStringLiteral("content"), title);
    QJsonObject overlay; overlay.insert(QStringLiteral("primaryText"), primary);
    QJsonObject vm;
    vm.insert(QStringLiteral("entityId"), QStringLiteral("shorts-shelf-item-") + videoId);
    vm.insert(QStringLiteral("onTap"), onTap);
    vm.insert(QStringLiteral("overlayMetadata"), overlay);
    QJsonObject holder; holder.insert(QStringLiteral("shortsLockupViewModel"), vm);
    return holder;
}

// Derived variant (DSC-09 acceptance): search_en_p2 with every row replaced by a
// shortsLockupViewModel row for the same video id (channel rows dropped); the shelves of
// Shorts and the continuation item are kept as recorded.
QJsonObject allShortsPage2() {
    QJsonObject p2 = catalog_test::fixtureJson(QStringLiteral("search_en_p2.json"));
    QJsonArray commands = p2.value(QStringLiteral("onResponseReceivedCommands")).toArray();
    QJsonObject command = commands.at(0).toObject();
    QJsonObject action = command.value(QStringLiteral("appendContinuationItemsAction")).toObject();
    QJsonArray items = action.value(QStringLiteral("continuationItems")).toArray();
    for (int i = 0; i < items.size(); ++i) {
        QJsonObject holder = items.at(i).toObject();
        if (!holder.contains(QStringLiteral("itemSectionRenderer"))) continue;
        QJsonObject section = holder.value(QStringLiteral("itemSectionRenderer")).toObject();
        QJsonArray rows;
        for (const QJsonValue &row : section.value(QStringLiteral("contents")).toArray()) {
            const QJsonObject r = row.toObject();
            if (r.contains(QStringLiteral("videoRenderer"))) {
                const QJsonObject v = r.value(QStringLiteral("videoRenderer")).toObject();
                rows.append(shortsLockup(v.value(QStringLiteral("videoId")).toString(), json::text(v.value(QStringLiteral("title")))));
            } else if (r.contains(QStringLiteral("gridShelfViewModel"))) {
                rows.append(r);
            }
        }
        section.insert(QStringLiteral("contents"), rows);
        holder.insert(QStringLiteral("itemSectionRenderer"), section);
        items.replace(i, holder);
    }
    action.insert(QStringLiteral("continuationItems"), items);
    command.insert(QStringLiteral("appendContinuationItemsAction"), action);
    commands.replace(0, command);
    p2.insert(QStringLiteral("onResponseReceivedCommands"), commands);
    return p2;
}

// Adds a members-only badge to the first `left` videoRenderer rows below `v`.
QJsonValue withMembersBadges(const QJsonValue &v, int &left) {
    if (left <= 0) return v;
    if (v.isArray()) {
        QJsonArray a = v.toArray();
        for (int i = 0; i < a.size() && left > 0; ++i) a.replace(i, withMembersBadges(a.at(i), left));
        return a;
    }
    if (!v.isObject()) return v;
    QJsonObject o = v.toObject();
    if (o.contains(QStringLiteral("videoRenderer"))) {
        QJsonObject r = o.value(QStringLiteral("videoRenderer")).toObject();
        QJsonObject label;
        label.insert(QStringLiteral("label"), QStringLiteral("Members only"));
        label.insert(QStringLiteral("style"), QStringLiteral("BADGE_STYLE_TYPE_MEMBERS_ONLY"));
        QJsonObject badge; badge.insert(QStringLiteral("metadataBadgeRenderer"), label);
        QJsonArray badges = r.value(QStringLiteral("badges")).toArray();
        badges.append(badge);
        r.insert(QStringLiteral("badges"), badges);
        o.insert(QStringLiteral("videoRenderer"), r);
        --left;
        return o;
    }
    for (auto it = o.begin(); it != o.end() && left > 0; ++it) it.value() = withMembersBadges(it.value(), left);
    return o;
}

QByteArray tokenNeedle(const QString &token) { return "\"continuation\":\"" + token.toUtf8() + "\""; }

// A scripted second tier.
struct FakeFallback {
    int calls = 0;
    int lastOffset = -1;
    int rows = SearchService::kFallbackPageSize;
    Outcome outcome = Outcome::Ok;
    bool decline = false;
    SearchFallback hook() {
        SearchFallback f;
        f.search = [this](const QString &, const SearchFilter &, int offset, int count, QObject *context,
                          SearchFallback::Done done) -> quint64 {
            ++calls;
            lastOffset = offset;
            if (decline) return 0;
            std::vector<Item> v;
            for (int i = 0; i < rows && i < count; ++i) {
                Item it;
                it.id = QStringLiteral("fb%1").arg(offset + i, 9, 10, QLatin1Char('0')).toStdString();
                it.title = "fallback row";
                v.push_back(it);
            }
            const Outcome o = outcome;
            QTimer::singleShot(0, context, [done, o, v] { done(o, v); });
            return static_cast<quint64>(calls);
        };
        f.cancel = [](quint64) {};
        return f;
    }
};

}  // namespace

// NFR-PERF-09: after a shown page the next one is fetched ahead into the cache, once; scrolling
// then shows it without a request. A press while it is in flight waits for it (no second
// request). A failed prefetch never starts the yt-dlp tier. A new search drops it.
static void nextPagePrefetch() {
    const QString token = tokenOfFirstPage(QStringLiteral("search_en_p1.json"));
    CHECK(!token.isEmpty());
    {
        Stack s;
        s.net.on(QStringLiteral("youtubei/v1/search"), "\"query\"", 200, catalog_test::fixture(QStringLiteral("search_en_p1.json")));
        s.net.on(QStringLiteral("youtubei/v1/search"), "\"continuation\"", 200, catalog_test::fixture(QStringLiteral("search_en_p2.json")));
        DiscoverySearchModel m(&s.service);
        m.setPrefetch(true, 0);
        m.search(QStringLiteral("kurzgesagt"));
        CHECK(catalog_test::waitFor([&] { return !m.busy() && m.count() > 0; }));
        const int page1 = m.count();
        CHECK(catalog_test::waitFor([&] { return s.net.count(QStringLiteral("youtubei/v1/search"), "\"continuation\"") == 1; }));
        catalog_test::waitFor([] { return false; }, 100);
        CHECK_EQ(m.prefetches(), 1);
        CHECK_EQ(m.count(), page1);                        // nothing shown until the user scrolls
        m.loadMore();
        CHECK(m.count() > page1);                          // served from the cache at once
        CHECK(!m.busy());
        CHECK_EQ(s.net.count(QStringLiteral("youtubei/v1/search"), "\"continuation\""), 1);
    }
    {
        // In flight when the user scrolls: one request, shown when it lands.
        Stack s;
        s.net.on(QStringLiteral("youtubei/v1/search"), "\"query\"", 200, catalog_test::fixture(QStringLiteral("search_en_p1.json")));
        s.net.on(QStringLiteral("youtubei/v1/search"), "\"continuation\"", 200, catalog_test::fixture(QStringLiteral("search_en_p2.json")), 300);
        DiscoverySearchModel m(&s.service);
        m.setPrefetch(true, 0);
        m.search(QStringLiteral("kurzgesagt"));
        CHECK(catalog_test::waitFor([&] { return !m.busy() && m.count() > 0; }));
        const int page1 = m.count();
        CHECK(catalog_test::waitFor([&] { return s.net.count(QStringLiteral("youtubei/v1/search"), "\"continuation\"") == 1; }));
        m.loadMore();
        CHECK(m.busy());
        CHECK(catalog_test::waitFor([&] { return !m.busy() && m.count() > page1; }));
        CHECK_EQ(s.net.count(QStringLiteral("youtubei/v1/search"), tokenNeedle(token)), 1);   // page 2 asked once
    }
    {
        // A failing prefetch: no fallback call; the user's own scroll still gets the full chain.
        Stack s;
        FakeFallback fb;
        s.service.setFallback(fb.hook());
        s.net.on(QStringLiteral("youtubei/v1/search"), "\"query\"", 200, catalog_test::fixture(QStringLiteral("search_en_p1.json")));
        s.net.on(QStringLiteral("youtubei/v1/search"), "\"continuation\"", 503, QByteArray());
        DiscoverySearchModel m(&s.service);
        m.setPrefetch(true, 0);
        m.search(QStringLiteral("kurzgesagt"));
        CHECK(catalog_test::waitFor([&] { return !m.busy() && m.count() > 0; }));
        CHECK(catalog_test::waitFor([&] { return s.net.count(QStringLiteral("youtubei/v1/search"), "\"continuation\"") == 1; }));
        catalog_test::waitFor([] { return false; }, 100);
        CHECK_EQ(fb.calls, 0);
        CHECK(m.errorText().isEmpty());                    // a prefetch failure is silent
        m.loadMore();
        CHECK(catalog_test::waitFor([&] { return !m.busy(); }));
        CHECK_EQ(fb.calls, 1);                             // the user's request may fall back
    }
    {
        // A new search drops the pending prefetch.
        Stack s;
        s.net.on(QStringLiteral("youtubei/v1/search"), "\"query\"", 200, catalog_test::fixture(QStringLiteral("search_en_p1.json")));
        s.net.on(QStringLiteral("youtubei/v1/search"), "\"continuation\"", 200, catalog_test::fixture(QStringLiteral("search_en_p2.json")));
        DiscoverySearchModel m(&s.service);
        m.setPrefetch(true, 200);
        m.search(QStringLiteral("kurzgesagt"));
        CHECK(catalog_test::waitFor([&] { return !m.busy() && m.count() > 0; }));
        m.search(QStringLiteral("other"));
        CHECK(catalog_test::waitFor([&] { return !m.busy() && m.count() > 0; }));
        m.cancelPrefetch();
        catalog_test::waitFor([] { return false; }, 400);
        CHECK_EQ(s.net.count(QStringLiteral("youtubei/v1/search"), "\"continuation\""), 0);
    }
}

// Instant first paint: the last page 1 is written to disk, restored at start (rows, query,
// filters) and swapped in place by refreshInPlace() when the fresh page lands; a failed refresh
// keeps the restored rows. Nothing is written while the history is paused; old snapshots and
// a later normal search behave as usual.
static void lastSearchSnapshot() {
    QTemporaryDir dir;
    const QString path = dir.path() + QStringLiteral("/catalog/last_search.json");
    bool allowed = true;
    int page1 = 0;
    std::string firstId;
    {
        Stack s;
        s.net.on(QStringLiteral("youtubei/v1/search"), "\"query\"", 200, catalog_test::fixture(QStringLiteral("search_en_p1.json")));
        DiscoverySearchModel m(&s.service);
        m.setPrefetch(false);
        m.setSnapshot(path, [&allowed] { return allowed; });
        m.setSort(2);
        m.search(QStringLiteral("kurzgesagt"));
        CHECK(catalog_test::waitFor([&] { return !m.busy() && m.count() > 0; }));
        page1 = m.count();
        firstId = m.get(0).value(QStringLiteral("videoId")).toString().toStdString();
        CHECK(QFile::exists(path));
        CHECK_EQ(static_cast<int>(QFile(path).permissions() & (QFileDevice::ReadGroup | QFileDevice::ReadOther)), 0);
    }
    {
        // Start-up: rows on screen before any request.
        Stack s;
        s.net.on(QStringLiteral("youtubei/v1/search"), "\"query\"", 200, catalog_test::fixture(QStringLiteral("search_en_p1.json")), 200);
        DiscoverySearchModel m(&s.service);
        m.setPrefetch(false);
        m.setSnapshot(path);
        QElapsedTimer t; t.start();
        CHECK(m.restoreSnapshot());
        CHECK(t.elapsed() < 100);
        CHECK(m.restored());
        CHECK_EQ(m.count(), page1);
        CHECK_EQ(m.query(), QStringLiteral("kurzgesagt"));
        CHECK_EQ(m.sort(), 2);
        CHECK(!m.canLoadMore());                            // stale tokens are not used
        CHECK_EQ(s.net.requests.size(), 0);
        int resets = 0, removed = 0;
        QObject::connect(&m, &QAbstractItemModel::modelReset, [&resets] { ++resets; });
        QObject::connect(&m, &QAbstractItemModel::rowsRemoved, [&removed] { ++removed; });
        m.refreshInPlace();
        CHECK_EQ(m.count(), page1);                         // still shown while the request runs
        CHECK(catalog_test::waitFor([&] { return !m.busy() && !m.restored(); }));
        CHECK_EQ(m.count(), page1);
        CHECK_EQ(resets, 1);                                // one swap, never an empty list
        CHECK_EQ(removed, 0);
        CHECK(m.canLoadMore());
        CHECK_EQ(m.get(0).value(QStringLiteral("videoId")).toString().toStdString(), firstId);
    }
    {
        // A failed refresh keeps the restored rows; the next normal search clears as usual.
        Stack s;
        s.net.on(QStringLiteral("youtubei/v1/search"), "\"query\":\"kurzgesagt\"", 503, QByteArray());
        s.net.on(QStringLiteral("youtubei/v1/search"), "\"query\"", 200, catalog_test::fixture(QStringLiteral("search_en_p1.json")), 100);
        DiscoverySearchModel m(&s.service);
        m.setPrefetch(false);
        m.setSnapshot(path);
        CHECK(m.restoreSnapshot());
        m.refreshInPlace();
        CHECK(catalog_test::waitFor([&] { return !m.busy(); }));
        CHECK_EQ(m.count(), page1);
        CHECK(m.restored());
        m.search(QStringLiteral("other"));
        CHECK_EQ(m.count(), 0);                             // normal search: cleared at once
        CHECK(!m.restored());
        CHECK(catalog_test::waitFor([&] { return !m.busy() && m.count() > 0; }));
    }
    {
        // History paused: nothing is written; too old: not restored.
        QFile::remove(path);
        allowed = false;
        Stack s;
        s.net.on(QStringLiteral("youtubei/v1/search"), "\"query\"", 200, catalog_test::fixture(QStringLiteral("search_en_p1.json")));
        DiscoverySearchModel m(&s.service);
        m.setPrefetch(false);
        m.setSnapshot(path, [&allowed] { return allowed; });
        m.search(QStringLiteral("kurzgesagt"));
        CHECK(catalog_test::waitFor([&] { return !m.busy() && m.count() > 0; }));
        CHECK(!QFile::exists(path));
        allowed = true;
        m.search(QStringLiteral("kurzgesagt again"));
        CHECK(catalog_test::waitFor([&] { return !m.busy() && m.count() > 0; }));
        CHECK(QFile::exists(path));
        DiscoverySearchModel later(&s.service);
        later.setSnapshot(path);
        CHECK(!later.restoreSnapshot(QDateTime::currentMSecsSinceEpoch() + DiscoverySearchModel::kSnapshotMaxAgeMs + 1000));
        CHECK_EQ(later.count(), 0);
        m.clearSnapshot();
        CHECK(!QFile::exists(path));
    }
}

// E12: the likely-next-tap signals. Visible sets settle, are capped at 3 valid ids, are emitted
// only when they change and at most once per interval; presses become touchDown, a scroll
// becomes touchCancelled, a click becomes opened (no cancel); nothing while inactive.
static void tapIntentSignals() {
    qint64 now = 1000;
    bool active = true;
    TapIntent intent(nullptr, [&now] { return now; });
    intent.setActive([&active] { return active; });
    intent.setTimings(20, 2000);
    QList<QStringList> visible;
    QStringList downs, cancels, opens;
    QObject::connect(&intent, &TapIntent::likelyVisible, [&visible](const QStringList &ids) { visible.append(ids); });
    QList<int> downIds, cancelIds, openIds;
    QObject::connect(&intent, &TapIntent::touchDown, [&downs, &downIds](const QString &id, const QVariantMap &, int p) { downs.append(id); downIds.append(p); });
    QObject::connect(&intent, &TapIntent::touchCancelled, [&cancels, &cancelIds](const QString &id, int p) { cancels.append(id); cancelIds.append(p); });
    QObject::connect(&intent, &TapIntent::opened, [&opens, &openIds](const QString &id, int p) { opens.append(id); openIds.append(p); });

    intent.rowsVisible(QStringList() << QStringLiteral("aaaaaaaaaaa") << QStringLiteral("not an id") << QStringLiteral("bbbbbbbbbbb")
                                     << QStringLiteral("aaaaaaaaaaa") << QStringLiteral("ccccccccccc") << QStringLiteral("ddddddddddd"));
    intent.rowsVisible(QStringList() << QStringLiteral("aaaaaaaaaaa") << QStringLiteral("bbbbbbbbbbb") << QStringLiteral("ccccccccccc"));
    CHECK(catalog_test::waitFor([&] { return visible.size() == 1; }, 1000));
    CHECK(visible.value(0) == QStringList() << QStringLiteral("aaaaaaaaaaa") << QStringLiteral("bbbbbbbbbbb") << QStringLiteral("ccccccccccc"));
    // Same set again: nothing. A new set within 2 s waits for the interval.
    intent.rowsVisible(QStringList() << QStringLiteral("aaaaaaaaaaa") << QStringLiteral("bbbbbbbbbbb") << QStringLiteral("ccccccccccc"));
    catalog_test::waitFor([] { return false; }, 80);
    CHECK_EQ(visible.size(), 1);
    now += 500;
    intent.setTimings(20, 300);   // shorter interval so the deferred emit is observable
    intent.rowsVisible(QStringList() << QStringLiteral("eeeeeeeeeee"));
    catalog_test::waitFor([] { return false; }, 60);
    CHECK_EQ(visible.size(), 2);  // 500 ms > 300 ms interval: emitted after settling
    CHECK(visible.value(1) == QStringList() << QStringLiteral("eeeeeeeeeee"));

    // Presses.
    QVariantMap hints;
    hints.insert(QStringLiteral("title"), QStringLiteral("T"));
    intent.pressed(QStringLiteral("aaaaaaaaaaa"), hints);
    intent.released(QStringLiteral("aaaaaaaaaaa"), false);          // turned into a scroll
    intent.pressed(QStringLiteral("bbbbbbbbbbb"), hints);
    intent.opening(QStringLiteral("bbbbbbbbbbb"));                  // tapped (page, before open())
    intent.opening(QStringLiteral("bbbbbbbbbbb"));                  // the item's own click: dropped
    intent.released(QStringLiteral("bbbbbbbbbbb"), true);
    intent.pressed(QStringLiteral("bad"), hints);                   // not a video id
    CHECK(downs == QStringList() << QStringLiteral("aaaaaaaaaaa") << QStringLiteral("bbbbbbbbbbb"));
    CHECK(cancels == QStringList() << QStringLiteral("aaaaaaaaaaa"));
    CHECK(opens == QStringList() << QStringLiteral("bbbbbbbbbbb"));
    // Press ids: increasing, the cancel and the open carry their press's id.
    CHECK(downIds == QList<int>() << 1 << 2);
    CHECK(cancelIds == QList<int>() << 1);
    CHECK(openIds == QList<int>() << 2);
    now += 2000;
    intent.opening(QStringLiteral("bbbbbbbbbbb"));                  // no press (e.g. a link): fresh id
    CHECK(openIds == QList<int>() << 2 << 3);

    // In the background: nothing.
    active = false;
    now += 5000;
    intent.pressed(QStringLiteral("ccccccccccc"), hints);
    intent.rowsVisible(QStringList() << QStringLiteral("fffffffffff"));
    catalog_test::waitFor([] { return false; }, 60);
    CHECK_EQ(downs.size(), 2);
    CHECK_EQ(visible.size(), 2);
}

// YouTube-style cards: search rows have channelAvatar (their own owner avatar; a channel row's is
// the channel's), and it survives the last-search snapshot.
static void channelAvatars() {
    AvatarBook::instance().clear();
    QTemporaryDir dir;
    const QString path = dir.path() + QStringLiteral("/last_search.json");
    int videoRows = 0;
    {
        Stack s;
        s.net.on(QStringLiteral("youtubei/v1/search"), "\"query\"", 200, catalog_test::fixture(QStringLiteral("search_en_p1.json")));
        DiscoverySearchModel m(&s.service);
        m.setPrefetch(false);
        m.setSnapshot(path);
        m.search(QStringLiteral("kurzgesagt"));
        CHECK(catalog_test::waitFor([&] { return !m.busy() && m.count() > 0; }));
        CHECK(m.roleNames().values().contains("channelAvatar"));
        for (int i = 0; i < m.count(); ++i) {
            const QVariantMap row = m.get(i);
            const QString avatar = row.value(QStringLiteral("channelAvatar")).toString();
            // A channel row's picture is its avatar in both sizes.
            if (row.value(QStringLiteral("kind")).toString() == QLatin1String("channel"))
                CHECK_EQ(row.value(QStringLiteral("thumbnailLarge")).toString(), row.value(QStringLiteral("thumbnail")).toString());
            if (row.value(QStringLiteral("kind")).toString() == QLatin1String("video")) {
                ++videoRows;
                CHECK(avatar.startsWith(QLatin1String("https://")));
                CHECK_EQ(avatar, row.value(QStringLiteral("avatar")).toString());   // the row's own
                // thumbnailLarge: the 1280x720 picture of the row's video, or the 480x360 one when
                // the answer named only smaller pictures for it; the small one stays for compact rows.
                const QString large = row.value(QStringLiteral("thumbnailLarge")).toString();
                const QString id = row.value(QStringLiteral("videoId")).toString();
                CHECK(large == QStringLiteral("https://i.ytimg.com/vi/%1/hq720.jpg").arg(id) ||
                      large == QStringLiteral("https://i.ytimg.com/vi/%1/hqdefault.jpg").arg(id));
                CHECK(row.value(QStringLiteral("thumbnail")).toString().endsWith(QLatin1String("/mqdefault.jpg")));
            }
        }
        CHECK(videoRows >= 10);
    }
    {
        AvatarBook::instance().clear();   // a new start: the snapshot alone brings the avatars back
        Stack s;
        DiscoverySearchModel m(&s.service);
        m.setSnapshot(path);
        CHECK(m.restoreSnapshot());
        int with = 0;
        for (int i = 0; i < m.count(); ++i)
            if (m.get(i).value(QStringLiteral("channelAvatar")).toString().startsWith(QLatin1String("https://"))) ++with;
        CHECK(with >= videoRows);
    }
    AvatarBook::instance().clear();
}

// ---------------------------------------------------------------------------------------------
// DSC-08: the sp encoder against YouTube's canonical codes, each derived by decoding.

static void encoderCanonical() {
    struct Case { const char *sp; SearchFilter filter; };
    std::vector<Case> cases;
    auto add = [&cases](const char *sp, SearchSort s, UploadDateFilter d, ResultTypeFilter t, DurationFilter l, bool live) {
        Case c; c.sp = sp; c.filter.sort = s; c.filter.uploadDate = d; c.filter.type = t; c.filter.duration = l; c.filter.live = live;
        cases.push_back(c);
    };
    const SearchSort R = SearchSort::Relevance;
    const UploadDateFilter AD = UploadDateFilter::Any;
    const ResultTypeFilter AT = ResultTypeFilter::Any;
    const DurationFilter AL = DurationFilter::Any;
    add("EgIQAQ%3D%3D", R, AD, ResultTypeFilter::Video, AL, false);      // 12 02 10 01
    add("EgIQAg%3D%3D", R, AD, ResultTypeFilter::Channel, AL, false);    // 12 02 10 02
    add("EgIQAw%3D%3D", R, AD, ResultTypeFilter::Playlist, AL, false);   // 12 02 10 03
    add("EgJAAQ%3D%3D", R, AD, AT, AL, true);                            // 12 02 40 01 (field 8)
    add("EgQIARAB", R, UploadDateFilter::LastHour, ResultTypeFilter::Video, AL, false);   // 12 04 08 01 10 01
    add("CAI%3D", SearchSort::UploadDate, AD, AT, AL, false);           // 08 02
    add("CAM%3D", SearchSort::Views, AD, AT, AL, false);                // 08 03
    add("CAE%3D", SearchSort::Rating, AD, AT, AL, false);               // 08 01
    add("EgIYAQ%3D%3D", R, AD, AT, DurationFilter::Under4Minutes, false);     // 12 02 18 01
    add("EgIYAg%3D%3D", R, AD, AT, DurationFilter::Over20Minutes, false);     // 12 02 18 02
    add("EgIYAw%3D%3D", R, AD, AT, DurationFilter::From4To20Minutes, false);  // 12 02 18 03
    add("EgIIAg%3D%3D", R, UploadDateFilter::Today, AT, AL, false);           // 12 02 08 02
    add("EgIIAw%3D%3D", R, UploadDateFilter::Week, AT, AL, false);
    add("EgIIBA%3D%3D", R, UploadDateFilter::Month, AT, AL, false);
    add("EgIIBQ%3D%3D", R, UploadDateFilter::Year, AT, AL, false);
    add("EgQIARAD", R, UploadDateFilter::LastHour, ResultTypeFilter::Playlist, AL, false);   // search_empty's params
    add("CAISAhAB", SearchSort::UploadDate, AD, ResultTypeFilter::Video, AL, false);          // 08 02 12 02 10 01
    add("EgYQARgDQAE%3D", R, AD, ResultTypeFilter::Video, DurationFilter::From4To20Minutes, true);
    for (const Case &c : cases) {
        CHECK_EQ(searchSp(c.filter), std::string(c.sp));
        SearchFilter decoded;
        CHECK(decodeSearchParams(c.sp, &decoded));
        CHECK(decoded == c.filter);
        CHECK_EQ(sp::percentEncode(searchParams(c.filter)), std::string(c.sp));
        CHECK_EQ(sp::percentDecode(c.sp), searchParams(c.filter));
    }
    // Field order is ascending inside the sub-message; relevance and "any" are omitted.
    const std::vector<std::uint8_t> b = encodeSearchFilterBytes(cases[17].filter);
    const std::vector<std::uint8_t> expected = {0x12, 0x06, 0x10, 0x01, 0x18, 0x03, 0x40, 0x01};
    CHECK(b == expected);
    CHECK(searchParams(SearchFilter()).empty());
    CHECK(SearchFilter().isDefault());
    // Videos-only mode pins type=video; mixed mode leaves it open; a chosen type always wins.
    CHECK_EQ(searchParams(effectiveSearchFilter(SearchFilter(), false)), std::string("EgIQAQ=="));
    CHECK(searchParams(effectiveSearchFilter(SearchFilter(), true)).empty());
    SearchFilter ch; ch.type = ResultTypeFilter::Channel;
    CHECK_EQ(searchParams(effectiveSearchFilter(ch, false)), std::string("EgIQAg=="));
    // Malformed or unknown codes are refused.
    CHECK(!decodeSearchParams("EgIgAQ%3D%3D", nullptr));   // field 4: not ours
    CHECK(!decodeSearchParams("EgIQBA%3D%3D", nullptr));   // type 4
    CHECK(!decodeSearchParams("Eg", nullptr));             // truncated
    CHECK(!decodeSearchParams("E!", nullptr));
    std::vector<std::uint8_t> out;
    CHECK(sp::base64Decode("CAI=", &out) && out.size() == 2 && out[0] == 0x08 && out[1] == 0x02);
    CHECK_EQ(sp::base64Encode(std::vector<std::uint8_t>{0x08, 0x02, 0x12}), std::string("CAIS"));
}

// ---------------------------------------------------------------------------------------------
// DSC-08: the recorded filter fixtures decode to the expected filters and parse to the right kinds.

static void filterFixtures() {
    // The manifest is not gzipped; the suites run from the repository root.
    QFile mf(QStringLiteral("tests/fixtures/catalog/MANIFEST.json"));
    CHECK(mf.open(QIODevice::ReadOnly));
    const QJsonObject manifest = QJsonDocument::fromJson(mf.readAll()).object();
    auto paramsOf = [&manifest](const QString &name) -> std::string {
        QString found;
        json::forEachKey(manifest, name + QStringLiteral(".gz"), [&found](const QJsonObject &o) {
            found = json::at(o, QStringLiteral("request.body.params")).toString();
            return false;
        });
        return found.toStdString();
    };
    struct Case { const char *fixture; ResultTypeFilter type; UploadDateFilter date; bool live; };
    const Case cases[] = {
        {"search_filter_channel.json", ResultTypeFilter::Channel, UploadDateFilter::Any, false},
        {"search_filter_playlist.json", ResultTypeFilter::Playlist, UploadDateFilter::Any, false},
        {"search_filter_live.json", ResultTypeFilter::Any, UploadDateFilter::Any, true},
        {"search_filter_hour_video.json", ResultTypeFilter::Video, UploadDateFilter::LastHour, false},
        {"search_empty.json", ResultTypeFilter::Playlist, UploadDateFilter::LastHour, false},
    };
    for (const Case &c : cases) {
        SearchFilter f; f.type = c.type; f.uploadDate = c.date; f.live = c.live;
        const std::string recorded = paramsOf(QString::fromLatin1(c.fixture));
        CHECK(!recorded.empty());
        CHECK_EQ(searchParams(f), recorded);                       // our encoder sends what was recorded
        SearchFilter decoded;
        CHECK(decodeSearchParams(recorded, &decoded) && decoded == f);
    }
    const QString en = QStringLiteral("en");
    {
        const SearchResult r = SearchService::parseFirstPage(catalog_test::fixtureJson(QStringLiteral("search_filter_channel.json")), en);
        CHECK(r.outcome == Outcome::Ok);
        CHECK(countKind(r.rows, ItemKind::Channel) >= 10);
        CHECK_EQ(countKind(r.rows, ItemKind::Channel), static_cast<int>(r.rows.size()));
        CHECK(!r.continuation.isEmpty());
        for (const Item &i : r.rows) CHECK(validChannelId(i.id));
    }
    {
        const SearchResult r = SearchService::parseFirstPage(catalog_test::fixtureJson(QStringLiteral("search_filter_playlist.json")), en);
        CHECK(r.outcome == Outcome::Ok);
        CHECK(countKind(r.rows, ItemKind::Playlist) >= 10);
        CHECK_EQ(countKind(r.rows, ItemKind::Playlist), static_cast<int>(r.rows.size()));
        for (const Item &i : r.rows) CHECK(i.videoCount > 0);
    }
    {
        const SearchResult r = SearchService::parseFirstPage(catalog_test::fixtureJson(QStringLiteral("search_filter_live.json")), en);
        CHECK(r.outcome == Outcome::Ok);
        int live = 0;
        for (const Item &i : r.rows) if (i.liveStatus == nordtile::video::media::LiveStatus::IsLive) ++live;
        CHECK(r.rows.size() >= 10);
        CHECK(live * 10 >= static_cast<int>(r.rows.size()) * 8);   // the live filter returns live rows
    }
    {
        const SearchResult r = SearchService::parseFirstPage(catalog_test::fixtureJson(QStringLiteral("search_filter_hour_video.json")), en);
        CHECK(r.outcome == Outcome::Ok);
        CHECK(countKind(r.rows, ItemKind::Video) >= 10);
        CHECK_EQ(countKind(r.rows, ItemKind::Channel) + countKind(r.rows, ItemKind::Playlist), 0);
        int recent = 0, aged = 0;
        // Uploads are recent; finished streams ("Streamed 6 hours ago") count from their start.
        for (const Item &i : r.rows) {
            if (i.publishedAgeSeconds < 0 || i.publishedText.find("Streamed") == 0) continue;
            ++aged;
            if (i.publishedAgeSeconds <= 2 * 3600) ++recent;
        }
        CHECK(aged >= 5);
        CHECK_EQ(recent, aged);
    }
    // Mixed answer (no params) holds videos, channels and Shorts; de/ja parse comparably.
    for (const char *f : {"search_en_p1.json", "search_de_p1.json", "search_ja_p1.json"}) {
        const QString hl = QString::fromLatin1(f).mid(7, 2);
        const SearchResult r = SearchService::parseFirstPage(catalog_test::fixtureJson(QString::fromLatin1(f)), hl);
        CHECK(r.outcome == Outcome::Ok);
        CHECK(countKind(r.rows, ItemKind::Video) >= 10);
        CHECK(countKind(r.rows, ItemKind::Channel) >= 1);
        CHECK(!r.continuation.isEmpty());
    }
}

// ---------------------------------------------------------------------------------------------
// DSC-01: valid empty versus shape missing, for page 1 and continuations.

static void validEmptyVsDrift() {
    const QString en = QStringLiteral("en");
    const QJsonObject empty = catalog_test::fixtureJson(QStringLiteral("search_empty.json"));
    CHECK(SearchService::parseFirstPage(empty, en).outcome == Outcome::ValidEmpty);
    // Derived: the container removed -> drift.
    QJsonObject noContainer = empty; noContainer.remove(QStringLiteral("contents"));
    CHECK(SearchService::parseFirstPage(noContainer, en).outcome == Outcome::ShapeDrift);
    QJsonObject p1 = catalog_test::fixtureJson(QStringLiteral("search_en_p1.json"));
    CHECK(SearchService::parseFirstPage(p1, en).outcome == Outcome::Ok);
    p1.remove(QStringLiteral("contents"));
    CHECK(SearchService::parseFirstPage(p1, en).outcome == Outcome::ShapeDrift);
    CHECK(SearchService::parseFirstPage(QJsonObject(), en).outcome == Outcome::ShapeDrift);

    QJsonObject p2 = catalog_test::fixtureJson(QStringLiteral("search_en_p2.json"));
    const SearchResult r2 = SearchService::parseContinuation(p2, en);
    CHECK(r2.outcome == Outcome::Ok);
    CHECK(r2.rows.size() >= 15);
    CHECK(!r2.continuation.isEmpty());
    // Page 3 arrives as a reloadContinuationItemsCommand (body slot) holding Shorts shelves.
    const SearchResult r3 = SearchService::parseContinuation(catalog_test::fixtureJson(QStringLiteral("search_en_p3.json")), en);
    CHECK(r3.outcome == Outcome::Ok);
    CHECK(countKind(r3.rows, ItemKind::Short) >= 10);
    CHECK(!r3.continuation.isEmpty());
    CHECK(SearchService::parseContinuation(continuationAnswer(QJsonArray(), QString()), en).outcome == Outcome::ValidEmpty);
    p2.remove(QStringLiteral("onResponseReceivedCommands"));
    CHECK(SearchService::parseContinuation(p2, en).outcome == Outcome::ShapeDrift);

    // Through the model: valid empty shows "no results", not an error.
    Stack s;
    s.net.on(QStringLiteral("youtubei/v1/search"), QByteArray(), 200, catalog_test::fixture(QStringLiteral("search_empty.json")));
    DiscoverySearchModel m(&s.service);
    m.search(QStringLiteral("qzxvwk jjvqpl xqzwvk fixture sailtube"));
    CHECK(catalog_test::waitFor([&] { return !m.busy(); }));
    CHECK(m.validEmpty());
    CHECK_EQ(m.count(), 0);
    CHECK(m.errorText().isEmpty());
    CHECK(!m.canLoadMore());
    CHECK_EQ(s.counters.value(Area::Search, Outcome::ValidEmpty), static_cast<std::int64_t>(1));
    CHECK_EQ(s.counters.driftTotal(), static_cast<std::int64_t>(0));

    // HTTP failures are never drift.
    Stack f;
    f.net.on(QStringLiteral("youtubei/v1/search"), QByteArray(), 429, QByteArray());
    DiscoverySearchModel m2(&f.service);
    m2.search(QStringLiteral("x"));
    CHECK(catalog_test::waitFor([&] { return !m2.busy(); }));
    CHECK(!m2.errorText().isEmpty());
    CHECK_EQ(f.counters.value(Area::Search, Outcome::RateLimited), static_cast<std::int64_t>(1));
    CHECK_EQ(f.counters.driftTotal(), static_cast<std::int64_t>(0));
}

// ---------------------------------------------------------------------------------------------
// DSC-01 second tier: fallback on drift and on 5xx, never on valid empty.

static void fallbackTier() {
    QJsonObject drift = catalog_test::fixtureJson(QStringLiteral("search_en_p1.json"));
    drift.remove(QStringLiteral("contents"));   // derived: container removed
    struct Case { int status; QByteArray body; bool expectFallback; Outcome firstTier; };
    const QList<Case> cases = {
        {200, bytes(drift), true, Outcome::ShapeDrift},
        {503, QByteArray(), true, Outcome::Http5xx},
        {200, catalog_test::fixture(QStringLiteral("search_empty.json")), false, Outcome::ValidEmpty},
        {200, catalog_test::fixture(QStringLiteral("search_en_p1.json")), false, Outcome::Ok},
    };
    for (const Case &c : cases) {
        Stack s;
        FakeFallback fb;
        s.service.setFallback(fb.hook());
        s.net.on(QStringLiteral("youtubei/v1/search"), QByteArray(), c.status, c.body);
        bool got = false; SearchResult res;
        s.service.firstPage(QStringLiteral("kurzgesagt"), SearchFilter(), [&](const SearchResult &r) { res = r; got = true; });
        CHECK(catalog_test::waitFor([&] { return got; }));
        CHECK_EQ(fb.calls, c.expectFallback ? 1 : 0);
        CHECK_EQ(res.fallback, c.expectFallback);
        CHECK(res.firstTierOutcome == c.firstTier);
        CHECK_EQ(s.counters.value(Area::Search, c.firstTier), static_cast<std::int64_t>(1));
        if (c.expectFallback) {
            CHECK(res.outcome == Outcome::Ok);
            CHECK_EQ(static_cast<int>(res.rows.size()), SearchService::kFallbackPageSize);
            CHECK_EQ(fb.lastOffset, 0);
            CHECK_EQ(res.continuation, SearchService::fallbackToken(SearchService::kFallbackPageSize));
            // Fallback paging continues on the second tier with offsets, without HTTP.
            const int before = s.net.requests.size();
            got = false;
            s.service.nextPage(QStringLiteral("kurzgesagt"), SearchFilter(), res.continuation, 20, [&](const SearchResult &r) { res = r; got = true; });
            CHECK(catalog_test::waitFor([&] { return got; }));
            CHECK_EQ(fb.lastOffset, 20);
            CHECK_EQ(s.net.requests.size(), before);
            CHECK(res.fallback && res.outcome == Outcome::Ok);
        }
    }
    // A failing or declining second tier leaves the first-tier class visible.
    {
        Stack s;
        FakeFallback fb; fb.outcome = Outcome::Network;
        s.service.setFallback(fb.hook());
        s.net.on(QStringLiteral("youtubei/v1/search"), QByteArray(), 503, QByteArray());
        bool got = false; SearchResult res;
        s.service.firstPage(QStringLiteral("q"), SearchFilter(), [&](const SearchResult &r) { res = r; got = true; });
        CHECK(catalog_test::waitFor([&] { return got; }));
        CHECK_EQ(fb.calls, 1);
        CHECK(res.outcome == Outcome::Http5xx);
        CHECK(!res.fallback);
    }
    {
        Stack s;
        FakeFallback fb; fb.decline = true;
        s.service.setFallback(fb.hook());
        s.net.on(QStringLiteral("youtubei/v1/search"), QByteArray(), 200, bytes(drift));
        bool got = false; SearchResult res;
        s.service.firstPage(QStringLiteral("q"), SearchFilter(), [&](const SearchResult &r) { res = r; got = true; });
        CHECK(catalog_test::waitFor([&] { return got; }));
        CHECK(res.outcome == Outcome::ShapeDrift);
    }
    // Drift on a continuation falls back with the rows received so far as offset.
    {
        Stack s;
        FakeFallback fb;
        s.service.setFallback(fb.hook());
        s.net.on(QStringLiteral("youtubei/v1/search"), QByteArray(), 200, "{\"responseContext\":{}}");
        bool got = false; SearchResult res;
        s.service.nextPage(QStringLiteral("q"), SearchFilter(), QStringLiteral("TOKEN"), 37, [&](const SearchResult &r) { res = r; got = true; });
        CHECK(catalog_test::waitFor([&] { return got; }));
        CHECK_EQ(fb.lastOffset, 37);
        CHECK(res.fallback);
        CHECK(res.firstTierOutcome == Outcome::ShapeDrift);
    }
    // The model shows fallback rows and says so.
    {
        Stack s;
        FakeFallback fb;
        s.service.setFallback(fb.hook());
        s.net.on(QStringLiteral("youtubei/v1/search"), QByteArray(), 502, QByteArray());
        DiscoverySearchModel m(&s.service);
        m.search(QStringLiteral("kurzgesagt"));
        CHECK(catalog_test::waitFor([&] { return !m.busy(); }));
        CHECK_EQ(m.count(), SearchService::kFallbackPageSize);
        CHECK(m.fallbackActive());
        CHECK(m.errorText().isEmpty());
        CHECK(m.canLoadMore());
    }
    // The yt-dlp adapter: plain video searches only; without a runtime it declines.
    {
        SearchFilter plain;
        CHECK(ytDlpSearchSupports(plain));
        SearchFilter video; video.type = ResultTypeFilter::Video;
        CHECK(ytDlpSearchSupports(video));
        SearchFilter hour; hour.uploadDate = UploadDateFilter::LastHour;
        CHECK(!ytDlpSearchSupports(hour));
        SearchFilter channels; channels.type = ResultTypeFilter::Channel;
        CHECK(!ytDlpSearchSupports(channels));
        SearchFilter live; live.live = true;
        CHECK(!ytDlpSearchSupports(live));
        nordtile::video::resolver::RunnerOptions options;   // no runtime configured
        nordtile::video::resolver::YtDlpRunner runner(options);
        nordtile::video::resolver::YtDlpVideoService ytdlp(&runner);
        const SearchFallback hook = makeYtDlpSearchFallback(&ytdlp);
        CHECK(static_cast<bool>(hook));
        int called = 0;
        const quint64 id = hook.search(QStringLiteral("q"), plain, 0, 20, &ytdlp, [&called](Outcome, const std::vector<Item> &) { ++called; });
        CHECK_EQ(id, static_cast<quint64>(0));   // declined: runtime missing
        Stack s;
        s.service.setFallback(hook);
        s.net.on(QStringLiteral("youtubei/v1/search"), QByteArray(), 503, QByteArray());
        bool got = false; SearchResult res;
        s.service.firstPage(QStringLiteral("q"), plain, [&](const SearchResult &r) { res = r; got = true; });
        CHECK(catalog_test::waitFor([&] { return got; }));
        CHECK(res.outcome == Outcome::Http5xx);
        CHECK_EQ(called, 0);
    }
}

// ---------------------------------------------------------------------------------------------
// DSC-08/09: request bodies and paging p1 -> p2 -> p3 with continuation tokens.

static void pagingThreePages() {
    const QString t1 = tokenOfFirstPage(QStringLiteral("search_en_p1.json"));
    const QString t2 = tokenOfContinuation(QStringLiteral("search_en_p2.json"));
    const QString t3 = tokenOfContinuation(QStringLiteral("search_en_p3.json"));
    CHECK(!t1.isEmpty() && !t2.isEmpty() && !t3.isEmpty() && t1 != t2 && t2 != t3);
    Stack s;
    s.net.on(QStringLiteral("youtubei/v1/search"), tokenNeedle(t1), 200, catalog_test::fixture(QStringLiteral("search_en_p2.json")));
    s.net.on(QStringLiteral("youtubei/v1/search"), tokenNeedle(t2), 200, catalog_test::fixture(QStringLiteral("search_en_p3.json")));
    s.net.on(QStringLiteral("youtubei/v1/search"), "\"query\":\"kurzgesagt\"", 200, catalog_test::fixture(QStringLiteral("search_en_p1.json")));
    DiscoverySearchModel m(&s.service);
    CHECK(m.mixed());
    m.search(QStringLiteral("  kurzgesagt \n"));
    CHECK_EQ(m.query(), QStringLiteral("kurzgesagt"));
    CHECK(m.busy());
    CHECK(catalog_test::waitFor([&] { return !m.busy(); }));
    CHECK_EQ(s.net.requests.size(), 1);
    const QJsonObject body1 = QJsonDocument::fromJson(s.net.requests.at(0).body).object();
    CHECK_EQ(body1.value(QStringLiteral("query")).toString(), QStringLiteral("kurzgesagt"));
    CHECK(!body1.contains(QStringLiteral("params")));          // mixed mode, default filter
    CHECK(!body1.contains(QStringLiteral("continuation")));
    const int afterP1 = m.count();
    CHECK(afterP1 >= 20);
    CHECK(m.canLoadMore());
    // Kinds and roles.
    bool sawChannel = false, sawShort = false;
    const QHash<int, QByteArray> roles = m.roleNames();
    for (const char *r : {"videoId", "title", "channel", "channelId", "duration", "durationSeconds", "viewCount", "thumbnail", "live",
                          "kind", "itemId", "upcoming", "membersOnly", "isShort", "avatar", "subscriberCount", "videoCount"})
        CHECK(roles.values().contains(QByteArray(r)));
    for (int i = 0; i < m.count(); ++i) {
        const QVariantMap row = m.get(i);
        const QString kind = row.value(QStringLiteral("kind")).toString();
        if (kind == QLatin1String("channel")) {
            sawChannel = true;
            CHECK(row.value(QStringLiteral("videoId")).toString().isEmpty());
            CHECK(validChannelId(row.value(QStringLiteral("channelId")).toString().toStdString()));
        } else if (kind == QLatin1String("short")) {
            sawShort = true;
            CHECK(row.value(QStringLiteral("isShort")).toBool());
        }
        if (kind == QLatin1String("video")) CHECK(validVideoId(row.value(QStringLiteral("videoId")).toString().toStdString()));
    }
    CHECK(sawChannel && sawShort);

    m.loadMore();
    CHECK(catalog_test::waitFor([&] { return !m.busy(); }));
    CHECK_EQ(s.net.requests.size(), 2);
    const QJsonObject body2 = QJsonDocument::fromJson(s.net.requests.at(1).body).object();
    CHECK_EQ(body2.value(QStringLiteral("continuation")).toString(), t1);
    CHECK(!body2.contains(QStringLiteral("query")));
    const int afterP2 = m.count();
    CHECK(afterP2 > afterP1);
    m.loadMore();
    CHECK(catalog_test::waitFor([&] { return !m.busy(); }));
    CHECK_EQ(s.net.requests.size(), 3);
    CHECK_EQ(QJsonDocument::fromJson(s.net.requests.at(2).body).object().value(QStringLiteral("continuation")).toString(), t2);
    CHECK(m.count() > afterP2);
    CHECK(m.canLoadMore());                                     // p3 still carries a token
    // No row appears twice (page 3 repeats Shorts from page 1).
    QSet<QString> ids;
    for (int i = 0; i < m.count(); ++i) {
        const QVariantMap row = m.get(i);
        const QString key = row.value(QStringLiteral("kind")).toString().replace(QLatin1String("short"), QLatin1String("video")) +
                            QLatin1Char(':') + row.value(QStringLiteral("itemId")).toString();
        CHECK(!ids.contains(key));
        ids.insert(key);
    }

    // Videos-only mode pins type=video in params; a filter change re-runs the query.
    m.setMixed(false);
    CHECK(catalog_test::waitFor([&] { return !m.busy(); }));
    CHECK_EQ(s.net.requests.size(), 4);
    CHECK_EQ(QJsonDocument::fromJson(s.net.requests.at(3).body).object().value(QStringLiteral("params")).toString(), QStringLiteral("EgIQAQ=="));
    m.setFilters(static_cast<int>(SearchSort::UploadDate), static_cast<int>(UploadDateFilter::LastHour), 0, 0, true, true);
    CHECK(catalog_test::waitFor([&] { return !m.busy(); }));
    CHECK_EQ(s.net.requests.size(), 5);                         // one request for several filters
    SearchFilter sent;
    CHECK(decodeSearchParams(QJsonDocument::fromJson(s.net.requests.at(4).body).object().value(QStringLiteral("params")).toString().toStdString(), &sent));
    CHECK(sent.sort == SearchSort::UploadDate && sent.uploadDate == UploadDateFilter::LastHour && sent.live && sent.type == ResultTypeFilter::Any);
    CHECK(m.filtersActive());
    m.resetFilters();
    CHECK(!m.filtersActive());
}

// ---------------------------------------------------------------------------------------------
// DSC-09 acceptance: page 2 all Shorts (derived from search_en_p2) still reaches page 3.

static void allShortsPageReachesPageThree() {
    const QString t1 = tokenOfFirstPage(QStringLiteral("search_en_p1.json"));
    const QJsonObject p2 = allShortsPage2();
    const SearchResult parsed2 = SearchService::parseContinuation(p2, QStringLiteral("en"));
    CHECK(parsed2.outcome == Outcome::Ok);
    CHECK_EQ(countKind(parsed2.rows, ItemKind::Short), static_cast<int>(parsed2.rows.size()));   // every row is a Short
    CHECK(parsed2.rows.size() >= 15);
    const QString t2 = parsed2.continuation;
    CHECK(!t2.isEmpty());
    const QString t3 = tokenOfContinuation(QStringLiteral("search_en_p3.json"));
    for (int pass = 0; pass < 2; ++pass) {
        const bool hide = pass == 0;
        Stack s;
        s.net.on(QStringLiteral("youtubei/v1/search"), tokenNeedle(t1), 200, bytes(p2));
        s.net.on(QStringLiteral("youtubei/v1/search"), tokenNeedle(t2), 200, catalog_test::fixture(QStringLiteral("search_en_p3.json")));
        // After page 3 the list ends (valid empty continuation, no token).
        s.net.on(QStringLiteral("youtubei/v1/search"), tokenNeedle(t3), 200, bytes(continuationAnswer(QJsonArray(), QString())));
        s.net.on(QStringLiteral("youtubei/v1/search"), "\"query\"", 200, catalog_test::fixture(QStringLiteral("search_en_p1.json")));
        DiscoverySearchModel m(&s.service);
        m.setHideShorts(hide);
        m.search(QStringLiteral("kurzgesagt"));
        CHECK(catalog_test::waitFor([&] { return !m.busy(); }));
        const int afterP1 = m.count();
        const int hiddenP1 = m.hiddenShorts();
        CHECK(afterP1 > 0);
        if (hide) CHECK(hiddenP1 >= 5);
        m.loadMore();
        CHECK(catalog_test::waitFor([&] { return !m.busy(); }));
        if (hide) {
            // Page 2 added nothing visible: page 3 (and the closing page) were fetched automatically.
            CHECK_EQ(s.net.count(QStringLiteral("youtubei/v1/search"), tokenNeedle(t2)), 1);
            CHECK_EQ(s.net.count(QStringLiteral("youtubei/v1/search"), tokenNeedle(t3)), 1);
            CHECK_EQ(m.count(), afterP1);
            CHECK(m.hiddenShorts() >= hiddenP1 + 10);           // page 2's Shorts (some repeat page 1)
            CHECK_EQ(m.hiddenCount(), m.hiddenShorts());
            CHECK(!m.canLoadMore());                            // stopped only for lack of a token
            CHECK(m.errorText().isEmpty());
            // Turning the setting off shows the hidden rows again without a request.
            const int requests = s.net.requests.size();
            m.setHideShorts(false);
            CHECK(m.count() > afterP1);
            CHECK_EQ(m.hiddenShorts(), 0);
            CHECK_EQ(s.net.requests.size(), requests);
        } else {
            // Shorts visible: page 2 adds rows, so nothing is fetched until the user scrolls.
            CHECK_EQ(s.net.count(QStringLiteral("youtubei/v1/search"), tokenNeedle(t2)), 0);
            CHECK(m.count() > afterP1);
            CHECK(m.canLoadMore());
        }
    }
}

// ---------------------------------------------------------------------------------------------
// DSC-09: at most 5 automatic fetches in a row for pages that add nothing.

static void emptyPageCap() {
    CHECK(SearchPaging::kMaxEmptyPagesInARow == 5);
    SearchPaging p;
    CHECK(p.afterPage(3, false) == PagingStep::Stop);
    CHECK(p.afterPage(0, false) == PagingStep::Stop);           // no token: stop, even when empty
    CHECK(p.afterPage(3, true) == PagingStep::Wait);
    for (int i = 0; i < 5; ++i) CHECK(p.afterPage(0, true) == PagingStep::FetchNext);
    CHECK(p.afterPage(0, true) == PagingStep::Wait);            // cap reached
    CHECK(p.capped());
    p.userRequested();
    CHECK(p.afterPage(0, true) == PagingStep::FetchNext);
    CHECK(p.afterPage(1, true) == PagingStep::Wait);
    CHECK_EQ(p.emptyStreak(), 0);

    // Model: page 1, then continuation pages that only repeat a page-1 row (duplicates).
    const QJsonObject p1 = catalog_test::fixtureJson(QStringLiteral("search_en_p1.json"));
    const QString t1 = tokenOfFirstPage(QStringLiteral("search_en_p1.json"));
    QJsonObject dupRow;
    json::forEachKey(p1, QStringLiteral("videoRenderer"), [&dupRow](const QJsonObject &o) {
        dupRow.insert(QStringLiteral("videoRenderer"), o);
        return false;
    });
    QJsonArray dupRows; dupRows.append(dupRow);
    Stack s;
    s.net.on(QStringLiteral("youtubei/v1/search"), tokenNeedle(t1), 200, bytes(continuationAnswer(dupRows, QStringLiteral("dup-1"))));
    for (int i = 1; i <= 20; ++i)
        s.net.on(QStringLiteral("youtubei/v1/search"), tokenNeedle(QStringLiteral("dup-%1").arg(i)), 200,
                 bytes(continuationAnswer(dupRows, QStringLiteral("dup-%1").arg(i + 1))));
    s.net.on(QStringLiteral("youtubei/v1/search"), "\"query\"", 200, bytes(p1));
    DiscoverySearchModel m(&s.service);
    m.search(QStringLiteral("kurzgesagt"));
    CHECK(catalog_test::waitFor([&] { return !m.busy(); }));
    const int rows = m.count();
    m.loadMore();
    CHECK(catalog_test::waitFor([&] { return !m.busy(); }));
    // The user's page plus 5 automatic ones, then it waits with the token kept.
    CHECK_EQ(s.net.requests.size(), 1 + 1 + 5);
    CHECK_EQ(m.count(), rows);
    CHECK(m.canLoadMore());
    m.loadMore();
    CHECK(catalog_test::waitFor([&] { return !m.busy(); }));
    CHECK_EQ(s.net.requests.size(), 1 + 2 * 6);
}

// ---------------------------------------------------------------------------------------------
// DSC-10: cache TTLs with a fake clock (domain and through the model).

static void searchCache() {
    std::int64_t now = 1000000;
    CatalogClock clock = [&now] { return now; };
    SearchCache c(clock);
    SearchFilter f;
    const std::string k = SearchCache::key("kurzgesagt", f, "mixed");
    CHECK(k != SearchCache::key("kurzgesagt", f, "videos"));
    SearchFilter hour; hour.uploadDate = UploadDateFilter::LastHour;
    CHECK(k != SearchCache::key("kurzgesagt", hour, "mixed"));
    CHECK(k != SearchCache::key("Kurzgesagt", f, "mixed"));
    CachedSearchPage p1; p1.continuation = "t1"; Item it; it.id = "aaaaaaaaaaa"; p1.rows.push_back(it);
    c.store(k, 0, p1);
    CachedSearchPage p2; p2.continuation = "t2";
    c.store(k, 1, p2);
    c.store(k, 5, p2);                                          // a gap is not an extension
    CHECK_EQ(c.pageCount(k), static_cast<std::size_t>(2));
    now += 2 * kMin - 1;
    CHECK(c.page(k, 0) != nullptr);                             // < 2 min: page 1 reused
    now += 1;
    CHECK(c.page(k, 0) == nullptr);                             // 2..10 min: page 1 fetched again
    CHECK(c.page(k, 1) != nullptr);                             // deeper pages stay while the entry lives
    CHECK(c.contains(k));
    now += 8 * kMin - 1;
    CHECK(c.contains(k));
    now += 1;                                                   // 10 min: gone
    CHECK(!c.contains(k));
    CHECK(c.page(k, 1) == nullptr);
    // Refetching page 1 starts the chain again.
    c.store(k, 0, p1); c.store(k, 1, p2);
    c.store(k, 0, p1);
    CHECK_EQ(c.pageCount(k), static_cast<std::size_t>(1));
    // At most 8 queries; the least recently used one goes.
    SearchCache lru(clock);
    for (int i = 0; i < 8; ++i) lru.store("q" + std::to_string(i), 0, p1);
    CHECK_EQ(lru.size(), static_cast<std::size_t>(8));
    CHECK(lru.page("q0", 0) != nullptr);                        // touch q0: q1 is now the oldest
    lru.store("q8", 0, p1);
    CHECK_EQ(lru.size(), static_cast<std::size_t>(8));
    CHECK(lru.contains("q0"));
    CHECK(!lru.contains("q1"));
    CHECK(lru.contains("q8"));

    // Through the model.
    const QString t1 = tokenOfFirstPage(QStringLiteral("search_en_p1.json"));
    const QString t2 = tokenOfContinuation(QStringLiteral("search_en_p2.json"));
    Stack s;
    s.net.on(QStringLiteral("youtubei/v1/search"), tokenNeedle(t1), 200, catalog_test::fixture(QStringLiteral("search_en_p2.json")));
    s.net.on(QStringLiteral("youtubei/v1/search"), tokenNeedle(t2), 200, catalog_test::fixture(QStringLiteral("search_en_p3.json")));
    s.net.on(QStringLiteral("youtubei/v1/search"), "\"query\"", 200, catalog_test::fixture(QStringLiteral("search_en_p1.json")));
    DiscoverySearchModel m(&s.service, nullptr, clock);
    m.search(QStringLiteral("kurzgesagt"));
    CHECK(catalog_test::waitFor([&] { return !m.busy(); }));
    const int p1rows = m.count();
    m.loadMore();
    CHECK(catalog_test::waitFor([&] { return !m.busy(); }));
    const int p2rows = m.count();
    CHECK_EQ(s.net.requests.size(), 2);
    now += kMin;
    m.search(QStringLiteral("kurzgesagt"));                     // within 2 min: no request
    CHECK(!m.busy());
    CHECK_EQ(s.net.requests.size(), 2);
    CHECK_EQ(m.count(), p1rows);
    m.loadMore();                                               // cached page 2: no request
    CHECK_EQ(s.net.requests.size(), 2);
    CHECK_EQ(m.count(), p2rows);
    m.loadMore();                                               // only the new continuation is fetched
    CHECK(catalog_test::waitFor([&] { return !m.busy(); }));
    CHECK_EQ(s.net.requests.size(), 3);
    CHECK_EQ(s.net.count(QStringLiteral("youtubei/v1/search"), tokenNeedle(t2)), 1);
    now += 2 * kMin;                                            // 3 min: page 1 again
    m.search(QStringLiteral("kurzgesagt"));
    CHECK(catalog_test::waitFor([&] { return !m.busy(); }));
    CHECK_EQ(s.net.requests.size(), 4);
    CHECK_EQ(s.net.count(QStringLiteral("youtubei/v1/search"), "\"query\""), 2);
    // Another kind is another entry.
    m.setMixed(false);
    CHECK(catalog_test::waitFor([&] { return !m.busy(); }));
    CHECK_EQ(s.net.requests.size(), 5);
    m.setMixed(true);                                           // back within 2 min: cached
    CHECK_EQ(s.net.requests.size(), 5);
    // Failures are not cached.
    Stack e;
    e.net.on(QStringLiteral("youtubei/v1/search"), QByteArray(), 500, QByteArray());
    DiscoverySearchModel me(&e.service, nullptr, clock);
    me.search(QStringLiteral("x"));
    CHECK(catalog_test::waitFor([&] { return !me.busy(); }));
    me.search(QStringLiteral("x"));
    CHECK(catalog_test::waitFor([&] { return !me.busy(); }));
    CHECK_EQ(e.net.requests.size(), 2);
}

// ---------------------------------------------------------------------------------------------
// DSC-18 in search: members-only rows are badged, or hidden and counted.

static void membersOnlyCounted() {
    // Derived: the first two videoRenderer rows of page 1 get a members-only badge
    // (metadataBadgeRenderer, the older badge generation of search rows).
    int left = 2;
    const QByteArray raw = bytes(withMembersBadges(catalog_test::fixtureJson(QStringLiteral("search_en_p1.json")), left).toObject());
    CHECK_EQ(left, 0);
    const SearchResult parsed = SearchService::parseFirstPage(QJsonDocument::fromJson(raw).object(), QStringLiteral("en"));
    int members = 0;
    for (const Item &i : parsed.rows) if (i.membersOnly) ++members;
    CHECK_EQ(members, 2);
    Stack s;
    s.net.on(QStringLiteral("youtubei/v1/search"), QByteArray(), 200, raw);
    DiscoverySearchModel m(&s.service);
    m.search(QStringLiteral("kurzgesagt"));
    CHECK(catalog_test::waitFor([&] { return !m.busy(); }));
    const int all = m.count();
    int badged = 0;
    for (int i = 0; i < m.count(); ++i) if (m.get(i).value(QStringLiteral("membersOnly")).toBool()) ++badged;
    CHECK_EQ(badged, 2);
    m.setHideMembersOnly(true);
    CHECK_EQ(m.count(), all - 2);
    CHECK_EQ(m.hiddenMembersOnly(), 2);
    CHECK_EQ(m.hiddenCount(), 2);
}

// ---------------------------------------------------------------------------------------------
// DSC-11: suggestions.

static void suggestParsing() {
    for (const char *f : {"suggest_en.json", "suggest_de.json", "suggest_ja.json"}) {
        const SuggestResult r = SuggestService::parse(catalog_test::fixture(QString::fromLatin1(f)));
        CHECK(r.outcome == Outcome::Ok);
        CHECK_EQ(r.suggestions.size(), 10);
        CHECK_EQ(r.query, QStringLiteral("kurzg"));
        for (const QString &s : r.suggestions) CHECK(!s.isEmpty());
    }
    CHECK(SuggestService::parse(catalog_test::fixture(QStringLiteral("suggest_ja.json"))).suggestions.join(QString()).contains(QStringLiteral("日本語")));
    // More than 10 entries are cut to 10.
    QJsonArray many;
    for (int i = 0; i < 15; ++i) many.append(QStringLiteral("entry %1").arg(i));
    QJsonArray top; top.append(QStringLiteral("e")); top.append(many);
    const SuggestResult cut = SuggestService::parse(QJsonDocument(top).toJson());
    CHECK_EQ(cut.suggestions.size(), 10);
    CHECK_EQ(cut.suggestions.first(), QStringLiteral("entry 0"));
    // Latin-1 answers (legacy charset) still parse.
    const QByteArray latin1 = QByteArray("[\"m\",[\"m") + char(0xfc) + QByteArray("nchen\"]]");
    const SuggestResult l1 = SuggestService::parse(latin1);
    CHECK(l1.outcome == Outcome::Ok);
    CHECK_EQ(l1.suggestions.value(0), QString::fromUtf8("m\xc3\xbcnchen"));
    CHECK(SuggestService::parse("<html>").outcome == Outcome::ShapeDrift);
    CHECK(SuggestService::parse("[\"q\"]").outcome == Outcome::ShapeDrift);
    CHECK(SuggestService::parse("[\"q\",[]]").outcome == Outcome::ValidEmpty);
    // The request: GET, client=firefox, ds=yt, hl from the device locale, 5 s.
    const HttpRequest req = SuggestService::buildRequest(QStringLiteral("a+b & c"), QStringLiteral("de"));
    CHECK_EQ(req.url.host(), QStringLiteral("suggestqueries.google.com"));
    CHECK_EQ(req.url.path(), QStringLiteral("/complete/search"));
    CHECK_EQ(req.method, QByteArray("GET"));
    CHECK_EQ(req.timeoutMs, 5000);
    const QUrlQuery query(req.url);
    CHECK_EQ(query.queryItemValue(QStringLiteral("client")), QStringLiteral("firefox"));
    CHECK_EQ(query.queryItemValue(QStringLiteral("ds")), QStringLiteral("yt"));
    CHECK_EQ(query.queryItemValue(QStringLiteral("hl")), QStringLiteral("de"));
    CHECK_EQ(query.queryItemValue(QStringLiteral("q"), QUrl::FullyDecoded), QStringLiteral("a+b & c"));
    CHECK(DiscoveryLocale::fromLocale(QLocale(QStringLiteral("de_DE"))).hl == QStringLiteral("de"));

    // SuggestCache: 10 min, 128 entries.
    std::int64_t now = 0;
    CatalogClock clock = [&now] { return now; };
    SuggestCache c(clock);
    c.store("a", std::vector<std::string>{"a1"});
    std::vector<std::string> out;
    now = 10 * kMin - 1;
    CHECK(c.lookup("a", &out) && out.size() == 1);
    now = 10 * kMin;
    CHECK(!c.lookup("a", &out));
    for (int i = 0; i < 129; ++i) c.store("q" + std::to_string(i), std::vector<std::string>());
    CHECK_EQ(c.size(), static_cast<std::size_t>(128));
    CHECK(!c.lookup("q0", nullptr));                            // the 129th query evicted one entry
    CHECK(c.lookup("q128", nullptr));
}

static void suggestService() {
    std::int64_t now = 0;
    CatalogClock clock = [&now] { return now; };
    FakeTransport net;
    DiscoveryLocale de; de.hl = QStringLiteral("de"); de.gl = QStringLiteral("DE");
    CatalogCounters counters;
    SuggestService svc(&net, de, &counters, nullptr, clock);
    net.on(QStringLiteral("q=kurzg"), QByteArray(), 200, catalog_test::fixture(QStringLiteral("suggest_de.json")));
    bool got = false; SuggestResult res;
    svc.fetch(QStringLiteral("kurzg"), [&](const SuggestResult &r) { res = r; got = true; });
    CHECK(catalog_test::waitFor([&] { return got; }));
    CHECK_EQ(net.requests.size(), 1);
    CHECK_EQ(QUrlQuery(net.requests.at(0).url).queryItemValue(QStringLiteral("hl")), QStringLiteral("de"));
    CHECK_EQ(res.suggestions.size(), 10);
    CHECK(!res.fromCache);
    // A repeat within 10 min sends nothing; after 10 min it asks again.
    got = false;
    now += 9 * kMin;
    svc.fetch(QStringLiteral("kurzg"), [&](const SuggestResult &r) { res = r; got = true; });
    CHECK(catalog_test::waitFor([&] { return got; }));
    CHECK_EQ(net.requests.size(), 1);
    CHECK(res.fromCache && res.suggestions.size() == 10);
    got = false;
    now += 2 * kMin;
    svc.fetch(QStringLiteral("kurzg"), [&](const SuggestResult &r) { res = r; got = true; });
    CHECK(catalog_test::waitFor([&] { return got; }));
    CHECK_EQ(net.requests.size(), 2);
    // A silent/failed server (timeout, no route) yields no suggestions, and is not cached.
    got = false;
    svc.fetch(QStringLiteral("nothing"), [&](const SuggestResult &r) { res = r; got = true; });
    CHECK(catalog_test::waitFor([&] { return got; }));
    CHECK(res.outcome == Outcome::Network);
    CHECK(res.suggestions.isEmpty());
    got = false;
    svc.fetch(QStringLiteral("nothing"), [&](const SuggestResult &r) { res = r; got = true; });
    CHECK(catalog_test::waitFor([&] { return got; }));
    CHECK_EQ(net.count(QStringLiteral("q=nothing")), 2);
    CHECK_EQ(counters.value(Area::Suggest, Outcome::Network), static_cast<std::int64_t>(2));
    // Cancel: the callback never runs.
    bool cancelled = true;
    const quint64 id = svc.fetch(QStringLiteral("kurzg2"), [&](const SuggestResult &) { cancelled = false; });
    svc.cancel(id);
    QElapsedTimer t; t.start();
    catalog_test::waitFor([&] { return t.elapsed() > 50; }, 200);
    CHECK(cancelled);
}

static void suggestionModel() {
    FakeTransport net;
    SuggestService svc(&net, DiscoveryLocale(), nullptr);
    net.on(QStringLiteral("q=kurzg&"), QByteArray(), 200, catalog_test::fixture(QStringLiteral("suggest_en.json")));
    net.on(QStringLiteral("q=kurzg"), QByteArray(), 200, catalog_test::fixture(QStringLiteral("suggest_en.json")));
    // The older query answers late (400 ms); the newer one at once.
    net.on(QStringLiteral("q=slow"), QByteArray(), 200, "[\"slow\",[\"slow answer\"]]", 400);
    net.on(QStringLiteral("q=fast"), QByteArray(), 200, "[\"fast\",[\"fast answer\"]]");
    net.on(QStringLiteral("q=late"), QByteArray(), 200, "[\"late\",[\"late answer\"]]", 300);
    const QString iniPath = QDir(QString::fromLocal8Bit(qgetenv("SAILTUBE_CATALOG_FIXTURES"))).absoluteFilePath(QStringLiteral("../settings.ini"));
    QFile::remove(iniPath);
    std::unique_ptr<QSettings> settings(new QSettings(iniPath, QSettings::IniFormat));
    timingLines().clear();
    {
        SuggestionModel m(&svc, settings.get());
        CHECK_EQ(m.count(), 0);                                 // no history yet
        // Five keystrokes within 300 ms send one request, for the last text.
        for (const char *t : {"k", "ku", "kur", "kurz", "kurzg"}) m.setQuery(QString::fromLatin1(t));
        QElapsedTimer t; t.start();
        CHECK(catalog_test::waitFor([&] { return m.count() > 0; }, 2000));
        CHECK(t.elapsed() >= SuggestionModel::kDebounceMs - 20);
        CHECK_EQ(net.requests.size(), 1);
        CHECK_EQ(QUrlQuery(net.requests.at(0).url).queryItemValue(QStringLiteral("q")), QStringLiteral("kurzg"));
        CHECK_EQ(QUrlQuery(net.requests.at(0).url).queryItemValue(QStringLiteral("hl")), QStringLiteral("en"));
        CHECK_EQ(m.count(), 10);
        CHECK_EQ(m.get(0), QStringLiteral("kurz gesagt"));
        CHECK(!m.data(m.index(0), SuggestionModel::HistoryRole).toBool());
        CHECK(timingLines().size() == 1 && timingLines().first().startsWith(QLatin1String("sailtube-timing suggest ")));

        // Stale reply: "slow" is asked first, "fast" second; the late "slow" answer is dropped.
        m.setQuery(QStringLiteral("slow"));
        CHECK(catalog_test::waitFor([&] { return net.count(QStringLiteral("q=slow")) == 1; }, 2000));
        m.setQuery(QStringLiteral("fast"));
        CHECK(catalog_test::waitFor([&] { return m.get(0) == QLatin1String("fast answer"); }, 2000));
        QElapsedTimer w; w.start();
        catalog_test::waitFor([&] { return w.elapsed() > 500; }, 1000);
        CHECK_EQ(m.get(0), QStringLiteral("fast answer"));
        CHECK_EQ(m.count(), 1);
        // Its answer still filled the cache: asking for "slow" again sends nothing.
        m.setQuery(QStringLiteral("slow"));
        CHECK(catalog_test::waitFor([&] { return m.get(0) == QLatin1String("slow answer"); }, 2000));
        CHECK_EQ(net.count(QStringLiteral("q=slow")), 1);

        // Submit suppresses in-flight replies and records the history.
        m.setQuery(QStringLiteral("late"));
        CHECK(catalog_test::waitFor([&] { return net.count(QStringLiteral("q=late")) == 1; }, 2000));
        m.submit(QStringLiteral("late"));
        CHECK_EQ(m.count(), 0);
        w.restart();
        catalog_test::waitFor([&] { return w.elapsed() > 450; }, 1000);
        CHECK_EQ(m.count(), 0);
        CHECK_EQ(m.historyCount(), 1);

        // Failure: no suggestions, no error.
        m.setQuery(QStringLiteral("unknown"));
        CHECK(catalog_test::waitFor([&] { return net.count(QStringLiteral("q=unknown")) == 1; }, 2000));
        w.restart();
        catalog_test::waitFor([&] { return w.elapsed() > 100; }, 500);
        CHECK_EQ(m.count(), 0);

        // An empty query lists the history, most recent first, without duplicates.
        m.submit(QStringLiteral("kurzgesagt"));
        m.submit(QStringLiteral("LATE"));
        m.setQuery(QString());
        CHECK_EQ(m.count(), 2);
        CHECK_EQ(m.get(0), QStringLiteral("LATE"));
        CHECK_EQ(m.get(1), QStringLiteral("kurzgesagt"));
        CHECK(m.data(m.index(0), SuggestionModel::HistoryRole).toBool());
        // Paused: nothing is recorded.
        m.setHistoryPaused(true);
        m.submit(QStringLiteral("private"));
        CHECK_EQ(m.historyCount(), 2);
    }
    settings->sync();
    {
        SuggestionModel again(&svc, settings.get());
        CHECK_EQ(again.historyCount(), 2);                      // persisted under catalog/search/
        CHECK(again.historyPaused());
        CHECK_EQ(settings->value(QStringLiteral("catalog/search/history")).toStringList().value(0), QStringLiteral("LATE"));
        again.removeHistory(QStringLiteral("LATE"));
        CHECK_EQ(again.historyCount(), 1);
        again.clearHistory();
        CHECK_EQ(again.historyCount(), 0);
        CHECK_EQ(again.count(), 0);
        CHECK(settings->value(QStringLiteral("catalog/search/history")).toStringList().isEmpty());
    }
    QFile::remove(iniPath);
}

// ---------------------------------------------------------------------------------------------
// NFR-PERF-09 (host side): parse + model fill of search_en_p1 well under 1 s, timing line logged.

static void hostTiming() {
    Stack s;
    s.net.on(QStringLiteral("youtubei/v1/search"), QByteArray(), 200, catalog_test::fixture(QStringLiteral("search_en_p1.json")));
    DiscoverySearchModel m(&s.service);
    timingLines().clear();
    stageLines().clear();
    QElapsedTimer t; t.start();
    m.search(QStringLiteral("kurzgesagt"));
    CHECK(catalog_test::waitFor([&] { return !m.busy(); }));
    const qint64 ms = t.elapsed();
    std::printf("search page 1 parse + fill: %lld ms (%d rows)\n", static_cast<long long>(ms), m.count());
    CHECK(m.count() >= 20);
    CHECK(ms < 500);
    CHECK_EQ(timingLines().size(), 1);
    CHECK(timingLines().value(0).startsWith(QLatin1String("sailtube-timing search page1 ")));
    // One stage line for the request: endpoint, network, parse, delivery, size, status.
    bool stage = false;
    for (const QString &l : stageLines())
        stage = stage || (l.startsWith(QLatin1String("sailtube-timing innertube search net=")) && l.contains(QLatin1String(" parse=")) &&
                          l.contains(QLatin1String(" ready=")) && l.contains(QLatin1String(" status=200")));
    CHECK(stage);
    // The parse alone, on this thread.
    t.restart();
    const QJsonObject doc = catalog_test::fixtureJson(QStringLiteral("search_en_p1.json"));
    const SearchResult r = SearchService::parseFirstPage(doc, QStringLiteral("en"));
    const qint64 parseMs = t.elapsed();
    std::printf("search page 1 JSON + parse: %lld ms\n", static_cast<long long>(parseMs));
    CHECK(r.outcome == Outcome::Ok);
    CHECK(parseMs < 300);
}

// Cancel and destruction: no callback after cancel, the model is idle.
static void cancellation() {
    Stack s;
    s.net.on(QStringLiteral("youtubei/v1/search"), QByteArray(), 200, catalog_test::fixture(QStringLiteral("search_en_p1.json")), 150);
    DiscoverySearchModel m(&s.service);
    m.search(QStringLiteral("kurzgesagt"));
    CHECK(m.busy());
    m.cancel();
    CHECK(!m.busy());
    QElapsedTimer t; t.start();
    catalog_test::waitFor([&] { return t.elapsed() > 300; }, 600);
    CHECK_EQ(m.count(), 0);
    CHECK_EQ(s.service.pending(), 0);
    m.search(QString());
    CHECK(!m.busy());
    CHECK_EQ(s.net.requests.size(), 1);                         // an empty query sends nothing
}

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    previousHandler = qInstallMessageHandler(captureMessages);
    encoderCanonical();
    filterFixtures();
    validEmptyVsDrift();
    fallbackTier();
    pagingThreePages();
    allShortsPageReachesPageThree();
    emptyPageCap();
    searchCache();
    membersOnlyCounted();
    suggestParsing();
    suggestService();
    suggestionModel();
    hostTiming();
    cancellation();
    nextPagePrefetch();
    lastSearchSnapshot();
    tapIntentSignals();
    channelAvatars();
    return catalog_test::finish("catalog_search");
}
