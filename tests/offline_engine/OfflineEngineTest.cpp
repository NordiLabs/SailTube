// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// NFR-TST-02: the six offline engine cases, one named test each, against the real download engine
// (DownloadManager -> DownloadEngine -> QNetworkAccessManager) and a fake googlevideo on 127.0.0.1
// that honours Range and plays scripted faults. Runs under tests/offline_engine/no_network.so; the
// loopback media proxy's side of the same cases is SailTune's pinned MediaProxyTest (run.sh).
// Exit 0 pass, 1 fail, 77 skipped (no QSQLITE driver). Build: tests/offline_engine/run.sh
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
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QSet>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>
#include <cstdio>
#include <dlfcn.h>
#include <functional>
#include <memory>

using namespace nordtile::video;
using namespace nordtile::video::downloads;

static int failures = 0;
static int checks = 0;
static const char *currentCase = "";
#define CHECK(c) do { ++checks; if (!(c)) { std::printf("FAIL [%s] %s:%d: %s\n", currentCase, __FILE__, __LINE__, #c); ++failures; } } while (0)

static void spin(int ms) {
    QEventLoop loop;
    QTimer::singleShot(ms, &loop, SLOT(quit()));
    loop.exec();
}

static bool waitFor(const std::function<bool()> &cond, int timeoutMs = 15000) {
    QElapsedTimer t;
    t.start();
    while (!cond()) {
        if (t.elapsed() > timeoutMs) return false;
        spin(5);
    }
    return true;
}

static QByteArray bytesOf(int size, int seed) {
    QByteArray b(size, '\0');
    for (int i = 0; i < size; ++i) b[i] = static_cast<char>((i * 31 + seed * 17 + (i >> 9)) & 0xFF);
    return b;
}

static QByteArray digest(const QByteArray &b) { return QCryptographicHash::hash(b, QCryptographicHash::Sha256); }

