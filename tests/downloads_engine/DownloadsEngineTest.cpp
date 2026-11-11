// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Offline engine tests (NFR-TST-02) of DL-02/03/05/06/07/08/09 through
// DownloadManager: a fake media host on QTcpServer 127.0.0.1 (Range/206 and
// faults), a fake resolver and a fake remuxer. Build: tests/downloads_engine/run.sh
#include "Core/Downloads/Domain/FileNaming.h"
#include "Core/Downloads/Infrastructure/DownloadManager.h"
#include "Core/Library/Infrastructure/LibrarySchema.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QPointer>
#include <QProcess>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryFile>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>
#include <cstdio>
#include <functional>
#include <memory>

using namespace nordtile::video;
using namespace nordtile::video::downloads;

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

static bool waitFor(const std::function<bool()> &cond, int timeoutMs = 15000) {
    QElapsedTimer t;
    t.start();
    while (!cond()) {
        if (t.elapsed() > timeoutMs) return false;
        QEventLoop loop;
        QTimer::singleShot(5, &loop, SLOT(quit()));
        loop.exec();
    }
    return true;
}

static void spin(int ms) {
    QEventLoop loop;
    QTimer::singleShot(ms, &loop, SLOT(quit()));
    loop.exec();
}

static QByteArray pattern(int size, int seed) {
    QByteArray b(size, '\0');
    for (int i = 0; i < size; ++i) b[i] = static_cast<char>((i * 7 + seed * 13 + (i >> 8)) & 0xFF);
    return b;
}

static QByteArray sha(const QByteArray &b) { return QCryptographicHash::hash(b, QCryptographicHash::Sha256); }

