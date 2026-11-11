// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// The QML-facing library models: roles, edits forwarded to the store, and the
// incremental watch-state signal (LIB-06). Build: tests/library_models/run.sh
#include "Clients/Sailfish/Presentation/LibraryModels.h"
#include "Core/Library/Infrastructure/LibraryStore.h"

#include <QCoreApplication>
#include <QDir>
#include <cstdio>

using namespace nordtile::video;
using namespace nordtile::video::sailfish;

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

static qint64 fakeNow = 1700000000000LL;
static qint64 fakeClock() { return fakeNow; }

static QVariantMap video(const char *id, const char *title, double seconds = 600) {
    QVariantMap m;
    m.insert(QStringLiteral("videoId"), QString::fromLatin1(id));
    m.insert(QStringLiteral("title"), QString::fromUtf8(title));
    m.insert(QStringLiteral("channel"), QStringLiteral("Channel"));
    m.insert(QStringLiteral("channelId"), QStringLiteral("UCaaaaaaaaaaaaaaaaaaaaaa"));
    m.insert(QStringLiteral("durationSeconds"), seconds);
    m.insert(QStringLiteral("thumbnail"), QStringLiteral("https://i.ytimg.com/vi/x/mqdefault.jpg"));
    return m;
}

static int roleOf(const QAbstractItemModel &m, const char *name) {
    const QHash<int, QByteArray> roles = m.roleNames();
    for (auto it = roles.constBegin(); it != roles.constEnd(); ++it) if (it.value() == name) return it.key();
    return -1;
}