static QByteArray slurp(const QString &path) {
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

// ---- fake googlevideo ---------------------------------------------------------------------------

// One request as the fake saw it and what it answered.
struct Request {
    QString name;
    int tok = 0;          // generation of the signed address (1: first resolve, 2: after a re-resolve)
    QByteArray range;     // the raw Range header
    qint64 start = -1;
    qint64 end = -1;
    int status = 0;
    qint64 bodySent = 0;
};

// A scripted fault, used once: the first request for `name` starting at `start` (and of address
// generation `tok`, 0 = any) gets it instead of the media.
struct Fault {
    QString name;
    qint64 start = 0;
    int tok = 0;
    int status = 0;           // 403, 416, 503 ...; 0 with dropAfter: serve and cut
    qint64 dropAfter = -1;    // serve this many body bytes of a 206 and close the connection
    bool noLength = false;    // with dropAfter: no Content-Length, so the close ends the body without an error
    bool used = false;
};

class FakeGoogleVideo {
public:
    QTcpServer server;
    QHash<QString, QByteArray> media;   // /videoplayback/<name>
    QList<Fault> faults;
    QList<Request> requests;

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
    void fault(const QString &name, qint64 start, int status, int tok = 0, qint64 dropAfter = -1, bool noLength = false) {
        Fault f;
        f.name = name; f.start = start; f.status = status; f.tok = tok; f.dropAfter = dropAfter; f.noLength = noLength;
        faults.append(f);
    }
    QList<Request> of(const QString &name) const {
        QList<Request> out;
        for (const Request &r : requests) if (r.name == name) out.append(r);
        return out;
    }

private:
    static void answer(QTcpSocket *s, int status, const QByteArray &extraHeaders) {
        s->write("HTTP/1.1 " + QByteArray::number(status) + " Scripted\r\n" + extraHeaders
                 + "Content-Length: 0\r\nConnection: close\r\n\r\n");
        s->flush();
        s->disconnectFromHost();
    }
    void onRead(QTcpSocket *s) {
        if (s->property("done").toBool()) { s->readAll(); return; }
        const QByteArray buf = s->property("buf").toByteArray() + s->readAll();
        const int headEnd = buf.indexOf("\r\n\r\n");
        if (headEnd < 0) { s->setProperty("buf", buf); return; }
        s->setProperty("done", true);
        const QList<QByteArray> lines = buf.left(headEnd).split('\n');
        const QUrl url(QStringLiteral("http://x") + QString::fromLatin1(lines.value(0).trimmed().split(' ').value(1)));
        Request r;
        for (const QByteArray &l : lines)
            if (l.toLower().startsWith("range:")) r.range = l.mid(6).trimmed();
        r.name = url.path().mid(QStringLiteral("/videoplayback/").size());
        r.tok = QUrlQuery(url).queryItemValue(QStringLiteral("tok")).toInt();
        if (!media.contains(r.name)) { r.status = 404; requests.append(r); answer(s, 404, QByteArray()); return; }
        const QByteArray d = media.value(r.name);
        const qint64 total = d.size();
        r.start = 0; r.end = total - 1;
        bool ranged = false;
        if (r.range.startsWith("bytes=")) {
            const QList<QByteArray> ab = r.range.mid(6).split('-');
            r.start = ab.value(0).toLongLong();
            if (!ab.value(1).isEmpty()) r.end = qMin<qint64>(ab.value(1).toLongLong(), total - 1);
            ranged = true;
        }
        Fault *hit = nullptr;
        for (Fault &f : faults)
            if (!f.used && f.name == r.name && f.start == r.start && (f.tok == 0 || f.tok == r.tok)) { hit = &f; break; }
        if (hit) hit->used = true;
        if (hit && hit->status) {
            r.status = hit->status;
            requests.append(r);
            // A 416 names the real length the way a media host does: "bytes */<total>".
            answer(s, hit->status, hit->status == 416 ? "Content-Range: bytes */" + QByteArray::number(total) + "\r\n" : QByteArray());
            return;
        }
        if (r.start >= total) {
            r.status = 416;
            requests.append(r);
            answer(s, 416, "Content-Range: bytes */" + QByteArray::number(total) + "\r\n");
            return;
        }
        const QByteArray body = d.mid(static_cast<int>(r.start), static_cast<int>(r.end - r.start + 1));
        QByteArray head = ranged ? QByteArray("HTTP/1.1 206 Partial Content\r\nContent-Range: bytes ") + QByteArray::number(r.start)
                                       + "-" + QByteArray::number(r.end) + "/" + QByteArray::number(total) + "\r\n"
                                 : QByteArray("HTTP/1.1 200 OK\r\n");
        head += "Content-Type: application/octet-stream\r\n";
        if (!(hit && hit->dropAfter >= 0 && hit->noLength)) head += "Content-Length: " + QByteArray::number(body.size()) + "\r\n";
        head += "Connection: close\r\n\r\n";
        r.status = ranged ? 206 : 200;
        if (hit && hit->dropAfter >= 0) {
            r.bodySent = qMin<qint64>(hit->dropAfter, body.size());
            requests.append(r);
            s->write(head + body.left(static_cast<int>(r.bodySent)));
            s->flush();
            s->disconnectFromHost();   // truncated: fewer bytes than Content-Length
            return;
        }
        r.bodySent = body.size();
        requests.append(r);
        s->write(head + body);
        s->flush();
        s->disconnectFromHost();
    }
};

// ---- fake resolver ------------------------------------------------------------------------------

class FakeResolver final : public DownloadResolver {
public:
    FakeGoogleVideo *host = nullptr;
    QHash<QString, int> length;     // videoId -> clen of its one audio track
    QHash<QString, qint64> addressAgeMs;   // videoId -> how old the address is when handed out
    QHash<QString, int> calls;
    QSet<quint64> cancelled;
    QObject context;
    quint64 next = 1;

    quint64 resolve(const QString &videoId, Completion completion) override {
        const quint64 id = next++;
        const int generation = ++calls[videoId];
        QTimer::singleShot(0, &context, [this, id, videoId, generation, completion]() {
            if (cancelled.remove(id)) return;
            ResolvedVideo v;
            v.resolvedAtMs = QDateTime::currentMSecsSinceEpoch() - addressAgeMs.value(videoId, 0);
            if (!length.contains(videoId)) { completion(resolver::ErrorCode::VideoUnavailable, v); return; }
            v.title = videoId;
            media::MediaFormat f;
            f.id = "140"; f.container = "m4a"; f.protocol = media::Protocol::Https;
            f.videoCodecTag = "none"; f.audioCodecTag = "mp4a.40.2";
            f.videoCodec = media::parseVideoCodec("none"); f.audioCodec = media::parseAudioCodec("mp4a.40.2");
            f.totalKbps = 128; f.audioKbps = 128;
            f.url = (host->base() + QStringLiteral("/videoplayback/a-") + videoId
                     + QStringLiteral("?tok=%1&clen=%2").arg(generation).arg(length.value(videoId))).toStdString();
            f.headers["User-Agent"] = "SailTubeOffline/1";
            v.formats.push_back(f);
            completion(resolver::ErrorCode::None, v);
        });
        return id;
    }
    void cancel(quint64 id) override { cancelled.insert(id); }
};

// ---- rig ----------------------------------------------------------------------------------------

static int serial = 0;

struct Rig {
    FakeGoogleVideo host;
    FakeResolver resolver;
    QString base, db, appData, dir;
    std::unique_ptr<DownloadManager> manager;
    qint64 freeBytes = -1;   // -1: the engine's own QStorageInfo answer

    explicit Rig(const char *name) {
        CHECK(host.start());
        resolver.host = &host;
        const QString tmp = QString::fromLocal8Bit(qgetenv("TMPDIR"));
        base = (tmp.isEmpty() ? QDir::tempPath() : tmp) + QStringLiteral("/offline-%1-%2").arg(QLatin1String(name)).arg(QCoreApplication::applicationPid());
        QDir(base).removeRecursively();
        QDir().mkpath(base);
        db = base + QStringLiteral("/library.db");
        appData = base + QStringLiteral("/appdata");
        dir = appData + QStringLiteral("/downloads");
        const QString setup = QStringLiteral("setup-%1").arg(++serial);
        {
            QSqlDatabase d = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), setup);
            d.setDatabaseName(db);
            CHECK(d.open());
            QSqlQuery q(d);
            for (const QString &sql : library::librarySchemaStep(0)) q.exec(sql);
            q.exec(QStringLiteral("PRAGMA user_version=1"));
            d.close();
        }
        QSqlDatabase::removeDatabase(setup);
    }
    ~Rig() {
        manager.reset();
        QDir(base).removeRecursively();
    }
    void open() {
        DownloadManagerConfig c;
        c.databasePath = db;
        c.connectionName = QStringLiteral("offline-%1").arg(++serial);
        c.roots.appData = appData;
        c.roots.userFolders = QStringList() << base + QStringLiteral("/Videos");
        c.roots.removableRoot = base + QStringLiteral("/media");
        c.roots.refuseTemporary = false;   // TMPDIR is under /tmp in the build container
        c.progressIntervalMs = 50;
        c.engine.chunkBytes = 64 * 1024;
        c.engine.youngTokenDelayMs = 50;
        c.engine.transientDelayMs = 50;
        c.engine.stallTimeoutMs = 10000;
        const auto loopback = [](const QUrl &u) { return u.scheme() == QLatin1String("http") && u.host() == QLatin1String("127.0.0.1"); };
        c.engine.mediaHostAllowed = loopback;
        c.engine.thumbnailHostAllowed = loopback;
        c.engine.unmetered = []() { return true; };
        c.engine.freeBytes = [this](const QString &d) { return freeBytes >= 0 ? freeBytes : EngineOptions::defaultFreeBytes(d); };
        manager.reset(new DownloadManager(c, &resolver, nullptr));
        QString error;
        CHECK(manager->open(&error));
    }
    QString name(const QString &id) const { return QStringLiteral("a-") + id; }
    void add(const QString &id, int size, int seed, qint64 age = 0) {
        host.media.insert(name(id), bytesOf(size, seed));
        resolver.length.insert(id, size);
        resolver.addressAgeMs.insert(id, age);
    }
    int enqueue(const QString &id) {
        QVariantMap h;
        h.insert(QStringLiteral("videoId"), id);
        h.insert(QStringLiteral("title"), id);
        h.insert(QStringLiteral("channel"), QStringLiteral("Channel"));
        h.insert(QStringLiteral("durationSeconds"), 60);
        return manager->enqueue(h, 0);
    }
    int stateOf(int item) const { return manager->record(item).value(QStringLiteral("state"), -1).toInt(); }
    QString errorOf(int item) const { return manager->record(item).value(QStringLiteral("error")).toString(); }
    // Waits for Done or Failed (never the full timeout on a failure); true when Done.
    bool finishes(int item) {
        waitFor([&]() { const int s = stateOf(item); return s == int(State::Done) || s == int(State::Failed); });
        const int s = stateOf(item);
        if (s != int(State::Done)) std::printf("  [%s] item %d ended in state %d, error '%s'\n", currentCase, item, s, qPrintable(errorOf(item)));
        return s == int(State::Done);
    }
    QString file(const QString &id) const { return dir + QLatin1Char('/') + id + QStringLiteral(" [") + id + QStringLiteral("].m4a"); }
    bool intact(const QString &id) const {
        const QByteArray got = slurp(file(id));
        return got.size() == host.media.value(name(id)).size() && digest(got) == digest(host.media.value(name(id)));
    }
    // What the fake host saw for `id`, in the order it parsed the requests (printed when a case fails).
    void dump(const QString &id) const {
        const QList<Request> list = host.of(name(id));
        for (int i = 0; i < list.size(); ++i)
            std::printf("  [%s] request %d: tok %d, %s, status %d, start %lld, end %lld, body sent %lld\n", currentCase, i,
                        list[i].tok, list[i].range.constData(), list[i].status, static_cast<long long>(list[i].start),
                        static_cast<long long>(list[i].end), static_cast<long long>(list[i].bodySent));
    }
    // The requests for `id` without exact repeats (same Range header and address generation), in
    // first-seen order; `repeated` gets the Range headers that came again. The engine never asks for
    // the same range twice in these cases, but Qt 5.6's HTTP layer sometimes re-sends, on its own, a
    // request whose answer was cut short: the reply has already finished (the engine reopens at the
    // cut) and the second answer is dropped unread, yet the host sees the request twice.
    QList<Request> distinct(const QString &id, QList<QByteArray> *repeated) const {
        QList<Request> out;
        for (const Request &r : host.of(name(id))) {
            bool seen = false;
            for (const Request &o : out) if (o.range == r.range && o.tok == r.tok) { seen = true; break; }
            if (seen) repeated->append(r.range);
            else out.append(r);
        }
        return out;
    }
    int indexOf(const QString &id, int status, qint64 start, int from = 0) const {
        const QList<Request> list = host.of(name(id));
        for (int i = from; i < list.size(); ++i) if (list[i].status == status && list[i].start == start) return i;
        return -1;
    }
};

