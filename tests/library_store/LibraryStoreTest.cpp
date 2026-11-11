// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// QtSql checks of the library store (LIB-01..07, IMP-01 merge, NFR-ROB-02, RES-02).
// Build: tests/library_store/run.sh
#include "Core/Library/Infrastructure/LibrarySchema.h"
#include "Core/Library/Infrastructure/LibraryStore.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QMetaMethod>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QThread>

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <memory>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

using namespace nordtile::video::library;

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

static const qint64 S = 1000;
static const qint64 Day = 24LL * 3600 * 1000;
static qint64 g_now = 1700000000000LL;
static qint64 fakeNow() { return g_now; }

static QString g_root;
static int g_connection = 0;

static QString freshDir(const char *name) {
    const QString d = g_root + QLatin1Char('/') + QLatin1String(name);
    QDir(d).removeRecursively();
    return d;
}
static QString dbIn(const QString &dir) { return dir + QStringLiteral("/library.db"); }
static QString Q(const std::string &s) { return QString::fromUtf8(s.c_str()); }
static std::string vid(int i) { return QStringLiteral("v%1").arg(i, 10, 10, QLatin1Char('0')).toStdString(); }
static std::string uc(int i) { return QStringLiteral("UC%1").arg(i, 22, 10, QLatin1Char('0')).toStdString(); }

static std::unique_ptr<LibraryStore> openStore(const QString &path, QString *error = nullptr) {
    std::unique_ptr<LibraryStore> s(new LibraryStore(path, QStringLiteral("store-%1").arg(++g_connection)));
    QString e;
    const bool ok = s->open(&e);
    if (error) *error = e;
    if (!ok && !error) std::printf("open failed: %s\n", qPrintable(e));
    return s;
}

// A raw connection for assertions behind the store's back.
struct Raw {
    QString name;
    explicit Raw(const QString &path) : name(QStringLiteral("raw-%1").arg(++g_connection)) {
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), name);
        db.setDatabaseName(path);
        if (!db.open()) std::printf("raw open failed: %s\n", qPrintable(db.lastError().text()));
    }
    ~Raw() {
        {
            QSqlDatabase db = QSqlDatabase::database(name, false);
            db.close();
        }
        QSqlDatabase::removeDatabase(name);
    }
    QSqlDatabase db() const { return QSqlDatabase::database(name, false); }
    bool exec(const QString &sql) {
        QSqlQuery q(db());
        const bool ok = q.exec(sql);
        if (!ok) std::printf("raw exec failed: %s: %s\n", qPrintable(sql), qPrintable(q.lastError().text()));
        return ok;
    }
    QVariant scalar(const QString &sql, const QVariantList &bind = QVariantList()) {
        QSqlQuery q(db());
        q.prepare(sql);
        for (int i = 0; i < bind.size(); ++i) q.bindValue(i, bind.at(i));
        if (!q.exec() || !q.next()) return QVariant();
        return q.value(0);
    }
};

static int fileMode(const QString &path) {
    struct stat st;
    if (::stat(QFile::encodeName(path).constData(), &st) != 0) return -1;
    return int(st.st_mode & 0777);
}

static QByteArray fileHash(const QString &path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return QByteArray();
    return QCryptographicHash::hash(f.readAll(), QCryptographicHash::Sha1);
}

struct Recorder {
    int subscriptions = 0, playlists = 0, history = 0;
    QStringList watch, resume, playlist;
    void attach(LibraryStore *s) {
        QObject::connect(s, &LibraryStore::subscriptionsChanged, [this]() { ++subscriptions; });
        QObject::connect(s, &LibraryStore::playlistsChanged, [this]() { ++playlists; });
        QObject::connect(s, &LibraryStore::historyChanged, [this]() { ++history; });
        QObject::connect(s, &LibraryStore::watchStateChanged, [this](const QString &id) { watch << id; });
        QObject::connect(s, &LibraryStore::resumeChanged, [this](const QString &id) { resume << id; });
        QObject::connect(s, &LibraryStore::playlistChanged, [this](const QString &id) { playlist << id; });
    }
    void clear() { *this = Recorder(); }
};

static VideoEntry entry(const std::string &id, const char *title = "Title") {
    VideoEntry v;
    v.videoId = id;
    v.title = title;
    v.channel = "Channel";
    v.channelId = uc(1);
    v.durationSeconds = 600;
    v.thumbnailUrl = "https://i.ytimg.com/vi/" + id + "/mqdefault.jpg";
    return v;
}

static PlaybackProgress progress(const std::string &id, qint64 pos, qint64 dur = 600 * S, bool ended = false) {
    PlaybackProgress p;
    p.video = entry(id);
    p.positionMs = pos;
    p.durationMs = dur;
    p.ended = ended;
    return p;
}

static ChannelRecord channel(const std::string &id, const char *name, qint64 latest = 0) {
    ChannelRecord c;
    c.channelId = id;
    c.name = name;
    c.latestUploadMs = latest;
    return c;
}

static bool pump(const std::function<bool()> &done, int timeoutMs) {
    QElapsedTimer t;
    t.start();
    while (!done() && t.elapsed() < timeoutMs) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(5);
    }
    return done();
}

// ---- open, permissions, schema ---------------------------------------------

static void openFresh() {
    const QString dir = freshDir("fresh") + QStringLiteral("/nested");
    {
        std::unique_ptr<LibraryStore> s = openStore(dbIn(dir));
        CHECK(s->isOpen());
        CHECK(QDir(dir).exists());
        CHECK(s->schemaVersion() == librarySchemaVersion);
        CHECK(fileMode(dbIn(dir)) == 0600);
        s->recordProgress(progress(vid(1), 60 * S), true);
        const QStringList side = QStringList() << dbIn(dir) + "-wal" << dbIn(dir) + "-shm";
        for (const QString &f : side) {
            CHECK(QFile::exists(f));
            CHECK(fileMode(f) == 0600);
        }
        // LIB-02: watch later exists on a fresh install and is listed first.
        const std::vector<PlaylistRecord> lists = s->playlists();
        CHECK(lists.size() == 1);
        CHECK(!lists.empty() && lists[0].id == watchLaterPlaylistId && lists[0].kind == PlaylistKind::WatchLater);
        CHECK(!lists.empty() && lists[0].title == "Watch later");
        CHECK(s->historyCap() == 500);
        Raw raw(dbIn(dir));
        CHECK(raw.scalar("PRAGMA user_version").toInt() == librarySchemaVersion);
        CHECK(raw.scalar("PRAGMA journal_mode").toString() == "wal");
    }
    CHECK(fileMode(dbIn(dir)) == 0600);
}

static void schemaRefusal() {
    const QString dir = freshDir("newer");
    QDir().mkpath(dir);
    {
        Raw raw(dbIn(dir));
        raw.exec("CREATE TABLE future(a TEXT)");
        raw.exec("INSERT INTO future VALUES('keep')");
        raw.exec("PRAGMA user_version = 99");
    }
    const QByteArray before = fileHash(dbIn(dir));
    QString error;
    {
        std::unique_ptr<LibraryStore> s = openStore(dbIn(dir), &error);
        CHECK(!s->isOpen());
        CHECK(!error.isEmpty());
        CHECK(s->playlists().empty());
        s->recordProgress(progress(vid(1), 60 * S), true); // ignored when closed
    }
    CHECK(fileHash(dbIn(dir)) == before);
    Raw raw(dbIn(dir));
    CHECK(raw.scalar("PRAGMA user_version").toInt() == 99);
    CHECK(raw.scalar("SELECT a FROM future").toString() == "keep");
    CHECK(!raw.scalar("SELECT name FROM sqlite_master WHERE name = 'history'").isValid());
}