static QByteArray readFile(const QString &path) {
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

// ---- fake media host -------------------------------------------------------------

struct Hit {
    QString name;
    int tok = 0;
    qint64 start = -1;
    qint64 end = -1;
    int status = 0; // -1 parked
    bool cookie = false;
    QByteArray userAgent;
    QByteArray acceptEncoding;
    bool ranged = false;
};

struct Parked {
    QPointer<QTcpSocket> socket;
    QString name;
    int tok = 0;
    qint64 start = 0;
    qint64 end = 0;
};

class FakeServer {
public:
    QTcpServer server;
    QHash<QString, QByteArray> media;
    QHash<QString, QByteArray> contentTypes;   // by resource; default application/octet-stream   // /videoplayback/<name>
    QHash<QString, QByteArray> thumbs;  // /thumb/<name>
    QList<Hit> hits;
    QSet<QString> young403;             // first request at byte 0 gets 403 (once)
    QHash<QString, qint64> dropAt;      // close the connection after this byte (once)
    QHash<QString, qint64> forbidFrom;  // tok=1 requests starting at or after N get 403
    bool hold = false;
    qint64 holdFrom = 0;
    QList<Parked> parked;
    int inflight = 0;
    int maxInflight = 0;
    std::function<void(const Hit &)> onServed;

    bool start() {
        if (!server.listen(QHostAddress::LocalHost, 0)) return false;
        QObject::connect(&server, &QTcpServer::newConnection, [this]() {
            while (QTcpSocket *s = server.nextPendingConnection()) {
                QObject::connect(s, &QTcpSocket::readyRead, [this, s]() { onRead(s); });
                QObject::connect(s, &QTcpSocket::disconnected, [this, s]() { countDown(s); s->deleteLater(); });
            }
        });
        return true;
    }
    QString base() const { return QStringLiteral("http://127.0.0.1:%1").arg(server.serverPort()); }
    int mediaHits(const QString &name) const {
        int n = 0;
        for (const Hit &h : hits) if (h.name == name) ++n;
        return n;
    }
    bool sawRange(const QString &name, qint64 start, int fromIndex = 0) const {
        for (int i = fromIndex; i < hits.size(); ++i)
            if (hits[i].name == name && hits[i].start == start) return true;
        return false;
    }
    void release() {
        hold = false;
        const QList<Parked> list = parked;
        parked.clear();
        for (const Parked &p : list) {
            if (p.socket && p.socket->state() == QAbstractSocket::ConnectedState) serve(p.socket, p.name, p.tok, p.start, p.end, 206);
            else if (p.socket) countDown(p.socket);
        }
    }

private:
    void countDown(QTcpSocket *s) {
        if (s->property("counted").toBool()) {
            s->setProperty("counted", false);
            --inflight;
        }
    }
    void finish(QTcpSocket *s) {
        countDown(s);
        s->flush();
        s->disconnectFromHost();
    }
    void simple(QTcpSocket *s, int status, const char *reason, const QByteArray &body = QByteArray()) {
        s->write(QByteArray("HTTP/1.1 ") + QByteArray::number(status) + " " + reason + "\r\nContent-Length: "
                 + QByteArray::number(body.size()) + "\r\nConnection: close\r\n\r\n" + body);
        finish(s);
    }
    void onRead(QTcpSocket *s) {
        if (s->property("handled").toBool()) { s->readAll(); return; }
        QByteArray buf = s->property("buf").toByteArray() + s->readAll();
        const int endOfHead = buf.indexOf("\r\n\r\n");
        if (endOfHead < 0) { s->setProperty("buf", buf); return; }
        s->setProperty("handled", true);
        const QList<QByteArray> lines = buf.left(endOfHead).split('\n');
        const QList<QByteArray> request = lines.value(0).trimmed().split(' ');
        const QUrl url(QStringLiteral("http://x") + QString::fromLatin1(request.value(1)));
        QByteArray range, agent, encoding;
        bool cookie = false;
        for (const QByteArray &l : lines) {
            const QByteArray lower = l.toLower();
            if (lower.startsWith("range:")) range = l.mid(6).trimmed();
            if (lower.startsWith("user-agent:")) agent = l.mid(11).trimmed();
            if (lower.startsWith("accept-encoding:")) encoding = l.mid(16).trimmed();
            if (lower.startsWith("cookie:")) cookie = true;
        }
        const QString path = url.path();
        if (path.startsWith(QLatin1String("/thumb/"))) {
            const QString name = path.mid(7);
            if (thumbs.contains(name)) simple(s, 200, "OK", thumbs.value(name));
            else simple(s, 404, "Not Found");
            return;
        }
        const QString name = path.mid(QStringLiteral("/videoplayback/").size());
        const int tok = QUrlQuery(url).queryItemValue(QStringLiteral("tok")).toInt();
        s->setProperty("counted", true);
        maxInflight = qMax(maxInflight, ++inflight);
        Hit h;
        h.name = name; h.tok = tok;
        h.cookie = cookie; h.userAgent = agent; h.acceptEncoding = encoding; h.ranged = !range.isEmpty();
        const QByteArray d = media.value(name);
        bool ranged = false;
        if (range.startsWith("bytes=")) {
            const QList<QByteArray> ab = range.mid(6).split('-');
            h.start = ab.value(0).toLongLong();
            h.end = ab.value(1).isEmpty() ? d.size() - 1 : ab.value(1).toLongLong();
            ranged = true;
        } else {
            h.start = 0; h.end = d.size() - 1;
        }
        if (!media.contains(name)) { h.status = 404; log(h); simple(s, 404, "Not Found"); return; }
        if (young403.contains(name) && h.start == 0) { young403.remove(name); h.status = 403; log(h); simple(s, 403, "Forbidden"); return; }
        if (forbidFrom.contains(name) && tok == 1 && h.start >= forbidFrom.value(name)) {
            h.status = 403; log(h); simple(s, 403, "Forbidden"); return;
        }
        if (h.start >= d.size()) { h.status = 416; log(h); simple(s, 416, "Range Not Satisfiable"); return; }
        h.end = qMin<qint64>(h.end, d.size() - 1);
        if (hold && h.start >= holdFrom) {
            Parked p;
            p.socket = s; p.name = name; p.tok = tok; p.start = h.start; p.end = h.end;
            parked.append(p);
            h.status = -1;
            hits.append(h);
            return;
        }
        serve(s, name, tok, h.start, h.end, ranged ? 206 : 200, &h);
    }
    void log(const Hit &h) {
        hits.append(h);
        if (onServed) onServed(h);
    }
    void serve(QTcpSocket *s, const QString &name, int tok, qint64 start, qint64 end, int status, Hit *logged = nullptr) {
        const QByteArray d = media.value(name);
        const QByteArray body = d.mid(static_cast<int>(start), static_cast<int>(end - start + 1));
        QByteArray head = status == 206 ? QByteArray("HTTP/1.1 206 Partial Content\r\n") : QByteArray("HTTP/1.1 200 OK\r\n");
        if (status == 206)
            head += "Content-Range: bytes " + QByteArray::number(start) + "-" + QByteArray::number(end) + "/"
                    + QByteArray::number(d.size()) + "\r\n";
        head += "Content-Type: " + contentTypes.value(name, "application/octet-stream") + "\r\nContent-Length: " + QByteArray::number(body.size())
                + "\r\nConnection: close\r\n\r\n";
        Hit h;
        if (logged) h = *logged;
        h.name = name; h.tok = tok; h.start = start; h.end = end; h.status = status;
        if (logged) log(h);
        else if (onServed) onServed(h);
        if (dropAt.contains(name) && start <= dropAt.value(name) && dropAt.value(name) < end) {
            const qint64 drop = dropAt.take(name);
            s->write(head + body.left(static_cast<int>(drop - start)));
            finish(s);
            return;
        }
        s->write(head + body);
        finish(s);
    }
};

// ---- fake resolver ---------------------------------------------------------------------

struct FakeTrack {
    std::string id;
    std::string container;
    std::string vcodec;
    std::string acodec;
    int height = 0;
    QString name;    // server resource
    int length = 0;
};

struct FakeVideo {
    QString title;
    std::vector<FakeTrack> tracks;
    bool clenChangesOnReResolve = false;
    qint64 expireAfterSeconds = 0;  // 0: no expire parameter
    QString host;                   // replaces 127.0.0.1 in the URLs (allowlist test)
    resolver::ErrorCode error = resolver::ErrorCode::None;
};

class FakeResolver final : public DownloadResolver {
public:
    FakeServer *server = nullptr;
    std::function<qint64()> clock;
    QHash<QString, FakeVideo> videos;
    QHash<QString, int> calls;
    QObject context;
    QSet<quint64> cancelled;
    quint64 next = 1;

    quint64 resolve(const QString &videoId, Completion completion) override {
        const quint64 id = next++;
        const int gen = ++calls[videoId];
        QTimer::singleShot(0, &context, [this, id, videoId, gen, completion]() {
            if (cancelled.remove(id)) return;
            ResolvedVideo v;
            v.resolvedAtMs = clock();
            if (!videos.contains(videoId)) { completion(resolver::ErrorCode::VideoUnavailable, v); return; }
            const FakeVideo &fv = videos[videoId];
            if (fv.error != resolver::ErrorCode::None) { completion(fv.error, v); return; }
            v.title = fv.title;
            for (const FakeTrack &t : fv.tracks) {
                media::MediaFormat f;
                f.id = t.id; f.container = t.container; f.protocol = media::Protocol::Https;
                f.videoCodecTag = t.vcodec; f.audioCodecTag = t.acodec;
                f.videoCodec = media::parseVideoCodec(t.vcodec); f.audioCodec = media::parseAudioCodec(t.acodec);
                f.height = t.height; f.totalKbps = 100; f.audioKbps = t.height ? 0 : 128;
                const int clen = (gen > 1 && fv.clenChangesOnReResolve) ? t.length + 1 : t.length;
                QString base = server->base();
                if (!fv.host.isEmpty()) base.replace(QStringLiteral("127.0.0.1"), fv.host);
                QString url = base + QStringLiteral("/videoplayback/") + t.name
                    + QStringLiteral("?tok=%1&clen=%2").arg(gen).arg(clen);
                if (fv.expireAfterSeconds) url += QStringLiteral("&expire=%1").arg(clock() / 1000 + fv.expireAfterSeconds);
                f.url = url.toStdString();
                f.headers["User-Agent"] = "SailTubeTest/1";
                f.headers["Cookie"] = "must-not-be-sent=1";
                v.formats.push_back(f);
            }
            completion(resolver::ErrorCode::None, v);
        });
        return id;
    }
    void cancel(quint64 id) override { cancelled.insert(id); }
};

// ISO BMFF boxes: size, type, payload.
static QByteArray box(const char *type, const QByteArray &payload) {
    QByteArray b;
    const quint32 size = static_cast<quint32>(8 + payload.size());
    b.append(char(size >> 24)); b.append(char(size >> 16)); b.append(char(size >> 8)); b.append(char(size));
    b.append(type, 4);
    return b + payload;
}
// What YouTube's DASH audio (itag 140) looks like: ftyp "dash", moov, sidx, moof/mdat pairs.
static QByteArray dashM4a(int fragments) {
    QByteArray f = box("ftyp", QByteArray("dash\0\0\0\0iso6mp41", 16)) + box("moov", pattern(300, 3)) + box("sidx", pattern(40, 4));
    for (int i = 0; i < fragments; ++i) f += box("moof", pattern(100, 10 + i)) + box("mdat", pattern(5000, 20 + i));
    return f;
}
static QByteArray flatM4a() { return box("ftyp", QByteArray("M4A \0\0\0\0isom", 12)) + box("moov", pattern(300, 3)) + box("mdat", pattern(20000, 5)); }

class FakeRemuxer final : public Remuxer {
public:
    bool available = true;
    bool succeed = true;
    int calls = 0;
    QString lastVideo, lastAudio;
    QObject context;
    MuxerAvailability availability() override {
        MuxerAvailability a;
        a.loaded = available; a.mp4 = available; a.webm = available;
        return a;
    }
    quint64 remux(const QString &video, const QString &audio, const QString &output, MuxContainer, Completion done) override {
        ++calls;
        lastVideo = video;
        lastAudio = audio;
        const bool ok = succeed;
        QTimer::singleShot(0, &context, [video, audio, output, ok, done]() {
            if (!ok) { done(false, QStringLiteral("mux failed")); return; }
            QFile out(output + QStringLiteral(".part"));
            out.open(QIODevice::WriteOnly);
            if (video.isEmpty() || audio.isEmpty()) {
                out.write(flatM4a()); // one-track flatten: a flat MP4
            } else {
                out.write(readFile(video));
                out.write(readFile(audio));
            }
            out.close();
            QFile::remove(output);
            QFile::rename(output + QStringLiteral(".part"), output);
            done(true, QString());
        });
        return static_cast<quint64>(calls);
    }
    void cancel(quint64) override {}
};

// ---- environment --------------------------------------------------------------------------

static qint64 clockOffsetMs = 0;
static bool unmeteredNow = true;
static qint64 freeBytesNow = -1;   // -1: the engine's own QStorageInfo answer
static int connectionSerial = 0;

struct Env {
    QString base;
    QString db;
    QString appData;
};

static Env makeEnv(const char *name) {
    Env e;
    const QString tmp = QDir::fromNativeSeparators(QString::fromLocal8Bit(qgetenv("TMPDIR")));
    e.base = (tmp.isEmpty() ? QDir::tempPath() : tmp) + QStringLiteral("/dl-engine-%1-%2").arg(QLatin1String(name)).arg(QCoreApplication::applicationPid());
    QDir(e.base).removeRecursively();
    QDir().mkpath(e.base);
    e.db = e.base + QStringLiteral("/library.db");
    e.appData = e.base + QStringLiteral("/appdata");
    {
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), QStringLiteral("setup"));
        db.setDatabaseName(e.db);
        db.open();
        QSqlQuery q(db);
        q.exec(QStringLiteral("PRAGMA journal_mode=WAL"));
        for (const QString &sql : library::librarySchemaStep(0)) q.exec(sql);
        q.exec(QStringLiteral("PRAGMA user_version=1"));
        db.close();
    }
    QSqlDatabase::removeDatabase(QStringLiteral("setup"));
    return e;
}