// ---- the six cases ------------------------------------------------------------------------------

// 1. Range: every request is a bounded "bytes=a-b" chunk, contiguous and in order, answered 206 with a
// matching Content-Range; the file is byte-identical and ends at exactly clen.
static void caseRange() {
    currentCase = "Range";
    Rig rig("range");
    rig.open();
    const QString id = QStringLiteral("range000001");
    rig.add(id, 200000, 1);
    const int item = rig.enqueue(id);
    CHECK(item > 0);
    CHECK(rig.finishes(item));
    CHECK(rig.intact(id));
    const QList<Request> list = rig.host.of(rig.name(id));
    CHECK(list.size() == 4);   // 3 x 64 KiB + the rest
    qint64 expect = 0;
    for (const Request &r : list) {
        CHECK(r.status == 206 && r.start == expect);
        CHECK(r.range == "bytes=" + QByteArray::number(r.start) + "-" + QByteArray::number(r.end));
        CHECK(r.end - r.start + 1 <= 64 * 1024);
        expect = r.end + 1;
    }
    CHECK(expect == 200000);
    CHECK(QFileInfo(rig.file(id)).size() == 200000);
    CHECK(!QFileInfo(rig.file(id) + QStringLiteral(".part")).exists());
}

// 2. 403 at byte 0 right after the resolve: retried on the same address (no re-resolve), then served.
static void case403AtByteZero() {
    currentCase = "403 at byte 0";
    Rig rig("forbid0");
    rig.open();
    const QString id = QStringLiteral("forbid00001");
    rig.add(id, 150000, 2, 0);
    rig.host.fault(rig.name(id), 0, 403);
    const int item = rig.enqueue(id);
    CHECK(rig.finishes(item));
    CHECK(rig.intact(id));
    CHECK(rig.resolver.calls.value(id) == 1);
    const QList<Request> list = rig.host.of(rig.name(id));
    CHECK(list.size() >= 2 && list[0].status == 403 && list[0].start == 0);
    CHECK(list.size() >= 2 && list[1].status == 206 && list[1].start == 0 && list[1].tok == 1);   // same address
}