// NFR-ROB-02: orphaned temp files of interrupted imports/exports are removed at start.
static void orphanCleanup() {
    const QString dir = freshDir("orphans");
    QDir().mkpath(dir);
    const QStringList orphans = QStringList() << "backup.tmp" << "newpipe.db.part-import" << ".hidden.tmp" << "x.zip.part-import";
    const QStringList keep = QStringList() << "notes.txt" << "tmp.keep" << "library.db.bak";
    for (const QString &n : orphans + keep) {
        QFile f(dir + "/" + n);
        f.open(QIODevice::WriteOnly);
        f.write("x");
    }
    std::unique_ptr<LibraryStore> s = openStore(dbIn(dir));
    CHECK(s->isOpen());
    for (const QString &n : orphans) CHECK(!QFile::exists(dir + "/" + n));
    for (const QString &n : keep) CHECK(QFile::exists(dir + "/" + n));
}

// ---- LIB-01 -------------------------------------------------------------------

static void subscriptions() {
    const QString dir = freshDir("subs");
    std::unique_ptr<LibraryStore> s = openStore(dbIn(dir));
    Recorder rec;
    rec.attach(s.get());
    CHECK(!s->subscribe(channel("UCshort", "Bad")));
    CHECK(!s->subscribe(channel("XX" + std::string(22, 'a'), "Bad")));
    CHECK(rec.subscriptions == 0);
    g_now += 1000;
    ChannelRecord first = channel(uc(1), "First");
    first.avatarUrl = "https://yt3.example/a.jpg";
    CHECK(s->subscribe(first));
    CHECK(s->isSubscribed(Q(uc(1))));
    CHECK(rec.subscriptions == 1);
    const qint64 added = g_now;
    g_now += 5000;
    CHECK(s->subscribe(channel(uc(1), "Renamed"))); // twice: one entry, name updated, avatar and time kept
    std::vector<ChannelRecord> list = s->subscriptions(SubscriptionSort::Name);
    CHECK(list.size() == 1);
    CHECK(!list.empty() && list[0].name == "Renamed" && list[0].avatarUrl == "https://yt3.example/a.jpg");
    CHECK(!list.empty() && list[0].addedMs == added);
    CHECK(s->unsubscribe(Q(uc(1))));
    CHECK(!s->isSubscribed(Q(uc(1))));
    CHECK(!s->unsubscribe(Q(uc(1))));
    CHECK(s->subscriptions(SubscriptionSort::Name).empty());

    // 20-channel fixture, both orders.
    const char *names[20] = {"zeta", "Alpha", "beta", "Beta", "gamma", "Delta", "epsilon", "kappa", "alpha", "omega",
                             "Omicron", "pi", "Rho", "sigma", "Tau", "upsilon", "Phi", "chi", "Psi", "eta"};
    for (int i = 0; i < 20; ++i) CHECK(s->subscribe(channel(uc(100 + i), names[i], i % 5 == 0 ? 0 : 1000000 + i * 1000)));
    list = s->subscriptions(SubscriptionSort::Name);
    CHECK(list.size() == 20);
    const char *expected[20] = {"Alpha", "alpha", "beta", "Beta", "chi", "Delta", "epsilon", "eta", "gamma", "kappa",
                                "omega", "Omicron", "Phi", "pi", "Psi", "Rho", "sigma", "Tau", "upsilon", "zeta"};
    for (int i = 0; i < 20 && i < int(list.size()); ++i) CHECK(list[std::size_t(i)].name == expected[i]);
    list = s->subscriptions(SubscriptionSort::LatestUpload);
    CHECK(list.size() == 20);
    // Newest upload is index 19 ("eta"); unknown ones (i % 5 == 0) come last, by name.
    CHECK(!list.empty() && list[0].name == "eta");
    CHECK(list.size() == 20 && list[15].name == "Alpha" && list[16].name == "Delta" && list[17].name == "Omicron"
          && list[18].name == "upsilon" && list[19].name == "zeta");
    for (std::size_t i = 1; i < 16 && i < list.size(); ++i) CHECK(list[i - 1].latestUploadMs > list[i].latestUploadMs);
    // Feed write-back.
    s->updateChannelInfo(Q(uc(100)), 9000000, QString(), QStringLiteral("https://yt3.example/z.jpg"));
    list = s->subscriptions(SubscriptionSort::LatestUpload);
    CHECK(!list.empty() && list[0].name == "zeta" && list[0].latestUploadMs == 9000000 && list[0].avatarUrl == "https://yt3.example/z.jpg");
    const int before = rec.subscriptions;
    s->updateChannelInfo(Q(uc(100)), 9000000, QString(), QString()); // nothing changes, no signal
    CHECK(rec.subscriptions == before);
    const QStringList ids = s->subscribedChannelIds();
    CHECK(ids.size() == 20 && ids.first() == Q(uc(100)) && ids.last() == Q(uc(119)));

    // Pending handles: added by an import, resolved online or dropped.
    ImportBatch b;
    PendingHandle h1, h2;
    h1.url = "https://www.youtube.com/@first";
    h1.name = "First handle";
    h2.url = "https://www.youtube.com/c/second";
    b.pendingHandles.push_back(h1);
    b.pendingHandles.push_back(h2);
    CHECK(s->applyImport(b).handlesPending == 2);
    CHECK(s->pendingHandles().size() == 2);
    CHECK(!s->resolvePendingHandle(Q(h1.url), channel("UCbad", "")));
    CHECK(s->resolvePendingHandle(Q(h1.url), channel(uc(7), "")));
    CHECK(s->isSubscribed(Q(uc(7))));
    list = s->subscriptions(SubscriptionSort::Name);
    bool named = false;
    for (const ChannelRecord &c : list) if (c.channelId == uc(7)) named = c.name == "First handle";
    CHECK(named);
    CHECK(s->pendingHandles().size() == 1);
    s->dropPendingHandle(Q(h2.url));
    CHECK(s->pendingHandles().empty());

    s.reset();
    s = openStore(dbIn(dir));
    CHECK(s->isSubscribed(Q(uc(7))) && s->isSubscribed(Q(uc(119))) && !s->isSubscribed(Q(uc(1))));
    CHECK(s->subscriptions(SubscriptionSort::Name).size() == 21);
}

// ---- LIB-02/03 ----------------------------------------------------------------

static bool isLocalId(const std::string &id) {
    if (id.size() != 12) return false;
    for (char c : id) if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    return true;
}