static std::unique_ptr<DownloadManager> makeManager(const Env &e, FakeResolver *resolver, Remuxer *remuxer) {
    DownloadManagerConfig c;
    c.databasePath = e.db;
    c.connectionName = QStringLiteral("dl-%1").arg(++connectionSerial);
    c.roots.appData = e.appData;
    c.roots.userFolders = QStringList() << e.base + QStringLiteral("/Videos");
    c.roots.removableRoot = e.base + QStringLiteral("/media");
    c.roots.refuseTemporary = false; // TMPDIR lives under /tmp in the build container
    c.progressIntervalMs = 50;
    c.engine.chunkBytes = 64 * 1024;
    c.engine.youngTokenDelayMs = 50;
    c.engine.transientDelayMs = 50;
    c.engine.stallTimeoutMs = 10000;
    c.engine.progressPersistMs = 100000; // only the 4 MiB rule or forced writes
    const auto local = [](const QUrl &u) { return u.scheme() == QLatin1String("http") && u.host() == QLatin1String("127.0.0.1"); };
    c.engine.mediaHostAllowed = local;
    c.engine.thumbnailHostAllowed = local;
    c.engine.unmetered = []() { return unmeteredNow; };
    c.engine.freeBytes = [](const QString &dir) { return freeBytesNow >= 0 ? freeBytesNow : EngineOptions::defaultFreeBytes(dir); };
    c.engine.clockMs = []() { return QDateTime::currentMSecsSinceEpoch() + clockOffsetMs; };
    std::unique_ptr<DownloadManager> m(new DownloadManager(c, resolver, remuxer));
    QString error;
    if (!m->open(&error)) std::printf("open failed: %s\n", qPrintable(error));
    return m;
}

static qint64 storedBytes(const Env &e, int id) {
    qint64 v = -1;
    {
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), QStringLiteral("peek"));
        db.setDatabaseName(e.db);
        db.open();
        QSqlQuery q(db);
        q.prepare(QStringLiteral("SELECT audio_bytes_done FROM downloads WHERE id=?"));
        q.addBindValue(id);
        if (q.exec() && q.next()) v = q.value(0).toLongLong();
        db.close();
    }
    QSqlDatabase::removeDatabase(QStringLiteral("peek"));
    return v;
}

static QVariantMap hints(const QString &id, const QString &title, const QString &thumb = QString()) {
    QVariantMap h;
    h.insert(QStringLiteral("videoId"), id);
    h.insert(QStringLiteral("title"), title);
    h.insert(QStringLiteral("channel"), QStringLiteral("Channel"));
    h.insert(QStringLiteral("durationSeconds"), 61);
    if (!thumb.isEmpty()) h.insert(QStringLiteral("thumbnailUrl"), thumb);
    return h;
}

static FakeTrack audioTrack(const QString &name, int length, const char *itag = "140") {
    FakeTrack t;
    t.id = itag; t.container = "m4a"; t.vcodec = "none"; t.acodec = "mp4a.40.2"; t.name = name; t.length = length;
    return t;
}

static FakeTrack videoTrack(const QString &name, int length) {
    FakeTrack t;
    t.id = "136"; t.container = "mp4"; t.vcodec = "avc1.4D401F"; t.acodec = "none"; t.height = 720; t.name = name; t.length = length;
    return t;
}

static int stateOf(DownloadManager &m, int id) { return m.record(id).value(QStringLiteral("state"), -1).toInt(); }

static bool noPartFiles(const QString &dir) {
    return QDir(dir).entryList(QStringList() << QStringLiteral("*.part"), QDir::Files).isEmpty();
}

struct Rig {
    FakeServer server;
    FakeResolver resolver;
    FakeRemuxer remuxer;
    Rig() {
        server.start();
        resolver.server = &server;
        resolver.clock = []() { return QDateTime::currentMSecsSinceEpoch() + clockOffsetMs; };
    }
    void addAudio(const QString &id, const QString &title, int length, int seed) {
        const QString name = QStringLiteral("a-") + id;
        server.media.insert(name, pattern(length, seed));
        FakeVideo v;
        v.title = title;
        v.tracks.push_back(audioTrack(name, length));
        resolver.videos.insert(id, v);
    }
    QByteArray bytes(const QString &id, const char *prefix = "a-") const { return server.media.value(QLatin1String(prefix) + id); }
};

// ---- scenarios ---------------------------------------------------------------------------

// Audio end to end: DL-05 name, checksum, no .part, thumbnail (DL-07), localMedia,
// cookies never sent, the format's headers used, file-missing handling.
static void audioEndToEnd() {
    Rig rig;
    Env env = makeEnv("audio");
    const QString id = QStringLiteral("aaaaaaaaaa1");
    const QString title = QString::fromUtf8("../Tr\xC3\xBC" "e: %(title)s \xE6\x97\xA5\xE6\x9C\xAC / mix");
    rig.addAudio(id, QString(), 200000, 1);
    QByteArray jpeg("\xFF\xD8\xFF\xE0", 4);
    jpeg += pattern(2000, 9);
    rig.server.thumbs.insert(QStringLiteral("t1.jpg"), jpeg);
    std::unique_ptr<DownloadManager> m = makeManager(env, &rig.resolver, &rig.remuxer);
    int progressSignals = 0;
    QObject::connect(m.get(), &DownloadManager::progressChanged, [&progressSignals](int) { ++progressSignals; });
    const int dl = m->enqueue(hints(id, title, rig.server.base() + QStringLiteral("/thumb/t1.jpg")), 0);
    CHECK(dl > 0);
    CHECK(m->enqueue(hints(id, title), 0) == dl); // same id + kind: no duplicate
    CHECK(m->state(id) == static_cast<int>(State::Queued));
    CHECK(m->keepAliveWanted());
    CHECK(waitFor([&]() { return stateOf(*m, dl) == static_cast<int>(State::Done); }));
    CHECK(waitFor([&]() { return !m->record(dl).value(QStringLiteral("thumbnailPath")).toString().isEmpty(); }));
    const QString dir = env.appData + QStringLiteral("/downloads");
    const QString expected = QString::fromStdString(fileName(baseName(title.toStdString(), id.toStdString()), "m4a"));
    CHECK(expected == QString::fromUtf8("Tr\xC3\xBC" "e %(title)s \xE6\x97\xA5\xE6\x9C\xAC mix [aaaaaaaaaa1].m4a"));
    const QString path = dir + QLatin1Char('/') + expected;
    CHECK(QFileInfo(path).size() == 200000);
    CHECK(sha(readFile(path)) == sha(rig.bytes(id)));
    CHECK(noPartFiles(dir));
    CHECK(QDir(dir).entryList(QDir::Files).size() == 2); // file + thumbnail, nothing else
    CHECK(rig.server.mediaHits(QStringLiteral("a-") + id) == 4); // 3 full 64 KiB chunks + the rest
    CHECK(rig.server.sawRange(QStringLiteral("a-") + id, 196608) && rig.server.hits.last().end == 199999);
    for (const Hit &h : rig.server.hits) {
        CHECK(!h.cookie);                                  // never cookies, even when a format lists one
        CHECK(h.userAgent == "SailTubeTest/1");            // the format's own headers
        CHECK(h.acceptEncoding == "identity" && h.ranged); // bounded Range chunks, no transfer encoding
    }
    CHECK(waitFor([&]() { return progressSignals >= 1; })); // throttled: arrives within one interval
    CHECK(!m->keepAliveWanted() && m->activeCount() == 0);
    CHECK(m->fraction(id) == 1.0);

    const QVariantMap local = m->localMedia(id);
    CHECK(local.value(QStringLiteral("available")).toBool());
    CHECK(local.value(QStringLiteral("kind")).toString() == QLatin1String("audio"));
    CHECK(local.value(QStringLiteral("layout")).toString() == QLatin1String("single"));
    CHECK(local.value(QStringLiteral("audioPath")).toString() == path);
    CHECK(local.value(QStringLiteral("filePath")).toString() == path);
    CHECK(local.value(QStringLiteral("thumbnailPath")).toString() == dir + QLatin1Char('/')
          + QString::fromStdString(thumbnailFileName(baseName(title.toStdString(), id.toStdString()))));
    CHECK(readFile(local.value(QStringLiteral("thumbnailPath")).toString()) == jpeg);
    CHECK(m->localMedia(QStringLiteral("zzzzzzzzzzz")).value(QStringLiteral("available")).toBool() == false);

    // A vanished file: not available, the item turns Failed with file_missing (kept, retryable).
    QFile::remove(path);
    const QVariantMap gone = m->localMedia(id);
    CHECK(!gone.value(QStringLiteral("available")).toBool());
    CHECK(gone.value(QStringLiteral("error")).toString() == QLatin1String("file_missing"));
    CHECK(stateOf(*m, dl) == static_cast<int>(State::Failed));
    CHECK(m->record(dl).value(QStringLiteral("error")).toString() == QLatin1String("file_missing"));
    // Re-enqueueing a failed item retries it.
    CHECK(m->enqueue(hints(id, title), 0) == dl);
    CHECK(waitFor([&]() { return stateOf(*m, dl) == static_cast<int>(State::Done); }));
    CHECK(sha(readFile(path)) == sha(rig.bytes(id)));
    // Remove with files.
    CHECK(m->remove(dl, true));
    CHECK(!QFileInfo(path).exists() && m->state(id) == -1 && m->count() == 0);
    CHECK(QDir(dir).entryList(QDir::Files).isEmpty());
}