// 3. The young-token window: a 403 at byte 0 is tolerated in place only while the address is young
// (ResumeLimits::youngTokenWindowMs, 10 s, at most youngTokenRetries = 2 times); an old address, or one
// still refused after the retries, is re-resolved for the same format.
static void caseYoungTokenWindow() {
    currentCase = "young-token window";
    Rig rig("young");
    rig.open();
    const ResumeLimits limits;
    CHECK(limits.youngTokenWindowMs == 10000 && limits.youngTokenRetries == 2);
    {   // inside the window (2 s old): two refusals are both retried in place
        const QString id = QStringLiteral("youngin0001");
        rig.add(id, 100000, 3, 2000);
        rig.host.fault(rig.name(id), 0, 403, 1);
        rig.host.fault(rig.name(id), 0, 403, 1);
        const int item = rig.enqueue(id);
        CHECK(rig.finishes(item));
        CHECK(rig.intact(id));
        CHECK(rig.resolver.calls.value(id) == 1);
        const QList<Request> list = rig.host.of(rig.name(id));
        CHECK(list.size() >= 3 && list[2].status == 206 && list[2].start == 0 && list[2].tok == 1);
    }
    {   // inside the window but refused a third time: the retries are spent, re-resolve
        const QString id = QStringLiteral("youngin0002");
        rig.add(id, 100000, 4, 2000);
        for (int i = 0; i < 3; ++i) rig.host.fault(rig.name(id), 0, 403, 1);
        const int item = rig.enqueue(id);
        CHECK(rig.finishes(item));
        CHECK(rig.intact(id));
        CHECK(rig.resolver.calls.value(id) == 2);
        const QList<Request> list = rig.host.of(rig.name(id));
        CHECK(list.size() >= 4 && list[3].status == 206 && list[3].start == 0 && list[3].tok == 2);
    }
    {   // outside the window (30 s old): the first refusal at byte 0 re-resolves at once
        const QString id = QStringLiteral("youngout001");
        rig.add(id, 100000, 5, 30000);
        rig.host.fault(rig.name(id), 0, 403, 1);
        const int item = rig.enqueue(id);
        CHECK(rig.finishes(item));
        CHECK(rig.intact(id));
        CHECK(rig.resolver.calls.value(id) == 2);
        const QList<Request> list = rig.host.of(rig.name(id));
        CHECK(list.size() >= 2 && list[0].status == 403 && list[1].status == 206 && list[1].start == 0 && list[1].tok == 2);
    }
}