static void playlists() {
    const QString dir = freshDir("playlists");
    std::unique_ptr<LibraryStore> s = openStore(dbIn(dir));
    Recorder rec;
    rec.attach(s.get());
    const QString a = s->createPlaylist(QStringLiteral("  Road   trip "));
    const QString b = s->createPlaylist(QString());
    CHECK(isLocalId(a.toStdString()) && isLocalId(b.toStdString()) && a != b);
    std::vector<PlaylistRecord> lists = s->playlists();
    CHECK(lists.size() == 3);
    CHECK(lists.size() == 3 && lists[0].id == watchLaterPlaylistId && lists[1].id == b.toStdString() && lists[2].id == a.toStdString());
    CHECK(lists.size() == 3 && lists[1].title == "New playlist" && lists[2].title == "Road trip");
    CHECK(rec.playlists == 2);

    CHECK(s->renamePlaylist(a, QStringLiteral("Road trip 2026")));
    CHECK(!s->renamePlaylist(a, QStringLiteral("   ")));
    CHECK(!s->renamePlaylist(QString::fromLatin1(watchLaterPlaylistId), QStringLiteral("Later")));
    CHECK(!s->deletePlaylist(QString::fromLatin1(watchLaterPlaylistId)));
    PlaylistRecord p;
    CHECK(s->playlist(a, &p) && p.title == "Road trip 2026");
    CHECK(s->renamePlaylist(a, Q(std::string(150, 'x'))) && s->playlist(a, &p) && p.title == std::string(100, 'x'));

    // Items: no duplicates, dense positions on remove and move.
    CHECK(s->addToPlaylist(a, entry(vid(1), "One")));
    CHECK(s->addToPlaylist(a, entry(vid(2), "Two")));
    CHECK(s->addToPlaylist(a, entry(vid(3), "Three")));
    CHECK(!s->addToPlaylist(a, entry(vid(1), "One again")));
    CHECK(!s->addToPlaylist(a, entry("bad", "Bad")));
    CHECK(rec.playlist.count(a) == 3);
    std::vector<VideoEntry> items = s->playlistItems(a);
    CHECK(items.size() == 3 && items[0].videoId == vid(1) && items[0].title == "One" && items[2].videoId == vid(3));
    CHECK(s->playlistContains(a, Q(vid(2))) && !s->playlistContains(a, Q(vid(4))));
    CHECK(s->playlist(a, &p) && p.itemCount == 3 && p.thumbnailUrl == entry(vid(1)).thumbnailUrl);
    CHECK(s->removeFromPlaylist(a, Q(vid(2))));
    CHECK(!s->removeFromPlaylist(a, Q(vid(2))));
    {
        Raw raw(dbIn(dir));
        CHECK(raw.scalar("SELECT group_concat(position) FROM (SELECT position FROM playlist_items WHERE playlist_id = ? ORDER BY position)",
                         QVariantList() << a).toString() == "0,1");
    }
    CHECK(s->addToPlaylist(a, entry(vid(4))));
    CHECK(s->moveInPlaylist(a, 0, 2));
    items = s->playlistItems(a);
    CHECK(items.size() == 3 && items[0].videoId == vid(3) && items[1].videoId == vid(4) && items[2].videoId == vid(1));
    CHECK(s->moveInPlaylist(a, 2, 0));
    items = s->playlistItems(a);
    CHECK(items.size() == 3 && items[0].videoId == vid(1) && items[1].videoId == vid(3) && items[2].videoId == vid(4));
    CHECK(!s->moveInPlaylist(a, 0, 3));
    CHECK(!s->moveInPlaylist(a, -1, 0));
    {
        Raw raw(dbIn(dir));
        CHECK(raw.scalar("SELECT group_concat(position) FROM (SELECT position FROM playlist_items WHERE playlist_id = ? ORDER BY position)",
                         QVariantList() << a).toString() == "0,1,2");
    }
    CHECK(s->addToPlaylist(QString::fromLatin1(watchLaterPlaylistId), entry(vid(9))));
    CHECK(s->playlistItems(QString::fromLatin1(watchLaterPlaylistId)).size() == 1);

    // Reorder survives closing and reopening (LIB-02 SHOULD); watch later stays first.
    const QString c = s->createPlaylist(QStringLiteral("Third")); // order: WL, c, b, a
    CHECK(s->movePlaylist(a, 1));                                 // WL, a, c, b
    CHECK(!s->movePlaylist(QString::fromLatin1(watchLaterPlaylistId), 3));
    CHECK(s->movePlaylist(b, 0));                                 // clamped below WL: WL, b, a, c
    s.reset();
    s = openStore(dbIn(dir));
    rec.clear();
    rec.attach(s.get());
    lists = s->playlists();
    CHECK(lists.size() == 4 && lists[0].id == watchLaterPlaylistId && lists[1].id == b.toStdString()
          && lists[2].id == a.toStdString() && lists[3].id == c.toStdString());
    const QString d = s->createPlaylist(QStringLiteral("Newest"));
    lists = s->playlists();
    CHECK(lists.size() == 5 && lists[1].id == d.toStdString()); // new lists on top, below watch later

    // LIB-03 mirrors: one per remote id, re-save updates the header.
    RemotePlaylistRef ref;
    ref.remoteId = "https://www.youtube.com/playlist?list=PLrAXtmErZgOeiKm4sgNOknGvNjby9efdf";
    ref.title = "Mix";
    ref.channel = "Owner";
    ref.thumbnailUrl = "https://i.ytimg.com/pl.jpg";
    ref.itemCountHint = 42;
    const QString m = s->saveRemotePlaylist(ref);
    CHECK(m == QStringLiteral("r-PLrAXtmErZgOeiKm4sgNOknGvNjby9efdf"));
    ref.remoteId = "PLrAXtmErZgOeiKm4sgNOknGvNjby9efdf";
    ref.title = "Mix renamed";
    ref.channel = "New owner";
    CHECK(s->saveRemotePlaylist(ref) == m);
    CHECK(s->mirrorIdForRemote(QStringLiteral("https://youtube.com/watch?v=dQw4w9WgXcQ&list=PLrAXtmErZgOeiKm4sgNOknGvNjby9efdf")) == m);
    CHECK(s->mirrorIdForRemote(QStringLiteral("PLnotsaved000")).isEmpty());
    ref.remoteId = "not a list!";
    CHECK(s->saveRemotePlaylist(ref).isEmpty());
    lists = s->playlists();
    int mirrors = 0;
    for (const PlaylistRecord &x : lists) {
        if (x.kind != PlaylistKind::RemoteMirror) continue;
        ++mirrors;
        CHECK(x.title == "Mix renamed" && x.channel == "New owner" && x.itemCount == 42 && x.refreshedMs == 0);
        CHECK(x.remoteId == "PLrAXtmErZgOeiKm4sgNOknGvNjby9efdf");
    }
    CHECK(mirrors == 1);
    CHECK(!s->renamePlaylist(m, QStringLiteral("Mine")));
    CHECK(!s->addToPlaylist(m, entry(vid(1))));
    CHECK(!s->movePlaylist(m, 1));

    // 5000 items in one transaction, in order; a refresh replaces the contents.
    std::vector<VideoEntry> many;
    for (int i = 0; i < 5000; ++i) many.push_back(entry(vid(10000 + i)));
    many.push_back(entry(vid(10000))); // duplicate dropped
    QElapsedTimer t;
    t.start();
    CHECK(s->replaceRemoteItems(m, many, g_now));
    const qint64 elapsed = t.elapsed();
    std::printf("  replaceRemoteItems(5000): %lld ms\n", static_cast<long long>(elapsed));
    CHECK(elapsed < 5000);
    items = s->playlistItems(m);
    CHECK(items.size() == 5000);
    bool ordered = items.size() == 5000;
    for (std::size_t i = 0; ordered && i < items.size(); ++i) ordered = items[i].videoId == vid(10000 + int(i));
    CHECK(ordered);
    CHECK(s->playlist(m, &p) && p.itemCount == 5000 && p.refreshedMs == g_now && p.thumbnailUrl == "https://i.ytimg.com/pl.jpg");
    std::vector<VideoEntry> three;
    for (int i = 0; i < 3; ++i) three.push_back(entry(vid(20000 + i)));
    CHECK(s->replaceRemoteItems(m, three, g_now + 1));
    CHECK(s->playlistItems(m).size() == 3 && s->playlist(m, &p) && p.itemCount == 3);
    CHECK(!s->replaceRemoteItems(a, three, g_now));

    // Delete: local and mirror ("unsave") allowed, items go with them.
    CHECK(s->deletePlaylist(m));
    CHECK(s->deletePlaylist(a));
    CHECK(!s->playlist(a, &p) && !s->playlist(m, &p));
    CHECK(s->mirrorIdForRemote(QStringLiteral("PLrAXtmErZgOeiKm4sgNOknGvNjby9efdf")).isEmpty());
    Raw raw(dbIn(dir));
    CHECK(raw.scalar("SELECT COUNT(*) FROM playlist_items WHERE playlist_id IN (?, ?)", QVariantList() << a << m).toInt() == 0);
}

