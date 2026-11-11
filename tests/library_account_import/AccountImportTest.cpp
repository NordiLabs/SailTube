// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// IMP-02 account import with synthetic browse pages in the documented shape
// (channelRenderer, gridChannelRenderer, gridPlaylistRenderer,
// playlistRenderer, lockupViewModel, continuationItemRenderer): every UC id
// and playlist id is imported, no UC row becomes a playlist, a feed stops
// after 20 pages of a 25-page chain and at the first page that adds nothing,
// and every request carries the selected X-Goog-AuthUser and page id. The
// batch then goes through ImportController -> ImportJob -> LibraryStore.
#include "Clients/Sailfish/Presentation/ImportController.h"
#include "Core/Library/Infrastructure/AccountImport.h"
#include "Core/Library/Infrastructure/LibraryStore.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QJsonArray>
#include <QTimer>
#include <cstdio>

using namespace nordtile::video;
using namespace nordtile::video::library;

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

static QString channelId(int n) { return QStringLiteral("UC") + QString::number(n).rightJustified(22, QLatin1Char('a')); }
static QString listId(int n) { return QStringLiteral("PL") + QString::number(n).rightJustified(32, QLatin1Char('b')); }

static QJsonObject runs(const QString &t) {
    QJsonObject run; run.insert(QStringLiteral("text"), t);
    QJsonObject o; o.insert(QStringLiteral("runs"), QJsonArray() << run);
    return o;
}
static QJsonObject simple(const QString &t) { QJsonObject o; o.insert(QStringLiteral("simpleText"), t); return o; }
static QJsonObject thumbs(const QString &url) {
    QJsonObject small; small.insert(QStringLiteral("url"), QStringLiteral("//yt3.example/small.jpg"));
    QJsonObject big; big.insert(QStringLiteral("url"), url);
    QJsonObject o; o.insert(QStringLiteral("thumbnails"), QJsonArray() << small << big);
    return o;
}
static QJsonObject wrap(const char *key, const QJsonObject &r) { QJsonObject o; o.insert(QLatin1String(key), r); return o; }

static QJsonObject channelRow(int n, bool grid) {
    QJsonObject r;
    r.insert(QStringLiteral("channelId"), channelId(n));
    r.insert(QStringLiteral("title"), grid ? runs(QStringLiteral("Channel %1").arg(n)) : simple(QStringLiteral("Channel %1").arg(n)));
    r.insert(QStringLiteral("thumbnail"), thumbs(QStringLiteral("//yt3.example/c%1.jpg").arg(n)));
    return wrap(grid ? "gridChannelRenderer" : "channelRenderer", r);
}

static QJsonObject continuationRow(const QString &token) {
    QJsonObject cmd; cmd.insert(QStringLiteral("token"), token);
    QJsonObject ep; ep.insert(QStringLiteral("continuationCommand"), cmd);
    QJsonObject r; r.insert(QStringLiteral("continuationEndpoint"), ep);
    return wrap("continuationItemRenderer", r);
}

// A first browse page: tabs > sectionList > itemSection > shelf > grid items.
static QJsonObject browsePage(const QJsonArray &items) {
    QJsonObject grid; grid.insert(QStringLiteral("items"), items);
    QJsonObject shelfContent; shelfContent.insert(QStringLiteral("gridRenderer"), grid);
    QJsonObject shelf; shelf.insert(QStringLiteral("content"), shelfContent);
    QJsonObject section; section.insert(QStringLiteral("contents"), QJsonArray() << wrap("shelfRenderer", shelf));
    QJsonObject list; list.insert(QStringLiteral("contents"), QJsonArray() << wrap("itemSectionRenderer", section));
    QJsonObject content; content.insert(QStringLiteral("sectionListRenderer"), list);
    QJsonObject tab; tab.insert(QStringLiteral("content"), content);
    QJsonObject tabs; tabs.insert(QStringLiteral("tabs"), QJsonArray() << wrap("tabRenderer", tab));
    QJsonObject contents; contents.insert(QStringLiteral("twoColumnBrowseResultsRenderer"), tabs);
    QJsonObject page; page.insert(QStringLiteral("contents"), contents);
    return page;
}

// A continuation answer: onResponseReceivedActions > appendContinuationItemsAction.
static QJsonObject continuationPage(const QJsonArray &items) {
    QJsonObject append; append.insert(QStringLiteral("continuationItems"), items);
    QJsonObject action; action.insert(QStringLiteral("appendContinuationItemsAction"), append);
    QJsonObject page; page.insert(QStringLiteral("onResponseReceivedActions"), QJsonArray() << action);
    return page;
}

