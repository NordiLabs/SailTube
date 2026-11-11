// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// NFR-SEC-05 download safety, against the real DownloadEngine (Core/Downloads, read-only) with its
// real queue store: a local media host on 127.0.0.1 that misbehaves on purpose, a scripted
// DownloadResolver and the documented host policy. Named cases:
//  * DL-05 names on disk: hostile titles end as one safe file inside the chosen folder;
//  * confinement: nothing is written outside the folder (a planted symlink is a probe: the engine
//    claims no symlink protection, so a write through one is reported as a GAP, not a failure);
//  * size: Content-Length / Content-Range disagreeing with the announced size is refused, a body
//    longer than its range is clipped, an unknown size is refused;
//  * type: the thumbnail must be a JPEG within its size cap; media bodies are not type-checked
//    (probe, reported as a GAP);
//  * hosts: https + *.googlevideo.com media and https://i.ytimg.com thumbnails only (the defaults),
//    redirects re-checked and capped, no cookies or credentials sent.
// Build: tests/download_safety/run.sh
#include "tests/fault_injection/FaultSupport.h"   // CHECK, waitFor, the named-case runner

#include "Core/Downloads/Domain/FileNaming.h"
#include "Core/Downloads/Infrastructure/DownloadEngine.h"
#include "Core/Downloads/Infrastructure/DownloadQueueStore.h"
#include "Core/Library/Infrastructure/LibrarySchema.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <memory>

using namespace nordtile::video;
using namespace nordtile::video::downloads;