// ---- LIB-04/05 -----------------------------------------------------------------

static void history() {
    const QString dir = freshDir("history");
    std::unique_ptr<LibraryStore> s = openStore(dbIn(dir));
    Recorder rec;
    rec.attach(s.get());
    CHECK(s->historyCap() == 500);
    CHECK(s->setHistoryCap(100));
    CHECK(!s->setHistoryCap(99));
    CHECK(!s->setHistoryCap(5001));
    CHECK(!s->setHistoryCap(0));
    CHECK(s->historyCap() == 100);
    CHECK(s->metaValue("history_cap") == "100");

    // Cap 100: the 101st record evicts the oldest.
    for (int i = 0; i < 100; ++i) {
        g_now += 1000;
        s->recordProgress(progress(vid(i), 60 * S), true);
    }
    CHECK(s->historyCount() == 100);
    rec.clear();
    g_now += 1000;
    s->recordProgress(progress(vid(100), 60 * S), true);
    CHECK(s->historyCount() == 100);
    CHECK(!s->watchState(Q(vid(0))).known && s->watchState(Q(vid(1))).known && s->watchState(Q(vid(100))).known);
    CHECK(rec.watch.size() == 2 && rec.watch.contains(Q(vid(0))) && rec.watch.contains(Q(vid(100))));
    std::vector<HistoryRecord> page = s->history(0, 2);
    CHECK(page.size() == 2 && page[0].video.videoId == vid(100) && page[1].video.videoId == vid(99));
    page = s->history(99, 10);
    CHECK(page.size() == 1 && page[0].video.videoId == vid(1));
    CHECK(s->setHistoryCap(500));
    s->clearHistory();

    // Order equals recency even with an identical clock.
    const qint64 frozen = g_now;
    s->recordProgress(progress(vid(201), 30 * S), true);
    s->recordProgress(progress(vid(202), 30 * S), true);
    s->recordProgress(progress(vid(203), 30 * S), true);
    s->recordProgress(progress(vid(201), 40 * S), false);
    CHECK(g_now == frozen);
    page = s->history(0, 10);
    CHECK(page.size() == 3 && page[0].video.videoId == vid(201) && page[1].video.videoId == vid(203)
          && page[2].video.videoId == vid(202));

    // Watched at 80 %, sticky after a later partial view; play count per session.
    g_now += 1000;
    s->recordProgress(progress(vid(210), 480 * S), true); // 80 % of 600 s
    CHECK(s->watchState(Q(vid(210))).watched);
    s->recordProgress(progress(vid(210), 30 * S), true);
    HistoryRecord r;
    CHECK(s->historyRecord(Q(vid(210)), &r) && r.watched && r.positionMs == 30 * S && r.playCount == 2);
    CHECK(r.fraction > 0.79 && r.video.title == "Title" && r.durationMs == 600 * S);
    s->recordProgress(progress(vid(211), 479 * S), true);
    CHECK(!s->watchState(Q(vid(211))).watched);
    s->recordProgress(progress(vid(212), 585 * S), true); // 15 s before the end
    CHECK(s->watchState(Q(vid(212))).watched);

    // Paused history: no record, the resume point still updates.
    s->setHistoryPaused(true);
    CHECK(s->historyPaused());
    const int count = s->historyCount();
    rec.clear();
    s->recordProgress(progress(vid(220), 120 * S), true);
    CHECK(s->historyCount() == count && !s->watchState(Q(vid(220))).known);
    CHECK(s->resumePositionMs(Q(vid(220))) == 120 * S);
    CHECK(rec.history == 0 && rec.watch.isEmpty() && rec.resume == QStringList() << Q(vid(220)));
    s->setHistoryPaused(false);
    CHECK(!s->historyPaused());

    // Manual mark clears the resume point; unmark leaves it, and creates nothing when absent.
    s->recordProgress(progress(vid(230), 100 * S), true);
    CHECK(s->resumePositionMs(Q(vid(230))) == 100 * S);
    s->markWatched(Q(vid(230)), entry(vid(230)));
    CHECK(s->resumePositionMs(Q(vid(230))) == -1);
    CHECK(s->historyRecord(Q(vid(230)), &r) && r.watched && r.fraction == 1.0 && r.positionMs == 600 * S);
    s->markWatched(Q(vid(231)), entry(vid(231), "Never played"));
    CHECK(s->historyRecord(Q(vid(231)), &r) && r.watched && r.video.title == "Never played");
    s->recordProgress(progress(vid(232), 100 * S), true);
    s->markWatched(Q(vid(232)), entry(vid(232)));
    s->recordProgress(progress(vid(232), 200 * S), true); // resume point again
    s->unmarkWatched(Q(vid(232)));
    CHECK(s->historyRecord(Q(vid(232)), &r) && !r.watched && r.fraction == 0 && r.positionMs == 0);
    CHECK(s->resumePositionMs(Q(vid(232))) == 200 * S);
    const int beforeUnmark = s->historyCount();
    s->unmarkWatched(Q(vid(299)));
    CHECK(s->historyCount() == beforeUnmark && !s->historyRecord(Q(vid(299)), &r));

    s->removeFromHistory(Q(vid(211)));
    CHECK(!s->watchState(Q(vid(211))).known && !s->historyRecord(Q(vid(211)), &r));

    // Persistence.
    const std::vector<HistoryRecord> all = s->history(0, 1000);
    s.reset();
    s = openStore(dbIn(dir));
    rec.clear();
    rec.attach(s.get());
    const std::vector<HistoryRecord> again = s->history(0, 1000);
    CHECK(all.size() == again.size());
    bool same = all.size() == again.size();
    for (std::size_t i = 0; same && i < all.size(); ++i)
        same = all[i].video.videoId == again[i].video.videoId && all[i].watched == again[i].watched
               && all[i].playCount == again[i].playCount && all[i].positionMs == again[i].positionMs;
    CHECK(same);
    CHECK(s->watchState(Q(vid(210))).watched);

    // Clearing history leaves every resume point.
    CHECK(s->resumePositionMs(Q(vid(232))) == 200 * S && s->resumePositionMs(Q(vid(220))) == 120 * S);
    s->clearHistory();
    CHECK(s->historyCount() == 0 && s->history(0, 10).empty() && !s->watchState(Q(vid(210))).known);
    CHECK(rec.history == 1 && rec.watch.size() == int(all.size()));
    CHECK(s->resumePositionMs(Q(vid(232))) == 200 * S && s->resumePositionMs(Q(vid(220))) == 120 * S);
    s.reset();
    s = openStore(dbIn(dir));
    CHECK(s->historyCount() == 0 && s->resumePositionMs(Q(vid(232))) == 200 * S);
}