// Young token, truncation, 403 later + re-resolve (same and different clen), expiry.
static void resumeFaults() {
    Rig rig;
    Env env = makeEnv("faults");
    std::unique_ptr<DownloadManager> m = makeManager(env, &rig.resolver, &rig.remuxer);
    const QString dir = env.appData + QStringLiteral("/downloads");

    // 403 at byte 0 right after the resolve: retried on the same URL, no re-resolve.
    const QString young = QStringLiteral("young000001");
    rig.addAudio(young, QStringLiteral("Young"), 100000, 2);
    rig.server.young403.insert(QStringLiteral("a-") + young);
    int a = m->enqueue(hints(young, QString()), 0);
    CHECK(waitFor([&]() { return stateOf(*m, a) == static_cast<int>(State::Done); }));
    CHECK(rig.resolver.calls.value(young) == 1);
    CHECK(rig.server.hits.value(0).status == 403 && rig.server.hits.value(1).start == 0 && rig.server.hits.value(1).tok == 1);
    CHECK(sha(readFile(dir + QStringLiteral("/Young [young000001].m4a"))) == sha(rig.bytes(young)));

    // Connection closed mid-chunk: reopened exactly at the cut.
    const QString cut = QStringLiteral("cut00000001");
    rig.addAudio(cut, QStringLiteral("Cut"), 150000, 3);
    rig.server.dropAt.insert(QStringLiteral("a-") + cut, 30000);
    int mark = rig.server.hits.size();
    a = m->enqueue(hints(cut, QString()), 0);
    CHECK(waitFor([&]() { return stateOf(*m, a) == static_cast<int>(State::Done); }));
    CHECK(rig.server.sawRange(QStringLiteral("a-") + cut, 30000, mark));
    CHECK(sha(readFile(dir + QStringLiteral("/Cut [cut00000001].m4a"))) == sha(rig.bytes(cut)));
    CHECK(rig.resolver.calls.value(cut) == 1);

    // 403 after 64 KiB (an expired token): re-resolve the same itag, same clen -> continue.
    const QString later = QStringLiteral("later000001");
    rig.addAudio(later, QStringLiteral("Later"), 180000, 4);
    rig.server.forbidFrom.insert(QStringLiteral("a-") + later, 65536);
    mark = rig.server.hits.size();
    a = m->enqueue(hints(later, QString()), 0);
    CHECK(waitFor([&]() { return stateOf(*m, a) == static_cast<int>(State::Done); }));
    CHECK(rig.resolver.calls.value(later) == 2);
    bool continued = false;
    for (int i = mark; i < rig.server.hits.size(); ++i)
        if (rig.server.hits[i].name == QStringLiteral("a-") + later && rig.server.hits[i].tok == 2) {
            continued = rig.server.hits[i].start == 65536;
            break;
        }
    CHECK(continued);
    const QByteArray laterFile = readFile(dir + QStringLiteral("/Later [later000001].m4a"));
    CHECK(laterFile.size() == 180000 && sha(laterFile) == sha(rig.bytes(later)));

    // Same, but the re-resolved clen differs: the item fails with size_mismatch.
    const QString mism = QStringLiteral("mismatch001");
    rig.addAudio(mism, QStringLiteral("Mismatch"), 180000, 5);
    rig.resolver.videos[mism].clenChangesOnReResolve = true;
    rig.server.forbidFrom.insert(QStringLiteral("a-") + mism, 65536);
    a = m->enqueue(hints(mism, QString()), 0);
    CHECK(waitFor([&]() { return stateOf(*m, a) == static_cast<int>(State::Failed); }));
    CHECK(m->record(a).value(QStringLiteral("error")).toString() == QLatin1String("size_mismatch"));
    CHECK(!QFileInfo(dir + QStringLiteral("/Mismatch [mismatch001].m4a")).exists());

    // The `expire` parameter passes mid-transfer: re-resolve before the next chunk, then continue.
    const QString exp = QStringLiteral("expire00001");
    rig.addAudio(exp, QStringLiteral("Expire"), 150000, 6);
    rig.resolver.videos[exp].expireAfterSeconds = 600;
    const QString expName = QStringLiteral("a-") + exp;
    rig.server.onServed = [expName](const Hit &h) {
        if (h.name == expName && h.start == 0 && h.tok == 1) clockOffsetMs += 3600 * 1000;
    };
    mark = rig.server.hits.size();
    a = m->enqueue(hints(exp, QString()), 0);
    CHECK(waitFor([&]() { return stateOf(*m, a) == static_cast<int>(State::Done); }));
    rig.server.onServed = nullptr;
    CHECK(rig.resolver.calls.value(exp) == 2);
    bool tok1Second = false, tok2Second = false;
    for (int i = mark; i < rig.server.hits.size(); ++i) {
        const Hit &h = rig.server.hits[i];
        if (h.name != expName || h.start != 65536) continue;
        if (h.tok == 1) tok1Second = true;
        if (h.tok == 2) tok2Second = true;
    }
    CHECK(!tok1Second && tok2Second); // the expired URL was never used again
    CHECK(sha(readFile(dir + QStringLiteral("/Expire [expire00001].m4a"))) == sha(rig.bytes(exp)));
    clockOffsetMs = 0;

    // Resolver failure is stored as a code.
    const QString bad = QStringLiteral("private0001");
    FakeVideo pv; pv.error = resolver::ErrorCode::PrivateVideo;
    rig.resolver.videos.insert(bad, pv);
    a = m->enqueue(hints(bad, QString()), 0);
    CHECK(waitFor([&]() { return stateOf(*m, a) == static_cast<int>(State::Failed); }));
    CHECK(m->record(a).value(QStringLiteral("error")).toString() == QLatin1String("resolve_private_video"));

    // A URL outside the allowlist is never fetched (NFR-SEC-05).
    const QString off = QStringLiteral("offlist0001");
    rig.addAudio(off, QStringLiteral("Off"), 1000, 10);
    rig.resolver.videos[off].host = QStringLiteral("localhost");
    a = m->enqueue(hints(off, QString()), 0);
    CHECK(waitFor([&]() { return stateOf(*m, a) == static_cast<int>(State::Failed); }));
    CHECK(m->record(a).value(QStringLiteral("error")).toString() == QLatin1String("host_refused"));
    CHECK(rig.server.mediaHits(QStringLiteral("a-") + off) == 0);
    CHECK(EngineOptions::defaultMediaHost(QUrl(QStringLiteral("https://rr3---sn-abc.googlevideo.com/videoplayback?x=1"))));
    CHECK(!EngineOptions::defaultMediaHost(QUrl(QStringLiteral("http://rr3---sn-abc.googlevideo.com/videoplayback"))));
    CHECK(!EngineOptions::defaultMediaHost(QUrl(QStringLiteral("https://evilgooglevideo.com/videoplayback"))));
    CHECK(!EngineOptions::defaultMediaHost(QUrl(QStringLiteral("https://googlevideo.com.evil.example/x"))));
    CHECK(!EngineOptions::defaultMediaHost(QUrl(QStringLiteral("https://127.0.0.1/x"))));
    // NFR-SEC-05 G3: the resolvers' rule, since this check alone guards redirect targets.
    CHECK(EngineOptions::defaultMediaHost(QUrl(QStringLiteral("https://rr3---sn-abc.googlevideo.com:443/videoplayback"))));
    CHECK(!EngineOptions::defaultMediaHost(QUrl(QStringLiteral("https://user:pw@rr3---sn-abc.googlevideo.com/videoplayback"))));
    CHECK(!EngineOptions::defaultMediaHost(QUrl(QStringLiteral("https://rr3---sn-abc.googlevideo.com:8443/videoplayback"))));
    CHECK(!EngineOptions::defaultMediaHost(QUrl(QStringLiteral("https://rr3---sn-abc.googlevideo.com/other"))));
    CHECK(!EngineOptions::defaultMediaHost(QUrl(QStringLiteral("https://rr3---sn-abc.googlevideo.com/videoplayback/../x"))));
    CHECK(EngineOptions::defaultThumbnailHost(QUrl(QStringLiteral("https://i.ytimg.com/vi/x/mqdefault.jpg"))));
    CHECK(!EngineOptions::defaultThumbnailHost(QUrl(QStringLiteral("https://i.ytimg.com.evil.example/x.jpg"))));
    CHECK(!EngineOptions::defaultThumbnailHost(QUrl(QStringLiteral("https://rr1.googlevideo.com/x.jpg"))));
}