struct Request { QJsonObject body; QMap<QByteArray, QByteArray> headers; };

// Serves FEchannels and FEplaylist_aggregation as continuation chains.
class FakeAccount final : public AccountBrowseTransport {
public:
    void browse(const QJsonObject &body, const QMap<QByteArray, QByteArray> &headers, Done done) override {
        Request r; r.body = body; r.headers = headers;
        requests.push_back(r);
        if (failAt == static_cast<int>(requests.size())) { answer(done, false, QJsonObject()); return; }
        QString feed = body.value(QStringLiteral("browseId")).toString();
        int page = 0;
        const QString token = body.value(QStringLiteral("continuation")).toString();
        if (!token.isEmpty()) {
            const QStringList parts = token.split(QLatin1Char(':'));
            feed = parts.value(0);
            page = parts.value(1).toInt();
        }
        answer(done, true, feed == QLatin1String("FEchannels") ? channelsPage(page) : playlistsPage(page));
    }
    void answer(Done done, bool ok, const QJsonObject &reply) {
        if (async) QTimer::singleShot(1, [done, ok, reply]() { done(ok, reply); });
        else done(ok, reply);
    }
    QJsonObject channelsPage(int page) const {
        QJsonArray items;
        for (int i = 0; i < channelsPerPage; ++i) {
            const int n = page * channelsPerPage + i;
            items << channelRow(n, n % 2 == 1);
        }
        if (page == 0) { QJsonObject bad; bad.insert(QStringLiteral("channelId"), QStringLiteral("notachannel")); items << wrap("channelRenderer", bad); }
        if (page + 1 < channelPages) items << continuationRow(QStringLiteral("FEchannels:%1").arg(page + 1));
        return page == 0 ? browsePage(items) : continuationPage(items);
    }
    QJsonObject playlistsPage(int page) const {
        QJsonArray items;
        if (page == 0 || repeatPlaylists) {
            QJsonObject g;
            g.insert(QStringLiteral("playlistId"), listId(1));
            g.insert(QStringLiteral("title"), runs(QStringLiteral("Grid list")));
            g.insert(QStringLiteral("videoCountShortText"), simple(QStringLiteral("12")));
            g.insert(QStringLiteral("thumbnail"), thumbs(QStringLiteral("https://i.ytimg.com/p1.jpg")));
            g.insert(QStringLiteral("shortBylineText"), runs(QStringLiteral("Owner")));
            items << wrap("gridPlaylistRenderer", g);
            QJsonObject p;
            p.insert(QStringLiteral("playlistId"), listId(2));
            p.insert(QStringLiteral("title"), simple(QStringLiteral("Plain list")));
            p.insert(QStringLiteral("videoCount"), QStringLiteral("1,234"));
            p.insert(QStringLiteral("thumbnails"), QJsonArray() << thumbs(QStringLiteral("https://i.ytimg.com/p2.jpg")));
            items << wrap("playlistRenderer", p);
            QJsonObject title; title.insert(QStringLiteral("content"), QStringLiteral("Lockup list"));
            QJsonObject lmeta; lmeta.insert(QStringLiteral("title"), title);
            QJsonObject meta; meta.insert(QStringLiteral("lockupMetadataViewModel"), lmeta);
            QJsonObject l;
            l.insert(QStringLiteral("contentId"), QStringLiteral("OLAK5uy_lockupplaylistid000000000000000"));
            l.insert(QStringLiteral("contentType"), QStringLiteral("LOCKUP_CONTENT_TYPE_PLAYLIST"));
            l.insert(QStringLiteral("metadata"), meta);
            items << wrap("lockupViewModel", l);
            // UC rows in the aggregation page are channels, never playlists
            QJsonObject uc = l; uc.insert(QStringLiteral("contentId"), channelId(900));
            items << wrap("lockupViewModel", uc);
            QJsonObject ucList; ucList.insert(QStringLiteral("playlistId"), channelId(901));
            ucList.insert(QStringLiteral("title"), simple(QStringLiteral("Uploads")));
            items << wrap("gridPlaylistRenderer", ucList);
            QJsonObject video = l; video.insert(QStringLiteral("contentType"), QStringLiteral("LOCKUP_CONTENT_TYPE_VIDEO"));
            video.insert(QStringLiteral("contentId"), QStringLiteral("dQw4w9WgXcQ"));
            items << wrap("lockupViewModel", video);
        }
        if (page < 5) items << continuationRow(QStringLiteral("FEplaylist_aggregation:%1").arg(page + 1));
        return page == 0 ? browsePage(items) : continuationPage(items);
    }
    std::vector<Request> requests;
    int channelPages = 25;
    int channelsPerPage = 2;
    bool repeatPlaylists = true;
    int failAt = 0;
    bool async = false;
};