static void resume() {
    const QString dir = freshDir("resume");
    std::unique_ptr<LibraryStore> s = openStore(dbIn(dir));
    s->setHistoryPaused(true); // resume rules only
    Recorder rec;
    rec.attach(s.get());
    s->recordProgress(progress(vid(1), 9 * S), true);
    CHECK(s->resumePositionMs(Q(vid(1))) == -1);
    s->recordProgress(progress(vid(1), 10 * S), true);
    CHECK(s->resumePositionMs(Q(vid(1))) == 10 * S);
    s->recordProgress(progress(vid(1), 600 * S - 14 * S), false); // 14 s before the end: "none" deletes the point
    CHECK(s->resumePositionMs(Q(vid(1))) == -1);
    s->recordProgress(progress(vid(1), 600 * S - 16 * S), false);
    CHECK(s->resumePositionMs(Q(vid(1))) == 584 * S);
    s->recordProgress(progress(vid(1), 300 * S, 600 * S, true), false); // ended
    CHECK(s->resumePositionMs(Q(vid(1))) == -1);
    PlaybackProgress live = progress(vid(2), 300 * S, 0);
    live.live = true;
    s->recordProgress(live, true);
    CHECK(s->resumePositionMs(Q(vid(2))) == -1);
    CHECK(rec.resume.count(Q(vid(1))) == 4);
    s->recordProgress(progress(vid(3), 100 * S), true);
    s->clearResume(Q(vid(3)));
    CHECK(s->resumePositionMs(Q(vid(3))) == -1);

    // Expiry: 30 days kept, 31 days gone (read side, swept on write and at open).
    s->recordProgress(progress(vid(10), 100 * S), true);
    const qint64 t0 = g_now;
    g_now = t0 + 30 * Day;
    CHECK(s->resumePositionMs(Q(vid(10))) == 100 * S);
    g_now = t0 + 31 * Day;
    CHECK(s->resumePositionMs(Q(vid(10))) == -1);
    s->recordProgress(progress(vid(11), 100 * S), true); // sweep on write
    {
        Raw raw(dbIn(dir));
        CHECK(raw.scalar("SELECT COUNT(*) FROM resume WHERE video_id = ?", QVariantList() << Q(vid(10))).toInt() == 0);
    }
    // At open: a point written 31 days "ago" by another instance is swept.
    {
        Raw raw(dbIn(dir));
        raw.exec(QStringLiteral("INSERT INTO resume VALUES('%1', 50000, 600000, %2)").arg(Q(vid(12))).arg(g_now - 31 * Day));
    }
    s.reset();
    s = openStore(dbIn(dir));
    CHECK(s->resumePositionMs(Q(vid(12))) == -1);
    {
        Raw raw(dbIn(dir));
        CHECK(raw.scalar("SELECT COUNT(*) FROM resume WHERE video_id = ?", QVariantList() << Q(vid(12))).toInt() == 0);
    }

    // Capacity: the 1001st entry evicts the least recently touched one (identical clock too).
    s.reset();
    const QString dir2 = freshDir("resume-cap");
    s = openStore(dbIn(dir2));
    s->setHistoryPaused(true);
    for (int i = 0; i < 1000; ++i) {
        if (i < 500) g_now += 1;
        s->recordProgress(progress(vid(1000 + i), 100 * S), true);
    }
    s->recordProgress(progress(vid(1000), 120 * S), false); // touch the oldest: now the newest
    s->recordProgress(progress(vid(3000), 100 * S), true);  // 1001st
    CHECK(s->resumePositionMs(Q(vid(1000))) == 120 * S);
    CHECK(s->resumePositionMs(Q(vid(1001))) == -1);
    CHECK(s->resumePositionMs(Q(vid(1002))) == 100 * S);
    CHECK(s->resumePositionMs(Q(vid(3000))) == 100 * S);
    g_now += 1;
    s->recordProgress(progress(vid(3001), 100 * S), true); // the next oldest goes
    CHECK(s->resumePositionMs(Q(vid(1002))) == -1 && s->resumePositionMs(Q(vid(1003))) == 100 * S);
    {
        Raw raw(dbIn(dir2));
        CHECK(raw.scalar("SELECT COUNT(*) FROM resume").toInt() == 1000);
    }
    s.reset();
    s = openStore(dbIn(dir2));
    CHECK(s->resumePositionMs(Q(vid(1001))) == -1 && s->resumePositionMs(Q(vid(1003))) == 100 * S
          && s->resumePositionMs(Q(vid(1000))) == 120 * S);
}

// ---- LIB-06 incremental lookups ---------------------------------------------------

static void incremental() {
    const QString dir = freshDir("incremental");
    std::unique_ptr<LibraryStore> s = openStore(dbIn(dir));
    CHECK(s->setHistoryCap(5000));
    ImportBatch b;
    for (int i = 0; i < 5000; ++i) {
        HistoryRecord h;
        h.video = entry(vid(i));
        h.positionMs = 30 * S;
        h.durationMs = 600 * S;
        h.fraction = 0.05;
        h.watchedAtMs = g_now - 5000 + i;
        b.history.push_back(h);
    }
    QElapsedTimer t;
    t.start();
    const ImportSummary sum = s->applyImport(b);
    std::printf("  applyImport(5000 history): %lld ms\n", static_cast<long long>(t.elapsed()));
    CHECK(sum.historyAdded == 5000 && s->historyCount() == 5000);
    Recorder rec;
    rec.attach(s.get());
    g_now += 1000;
    t.restart();
    s->recordProgress(progress(vid(2500), 300 * S), false);
    std::printf("  recordProgress with 5000 history rows: %lld ms\n", static_cast<long long>(t.elapsed()));
    CHECK(rec.watch == QStringList() << Q(vid(2500)));
    CHECK(rec.resume == QStringList() << Q(vid(2500)));
    CHECK(rec.history == 1 && rec.subscriptions == 0 && rec.playlists == 0 && rec.playlist.isEmpty());
    CHECK(s->watchState(Q(vid(2500))).fraction == 0.5);
    // No signal carries more than one id: nothing proportional to the history size.
    const QMetaObject *mo = s->metaObject();
    int signalCount = 0;
    for (int i = mo->methodOffset(); i < mo->methodCount(); ++i) {
        const QMetaMethod m = mo->method(i);
        if (m.methodType() != QMetaMethod::Signal) continue;
        ++signalCount;
        CHECK(m.parameterCount() <= 1);
        if (m.parameterCount() == 1) CHECK(m.parameterType(0) == QMetaType::QString);
    }
    CHECK(signalCount == 6);
    // In-memory lookups stay fast (no SQL): 100k lookups well under a second.
    t.restart();
    int known = 0;
    for (int i = 0; i < 100000; ++i) known += s->watchState(Q(vid(i % 6000))).known ? 1 : 0;
    CHECK(known > 0 && t.elapsed() < 2000);
}

// ---- RES-02 busy database -----------------------------------------------------------

static void busyQueue() {
    const QString dir = freshDir("busy");
    std::unique_ptr<LibraryStore> s = openStore(dbIn(dir));
    s->recordProgress(progress(vid(1), 60 * S), true);
    Raw blocker(dbIn(dir));
    CHECK(blocker.exec("BEGIN IMMEDIATE"));
    CHECK(blocker.exec("INSERT OR REPLACE INTO meta(key, value) VALUES('blocker', 'x')"));
    QElapsedTimer t;
    t.start();
    s->recordProgress(progress(vid(1), 120 * S), false);
    const qint64 first = t.elapsed();
    t.restart();
    s->recordProgress(progress(vid(1), 130 * S), false);  // queued behind the first, same order
    s->recordProgress(progress(vid(2), 140 * S), true);
    const qint64 more = t.elapsed();
    std::printf("  busy recordProgress: %lld ms, two more: %lld ms\n", static_cast<long long>(first), static_cast<long long>(more));
    CHECK(first < 200);
    CHECK(more < 400);
    CHECK(s->resumePositionMs(Q(vid(1))) == 130 * S);
    CHECK(s->resumePositionMs(Q(vid(2))) == 140 * S);
    CHECK(s->watchState(Q(vid(2))).known);
    HistoryRecord r;
    CHECK(s->historyRecord(Q(vid(1)), &r) && r.positionMs == 130 * S && r.playCount == 1); // merge saw the queued write
    CHECK(blocker.scalar("SELECT position_ms FROM resume WHERE video_id = ?", QVariantList() << Q(vid(1))).toLongLong() == 60 * S);
    CHECK(blocker.exec("COMMIT"));
    const bool landed = pump([&]() {
        return blocker.scalar("SELECT position_ms FROM resume WHERE video_id = ?", QVariantList() << Q(vid(2))).toLongLong() == 140 * S;
    }, 3000);
    CHECK(landed);
    CHECK(blocker.scalar("SELECT position_ms FROM resume WHERE video_id = ?", QVariantList() << Q(vid(1))).toLongLong() == 130 * S);
    CHECK(blocker.scalar("SELECT position_ms FROM history WHERE video_id = ?", QVariantList() << Q(vid(1))).toLongLong() == 130 * S);
    CHECK(blocker.scalar("SELECT value FROM meta WHERE key = 'blocker'").toString() == "x");
}