// The "Download quality" setting reaches the manager: default kind and height, used by every
// enqueue that names neither (the dialog's start values, "Download all").
static void downloadQualitySetting() {
    Rig rig;
    Env env = makeEnv("quality");
    std::unique_ptr<DownloadManager> m = makeManager(env, &rig.resolver, &rig.remuxer);
    unmeteredNow = false;   // nothing transfers: only what is queued matters here
    int changes = 0;
    QObject::connect(m.get(), &DownloadManager::defaultQualityHeightChanged, [&changes]() { ++changes; });

    m->setDownloadDefaults(720, 0);
    CHECK(m->defaultKind() == 1 && m->defaultQualityHeight() == 720);
    CHECK(m->property("defaultKind").toInt() == 1 && m->property("defaultQualityHeight").toInt() == 720);
    const int before = changes;
    m->setDownloadDefaults(720, 1080);                       // nothing changed: no signal
    CHECK(changes == before);
    int a = m->enqueue(hints(QStringLiteral("quality0001"), QStringLiteral("Q1")), -1);
    CHECK(m->record(a).value(QStringLiteral("kind")).toInt() == 1);
    CHECK(m->record(a).value(QStringLiteral("qualityHeight")).toInt() == 720);
    a = m->enqueue(hints(QStringLiteral("quality0002"), QStringLiteral("Q2")), 1, 360);   // the dialog's own pick wins
    CHECK(m->record(a).value(QStringLiteral("qualityHeight")).toInt() == 360);

    m->setDownloadDefaults(-1, 0);                           // "Audio only"
    CHECK(changes == before + 1);
    CHECK(m->defaultKind() == 0 && m->defaultQualityHeight() == 720);
    a = m->enqueue(hints(QStringLiteral("quality0003"), QStringLiteral("Q3")), -1);
    CHECK(m->record(a).value(QStringLiteral("kind")).toInt() == 0);
    a = m->enqueue(hints(QStringLiteral("quality0004"), QStringLiteral("Q4")), 1);        // video asked for by name
    CHECK(m->record(a).value(QStringLiteral("kind")).toInt() == 1);
    CHECK(m->record(a).value(QStringLiteral("qualityHeight")).toInt() == 720);
    QVariantList all;
    all << hints(QStringLiteral("quality0005"), QStringLiteral("Q5")) << hints(QStringLiteral("quality0006"), QStringLiteral("Q6"));
    CHECK(m->enqueueAll(all, -1, 0) == 2);                   // "Download all" follows the setting
    int audio = 0;
    for (const QVariant &r : m->records()) {
        const QVariantMap rec = r.toMap();
        const QString id = rec.value(QStringLiteral("videoId")).toString();
        if ((id == QLatin1String("quality0005") || id == QLatin1String("quality0006")) && rec.value(QStringLiteral("kind")).toInt() == 0) ++audio;
    }
    CHECK(audio == 2);

    m->setDownloadDefaults(1080, 480);
    CHECK(m->defaultKind() == 1 && m->defaultQualityHeight() == 1080);
    unmeteredNow = true;
}

// NFR-SEC-05: a planted .part link, an error page as the body, a full volume, planted remux outputs.
static void safetyChecks() {
    {   // G1: a symlink at "<name>.part" is replaced, never written through; its target stays empty.
        Rig rig;
        Env env = makeEnv("plantpart");
        const QString id = QStringLiteral("plant000001");
        rig.addAudio(id, QStringLiteral("Planted"), 150000, 4);
        const QString dir = env.appData + QStringLiteral("/downloads");
        QDir().mkpath(dir);
        const QString victim = env.base + QStringLiteral("/outside.bin");
        { QFile f(victim); f.open(QIODevice::WriteOnly); }
        const QString part = dir + QStringLiteral("/Planted [plant000001].m4a.part");
        CHECK(QFile::link(victim, part));
        std::unique_ptr<DownloadManager> m = makeManager(env, &rig.resolver, &rig.remuxer);
        const int dl = m->enqueue(hints(id, QStringLiteral("Planted")), 0);
        CHECK(waitFor([&]() { return stateOf(*m, dl) == static_cast<int>(State::Done); }));
        CHECK(QFileInfo(victim).size() == 0);
        const QString path = m->localMedia(id).value(QStringLiteral("filePath")).toString();
        CHECK(!path.isEmpty() && !QFileInfo(path).isSymLink());
        CHECK(readFile(path) == rig.bytes(id));
        CHECK(noPartFiles(dir));
    }
    {   // G2: an HTML page of exactly the announced size is refused as not media.
        Rig rig;
        Env env = makeEnv("notmedia");
        const QString id = QStringLiteral("notmedia001");
        rig.addAudio(id, QStringLiteral("Page"), 4096, 5);
        rig.server.contentTypes.insert(QStringLiteral("a-") + id, "text/html; charset=utf-8");
        std::unique_ptr<DownloadManager> m = makeManager(env, &rig.resolver, &rig.remuxer);
        const int dl = m->enqueue(hints(id, QStringLiteral("Page")), 0);
        CHECK(waitFor([&]() { return stateOf(*m, dl) == static_cast<int>(State::Failed); }));
        CHECK(m->record(dl).value(QStringLiteral("error")).toString() == QLatin1String("not_media"));
        CHECK(m->localMedia(id).value(QStringLiteral("filePath")).toString().isEmpty());
        CHECK(rig.server.mediaHits(QStringLiteral("a-") + id) == 1);   // refused at once, not retried
    }
    {   // G4: not enough free space for the rest of the download plus the reserve: no transfer.
        Rig rig;
        Env env = makeEnv("nospace");
        const QString id = QStringLiteral("nospace0001");
        rig.addAudio(id, QStringLiteral("Big"), 100000, 6);
        freeBytesNow = freeSpaceReserveBytes;   // the reserve alone; the file does not fit
        std::unique_ptr<DownloadManager> m = makeManager(env, &rig.resolver, &rig.remuxer);
        const int dl = m->enqueue(hints(id, QStringLiteral("Big")), 0);
        CHECK(waitFor([&]() { return stateOf(*m, dl) == static_cast<int>(State::Failed); }));
        CHECK(m->record(dl).value(QStringLiteral("error")).toString() == QLatin1String("no_space"));
        CHECK(rig.server.mediaHits(QStringLiteral("a-") + id) == 0);
        // Room again: a retry downloads it.
        freeBytesNow = freeSpaceReserveBytes + 2 * 100000 + 1;   // the file and its flatten copy
        m->retry(dl);
        CHECK(waitFor([&]() { return stateOf(*m, dl) == static_cast<int>(State::Done); }));
        freeBytesNow = -1;
    }
    {   // A link planted at the flatten's temporary output is removed before the remux writes.
        Rig rig;
        Env env = makeEnv("plantflat");
        const QByteArray dash = dashM4a(2);
        const QString id = QStringLiteral("plantflat01");
        const QString name = QStringLiteral("a-") + id;
        rig.server.media.insert(name, dash);
        FakeVideo v;
        v.title = QStringLiteral("Flat link");
        v.tracks.push_back(audioTrack(name, dash.size()));
        rig.resolver.videos.insert(id, v);
        const QString dir = env.appData + QStringLiteral("/downloads");
        QDir().mkpath(dir);
        const QString victim = env.base + QStringLiteral("/outside-flat.bin");
        { QFile f(victim); f.open(QIODevice::WriteOnly); }
        CHECK(QFile::link(victim, dir + QStringLiteral("/Flat link [plantflat01].m4a.flat.part")));
        std::unique_ptr<DownloadManager> m = makeManager(env, &rig.resolver, &rig.remuxer);
        const int dl = m->enqueue(hints(id, QStringLiteral("Flat link")), 0);
        CHECK(waitFor([&]() { return stateOf(*m, dl) == static_cast<int>(State::Done); }));
        CHECK(QFileInfo(victim).size() == 0);
        CHECK(isFlatMp4(DownloadEngine::mp4LayoutOf(m->localMedia(id).value(QStringLiteral("filePath")).toString())));
    }
}

