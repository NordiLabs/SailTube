// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// NFR-ROB-02 host test (Linux): a writer process commits >= 200 updates to the three kinds of store
// SailTube uses (the SQLite LibraryStore resume position, a QSaveFile JSON file, QSettings) and is
// SIGKILLed at random points. After every kill each store opens, passes its integrity check and
// holds either the last acknowledged value or the one being written. Then a launch seeded with
// orphaned temp, spool and lock files leaves none of them (StartupCleanup).
#include "../../Core/Library/Infrastructure/LibraryStore.h"
#include "../../Core/Storage/Infrastructure/StartupCleanup.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QSettings>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <csignal>
#include <random>
#include <sys/wait.h>
#include <unistd.h>

using namespace nordtile::video;
static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++failures; } } while (0)

static const char kVideo[] = "dQw4w9WgXcQ";

static library::PlaybackProgress progress(qint64 n) {
    library::PlaybackProgress p;
    p.video.videoId = kVideo;
    p.video.title = "Kill test";
    p.video.durationSeconds = 100000;
    p.positionMs = 60000 + n * 1000;   // inside the resume window
    p.durationMs = 100000000;
    return p;
}

// Writer: continues from `start`, prints "<n>\n" after all three stores committed value n.
static int writer(const QString &dir, qint64 start) {
    library::LibraryStore store(dir + QStringLiteral("/library.db"), QStringLiteral("writer"));
    if (!store.open()) return 3;
    QSettings settings(dir + QStringLiteral("/SailTube.conf"), QSettings::IniFormat);
    for (qint64 n = start;; ++n) {
        store.recordProgress(progress(n), false);
        QSaveFile json(dir + QStringLiteral("/state.json"));
        if (!json.open(QIODevice::WriteOnly)) return 4;
        QJsonObject o; o.insert(QStringLiteral("n"), static_cast<double>(n));
        o.insert(QStringLiteral("pad"), QString(4096, QLatin1Char('x')));   // a write that takes some time
        json.write(QJsonDocument(o).toJson());
        if (!json.commit()) return 5;
        settings.setValue(QStringLiteral("n"), n);
        settings.sync();
        if (settings.status() != QSettings::NoError) return 6;
        std::printf("%lld\n", static_cast<long long>(n));
        std::fflush(stdout);
    }
}

struct Values { qint64 db = -1, json = -1, ini = -1; bool dbOk = false; };

static Values read(const QString &dir) {
    Values v;
    {
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), QStringLiteral("check"));
        db.setDatabaseName(dir + QStringLiteral("/library.db"));
        if (db.open()) {
            QSqlQuery q(db);
            v.dbOk = q.exec(QStringLiteral("PRAGMA integrity_check")) && q.next() && q.value(0).toString() == QLatin1String("ok");
        }
        db.close();
    }
    QSqlDatabase::removeDatabase(QStringLiteral("check"));
    {
        library::LibraryStore store(dir + QStringLiteral("/library.db"), QStringLiteral("reader"));
        if (store.open()) {
            const qint64 ms = store.resumePositionMs(QString::fromLatin1(kVideo));
            v.db = ms >= 60000 ? (ms - 60000) / 1000 : -1;
        }
    }
    QFile f(dir + QStringLiteral("/state.json"));
    if (f.open(QIODevice::ReadOnly)) {
        const QJsonDocument d = QJsonDocument::fromJson(f.readAll());
        if (d.isObject()) v.json = static_cast<qint64>(d.object().value(QStringLiteral("n")).toDouble(-1));
    }
    // Parse the file itself: QSettings caches a file per process and may not notice a rewrite with
    // the same size within the same second, which would test the cache, not the file.
    QFile conf(dir + QStringLiteral("/SailTube.conf"));
    if (conf.open(QIODevice::ReadOnly)) {
        for (const QByteArray &line : conf.readAll().split('\n'))
            if (line.startsWith("n=")) { bool ok = false; const qint64 n = line.mid(2).trimmed().toLongLong(&ok); if (ok) v.ini = n; }
    }
    return v;
}