// ---- IMP-01 merge rules ---------------------------------------------------------------

static HistoryRecord imported(const std::string &id, qint64 at, bool watched, int plays, double fraction = 0.3) {
    HistoryRecord h;
    h.video = entry(id, "Imported");
    h.positionMs = qint64(fraction * 600 * S);
    h.durationMs = 600 * S;
    h.fraction = fraction;
    h.watched = watched;
    h.watchedAtMs = at;
    h.playCount = plays;
    return h;
}

static ResumeRecord point(const std::string &id, qint64 pos, qint64 dur = 600 * S) {
    ResumeRecord r;
    r.videoId = id;
    r.positionMs = pos;
    r.durationMs = dur;
    r.touchedMs = 1;
    return r;
}

static void importMerge() {
    const QString dir = freshDir("import");
    std::unique_ptr<LibraryStore> s = openStore(dbIn(dir));
    const std::string L1 = vid(1), L2 = vid(2), N1 = vid(11), N2 = vid(12), N3 = vid(13);
    const std::string PL_EXIST = "PLexisting00000001", PL_NEW = "PLnew0000000000002";
    s->subscribe(channel(uc(1), "Local A"));
    g_now += 1000;
    s->recordProgress(progress(L1, 60 * S), true);   // resume 60 s, not watched
    g_now += 1000;
    s->recordProgress(progress(L2, 590 * S), true);  // watched
    const QString fav = s->createPlaylist(QStringLiteral("Favorites"));
    s->addToPlaylist(fav, entry(vid(50)));
    RemotePlaylistRef existing;
    existing.remoteId = PL_EXIST;
    existing.title = "Local mirror";
    s->saveRemotePlaylist(existing);

    ImportBatch b;
    b.source = "newpipe";
    b.subscriptions.push_back(channel(uc(1), "Imported A"));
    b.subscriptions.push_back(channel(uc(2), "B"));
    b.subscriptions.push_back(channel(uc(2), "B again"));
    b.subscriptions.push_back(channel("UCbad", "Bad"));
    PendingHandle h;
    h.url = "https://www.youtube.com/@newhandle";
    b.pendingHandles.push_back(h);
    h.url = "https://www.youtube.com/channel/" + uc(3);
    h.name = "C";
    b.pendingHandles.push_back(h);
    h.url = "https://example.com/x";
    b.pendingHandles.push_back(h);
    b.history.push_back(imported(L1, 100, true, 3));
    b.history.push_back(imported(L2, 100, true, 2));
    b.history.push_back(imported(N1, 500, false, 1));
    b.history.push_back(imported(N2, 300, true, 4));
    b.history.push_back(imported(N3, 400, false, 1));
    b.history.push_back(imported("bad", 400, false, 1));
    b.resume.push_back(point(L1, 300 * S));
    b.resume.push_back(point(N1, 50 * S));
    b.resume.push_back(point(N2, 5 * S));
    b.resume.push_back(point(N3, 590 * S));
    ImportedPlaylist favorites;
    favorites.title = "  FAVORITES ";
    favorites.items.push_back(entry(vid(60)));
    b.playlists.push_back(favorites);
    ImportedPlaylist trip;
    trip.title = "Road trip";
    trip.items.push_back(entry(vid(61)));
    trip.items.push_back(entry(vid(62)));
    trip.items.push_back(entry(vid(61)));
    trip.items.push_back(entry("bad"));
    b.playlists.push_back(trip);
    RemotePlaylistRef r1, r2, r3;
    r1.remoteId = PL_EXIST;
    r1.title = "Remote title";
    r2.remoteId = "https://www.youtube.com/playlist?list=" + PL_NEW;
    r2.title = "New mirror";
    r2.itemCountHint = 42;
    r3.remoteId = "bad id!";
    b.remotePlaylists.push_back(r1);
    b.remotePlaylists.push_back(r2);
    b.remotePlaylists.push_back(r3);
    b.skippedSections.push_back("stream_state");
    b.skippedRows = 2;

    // The importer runs on its own instance; the primary store reloads afterwards.
    Recorder main;
    main.attach(s.get());
    std::unique_ptr<LibraryStore> importer = openStore(dbIn(dir));
    Recorder rec;
    rec.attach(importer.get());
    g_now += 1000;
    const ImportSummary sum = importer->applyImport(b);
    CHECK(sum.subscriptionsAdded == 2);
    CHECK(sum.handlesPending == 1);
    CHECK(sum.historyAdded == 3);
    CHECK(sum.historyUpdated == 2);
    CHECK(sum.resumeAdded == 1);
    CHECK(sum.playlistsAdded == 1);
    CHECK(sum.playlistsSkipped == 1);
    CHECK(sum.playlistItemsAdded == 2);
    CHECK(sum.remotePlaylistsAdded == 1);
    CHECK(sum.skippedRows == 6);
    CHECK(sum.skippedSections.size() == 1 && sum.skippedSections[0] == "stream_state");
    CHECK(!sum.nothingNew());
    // Signals fire once each, watch state per changed id.
    CHECK(rec.subscriptions == 1 && rec.playlists == 1 && rec.history == 1);
    CHECK(rec.playlist.size() == 1);
    CHECK(rec.watch.size() == 4 && rec.watch.contains(Q(L1)) && rec.watch.contains(Q(N1)) && rec.watch.contains(Q(N2))
          && rec.watch.contains(Q(N3)));
    CHECK(rec.resume == QStringList() << Q(N1));
    // The importer's own indexes are current.
    CHECK(importer->isSubscribed(Q(uc(2))) && importer->watchState(Q(N2)).watched && importer->resumePositionMs(Q(N1)) == 50 * S);

    // Local wins.
    std::vector<ChannelRecord> subs = importer->subscriptions(SubscriptionSort::Name);
    CHECK(subs.size() == 3);
    for (const ChannelRecord &c : subs) if (c.channelId == uc(1)) CHECK(c.name == "Local A");
    HistoryRecord r;
    CHECK(importer->historyRecord(Q(L1), &r) && r.watched && r.playCount == 4 && r.positionMs == 60 * S && r.video.title == "Title");
    CHECK(importer->historyRecord(Q(L2), &r) && r.watched && r.playCount == 3);
    CHECK(importer->resumePositionMs(Q(L1)) == 60 * S);
    CHECK(importer->resumePositionMs(Q(N2)) == -1 && importer->resumePositionMs(Q(N3)) == -1);
    {
        Raw raw(dbIn(dir));
        CHECK(raw.scalar("SELECT touched_ms FROM resume WHERE video_id = ?", QVariantList() << Q(N1)).toLongLong() == g_now);
    }
    // Local records stay newest; imported ones follow in their own access order.
    std::vector<HistoryRecord> order = importer->history(0, 10);
    CHECK(order.size() == 5);
    const std::string expected[5] = {L2, L1, N1, N3, N2};
    for (std::size_t i = 0; i < 5 && i < order.size(); ++i) CHECK(order[i].video.videoId == expected[i]);
    // Playlists.
    std::vector<PlaylistRecord> lists = importer->playlists();
    CHECK(lists.size() == 5);
    CHECK(lists.size() == 5 && lists[0].id == watchLaterPlaylistId && lists[1].title == "Road trip"
          && lists[2].kind == PlaylistKind::RemoteMirror && lists[2].remoteId == PL_NEW && lists[2].itemCount == 42);
    CHECK(lists.size() == 5 && importer->playlistItems(Q(lists[1].id)).size() == 2);
    CHECK(importer->playlistItems(fav).size() == 1);
    PlaylistRecord mirror;
    CHECK(importer->playlist(importer->mirrorIdForRemote(Q(PL_EXIST)), &mirror) && mirror.title == "Local mirror");
    CHECK(importer->playlistItems(importer->mirrorIdForRemote(Q(PL_NEW))).empty());
    CHECK(importer->pendingHandles().size() == 1);

    // The primary store sees it after reload().
    CHECK(!s->watchState(Q(N1)).known);
    s->reload();
    CHECK(s->watchState(Q(N1)).known && s->watchState(Q(L1)).watched && s->isSubscribed(Q(uc(3))));
    CHECK(s->resumePositionMs(Q(N1)) == 50 * S);
    CHECK(main.watch.contains(Q(N1)) && main.watch.contains(Q(L1)) && main.resume.contains(Q(N1)));

    // Idempotent: the same batch again adds nothing.
    rec.clear();
    const ImportSummary again = importer->applyImport(b);
    CHECK(again.nothingNew());
    CHECK(again.playlistsSkipped == 2);
    CHECK(importer->historyRecord(Q(L1), &r) && r.playCount == 4);
    CHECK(rec.watch.isEmpty() && rec.resume.isEmpty() && rec.history == 0 && rec.subscriptions == 0 && rec.playlists == 0);
    importer.reset();

    // History cap applies after the import: local stays, only the newest imported fit.
    const QString dir2 = freshDir("import-cap");
    std::unique_ptr<LibraryStore> c = openStore(dbIn(dir2));
    CHECK(c->setHistoryCap(100));
    for (int i = 0; i < 90; ++i) {
        g_now += 1;
        c->recordProgress(progress(vid(500 + i), 30 * S), true);
    }
    ImportBatch big;
    for (int i = 0; i < 20; ++i) big.history.push_back(imported(vid(700 + i), 1000 + i, false, 1));
    const ImportSummary capped = c->applyImport(big);
    CHECK(capped.historyAdded == 10 && c->historyCount() == 100);
    CHECK(c->watchState(Q(vid(500))).known && c->watchState(Q(vid(719))).known && c->watchState(Q(vid(710))).known);
    CHECK(!c->watchState(Q(vid(709))).known);
    order = c->history(89, 2);
    CHECK(order.size() == 2 && order[0].video.videoId == vid(500) && order[1].video.videoId == vid(719));
    CHECK(c->applyImport(big).nothingNew());
}