static void spinUntil(const std::function<bool()> &done, int ms) {
    QElapsedTimer t; t.start();
    while (!done() && t.elapsed() < ms) {
        QEventLoop loop;
        QTimer::singleShot(5, &loop, SLOT(quit()));
        loop.exec();
    }
}

static void parserTests() {
    FakeAccount fake;
    const accountImport::Page first = accountImport::parseChannelsPage(fake.channelsPage(0));
    CHECK(first.channels.size() == 2);
    CHECK(first.skippedRows == 1); // "notachannel"
    CHECK(first.continuation == QStringLiteral("FEchannels:1"));
    if (first.channels.size() == 2) {
        CHECK(first.channels[0].channelId == channelId(0).toStdString());
        CHECK(first.channels[0].name == "Channel 0");
        CHECK(first.channels[1].name == "Channel 1"); // runs title of the grid renderer
        CHECK(first.channels[0].avatarUrl == "https://yt3.example/c0.jpg");
    }
    const accountImport::Page last = accountImport::parseChannelsPage(fake.channelsPage(24));
    CHECK(last.continuation.isEmpty());

    const accountImport::Page lists = accountImport::parsePlaylistsPage(fake.playlistsPage(0));
    CHECK(lists.playlists.size() == 3);
    CHECK(lists.skippedRows == 2); // the two UC rows; the video lockup is not a list row
    for (const RemotePlaylistRef &p : lists.playlists) CHECK(p.remoteId.compare(0, 2, "UC") != 0);
    if (lists.playlists.size() == 3) {
        CHECK(lists.playlists[0].remoteId == listId(1).toStdString());
        CHECK(lists.playlists[0].title == "Grid list" && lists.playlists[0].channel == "Owner");
        CHECK(lists.playlists[0].itemCountHint == 12);
        CHECK(lists.playlists[0].thumbnailUrl == "https://i.ytimg.com/p1.jpg");
        CHECK(lists.playlists[1].itemCountHint == 1234);
        CHECK(lists.playlists[2].remoteId == "OLAK5uy_lockupplaylistid000000000000000");
        CHECK(lists.playlists[2].title == "Lockup list");
    }

    AccountIdentity main;
    main.authUser = 1;
    const QMap<QByteArray, QByteArray> h = accountImport::headersFor(main);
    CHECK(h.value("X-Goog-AuthUser") == "1" && !h.contains("X-Goog-PageId"));
}