static bool oldOrNew(qint64 value, qint64 acked) { return value == acked || value == acked + 1 || (acked < 0 && value == -1); }

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    if (argc == 4 && std::strcmp(argv[1], "writer") == 0)
        return writer(QString::fromLocal8Bit(argv[2]), std::atoll(argv[3]));

    QTemporaryDir tmp;
    const QString dir = tmp.path();
    std::mt19937 rng(20261108);
    qint64 acked = -1, totalWrites = 0;
    int kills = 0;
    while (totalWrites < 200) {
        int pipefd[2];
        CHECK(::pipe(pipefd) == 0);
        const pid_t child = ::fork();
        if (child == 0) {
            ::dup2(pipefd[1], 1);
            ::close(pipefd[0]);
            const QByteArray start = QByteArray::number(acked + 1);
            ::execl(argv[0], argv[0], "writer", QFile::encodeName(dir).constData(), start.constData(), static_cast<char *>(nullptr));
            ::_exit(127);
        }
        ::close(pipefd[1]);
        FILE *in = ::fdopen(pipefd[0], "r");
        // Kill after a random number of acknowledged writes, at a random moment inside the next one.
        const int target = 1 + static_cast<int>(rng() % 15);
        char line[64];
        int seen = 0;
        while (seen < target && std::fgets(line, sizeof line, in)) { acked = std::atoll(line); ++seen; }
        ::usleep(static_cast<useconds_t>(rng() % 3000));
        ::kill(child, SIGKILL);
        while (std::fgets(line, sizeof line, in)) acked = std::atoll(line);   // acks already in the pipe
        std::fclose(in);
        int status = 0;
        ::waitpid(child, &status, 0);
        CHECK(WIFSIGNALED(status));
        totalWrites += seen;
        ++kills;
        const Values v = read(dir);
        CHECK(v.dbOk);
        CHECK(oldOrNew(v.db, acked));
        CHECK(oldOrNew(v.json, acked));
        CHECK(oldOrNew(v.ini, acked));
        if (failures) { std::printf("after kill %d: acked=%lld db=%lld json=%lld ini=%lld\n", kills, static_cast<long long>(acked),
                                    static_cast<long long>(v.db), static_cast<long long>(v.json), static_cast<long long>(v.ini)); break; }
        acked = std::max(acked, std::max(v.db, std::max(v.json, v.ini)));   // the next writer continues from the newest
    }
    std::printf("robustness: %lld writes, %d SIGKILLs, every store old-or-new and intact\n", static_cast<long long>(totalWrites), kills);

    using nordtile::storage::isAtomicWriteLeftover;
    CHECK(isAtomicWriteLeftover(QStringLiteral("state.json.Ab12Cd"), QStringList()));
    CHECK(isAtomicWriteLeftover(QStringLiteral("notes.x9YzQ1"), QStringList() << QStringLiteral("notes")));
    CHECK(!isAtomicWriteLeftover(QStringLiteral("notes.x9YzQ1"), QStringList()));      // not store-like, no sibling
    CHECK(!isAtomicWriteLeftover(QStringLiteral("state.json.backup"), QStringList()));  // a word, not a temp name
    CHECK(!isAtomicWriteLeftover(QStringLiteral("state.json"), QStringList()));
    // Seeded orphans: QSaveFile/QSettings temp files, a spool file, a stale lock and a live lock.
    const QStringList seeded = {QStringLiteral("state.json.Ab12Cd"), QStringLiteral("SailTube.conf.x9YzQ1"),
                                QStringLiteral("import.tmp"), QStringLiteral("queue.spool"), QStringLiteral("SailTube.conf.lock")};
    for (const QString &name : seeded) {
        QFile f(dir + QLatin1Char('/') + name);
        CHECK(f.open(QIODevice::WriteOnly));
        f.write(name.endsWith(QLatin1String(".lock")) ? QByteArray("999999\nharbour-sailtube\nhost\n") : QByteArray("x"));
    }
    QFile live(dir + QStringLiteral("/library.db.lock"));
    CHECK(live.open(QIODevice::WriteOnly));
    live.write(QByteArray::number(static_cast<qint64>(::getpid())) + "\nharbour-sailtube\nhost\n");
    live.close();
    QFile keep(dir + QStringLiteral("/video.mp4.part"));
    CHECK(keep.open(QIODevice::WriteOnly)); keep.close();
    QFile word(dir + QStringLiteral("/state.json.backup"));
    CHECK(word.open(QIODevice::WriteOnly)); word.close();
    const nordtile::storage::CleanupReport r = nordtile::storage::removeOrphans(QStringList() << dir << dir + QStringLiteral("/missing"));
    for (const QString &name : seeded) CHECK(!QFile::exists(dir + QLatin1Char('/') + name));
    CHECK(QFile::exists(dir + QStringLiteral("/library.db.lock")));   // owned by a live process
    CHECK(QFile::exists(dir + QStringLiteral("/video.mp4.part")));    // download resume data
    CHECK(QFile::exists(dir + QStringLiteral("/state.json.backup"))); // a real file, not a temp name
    CHECK(QFile::exists(dir + QStringLiteral("/state.json")) && QFile::exists(dir + QStringLiteral("/SailTube.conf")));
    for (const QString &name : seeded) CHECK(r.removed.contains(name));   // plus leftovers the kills produced
    CHECK(r.kept == 1);
    const Values after = read(dir);
    CHECK(after.dbOk && after.json >= 0 && after.ini >= 0);
    std::printf("robustness: startup cleanup removed %d orphan(s), kept the live lock, partials and real files\n",
                static_cast<int>(r.removed.size()));
    if (failures) { std::printf("robustness: %d failure(s)\n", failures); return 1; }
    std::printf("robustness StoreKillTest: all checks passed\n");
    return 0;
}