// ---- IMP-04 snapshot round trip ---------------------------------------------------------

static void snapshotRoundTrip() {
    const QString dir = freshDir("snapshot-src");
    std::unique_ptr<LibraryStore> p = openStore(dbIn(dir));
    ChannelRecord a = channel(uc(1), "Alpha", 5000);
    a.avatarUrl = "https://yt3.example/a.jpg";
    p->subscribe(a);
    p->subscribe(channel(uc(2), "Beta"));
    ImportBatch handles;
    PendingHandle h;
    h.url = "https://www.youtube.com/@later";
    h.name = "Later";
    h.addedMs = 1234;
    handles.pendingHandles.push_back(h);
    p->applyImport(handles);
    p->addToPlaylist(QString::fromLatin1(watchLaterPlaylistId), entry(vid(1)));
    p->addToPlaylist(QString::fromLatin1(watchLaterPlaylistId), entry(vid(2)));
    const QString one = p->createPlaylist(QStringLiteral("One"));
    for (int i = 0; i < 3; ++i) p->addToPlaylist(one, entry(vid(10 + i)));
    p->createPlaylist(QStringLiteral("Two"));
    RemotePlaylistRef ref;
    ref.remoteId = "PLsnapshot00000001";
    ref.title = "Remote";
    ref.channel = "Owner";
    const QString m = p->saveRemotePlaylist(ref);
    std::vector<VideoEntry> five;
    for (int i = 0; i < 5; ++i) five.push_back(entry(vid(30 + i)));
    p->replaceRemoteItems(m, five, g_now);
    for (int i = 0; i < 5; ++i) {
        g_now += 1000;
        p->recordProgress(progress(vid(40 + i), (i == 2 ? 590 : 60 + i) * S), true);
    }
    p->recordProgress(progress(vid(41), 70 * S), true); // play count 2
    p->setHistoryCap(300);

    const ImportBatch snap = p->snapshot();
    CHECK(snap.source == "sailtube");
    CHECK(snap.subscriptions.size() == 2 && snap.pendingHandles.size() == 1);
    CHECK(snap.history.size() == 5 && snap.history[0].video.videoId == vid(41));
    CHECK(snap.resume.size() == 4);
    CHECK(snap.playlists.size() == 3 && snap.playlists[0].title == watchLaterPlaylistId && snap.playlists[0].items.size() == 2);
    CHECK(snap.remotePlaylists.size() == 1 && snap.remotePlaylists[0].itemCountHint == 5);

    const QString dir2 = freshDir("snapshot-dst");
    std::unique_ptr<LibraryStore> q = openStore(dbIn(dir2));
    g_now += 1000;
    const ImportSummary sum = q->applyImport(snap);
    CHECK(sum.subscriptionsAdded == 2 && sum.handlesPending == 1 && sum.historyAdded == 5 && sum.resumeAdded == 4);
    CHECK(sum.playlistsAdded == 2 && sum.playlistItemsAdded == 5 && sum.remotePlaylistsAdded == 1 && sum.playlistsSkipped == 0);
    const std::vector<ChannelRecord> sa = p->subscriptions(SubscriptionSort::Name), sb = q->subscriptions(SubscriptionSort::Name);
    bool same = sa.size() == sb.size();
    for (std::size_t i = 0; same && i < sa.size(); ++i)
        same = sa[i].channelId == sb[i].channelId && sa[i].name == sb[i].name && sa[i].avatarUrl == sb[i].avatarUrl
               && sa[i].addedMs == sb[i].addedMs && sa[i].latestUploadMs == sb[i].latestUploadMs;
    CHECK(same);
    CHECK(q->pendingHandles().size() == 1 && q->pendingHandles()[0].addedMs == 1234);
    const std::vector<HistoryRecord> ha = p->history(0, 100), hb = q->history(0, 100);
    same = ha.size() == hb.size();
    for (std::size_t i = 0; same && i < ha.size(); ++i)
        same = ha[i].video.videoId == hb[i].video.videoId && ha[i].video.title == hb[i].video.title
               && ha[i].watched == hb[i].watched && ha[i].playCount == hb[i].playCount && ha[i].fraction == hb[i].fraction
               && ha[i].positionMs == hb[i].positionMs && ha[i].watchedAtMs == hb[i].watchedAtMs
               && ha[i].durationMs == hb[i].durationMs;
    CHECK(same);
    // Local lists (watch later included) keep their order and items; mirrors come back by
    // remote id with their header (items are fetched on first open). The batch keeps
    // locals and mirrors apart, so their relative order is not part of the round trip.
    std::vector<PlaylistRecord> pa, pb, ma, mb;
    for (const PlaylistRecord &x : p->playlists()) (x.kind == PlaylistKind::RemoteMirror ? ma : pa).push_back(x);
    for (const PlaylistRecord &x : q->playlists()) (x.kind == PlaylistKind::RemoteMirror ? mb : pb).push_back(x);
    same = pa.size() == pb.size() && pa.size() == 3;
    for (std::size_t i = 0; same && i < pa.size(); ++i) {
        same = pa[i].kind == pb[i].kind && pa[i].title == pb[i].title;
        const std::vector<VideoEntry> ia = p->playlistItems(Q(pa[i].id)), ib = q->playlistItems(Q(pb[i].id));
        same = same && ia.size() == ib.size();
        for (std::size_t k = 0; same && k < ia.size(); ++k) same = ia[k].videoId == ib[k].videoId && ia[k].title == ib[k].title;
    }
    CHECK(same);
    CHECK(ma.size() == 1 && mb.size() == 1 && ma[0].remoteId == mb[0].remoteId && ma[0].title == mb[0].title
          && ma[0].channel == mb[0].channel && mb[0].itemCount == 5 && q->playlistItems(Q(mb[0].id)).empty());
    for (int i = 0; i < 5; ++i) CHECK(p->resumePositionMs(Q(vid(40 + i))) == q->resumePositionMs(Q(vid(40 + i))));
    // Re-importing a SailTube backup, into the copy or the original, adds nothing.
    CHECK(q->applyImport(snap).nothingNew());
    CHECK(p->applyImport(snap).nothingNew());
    HistoryRecord r;
    CHECK(p->historyRecord(Q(vid(41)), &r) && r.playCount == 2);
}