// Kill and restart mid-download: the new engine resumes with Range from the part size.
static void restartResume() {
    Rig rig;
    Env env = makeEnv("restart");
    const QString id = QStringLiteral("restart0001");
    rig.addAudio(id, QStringLiteral("Restart"), 200000, 7);
    rig.server.hold = true;
    rig.server.holdFrom = 65536;
    const QString dir = env.appData + QStringLiteral("/downloads");
    const QString part = dir + QStringLiteral("/Restart [restart0001].m4a.part");
    int dl = 0;
    {
        std::unique_ptr<DownloadManager> m = makeManager(env, &rig.resolver, &rig.remuxer);
        dl = m->enqueue(hints(id, QString()), 0);
        CHECK(waitFor([&]() { return rig.server.parked.size() == 1; }));
        CHECK(QFileInfo(part).size() == 65536);
        CHECK(stateOf(*m, dl) == static_cast<int>(State::Downloading));
        // Progress is not written per chunk (2 s / 4 MiB rule): the row still says 0.
        CHECK(storedBytes(env, dl) == 0);
    } // destroyed: like a kill, the row stays Downloading
    CHECK(storedBytes(env, dl) == 65536); // forced write when the engine stopped
    rig.server.hold = false;
    const int mark = rig.server.hits.size();
    std::unique_ptr<DownloadManager> m2 = makeManager(env, &rig.resolver, &rig.remuxer);
    CHECK(m2->count() == 1);
    CHECK(waitFor([&]() { return stateOf(*m2, dl) == static_cast<int>(State::Done); }));
    CHECK(rig.server.sawRange(QStringLiteral("a-") + id, 65536, mark));
    CHECK(!rig.server.sawRange(QStringLiteral("a-") + id, 0, mark)); // never restarted from zero
    const QByteArray file = readFile(dir + QStringLiteral("/Restart [restart0001].m4a"));
    CHECK(file.size() == 200000 && sha(file) == sha(rig.bytes(id)));
    CHECK(noPartFiles(dir));
    rig.server.release();
}

// Pause keeps the part, resume continues from it; cancel deletes parts and the row.
static void pauseResumeCancel() {
    Rig rig;
    Env env = makeEnv("pause");
    const QString dir = env.appData + QStringLiteral("/downloads");
    const QString id = QStringLiteral("pause000001");
    rig.addAudio(id, QStringLiteral("Pause"), 200000, 8);
    rig.server.hold = true;
    rig.server.holdFrom = 65536;
    std::unique_ptr<DownloadManager> m = makeManager(env, &rig.resolver, &rig.remuxer);
    const int dl = m->enqueue(hints(id, QString()), 0);
    CHECK(waitFor([&]() { return rig.server.parked.size() == 1; }));
    CHECK(m->pause(dl));
    CHECK(stateOf(*m, dl) == static_cast<int>(State::Paused));
    CHECK(!m->keepAliveWanted());
    CHECK(QFileInfo(dir + QStringLiteral("/Pause [pause000001].m4a.part")).size() == 65536);
    rig.server.release();
    spin(100);
    CHECK(stateOf(*m, dl) == static_cast<int>(State::Paused));
    const int mark = rig.server.hits.size();
    CHECK(m->resume(dl));
    CHECK(waitFor([&]() { return stateOf(*m, dl) == static_cast<int>(State::Done); }));
    CHECK(rig.server.sawRange(QStringLiteral("a-") + id, 65536, mark));
    CHECK(sha(readFile(dir + QStringLiteral("/Pause [pause000001].m4a"))) == sha(rig.bytes(id)));
    CHECK(!m->pause(dl)); // Done cannot pause
    CHECK(!m->cancel(dl)); // Done is removed, not cancelled

    const QString cid = QStringLiteral("cancel00001");
    rig.addAudio(cid, QStringLiteral("Cancel"), 200000, 9);
    rig.server.hold = true;
    const int c = m->enqueue(hints(cid, QString()), 0);
    CHECK(waitFor([&]() { return rig.server.parked.size() == 1; }));
    int removedSignals = 0;
    QObject::connect(m.get(), &DownloadManager::itemRemoved, [&removedSignals](int, const QString &) { ++removedSignals; });
    CHECK(m->cancel(c));
    CHECK(removedSignals == 1);
    CHECK(m->record(c).isEmpty() && m->state(cid) == -1);
    CHECK(!QFileInfo(dir + QStringLiteral("/Cancel [cancel00001].m4a.part")).exists());
    CHECK(noPartFiles(dir));
    rig.server.release();
    spin(50);
    // The row is gone from the database too.
    std::unique_ptr<DownloadManager> again = makeManager(env, &rig.resolver, &rig.remuxer);
    CHECK(again->count() == 1);
}

// Exactly 2 transfers with 3 queued; the queue of 5 survives a restart; unmetered gate.
static void concurrencyAndGate() {
    Rig rig;
    Env env = makeEnv("concurrency");
    const QString dir = env.appData + QStringLiteral("/downloads");
    QStringList ids;
    for (int i = 0; i < 5; ++i) {
        const QString id = QStringLiteral("conc%1abcde").arg(i, 2, 10, QLatin1Char('0'));
        ids << id;
        rig.addAudio(id, QStringLiteral("Conc %1").arg(i), 150000, 20 + i);
    }
    rig.server.hold = true;
    rig.server.holdFrom = 0;
    int maxTransfers = 0;
    QList<int> dl;
    {
        std::unique_ptr<DownloadManager> m = makeManager(env, &rig.resolver, &rig.remuxer);
        DownloadManager *mp = m.get();
        QObject::connect(mp, &DownloadManager::itemChanged, [mp, &maxTransfers](int, const QString &) {
            maxTransfers = qMax(maxTransfers, mp->engine()->transferCount());
        });
        for (const QString &id : ids) dl << m->enqueue(hints(id, QString()), 0);
        CHECK(waitFor([&]() { return rig.server.parked.size() == 2; }));
        spin(150);
        CHECK(rig.server.parked.size() == 2);
        CHECK(m->engine()->transferCount() == 2);
        int queued = 0;
        for (int id : dl) if (stateOf(*m, id) == static_cast<int>(State::Queued)) ++queued;
        CHECK(queued == 3);
        CHECK(m->activeCount() == 5 && m->keepAliveWanted());
    } // kill with 2 running and 3 queued
    CHECK(waitFor([&]() { return rig.server.inflight == 0; })); // the killed transfers' sockets closed
    rig.server.parked.clear();
    rig.server.maxInflight = 0;
    rig.server.hold = false;
    std::unique_ptr<DownloadManager> m = makeManager(env, &rig.resolver, &rig.remuxer);
    DownloadManager *mp = m.get();
    QObject::connect(mp, &DownloadManager::itemChanged, [mp, &maxTransfers](int, const QString &) {
        maxTransfers = qMax(maxTransfers, mp->engine()->transferCount());
    });
    CHECK(m->count() == 5);
    for (int id : dl) CHECK(stateOf(*m, id) == static_cast<int>(State::Queued) || stateOf(*m, id) == static_cast<int>(State::Resolving));
    CHECK(waitFor([&]() {
        for (int id : dl) if (stateOf(*m, id) != static_cast<int>(State::Done)) return false;
        return true;
    }));
    CHECK(maxTransfers == 2);
    CHECK(rig.server.maxInflight <= 2);
    for (int i = 0; i < 5; ++i)
        CHECK(sha(readFile(dir + QStringLiteral("/Conc %1 [%2].m4a").arg(i).arg(ids[i]))) == sha(rig.bytes(ids[i])));

    // Unmetered only + a metered connection: nothing starts.
    m->setUnmeteredOnly(true);
    unmeteredNow = false;
    const QString g = QStringLiteral("gate0000001");
    rig.addAudio(g, QStringLiteral("Gate"), 100000, 30);
    const int gid = m->enqueue(hints(g, QString()), 0);
    CHECK(waitFor([&]() { return stateOf(*m, gid) == static_cast<int>(State::WaitingForNetwork); }));
    spin(150);
    CHECK(rig.resolver.calls.value(g) == 0 && rig.server.mediaHits(QStringLiteral("a-") + g) == 0);
    CHECK(!m->keepAliveWanted());
    // The setting persists with the library.
    {
        std::unique_ptr<DownloadManager> other = makeManager(env, &rig.resolver, &rig.remuxer);
        CHECK(other->unmeteredOnly());
    }
    unmeteredNow = true;
    m->engine()->networkChanged();
    CHECK(waitFor([&]() { return stateOf(*m, gid) == static_cast<int>(State::Done); }));

    // Gate closing mid-transfer moves the transfer to WaitingForNetwork, keeping the part.
    const QString g2 = QStringLiteral("gate0000002");
    rig.addAudio(g2, QStringLiteral("Gate2"), 200000, 31);
    rig.server.hold = true;
    rig.server.holdFrom = 65536;
    const int g2id = m->enqueue(hints(g2, QString()), 0);
    CHECK(waitFor([&]() { return rig.server.parked.size() == 1; }));
    unmeteredNow = false;
    m->engine()->networkChanged();
    CHECK(waitFor([&]() { return stateOf(*m, g2id) == static_cast<int>(State::WaitingForNetwork); }));
    CHECK(QFileInfo(dir + QStringLiteral("/Gate2 [gate0000002].m4a.part")).size() == 65536);
    rig.server.release();
    unmeteredNow = true;
    m->engine()->networkChanged();
    CHECK(waitFor([&]() { return stateOf(*m, g2id) == static_cast<int>(State::Done); }));
    CHECK(sha(readFile(dir + QStringLiteral("/Gate2 [gate0000002].m4a"))) == sha(rig.bytes(g2)));
    m->setUnmeteredOnly(false);
}