namespace {

QByteArray pattern(int size, int seed) {
    QByteArray b(size, '\0');
    for (int i = 0; i < size; ++i) b[i] = static_cast<char>((i * 31 + seed * 7 + (i >> 9)) & 0xFF);
    return b;
}

QByteArray readFile(const QString &path) {
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

bool writeFile(const QString &path, const QByteArray &data) {
    QFile f(path);
    return f.open(QIODevice::WriteOnly) && f.write(data) == data.size();
}

// ---- local media host -----------------------------------------------------------------------

struct Resource {
    enum class Mode {
        Normal,          // 206 for the asked range
        Length200,       // 200, ignores Range, Content-Length = size + `extra`
        RangeTotal,      // 206 whose Content-Range total is size + `extra`
        Overlong,        // 206 for the asked range, `extra` more body bytes than the range
        NoLength,        // 200, no Content-Length, close-delimited
        Redirect         // 302 to `location`
    };
    Resource() {}
    Resource(const QByteArray &d, const QByteArray &type) : data(d), contentType(type) {}
    QByteArray data;
    QByteArray contentType = "application/octet-stream";
    Mode mode = Mode::Normal;
    int extra = 0;
    QByteArray location;
};

struct Hit {
    QString path;
    QByteArray range;
    bool cookie = false;
    bool authorization = false;
};

class MediaHost {
public:
    QTcpServer server;
    QHash<QString, Resource> media;    // /media/<name>
    QHash<QString, QByteArray> thumbs; // /thumb/<name>
    QList<Hit> hits;

    bool start() {
        if (!server.listen(QHostAddress::LocalHost, 0)) return false;
        QObject::connect(&server, &QTcpServer::newConnection, [this]() {
            while (QTcpSocket *s = server.nextPendingConnection()) {
                QObject::connect(s, &QTcpSocket::readyRead, [this, s]() { onRead(s); });
                QObject::connect(s, &QTcpSocket::disconnected, s, &QObject::deleteLater);
            }
        });
        return true;
    }
    QString base() const { return QStringLiteral("http://127.0.0.1:%1").arg(server.serverPort()); }
    int count(const QString &prefix) const {
        int n = 0;
        for (const Hit &h : hits) if (h.path.startsWith(prefix)) ++n;
        return n;
    }

private:
    static void reply(QTcpSocket *s, const QByteArray &head, const QByteArray &body) {
        s->write(head + "Connection: close\r\n\r\n" + body);
        s->flush();
        s->disconnectFromHost();
    }
    void onRead(QTcpSocket *s) {
        if (s->property("handled").toBool()) { s->readAll(); return; }
        const QByteArray buf = s->property("buf").toByteArray() + s->readAll();
        const int end = buf.indexOf("\r\n\r\n");
        if (end < 0) { s->setProperty("buf", buf); return; }
        s->setProperty("handled", true);
        const QList<QByteArray> lines = buf.left(end).split('\n');
        const QUrl url(QStringLiteral("http://x") + QString::fromLatin1(lines.value(0).trimmed().split(' ').value(1)));
        Hit h;
        h.path = url.path();
        for (const QByteArray &l : lines) {
            const QByteArray lower = l.trimmed().toLower();
            if (lower.startsWith("range:")) h.range = l.mid(6).trimmed();
            if (lower.startsWith("cookie:")) h.cookie = true;
            if (lower.startsWith("authorization:")) h.authorization = true;
        }
        hits.append(h);
        if (h.path.startsWith(QLatin1String("/thumb/"))) {
            const QString name = h.path.mid(7);
            if (!thumbs.contains(name)) { reply(s, "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n", QByteArray()); return; }
            const QByteArray body = thumbs.value(name);
            reply(s, "HTTP/1.1 200 OK\r\nContent-Type: image/jpeg\r\nContent-Length: " + QByteArray::number(body.size()) + "\r\n", body);
            return;
        }
        const QString name = h.path.mid(7);   // "/media/"
        if (!media.contains(name)) { reply(s, "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n", QByteArray()); return; }
        const Resource &r = media[name];
        const qint64 size = r.data.size();
        qint64 a = 0, b = size - 1;
        if (h.range.startsWith("bytes=")) {
            const QList<QByteArray> ab = h.range.mid(6).split('-');
            a = ab.value(0).toLongLong();
            if (!ab.value(1).isEmpty()) b = qMin(ab.value(1).toLongLong(), size - 1);
        }
        const QByteArray type = "Content-Type: " + r.contentType + "\r\n";
        switch (r.mode) {
        case Resource::Mode::Redirect:
            reply(s, "HTTP/1.1 302 Found\r\nLocation: " + r.location + "\r\nContent-Length: 0\r\n", QByteArray());
            return;
        case Resource::Mode::Length200: {
            const QByteArray body = r.data + QByteArray(r.extra, 'x');
            reply(s, "HTTP/1.1 200 OK\r\n" + type + "Content-Length: " + QByteArray::number(body.size()) + "\r\n", body);
            return;
        }
        case Resource::Mode::NoLength:
            reply(s, "HTTP/1.1 200 OK\r\n" + type, r.data);
            return;
        default:
            break;
        }
        if (a >= size) { reply(s, "HTTP/1.1 416 Range Not Satisfiable\r\nContent-Length: 0\r\n", QByteArray()); return; }
        QByteArray body = r.data.mid(static_cast<int>(a), static_cast<int>(b - a + 1));
        qint64 announced = size;
        if (r.mode == Resource::Mode::RangeTotal) announced = size + r.extra;
        if (r.mode == Resource::Mode::Overlong) body += pattern(r.extra, 99);
        reply(s, "HTTP/1.1 206 Partial Content\r\n" + type + "Content-Range: bytes " + QByteArray::number(a) + "-" +
                 QByteArray::number(b) + "/" + QByteArray::number(announced) + "\r\nContent-Length: " +
                 QByteArray::number(body.size()) + "\r\n", body);
    }
};

// ---- scripted resolver ----------------------------------------------------------------------

struct FakeVideo {
    QString title;
    QString url;            // the audio track's URL (empty: <host>/media/<id>)
    qint64 announced = 0;   // the resolver's size for it (0 unknown)
    QString thumbnailUrl;
};

class FakeResolver final : public DownloadResolver {
public:
    MediaHost *host = nullptr;
    QHash<QString, FakeVideo> videos;
    QObject context;
    quint64 next = 1;
    quint64 resolve(const QString &videoId, Completion completion) override {
        const quint64 id = next++;
        const FakeVideo v = videos.value(videoId);
        const QString base = host->base();
        QTimer::singleShot(0, &context, [videoId, v, base, completion]() {
            ResolvedVideo out;
            out.title = v.title;
            out.thumbnailUrl = v.thumbnailUrl;
            out.resolvedAtMs = QDateTime::currentMSecsSinceEpoch();
            media::MediaFormat f;
            f.id = "140";
            f.container = "m4a";
            f.protocol = media::Protocol::Https;
            f.audioCodecTag = "mp4a.40.2";
            f.audioCodec = media::parseAudioCodec(f.audioCodecTag);
            f.videoCodecTag = "none";
            f.videoCodec = media::parseVideoCodec(f.videoCodecTag);
            f.audioKbps = 128;
            f.fileSize = v.announced;
            f.url = (v.url.isEmpty() ? base + QStringLiteral("/media/") + videoId : v.url).toStdString();
            f.headers["User-Agent"] = "SailTubeSafetyTest/1";
            f.headers["Cookie"] = "must-not-be-sent=1";
            f.headers["Authorization"] = "Bearer must-not-be-sent";
            out.formats.push_back(f);
            completion(resolver::ErrorCode::None, out);
        });
        return id;
    }
    void cancel(quint64) override {}
};

// ---- the engine in a private folder -----------------------------------------------------------

std::function<bool(const QUrl &)> localOnly() {
    return [](const QUrl &u) { return u.scheme() == QLatin1String("http") && u.host() == QLatin1String("127.0.0.1"); };
}

int serial = 0;

struct Rig {
    QTemporaryDir root;
    QString folder;   // the chosen download folder
    QString outside;  // a sibling folder that must stay untouched
    MediaHost host;
    FakeResolver resolver;
    std::unique_ptr<DownloadQueueStore> store;
    std::unique_ptr<DownloadEngine> engine;

    // `defaultHosts`: the engine's documented host policy instead of the 127.0.0.1 test policy.
    explicit Rig(bool defaultMediaHosts = false) {
        CHECK(root.isValid());
        folder = root.path() + QStringLiteral("/Downloads");
        outside = root.path() + QStringLiteral("/outside");
        QDir().mkpath(folder);
        QDir().mkpath(outside);
        CHECK(host.start());
        resolver.host = &host;
        const QString db = root.path() + QStringLiteral("/library.db");
        const QString setup = QStringLiteral("safety-setup-%1").arg(++serial);
        {
            QSqlDatabase d = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), setup);
            d.setDatabaseName(db);
            CHECK(d.open());
            QSqlQuery q(d);
            for (const QString &sql : library::librarySchemaStep(0)) CHECK(q.exec(sql));
            CHECK(q.exec(QStringLiteral("PRAGMA user_version=1")));
            d.close();
        }
        QSqlDatabase::removeDatabase(setup);
        store.reset(new DownloadQueueStore(db, QStringLiteral("safety-%1").arg(serial)));
        QString error;
        CHECK(store->open(&error));
        EngineOptions o;
        o.chunkBytes = 16 * 1024;
        o.youngTokenDelayMs = 20;
        o.transientDelayMs = 20;
        o.stallTimeoutMs = 5000;
        o.thumbnailMaxBytes = 4096;
        if (!defaultMediaHosts) o.mediaHostAllowed = localOnly();
        o.thumbnailHostAllowed = localOnly();   // never the real thumbnail host from a test
        o.unmetered = []() { return true; };
        const QString dir = folder;
        o.locate = [dir](const QString &) { LocationDecision d; d.directory = dir.toStdString(); return d; };
        engine.reset(new DownloadEngine(store.get(), &resolver, nullptr, o));
        engine->start();
    }
    ~Rig() { engine.reset(); store.reset(); }