// ---- meta -----------------------------------------------------------------------------

static void meta() {
    const QString dir = freshDir("meta");
    {
        std::unique_ptr<LibraryStore> s = openStore(dbIn(dir));
        CHECK(s->metaValue("resume_behaviour", "always") == "always");
        s->setMetaValue("resume_behaviour", "ask");
        s->setHistoryPaused(true);
        s->setMetaValue("import/newpipe/x", "9"); // reserved prefix
        CHECK(s->metaValue("resume_behaviour") == "ask" && s->metaValue("import/newpipe/x").isEmpty());
    }
    std::unique_ptr<LibraryStore> s = openStore(dbIn(dir));
    CHECK(s->metaValue("resume_behaviour") == "ask");
    CHECK(s->metaValue("history_paused") == "1" && s->historyPaused());
}

// ---- NFR-ROB-02 / LIB-01 crash safety ---------------------------------------------------------

static void crashSafety() {
    const QString dir = freshDir("crash");
    {
        std::unique_ptr<LibraryStore> s = openStore(dbIn(dir));
        s->subscribe(channel(uc(1), "Kept"));
    }
    // A writer that never commits (closed mid-transaction) leaves the previous store.
    {
        Raw raw(dbIn(dir));
        CHECK(raw.exec("BEGIN IMMEDIATE"));
        CHECK(raw.exec("DELETE FROM subscriptions"));
        CHECK(raw.exec(QStringLiteral("INSERT INTO subscriptions(channel_id, name, added_ms) VALUES('%1', 'Torn', 1)").arg(Q(uc(2)))));
    }
    {
        std::unique_ptr<LibraryStore> s = openStore(dbIn(dir));
        CHECK(s->isOpen() && s->isSubscribed(Q(uc(1))) && !s->isSubscribed(Q(uc(2))));
    }
    // A child killed with SIGKILL in the middle of a transaction.
    std::fflush(stdout);
    pid_t pid = fork();
    if (pid == 0) {
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), QStringLiteral("child"));
        db.setDatabaseName(dbIn(dir));
        if (!db.open()) _exit(3);
        QSqlQuery q(db);
        q.exec(QStringLiteral("INSERT INTO subscriptions(channel_id, name, added_ms) VALUES('%1', 'Committed', 1)").arg(Q(uc(3))));
        q.exec(QStringLiteral("BEGIN IMMEDIATE"));
        q.exec(QStringLiteral("DELETE FROM subscriptions"));
        for (int i = 0; i < 200; ++i)
            q.exec(QStringLiteral("INSERT INTO history(video_id, watched_at_ms, seq) VALUES('%1', 1, %2)").arg(Q(vid(i))).arg(i));
        ::kill(::getpid(), SIGKILL);
        _exit(4);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    CHECK(WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL);
    {
        std::unique_ptr<LibraryStore> s = openStore(dbIn(dir));
        CHECK(s->isOpen() && s->isSubscribed(Q(uc(1))) && s->isSubscribed(Q(uc(3))) && s->historyCount() == 0);
        Raw raw(dbIn(dir));
        CHECK(raw.scalar("PRAGMA integrity_check").toString() == "ok");
    }
    // Kills at random points during store writes: every reopen parses and holds a whole state.
    std::srand(7);
    for (int round = 0; round < 5; ++round) {
        std::fflush(stdout);
        pid = fork();
        if (pid == 0) {
            LibraryStore s(dbIn(dir), QStringLiteral("child-store"));
            if (!s.open()) _exit(3);
            for (int i = 0; i < 1000000; ++i) {
                s.recordProgress(progress(vid(i % 300), (20 + i % 500) * S), i % 7 == 0);
                s.setMetaValue(QStringLiteral("counter"), QString::number(i));
                if (i % 50 == 0) s.subscribe(channel(uc(1000 + i % 200), "Churn"));
                if (i % 50 == 25) s.unsubscribe(Q(uc(1000 + (i - 25) % 200)));
            }
            _exit(0);
        }
        QThread::msleep(30 + std::rand() % 120);
        ::kill(pid, SIGKILL);
        waitpid(pid, &status, 0);
        std::unique_ptr<LibraryStore> s = openStore(dbIn(dir));
        CHECK(s->isOpen());
        CHECK(s->historyCount() <= s->historyCap());
        bool ok = false;
        s->metaValue("counter", "0").toInt(&ok);
        CHECK(ok);
        CHECK(s->isSubscribed(Q(uc(1))));
        Raw raw(dbIn(dir));
        CHECK(raw.scalar("PRAGMA integrity_check").toString() == "ok");
        CHECK(raw.scalar("PRAGMA user_version").toInt() == librarySchemaVersion);
    }
}

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    setLibraryClockForTests(fakeNow);
    const QByteArray env = qgetenv("LIBRARY_STORE_TMP");
    g_root = env.isEmpty() ? QDir::tempPath() + QStringLiteral("/sailtube-library-store-%1").arg(QCoreApplication::applicationPid())
                           : QString::fromLocal8Bit(env);
    QDir(g_root).removeRecursively();
    QDir().mkpath(g_root);
    {
        Raw probe(g_root + QStringLiteral("/probe.db"));
        std::printf("  sqlite %s\n", qPrintable(probe.scalar("SELECT sqlite_version()").toString()));
    }
    openFresh();
    schemaRefusal();
    orphanCleanup();
    subscriptions();
    playlists();
    history();
    resume();
    incremental();
    busyQueue();
    importMerge();
    snapshotRoundTrip();
    meta();
    crashSafety();
    QDir(g_root).removeRecursively();
    std::printf("library store tests: %s (%d failure%s)\n", failures ? "FAIL" : "PASS", failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