// 4. Truncation and reopen: a body cut short is kept and the transfer reopens exactly at the cut, on
// the same address; twice in one file. What the engine asked for is exactly five ranges, in order;
// anything else (a restart from zero would add 65536-131071) fails.
// Only a request whose answer the host cut may reach the host a second time: that is Qt 5.6's HTTP
// layer re-sending it by itself (seen about once in 200 cuts, depending on timing; the engine issued
// five requests every time, see docs/BUILDING.md "Offline engine suite").
static void truncationOnce(Rig &rig, const QString &id, bool noLength) {
    const int failuresBefore = failures;
    rig.add(id, 200000, 6);
    rig.host.fault(rig.name(id), 0, 0, 0, 30000, noLength);    // first chunk: 30000 of 65536 bytes, then closed
    rig.host.fault(rig.name(id), 95536, 0, 0, 1, noLength);    // the chunk after the reopen: one byte, then closed
    const int item = rig.enqueue(id);
    CHECK(rig.finishes(item));
    CHECK(rig.intact(id));
    CHECK(rig.resolver.calls.value(id) == 1);
    QList<QByteArray> repeated;
    const QList<Request> list = rig.distinct(id, &repeated);
    const char *const expected[] = { "bytes=0-65535",         // cut after 30000 bytes
                                     "bytes=30000-95535",     // reopened right at the cut
                                     "bytes=95536-161071",    // cut after one byte
                                     "bytes=95537-161072",    // reopened right after it
                                     "bytes=161073-199999" };
    CHECK(list.size() == 5);                                 // never restarted from zero, nothing asked twice over
    for (int i = 0; i < list.size() && i < 5; ++i) {
        CHECK(list[i].range == expected[i]);
        CHECK(list[i].tok == 1 && list[i].status == 206);   // the same address throughout
    }
    if (list.size() == 5) CHECK(list[0].bodySent == 30000 && list[2].bodySent == 1);   // the cuts hit the engine's requests
    for (const QByteArray &range : repeated) CHECK(range == expected[0] || range == expected[2]);   // only cut answers
    CHECK(repeated.size() <= 4);                             // Qt re-sends a request at most twice
    if (!repeated.isEmpty())
        std::printf("  [%s] note: the HTTP layer re-sent %d cut request(s) of %s by itself\n", currentCase, repeated.size(), qPrintable(id));
    if (failures != failuresBefore) rig.dump(id);
}