// DL-03: remuxed into one file when a muxer exists; a track pair otherwise.
static void videoPairs() {
    Rig rig;
    Env env = makeEnv("video");
    const QString dir = env.appData + QStringLiteral("/downloads");
    const auto addVideo = [&rig](const QString &id, const QString &title, int seed) {
        rig.server.media.insert(QStringLiteral("v-") + id, pattern(300000, seed));
        rig.server.media.insert(QStringLiteral("a-") + id, pattern(100000, seed + 1));
        FakeVideo v;
        v.title = title;
        v.tracks.push_back(videoTrack(QStringLiteral("v-") + id, 300000));
        v.tracks.push_back(audioTrack(QStringLiteral("a-") + id, 100000));
        FakeTrack av1 = videoTrack(QStringLiteral("missing"), 1);
        av1.id = "399"; av1.vcodec = "av01.0.08M.08"; av1.height = 1080;
        v.tracks.push_back(av1); // AV1 is never picked
        rig.resolver.videos.insert(id, v);
    };
    std::unique_ptr<DownloadManager> m = makeManager(env, &rig.resolver, &rig.remuxer);
    m->setDefaultQualityHeight(1080);

    const QString muxed = QStringLiteral("muxed000001");
    addVideo(muxed, QStringLiteral("Muxed"), 40);
    int a = m->enqueue(hints(muxed, QString()), 1);
    CHECK(m->record(a).value(QStringLiteral("qualityHeight")).toInt() == 1080); // the caller's ceiling
    CHECK(waitFor([&]() { return stateOf(*m, a) == static_cast<int>(State::Done); }));
    CHECK(rig.remuxer.calls == 1);
    CHECK(rig.server.mediaHits(QStringLiteral("missing")) == 0);
    QVariantMap local = m->localMedia(muxed);
    CHECK(local.value(QStringLiteral("available")).toBool());
    CHECK(local.value(QStringLiteral("layout")).toString() == QLatin1String("muxed"));
    CHECK(local.value(QStringLiteral("filePath")).toString() == dir + QStringLiteral("/Muxed [muxed000001].mp4"));
    CHECK(local.value(QStringLiteral("videoPath")).toString() == local.value(QStringLiteral("filePath")).toString());
    CHECK(sha(readFile(dir + QStringLiteral("/Muxed [muxed000001].mp4")))
          == sha(rig.bytes(muxed, "v-") + rig.bytes(muxed, "a-")));
    CHECK(!QFileInfo(dir + QStringLiteral("/Muxed [muxed000001].video.mp4")).exists());
    CHECK(!QFileInfo(dir + QStringLiteral("/Muxed [muxed000001].audio.m4a")).exists());
    CHECK(noPartFiles(dir));

    // No muxer: the two tracks stay as one logical item.
    rig.remuxer.available = false;
    const QString pair = QStringLiteral("pair0000001");
    addVideo(pair, QStringLiteral("Pair"), 50);
    a = m->enqueue(hints(pair, QString()), 1, 720);
    CHECK(waitFor([&]() { return stateOf(*m, a) == static_cast<int>(State::Done); }));
    CHECK(rig.remuxer.calls == 1);
    local = m->localMedia(pair);
    CHECK(local.value(QStringLiteral("available")).toBool());
    CHECK(local.value(QStringLiteral("layout")).toString() == QLatin1String("pair"));
    CHECK(local.value(QStringLiteral("videoPath")).toString() == dir + QStringLiteral("/Pair [pair0000001].video.mp4"));
    CHECK(local.value(QStringLiteral("audioPath")).toString() == dir + QStringLiteral("/Pair [pair0000001].audio.m4a"));
    CHECK(sha(readFile(local.value(QStringLiteral("videoPath")).toString())) == sha(rig.bytes(pair, "v-")));
    CHECK(sha(readFile(local.value(QStringLiteral("audioPath")).toString())) == sha(rig.bytes(pair, "a-")));
    CHECK(m->fraction(pair) == 1.0);
    // Losing one track of a pair makes it unavailable.
    QFile::remove(local.value(QStringLiteral("audioPath")).toString());
    CHECK(!m->localMedia(pair).value(QStringLiteral("available")).toBool());

    // A muxer that fails: kept as a pair, with a notice.
    rig.remuxer.available = true;
    rig.remuxer.succeed = false;
    QStringList notices;
    QObject::connect(m.get(), &DownloadManager::notice, [&notices](const QString &k) { notices << k; });
    const QString bad = QStringLiteral("muxfail0001");
    addVideo(bad, QStringLiteral("MuxFail"), 60);
    a = m->enqueue(hints(bad, QString()), 1);
    CHECK(waitFor([&]() { return stateOf(*m, a) == static_cast<int>(State::Done); }));
    CHECK(m->localMedia(bad).value(QStringLiteral("layout")).toString() == QLatin1String("pair"));
    CHECK(notices.contains(QLatin1String(noticeKeys::remuxFailed)));
    CHECK(noPartFiles(dir));
}