static void pagerTests() {
    AccountIdentity brand;
    brand.authUser = 2;
    brand.pageId = QStringLiteral("112233445566778899");
    {
        FakeAccount fake;
        AccountImporter importer(&fake);
        bool finished = false;
        ImportBatch batch;
        QString error;
        QObject::connect(&importer, &AccountImporter::finished, [&](const ImportBatch &b, const QString &e) {
            finished = true; batch = b; error = e;
        });
        CHECK(importer.start(brand));
        CHECK(finished && error.isEmpty());
        CHECK(batch.source == "account");
        CHECK(batch.subscriptions.size() == 40);   // 20 of 25 pages, 2 channels each
        CHECK(batch.remotePlaylists.size() == 3);  // page 2 repeats page 1: the feed stops there
        CHECK(fake.requests.size() == 22);         // 20 channel pages + 2 playlist pages
        int channelRequests = 0;
        for (const Request &r : fake.requests) {
            CHECK(r.headers.value("X-Goog-AuthUser") == "2");
            CHECK(r.headers.value("X-Goog-PageId") == "112233445566778899");
            if (r.body.value(QStringLiteral("browseId")).toString() == QLatin1String("FEchannels")
                || r.body.value(QStringLiteral("continuation")).toString().startsWith(QLatin1String("FEchannels:")))
                ++channelRequests;
        }
        CHECK(channelRequests == 20);
        CHECK(fake.requests[0].body.value(QStringLiteral("browseId")).toString() == QStringLiteral("FEchannels"));
        CHECK(fake.requests[1].body.value(QStringLiteral("continuation")).toString() == QStringLiteral("FEchannels:1"));
        CHECK(fake.requests[20].body.value(QStringLiteral("browseId")).toString() == QStringLiteral("FEplaylist_aggregation"));
    }
    {
        // a short chain ends with its last page; a page that adds nothing ends the feed
        FakeAccount fake;
        fake.channelPages = 3;
        fake.repeatPlaylists = false;
        AccountImporter importer(&fake);
        ImportBatch batch;
        QObject::connect(&importer, &AccountImporter::finished, [&](const ImportBatch &b, const QString &) { batch = b; });
        CHECK(importer.start(AccountIdentity()));
        CHECK(batch.subscriptions.size() == 6);
        CHECK(fake.requests.size() == 3 + 2); // playlists: page 1 adds 3, page 2 adds nothing
    }
    {
        // a failed page reports an error
        FakeAccount fake;
        fake.failAt = 4;
        AccountImporter importer(&fake);
        QString error;
        bool finished = false;
        QObject::connect(&importer, &AccountImporter::finished, [&](const ImportBatch &, const QString &e) { finished = true; error = e; });
        CHECK(importer.start(brand));
        CHECK(finished && error == QStringLiteral("network"));
        CHECK(fake.requests.size() == 4);
        CHECK(!importer.isRunning());
    }
    {
        // cancelled: a late answer is ignored and nothing is reported
        FakeAccount fake;
        fake.async = true;
        AccountImporter importer(&fake);
        bool finished = false;
        QObject::connect(&importer, &AccountImporter::finished, [&](const ImportBatch &, const QString &) { finished = true; });
        CHECK(importer.start(brand));
        CHECK(importer.isRunning());
        CHECK(!importer.start(brand)); // one run at a time
        importer.cancel();
        spinUntil([]() { return false; }, 50);
        CHECK(!finished && fake.requests.size() == 1);
    }
}

static void controllerTests(const QString &dir) {
    QDir().mkpath(dir);
    LibraryStore store(dir + QStringLiteral("/library.db"), QStringLiteral("account-import"));
    CHECK(store.open());
    sailfish::ImportController controller(&store, dir, QString());
    CHECK(!controller.accountImportAvailable());
    CHECK(!controller.startAccountImport()); // signed out
    FakeAccount fake;
    fake.async = true;
    controller.setAccountTransport(&fake);
    controller.setAccountIdentity(3, QString());
    CHECK(controller.accountImportAvailable());
    int dones = 0;
    bool lastOk = false;
    QObject::connect(&controller, &sailfish::ImportController::done, [&](bool ok) { ++dones; lastOk = ok; });
    CHECK(controller.startAccountImport());
    CHECK(controller.busy());
    spinUntil([&]() { return dones > 0; }, 10000);
    CHECK(dones == 1 && lastOk);
    CHECK(store.subscriptions(SubscriptionSort::Name).size() == 40);
    CHECK(!store.mirrorIdForRemote(listId(1)).isEmpty());
    CHECK(!store.mirrorIdForRemote(QStringLiteral("OLAK5uy_lockupplaylistid000000000000000")).isEmpty());
    CHECK(store.mirrorIdForRemote(channelId(900)).isEmpty());
    CHECK(store.mirrorIdForRemote(channelId(901)).isEmpty());
    CHECK(fake.requests.front().headers.value("X-Goog-AuthUser") == "3");
    CHECK(controller.summaryText().contains(QStringLiteral("40")));

    // a second import adds nothing
    CHECK(controller.startAccountImport());
    spinUntil([&]() { return dones > 1; }, 10000);
    CHECK(dones == 2 && lastOk);
    CHECK(controller.summary().value(QStringLiteral("nothingNew")).toBool());

    // a failure applies nothing and says so
    fake.failAt = static_cast<int>(fake.requests.size()) + 2;
    CHECK(controller.startAccountImport());
    spinUntil([&]() { return dones > 2; }, 10000);
    CHECK(dones == 3 && !lastOk && !controller.errorText().isEmpty());

    controller.setAccountTransport(nullptr); // signed out again
    CHECK(!controller.accountImportAvailable());
}

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    const QString dir = QString::fromLocal8Bit(qgetenv("TEST_DIR"));
    parserTests();
    pagerTests();
    controllerTests(dir + QStringLiteral("/controller"));
    std::printf("library account import tests: %s (%d failures)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