static void caseTruncationAndReopen() {
    currentCase = "truncation and reopen";
    Rig rig("trunc");
    rig.open();
    // OFFLINE_ENGINE_REPEAT=n repeats the scenario with fresh ids (diagnosis of rare orderings).
    const int repeat = qMax(1, qgetenv("OFFLINE_ENGINE_REPEAT").toInt());
    for (int i = 0; i < repeat; ++i) truncationOnce(rig, QStringLiteral("trunc%1").arg(i + 1, 6, 10, QLatin1Char('0')), false);
    // The same cuts without a Content-Length: the close ends the body, so the reply finishes without
    // an error and only the byte count shows that it was short. The engine still reopens at the cut.
    truncationOnce(rig, QStringLiteral("nolen000001"), true);
}

// 5. 416 "bytes */total": the part on disk is not satisfiable for the media host; the engine discards
// it and restarts the track from byte 0 instead of failing, and still ends at exactly clen.
static void case416StarTotal() {
    currentCase = "416 */total";
    Rig rig("r416");
    rig.open();
    const QString id = QStringLiteral("range416001");
    rig.add(id, 200000, 7);
    rig.host.fault(rig.name(id), 65536, 416);
    const int item = rig.enqueue(id);
    CHECK(rig.finishes(item));
    CHECK(rig.intact(id));
    const int refused = rig.indexOf(id, 416, 65536);
    CHECK(refused == 1);
    CHECK(rig.indexOf(id, 206, 0, refused + 1) == refused + 1);   // restarted from zero right after the 416
    CHECK(rig.resolver.calls.value(id) == 1);
    CHECK(QFileInfo(rig.file(id)).size() == 200000);
}