// DL-06: refused folders, fallback with a visible notice. DL-08: download all.
static void locationAndDownloadAll() {
    Rig rig;
    Env env = makeEnv("location");
    std::unique_ptr<DownloadManager> m = makeManager(env, &rig.resolver, &rig.remuxer);
    const QString readOnly = env.base + QStringLiteral("/ro");
    QDir().mkpath(readOnly);
    QFile::setPermissions(readOnly, QFileDevice::ReadOwner | QFileDevice::ExeOwner);
    QVariantMap c = m->chooseFolder(readOnly);
    CHECK(!c.value(QStringLiteral("ok")).toBool());
    CHECK(c.value(QStringLiteral("errorKey")).toString() == QLatin1String("downloads_error_folder_readonly"));
    CHECK(m->folder().isEmpty());
    c = m->chooseFolder(env.base + QStringLiteral("/nope"));
    CHECK(c.value(QStringLiteral("errorKey")).toString() == QLatin1String("downloads_error_folder_missing"));
    // /tmp is refused with the production roots (PLT-09).
    DownloadLocation production((LocationRoots()));
    CHECK(production.checkChosen(QStringLiteral("/tmp")).errorKey == QLatin1String("downloads_error_folder_temporary"));
    CHECK(production.checkChosen(QStringLiteral("relative/dir")).errorKey == QLatin1String("downloads_error_folder_invalid"));
    CHECK(DownloadLocation::defaultRoots().appData.endsWith(QCoreApplication::applicationName()));

    const QString sd = env.base + QStringLiteral("/media/SDCARD");
    QDir().mkpath(sd);
    QDir().mkpath(env.base + QStringLiteral("/Videos"));
    const QStringList candidates = m->candidateFolders();
    CHECK(candidates.contains(sd) && candidates.contains(env.base + QStringLiteral("/Videos")));
    c = m->chooseFolder(sd);
    CHECK(c.value(QStringLiteral("ok")).toBool() && m->folder() == sd);
    CHECK(QDir(sd).entryList(QDir::Files | QDir::Hidden).isEmpty()); // the probe file is gone

    // Downloads go to the chosen folder while it is there.
    const QString onSd = QStringLiteral("onsdcard001");
    rig.addAudio(onSd, QStringLiteral("OnSd"), 70000, 70);
    int a = m->enqueue(hints(onSd, QString()), 0);
    CHECK(waitFor([&]() { return stateOf(*m, a) == static_cast<int>(State::Done); }));
    CHECK(QFileInfo(sd + QStringLiteral("/OnSd [onsdcard001].m4a")).size() == 70000);

    // Card removed: the next download goes to the private dir and a notice is raised.
    QDir(sd).removeRecursively();
    QStringList notices;
    QObject::connect(m.get(), &DownloadManager::notice, [&notices](const QString &k) { notices << k; });
    const QString fb = QStringLiteral("fallback001");
    rig.addAudio(fb, QStringLiteral("Fallback"), 70000, 71);
    a = m->enqueue(hints(fb, QString()), 0);
    CHECK(waitFor([&]() { return stateOf(*m, a) == static_cast<int>(State::Done); }));
    CHECK(notices.contains(QLatin1String(noticeKeys::folderUnavailable)));
    CHECK(m->record(a).value(QStringLiteral("notice")).toString() == QLatin1String(noticeKeys::folderUnavailable));
    CHECK(QFileInfo(env.appData + QStringLiteral("/downloads/Fallback [fallback001].m4a")).size() == 70000);
    m->resetFolder();
    CHECK(m->folder().isEmpty());

    // DL-08: a 10-item playlist with 3 already downloaded queues exactly 7; again: nothing.
    QVariantList playlist;
    QStringList ids;
    for (int i = 0; i < 10; ++i) {
        const QString id = QStringLiteral("plist%1xyzw").arg(i, 2, 10, QLatin1Char('0'));
        ids << id;
        rig.addAudio(id, QStringLiteral("P%1").arg(i), 20000, 80 + i);
        QVariantMap e;
        e.insert(QStringLiteral("videoId"), id);
        e.insert(QStringLiteral("title"), QStringLiteral("P%1").arg(i));
        playlist << e;
    }
    QList<int> first;
    for (int i = 0; i < 3; ++i) first << m->enqueue(hints(ids[i * 3], QString()), 0);
    CHECK(waitFor([&]() {
        for (int id : first) if (stateOf(*m, id) != static_cast<int>(State::Done)) return false;
        return true;
    }));
    m->setUnmeteredOnly(true);
    unmeteredNow = false; // keep the rest waiting so the second run sees them unfinished
    CHECK(m->enqueueAll(playlist, 0) == 7);
    CHECK(m->enqueueAll(playlist, 0) == 0);
    CHECK(m->count() == 2 + 10);
    // Plain id strings work too; the video kind is separate.
    QVariantList plain;
    for (const QString &id : ids) plain << id;
    CHECK(m->enqueueAll(plain, 1) == 10);
    unmeteredNow = true;
    m->setUnmeteredOnly(false);
}

static void noExtractorProcess() {
#ifdef Q_OS_LINUX
    // No child process was ever started by this test binary (the engine spawns none).
    QDir task(QStringLiteral("/proc/self/task"));
    bool any = false;
    for (const QString &tid : task.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        QFile f(task.absoluteFilePath(tid) + QStringLiteral("/children"));
        if (f.open(QIODevice::ReadOnly) && !f.readAll().trimmed().isEmpty()) any = true;
    }
    CHECK(!any);
#endif
    CHECK(QCoreApplication::instance()->findChildren<QProcess *>().isEmpty());
}


// DL-01/DL-03: a DASH audio track (itag 140: ftyp dash, sidx, moof/mdat) is rewritten
// as a flat file (moov + one mdat); the device player cannot read fragments from disk.
static void dashAudioFlattened() {
    const QByteArray dash = dashM4a(3);
    {
        // the box scanner itself, on the bytes as downloaded
        QTemporaryFile probe;
        probe.open();
        probe.write(dash);
        probe.flush();
        const Mp4Layout l = DownloadEngine::mp4LayoutOf(probe.fileName());
        CHECK(l.valid && l.majorBrand == "dash" && l.moof == 3 && l.sidx == 1 && l.mdat == 3);
        CHECK(isFragmentedMp4(l) && !isFlatMp4(l));
    }
    for (int mode = 0; mode < 3; ++mode) { // 0: remuxer flattens, 1: remuxer fails, 2: no MP4 muxer
        Rig rig;
        rig.remuxer.succeed = mode != 1;
        rig.remuxer.available = mode != 2;
        Env env = makeEnv(QStringLiteral("dash%1").arg(mode).toLatin1().constData());
        const QString id = QStringLiteral("dashaudio0%1").arg(mode);
        const QString name = QStringLiteral("a-") + id;
        rig.server.media.insert(name, dash);
        FakeVideo v;
        v.title = QStringLiteral("Dash");
        v.tracks.push_back(audioTrack(name, dash.size()));
        rig.resolver.videos.insert(id, v);
        std::unique_ptr<DownloadManager> m = makeManager(env, &rig.resolver, &rig.remuxer);
        QStringList notices;
        QObject::connect(m.get(), &DownloadManager::notice, [&notices](const QString &k) { notices << k; });
        const int dl = m->enqueue(hints(id, QStringLiteral("Dash")), 0);
        CHECK(waitFor([&]() { return stateOf(*m, dl) == static_cast<int>(State::Done); }));
        const QString path = m->localMedia(id).value(QStringLiteral("filePath")).toString();
        CHECK(!path.isEmpty() && path.endsWith(QStringLiteral(".m4a")));
        const QString dir = QFileInfo(path).absolutePath();
        CHECK(noPartFiles(dir));
        CHECK(!QFileInfo(path + QStringLiteral(".flat")).exists());
        const Mp4Layout out = DownloadEngine::mp4LayoutOf(path);
        if (mode == 0) {
            CHECK(rig.remuxer.calls == 1);
            CHECK(rig.remuxer.lastVideo.isEmpty() && rig.remuxer.lastAudio == path); // one-track remux
            CHECK(isFlatMp4(out) && out.moof == 0 && out.sidx == 0 && out.moovBeforeMdat);
            CHECK(notices.isEmpty());
        } else {
            CHECK(rig.remuxer.calls == (mode == 1 ? 1 : 0));
            CHECK(readFile(path) == dash);             // kept as downloaded
            CHECK(notices.contains(QStringLiteral("downloads_notice_remux_failed")));
        }
    }
    // a flat or non-MP4 download is never touched
    Rig rig;
    Env env = makeEnv("flat");
    const QString id = QStringLiteral("flataudio01");
    rig.server.media.insert(QStringLiteral("a-") + id, flatM4a());
    FakeVideo v;
    v.title = QStringLiteral("Flat");
    v.tracks.push_back(audioTrack(QStringLiteral("a-") + id, flatM4a().size()));
    rig.resolver.videos.insert(id, v);
    std::unique_ptr<DownloadManager> m = makeManager(env, &rig.resolver, &rig.remuxer);
    const int dl = m->enqueue(hints(id, QStringLiteral("Flat")), 0);
    CHECK(waitFor([&]() { return stateOf(*m, dl) == static_cast<int>(State::Done); }));
    CHECK(rig.remuxer.calls == 0);
    CHECK(isFlatMp4(DownloadEngine::mp4LayoutOf(m->localMedia(id).value(QStringLiteral("filePath")).toString())));
}

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("org.example"));
    QCoreApplication::setApplicationName(QStringLiteral("sailtube-test"));
    if (!QSqlDatabase::isDriverAvailable(QStringLiteral("QSQLITE"))) {
        std::printf("downloads engine tests: SKIPPED (no QSQLITE driver)\n");
        return 0;
    }
    audioEndToEnd();
    resumeFaults();
    restartResume();
    pauseResumeCancel();
    concurrencyAndGate();
    videoPairs();
    locationAndDownloadAll();
    noExtractorProcess();
    dashAudioFlattened();
    safetyChecks();
    downloadQualitySetting();
    std::printf("downloads engine tests: %s (%d failures)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