static QVariant at(const QAbstractItemModel &m, int row, const char *role) { return m.data(m.index(row, 0), roleOf(m, role)); }

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    library::setLibraryClockForTests(&fakeClock);
    const QString dir = QString::fromLocal8Bit(qgetenv("TEST_DIR")) + QStringLiteral("/models");
    QDir().mkpath(dir);
    library::LibraryStore store(dir + QStringLiteral("/library.db"), QStringLiteral("models-test"));
    CHECK(store.open());

    // map conversion round trip
    const library::VideoEntry e = videoFromMap(video("abcdefghijk", "Título", 61));
    CHECK(e.videoId == "abcdefghijk" && e.durationSeconds == 61 && e.title == "Título");
    CHECK(videoToMap(e).value(QStringLiteral("title")).toString() == QString::fromUtf8("Título"));
    CHECK(videoFromMap(QVariantMap()).durationSeconds == -1);

    // subscriptions
    SubscriptionsModel subs(&store);
    CHECK(subs.count() == 0);
    const int rev = subs.revision();
    CHECK(subs.toggle(QStringLiteral("UCbbbbbbbbbbbbbbbbbbbbbb"), QStringLiteral("Beta"), QString()));
    CHECK(subs.toggle(QStringLiteral("UCaaaaaaaaaaaaaaaaaaaaaa"), QStringLiteral("alpha"), QString()));
    CHECK(subs.count() == 2 && subs.revision() > rev);
    CHECK(at(subs, 0, "name").toString() == QStringLiteral("alpha")); // A–Z, case-insensitive
    CHECK(subs.isSubscribed(QStringLiteral("UCbbbbbbbbbbbbbbbbbbbbbb")));
    CHECK(!subs.toggle(QStringLiteral("UCbbbbbbbbbbbbbbbbbbbbbb"), QString(), QString()));
    CHECK(subs.count() == 1 && subs.channelIds() == QStringList(QStringLiteral("UCaaaaaaaaaaaaaaaaaaaaaa")));
    CHECK(!subs.subscribe(QStringLiteral("not-a-channel"), QStringLiteral("x"), QString()));

    // playlists: watch later first; add is a no-op for a duplicate
    PlaylistsModel lists(&store);
    CHECK(lists.count() >= 1 && at(lists, 0, "watchLater").toBool());
    CHECK(lists.toggleWatchLater(video("abcdefghijk", "One")));
    CHECK(lists.inWatchLater(QStringLiteral("abcdefghijk")));
    CHECK(!lists.toggleWatchLater(video("abcdefghijk", "One")));
    CHECK(!lists.inWatchLater(QStringLiteral("abcdefghijk")));
    const QString id = lists.createWith(QStringLiteral("Mix"), video("abcdefghijk", "One"));
    CHECK(!id.isEmpty() && lists.contains(id, QStringLiteral("abcdefghijk")));
    CHECK(!lists.add(id, video("abcdefghijk", "One")));
    CHECK(lists.add(id, video("bcdefghijkl", "Two")));
    CHECK(at(lists, 1, "playlistId").toString() == id); // new list right below watch later
    CHECK(at(lists, 1, "itemCount").toInt() == 2);
    QObject *opened = lists.openList(id);
    PlaylistItemsModel *items = qobject_cast<PlaylistItemsModel *>(opened);
    CHECK(items && items->count() == 2 && items->title() == QStringLiteral("Mix"));
    CHECK(items && at(*items, 0, "videoId").toString() == QStringLiteral("abcdefghijk"));
    CHECK(items && items->move(0, 1) && at(*items, 0, "videoId").toString() == QStringLiteral("bcdefghijkl"));
    CHECK(items && items->items(true).size() == 2);
    CHECK(items && items->remove(QStringLiteral("bcdefghijkl")) && items->count() == 1);
    delete opened;
    CHECK(lists.rename(id, QStringLiteral("Mix 2")));
    CHECK(!lists.rename(QStringLiteral("watch-later"), QStringLiteral("x")));
    const QString mirror = lists.saveRemote(QStringLiteral("PLxyz0123456789"), QStringLiteral("Remote"), QStringLiteral("Owner"), QString(), 12);
    CHECK(!mirror.isEmpty() && lists.mirrorFor(QStringLiteral("PLxyz0123456789")) == mirror);
    CHECK(lists.saveRemote(QStringLiteral("PLxyz0123456789"), QStringLiteral("Remote 2"), QString(), QString(), 12) == mirror);
    CHECK(lists.remove(id));

    // history + watch index (incremental)
    HistoryModel history(&store);
    WatchStateIndex watch(&store);
    QStringList spy;
    QObject::connect(&watch, &WatchStateIndex::changed, [&spy](const QString &id) { spy.append(id); });
    library::PlaybackProgress p;
    p.video = videoFromMap(video("cdefghijklm", "Three", 100));
    p.positionMs = 50000;
    p.durationMs = 100000;
    store.recordProgress(p, true);
    CHECK(spy == QStringList(QStringLiteral("cdefghijklm")));
    QVariantMap st = watch.state(QStringLiteral("cdefghijklm"));
    CHECK(st.value(QStringLiteral("showProgress")).toBool() && !st.value(QStringLiteral("watched")).toBool());
    CHECK(qAbs(st.value(QStringLiteral("progress")).toDouble() - 0.5) < 0.01);
    CHECK(history.count() == 1 && history.total() == 1);
    CHECK(at(history, 0, "title").toString() == QStringLiteral("Three"));
    history.markWatched(video("cdefghijklm", "Three", 100));
    CHECK(watch.state(QStringLiteral("cdefghijklm")).value(QStringLiteral("watched")).toBool());
    CHECK(watch.resumePosition(QStringLiteral("cdefghijklm")) < 0); // mark clears the resume point
    history.unmarkWatched(QStringLiteral("cdefghijklm"));
    CHECK(!watch.state(QStringLiteral("cdefghijklm")).value(QStringLiteral("watched")).toBool());

    // LIB-07 settings and contexts
    CHECK(!watch.hideWatched());
    history.markWatched(video("cdefghijklm", "Three", 100));
    CHECK(!watch.hidden(QStringLiteral("cdefghijklm"), QStringLiteral("feed")));
    watch.setHideWatched(true);
    CHECK(watch.hidden(QStringLiteral("cdefghijklm"), QStringLiteral("feed")));
    CHECK(watch.hidden(QStringLiteral("cdefghijklm"), QStringLiteral("channel")));
    CHECK(!watch.hidden(QStringLiteral("cdefghijklm"), QStringLiteral("search")));
    watch.setHideWatchedInSearch(true);
    CHECK(watch.hidden(QStringLiteral("cdefghijklm"), QStringLiteral("search")));
    CHECK(!watch.hidden(QStringLiteral("cdefghijklm"), QStringLiteral("other")));

    // paging: 40 per page
    for (int i = 0; i < 45; ++i) {
        library::PlaybackProgress q;
        q.video.videoId = QStringLiteral("v%1").arg(i, 10, 10, QLatin1Char('0')).toStdString();
        q.positionMs = 20000;
        q.durationMs = 600000;
        ++fakeNow;
        store.recordProgress(q, true);
    }
    CHECK(history.total() == 46);
    CHECK(history.count() == HistoryModel::pageSize);
    CHECK(history.canFetchMore(QModelIndex()));
    history.fetchMore(QModelIndex());
    CHECK(history.count() == 46 && !history.canFetchMore(QModelIndex()));
    CHECK(at(history, 0, "videoId").toString() == QStringLiteral("v0000000044")); // newest first
    // Incremental updates: the Library tab's History shelf stays attached while a video plays, and
    // the store reports every progress save. The usual changes never reset the model (a reset
    // rebuilds the tiles and throws the scroll position away), and the rows always equal the
    // store's newest page.
    {
        HistoryModel shelf(&store);
        CHECK(shelf.count() == HistoryModel::pageSize && shelf.total() == 46);
        struct Counts { int reset = 0, inserted = 0, removed = 0, moved = 0, changed = 0; } c;
        QObject::connect(&shelf, &QAbstractItemModel::modelReset, [&c]() { ++c.reset; });
        QObject::connect(&shelf, &QAbstractItemModel::rowsInserted, [&c](const QModelIndex &, int a, int b) { c.inserted += b - a + 1; });
        QObject::connect(&shelf, &QAbstractItemModel::rowsRemoved, [&c](const QModelIndex &, int a, int b) { c.removed += b - a + 1; });
        QObject::connect(&shelf, &QAbstractItemModel::rowsMoved, [&c](const QModelIndex &, int a, int b, const QModelIndex &, int) { c.moved += b - a + 1; });
        QObject::connect(&shelf, &QAbstractItemModel::dataChanged, [&c](const QModelIndex &a, const QModelIndex &b) { c.changed += b.row() - a.row() + 1; });
        const auto matchesStore = [&]() {
            const std::vector<library::HistoryRecord> want = store.history(0, shelf.count());
            if (static_cast<int>(want.size()) != shelf.count()) return false;
            for (int i = 0; i < shelf.count(); ++i) {
                const library::HistoryRecord &h = want[static_cast<std::size_t>(i)];
                if (at(shelf, i, "videoId").toString().toStdString() != h.video.videoId) return false;
                if (at(shelf, i, "fraction").toDouble() != h.fraction || at(shelf, i, "watched").toBool() != h.watched) return false;
            }
            return true;
        };
        const auto watch = [&](const QString &id, qint64 positionMs, bool newSession) {
            library::PlaybackProgress q;
            q.video.videoId = id.toStdString();
            q.positionMs = positionMs;
            q.durationMs = 600000;
            ++fakeNow;
            store.recordProgress(q, newSession);
        };
        const auto only = [&c](int inserted, int removed, int moved, int changed) {
            const bool ok = c.reset == 0 && c.inserted == inserted && c.removed == removed && c.moved == moved && c.changed == changed;
            if (!ok) std::printf("  history updates: reset %d inserted %d removed %d moved %d changed %d\n", c.reset, c.inserted, c.removed, c.moved, c.changed);
            c = Counts();
            return ok;
        };

        // A progress save of the video on top (every 30 s while it plays): one row re-announced.
        watch(QStringLiteral("v0000000044"), 300000, false);
        CHECK(only(0, 0, 0, 1));
        CHECK(qAbs(at(shelf, 0, "fraction").toDouble() - 0.5) < 0.01 && matchesStore());
        // An older video of the window is watched again: it moves to the front.
        CHECK(at(shelf, 34, "videoId").toString() == QStringLiteral("v0000000010"));
        watch(QStringLiteral("v0000000010"), 20000, true);
        CHECK(only(0, 0, 1, 1));
        CHECK(at(shelf, 0, "videoId").toString() == QStringLiteral("v0000000010") && matchesStore());
        // A new video: inserted at the front, the last row leaves the 40-row window.
        watch(QStringLiteral("newvideo001"), 20000, true);
        CHECK(only(1, 1, 0, 0));
        CHECK(shelf.count() == HistoryModel::pageSize && shelf.total() == 47 && matchesStore());
        // A video from beyond the window is watched again: the same shape from the model's side.
        watch(QStringLiteral("v0000000001"), 20000, true);
        CHECK(only(1, 1, 0, 0));
        CHECK(at(shelf, 0, "videoId").toString() == QStringLiteral("v0000000001") && matchesStore());
        // One entry removed from the middle: its row goes, the window refills at the end.
        shelf.remove(QStringLiteral("v0000000030"));
        CHECK(only(1, 1, 0, 0));
        CHECK(shelf.count() == HistoryModel::pageSize && shelf.total() == 46 && matchesStore());
        // Marking watched changes one row's values only.
        shelf.markWatched(video("v0000000040", "", 600));
        CHECK(c.reset == 0 && c.inserted == 0 && c.removed == 0 && matchesStore());
    }

    history.setCap(100);
    CHECK(history.cap() == 100);
    history.setPaused(true);
    CHECK(history.paused());
    history.clear();
    CHECK(history.total() == 0 && history.count() == 0);

    std::printf("library models tests: %s (%d failures)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