// 6. 503 and low space.
// (a) the media host answering 503 is transient: the same address is asked again at the same offset;
// (b) low space: the engine's free-space precheck (NFR-SEC-05 G4) fails the item as "no_space" before
//     any media request, and a retry continues once there is room;
// (c) a symlink to /dev/full planted at the part name (formerly how this case filled the disk) is
//     replaced, never written through (NFR-SEC-05 G1): the item finishes intact.
// Library component edit: the old form of (c), a write through the link, is no longer possible.
static void case503AndLowSpace() {
    currentCase = "503 when space is low";
    Rig rig("r503");
    rig.open();
    {
        const QString id = QStringLiteral("busy5030001");
        rig.add(id, 200000, 8);
        rig.host.fault(rig.name(id), 65536, 503);
        const int item = rig.enqueue(id);
        CHECK(rig.finishes(item));
        CHECK(rig.intact(id));
        CHECK(rig.resolver.calls.value(id) == 1);
        const int busy = rig.indexOf(id, 503, 65536);
        CHECK(busy == 1);
        const QList<Request> list = rig.host.of(rig.name(id));
        CHECK(list.size() > busy + 1 && list[busy + 1].status == 206 && list[busy + 1].start == 65536 && list[busy + 1].tok == 1);
    }
    {
        const QString id = QStringLiteral("lowspace001");
        rig.add(id, 200000, 9);
        rig.freeBytes = 1000;
        const int item = rig.enqueue(id);
        CHECK(waitFor([&]() { const int s = rig.stateOf(item); return s == int(State::Done) || s == int(State::Failed); }));
        CHECK(rig.stateOf(item) == int(State::Failed) && rig.errorOf(item) == QLatin1String(errorCodes::noSpace));
        CHECK(!QFileInfo(rig.file(id)).exists());
        CHECK(rig.host.of(rig.name(id)).isEmpty());             // refused before contacting the host
        spin(200);
        CHECK(rig.stateOf(item) == int(State::Failed));
        rig.freeBytes = -1;
        CHECK(rig.manager->retry(item));
        CHECK(rig.finishes(item));
        CHECK(rig.intact(id));
    }
    {   // Disk full mid-transfer (enospc.so under run.sh): writes past 64 KiB of the part file fail with ENOSPC.
        if (qgetenv("OFFLINE_ENOSPC_SHIM") != "1") {
            std::printf("  [%s] enospc.so not loaded: the mid-transfer ENOSPC half is skipped\n", currentCase);
        } else {
            const QString id = QStringLiteral("enospcmid01");
            rig.add(id, 400000, 11);
            qputenv("OFFLINE_ENOSPC_AFTER", "65536");
            qputenv("OFFLINE_ENOSPC_SUFFIX", ".part");
            const int item = rig.enqueue(id);
            CHECK(waitFor([&]() { const int s = rig.stateOf(item); return s == int(State::Done) || s == int(State::Failed); }));
            qunsetenv("OFFLINE_ENOSPC_SUFFIX");
            CHECK(rig.stateOf(item) == int(State::Failed));
            const QString why = rig.errorOf(item);
            CHECK(why == QLatin1String(errorCodes::write) || why == QLatin1String(errorCodes::noSpace));
            CHECK(!QFileInfo(rig.file(id)).exists());            // no final file from a short write
            spin(200);
            CHECK(rig.stateOf(item) == int(State::Failed));       // no retry loop while the disk is full
            CHECK(rig.manager->retry(item));                       // space again: the retry completes the file
            CHECK(rig.finishes(item));
            CHECK(rig.intact(id));
        }
    }
    if (!QFileInfo(QStringLiteral("/dev/full")).exists()) {
        std::printf("  [%s] /dev/full missing: the planted-link half is skipped\n", currentCase);
        return;
    }
    {
        const QString id = QStringLiteral("diskfull001");
        rig.add(id, 200000, 10);
        QDir().mkpath(rig.dir);
        CHECK(QFile::link(QStringLiteral("/dev/full"), rig.file(id) + QStringLiteral(".part")));
        const int item = rig.enqueue(id);
        CHECK(rig.finishes(item));
        CHECK(rig.intact(id));
        CHECK(!QFileInfo(rig.file(id)).isSymLink());
        CHECK(!QFileInfo(rig.file(id) + QStringLiteral(".part")).exists());
    }
}