    qint64 add(const QString &videoId, const FakeVideo &video) {
        resolver.videos.insert(videoId, video);
        EnqueueHints hints;
        hints.videoId = videoId;
        hints.title = video.title;
        hints.thumbnailUrl = video.thumbnailUrl;
        return engine->enqueue(hints, Kind::Audio, 0);
    }
    State waitEnd(qint64 id, int ms = 15000) {
        catalog_test::waitFor([this, id] {
            const DownloadRecord *r = engine->record(id);
            return !r || r->state == State::Done || r->state == State::Failed;
        }, ms);
        const DownloadRecord *r = engine->record(id);
        return r ? r->state : State::Cancelled;
    }
    std::string error(qint64 id) const { const DownloadRecord *r = engine->record(id); return r ? r->error : std::string("?"); }
    // Every path below `dir`, relative, sorted.
    static QStringList tree(const QString &dir) {
        QStringList out;
        QDirIterator it(dir, QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
        while (it.hasNext()) out << QDir(dir).relativeFilePath(it.next());
        out.sort();
        return out;
    }
};

std::string audioName(const QString &title, const QString &id) {
    return fileName(baseName(title.toStdString(), id.toStdString()), "m4a");
}

// ---- DL-05 names on disk ---------------------------------------------------------------------

void hostileTitlesOnDisk() {
    Rig rig;
    std::string cjk;
    for (int i = 0; i < 100; ++i) cjk += "\xE6\x97\xA5";
    const struct { const char *id; QString title; } rows[] = {
        {"fiSafety001", QStringLiteral("../../outside/escape")},
        {"fiSafety002", QStringLiteral("a/b\\c/../../d")},
        {"fiSafety003", QString::fromUtf8("ctl\x01\x02\x7f" "del\ttab\nnl")},
        {"fiSafety004", QStringLiteral(".hidden")},
        {"fiSafety005", QStringLiteral("-rf --no-preserve-root")},
        {"fiSafety006", QStringLiteral("CON")},
        {"fiSafety007", QString::fromUtf8(cjk.c_str())},
        {"fiSafety008", QStringLiteral("%(title)s ${HOME} $(id)")},
        {"fiSafety009", QString::fromUtf8("evil\xE2\x80\xAEgpj.exe")},
        {"fiSafety010", QStringLiteral("<>:\"|?*")},
        {"fiSafety011", QStringLiteral("..")},
    };
    std::vector<qint64> ids;
    int n = 0;
    for (const auto &row : rows) {
        rig.host.media.insert(QLatin1String(row.id), Resource{pattern(20000 + n, n), "application/octet-stream"});
        FakeVideo v;
        v.title = row.title;
        v.announced = 20000 + n;
        ids.push_back(rig.add(QLatin1String(row.id), v));
        ++n;
    }
    n = 0;
    for (const auto &row : rows) {
        const qint64 id = ids[static_cast<std::size_t>(n)];
        CHECK(id > 0);
        CHECK(rig.waitEnd(id) == State::Done);
        const DownloadRecord *r = rig.engine->record(id);
        if (!r) { ++n; continue; }
        CHECK(safeFileName(r->fileName));
        CHECK(r->fileName.size() <= maxNameBytes);
        CHECK_EQ(r->fileName, audioName(row.title, QLatin1String(row.id)));
        const QString path = DownloadEngine::outputPath(*r);
        CHECK_EQ(QFileInfo(path).canonicalPath(), QFileInfo(rig.folder).canonicalFilePath());
        CHECK(readFile(path) == pattern(20000 + n, n));
        ++n;
    }
    // Everything is a flat, finished file in the folder; nothing anywhere else.
    const QStringList inside = Rig::tree(rig.folder);
    CHECK_EQ(inside.size(), static_cast<int>(sizeof(rows) / sizeof(rows[0])));
    for (const QString &p : inside) {
        CHECK(!p.contains(QLatin1Char('/')));
        CHECK(!p.endsWith(QLatin1String(".part")));
        CHECK(QFileInfo(rig.folder + QLatin1Char('/') + p).isFile());
    }
    CHECK(Rig::tree(rig.outside).isEmpty());
    // Hostile ids never reach a name: refused at enqueue.
    EnqueueHints bad;
    bad.title = QStringLiteral("x");
    for (const char *id : {"../../etc/x", "fiSafety01/", "fiSafety01\n", "", "fiSafety0123"}) {
        bad.videoId = QLatin1String(id);
        CHECK_EQ(rig.engine->enqueue(bad, Kind::Audio, 0), qint64(0));
    }
}

// ---- confinement -----------------------------------------------------------------------------

// A symlink planted at the in-progress name. The engine opens "<name>.part" for append without
// refusing links, so the bytes would land outside the folder: a GAP the engine does not claim.
void symlinkedPartProbe() {
    Rig rig;
    const QString id = QStringLiteral("fiSafety020");
    const QString title = QStringLiteral("planted part");
    const QString part = rig.folder + QLatin1Char('/') + QString::fromStdString(partFileName(audioName(title, id)));
    const QString victim = rig.outside + QStringLiteral("/victim.bin");
    CHECK(writeFile(victim, QByteArray()));
    CHECK(QFile::link(victim, part));
    rig.host.media.insert(id, Resource{pattern(30000, 2), "application/octet-stream"});
    FakeVideo v;
    v.title = title;
    v.announced = 30000;
    const qint64 rec = rig.add(id, v);
    const State end = rig.waitEnd(rec);
    const qint64 leaked = QFileInfo(victim).size();
    std::printf("  planted part symlink: state %s, %lld bytes written outside the folder\n", stateName(end),
                static_cast<long long>(leaked));
    fault::gap("confinement_symlinked_part_refused",
               "Core/Downloads/Infrastructure/DownloadEngine.cpp:607-608 beginTrack opens a planted <name>.part symlink for "
               "append and writes through it outside the chosen folder; :829 then renames the link into place",
               leaked > 0);
}

// A symlink planted at the final name: removed before the track starts, its target untouched.
void symlinkedFinalRefused() {
    Rig rig;
    const QString id = QStringLiteral("fiSafety021");
    const QString title = QStringLiteral("planted final");
    const QString finalPath = rig.folder + QLatin1Char('/') + QString::fromStdString(audioName(title, id));
    const QString victim = rig.outside + QStringLiteral("/keep.bin");
    CHECK(writeFile(victim, "keep"));
    CHECK(QFile::link(victim, finalPath));
    rig.host.media.insert(id, Resource{pattern(25000, 3), "application/octet-stream"});
    FakeVideo v;
    v.title = title;
    v.announced = 25000;
    const qint64 rec = rig.add(id, v);
    CHECK(rig.waitEnd(rec) == State::Done);
    CHECK_EQ(readFile(victim), QByteArray("keep"));
    CHECK(!QFileInfo(finalPath).isSymLink());
    CHECK(readFile(finalPath) == pattern(25000, 3));
    CHECK(Rig::tree(rig.outside) == QStringList() << QStringLiteral("keep.bin"));
}

// ---- size checks -----------------------------------------------------------------------------

// The download must end Failed with `code`, leaving no finished file.
void refused(Resource::Mode mode, int extra, qint64 announced, const char *code) {
    Rig rig;
    const QString id = QStringLiteral("fiSafety030");
    Resource r;
    r.data = pattern(40000, 4);
    r.mode = mode;
    r.extra = extra;
    rig.host.media.insert(id, r);
    FakeVideo v;
    v.title = QStringLiteral("size check");
    v.announced = announced;
    const qint64 rec = rig.add(id, v);
    CHECK(rig.waitEnd(rec) == State::Failed);
    CHECK_EQ(rig.error(rec), std::string(code));
    const QString finalPath = rig.folder + QLatin1Char('/') + QString::fromStdString(audioName(v.title, id));
    CHECK(!QFileInfo::exists(finalPath));
    CHECK(Rig::tree(rig.outside).isEmpty());
}

void overlongBodyClipped() {
    Rig rig;
    const QString id = QStringLiteral("fiSafety031");
    Resource r;
    r.data = pattern(50000, 5);
    r.mode = Resource::Mode::Overlong;
    r.extra = 3000;
    rig.host.media.insert(id, r);
    FakeVideo v;
    v.title = QStringLiteral("overlong");
    v.announced = 50000;
    const qint64 rec = rig.add(id, v);
    CHECK(rig.waitEnd(rec) == State::Done);
    const DownloadRecord *rr = rig.engine->record(rec);
    CHECK(rr != nullptr);
    if (!rr) return;
    const QByteArray got = readFile(DownloadEngine::outputPath(*rr));
    CHECK_EQ(got.size(), 50000);
    CHECK(got == r.data);
}

// ---- type checks -----------------------------------------------------------------------------

// A media request answered with an HTML page of exactly the announced size. The engine checks
// sizes, not types: the page is saved as the .m4a (probe, a GAP the engine does not claim).
void htmlBodyProbe() {
    Rig rig;
    const QString id = QStringLiteral("fiSafety040");
    QByteArray html = "<!DOCTYPE html><html><body>synthetic error page</body></html>";
    html += QByteArray(4096 - html.size(), ' ');
    Resource r;
    r.data = html;
    r.contentType = "text/html; charset=utf-8";
    rig.host.media.insert(id, r);
    FakeVideo v;
    v.title = QStringLiteral("not media");
    v.announced = html.size();
    const qint64 rec = rig.add(id, v);
    const State end = rig.waitEnd(rec);
    std::printf("  text/html media body: state %s\n", stateName(end));
    fault::gap("type_non_media_body_refused",
               "Core/Downloads/Infrastructure/DownloadEngine.cpp:690 checkResponse checks status, Content-Range and length "
               "only; a text/html body of the announced size is saved as media",
               end == State::Done);
}

void thumbnailTypeAndSize() {
    struct Row { const char *id; QByteArray body; bool saved; };
    QByteArray jpeg("\xFF\xD8\xFF\xE0", 4);
    jpeg += pattern(1000, 6);
    QByteArray bigJpeg("\xFF\xD8\xFF\xE0", 4);
    bigJpeg += pattern(8000, 7);   // above the 4096-byte cap of this rig
    const Row rows[] = {
        {"fiSafety050", jpeg, true},
        {"fiSafety051", QByteArray("<html>not an image</html>"), false},
        {"fiSafety052", QByteArray("\x89PNG\r\n\x1a\n", 8) + pattern(500, 8), false},
        {"fiSafety053", bigJpeg, false},
    };
    for (const Row &row : rows) {
        Rig rig;
        const QString id = QLatin1String(row.id);
        rig.host.media.insert(id, Resource{pattern(10000, 9), "application/octet-stream"});
        rig.host.thumbs.insert(id, row.body);
        FakeVideo v;
        v.title = QStringLiteral("thumb");
        v.announced = 10000;
        v.thumbnailUrl = rig.host.base() + QStringLiteral("/thumb/") + id;
        const qint64 rec = rig.add(id, v);
        CHECK(rig.waitEnd(rec) == State::Done);
        catalog_test::waitFor([&rig] { return rig.host.count(QStringLiteral("/thumb/")) > 0; }, 3000);
        catalog_test::waitFor([] { return false; }, 300);   // let the thumbnail reply settle
        const DownloadRecord *r = rig.engine->record(rec);
        CHECK(r != nullptr);
        if (!r) continue;
        CHECK_EQ(!r->thumbnailFile.empty(), row.saved);
        const QString jpg = rig.folder + QLatin1Char('/') + QString::fromStdString(thumbnailFileName(r->baseName));
        CHECK_EQ(QFileInfo::exists(jpg), row.saved);
        if (row.saved) CHECK(readFile(jpg) == row.body);
    }
}

// ---- hosts -----------------------------------------------------------------------------------

void defaultHostTables() {
    const struct { const char *url; bool media; bool thumb; } rows[] = {
        {"https://rr1---sn-fault.googlevideo.com/videoplayback?itag=140", true, false},
        {"https://redirector.googlevideo.com/videoplayback", true, false},
        {"https://i.ytimg.com/vi/fiSafety001/mqdefault.jpg", false, true},
        {"http://rr1---sn-fault.googlevideo.com/videoplayback", false, false},
        {"http://i.ytimg.com/vi/fiSafety001/mqdefault.jpg", false, false},
        {"https://googlevideo.com/videoplayback", false, false},
        {"https://evilgooglevideo.com/videoplayback", false, false},
        {"https://rr1.googlevideo.com.evil.invalid/videoplayback", false, false},
        {"https://rr1.googlevideo.com@evil.invalid/videoplayback", false, false},
        {"https://i.ytimg.com.evil.invalid/vi/x.jpg", false, false},
        {"https://i9.ytimg.com/vi/x.jpg", false, false},
        {"https://img.youtube.com/vi/x/0.jpg", false, false},
        {"https://www.youtube.com/watch?v=fiSafety001", false, false},
        {"ftp://rr1.googlevideo.com/videoplayback", false, false},
        {"file:///etc/passwd", false, false},
        {"https://127.0.0.1/videoplayback", false, false},
        {"", false, false},
    };
    for (const auto &row : rows) {
        const QUrl url(QLatin1String(row.url));
        CHECK_EQ(EngineOptions::defaultMediaHost(url), row.media);
        CHECK_EQ(EngineOptions::defaultThumbnailHost(url), row.thumb);
        if (EngineOptions::defaultMediaHost(url) != row.media) std::printf("  media host mismatch: %s\n", row.url);
        if (EngineOptions::defaultThumbnailHost(url) != row.thumb) std::printf("  thumbnail host mismatch: %s\n", row.url);
    }
}

// The engine's media check is the host suffix and the scheme only. The resolver paths
// (core's acceptedMediaUrl, the component's mediaHost) also refuse a userinfo part, a port other
// than 443 and a path other than /videoplayback; a redirect Location is checked by the engine alone.
void defaultHostLooseness() {
    const char *const loose[] = {
        "https://user:secret@rr1---sn-fault.googlevideo.com/videoplayback",
        "https://rr1---sn-fault.googlevideo.com:8443/videoplayback",
        "https://rr1---sn-fault.googlevideo.com/any/other/path",
    };
    int accepted = 0;
    for (const char *u : loose) if (EngineOptions::defaultMediaHost(QUrl(QLatin1String(u)))) ++accepted;
    std::printf("  %d of 3 loose googlevideo URLs accepted by EngineOptions::defaultMediaHost\n", accepted);
    fault::gap("hosts_media_check_port_userinfo_path",
               "Core/Downloads/Infrastructure/DownloadEngine.cpp:132-134 defaultMediaHost accepts userinfo, any port and "
               "any path on *.googlevideo.com (it alone guards redirect targets)",
               accepted > 0);
}

// With the engine's own policy: plain http and a foreign host are never contacted.
void defaultPolicyRefusesBeforeConnecting() {
    Rig rig(true);
    rig.host.media.insert(QStringLiteral("fiSafety060"), Resource{pattern(10000, 1), "application/octet-stream"});
    FakeVideo plain;
    plain.title = QStringLiteral("plain http");
    plain.announced = 10000;   // URL: http://127.0.0.1:<port>/media/fiSafety060
    FakeVideo foreign;
    foreign.title = QStringLiteral("foreign");
    foreign.announced = 10000;
    foreign.url = QStringLiteral("https://media.fault.invalid/videoplayback?itag=140");
    const qint64 a = rig.add(QStringLiteral("fiSafety060"), plain);
    const qint64 b = rig.add(QStringLiteral("fiSafety061"), foreign);
    CHECK(rig.waitEnd(a) == State::Failed);
    CHECK(rig.waitEnd(b) == State::Failed);
    CHECK_EQ(rig.error(a), std::string(errorCodes::hostRefused));
    CHECK_EQ(rig.error(b), std::string(errorCodes::hostRefused));
    CHECK_EQ(rig.host.hits.size(), 0);
    for (const QString &p : Rig::tree(rig.folder)) {   // at most the empty in-progress files
        CHECK(p.endsWith(QLatin1String(".part")));
        CHECK_EQ(QFileInfo(rig.folder + QLatin1Char('/') + p).size(), qint64(0));
    }
}

void redirects() {
    // To a foreign host: refused, never followed.
    {
        Rig rig;
        const QString id = QStringLiteral("fiSafety070");
        Resource r;
        r.data = pattern(10000, 1);
        r.mode = Resource::Mode::Redirect;
        r.location = "http://evil.fault.invalid/videoplayback";
        rig.host.media.insert(id, r);
        FakeVideo v;
        v.title = QStringLiteral("redirect away");
        v.announced = 10000;
        const qint64 rec = rig.add(id, v);
        CHECK(rig.waitEnd(rec) == State::Failed);
        CHECK_EQ(rig.error(rec), std::string(errorCodes::hostRefused));
        CHECK_EQ(rig.host.count(QStringLiteral("/media/")), 1);
    }
    // An endless chain on an allowed host: capped at three hops.
    {
        Rig rig;
        const QString id = QStringLiteral("fiSafety071");
        Resource r;
        r.data = pattern(10000, 1);
        r.mode = Resource::Mode::Redirect;
        r.location = ("/media/" + id).toLatin1();
        rig.host.media.insert(id, r);
        FakeVideo v;
        v.title = QStringLiteral("redirect loop");
        v.announced = 10000;
        const qint64 rec = rig.add(id, v);
        CHECK(rig.waitEnd(rec) == State::Failed);
        CHECK_EQ(rig.error(rec), std::string(errorCodes::hostRefused));
        CHECK_EQ(rig.host.count(QStringLiteral("/media/")), 4);
    }
}

void noCredentialsSent() {
    Rig rig;
    const QString id = QStringLiteral("fiSafety080");
    rig.host.media.insert(id, Resource{pattern(40000, 3), "application/octet-stream"});
    FakeVideo v;
    v.title = QStringLiteral("headers");
    v.announced = 40000;
    const qint64 rec = rig.add(id, v);
    CHECK(rig.waitEnd(rec) == State::Done);
    CHECK(rig.host.hits.size() >= 3);   // several Range chunks
    for (const Hit &h : rig.host.hits) {
        CHECK(!h.cookie);
        CHECK(!h.authorization);
        if (h.path.startsWith(QLatin1String("/media/"))) CHECK(h.range.startsWith("bytes="));
    }
}

}  // namespace

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    fault::run("dl05_hostile_titles_on_disk", hostileTitlesOnDisk);
    fault::run("confinement_symlinked_part_probe", symlinkedPartProbe);
    fault::run("confinement_symlinked_final_replaced", symlinkedFinalRefused);
    fault::run("size_content_length_mismatch_refused", [] { refused(Resource::Mode::Length200, 5, 40000, errorCodes::sizeMismatch); });
    fault::run("size_content_range_total_mismatch_refused", [] { refused(Resource::Mode::RangeTotal, 1000, 40000, errorCodes::sizeMismatch); });
    fault::run("size_unknown_length_refused", [] { refused(Resource::Mode::NoLength, 0, 0, errorCodes::http); });
    fault::run("size_body_longer_than_range_clipped", overlongBodyClipped);
    fault::run("type_non_media_body_probe", htmlBodyProbe);
    fault::run("type_thumbnail_jpeg_and_size_checked", thumbnailTypeAndSize);
    fault::run("hosts_default_policy_tables", defaultHostTables);
    fault::run("hosts_default_policy_looseness_probe", defaultHostLooseness);
    fault::run("hosts_default_policy_refuses_before_connecting", defaultPolicyRefusesBeforeConnecting);
    fault::run("hosts_redirects_rechecked_and_capped", redirects);
    fault::run("hosts_no_cookies_or_credentials_sent", noCredentialsSent);
    return fault::finish("download_safety engine");
}