// Under run.sh (OFFLINE_ENGINE_REQUIRE_GUARD=1) the network guard must be loaded into this Qt process and
// must stop Qt's own socket layer: a QTcpSocket to 1.1.1.1:443 fails and the guard counts it. Its
// refusal goes to a separate log so that the suite's "no blocked attempt" check stays meaningful.
static bool guardCanary() {
    currentCase = "guard canary";
    typedef int (*Counter)();
    const Counter counter = reinterpret_cast<Counter>(dlsym(RTLD_DEFAULT, "no_network_blocked_count"));
    CHECK(counter != nullptr);
    if (!counter) return false;
    const QByteArray log = qgetenv("NO_NETWORK_LOG");
    qputenv("NO_NETWORK_LOG", qgetenv("TMPDIR") + "/offline-canary.log");
    const int before = counter();
    const int failed = failures;
    QTcpSocket socket;
    socket.connectToHost(QHostAddress(QStringLiteral("1.1.1.1")), 443);
    const bool connected = socket.waitForConnected(3000);
    std::printf("  [guard canary] QTcpSocket to 1.1.1.1:443: %s (%s)\n", connected ? "CONNECTED" : "refused", qPrintable(socket.errorString()));
    CHECK(!connected && counter() > before);   // Qt may try the address more than once
    socket.abort();
    qputenv("NO_NETWORK_LOG", log);
    return failures == failed;
}

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("org.example"));
    QCoreApplication::setApplicationName(QStringLiteral("sailtube-offline-engine"));
    if (!QSqlDatabase::isDriverAvailable(QStringLiteral("QSQLITE"))) {
        std::printf("offline engine: SKIPPED (no QSQLITE driver)\n");
        return 77;
    }
    if (!qgetenv("OFFLINE_ENGINE_REQUIRE_GUARD").isEmpty()) {
        const bool ok = guardCanary();
        std::printf("CASE guardCanary %s\n", ok ? "PASS" : "FAIL");
        if (!ok) return 1;
    }
    struct Case { const char *name; void (*run)(); } cases[] = {
        { "caseRange", caseRange },
        { "case403AtByteZero", case403AtByteZero },
        { "caseYoungTokenWindow", caseYoungTokenWindow },
        { "caseTruncationAndReopen", caseTruncationAndReopen },
        { "case416StarTotal", case416StarTotal },
        { "case503AndLowSpace", case503AndLowSpace },
    };
    const QByteArray only = qgetenv("OFFLINE_ENGINE_ONLY");   // one case by name (diagnosis)
    for (const Case &c : cases) {
        if (!only.isEmpty() && only != c.name) continue;
        const int before = failures;
        c.run();
        std::printf("CASE %s %s\n", c.name, failures == before ? "PASS" : "FAIL");
        std::fflush(stdout);
    }
    std::printf("offline engine test: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
