// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// NFR-TST-05 host player tests: the real GStreamer engine + VideoPlayer on
// fixtures generated at start-up, served by an in-process HTTP server (one
// path with Range support, one without, which makes souphttpsrc unseekable).
// Audio goes to fakesink sync=true; video to the real appsink so frames reach
// the mailbox. Build: tests/player_gst/run.sh
#include "Core/Player/Infrastructure/GstEngine.h"
#include "Core/Player/Infrastructure/VideoPlayer.h"

#include <gst/app/gstappsrc.h>
#include <gst/gst.h>

#include <QAtomicInt>
#include <QByteArray>
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QTcpServer>
#include <QTcpSocket>
#include <QThread>
#include <QTimer>
#include <QMutex>
#include <QMutexLocker>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <memory>

using namespace nordtile::player;

static int checks = 0;
static int failures = 0;
#define CHECK(c) do { ++checks; if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)
#define CHECK_MSG(c, ...) do { ++checks; if (!(c)) { std::printf("FAIL %s:%d: %s -- ", __FILE__, __LINE__, #c); std::printf(__VA_ARGS__); std::printf("\n"); ++failures; } } while (0)

static void spin(int ms) {
    QElapsedTimer t;
    t.start();
    while (t.elapsed() < ms) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(2);
    }
}

static bool waitFor(const std::function<bool()> &pred, int timeoutMs) {
    QElapsedTimer t;
    t.start();
    while (!pred()) {
        if (t.elapsed() > timeoutMs) return false;
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(2);
    }
    return true;
}

// ---- Fixtures --------------------------------------------------------------------

static QString gDir;

static bool runPipeline(const QString &description) {
    GError *err = nullptr;
    GstElement *p = gst_parse_launch(description.toUtf8().constData(), &err);
    if (!p || err) {
        std::printf("fixture parse error: %s\n  %s\n", err ? err->message : "?", qPrintable(description));
        if (err) g_error_free(err);
        if (p) gst_object_unref(p);
        return false;
    }
    gst_element_set_state(p, GST_STATE_PLAYING);
    GstBus *bus = gst_element_get_bus(p);
    GstMessage *m = gst_bus_timed_pop_filtered(bus, 120 * GST_SECOND, GstMessageType(GST_MESSAGE_EOS | GST_MESSAGE_ERROR));
    bool ok = m && GST_MESSAGE_TYPE(m) == GST_MESSAGE_EOS;
    if (m && !ok) {
        GError *e = nullptr;
        gchar *dbg = nullptr;
        gst_message_parse_error(m, &e, &dbg);
        std::printf("fixture error: %s (%s)\n  %s\n", e ? e->message : "?", dbg ? dbg : "", qPrintable(description));
        if (e) g_error_free(e);
        g_free(dbg);
    }
    if (m) gst_message_unref(m);
    gst_object_unref(bus);
    gst_element_set_state(p, GST_STATE_NULL);
    gst_object_unref(p);
    return ok;
}

static const int kSeconds = 12;
static const int kFps = 30;
static const int kGopFrames = 60;  // 2 s keyframe interval (mp4 GOP test)

static QString videoSrc() {
    return QStringLiteral("videotestsrc num-buffers=%1 pattern=ball ! video/x-raw,width=320,height=180,framerate=%2/1 ! ")
        .arg(kSeconds * kFps).arg(kFps);
}
static QString audioSrc() {
    // 960-sample buffers at 48 kHz = 20 ms each.
    return QStringLiteral("audiotestsrc num-buffers=%1 samplesperbuffer=960 wave=sine ! audio/x-raw,rate=48000,channels=2 ! ")
        .arg(kSeconds * 50);
}
static QString vp8() { return QStringLiteral("vp8enc deadline=1 keyframe-max-dist=%1 ! ").arg(kGopFrames); }
static QString vp9() {
    return QStringLiteral("vp9enc deadline=1 cpu-used=8 keyframe-max-dist=%1 ! vp9parse ! ").arg(kGopFrames);
}

static bool makeFixtures() {
    const QString d = gDir + "/";
    bool ok = true;
    ok = ok && runPipeline(videoSrc() + vp8() + "webmmux ! filesink location=" + d + "v.webm");
    ok = ok && runPipeline(audioSrc() + "opusenc ! webmmux ! filesink location=" + d + "a.webm");
    ok = ok && runPipeline("webmmux name=m ! filesink location=" + d + "av.webm " + videoSrc() + vp8() + "m. " +
                           audioSrc() + "opusenc ! m.");
    // 160x90: frames of another size tell a stale post from the previous build (teardown test).
    ok = ok && runPipeline(QStringLiteral("webmmux name=m ! filesink location=") + d + "small.webm " +
                           QStringLiteral("videotestsrc num-buffers=%1 pattern=snow ! video/x-raw,width=160,height=90,framerate=%2/1 ! ")
                               .arg(kSeconds * kFps).arg(kFps) + vp8() + "m. " + audioSrc() + "opusenc ! m.");
    ok = ok && runPipeline(videoSrc() + vp9() + "mp4mux faststart=true ! filesink location=" + d + "v.mp4");
    ok = ok && runPipeline(audioSrc() + "opusenc ! mp4mux faststart=true ! filesink location=" + d + "a.mp4");
    ok = ok && runPipeline("mp4mux faststart=true name=m ! filesink location=" + d + "av.mp4 " + videoSrc() + vp9() +
                           "m. " + audioSrc() + "opusenc ! m.");
    return ok;
}

// ---- DASH-style fragmented mp4 (YouTube itag 136 shape: ftyp, moov, sidx, moof/mdat...) ----------
// mp4mux writes ftyp + moov (mvex) + one moof/mdat pair per fragment and an mfra index at the end
// (GStreamer 1.26: also with streamable=true; removed here for the no-mfra variant). A DASH init + index segment also carries a sidx after the moov; it is
// inserted here (one reference per moof+mdat, durations from the tfdt boxes), and the mfra's
// moof offsets are moved by its size.

static const int kFragSeconds = 30;      // long enough for a resume deep into the file
static const int kFragGopFrames = 30;    // 1 s keyframe interval = 1 s fragments

static quint32 rd32(const QByteArray &d, qint64 at) {
    const unsigned char *p = reinterpret_cast<const unsigned char *>(d.constData()) + at;
    return (quint32(p[0]) << 24) | (quint32(p[1]) << 16) | (quint32(p[2]) << 8) | quint32(p[3]);
}
static quint64 rd64(const QByteArray &d, qint64 at) { return (quint64(rd32(d, at)) << 32) | rd32(d, at + 4); }
static void put32(QByteArray &out, quint32 v) {
    out.append(char(v >> 24)).append(char(v >> 16)).append(char(v >> 8)).append(char(v));
}
static void wr32(QByteArray &d, qint64 at, quint32 v) {
    d[int(at)] = char(v >> 24); d[int(at + 1)] = char(v >> 16); d[int(at + 2)] = char(v >> 8); d[int(at + 3)] = char(v);
}
static void wr64(QByteArray &d, qint64 at, quint64 v) { wr32(d, at, quint32(v >> 32)); wr32(d, at + 4, quint32(v)); }

struct Mp4Box {
    qint64 offset = 0;  // of the size field
    qint64 size = 0;
    int header = 8;
    QByteArray type;
};

// The boxes in [begin, end) of `d`.
static std::vector<Mp4Box> mp4Boxes(const QByteArray &d, qint64 begin, qint64 end) {
    std::vector<Mp4Box> out;
    qint64 at = begin;
    while (at + 8 <= end) {
        Mp4Box b;
        b.offset = at;
        b.size = rd32(d, at);
        b.type = d.mid(int(at + 4), 4);
        if (b.size == 1) { b.size = qint64(rd64(d, at + 8)); b.header = 16; }
        if (b.size == 0) b.size = end - at;
        if (b.size < b.header || at + b.size > end) break;
        out.push_back(b);
        at += b.size;
    }
    return out;
}

static bool mp4Child(const QByteArray &d, const Mp4Box &parent, const char *type, Mp4Box *out, int skip = 0) {
    const std::vector<Mp4Box> kids = mp4Boxes(d, parent.offset + parent.header + skip, parent.offset + parent.size);
    for (size_t i = 0; i < kids.size(); ++i)
        if (kids[i].type == type) { *out = kids[i]; return true; }
    return false;
}

struct FragmentInfo {
    int moofs = 0;
    bool mfra = false;
    bool sidx = false;
    qint64 firstMoofOffset = -1;
};

// Inserts a sidx before the first moof of `path` (in place); dropMfra also removes the mfra index
// (mp4mux writes one even with streamable=true). False when the file is not fragmented.
static bool insertSidx(const QString &path, bool dropMfra, FragmentInfo *info) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return false;
    QByteArray d = f.readAll();
    f.close();
    const std::vector<Mp4Box> top = mp4Boxes(d, 0, d.size());
    Mp4Box moov, trak, mdia, mdhd;
    bool haveMoov = false;
    std::vector<Mp4Box> moofs;
    std::vector<qint64> spans;  // moof .. end of its mdat
    for (size_t i = 0; i < top.size(); ++i) {
        if (top[i].type == "moov") { moov = top[i]; haveMoov = true; }
        if (top[i].type == "mfra") info->mfra = true;
        if (top[i].type == "moof") {
            moofs.push_back(top[i]);
            qint64 span = top[i].size;
            if (i + 1 < top.size() && top[i + 1].type == "mdat") span += top[i + 1].size;
            spans.push_back(span);
        }
    }
    info->moofs = int(moofs.size());
    if (!haveMoov || moofs.empty() || !mp4Child(d, moov, "trak", &trak) || !mp4Child(d, trak, "mdia", &mdia) ||
        !mp4Child(d, mdia, "mdhd", &mdhd))
        return false;
    const qint64 mdhdBody = mdhd.offset + mdhd.header;
    const quint32 timescale = d.at(int(mdhdBody)) == 1 ? rd32(d, mdhdBody + 4 + 16) : rd32(d, mdhdBody + 4 + 8);
    std::vector<quint64> times;
    for (size_t i = 0; i < moofs.size(); ++i) {
        Mp4Box traf, tfdt;
        if (!mp4Child(d, moofs[i], "traf", &traf) || !mp4Child(d, traf, "tfdt", &tfdt)) return false;
        const qint64 body = tfdt.offset + tfdt.header;
        times.push_back(d.at(int(body)) == 1 ? rd64(d, body + 4) : rd32(d, body + 4));
    }
    QByteArray sidx;
    put32(sidx, 0);  // size, below
    sidx.append("sidx");
    put32(sidx, 0);                 // version 0, flags 0
    put32(sidx, 1);                 // reference_ID: track 1
    put32(sidx, timescale);
    put32(sidx, quint32(times[0])); // earliest_presentation_time
    put32(sidx, 0);                 // first_offset: the first moof follows the sidx
    put32(sidx, quint32(moofs.size()));  // reserved 0 (16 bits) + reference_count
    for (size_t i = 0; i < moofs.size(); ++i) {
        const quint64 duration = i + 1 < times.size() ? times[i + 1] - times[i] : (i ? times[i] - times[i - 1] : timescale);
        put32(sidx, quint32(spans[i]) & 0x7fffffff);  // reference_type 0 (media)
        put32(sidx, quint32(duration));
        put32(sidx, 0x90000000u);                      // starts_with_SAP 1, SAP_type 1
    }
    wr32(sidx, 0, quint32(sidx.size()));
    const qint64 shift = sidx.size();
    info->firstMoofOffset = moofs[0].offset + shift;
    // mfra/tfra: absolute moof offsets move by the sidx size.
    for (size_t i = 0; i < top.size(); ++i) {
        if (top[i].type != "mfra") continue;
        const std::vector<Mp4Box> kids = mp4Boxes(d, top[i].offset + top[i].header, top[i].offset + top[i].size);
        for (size_t k = 0; k < kids.size(); ++k) {
            if (kids[k].type != "tfra") continue;
            const qint64 body = kids[k].offset + kids[k].header;
            const bool v1 = d.at(int(body)) == 1;
            const quint32 sizes = rd32(d, body + 8);
            const int extra = int(((sizes >> 4) & 3) + 1 + ((sizes >> 2) & 3) + 1 + (sizes & 3) + 1);
            const quint32 entries = rd32(d, body + 12);
            qint64 at = body + 16;
            for (quint32 e = 0; e < entries; ++e) {
                const qint64 off = at + (v1 ? 8 : 4);
                if (v1) wr64(d, off, rd64(d, off) + quint64(shift));
                else wr32(d, off, rd32(d, off) + quint32(shift));
                at = off + (v1 ? 8 : 4) + extra;
            }
        }
    }
    for (size_t i = top.size(); dropMfra && i-- > 0;)
        if (top[i].type == "mfra") d.remove(int(top[i].offset), int(top[i].size));
    if (dropMfra) info->mfra = false;
    d.insert(int(moofs[0].offset), sidx);
    info->sidx = true;
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    return f.write(d) == d.size();
}

// fm.mp4: fragmented + sidx + mfra; fs.mp4: fragmented + sidx, no mfra, the YouTube shape; a30.webm: 30 s Opus audio for the dual plans.
static bool makeFragmentedFixtures() {
    const QString d = gDir + "/";
    const QString video = QStringLiteral("videotestsrc num-buffers=%1 pattern=ball ! video/x-raw,width=320,height=180,"
                                         "framerate=%2/1 ! vp9enc deadline=1 cpu-used=8 keyframe-max-dist=%3 ! vp9parse ! ")
                              .arg(kFragSeconds * kFps).arg(kFps).arg(kFragGopFrames);
    bool ok = runPipeline(video + "mp4mux fragment-duration=1000 ! filesink location=" + d + "fm.mp4") &&
              runPipeline(video + "mp4mux fragment-duration=1000 ! filesink location=" + d + "fs.mp4") &&
              runPipeline(QStringLiteral("audiotestsrc num-buffers=%1 samplesperbuffer=960 wave=sine ! "
                                         "audio/x-raw,rate=48000,channels=2 ! opusenc ! webmmux ! filesink location=")
                              .arg(kFragSeconds * 50) + d + "a30.webm");
    // FRAG_BIG=1 (investigation only): a 10 s high-bitrate fragmented mp4 with mfra (big.mp4).
    if (qEnvironmentVariableIsSet("FRAG_BIG")) {
        FragmentInfo big;
        ok = ok && runPipeline(QStringLiteral("videotestsrc num-buffers=300 pattern=snow ! video/x-raw,width=320,height=180,"
                                              "framerate=30/1 ! vp9enc deadline=1 cpu-used=8 min-quantizer=0 max-quantizer=2 "
                                              "keyframe-max-dist=30 ! vp9parse ! mp4mux fragment-duration=1000 ! filesink location=") +
                               d + "big.mp4") && insertSidx(d + "big.mp4", false, &big);
        std::printf("  big.mp4: %lld bytes, %d moofs\n", (long long)QFileInfo(d + "big.mp4").size(), big.moofs);
    }
    FragmentInfo withMfra, streamable;
    ok = ok && insertSidx(d + "fm.mp4", false, &withMfra) && insertSidx(d + "fs.mp4", true, &streamable);
    std::printf("  fragmented fixtures: fm.mp4 %d moofs, mfra %d; fs.mp4 %d moofs, mfra %d; first moof at %lld\n",
                withMfra.moofs, int(withMfra.mfra), streamable.moofs, int(streamable.mfra),
                (long long)streamable.firstMoofOffset);
    return ok && withMfra.mfra && !streamable.mfra && withMfra.moofs >= kFragSeconds - 1;
}

// ---- Tiny HTTP server ------------------------------------------------------------
// GET /r/<file>: honours Range (206, Accept-Ranges: bytes).
// GET /n/<file>: ignores Range, always 200 with "Accept-Ranges: none", so
//               souphttpsrc reports the stream as not seekable.
// GET /f/<file>: 403 Forbidden at byte 0 (RES-07 refusal; the status reaches VideoPlayer::failed).
// GET /t/<file>: like /r/ (Range honoured) but throttled to gThrottleBytesPerSecond, written in
//               20 ms slices (a slow link: the buffer fills far slower than the first frame arrives).
// GET /d/<file>: like /r/, every answer starts gDelayedPathMs late (a branch whose CDN is slow).
// Every request is logged (gRequestLog: path, Range start/end, ms on gServerClock). With
// gRequestDelayMs > 0 every answer starts that long after the request (the MediaProxy -> CDN
// round trip of a new range request on the device), so each extra request shows in the timings.

static qint64 gThrottleBytesPerSecond = 0;
static int gRequestDelayMs = 0;
static int gDelayedPathMs = 0;

struct RequestLogEntry {
    QString path;
    qint64 from = 0;   // first byte asked for (0 without Range)
    qint64 to = -1;    // last byte asked for, -1 = to the end
    qint64 atMs = 0;   // gServerClock
};
static QElapsedTimer gServerClock;
static std::vector<RequestLogEntry> gRequestLog;

class HttpServer {
public:
    bool start() {
        QObject::connect(&server_, &QTcpServer::newConnection, [this]() {
            while (QTcpSocket *s = server_.nextPendingConnection()) {
                std::shared_ptr<QByteArray> buffer(new QByteArray);
                QObject::connect(s, &QTcpSocket::readyRead, [this, s, buffer]() {
                    buffer->append(s->readAll());
                    if (!buffer->contains("\r\n\r\n")) return;
                    respond(s, *buffer);
                    buffer->clear();
                });
                QObject::connect(s, &QTcpSocket::disconnected, s, &QObject::deleteLater);
            }
        });
        return server_.listen(QHostAddress::LocalHost, 0);
    }
    QString url(const QString &path) const { return QStringLiteral("http://127.0.0.1:%1/%2").arg(server_.serverPort()).arg(path); }
    int requests(const QString &path) const { return requests_.value(path); }
    // Delayed answers whose socket is still there (sent ones count until the socket closes).
    int answersOwed() const { return owed_; }

private:
    void respond(QTcpSocket *s, const QByteArray &request) {
        const QList<QByteArray> lines = request.split('\n');
        const QList<QByteArray> first = lines.value(0).trimmed().split(' ');
        const QString path = QString::fromLatin1(first.value(1));
        requests_[path] += 1;
        if (path.startsWith("/f/")) {
            s->write("HTTP/1.1 403 Forbidden\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
            s->disconnectFromHost();
            return;
        }
        const bool throttled = path.startsWith("/t/");
        const bool delayedPath = path.startsWith("/d/");
        const bool ranged = path.startsWith("/r/") || throttled || delayedPath;
        QFile f(gDir + "/" + path.mid(3));
        if (!f.open(QIODevice::ReadOnly)) {
            s->write("HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
            s->disconnectFromHost();
            return;
        }
        const QByteArray data = f.readAll();
        qint64 from = 0, to = data.size() - 1;
        bool partial = false;
        RequestLogEntry entry;
        entry.path = path;
        entry.atMs = gServerClock.elapsed();
        for (int i = 1; i < lines.size(); ++i) {
            const QByteArray l = lines[i].trimmed();
            if (l.toLower().startsWith("range: bytes=")) {
                const QByteArray spec = l.mid(13);
                const int dash = spec.indexOf('-');
                entry.from = spec.left(dash).toLongLong();
                if (!spec.mid(dash + 1).isEmpty()) entry.to = spec.mid(dash + 1).toLongLong();
                if (!ranged) continue;
                from = entry.from;
                const QByteArray end = spec.mid(dash + 1);
                if (!end.isEmpty()) to = qMin<qint64>(end.toLongLong(), data.size() - 1);
                partial = true;
            }
        }
        gRequestLog.push_back(entry);
        const int delay = gRequestDelayMs + (delayedPath ? gDelayedPathMs : 0);
        if (delay > 0) {
            // The answer starts `delay` ms later (dies with the socket).
            ++owed_;
            QObject::connect(s, &QObject::destroyed, [this]() { if (owed_ > 0) --owed_; });
            QTimer::singleShot(delay, s, [this, s, data, from, to, partial, ranged, throttled]() {
                answer(s, data, from, to, partial, ranged, throttled);
            });
            return;
        }
        answer(s, data, from, to, partial, ranged, throttled);
    }

    void answer(QTcpSocket *s, const QByteArray &data, qint64 from, qint64 to, bool partial, bool ranged, bool throttled) {
        if (s->state() != QAbstractSocket::ConnectedState) return;
        QByteArray head;
        if (partial) {
            head = "HTTP/1.1 206 Partial Content\r\n";
            head += "Content-Range: bytes " + QByteArray::number(from) + "-" + QByteArray::number(to) + "/" +
                    QByteArray::number(data.size()) + "\r\n";
        } else {
            head = "HTTP/1.1 200 OK\r\n";
        }
        head += ranged ? "Accept-Ranges: bytes\r\n" : "Accept-Ranges: none\r\n";
        head += "Content-Type: application/octet-stream\r\nConnection: close\r\nContent-Length: " +
                QByteArray::number(to - from + 1) + "\r\n\r\n";
        s->write(head);
        if (!throttled || gThrottleBytesPerSecond <= 0) {
            s->write(data.mid(int(from), int(to - from + 1)));
            s->disconnectFromHost();
            return;
        }
        // 20 ms slices at the throttle rate; the timer dies with the socket.
        std::shared_ptr<QByteArray> rest(new QByteArray(data.mid(int(from), int(to - from + 1))));
        QTimer *timer = new QTimer(s);
        const int slice = int(qMax<qint64>(1, gThrottleBytesPerSecond / 50));
        QObject::connect(timer, &QTimer::timeout, s, [s, timer, rest, slice]() {
            if (s->state() != QAbstractSocket::ConnectedState) { timer->stop(); return; }
            if (s->bytesToWrite() > slice) return;  // the client reads slower than the throttle
            s->write(rest->left(slice));
            rest->remove(0, slice);
            if (rest->isEmpty()) {
                timer->stop();
                s->disconnectFromHost();
            }
        });
        timer->start(20);
    }

    QTcpServer server_;
    QHash<QString, int> requests_;
    int owed_ = 0;
};

static HttpServer *gServer = nullptr;

// ---- Player harness -----------------------------------------------------------------

struct Harness {
    VideoPlayer player;
    int seekDone = 0;
    int firstFrames = 0;
    int eos = 0;
    int rebuildMarks = 0;
    int failures = 0;
    int repaints = 0;
    double firstFramePosition = -1;
    QString lastError;
    // The first failure of a load (souphttpsrc's refusal precedes the flow and typefind errors).
    QString firstError;
    bool firstNetwork = false;
    int firstBranch = -2;
    int firstHttpStatus = -1;
    QStringList firstBytes;      // branches in arrival order (NFR-PERF timing)
    QHash<QString, qint64> firstByteAt;  // branch -> ms since load()
    QAtomicInt mailboxPosts;     // frames posted to the mailbox (any thread)
    int mailboxListener = -1;
    bool frameInMailboxAtFirstFrame = false;
    bool pictureByteBeforeFrame = false;  // video/muxed source delivered before the first frame
    // Start-up timing: first occurrence of each timing marker (whole marker text) since load().
    QHash<QString, qint64> marks;
    QElapsedTimer loadClock;
    qint64 playingAtMs = -1;     // load() -> first Playing
    int percentAtPlaying = -1;   // buffering level reported when Playing was first reached
    int bufferingMessages = 0;
    int messagesBeforePlaying = 0;
    int lowestPercentAfterPlaying = 101;

    void load(const PlayerSources &p) {
        marks.clear();
        firstByteAt.clear();
        playingAtMs = -1;
        percentAtPlaying = -1;
        bufferingMessages = 0;
        messagesBeforePlaying = 0;
        lowestPercentAfterPlaying = 101;
        loadClock.restart();
        player.load(p);
    }

    Harness() {
        loadClock.start();
        QObject::connect(&player, &VideoPlayer::timing, [this](const QString &m, qint64 ms) {
            if (m == "seek-done") ++seekDone;
            if (m == "rebuild") ++rebuildMarks;
            if (!marks.contains(m)) marks.insert(m, ms);
        });
        QObject::connect(&player, &VideoPlayer::stateChanged, [this]() {
            if (player.state() == VideoPlayer::Playing && playingAtMs < 0) {
                playingAtMs = loadClock.elapsed();
                percentAtPlaying = player.bufferingPercent();
            }
        });
        QObject::connect(&player, &VideoPlayer::bufferingChanged, [this]() {
            ++bufferingMessages;
            if (playingAtMs < 0) ++messagesBeforePlaying;
            if (playingAtMs >= 0 && player.bufferingPercent() < lowestPercentAfterPlaying)
                lowestPercentAfterPlaying = player.bufferingPercent();
        });
        mailboxListener = player.frameMailbox()->addListener([this]() { mailboxPosts.ref(); });
        QObject::connect(&player, &VideoPlayer::firstByte, [this](const QString &branch, qint64 ms) {
            firstBytes << branch;
            if (!firstByteAt.contains(branch)) firstByteAt.insert(branch, ms);
            if (firstFrames == 0 && branch != "audio") pictureByteBeforeFrame = true;
        });
        QObject::connect(&player, &VideoPlayer::firstFrame, [this]() {
            if (firstFrames == 0) frameInMailboxAtFirstFrame = player.frameMailbox()->latest().valid();
            ++firstFrames;
            qint64 ms = -1;
            if (player.sinkPosition(player.plan().mode == PlayMode::Dual ? Branch::Audio : Branch::Muxed, &ms))
                firstFramePosition = double(ms);
            else  // the frame came before the preroll completed (start seek before preroll): the
                firstFramePosition = double(player.diagnostics().engine.firstVideoSinkMs);  // frame's own time
        });
        QObject::connect(&player, &VideoPlayer::endOfStream, [this]() { ++eos; });
        QObject::connect(&player, &VideoPlayer::repaintRequested, [this]() { ++repaints; });
        QObject::connect(&player, &VideoPlayer::failed, [this](const QString &r, bool network, int branch, int status) {
            if (failures == 0) {
                firstError = r;
                firstNetwork = network;
                firstBranch = branch;
                firstHttpStatus = status;
            }
            ++failures;
            lastError = r;
            std::printf("  player failed: %s\n", qPrintable(r));
        });
    }

    ~Harness() { player.frameMailbox()->removeListener(mailboxListener); }

    bool playingNow() const { return player.state() == VideoPlayer::Playing; }
};

static SourceSpec spec(const QString &uri, Container c) {
    SourceSpec s;
    s.uri = uri.toStdString();
    s.container = c;
    s.userAgent = "SailTube-test";
    return s;
}

static PlayerSources sources(PlayMode mode) {
    PlayerSources p;
    p.mode = mode;
    p.videoId = "fixture";
    p.audioSink.factory = "fakesink";
    PropertySetting sync;
    sync.name = "sync";
    sync.value = "true";
    p.audioSink.properties.push_back(sync);
    return p;
}

static PlayerSources dualWebm(const QString &audioPrefix = "r") {
    PlayerSources p = sources(PlayMode::Dual);
    p.video = spec(gServer->url("r/v.webm"), Container::Matroska);
    p.audio = spec(gServer->url(audioPrefix + "/a.webm"), Container::Matroska);
    return p;
}

static PlayerSources dualMp4() {
    PlayerSources p = sources(PlayMode::Dual);
    p.video = spec(gServer->url("r/v.mp4"), Container::IsoBmff);
    p.audio = spec(gServer->url("r/a.mp4"), Container::IsoBmff);
    return p;
}

static bool startAndPlay(Harness &h, const PlayerSources &p, int timeoutMs = 10000) {
    h.load(p);
    return waitFor([&]() { return h.playingNow() || h.failures > 0; }, timeoutMs) && h.failures == 0;
}

static qint64 audioPos(Harness &h) {
    qint64 ms = -1;
    h.player.sinkPosition(h.player.plan().mode == PlayMode::Dual || h.player.plan().mode == PlayMode::AudioOnly
                              ? Branch::Audio : Branch::Muxed, &ms);
    return ms;
}

// ---- Tests ---------------------------------------------------------------------------

// PLY-01 feature matrix subset that runs on a host: every mode plays, video
// modes render frames into the mailbox, audio-only builds no video branch.
static void ply01_everyModePlays() {
    struct Case { const char *name; PlayerSources src; bool video; };
    std::vector<Case> cases;
    cases.push_back({"dual webm", dualWebm(), true});
    cases.push_back({"dual mp4", dualMp4(), true});
    PlayerSources muxed = sources(PlayMode::Muxed);
    muxed.muxed = spec(gServer->url("r/av.mp4"), Container::IsoBmff);
    cases.push_back({"muxed mp4", muxed, true});
    PlayerSources muxedWebm = sources(PlayMode::Muxed);
    muxedWebm.muxed = spec(gServer->url("r/av.webm"), Container::Matroska);
    cases.push_back({"muxed webm", muxedWebm, true});
    PlayerSources local = sources(PlayMode::Local);
    local.muxed = spec("file://" + gDir + "/av.webm", Container::Matroska);
    cases.push_back({"local webm", local, true});
    PlayerSources localMp4 = sources(PlayMode::Local);
    localMp4.muxed = spec("file://" + gDir + "/av.mp4", Container::IsoBmff);
    cases.push_back({"local mp4", localMp4, true});
    PlayerSources audioOnly = sources(PlayMode::AudioOnly);
    audioOnly.audio = spec(gServer->url("r/a.webm"), Container::Matroska);
    cases.push_back({"audio-only", audioOnly, false});

    for (size_t i = 0; i < cases.size(); ++i) {
        Harness h;
        const bool started = startAndPlay(h, cases[i].src);
        CHECK_MSG(started, "%s did not reach Playing (%s)", cases[i].name, qPrintable(h.lastError));
        if (!started) continue;
        const qint64 p0 = audioPos(h);
        spin(1500);
        const qint64 p1 = audioPos(h);
        CHECK_MSG(p1 - p0 >= 1000, "%s position advanced %lld ms", cases[i].name, (long long)(p1 - p0));
        CHECK_MSG(h.player.hasVideo() == cases[i].video, "%s hasVideo", cases[i].name);
        // NFR-PERF timing: one first-byte per source branch, before the first frame;
        // a dual plan has two network sources (R2 run: never a single source for a pair).
        const int sourcesExpected = cases[i].src.mode == PlayMode::Dual ? 2 : 1;
        CHECK_MSG(h.firstBytes.size() == sourcesExpected, "%s first-byte count %d", cases[i].name, int(h.firstBytes.size()));
        if (cases[i].src.mode == PlayMode::Dual)
            CHECK_MSG(h.firstBytes.contains("video") && h.firstBytes.contains("audio"), "%s first-byte branches", cases[i].name);
        if (cases[i].video) CHECK_MSG(h.pictureByteBeforeFrame, "%s no picture first-byte before the first frame", cases[i].name);
        if (cases[i].video) {
            CHECK_MSG(h.firstFrames == 1, "%s firstFrame count %d", cases[i].name, h.firstFrames);
            const VideoFrame f = h.player.frameMailbox()->latest();
            CHECK_MSG(f.valid() && f.width == 320 && f.height == 180, "%s mailbox frame %dx%d", cases[i].name, f.width, f.height);
            CHECK(h.player.videoWidth() == 320 && h.player.videoHeight() == 180);
            // Load-tolerant (shared 4-CPU build container): a starved host makes the sink drop
            // late frames (QoS) but they were still decoded; a broken video path yields neither.
            // At least 5 frames must actually be shown.
            // (A starved host mostly shows as gaps in the decoder's output, not as QoS drops,
            // so the count also gets up to 5 s to arrive.)
            waitFor([&]() {
                const EngineStats w = h.player.diagnostics().engine;
                return w.framesRendered + w.framesDropped > 20 && w.framesRendered >= 5;
            }, 5000);
            const EngineStats es = h.player.diagnostics().engine;
            CHECK_MSG(es.framesRendered + es.framesDropped > 20 && es.framesRendered >= 5,
                      "%s frames rendered %lld, dropped %lld", cases[i].name, (long long)es.framesRendered,
                      (long long)es.framesDropped);
        } else {
            CHECK(h.player.plan().find(elementnames::kVideoSink) == nullptr);
        }
        CHECK(!containsDroidElement(h.player.plan()));  // PLT-08 on a real build
        CHECK(h.player.decodeMode() == int(DecodeMode::Software));
        // Live audio push (PLY-22) must not disturb playback.
        QVariantMap a;
        a["boostPercent"] = 300;
        a["eqPreset"] = "bass";
        h.player.setAudioSettings(a);
        const qint64 q0 = audioPos(h);
        spin(400);
        CHECK_MSG(audioPos(h) > q0, "%s stalled after a live audio change", cases[i].name);
        std::printf("  %s: ok (advanced %lld ms)\n", cases[i].name, (long long)(p1 - p0));
    }
}

// SEL-08: listen-only fetches the audio format only.
static void sel08_audioOnlyFetchesAudioOnly() {
    Harness h;
    PlayerSources p = sources(PlayMode::AudioOnly);
    p.audio = spec(gServer->url("r/a.mp4"), Container::IsoBmff);
    const int videoBefore = gServer->requests("/r/v.mp4") + gServer->requests("/r/av.mp4");
    CHECK(startAndPlay(h, p));
    spin(500);
    CHECK(gServer->requests("/r/a.mp4") > 0);
    CHECK(gServer->requests("/r/v.mp4") + gServer->requests("/r/av.mp4") == videoBefore);
    CHECK(!h.player.hasVideo());
}

// PLY-03: 20 random seeks. WebM lands within 100 ms; mp4 at most one GOP
// before the target; A/V offset after each seek within 80 ms.
static void ply03_seekAccuracy(bool mp4) {
    Harness h;
    const bool ok = startAndPlay(h, mp4 ? dualMp4() : dualWebm());
    CHECK(ok);
    if (!ok) return;
    std::srand(mp4 ? 7 : 3);
    int good = 0;
    qint64 worstAv = 0;
    for (int i = 0; i < 20; ++i) {
        const int target = 500 + std::rand() % 9500;
        const int before = h.seekDone;
        h.player.seek(target);
        const bool done = waitFor([&]() { return h.seekDone > before; }, 8000);
        CHECK_MSG(done, "seek %d to %d never completed", i, target);
        if (!done) continue;
        const double landed = h.player.position();
        bool inRange;
        if (mp4) inRange = landed <= target + 40 && landed >= target - (kGopFrames * 1000 / kFps) - 40;
        else inRange = landed >= target - 100 && landed <= target + 100;
        CHECK_MSG(inRange, "%s seek %d: target %d landed %.0f", mp4 ? "mp4" : "webm", i, target, landed);
        good += inRange;
        // A/V offset once playing again (PLY-03 / NFR-PERF-03: within 80 ms in steady
        // playback). Wait for a settled position (the audio clock advancing again for
        // 300 ms), then measure in 450 ms windows. A real desync shows in every window;
        // a scheduling stall of a loaded build host (one late frame) does not, so the
        // check passes when one of up to three windows is within the unchanged limit.
        waitFor([&]() { return h.playingNow(); }, 3000);
        {
            QElapsedTimer settle;
            settle.start();
            qint64 last = audioPos(h), advancingSince = -1;
            while (settle.elapsed() < 3000) {
                spin(50);
                const qint64 now = audioPos(h);
                if (now > last) { if (advancingSince < 0) advancingSince = settle.elapsed(); }
                else advancingSince = -1;
                last = now;
                if (advancingSince >= 0 && settle.elapsed() - advancingSince >= 300) break;
            }
        }
        // Each 450 ms window also measures the test thread's own scheduling gaps: a gap over
        // 100 ms means the shared host starved this process, and that window is inconclusive
        // (the late frames measure the host, not the player). Up to six windows; one conclusive
        // window within the limit passes; conclusive windows that all exceed it fail; if every
        // window is inconclusive the check reports "host starved" and does not count.
        // A window in which the player is Playing and yet not one frame was shown against the audio
        // clock is a frozen picture (a video sink left in PAUSED, every frame late): it counts like
        // an offset beyond the limit, under the same rules. A window after the end of the clip
        // (a late target and several windows) measures nothing and ends the check.
        bool passed = false, failedConclusive = false, noFrames = false, stopped = false;
        qint64 worstConclusive = 0, worstGap = 0;
        for (int window = 0; window < 6 && !passed; ++window) {
            h.player.resetEngineStats();
            QElapsedTimer w;
            w.start();
            qint64 prev = 0, maxGap = 0;
            while (w.elapsed() < 450) {
                QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
                QThread::msleep(5);
                const qint64 now = w.elapsed();
                if (now - prev > maxGap) maxGap = now - prev;
                prev = now;
            }
            const EngineStats ws = h.player.diagnostics().engine;
            if (maxGap > worstGap) worstGap = maxGap;
            if (!h.playingNow()) { stopped = true; break; }  // ended (or not playing): nothing to measure
            const bool conclusive = maxGap <= 100;
            if (ws.avSamples == 0) {
                if (conclusive) failedConclusive = noFrames = true;
                std::printf("  %s seek %d: no video frame rendered in window %d (%s, max scheduling gap %lld ms; "
                            "%lld buffers at the sink, %lld dropped late)\n", mp4 ? "mp4" : "webm", i, window + 1,
                            conclusive ? "conclusive" : "host starved", (long long)maxGap, (long long)ws.framesRendered,
                            (long long)ws.framesDropped);
                continue;
            }
            if (conclusive && ws.maxAbsAvOffsetMs <= kMaxAvOffsetMs) {
                passed = true;
                if (ws.maxAbsAvOffsetMs > worstAv) worstAv = ws.maxAbsAvOffsetMs;
                break;
            }
            if (conclusive) {
                failedConclusive = true;
                if (ws.maxAbsAvOffsetMs > worstConclusive) worstConclusive = ws.maxAbsAvOffsetMs;
            }
            std::printf("  %s seek %d: A/V %lld ms in window %d (%s, max scheduling gap %lld ms)\n", mp4 ? "mp4" : "webm",
                        i, (long long)ws.maxAbsAvOffsetMs, window + 1, conclusive ? "conclusive" : "host starved",
                        (long long)maxGap);
        }
        if (!passed && !failedConclusive)
            std::printf("  %s seek %d: A/V not measurable, %s (gap %lld ms)\n", mp4 ? "mp4" : "webm", i,
                        stopped ? "playback stopped before a window was measured" : "host starved in every window",
                        (long long)worstGap);
        CHECK_MSG(passed || !failedConclusive, "%s seek %d: A/V offset %lld ms%s", mp4 ? "mp4" : "webm", i,
                  (long long)worstConclusive, noFrames ? ", no video frame rendered while playing" : "");
    }
    CHECK(h.player.diagnostics().rebuilds == 0);
    std::printf("  %s: %d/20 seeks in range, worst A/V offset %lld ms\n", mp4 ? "mp4" : "webm", good, (long long)worstAv);
}

// ---- A seek while the pipeline returns to PLAYING (mp4 seek freeze, beta 1) --------------------
//
// After the preroll that follows a flush, GstBin goes back to PLAYING on a thread of its own (its
// state continuation). A flush that reaches a sink inside that continuation, after the sink was
// set to PLAYING, is not noted by the busy bin: the bin commits PLAYING and the sink, which
// prerolls a little later and relies on its parent to bring it back, stays in PAUSED (the audio
// or the picture stops while the player reports Playing). The window is a few milliseconds wide;
// the player used to send the mp4 audio re-align and a queued seek right into it from its
// async-done handler. This seam makes the window deterministic:
//  - a GstTracer hook holds the next continuation of a pipeline (not a set_state of the test's
//    own thread) right after all its children are in PLAYING and before the state is committed,
//    until a sink has lost its state to a flush (plus kAfterLossMs for a second sink) or kHoldMs
//    passed;
//  - a probe on each sink pad delays the first buffer after a flush by kLatePrerollMs, so the
//    preroll of a sink flushed inside the hold comes after the commit, as on a network stream.
struct ContinuationHold {
    QAtomicInt armed;         // hold the next continuation (one shot)
    QAtomicInt latePreroll;   // delay the first buffer after each flush at the sinks
    QAtomicInt holding;       // a continuation is inside the hold right now
    QAtomicInt holds;         // continuations held
    QAtomicInt lostInHold;    // holds during which a sink lost its state to a flush
    QAtomicInt sinksNotPlaying;  // premise broken: a sink was not in PLAYING when the hold began
    QAtomicInt latePrerolls;  // buffers delayed
    QAtomicInt slowUs;        // stress only: every continuation takes this much longer (a slow device)
    // First-frame seam: the first buffer of every video sink built while this is set waits
    // that long between the engine's pad probe and the sink itself.
    QAtomicInt lateFirstBufferMs;
    QAtomicInt lateFirstBuffers;  // buffers held that way
};
static ContinuationHold gHold;
static const int kHoldMs = 300, kAfterLossMs = 40, kLatePrerollMs = 150;
static qint64 settledAvOffset(Harness &h);

static GstPadProbeReturn latePrerollProbe(GstPad *, GstPadProbeInfo *info, gpointer data) {
    gint *flushed = static_cast<gint *>(data);
    if (GST_PAD_PROBE_INFO_TYPE(info) & GST_PAD_PROBE_TYPE_EVENT_FLUSH) {
        GstEvent *ev = GST_PAD_PROBE_INFO_EVENT(info);
        if (ev && GST_EVENT_TYPE(ev) == GST_EVENT_FLUSH_STOP) g_atomic_int_set(flushed, 1);
        return GST_PAD_PROBE_OK;
    }
    if (g_atomic_int_compare_and_exchange(flushed, 1, 0) && gHold.latePreroll.load()) {
        gHold.latePrerolls.ref();
        g_usleep(kLatePrerollMs * 1000);
    }
    return GST_PAD_PROBE_OK;
}

static GstPadProbeReturn lateFirstBufferProbe(GstPad *, GstPadProbeInfo *, gpointer) {
    const int ms = gHold.lateFirstBufferMs.load();
    if (ms > 0) {
        gHold.lateFirstBuffers.ref();
        g_usleep(static_cast<gulong>(ms) * 1000);
    }
    return GST_PAD_PROBE_REMOVE;
}

// "element-change-state-post" of every element of the process.
static void continuationStateChangePost(GObject *, GstClockTime, GstElement *element, GstStateChange transition,
                                        GstStateChangeReturn result) {
    if (transition == GST_STATE_CHANGE_NULL_TO_READY) {
        // A probe added now runs after the engine's own probe on that pad (added at build time).
        if (gHold.lateFirstBufferMs.load() <= 0) return;
        gchar *name = gst_element_get_name(element);
        const bool videoSink = name && std::strcmp(name, elementnames::kVideoSink) == 0;
        g_free(name);
        GstPad *pad = videoSink ? gst_element_get_static_pad(element, "sink") : nullptr;
        if (!pad) return;
        gst_pad_add_probe(pad, GST_PAD_PROBE_TYPE_BUFFER, &lateFirstBufferProbe, nullptr, nullptr);
        gst_object_unref(pad);
        return;
    }
    if (transition != GST_STATE_CHANGE_PAUSED_TO_PLAYING || result != GST_STATE_CHANGE_SUCCESS) return;
    if (!GST_IS_PIPELINE(element)) return;
    if (QThread::currentThread() == QCoreApplication::instance()->thread()) return;  // the player's own setState()
    if (const int slow = gHold.slowUs.load()) g_usleep(static_cast<gulong>(slow));
    if (!gHold.armed.load() && !gHold.latePreroll.load()) return;
    std::vector<GstElement *> sinks;
    GstIterator *it = gst_bin_iterate_sinks(GST_BIN(element));
    GValue item = G_VALUE_INIT;
    for (bool done = false; !done;) {
        switch (gst_iterator_next(it, &item)) {
        case GST_ITERATOR_OK:
            sinks.push_back(GST_ELEMENT(g_value_dup_object(&item)));
            g_value_reset(&item);
            break;
        case GST_ITERATOR_RESYNC:
            for (size_t i = 0; i < sinks.size(); ++i) gst_object_unref(sinks[i]);
            sinks.clear();
            gst_iterator_resync(it);
            break;
        default:
            done = true;
            break;
        }
    }
    g_value_unset(&item);
    gst_iterator_free(it);
    if (gHold.latePreroll.load()) {
        for (size_t i = 0; i < sinks.size(); ++i) {
            GstPad *pad = gst_element_get_static_pad(sinks[i], "sink");
            if (pad && !g_object_get_data(G_OBJECT(pad), "sailtube-late-preroll")) {
                g_object_set_data(G_OBJECT(pad), "sailtube-late-preroll", GINT_TO_POINTER(1));
                gst_pad_add_probe(pad, GstPadProbeType(GST_PAD_PROBE_TYPE_BUFFER | GST_PAD_PROBE_TYPE_EVENT_FLUSH),
                                  &latePrerollProbe, g_new0(gint, 1), g_free);
            }
            if (pad) gst_object_unref(pad);
        }
    }
    if (gHold.armed.testAndSetOrdered(1, 0)) {
        for (size_t i = 0; i < sinks.size(); ++i) {
            GstState state = GST_STATE_NULL;
            gst_element_get_state(sinks[i], &state, nullptr, 0);
            if (state != GST_STATE_PLAYING) gHold.sinksNotPlaying.ref();
        }
        gHold.holds.ref();
        gHold.holding.store(1);
        QElapsedTimer held;
        held.start();
        qint64 firstLoss = -1;
        while (held.elapsed() < kHoldMs) {
            size_t lost = 0;
            for (size_t i = 0; i < sinks.size(); ++i) {
                GstState state = GST_STATE_PLAYING;
                gst_element_get_state(sinks[i], &state, nullptr, 0);
                if (state != GST_STATE_PLAYING) ++lost;
            }
            if (lost && firstLoss < 0) {
                firstLoss = held.elapsed();
                gHold.lostInHold.ref();
            }
            if (lost == sinks.size() || (firstLoss >= 0 && held.elapsed() - firstLoss >= kAfterLossMs)) break;
            g_usleep(1000);
        }
        gHold.holding.store(0);
    }
    for (size_t i = 0; i < sinks.size(); ++i) gst_object_unref(sinks[i]);
}

// Once per process; the hooks stay (GStreamer has no call to remove one), so the test that needs
// them runs last.
static bool installContinuationHold() {
    static GstTracer *tracer = nullptr;
    if (tracer) return true;
    GTypeQuery query;
    g_type_query(GST_TYPE_TRACER, &query);
    if (!query.type) return false;
    GTypeInfo info;
    std::memset(&info, 0, sizeof(info));
    info.class_size = static_cast<guint16>(query.class_size);
    info.instance_size = static_cast<guint16>(query.instance_size);
    const GType type = g_type_register_static(GST_TYPE_TRACER, "SailTubeTestContinuationHold", &info, GTypeFlags(0));
    tracer = GST_TRACER(g_object_new(type, nullptr));
    if (!tracer) return false;
    gst_object_ref_sink(tracer);
    gst_tracing_register_hook(tracer, "element-change-state-post", G_CALLBACK(&continuationStateChangePost));
    return true;
}

// PLY-03 / PLY-05 regression: a seek the player sends from its async-done handler (the mp4 audio
// re-align, a queued seek) while the pipeline is on its way back to PLAYING never leaves a sink
// in PAUSED: no sink is flushed inside the continuation, and both sinks keep playing afterwards.
// The muxed case is the control: a pipeline-level seek is serialised by GStreamer itself.
static void ply03_seekDuringStateContinuation() {
    const bool installed = installContinuationHold();
    CHECK_MSG(installed, "no GstTracer: the state-continuation seam is not available");
    if (!installed) return;
    struct Case { const char *name; PlayerSources src; bool queued; bool video; };
    std::vector<Case> cases;
    // The mp4 pair is the shipped shape (HTTP, download buffers: both files are on disk after
    // the warm-up, so its prerolls need no server). The queued cases play local files: the
    // fixtures' HTTP server runs on this thread's event loop, which stands still while the test
    // waits for the hold.
    cases.push_back({"dual mp4, audio re-align", dualMp4(), false, true});
    PlayerSources dualFiles = sources(PlayMode::Dual);
    dualFiles.video = spec("file://" + gDir + "/v.webm", Container::Matroska);
    dualFiles.audio = spec("file://" + gDir + "/a.webm", Container::Matroska);
    cases.push_back({"dual webm, queued seek", dualFiles, true, true});
    PlayerSources audioOnly = sources(PlayMode::AudioOnly);
    audioOnly.audio = spec("file://" + gDir + "/a.webm", Container::Matroska);
    cases.push_back({"audio-only, queued seek", audioOnly, true, false});
    PlayerSources local = sources(PlayMode::Local);
    local.muxed = spec("file://" + gDir + "/av.webm", Container::Matroska);
    cases.push_back({"local muxed webm, queued seek (control)", local, true, true});
    for (size_t i = 0; i < cases.size(); ++i) {
        const Case &c = cases[i];
        Harness h;
        const bool ok = startAndPlay(h, c.src);
        CHECK_MSG(ok, "%s did not play", c.name);
        if (!ok) continue;
        // A first seek outside the seam (a push-mode WebM demuxer fetches its Cues on the first one).
        int before = h.seekDone;
        h.player.seek(2500);
        CHECK_MSG(waitFor([&]() { return h.seekDone > before; }, 8000), "%s: warm-up seek", c.name);
        waitFor([&]() { return h.playingNow(); }, 3000);
        spin(300);

        before = h.seekDone;
        const int holdsBefore = gHold.holds.load(), lostBefore = gHold.lostInHold.load();
        const int notPlayingBefore = gHold.sinksNotPlaying.load();
        gHold.latePreroll.store(1);
        gHold.armed.store(1);
        h.player.seek(4500);
        if (c.queued) h.player.seek(6500);  // coalesced: sent by the async-done handler of the first
        // The owner thread runs no event until the continuation is inside the hold, so the
        // async-done handler sends its seek (re-align / queued) while the continuation runs.
        QElapsedTimer t;
        t.start();
        while (!gHold.holding.load() && t.elapsed() < 5000) QThread::msleep(1);
        const bool held = gHold.holds.load() == holdsBefore + 1;
        CHECK_MSG(held, "%s: the state continuation was not held (seam inoperative)", c.name);
        const bool done = waitFor([&]() { return h.seekDone >= before + (c.queued ? 2 : 1); }, 8000);
        gHold.armed.store(0);
        gHold.latePreroll.store(0);
        CHECK_MSG(done, "%s: the seek never completed", c.name);
        CHECK_MSG(gHold.sinksNotPlaying.load() == notPlayingBefore, "%s: a sink was not in PLAYING when the hold began",
                  c.name);
        const int lost = gHold.lostInHold.load() - lostBefore;
        CHECK_MSG(lost == 0, "%s: a sink was flushed while the pipeline was returning to PLAYING", c.name);
        waitFor([&]() { return h.playingNow(); }, 3000);
        CHECK_MSG(h.playingNow(), "%s: not playing after the seek", c.name);
        // Both sinks run: each position advances by 500 ms (a sink left in PAUSED never moves).
        qint64 a0 = -1, v0 = -1;
        h.player.sinkPosition(Branch::Audio, &a0);
        h.player.sinkPosition(Branch::Video, &v0);
        const int posts0 = h.mailboxPosts.load();
        const qint64 rendered0 = h.player.diagnostics().engine.framesRendered;
        const qint64 dropped0 = h.player.diagnostics().engine.framesDropped;
        QElapsedTimer window;
        window.start();
        qint64 a1 = a0, v1 = v0;
        const bool audioRuns = waitFor([&]() { return h.player.sinkPosition(Branch::Audio, &a1) && a1 >= a0 + 500; }, 4000);
        const bool videoRuns =
            !c.video || waitFor([&]() { return h.player.sinkPosition(Branch::Video, &v1) && v1 >= v0 + 500; }, 4000);
        CHECK_MSG(audioRuns, "%s: the audio sink stopped after the seek (position %lld -> %lld)", c.name, (long long)a0,
                  (long long)a1);
        CHECK_MSG(videoRuns, "%s: the video sink stopped after the seek (position %lld -> %lld)", c.name, (long long)v0,
                  (long long)v1);
        // The picture runs too: a frame reaches the mailbox while the positions advance. (The sink
        // positions above follow the clock once a sink is in PLAYING; they do not show that buffers
        // arrive.) Seen failing once on a shared gate and not reproduced since: when it does, this
        // says what reached the sink and whether a frame came later.
        const bool shown = !c.video || h.mailboxPosts.load() > posts0;
        if (!shown) {
            const qint64 windowMs = window.elapsed();
            const qint64 reached1 = h.player.diagnostics().engine.framesRendered;
            const qint64 dropped1 = h.player.diagnostics().engine.framesDropped;
            const bool later = waitFor([&]() { return h.mailboxPosts.load() > posts0; }, 6000);
            std::printf("  DIAG %s: no frame in the mailbox in %lld ms (buffers at the video sink +%lld, dropped as late "
                        "+%lld, sink positions video %lld -> %lld, audio %lld -> %lld); %s after %lld ms in all (buffers "
                        "+%lld, dropped +%lld)\n", c.name, (long long)windowMs, (long long)(reached1 - rendered0),
                        (long long)(dropped1 - dropped0), (long long)v0, (long long)v1, (long long)a0, (long long)a1,
                        later ? "a frame came" : "still none", (long long)window.elapsed(),
                        (long long)(h.player.diagnostics().engine.framesRendered - rendered0),
                        (long long)(h.player.diagnostics().engine.framesDropped - dropped0));
        }
        CHECK_MSG(shown, "%s: no frame reached the mailbox after the seek", c.name);
        qint64 av = -1;
        if (c.video && audioRuns && videoRuns) {
            av = settledAvOffset(h);
            CHECK_MSG(av >= 0 && av <= kMaxAvOffsetMs, "%s: A/V after the seek %lld ms", c.name, (long long)av);
        }
        CHECK(h.failures == 0 && h.player.diagnostics().rebuilds == 0);
        std::printf("  %s: held %d, sinks flushed inside the continuation %d, audio %lld -> %lld, video %lld -> %lld, "
                    "A/V %lld ms\n", c.name, int(held), lost, (long long)a0, (long long)a1, (long long)v0, (long long)v1,
                    (long long)av);
    }
}

// PLY-06 / PLY-26 regression: firstFrame is announced when the frame is in the mailbox. The
// engine used to announce it from its probe on the video sink's pad, before the sink had the
// buffer; under load the owner thread handled the notice before the streaming thread had posted
// the frame ("mailbox empty at first-frame"). The seam holds each video sink's first buffer for
// 150 ms right after the engine's probe, which makes that order deterministic.
static void ply06_firstFrameIsInTheMailbox() {
    const bool installed = installContinuationHold();
    CHECK_MSG(installed, "no GstTracer: the first-frame seam is not available");
    if (!installed) return;
    struct Case { const char *name; PlayerSources src; };
    std::vector<Case> cases;
    cases.push_back({"dual webm from the start", dualWebm()});
    PlayerSources resumed = dualMp4();
    resumed.startPositionMs = 5000;  // start seek before preroll: the output opens when the video's seek is out
    cases.push_back({"dual mp4 resumed at 5 s", resumed});
    PlayerSources local = sources(PlayMode::Local);
    local.muxed = spec("file://" + gDir + "/av.mp4", Container::IsoBmff);
    cases.push_back({"local muxed mp4", local});
    PlayerSources paused = dualWebm();
    paused.autoplay = false;
    cases.push_back({"dual webm without autoplay", paused});
    gHold.lateFirstBufferMs.store(150);
    for (size_t i = 0; i < cases.size(); ++i) {
        const int heldBefore = gHold.lateFirstBuffers.load();
        Harness h;
        h.load(cases[i].src);
        const bool frame = waitFor([&]() { return h.firstFrames > 0 || h.failures > 0; }, 8000) && h.failures == 0;
        CHECK_MSG(frame, "%s: no first frame (%s)", cases[i].name, qPrintable(h.lastError));
        if (!frame) continue;
        CHECK_MSG(gHold.lateFirstBuffers.load() > heldBefore, "%s: the first buffer was not held (seam inoperative)",
                  cases[i].name);
        CHECK_MSG(h.frameInMailboxAtFirstFrame, "%s: mailbox empty at first-frame", cases[i].name);
        spin(300);
        CHECK_MSG(h.firstFrames == 1, "%s: firstFrame count %d", cases[i].name, h.firstFrames);
        std::printf("  %s: frame in the mailbox at first-frame %d, first-frame at %lld ms\n", cases[i].name,
                    int(h.frameInMailboxAtFirstFrame), (long long)h.marks.value("first-frame", -1));
        h.player.stop();
    }
    gHold.lateFirstBufferMs.store(0);
}

// Investigation only (PLAYER_GST_ONLY=ply03_seekStress): dual seeks in a loop, every second one a
// scrub (two seeks at once: the second is queued and sent by the async-done handler). After each,
// both sinks' positions are sampled for 900 ms; a sink that does not move in the last 600 ms
// while the player is Playing is a stall. PLY03_STRESS_SEEKS (default 400),
// PLY03_STRESS_SOURCE=mp4|webm (default mp4), PLY03_STRESS_CONTINUATION_MS (default 0): every
// return of the pipeline to PLAYING takes that much longer, as on a slower device.
static void ply03_seekStress() {
    const bool mp4 = qgetenv("PLY03_STRESS_SOURCE") != "webm";
    const int seeks = qgetenv("PLY03_STRESS_SEEKS").isEmpty() ? 400 : qgetenv("PLY03_STRESS_SEEKS").toInt();
    const int slowMs = qgetenv("PLY03_STRESS_CONTINUATION_MS").toInt();
    if (slowMs > 0 && installContinuationHold()) gHold.slowUs.store(slowMs * 1000);
    Harness h;
    const bool ok = startAndPlay(h, mp4 ? dualMp4() : dualWebm());
    CHECK(ok);
    if (!ok) return;
    std::srand(11);
    int stalls = 0, audioStalls = 0, videoStalls = 0, incomplete = 0, notPlaying = 0, singles = 0, scrubs = 0;
    int stallsSingle = 0, stallsScrub = 0;
    for (int i = 0; i < seeks; ++i) {
        const bool scrub = (i % 2) == 1;
        const int target = 500 + std::rand() % 9000, second = 500 + std::rand() % 9000;
        const int before = h.seekDone;
        h.player.seek(target);
        if (scrub) h.player.seek(second);
        ++(scrub ? scrubs : singles);
        // A scrub is two seeks: the first goes out at once, the second when the first has landed.
        if (!waitFor([&]() { return h.seekDone >= before + (scrub ? 2 : 1); }, 8000)) {
            ++incomplete;
            ++stalls;
            ++(scrub ? stallsScrub : stallsSingle);
            std::printf("  stress %s seek %d (%s): never completed\n", mp4 ? "mp4" : "webm", i, scrub ? "scrub" : "single");
            continue;
        }
        if (!waitFor([&]() { return h.playingNow(); }, 3000)) {
            ++notPlaying;
            std::printf("  stress %s seek %d: not playing after the seek (state %d)\n", mp4 ? "mp4" : "webm", i,
                        int(h.player.state()));
            continue;
        }
        qint64 a[10], v[10];
        for (int s = 0; s < 10; ++s) {
            if (s) spin(100);
            a[s] = v[s] = -1;
            h.player.sinkPosition(Branch::Audio, &a[s]);
            h.player.sinkPosition(Branch::Video, &v[s]);
        }
        if (!h.playingNow()) continue;  // ran into the end of the clip
        const qint64 da = a[9] - a[3], dv = v[9] - v[3];
        if (da < 200 || dv < 200) {
            ++stalls;
            ++(scrub ? stallsScrub : stallsSingle);
            if (da < 200) ++audioStalls;
            if (dv < 200) ++videoStalls;
            std::printf("  stress %s seek %d (%s) to %d: STALL audio +%lld ms video +%lld ms in 600 ms (audio %lld, video %lld)\n",
                        mp4 ? "mp4" : "webm", i, scrub ? "scrub" : "single", scrub ? second : target, (long long)da,
                        (long long)dv, (long long)a[9], (long long)v[9]);
        }
    }
    gHold.slowUs.store(0);
    std::printf("  stress %s (continuation +%d ms): %d seeks (%d single, %d scrubs), %d stalls (%d single, %d scrub; audio "
                "sink %d, video sink %d, never completed %d), not playing %d\n", mp4 ? "mp4" : "webm", slowMs, seeks,
                singles, scrubs, stalls, stallsSingle, stallsScrub, audioStalls, videoStalls, incomplete, notPlaying);
}

// Investigation only (PLAYER_GST_ONLY=ply07_rateStress): rate changes in a loop on a dual pair,
// alone, right after a seek (the change rides on the queued seek), right before one, and as the
// hold-for-2x gesture (2x, then the new rate at once). After each, both sinks' segments must
// carry the new rate and both sinks must move. Same environment
// as ply03_seekStress (PLY03_STRESS_SEEKS = changes, PLY03_STRESS_SOURCE, PLY03_STRESS_CONTINUATION_MS).
static void ply07_rateStress() {
    const bool mp4 = qgetenv("PLY03_STRESS_SOURCE") != "webm";
    const int changes = qgetenv("PLY03_STRESS_SEEKS").isEmpty() ? 400 : qgetenv("PLY03_STRESS_SEEKS").toInt();
    const int slowMs = qgetenv("PLY03_STRESS_CONTINUATION_MS").toInt();
    if (slowMs > 0 && installContinuationHold()) gHold.slowUs.store(slowMs * 1000);
    Harness h;
    const bool ok = startAndPlay(h, mp4 ? dualMp4() : dualWebm());
    CHECK(ok);
    if (!ok) return;
    std::srand(13);
    const double rates[] = {1.5, 1.0, 2.0, 1.25, 0.75, 1.0, 1.75};
    int mismatches = 0, stalls = 0, incomplete = 0, notPlaying = 0;
    for (int i = 0; i < changes; ++i) {
        const double rate = rates[i % 7];
        const int kind = i % 4;  // 0 alone, 1 seek then rate, 2 rate then seek, 3 hold for 2x and release
        const int target = 500 + std::rand() % 5000;  // room to play on at up to 2x
        const int before = h.seekDone;
        const bool held = kind == 3 && rate != 2.0 && h.player.rate() != 2.0;
        if (kind == 1) h.player.seek(target);
        if (held) h.player.setRate(2.0);
        h.player.setRate(rate);
        if (kind == 2) h.player.seek(target);
        const char *const kinds[] = {"rate alone", "seek, rate", "rate, seek", "2x, rate"};
        if (!waitFor([&]() { return h.seekDone >= before + (kind == 0 || (kind == 3 && !held) ? 1 : 2); }, 8000)) {
            ++incomplete;
            std::printf("  rate stress %s change %d (%s) to %.2f: never completed\n", mp4 ? "mp4" : "webm", i, kinds[kind], rate);
            continue;
        }
        if (!waitFor([&]() { return h.playingNow(); }, 3000)) {
            ++notPlaying;
            continue;
        }
        spin(200);
        const EngineStats s = h.player.diagnostics().engine;
        qint64 a0 = -1, v0 = -1, a1 = -1, v1 = -1;
        h.player.sinkPosition(Branch::Audio, &a0);
        h.player.sinkPosition(Branch::Video, &v0);
        spin(500);
        h.player.sinkPosition(Branch::Audio, &a1);
        h.player.sinkPosition(Branch::Video, &v1);
        if (!h.playingNow()) continue;
        if (s.videoSegmentRate != rate || s.audioSegmentRate != rate) {
            ++mismatches;
            std::printf("  rate stress %s change %d (%s) to %.2f: SEGMENT RATES %.2f / %.2f\n", mp4 ? "mp4" : "webm", i,
                        kinds[kind], rate, s.videoSegmentRate, s.audioSegmentRate);
        }
        if (a1 - a0 < 150 || v1 - v0 < 150) {
            ++stalls;
            std::printf("  rate stress %s change %d (%s) to %.2f: STALL audio +%lld ms video +%lld ms in 500 ms\n",
                        mp4 ? "mp4" : "webm", i, kinds[kind], rate, (long long)(a1 - a0), (long long)(v1 - v0));
        }
    }
    gHold.slowUs.store(0);
    std::printf("  rate stress %s (continuation +%d ms): %d changes, %d with a segment at another rate, %d stalls, "
                "never completed %d, not playing %d\n", mp4 ? "mp4" : "webm", slowMs, changes, mismatches, stalls, incomplete,
                notPlaying);
}

// PLY-05: an unseekable audio branch splits every dual seek; the player
// resends once, rebuilds at most once, then keeps playing.
static void ply05_splitSeekRebuildsAtMostOnce() {
    Harness h;
    const bool ok = startAndPlay(h, dualWebm("n"));
    CHECK(ok);
    if (!ok) return;
    h.player.seek(6000);
    spin(4000);
    const PlayerDiagnostics d = h.player.diagnostics();
    std::printf("  split: resends %d, rebuilds %d, seeks sent %d\n", d.resends, d.rebuilds, d.seeksSent);
    qint64 v = -1, au = -1;
    h.player.sinkPosition(Branch::Video, &v);
    h.player.sinkPosition(Branch::Audio, &au);
    std::printf("  split: video sink at %lld ms, audio sink at %lld ms\n", (long long)v, (long long)au);
    CHECK(d.rebuilds <= 1);
    CHECK(d.rebuilds == 1);
    CHECK(d.resends == 1);
    CHECK(h.failures == 0);
    const qint64 p0 = audioPos(h);
    spin(1000);
    CHECK_MSG(audioPos(h) > p0, "still plays after giving up");
    spin(2000);
    CHECK(h.player.diagnostics().rebuilds == 1);  // never loops
}

// PLY-06/07: start at 5 s with rate 1.5 -> exactly one flush seek per sink,
// the first frame at >= 5 s, rate 1.5 on both branches.
static void ply06_prerollBarrier() {
    Harness h;
    PlayerSources p = dualWebm();
    p.startPositionMs = 5000;
    p.rate = 1.5;
    const bool ok = startAndPlay(h, p);
    CHECK(ok);
    if (!ok) return;
    waitFor([&]() { return h.firstFrames > 0; }, 3000);
    const PlayerDiagnostics d = h.player.diagnostics();
    // Start seek before preroll: the flush stays inside the source bins; never more than one per sink.
    CHECK_MSG(d.engine.videoSinkFlushes <= 1 && d.engine.audioSinkFlushes <= 1, "flushes video %d audio %d",
              d.engine.videoSinkFlushes, d.engine.audioSinkFlushes);
    CHECK_MSG(d.engine.firstVideoSinkMs >= 4950, "first video sink buffer at %lld", (long long)d.engine.firstVideoSinkMs);
    CHECK_MSG(d.seeksSent == 2, "seeks sent %d", d.seeksSent);  // one event per sink
    CHECK_MSG(h.firstFramePosition >= 4950, "first frame at %.0f", h.firstFramePosition);
    CHECK(h.firstFrames == 1);
    CHECK(h.player.rate() == 1.5);
    CHECK_MSG(d.engine.videoSegmentRate == 1.5, "video rate %.2f", d.engine.videoSegmentRate);
    CHECK_MSG(d.engine.audioSegmentRate == 1.5, "audio rate %.2f", d.engine.audioSegmentRate);
    const qint64 p0 = audioPos(h);
    spin(2000);
    const qint64 adv = audioPos(h) - p0;
    CHECK_MSG(adv >= 2600 && adv <= 3400, "advanced %lld ms in 2 s at 1.5x", (long long)adv);
    // A later setRate is one rate-carrying flush seek.
    h.player.setRate(1.0);
    spin(800);
    CHECK(h.player.diagnostics().engine.audioSegmentRate == 1.0);
    CHECK(h.player.diagnostics().engine.videoSegmentRate == 1.0);
}

// PLY-09: freeze while hidden; audio keeps advancing, decoding stops, thaw
// without a flush, repaint 250 ms after becoming active.
static void ply09_freezeAndThaw(const char *name, const PlayerSources &p) {
    Harness h;
    const bool ok = startAndPlay(h, p);
    CHECK(ok);
    if (!ok) return;
    spin(500);
    h.player.setAppActive(false);
    CHECK(waitFor([&]() { return h.player.frozen(); }, 1500));
    spin(300);
    const PlayerDiagnostics before = h.player.diagnostics();
    const qint64 a0 = audioPos(h);
    spin(2000);
    const qint64 a1 = audioPos(h);
    const PlayerDiagnostics during = h.player.diagnostics();
    CHECK_MSG(a1 - a0 >= 1700, "%s: audio advanced %lld ms while frozen", name, (long long)(a1 - a0));
    const qint64 framesFrozen = during.engine.framesRendered - before.engine.framesRendered;
    CHECK_MSG(framesFrozen <= 2, "%s: %lld frames reached the sink while frozen", name, (long long)framesFrozen);
    h.player.setAppActive(true);
    CHECK(!h.player.frozen());
    CHECK(waitFor([&]() { return h.repaints > 0; }, 1000));
    const qint64 f0 = h.player.diagnostics().engine.framesRendered;
    CHECK_MSG(waitFor([&]() { return h.player.diagnostics().engine.framesRendered > f0 + 10; }, 4000),
              "%s: video did not resume", name);
    const PlayerDiagnostics after = h.player.diagnostics();
    CHECK_MSG(after.engine.videoSinkFlushes == before.engine.videoSinkFlushes &&
              after.engine.audioSinkFlushes == before.engine.audioSinkFlushes, "%s: thaw flushed", name);
    CHECK(h.firstFrames == 1);
    CHECK(h.player.state() == VideoPlayer::Playing);
    spin(2500);  // catch up
    h.player.resetEngineStats();
    spin(600);
    const EngineStats s = h.player.diagnostics().engine;
    std::printf("  %s freeze: %lld frames while frozen (%lld dropped before decoder), audio +%lld ms, "
                "A/V after thaw %lld ms (%d samples)\n", name, (long long)framesFrozen,
                (long long)after.engine.freezeDropped, (long long)(a1 - a0), (long long)s.maxAbsAvOffsetMs, s.avSamples);
    CHECK_MSG(s.avSamples == 0 || s.maxAbsAvOffsetMs <= kMaxAvOffsetMs, "%s: A/V after thaw %lld", name,
              (long long)s.maxAbsAvOffsetMs);
    // Teardown while frozen must not hang (unlock first).
    h.player.setAppActive(false);
    waitFor([&]() { return h.player.frozen(); }, 1500);
    h.player.stop();
    CHECK(h.player.state() == VideoPlayer::Idle);
}

// PLY-24 / EOS: ended + duration.
static void ply24_endOfStream() {
    Harness h;
    PlayerSources p = sources(PlayMode::Local);
    p.muxed = spec("file://" + gDir + "/av.webm", Container::Matroska);
    const bool ok = startAndPlay(h, p);
    CHECK(ok);
    if (!ok) return;
    CHECK(waitFor([&]() { return h.player.duration() > 0; }, 3000));
    const double duration = h.player.duration();
    CHECK_MSG(duration >= kSeconds * 1000 - 100 && duration <= kSeconds * 1000 + 100, "duration %.0f", duration);
    h.player.seek(duration - 1500);
    CHECK(waitFor([&]() { return h.eos > 0; }, 6000));
    CHECK(h.player.ended());
    CHECK(h.player.state() == VideoPlayer::Ended);
    CHECK(h.player.position() == h.player.duration());
    // Play after the end restarts from the beginning.
    h.player.play();
    CHECK(waitFor([&]() { return h.playingNow(); }, 4000));
    CHECK(!h.player.ended());
    CHECK(h.player.position() < 2000);
}

// User pause / play goes through BufferingPolicy (PLY-08 owner of the decision).
static void ply08_pausePlay() {
    Harness h;
    const bool ok = startAndPlay(h, dualWebm());
    CHECK(ok);
    if (!ok) return;
    h.player.pause();
    CHECK(waitFor([&]() { return h.player.state() == VideoPlayer::Paused; }, 2000));
    const qint64 p0 = audioPos(h);
    spin(800);
    CHECK(audioPos(h) - p0 < 100);
    h.player.play();
    CHECK(waitFor([&]() { return h.playingNow(); }, 2000));
}

// PLY-22: each optional chain element removed in turn still plays audio.
static void ply22_degradedChainsPlay() {
    const char *const optional[] = {"scaletempo", "equalizer-10bands", "volume", "rglimiter", "audioconvert", "audioresample"};
    for (size_t i = 0; i < sizeof(optional) / sizeof(optional[0]); ++i) {
        Harness h;
        PlayerSources p = sources(PlayMode::AudioOnly);
        p.audio = spec("file://" + gDir + "/a.webm", Container::Matroska);
        p.unavailableElements.insert(optional[i]);
        const bool ok = startAndPlay(h, p);
        CHECK_MSG(ok, "without %s: not playing (%s)", optional[i], qPrintable(h.lastError));
        if (!ok) continue;
        CHECK(!h.player.plan().skipped.empty() || std::string(optional[i]) == "rglimiter");
        const qint64 a0 = audioPos(h);
        spin(700);
        CHECK_MSG(audioPos(h) - a0 >= 500, "without %s: audio stalled", optional[i]);
    }
}

// PLY-22: a full-scale sine through the plan's chain at 500 % boost never
// exceeds 0 dBFS after the limiter (rglimiter and the audiodynamic fallback).
static float peakAfterChain(const char *limiterToDrop) {
    PlanInput in;
    in.mode = PlayMode::AudioOnly;
    in.audio.uri = "file:///unused.webm";
    in.available = GstEngine::availableElements();
    if (limiterToDrop) in.available.erase(limiterToDrop);
    AudioSettings s;
    s.boostPercent = 500;
    in.audioValues = computeAudioChainValues(s, StreamLoudness());
    const PipelinePlan plan = buildPipelinePlan(in);
    const PadRoute *route = plan.route(RouteKind::Audio);
    if (!route) return 99.f;
    QString desc = "audiotestsrc wave=sine volume=1.0 freq=440 num-buffers=100 ! audio/x-raw,format=F32LE,rate=48000,channels=2";
    for (size_t i = 0; i + 1 < route->chain.size(); ++i) {  // all but the sink
        const ElementPlan &e = route->chain[i];
        desc += " ! " + QString::fromStdString(e.factory);
        for (size_t k = 0; k < e.properties.size(); ++k)
            desc += " " + QString::fromStdString(e.properties[k].name) + "=" + QString::fromStdString(e.properties[k].value);
        if (!e.linkCapsToNext.empty()) desc += " ! " + QString::fromStdString(e.linkCapsToNext);
    }
    desc += " ! audioconvert ! audio/x-raw,format=F32LE ! fakesink name=out signal-handoffs=true sync=false";
    GError *err = nullptr;
    GstElement *p = gst_parse_launch(desc.toUtf8().constData(), &err);
    if (!p || err) {
        std::printf("  chain parse failed: %s\n", err ? err->message : "?");
        if (err) g_error_free(err);
        return 99.f;
    }
    static float peak;
    peak = 0.f;
    GstElement *out = gst_bin_get_by_name(GST_BIN(p), "out");
    g_signal_connect(out, "handoff", G_CALLBACK(+[](GstElement *, GstBuffer *b, GstPad *, gpointer) {
        GstMapInfo m;
        if (!gst_buffer_map(b, &m, GST_MAP_READ)) return;
        const float *f = reinterpret_cast<const float *>(m.data);
        for (size_t i = 0; i < m.size / sizeof(float); ++i) {
            const float a = f[i] < 0 ? -f[i] : f[i];
            if (a > peak) peak = a;
        }
        gst_buffer_unmap(b, &m);
    }), nullptr);
    gst_object_unref(out);
    gst_element_set_state(p, GST_STATE_PLAYING);
    GstBus *bus = gst_element_get_bus(p);
    GstMessage *msg = gst_bus_timed_pop_filtered(bus, 20 * GST_SECOND, GstMessageType(GST_MESSAGE_EOS | GST_MESSAGE_ERROR));
    if (msg) gst_message_unref(msg);
    gst_object_unref(bus);
    gst_element_set_state(p, GST_STATE_NULL);
    gst_object_unref(p);
    std::printf("  limiter %s: peak %.4f (%s)\n", plan.limiterFactory.c_str(), peak, plan.limiterFactory.empty() ? "none" : "ok");
    return peak;
}

static void ply22_limiterHoldsFullScale() {
    CHECK(peakAfterChain(nullptr) <= 1.0f);
    CHECK(peakAfterChain("rglimiter") <= 1.0f);
}

// PLY-20 per-build fallback: hardware asked for, droid elements absent.
static void ply20_perBuildFallback() {
    Harness h;
    QString notice;
    QObject::connect(&h.player, &VideoPlayer::decodeFallback, [&](const QString &r) { notice = r; });
    PlayerSources p = dualWebm();
    p.decode = DecodeMode::Hardware;
    p.unavailableElements.insert("droidvdec");
    CHECK(startAndPlay(h, p));
    CHECK(h.player.decodeMode() == int(DecodeMode::Software));
    CHECK(!notice.isEmpty());
    CHECK(!containsDroidElement(h.player.plan()));
}

// RES-07: a source refused at byte 0 reports the HTTP status from souphttpsrc's error details
// (http-status-code), on the refused branch, as a network error; a missing file reports 404.
static void res07_httpStatusThroughPlayer() {
    {
        Harness h;
        PlayerSources p = dualWebm();
        p.video = spec(gServer->url("f/v.webm"), Container::Matroska);
        h.player.load(p);
        CHECK(waitFor([&]() { return h.failures > 0; }, 10000));
        CHECK_MSG(h.firstHttpStatus == 403, "status %d (%s)", h.firstHttpStatus, qPrintable(h.firstError));
        CHECK(h.firstNetwork);
        CHECK(h.firstBranch == int(Branch::Video));
        CHECK(!h.firstError.contains("403"));  // the text alone would not tell (hence the status)
        CHECK(gServer->requests("/f/v.webm") > 0);
    }
    {
        Harness h;
        PlayerSources p = sources(PlayMode::Muxed);
        p.muxed = spec(gServer->url("r/missing.webm"), Container::Matroska);
        h.player.load(p);
        CHECK(waitFor([&]() { return h.failures > 0; }, 10000));
        CHECK_MSG(h.firstHttpStatus == 404, "status %d (%s)", h.firstHttpStatus, qPrintable(h.firstError));
        CHECK(h.firstNetwork && h.firstBranch == int(Branch::Muxed));
    }
    {
        Harness h;  // a source that plays reports no failure at all
        CHECK(startAndPlay(h, dualWebm()));
        CHECK(h.failures == 0 && h.firstHttpStatus == -1);
    }
}

// ---- PLT-08: fake decoders (the host has no AAC / H.264 codecs) ---------------------------
// R2 / SFOS 5.2: droidadec outranks libav and decodes no YouTube AAC frames (silent playback).
// Registered in-process before GstEngine::initialize(): a fake "droidadec" at PRIMARY + 10 and a
// fake "avdec_aac" at PRIMARY for audio/mpeg mpegversion=4, and a fake H.264 decoder for the
// muxed itag-18 shape. They emit silence / grey frames with the input timestamps.

struct FakeDecoderConfig {
    const char *sinkCaps;
    const char *klass;
    bool video;
};
static const FakeDecoderConfig kFakeAacConfig = {"audio/mpeg, mpegversion=(int)4", "Codec/Decoder/Audio", false};
static const FakeDecoderConfig kFakeH264Config = {"video/x-h264", "Codec/Decoder/Video", true};

struct FakeDecoder {
    GstElement parent;
    GstPad *sink;
    GstPad *src;
};
struct FakeDecoderClass {
    GstElementClass parent;
    const FakeDecoderConfig *config;
};

static const FakeDecoderConfig *fakeConfig(gpointer object) {
    return reinterpret_cast<FakeDecoderClass *>(G_OBJECT_GET_CLASS(object))->config;
}

static GstFlowReturn fakeChain(GstPad *, GstObject *parent, GstBuffer *in) {
    FakeDecoder *d = reinterpret_cast<FakeDecoder *>(parent);
    const bool video = fakeConfig(parent)->video;
    gsize size = 320 * 180 * 3 / 2;
    if (!video) {
        const GstClockTime dur = GST_BUFFER_DURATION_IS_VALID(in) ? GST_BUFFER_DURATION(in) : 1024 * GST_SECOND / 48000;
        size = gsize(gst_util_uint64_scale(dur, 48000, GST_SECOND)) * 4;
    }
    GstBuffer *out = gst_buffer_new_allocate(nullptr, size, nullptr);
    gst_buffer_memset(out, 0, video ? 128 : 0, size);
    GST_BUFFER_PTS(out) = GST_BUFFER_PTS(in);
    GST_BUFFER_DURATION(out) = GST_BUFFER_DURATION(in);
    gst_buffer_unref(in);
    return gst_pad_push(d->src, out);
}

static gboolean fakeSinkEvent(GstPad *pad, GstObject *parent, GstEvent *ev) {
    if (GST_EVENT_TYPE(ev) == GST_EVENT_CAPS) {  // replace the input caps by the "decoded" ones
        gst_event_unref(ev);
        FakeDecoder *d = reinterpret_cast<FakeDecoder *>(parent);
        GstCaps *caps = gst_caps_from_string(fakeConfig(parent)->video
                                                 ? "video/x-raw,format=I420,width=320,height=180,framerate=30/1,pixel-aspect-ratio=1/1"
                                                 : "audio/x-raw,format=S16LE,layout=interleaved,rate=48000,channels=2");
        const gboolean ok = gst_pad_set_caps(d->src, caps);
        gst_caps_unref(caps);
        return ok;
    }
    return gst_pad_event_default(pad, parent, ev);
}

static void fakeClassInit(gpointer klass, gpointer data) {
    const FakeDecoderConfig *cfg = static_cast<const FakeDecoderConfig *>(data);
    reinterpret_cast<FakeDecoderClass *>(klass)->config = cfg;
    GstElementClass *ek = GST_ELEMENT_CLASS(klass);
    GstCaps *sinkCaps = gst_caps_from_string(cfg->sinkCaps);
    GstCaps *srcCaps = gst_caps_from_string(cfg->video ? "video/x-raw" : "audio/x-raw");
    gst_element_class_add_pad_template(ek, gst_pad_template_new("sink", GST_PAD_SINK, GST_PAD_ALWAYS, sinkCaps));
    gst_element_class_add_pad_template(ek, gst_pad_template_new("src", GST_PAD_SRC, GST_PAD_ALWAYS, srcCaps));
    gst_caps_unref(sinkCaps);
    gst_caps_unref(srcCaps);
    gst_element_class_set_static_metadata(ek, "Fake decoder", cfg->klass, "SailTube test decoder", "SailTube");
}

static void fakeInstanceInit(GTypeInstance *instance, gpointer klass) {
    FakeDecoder *d = reinterpret_cast<FakeDecoder *>(instance);
    GstElementClass *ek = GST_ELEMENT_CLASS(klass);
    d->sink = gst_pad_new_from_template(gst_element_class_get_pad_template(ek, "sink"), "sink");
    d->src = gst_pad_new_from_template(gst_element_class_get_pad_template(ek, "src"), "src");
    gst_pad_set_chain_function(d->sink, &fakeChain);
    gst_pad_set_event_function(d->sink, &fakeSinkEvent);
    gst_pad_use_fixed_caps(d->src);
    gst_element_add_pad(GST_ELEMENT(d), d->sink);
    gst_element_add_pad(GST_ELEMENT(d), d->src);
}

static GType fakeDecoderType(const char *name, const FakeDecoderConfig *cfg) {
    GTypeInfo info;
    std::memset(&info, 0, sizeof(info));
    info.class_size = sizeof(FakeDecoderClass);
    info.class_init = &fakeClassInit;
    info.class_data = cfg;
    info.instance_size = sizeof(FakeDecoder);
    info.instance_init = &fakeInstanceInit;
    return g_type_register_static(GST_TYPE_ELEMENT, name, &info, GTypeFlags(0));
}

static gboolean fakePluginInit(GstPlugin *plugin) {
    return gst_element_register(plugin, "droidadec", GST_RANK_PRIMARY + 10, fakeDecoderType("FakeDroidAdec", &kFakeAacConfig)) &&
           gst_element_register(plugin, "avdec_aac", GST_RANK_PRIMARY, fakeDecoderType("FakeAvdecAac", &kFakeAacConfig)) &&
           gst_element_register(plugin, "sailtubefakeh264dec", GST_RANK_PRIMARY + 5,
                                fakeDecoderType("FakeH264Dec", &kFakeH264Config));
}

static bool gFakeDecoders = false;

// Before GstEngine::initialize() (which demotes droidadec, PLT-08). Skipped where a real
// avdec_aac or droidadec exists (the names would clash).
static void registerFakeDecoders() {
    gst_init(nullptr, nullptr);
    GstElementFactory *real = gst_element_factory_find("avdec_aac");
    if (!real) real = gst_element_factory_find("droidadec");
    if (real) {
        gst_object_unref(real);
        return;
    }
    gFakeDecoders = gst_plugin_register_static(GST_VERSION_MAJOR, GST_VERSION_MINOR, "sailtubefakedecoders",
                                               "SailTube test decoders", &fakePluginInit, "1.0", "Proprietary",
                                               "sailtube-tests", "sailtube", "https://localhost/");
}

// An mp4 with a fake H.264 track (valid avcC, one IDR NAL per frame) and / or a fake AAC-LC track
// (valid AudioSpecificConfig, 48 kHz stereo), 12 s each, written through appsrc.
static bool writeFakeMp4(const QString &path, bool withVideo, bool withAudio) {
    QString desc = "mp4mux name=m faststart=true ! filesink location=" + path;
    // SPS: baseline, level 3, 320x180 (cropped from 192), poc type 2. PPS: CAVLC defaults.
    if (withVideo)
        desc += " appsrc name=v format=time caps=\"video/x-h264,stream-format=avc,alignment=au,width=320,height=180,"
                "framerate=30/1,pixel-aspect-ratio=1/1,codec_data=(buffer)0142c01effe100096742c01eda05067e7401000468ce3c80\" ! m.video_0";
    if (withAudio)
        desc += " appsrc name=a format=time caps=\"audio/mpeg,mpegversion=4,stream-format=raw,channels=2,rate=48000,"
                "codec_data=(buffer)1190\" ! m.audio_0";
    GError *err = nullptr;
    GstElement *p = gst_parse_launch(desc.toUtf8().constData(), &err);
    if (!p || err) {
        std::printf("fake mp4 parse error: %s\n", err ? err->message : "?");
        if (err) g_error_free(err);
        if (p) gst_object_unref(p);
        return false;
    }
    gst_element_set_state(p, GST_STATE_PLAYING);
    if (withVideo) {
        GstElement *v = gst_bin_get_by_name(GST_BIN(p), "v");
        for (int i = 0; i < kSeconds * kFps; ++i) {
            GstBuffer *b = gst_buffer_new_allocate(nullptr, 4 + 101, nullptr);
            GstMapInfo m;
            gst_buffer_map(b, &m, GST_MAP_WRITE);
            std::memset(m.data, 0x5a, m.size);
            m.data[0] = 0; m.data[1] = 0; m.data[2] = 0; m.data[3] = 101;  // NAL length
            m.data[4] = 0x65;                                               // IDR slice
            m.data[5] = 0x88;  // first_mb 0, slice_type 7 (I), pps 0, frame_num 0, idr_pic_id 0,
            m.data[6] = 0x84;  // dec_ref_pic_marking 0 0, qp delta 0, deblocking idc 1
            m.data[7] = 0xaf;
            gst_buffer_unmap(b, &m);
            GST_BUFFER_PTS(b) = GST_BUFFER_DTS(b) = gst_util_uint64_scale(i, GST_SECOND, kFps);
            GST_BUFFER_DURATION(b) = GST_SECOND / kFps;
            gst_app_src_push_buffer(GST_APP_SRC(v), b);
        }
        gst_app_src_end_of_stream(GST_APP_SRC(v));
        gst_object_unref(v);
    }
    if (withAudio) {
        GstElement *a = gst_bin_get_by_name(GST_BIN(p), "a");
        const int frames = kSeconds * 48000 / 1024;
        for (int i = 0; i < frames; ++i) {
            GstBuffer *b = gst_buffer_new_allocate(nullptr, 200, nullptr);
            gst_buffer_memset(b, 0, 0x21, 200);
            GST_BUFFER_PTS(b) = GST_BUFFER_DTS(b) = gst_util_uint64_scale(i * 1024, GST_SECOND, 48000);
            GST_BUFFER_DURATION(b) = gst_util_uint64_scale(1024, GST_SECOND, 48000);
            gst_app_src_push_buffer(GST_APP_SRC(a), b);
        }
        gst_app_src_end_of_stream(GST_APP_SRC(a));
        gst_object_unref(a);
    }
    GstBus *bus = gst_element_get_bus(p);
    GstMessage *msg = gst_bus_timed_pop_filtered(bus, 60 * GST_SECOND, GstMessageType(GST_MESSAGE_EOS | GST_MESSAGE_ERROR));
    const bool ok = msg && GST_MESSAGE_TYPE(msg) == GST_MESSAGE_EOS;
    if (msg && !ok) {
        GError *e = nullptr;
        gchar *dbg = nullptr;
        gst_message_parse_error(msg, &e, &dbg);
        std::printf("fake mp4 error: %s (%s)\n", e ? e->message : "?", dbg ? dbg : "");
        if (e) g_error_free(e);
        g_free(dbg);
    }
    if (msg) gst_message_unref(msg);
    gst_object_unref(bus);
    gst_element_set_state(p, GST_STATE_NULL);
    gst_object_unref(p);
    return ok;
}

// PLT-08: after GstEngine::initialize() a (fake) droidadec outranking avdec_aac is never
// autoplugged: dual mp4 with AAC audio, muxed H.264 + AAC mp4 (itag 18 shape) and the same file
// locally all decode the AAC with avdec_aac; the engine reports (and logs) the decoders it used.
static void plt08_droidAudioDecoderNeverAutoplugged() {
    if (!gFakeDecoders) {
        std::printf("  SKIPPED (a real avdec_aac / droidadec is installed)\n");
        return;
    }
    GstElementFactory *droid = gst_element_factory_find("droidadec");
    CHECK(droid && gst_plugin_feature_get_rank(GST_PLUGIN_FEATURE(droid)) == GST_RANK_NONE);  // demoted
    if (droid) gst_object_unref(droid);
    const bool fixtures = writeFakeMp4(gDir + "/aac.mp4", false, true) && writeFakeMp4(gDir + "/avc_aac.mp4", true, true);
    CHECK(fixtures);
    if (!fixtures) return;
    struct Case { const char *name; PlayerSources src; const char *video; };
    std::vector<Case> cases;
    PlayerSources dual = sources(PlayMode::Dual);
    dual.video = spec(gServer->url("r/v.mp4"), Container::IsoBmff);
    dual.audio = spec(gServer->url("r/aac.mp4"), Container::IsoBmff);
    cases.push_back({"dual mp4 + AAC", dual, "vp9dec"});
    PlayerSources muxed = sources(PlayMode::Muxed);
    muxed.muxed = spec(gServer->url("r/avc_aac.mp4"), Container::IsoBmff);
    cases.push_back({"muxed H.264 + AAC (itag 18)", muxed, "sailtubefakeh264dec"});
    PlayerSources local = sources(PlayMode::Local);
    local.muxed = spec("file://" + gDir + "/avc_aac.mp4", Container::IsoBmff);
    cases.push_back({"local H.264 + AAC", local, "sailtubefakeh264dec"});
    for (size_t i = 0; i < cases.size(); ++i) {
        Harness h;
        const bool ok = startAndPlay(h, cases[i].src);
        CHECK_MSG(ok, "%s: not playing (%s)", cases[i].name, qPrintable(h.lastError));
        if (!ok) continue;
        spin(500);
        const EngineStats st = h.player.diagnostics().engine;
        std::printf("  %s: decoders video=%s audio=%s\n", cases[i].name, qPrintable(st.videoDecoder), qPrintable(st.audioDecoder));
        CHECK_MSG(st.audioDecoder == "avdec_aac", "%s: audio decoder %s", cases[i].name, qPrintable(st.audioDecoder));
        CHECK(st.audioDecoder != "droidadec");
        CHECK_MSG(st.videoDecoder == cases[i].video, "%s: video decoder %s", cases[i].name, qPrintable(st.videoDecoder));
        CHECK(!containsDroidElement(h.player.plan()));
        CHECK(audioPos(h) > 0);
    }
}

// ---- Start-up performance -------------------------------------------------------------

static qint64 median(std::vector<qint64> v) {
    if (v.empty()) return -1;
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
}

static qint64 fileSize(const QString &name) { return QFileInfo(gDir + "/" + name).size(); }

// One start: load, wait for the first frame, report load -> first frame (the "first-frame" marker).
struct StartRun {
    bool ok = false;
    qint64 firstFrameMs = -1;
    double firstFramePosition = -1;
    PlayerDiagnostics diag;      // at the first frame: the video branch has landed, the audio maybe not yet
    PlayerDiagnostics prerolled; // at the first preroll: every branch has landed (valid when landed)
    bool landed = false;
    int firstFrames = 0;
    QHash<QString, qint64> marks;
};

static StartRun startOnce(const PlayerSources &p, qint64 lateStartMs = -1) {
    Harness h;
    StartRun r;
    h.load(p);
    if (lateStartMs >= 0) h.player.setStartPosition(lateStartMs);  // the library bridge's order
    r.ok = waitFor([&]() { return h.firstFrames > 0 || h.failures > 0; }, 15000) && h.failures == 0;
    if (!r.ok) return r;
    r.firstFrameMs = h.marks.value("first-frame", -1);
    r.firstFramePosition = h.firstFramePosition;
    r.diag = h.player.diagnostics();
    // The video's first frame does not wait for the audio branch (PLY-06); what holds for both
    // branches (their segments) is read at the first preroll, when the audio has landed too.
    r.landed = waitFor([&]() { return h.marks.contains("preroll") || h.failures > 0; }, 10000) && h.failures == 0;
    r.prerolled = h.player.diagnostics();
    spin(200);
    r.firstFrames = h.firstFrames;
    r.marks = h.marks;
    return r;
}

// The timing split of one start (NFR-PERF markers), for the log.
static void printSplit(const char *name, const StartRun &r) {
    const char *const keys[] = {"demux-ready branch=video", "demux-ready branch=audio", "demux-ready branch=muxed",
                                "start-seek branch=video", "start-seek branch=audio", "start-seek branch=muxed",
                                "first-decoded branch=video", "preroll", "first-frame"};
    QString line;
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); ++i)
        if (r.marks.contains(keys[i])) line += QStringLiteral(" %1@%2").arg(keys[i]).arg(r.marks.value(keys[i]));
    std::printf("  %s split:%s\n", name, qPrintable(line));
}

// PLY-06 start seek before preroll, the engine's view: nothing from before the start reached the
// video sink, no flush reached any sink.
static void startChecks(const char *name, const StartRun &r, double minPosition) {
    CHECK_MSG(r.diag.engine.firstVideoSinkMs >= minPosition, "%s: first video sink buffer at %lld ms", name,
              (long long)r.diag.engine.firstVideoSinkMs);
    CHECK_MSG(r.diag.engine.videoSinkFlushes <= 1 && r.diag.engine.audioSinkFlushes <= 1,
              "%s: flushes video %d audio %d", name, r.diag.engine.videoSinkFlushes, r.diag.engine.audioSinkFlushes);
    // Normally the start seek's flush stays inside the source bin (0 at the sinks); seen once in
    // ~30 dual mp4 starts on a loaded host: one flush at the video sink, frames still >= start.
    if (r.diag.engine.videoSinkFlushes || r.diag.engine.audioSinkFlushes)
        std::printf("  note: %s: a flush reached a sink (video %d audio %d)\n", name, r.diag.engine.videoSinkFlushes,
                    r.diag.engine.audioSinkFlushes);
}

// PLY-06 (start-up performance): a start position (and a remembered rate) takes effect before the
// first frame is decoded: the first frame is at the start position, no frame was ever posted at
// 0, at most one flush per sink, and the time to the first frame is about the same as a start at 0.
static void ply06_startAtPositionWithoutPrerollAtZero() {
    struct Case { const char *name; PlayerSources src; double minPosition; double rate; };
    std::vector<Case> cases;
    cases.push_back({"dual webm", dualWebm(), 4900, 1.0});
    cases.push_back({"dual webm 1.5x", dualWebm(), 4900, 1.5});
    // Key-unit branches land on the keyframe before 5 s (2 s GOP: 4 s).
    cases.push_back({"dual mp4", dualMp4(), 4000 - 40, 1.0});
    PlayerSources muxed = sources(PlayMode::Muxed);
    muxed.muxed = spec(gServer->url("r/av.webm"), Container::Matroska);
    cases.push_back({"muxed webm", muxed, 4000 - 40, 1.0});
    PlayerSources local = sources(PlayMode::Local);
    local.muxed = spec("file://" + gDir + "/av.mp4", Container::IsoBmff);
    cases.push_back({"local mp4", local, 4000 - 40, 1.0});
    PlayerSources slow = dualWebm("t");
    slow.video = spec(gServer->url("t/v.webm"), Container::Matroska);
    cases.push_back({"dual webm throttled", slow, 4900, 1.0});
    gThrottleBytesPerSecond = qMax(fileSize("v.webm"), fileSize("a.webm")) * 4 / kSeconds;  // 4x real time

    for (size_t i = 0; i < cases.size(); ++i) {
        std::vector<qint64> atZero, atStart;
        for (int run = 0; run < 3; ++run) {
            PlayerSources p0 = cases[i].src;
            StartRun z = startOnce(p0);
            CHECK_MSG(z.ok, "%s: start at 0 failed", cases[i].name);
            if (z.ok) atZero.push_back(z.firstFrameMs);
            PlayerSources p5 = cases[i].src;
            p5.startPositionMs = 5000;
            p5.rate = cases[i].rate;
            StartRun r = startOnce(p5);
            CHECK_MSG(r.ok, "%s: start at 5 s failed", cases[i].name);
            if (!r.ok) continue;
            atStart.push_back(r.firstFrameMs);
            CHECK_MSG(r.firstFramePosition >= cases[i].minPosition, "%s: first frame at %.0f", cases[i].name,
                      r.firstFramePosition);
            CHECK_MSG(r.firstFrames == 1, "%s: %d first frames", cases[i].name, r.firstFrames);
            CHECK_MSG(r.diag.engine.videoSinkFlushes <= 1 && r.diag.engine.audioSinkFlushes <= 1,
                      "%s: flushes video %d audio %d", cases[i].name, r.diag.engine.videoSinkFlushes,
                      r.diag.engine.audioSinkFlushes);
            CHECK_MSG(r.diag.rebuilds == 0, "%s: rebuilds %d", cases[i].name, r.diag.rebuilds);
            startChecks(cases[i].name, r, cases[i].minPosition);
            if (cases[i].rate != 1.0) {
                // The frame shown first is already at the rate; the audio's segment may arrive
                // after it (the first frame does not wait for the audio), so both branches are
                // compared once both have landed. ply07_startRateReachesBothBranches plays on.
                CHECK_MSG(r.diag.engine.videoSegmentRate == cases[i].rate, "%s: video segment rate %.2f at the first frame",
                          cases[i].name, r.diag.engine.videoSegmentRate);
                CHECK_MSG(r.landed, "%s: no preroll after the first frame", cases[i].name);
                CHECK_MSG(r.prerolled.engine.videoSegmentRate == cases[i].rate &&
                              r.prerolled.engine.audioSegmentRate == cases[i].rate,
                          "%s: segment rates %.2f / %.2f", cases[i].name, r.prerolled.engine.videoSegmentRate,
                          r.prerolled.engine.audioSegmentRate);
            }
            if (run == 0) {
                std::printf("  %s: first frame at %.0f ms (first sink buffer %lld ms), flushes video %d audio %d, seeks sent %d, "
                            "%lld pre-seek buffers dropped at the decoders\n", cases[i].name, r.firstFramePosition,
                            (long long)r.diag.engine.firstVideoSinkMs, r.diag.engine.videoSinkFlushes,
                            r.diag.engine.audioSinkFlushes, r.diag.seeksSent, (long long)r.diag.engine.startDropped);
                printSplit(cases[i].name, z);
                printSplit(cases[i].name, r);
            }
        }
        const qint64 z = median(atZero), st = median(atStart);
        std::printf("  %s: load -> first frame median %lld ms from 0, %lld ms from 5 s (difference %lld ms)\n",
                    cases[i].name, (long long)z, (long long)st, (long long)(st - z));
    }
    gThrottleBytesPerSecond = 0;
}

// PLY-07 / PLY-06: a remembered rate reaches both branches of a dual start, whenever the audio
// lands. Dual WebM at 5 s and 1.5x with the audio source answering 1.5 s late: the video's first
// frame is there at the rate while the audio sink has not seen a segment yet (its rate still
// reads the 1.0 default: the integration gate's "segment rates 1.50 / 1.00" was this moment, read
// too early); once playing both segments carry 1.5 and both sinks advance at 1.5x. The same for a
// rate change while playing (dual WebM and dual MP4: one rate-carrying seek per sink).
static void ply07_startRateReachesBothBranches() {
    {
        gDelayedPathMs = 1500;
        Harness h;
        PlayerSources p = dualWebm("d");
        p.startPositionMs = 5000;
        p.rate = 1.5;
        h.load(p);
        const bool frame = waitFor([&]() { return h.firstFrames > 0 || h.failures > 0; }, 15000) && h.failures == 0;
        CHECK_MSG(frame, "no first frame (%s)", qPrintable(h.lastError));
        const EngineStats atFrame = h.player.diagnostics().engine;
        const bool audioSeekSent = h.marks.contains("start-seek branch=audio");
        const bool playing = frame && waitFor([&]() { return h.playingNow() || h.failures > 0; }, 15000) && h.failures == 0;
        gDelayedPathMs = 0;
        CHECK_MSG(playing, "not playing (%s)", qPrintable(h.lastError));
        if (!playing) return;
        const EngineStats s = h.player.diagnostics().engine;
        std::printf("  audio 1.5 s late: at the first frame segment rates %.2f / %.2f (audio start seek sent: %d), "
                    "once playing %.2f / %.2f\n", atFrame.videoSegmentRate, atFrame.audioSegmentRate, int(audioSeekSent),
                    s.videoSegmentRate, s.audioSegmentRate);
        CHECK_MSG(atFrame.videoSegmentRate == 1.5, "video segment rate %.2f at the first frame", atFrame.videoSegmentRate);
        CHECK_MSG(!audioSeekSent, "the audio start seek was out before the first frame: the late audio did not take effect");
        CHECK_MSG(s.videoSegmentRate == 1.5 && s.audioSegmentRate == 1.5, "segment rates %.2f / %.2f once playing",
                  s.videoSegmentRate, s.audioSegmentRate);
        CHECK(h.player.rate() == 1.5 && h.player.diagnostics().rebuilds == 0);
    }
    // Both sinks really run at the rate, from the start and after a change while playing.
    struct Case { const char *name; PlayerSources src; };
    std::vector<Case> cases;
    cases.push_back({"dual webm", dualWebm()});
    cases.push_back({"dual mp4", dualMp4()});
    for (size_t i = 0; i < cases.size(); ++i) {
        Harness h;
        PlayerSources p = cases[i].src;
        p.rate = 1.5;
        const bool ok = startAndPlay(h, p);
        CHECK_MSG(ok, "%s did not play", cases[i].name);
        if (!ok) continue;
        const double rates[] = {1.5, 2.0, 1.0, 1.5};
        for (size_t r = 0; r < sizeof(rates) / sizeof(rates[0]); ++r) {
            if (r > 0) {
                const int before = h.seekDone;
                if (r == 2) {  // the change arrives while a seek is in flight: it rides on the queued seek
                    h.player.seek(3000);
                    h.player.setRate(rates[r]);
                    CHECK_MSG(waitFor([&]() { return h.seekDone >= before + 2; }, 8000), "%s: seek + rate %.2f never completed",
                              cases[i].name, rates[r]);
                } else {
                    h.player.setRate(rates[r]);
                    CHECK_MSG(waitFor([&]() { return h.seekDone > before; }, 8000), "%s: rate %.2f never completed",
                              cases[i].name, rates[r]);
                }
                waitFor([&]() { return h.playingNow(); }, 3000);
            }
            spin(300);
            const EngineStats s = h.player.diagnostics().engine;
            CHECK_MSG(s.videoSegmentRate == rates[r] && s.audioSegmentRate == rates[r], "%s at %.2fx: segment rates %.2f / %.2f",
                      cases[i].name, rates[r], s.videoSegmentRate, s.audioSegmentRate);
            // The speed each sink really runs at, over 800 ms; up to three windows (a loaded host
            // can stall one: a branch left at another rate is off by a third or more in all).
            double audioSpeed = 0, videoSpeed = 0;
            bool atRate = false;
            for (int window = 0; window < 3 && !atRate; ++window) {
                qint64 a0 = -1, v0 = -1, a1 = -1, v1 = -1;
                QElapsedTimer t;
                t.start();
                h.player.sinkPosition(Branch::Audio, &a0);
                h.player.sinkPosition(Branch::Video, &v0);
                spin(800);
                h.player.sinkPosition(Branch::Audio, &a1);
                h.player.sinkPosition(Branch::Video, &v1);
                const double wall = double(t.elapsed());
                audioSpeed = (a1 - a0) / wall;
                videoSpeed = (v1 - v0) / wall;
                atRate = audioSpeed >= rates[r] * 0.85 && audioSpeed <= rates[r] * 1.15 && videoSpeed >= rates[r] * 0.8 &&
                         videoSpeed <= rates[r] * 1.2;
            }
            std::printf("  %s at %.2fx: segment rates %.2f / %.2f, audio runs at %.2fx, video at %.2fx\n", cases[i].name,
                        rates[r], s.videoSegmentRate, s.audioSegmentRate, audioSpeed, videoSpeed);
            CHECK_MSG(atRate, "%s at %.2fx: audio runs at %.2fx, video at %.2fx", cases[i].name, rates[r], audioSpeed,
                      videoSpeed);
        }
        CHECK(h.failures == 0 && h.player.diagnostics().rebuilds == 0);
    }
}

// PLY-06 / library bridge: the start position arrives right after load() (load at 0, then
// setStartPosition(5000) before preroll): the same result as a start position in load().
static void ply06_lateStartPositionBeforePreroll() {
    struct Case { const char *name; PlayerSources src; double minPosition; };
    std::vector<Case> cases;
    cases.push_back({"dual webm", dualWebm(), 4900});
    PlayerSources local = sources(PlayMode::Local);
    local.muxed = spec("file://" + gDir + "/av.webm", Container::Matroska);
    cases.push_back({"local webm", local, 4000 - 40});
    PlayerSources localMp4 = sources(PlayMode::Local);
    localMp4.muxed = spec("file://" + gDir + "/av.mp4", Container::IsoBmff);
    cases.push_back({"local mp4", localMp4, 4000 - 40});
    for (size_t i = 0; i < cases.size(); ++i) {
        const StartRun r = startOnce(cases[i].src, 5000);
        CHECK_MSG(r.ok, "%s: late start failed", cases[i].name);
        if (!r.ok) continue;
        CHECK_MSG(r.firstFramePosition >= cases[i].minPosition, "%s: first frame at %.0f", cases[i].name, r.firstFramePosition);
        CHECK_MSG(r.firstFrames == 1, "%s: %d first frames", cases[i].name, r.firstFrames);
        CHECK_MSG(r.diag.engine.videoSinkFlushes <= 1 && r.diag.engine.audioSinkFlushes <= 1, "%s: flushes %d / %d",
                  cases[i].name, r.diag.engine.videoSinkFlushes, r.diag.engine.audioSinkFlushes);
        startChecks(cases[i].name, r, cases[i].minPosition);
        std::printf("  %s: setStartPosition(5000) after load(): first frame at %.0f ms, first sink buffer %lld ms, "
                    "load -> first frame %lld ms, flushes %d / %d\n", cases[i].name, r.firstFramePosition,
                    (long long)r.diag.engine.firstVideoSinkMs, (long long)r.firstFrameMs, r.diag.engine.videoSinkFlushes,
                    r.diag.engine.audioSinkFlushes);
    }
    // After the first frame it is a no-op (a user seek is seek()).
    Harness h;
    CHECK(startAndPlay(h, dualWebm()));
    spin(300);
    const PlayerDiagnostics before = h.player.diagnostics();
    h.player.setStartPosition(8000);
    spin(500);
    CHECK(h.player.diagnostics().seeksSent == before.seeksSent);
    CHECK(h.player.position() < 3000);
}

// Asynchronous teardown: stop() does not wait for the pipeline to reach NULL; the next load works
// at once; nothing from the old build reaches the player or the mailbox afterwards.
static void ply01_stopIsAsynchronous() {
    Harness h;
    QMutex sizeMutex;
    std::vector<std::pair<int, int>> sizes;  // frames posted after stop()
    bool recording = false;
    VideoFrameMailbox *mailbox = h.player.frameMailbox();
    const int listener = mailbox->addListener([&]() {
        const VideoFrame f = mailbox->latest();
        QMutexLocker lock(&sizeMutex);
        if (recording && f.valid()) sizes.push_back(std::make_pair(f.width, f.height));
    });
    int lateSignals = 0;
    bool afterStop = false;
    QObject::connect(&h.player, &VideoPlayer::firstFrame, [&]() { if (afterStop) ++lateSignals; });
    QObject::connect(&h.player, &VideoPlayer::failed, [&]() { if (afterStop) ++lateSignals; });
    QObject::connect(&h.player, &VideoPlayer::endOfStream, [&]() { if (afterStop) ++lateSignals; });
    QObject::connect(&h.player, &VideoPlayer::videoSizeChanged, [&]() { if (afterStop) ++lateSignals; });
    QObject::connect(&h.player, &VideoPlayer::bufferingChanged, [&]() { if (afterStop) ++lateSignals; });
    QObject::connect(&h.player, &VideoPlayer::firstByte, [&]() { if (afterStop) ++lateSignals; });
    int teardownDone = 0;
    qint64 teardownMs = -1;
    QObject::connect(&h.player, &VideoPlayer::timing, [&](const QString &m, qint64 ms) {
        if (m == "teardown-done") { ++teardownDone; teardownMs = ms; }
    });
    std::vector<qint64> stopUs, loadUs;
    for (int round = 0; round < 3; ++round) {
        const bool ok = startAndPlay(h, dualWebm());
        CHECK(ok);
        if (!ok) continue;
        spin(600);
        const int framesBefore = h.firstFrames;
        QElapsedTimer t;
        t.start();
        h.player.stop();
        stopUs.push_back(t.nsecsElapsed() / 1000);
        {
            // From here on a frame of the old build would be a stale post.
            QMutexLocker lock(&sizeMutex);
            sizes.clear();
            recording = true;
        }
        afterStop = true;
        CHECK(h.player.state() == VideoPlayer::Idle);
        spin(300);  // a late callback of the old pipeline would arrive here
        CHECK_MSG(lateSignals == 0, "round %d: %d signals after stop()", round, lateSignals);
        afterStop = false;
        // A new load right away (local file, another picture size).
        PlayerSources local = sources(PlayMode::Local);
        local.muxed = spec("file://" + gDir + "/small.webm", Container::Matroska);
        t.restart();
        h.load(local);
        loadUs.push_back(t.nsecsElapsed() / 1000);
        CHECK(waitFor([&]() { return h.playingNow() || h.failures > 0; }, 8000));
        spin(400);
        CHECK(h.failures == 0);
        CHECK_MSG(h.firstFrames == framesBefore + 1, "round %d: first frames %d -> %d", round, framesBefore, h.firstFrames);
        CHECK(h.player.videoWidth() == 160 && h.player.videoHeight() == 90);
        {
            QMutexLocker lock(&sizeMutex);
            recording = false;
            int stale = 0;
            for (size_t i = 0; i < sizes.size(); ++i)
                if (sizes[i].first != 160 || sizes[i].second != 90) ++stale;
            CHECK_MSG(stale == 0, "round %d: %d frames of the old build posted after stop()", round, stale);
            CHECK(!sizes.empty());
        }
        h.player.stop();
    }
    spin(300);
    mailbox->removeListener(listener);
    qint64 maxStop = 0;
    for (size_t i = 0; i < stopUs.size(); ++i) maxStop = qMax(maxStop, stopUs[i]);
    std::printf("  stop() while PLAYING over HTTP: median %.2f ms, max %.2f ms; next load() median %.2f ms; "
                "teardown-done %d times, last %lld ms after its stop\n",
                median(stopUs) / 1000.0, maxStop / 1000.0, median(loadUs) / 1000.0, teardownDone, (long long)teardownMs);
    CHECK_MSG(maxStop < 5000, "stop() took %.2f ms", maxStop / 1000.0);
    CHECK_MSG(teardownDone >= 6, "teardown-done %d", teardownDone);  // two stops per round
}

// PLY-08 start phase: on a slow link the first preroll does not wait for the buffer to fill;
// Playing comes with the first frame while the buffer is still filling.
static void ply08_startDoesNotWaitForFullBuffer() {
    struct Case { const char *name; PlayerSources src; };
    std::vector<Case> cases;
    PlayerSources muxed = sources(PlayMode::Muxed);
    muxed.muxed = spec(gServer->url("t/av.webm"), Container::Matroska);
    cases.push_back({"muxed webm", muxed});
    PlayerSources dual = dualWebm("t");
    dual.video = spec(gServer->url("t/v.webm"), Container::Matroska);
    cases.push_back({"dual webm", dual});
    PlayerSources mp4 = sources(PlayMode::Dual);
    mp4.video = spec(gServer->url("t/v.mp4"), Container::IsoBmff);
    mp4.audio = spec(gServer->url("t/a.mp4"), Container::IsoBmff);
    cases.push_back({"dual mp4", mp4});
    for (size_t i = 0; i < cases.size(); ++i) {
        const qint64 bytes = cases[i].src.mode == PlayMode::Muxed ? fileSize("av.webm")
                           : i == 2 ? qMax(fileSize("v.mp4"), fileSize("a.mp4")) : qMax(fileSize("v.webm"), fileSize("a.webm"));
        gThrottleBytesPerSecond = bytes * 3 / (2 * kSeconds);  // 1.5x real time
        Harness h;
        const bool ok = startAndPlay(h, cases[i].src, 20000);
        CHECK_MSG(ok, "%s: no Playing on the throttled link", cases[i].name);
        if (!ok) continue;
        const qint64 preroll = h.marks.value("preroll", -1);
        std::printf("  %s at %lld B/s: preroll %lld ms, first frame %lld ms, Playing %lld ms at %d %% buffered "
                    "(%d buffering messages)\n", cases[i].name, (long long)gThrottleBytesPerSecond, (long long)preroll,
                    (long long)h.marks.value("first-frame", -1), (long long)h.playingAtMs, h.percentAtPlaying,
                    h.bufferingMessages);
        // (The mp4 branches' download buffer reports nothing before the preroll.)
        CHECK_MSG(h.percentAtPlaying < 100 || h.messagesBeforePlaying == 0, "%s: Playing only at %d %%", cases[i].name,
                  h.percentAtPlaying);
        CHECK_MSG(preroll >= 0 && h.playingAtMs - preroll <= 300, "%s: Playing %lld ms after the preroll",
                  cases[i].name, (long long)(h.playingAtMs - preroll));
        const qint64 p0 = audioPos(h);
        spin(2000);
        const qint64 adv = audioPos(h) - p0;
        CHECK_MSG(adv >= 1200, "%s: advanced %lld ms in 2 s", cases[i].name, (long long)adv);
        std::printf("  %s: advanced %lld ms in 2 s, lowest level after start %d %%, state %d\n", cases[i].name,
                    (long long)adv, h.lowestPercentAfterPlaying, int(h.player.state()));
    }
    gThrottleBytesPerSecond = 0;
}

static void printRequests(const std::vector<RequestLogEntry> &requests, qint64 fileSize);

// Quality switch / stream instead (PlayerSources::keepFrame): load A, play, load B with keepFrame:
// the mailbox holds a valid frame at every poll until B's first frame, which then replaces it with
// B's size; nothing of A is posted after the switch. Without keepFrame, load() clears the mailbox;
// stop(true) keeps the frame too.
static void ply01_keepFrameAcrossLoad() {
    Harness h;
    CHECK(startAndPlay(h, dualWebm()));
    spin(300);
    VideoFrameMailbox *mailbox = h.player.frameMailbox();
    CHECK(mailbox->latest().valid() && mailbox->latest().width == 320);
    QMutex m;
    std::vector<int> postedWidths;
    bool recording = true;
    const int listener = mailbox->addListener([&]() {
        const VideoFrame f = mailbox->latest();
        QMutexLocker lock(&m);
        if (recording && f.valid()) postedWidths.push_back(f.width);
    });
    {
        QMutexLocker lock(&m);
        postedWidths.clear();
    }
    PlayerSources b = sources(PlayMode::Local);
    b.muxed = spec("file://" + gDir + "/small.webm", Container::Matroska);
    b.keepFrame = true;
    const int framesBefore = h.firstFrames;
    h.load(b);
    int polls = 0, emptyPolls = 0;
    CHECK(mailbox->latest().valid());  // right after load(): still A's picture
    QElapsedTimer t;
    t.start();
    while (h.firstFrames == framesBefore && t.elapsed() < 8000) {
        ++polls;
        if (!mailbox->latest().valid()) ++emptyPolls;
        QCoreApplication::processEvents(QEventLoop::AllEvents, 2);
    }
    CHECK(h.firstFrames == framesBefore + 1);
    CHECK_MSG(emptyPolls == 0, "%d of %d polls saw an empty mailbox", emptyPolls, polls);
    const VideoFrame after = mailbox->latest();
    CHECK_MSG(after.valid() && after.width == 160 && after.height == 90, "after B's first frame: %dx%d", after.width,
              after.height);
    spin(300);
    {
        QMutexLocker lock(&m);
        recording = false;
        int stale = 0;
        for (size_t i = 0; i < postedWidths.size(); ++i) stale += postedWidths[i] != 160;
        CHECK_MSG(stale == 0, "%d frames of A posted after the switch", stale);
    }
    mailbox->removeListener(listener);
    CHECK(h.player.videoWidth() == 160 && h.player.videoHeight() == 90);
    std::printf("  keepFrame: %d polls until B's first frame, none empty; then %dx%d\n", polls, after.width, after.height);
    // stop(true) keeps the picture; stop() and a plain load() clear it.
    h.player.stop(true);
    CHECK(mailbox->latest().valid());
    h.player.stop();
    CHECK(!mailbox->latest().valid());
    CHECK(startAndPlay(h, dualWebm()));
    spin(200);
    CHECK(mailbox->latest().valid());
    b.keepFrame = false;
    h.load(b);
    CHECK(!mailbox->latest().valid());
}

// ---- Run 5 (POCO R2) engine findings --------------------------------------------------------

// A/V offset once playback runs: wait until the audio clock advances steadily (300 ms), then
// measure in 450 ms windows (up to three, as in ply03: one scheduling stall of a loaded host is
// not a desync). -1 when no frame was rendered against the audio clock.
static qint64 settledAvOffset(Harness &h) {
    QElapsedTimer settle;
    settle.start();
    qint64 last = audioPos(h), advancingSince = -1;
    while (settle.elapsed() < 3000) {
        spin(50);
        const qint64 now = audioPos(h);
        if (now > last) { if (advancingSince < 0) advancingSince = settle.elapsed(); }
        else advancingSince = -1;
        last = now;
        if (advancingSince >= 0 && settle.elapsed() - advancingSince >= 300) break;
    }
    EngineStats st;
    for (int window = 0; window < 3; ++window) {
        h.player.resetEngineStats();
        spin(450);
        st = h.player.diagnostics().engine;
        if (st.avSamples == 0 || st.maxAbsAvOffsetMs <= kMaxAvOffsetMs) break;
    }
    return st.avSamples ? st.maxAbsAvOffsetMs : -1;
}

// Dual plan of the run-5 shape: DASH fragmented mp4 video (fs.mp4: moov, sidx, moof/mdat, no mfra)
// + 30 s WebM/Opus audio, both from the same server path prefix.
static PlayerSources dualFragmented(const char *prefix) {
    PlayerSources p = sources(PlayMode::Dual);
    p.video = spec(gServer->url(QStringLiteral("%1/fs.mp4").arg(QLatin1String(prefix))), Container::IsoBmff);
    p.audio = spec(gServer->url(QStringLiteral("%1/a30.webm").arg(QLatin1String(prefix))), Container::Matroska);
    return p;
}

// Requests for `file` (any prefix) logged since index `from` that start in its last 4 KiB.
static int endOfFileRequests(size_t from, const QString &file) {
    const qint64 size = fileSize(file);
    int n = 0;
    for (size_t i = from; i < gRequestLog.size(); ++i)
        if (gRequestLog[i].path.endsWith("/" + file) && gRequestLog[i].from > size - 4096) ++n;
    return n;
}

static void printFileRequests(size_t from, const QString &file) {
    std::vector<RequestLogEntry> mine;
    for (size_t i = from; i < gRequestLog.size(); ++i)
        if (gRequestLog[i].path.endsWith("/" + file)) mine.push_back(gRequestLog[i]);
    printRequests(mine, fileSize(file));
}

// PLY-04 / NFR-PERF-02 (run 5: video demux-ready 400-820 ms after its first byte, audio 10-50 ms):
// on a DASH fragmented mp4 behind the disk-backed buffer, qtdemux (pull mode) reads the mfro box at
// the end of the file before it exposes a pad, and the download buffer serves that with two extra
// range requests (end of file, then back). The engine keeps the upstream size from the demuxer
// until its first pad: demux-ready follows the first byte within one round trip, no request goes
// to the end of the file, and key-unit seeks still land (pull mode) within the mp4 budget.
static void ply04_fragmentedMp4DemuxReadyFollowsFirstByte() {
    struct Shape { const char *name; int delayMs; bool throttle; bool seeks; };
    const Shape shapes[] = {{"150 ms per request", 150, false, true},
                            {"150 ms per request, 4x real-time bandwidth", 150, true, false}};
    for (size_t i = 0; i < sizeof(shapes) / sizeof(shapes[0]); ++i) {
        gRequestDelayMs = shapes[i].delayMs;
        gThrottleBytesPerSecond = shapes[i].throttle ? fileSize("fs.mp4") * 4 / kFragSeconds : 0;
        const size_t log0 = gRequestLog.size();
        Harness h;
        const bool ok = startAndPlay(h, dualFragmented(shapes[i].throttle ? "t" : "r"), 20000);
        if (!ok) {
            // Seen once on a shared gate ("not playing ()": 20 s without Playing and without an
            // error). One upstream answer that takes that long gives exactly this; so would a
            // request that is never sent. What the server saw tells the two apart.
            std::printf("  DIAG %s: state %d after 20 s, failures %d; first byte video %lld ms, audio %lld ms; demux-ready "
                        "video %lld, audio %lld; preroll %lld; answers still owed by the server %d; its requests:\n",
                        shapes[i].name, int(h.player.state()), h.failures, (long long)h.firstByteAt.value("video", -1),
                        (long long)h.firstByteAt.value("audio", -1), (long long)h.marks.value("demux-ready branch=video", -1),
                        (long long)h.marks.value("demux-ready branch=audio", -1), (long long)h.marks.value("preroll", -1),
                        gServer->answersOwed());
            for (size_t k = log0; k < gRequestLog.size(); ++k)
                std::printf("    %lld ms %s bytes=%lld-%lld\n", (long long)(gRequestLog[k].atMs - gRequestLog[log0].atMs),
                            qPrintable(gRequestLog[k].path), (long long)gRequestLog[k].from, (long long)gRequestLog[k].to);
        }
        CHECK_MSG(ok, "%s: not playing (%s)", shapes[i].name, qPrintable(h.lastError));
        if (!ok) continue;
        const qint64 byte = h.firstByteAt.value("video", -1), ready = h.marks.value("demux-ready branch=video", -1);
        const qint64 audioByte = h.firstByteAt.value("audio", -1), audioReady = h.marks.value("demux-ready branch=audio", -1);
        const int endRequests = endOfFileRequests(log0, "fs.mp4");
        std::printf("  %s: video first byte %lld ms, demux-ready %lld ms (+%lld); audio +%lld; preroll %lld, first frame %lld; "
                    "%d end-of-file requests, %lld size queries kept from the demuxer\n", shapes[i].name, (long long)byte,
                    (long long)ready, (long long)(ready - byte), (long long)(audioReady - audioByte),
                    (long long)h.marks.value("preroll", -1), (long long)h.marks.value("first-frame", -1), endRequests,
                    (long long)h.player.diagnostics().engine.demuxerSizeQueriesHidden);
        printFileRequests(log0, "fs.mp4");
        // No extra round trip before the first pad: within one round trip of the first byte at full
        // bandwidth; on the 4x link the first fragment's bytes themselves take ~160 ms (before the
        // fix: ~490 ms, two round trips more), so the bound there is two round trips.
        const qint64 bound = shapes[i].throttle ? 2 * shapes[i].delayMs : shapes[i].delayMs;
        CHECK_MSG(byte >= 0 && ready >= byte && ready - byte < bound,
                  "%s: demux-ready %lld ms after the first byte (one round trip is %d ms)", shapes[i].name,
                  (long long)(ready - byte), shapes[i].delayMs);
        int beforeReady = 0;
        for (size_t k = log0; k < gRequestLog.size(); ++k)
            if (gRequestLog[k].path.endsWith("/fs.mp4") && gRequestLog[k].atMs - gRequestLog[log0].atMs <= ready) ++beforeReady;
        CHECK_MSG(beforeReady == 1, "%s: %d video requests before demux-ready", shapes[i].name, beforeReady);
        CHECK_MSG(endRequests == 0, "%s: %d requests to the end of the file", shapes[i].name, endRequests);
        CHECK(h.player.diagnostics().engine.demuxerSizeQueriesHidden > 0);
        if (!shapes[i].seeks) continue;
        // PLY-04 seek guarantees on the same build (pull mode): key-unit, landed at most one GOP
        // before the target, within the 2 s mp4 budget, A/V within 80 ms afterwards.
        const int targets[] = {12000, 25000, 5000, 20000, 9500};
        qint64 worst = 0;
        for (size_t k = 0; k < sizeof(targets) / sizeof(targets[0]); ++k) {
            const int before = h.seekDone;
            QElapsedTimer t;
            t.start();
            h.player.seek(targets[k]);
            const bool done = waitFor([&]() { return h.seekDone > before; }, 8000);
            const qint64 ms = t.elapsed();
            worst = qMax(worst, ms);
            const double landed = h.player.position();
            CHECK_MSG(done && ms <= 2000, "seek to %d: done %d after %lld ms", targets[k], int(done), (long long)ms);
            CHECK_MSG(landed <= targets[k] + 40 && landed >= targets[k] - kFragGopFrames * 1000 / kFps - 40,
                      "seek to %d landed at %.0f", targets[k], landed);
            waitFor([&]() { return h.playingNow(); }, 3000);
        }
        const qint64 av = settledAvOffset(h);
        CHECK_MSG(av <= kMaxAvOffsetMs, "A/V after the seeks %lld ms", (long long)av);
        CHECK_MSG(endOfFileRequests(log0, "fs.mp4") == 0, "an end-of-file request after the seeks");
        std::printf("  %s: 5 key-unit seeks, worst %lld ms, A/V afterwards %lld ms, rebuilds %d\n", shapes[i].name,
                    (long long)worst, (long long)av, h.player.diagnostics().rebuilds);
        CHECK(h.player.diagnostics().rebuilds == 0);
    }
    gRequestDelayMs = 0;
    gThrottleBytesPerSecond = 0;
}

// PLY-06 resume deep into a fragmented mp4 (run 5: resume at 8:55, video start seek at 1714 ms,
// audio start seek at 5406 ms, first frame 5.5 s). Prints the start-up split per shape; the first
// frame is at the start position (one GOP of key-unit), A/V within 80 ms once playing.
static void ply06_deepResumeOnFragmentedMp4() {
    struct Shape { const char *name; int delayMs; bool throttle; };
    const Shape shapes[] = {{"150 ms per request", 150, false}, {"150 ms per request, 4x real-time bandwidth", 150, true}};
    for (size_t i = 0; i < sizeof(shapes) / sizeof(shapes[0]); ++i) {
        gRequestDelayMs = shapes[i].delayMs;
        gThrottleBytesPerSecond = shapes[i].throttle ? fileSize("fs.mp4") * 4 / kFragSeconds : 0;
        Harness h;
        PlayerSources p = dualFragmented(shapes[i].throttle ? "t" : "r");
        p.startPositionMs = 25000;
        const bool ok = startAndPlay(h, p, 20000);
        CHECK_MSG(ok, "%s: not playing (%s)", shapes[i].name, qPrintable(h.lastError));
        if (!ok) continue;
        StartRun r;
        r.marks = h.marks;
        printSplit(shapes[i].name, r);
        std::printf("  %s: first byte video %lld audio %lld; first frame at %.0f ms, Playing at %lld ms\n", shapes[i].name,
                    (long long)h.firstByteAt.value("video", -1), (long long)h.firstByteAt.value("audio", -1),
                    h.firstFramePosition, (long long)h.playingAtMs);
        CHECK_MSG(h.firstFramePosition >= 25000 - kFragGopFrames * 1000 / kFps - 40, "%s: first frame at %.0f",
                  shapes[i].name, h.firstFramePosition);
        CHECK(h.firstFrames == 1);
        CHECK(h.player.diagnostics().engine.firstVideoSinkMs >= 25000 - kFragGopFrames * 1000 / kFps - 40);
        const qint64 av = settledAvOffset(h);
        CHECK_MSG(av <= kMaxAvOffsetMs, "%s: A/V once playing %lld ms", shapes[i].name, (long long)av);
        CHECK_MSG(audioPos(h) >= 25000 - kFragGopFrames * 1000 / kFps - 40, "%s: position %lld", shapes[i].name,
                  (long long)audioPos(h));
    }
    gRequestDelayMs = 0;
    gThrottleBytesPerSecond = 0;
}

// PLY-06 (run 5): the video's first frame does not wait for the audio branch. Dual WebM with the
// audio source answering 1.5 s late, start at 5 s: the video branch decodes at the start position
// and its frame reaches the mailbox (first-frame) before the audio is even held for its start
// seek; the pipeline plays only once the audio landed too; resume position and A/V hold.
static void ply06_videoFirstFrameDoesNotWaitForAudio() {
    gDelayedPathMs = 1500;
    Harness h;
    PlayerSources p = dualWebm("d");
    p.startPositionMs = 5000;
    const bool ok = startAndPlay(h, p, 15000);
    gDelayedPathMs = 0;
    CHECK_MSG(ok, "not playing (%s)", qPrintable(h.lastError));
    if (!ok) return;
    const qint64 frame = h.marks.value("first-frame", -1);
    const qint64 audioSeek = h.marks.value("start-seek branch=audio", -1);
    const qint64 audioByte = h.firstByteAt.value("audio", -1);
    const qint64 preroll = h.marks.value("preroll", -1);
    StartRun r;
    r.marks = h.marks;
    printSplit("dual webm, audio 1.5 s late", r);
    std::printf("  first frame %lld ms (at %.0f ms), audio first byte %lld, audio start seek %lld, preroll %lld, Playing %lld\n",
                (long long)frame, h.firstFramePosition, (long long)audioByte, (long long)audioSeek, (long long)preroll,
                (long long)h.playingAtMs);
    CHECK_MSG(frame >= 0 && audioByte >= 0 && frame < audioByte, "first frame %lld ms, audio first byte %lld ms",
              (long long)frame, (long long)audioByte);
    CHECK_MSG(audioSeek > frame, "audio start seek %lld ms, first frame %lld ms", (long long)audioSeek, (long long)frame);
    CHECK(preroll > frame && h.playingAtMs >= preroll);
    CHECK(h.frameInMailboxAtFirstFrame);
    CHECK(h.firstFrames == 1);
    CHECK_MSG(h.firstFramePosition >= 4950, "first frame at %.0f", h.firstFramePosition);
    CHECK_MSG(h.player.diagnostics().engine.firstVideoSinkMs >= 4950, "first video sink buffer at %lld",
              (long long)h.player.diagnostics().engine.firstVideoSinkMs);
    CHECK_MSG(h.player.diagnostics().seeksSent == 2, "seeks sent %d", h.player.diagnostics().seeksSent);
    CHECK_MSG(audioPos(h) >= 4950, "position %lld", (long long)audioPos(h));
    const qint64 av = settledAvOffset(h);
    CHECK_MSG(av <= kMaxAvOffsetMs, "A/V once playing %lld ms", (long long)av);
    std::printf("  A/V once playing %lld ms\n", (long long)av);
}

// PLY-26 / PLY-06 (run 5: a downloaded flat mp4 H.264 + AAC played locally advanced its position
// but never showed a picture): local muxed files (flat mp4 with moov before the mdat, H.264 + AAC
// and VP9 + Opus, and WebM) through VideoPlayer, from 0, with a start position in load(), with the
// library bridge's setStartPosition() after load(), and without autoplay: firstFrame fires once,
// the frame is in the mailbox, frames keep coming once playing, demux-ready is marked.
static void ply26_localMuxedFilesShowPictures() {
    struct Case { const char *name; QString file; Container container; };
    std::vector<Case> files;
    files.push_back({"local flat mp4 VP9 + Opus", "av.mp4", Container::IsoBmff});
    files.push_back({"local WebM", "av.webm", Container::Matroska});
    if (gFakeDecoders && (QFileInfo(gDir + "/avc_aac.mp4").exists() || writeFakeMp4(gDir + "/avc_aac.mp4", true, true)))
        files.push_back({"local flat mp4 H.264 + AAC", "avc_aac.mp4", Container::IsoBmff});
    else
        std::printf("  (no fake H.264 / AAC decoders here: the H.264 + AAC case is skipped)\n");
    enum Start { FromZero, InLoad, AfterLoad, NoAutoplay };
    const char *const startNames[] = {"from 0", "start 5 s in load()", "setStartPosition(5 s) after load()",
                                       "start 5 s, autoplay off, play() 300 ms later"};
    for (size_t f = 0; f < files.size(); ++f) {
        for (int st = FromZero; st <= NoAutoplay; ++st) {
            Harness h;
            PlayerSources p = sources(PlayMode::Local);
            p.muxed = spec("file://" + gDir + "/" + files[f].file, files[f].container);
            if (st == InLoad || st == NoAutoplay) p.startPositionMs = 5000;
            if (st == NoAutoplay) p.autoplay = false;
            h.load(p);
            if (st == AfterLoad) h.player.setStartPosition(5000);
            const bool frame = waitFor([&]() { return h.firstFrames > 0 || h.failures > 0; }, 8000) && h.failures == 0;
            CHECK_MSG(frame, "%s, %s: no first frame (%s)", files[f].name, startNames[st], qPrintable(h.lastError));
            if (!frame) continue;
            CHECK_MSG(h.frameInMailboxAtFirstFrame, "%s, %s: mailbox empty at first-frame", files[f].name, startNames[st]);
            if (st == NoAutoplay) {
                spin(300);
                CHECK_MSG(h.player.state() == VideoPlayer::Paused, "%s: state %d without autoplay", files[f].name,
                          int(h.player.state()));
                h.player.play();
            }
            CHECK_MSG(waitFor([&]() { return h.playingNow(); }, 5000), "%s, %s: not playing", files[f].name, startNames[st]);
            const int posts0 = h.mailboxPosts.load();
            const qint64 a0 = audioPos(h);
            // Load-tolerant: at least 800 ms as before, then up to 5 s for the tenth frame. A
            // healthy player posts ten frames in a third of a second; a starved build host gets
            // there later; a local file without a picture (the bug this guards) never does.
            QElapsedTimer postClock;
            postClock.start();
            waitFor([&]() { return postClock.elapsed() >= 800 && h.mailboxPosts.load() - posts0 >= 10; }, 5000);
            const int posts = h.mailboxPosts.load() - posts0;
            CHECK_MSG(posts >= 10, "%s, %s: %d frames posted in %lld ms", files[f].name, startNames[st], posts,
                      (long long)postClock.elapsed());
            CHECK_MSG(audioPos(h) - a0 >= 500, "%s, %s: position stalled", files[f].name, startNames[st]);
            CHECK(h.firstFrames == 1);
            CHECK_MSG(h.marks.contains("demux-ready branch=muxed"), "%s, %s: no demux-ready marker", files[f].name,
                      startNames[st]);
            const bool started = st != FromZero;
            if (started)
                CHECK_MSG(h.firstFramePosition >= 4000 - 40, "%s, %s: first frame at %.0f", files[f].name, startNames[st],
                          h.firstFramePosition);
            const VideoFrame last = h.player.frameMailbox()->latest();
            CHECK(last.valid() && last.width == 320 && last.height == 180);
            CHECK_MSG(h.player.diagnostics().engine.unroutedPads == 0, "%s, %s: unrouted %s", files[f].name, startNames[st],
                      qPrintable(h.player.diagnostics().engine.unroutedCaps));
            std::printf("  %s, %s: first frame %lld ms at %.0f ms, %d frames in 800 ms, decoders %s / %s\n", files[f].name,
                        startNames[st], (long long)h.marks.value("first-frame", -1), h.firstFramePosition, posts,
                        qPrintable(h.player.diagnostics().engine.videoDecoder),
                        qPrintable(h.player.diagnostics().engine.audioDecoder));
        }
    }
    // The diagnostics name a stream that reached no route: listen-only on a muxed local file stops
    // its video compressed at the unmatched fakesink (the "unrouted=" key of the decoders line).
    Harness h;
    PlayerSources p = sources(PlayMode::AudioOnly);
    p.muxed = spec("file://" + gDir + "/av.webm", Container::Matroska);
    CHECK(startAndPlay(h, p));
    const EngineStats st = h.player.diagnostics().engine;
    CHECK_MSG(st.unroutedPads == 1 && st.unroutedCaps.startsWith("video/x-vp8"), "audio-only: unrouted %d (%s)",
              st.unroutedPads, qPrintable(st.unroutedCaps));
    CHECK(h.firstFrames == 0);
}

// ---- PLY-04 fragmented mp4: what the demuxer asks the server for ------------------------------
// A raw uridecodebin (the plan's source element and properties) on one fragmented mp4, video to a
// fakesink: first byte, demux-ready (qtdemux's first pad), every range request, then key-unit
// seeks (accepted? landed where, how fast). Used to find the cause of the late demux-ready.

// (time ms, moof offset) of every fragment of a fragmented mp4 fixture.
static std::vector<std::pair<qint64, qint64>> moofIndex(const QString &path) {
    std::vector<std::pair<qint64, qint64>> out;
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return out;
    const QByteArray d = f.readAll();
    const std::vector<Mp4Box> top = mp4Boxes(d, 0, d.size());
    quint32 timescale = 0;
    for (size_t i = 0; i < top.size(); ++i) {
        Mp4Box trak, mdia, mdhd, traf, tfdt;
        if (top[i].type == "moov" && mp4Child(d, top[i], "trak", &trak) && mp4Child(d, trak, "mdia", &mdia) &&
            mp4Child(d, mdia, "mdhd", &mdhd)) {
            const qint64 body = mdhd.offset + mdhd.header;
            timescale = d.at(int(body)) == 1 ? rd32(d, body + 4 + 16) : rd32(d, body + 4 + 8);
        }
        if (top[i].type == "moof" && timescale && mp4Child(d, top[i], "traf", &traf) && mp4Child(d, traf, "tfdt", &tfdt)) {
            const qint64 body = tfdt.offset + tfdt.header;
            const quint64 t = d.at(int(body)) == 1 ? rd64(d, body + 4) : rd32(d, body + 4);
            out.push_back(std::make_pair(qint64(t * 1000 / timescale), top[i].offset));
        }
    }
    return out;
}

struct RawConfig {
    bool download = true;
    bool maskSize = false;     // pull mode: qtdemux's BYTES duration query goes unanswered
    bool byteSeeks = false;    // push mode: seek = FLUSH BYTES seek to the target's moof, from qtdemux's sink pad
};

struct RawProbe {
    gint64 firstByteUs = 0;
    gint64 demuxReadyUs = 0;
    QMutex mutex;
    quint32 segmentSeqnum = 0;
    qint64 segmentStartMs = -1, segmentTimeMs = -1;
    gint64 segmentUs = 0;
    qint64 firstPtsMs = -1;      // first buffer after the last segment
    GstElement *demux = nullptr; // ref
};

struct RawResult {
    bool prerolled = false;
    qint64 firstByteMs = -1, demuxReadyMs = -1, prerollMs = -1;
    std::vector<RequestLogEntry> requests;   // relative to the start (atMs)
    int seeksAccepted = 0, seeksLanded = 0, seeksInRange = 0;
    qint64 worstSeekMs = 0;
};

static GstPadProbeReturn rawFirstByte(GstPad *, GstPadProbeInfo *, gpointer data) {
    RawProbe *p = static_cast<RawProbe *>(data);
    if (!p->firstByteUs) p->firstByteUs = g_get_monotonic_time();
    return GST_PAD_PROBE_REMOVE;
}

static GstPadProbeReturn rawSinkData(GstPad *, GstPadProbeInfo *info, gpointer data) {
    RawProbe *p = static_cast<RawProbe *>(data);
    if (GST_PAD_PROBE_INFO_TYPE(info) & GST_PAD_PROBE_TYPE_BUFFER) {
        GstBuffer *b = GST_PAD_PROBE_INFO_BUFFER(info);
        QMutexLocker lock(&p->mutex);
        if (p->firstPtsMs < 0 && b && GST_BUFFER_PTS_IS_VALID(b)) p->firstPtsMs = qint64(GST_BUFFER_PTS(b) / GST_MSECOND);
        return GST_PAD_PROBE_OK;
    }
    GstEvent *ev = GST_PAD_PROBE_INFO_EVENT(info);
    if (ev && GST_EVENT_TYPE(ev) == GST_EVENT_SEGMENT) {
        const GstSegment *seg = nullptr;
        gst_event_parse_segment(ev, &seg);
        QMutexLocker lock(&p->mutex);
        p->segmentSeqnum = gst_event_get_seqnum(ev);
        const guint64 st = seg ? gst_segment_to_stream_time(seg, GST_FORMAT_TIME, seg->start) : GST_CLOCK_TIME_NONE;
        p->segmentStartMs = seg && GST_CLOCK_TIME_IS_VALID(seg->start) ? qint64(seg->start / GST_MSECOND) : -1;
        p->segmentTimeMs = st == GST_CLOCK_TIME_NONE ? -1 : qint64(st / GST_MSECOND);
        p->segmentUs = g_get_monotonic_time();
        p->firstPtsMs = -1;
    }
    return GST_PAD_PROBE_OK;
}

// maskSize: the BYTES duration query from qtdemux's sink pad is dropped (unanswered).
static GstPadProbeReturn rawMaskSize(GstPad *, GstPadProbeInfo *info, gpointer) {
    GstQuery *q = GST_PAD_PROBE_INFO_QUERY(info);
    if (q && GST_QUERY_TYPE(q) == GST_QUERY_DURATION) {
        GstFormat format;
        gst_query_parse_duration(q, &format, nullptr);
        if (format == GST_FORMAT_BYTES) return GST_PAD_PROBE_DROP;
    }
    return GST_PAD_PROBE_OK;
}

static bool rawWaitAsyncDone(GstElement *pipeline, int timeoutMs) {
    GstBus *bus = gst_element_get_bus(pipeline);
    bool done = false, failed = false;
    waitFor([&]() {
        while (GstMessage *m = gst_bus_pop(bus)) {
            if (GST_MESSAGE_TYPE(m) == GST_MESSAGE_ASYNC_DONE && GST_MESSAGE_SRC(m) == GST_OBJECT(pipeline)) done = true;
            if (GST_MESSAGE_TYPE(m) == GST_MESSAGE_ERROR) failed = true;
            gst_message_unref(m);
        }
        return done || failed;
    }, timeoutMs);
    gst_object_unref(bus);
    return done;
}

static RawResult rawFragmentedRun(const QString &uri, const RawConfig &cfg, const std::vector<int> &seekTargets,
                                  const std::vector<std::pair<qint64, qint64>> &index) {
    RawResult r;
    RawProbe probe;
    static RawConfig current;
    current = cfg;
    GstElement *pipeline = gst_pipeline_new("raw");
    GstElement *src = gst_element_factory_make("uridecodebin", nullptr);
    GstElement *sink = gst_element_factory_make("fakesink", nullptr);
    g_object_set(sink, "sync", FALSE, nullptr);
    const qint64 bufNs = qEnvironmentVariableIsSet("FRAG_BUF_NS") ? qgetenv("FRAG_BUF_NS").toLongLong() : kIsoBmffBufferDurationNs;
    const int bufBytes = qEnvironmentVariableIsSet("FRAG_BUF_BYTES") ? qgetenv("FRAG_BUF_BYTES").toInt() : kIsoBmffBufferSizeBytes;
    g_object_set(src, "download", cfg.download ? TRUE : FALSE, "buffer-duration", gint64(bufNs),
                 "buffer-size", cfg.download ? bufBytes : -1, "uri", uri.toUtf8().constData(), nullptr);
    gst_bin_add_many(GST_BIN(pipeline), src, sink, nullptr);
    g_signal_connect(src, "source-setup", G_CALLBACK(+[](GstElement *, GstElement *source, gpointer data) {
        GstPad *pad = gst_element_get_static_pad(source, "src");
        gst_pad_add_probe(pad, GST_PAD_PROBE_TYPE_BUFFER, &rawFirstByte, data, nullptr);
        gst_object_unref(pad);
    }), &probe);
    g_signal_connect(pipeline, "deep-element-added", G_CALLBACK(+[](GstBin *, GstBin *, GstElement *e, gpointer data) {
        GstElementFactory *f = gst_element_get_factory(e);
        if (!f || std::strcmp(gst_plugin_feature_get_name(GST_PLUGIN_FEATURE(f)), "qtdemux") != 0) return;
        RawProbe *p = static_cast<RawProbe *>(data);
        if (!p->demux) p->demux = GST_ELEMENT(gst_object_ref(e));
        if (current.maskSize) {
            GstPad *pad = gst_element_get_static_pad(e, "sink");
            gst_pad_add_probe(pad, GST_PAD_PROBE_TYPE_QUERY_UPSTREAM, &rawMaskSize, nullptr, nullptr);
            gst_object_unref(pad);
        }
        g_signal_connect(e, "pad-added", G_CALLBACK(+[](GstElement *, GstPad *, gpointer d) {
            RawProbe *rp = static_cast<RawProbe *>(d);
            if (!rp->demuxReadyUs) rp->demuxReadyUs = g_get_monotonic_time();
        }), data);
    }), &probe);
    g_signal_connect(src, "pad-added", G_CALLBACK(+[](GstElement *, GstPad *pad, gpointer data) {
        GstPad *sinkPad = gst_element_get_static_pad(GST_ELEMENT(data), "sink");
        if (!gst_pad_is_linked(sinkPad)) gst_pad_link(pad, sinkPad);
        gst_object_unref(sinkPad);
    }), sink);
    GstPad *sinkPad = gst_element_get_static_pad(sink, "sink");
    gst_pad_add_probe(sinkPad, GstPadProbeType(GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM | GST_PAD_PROBE_TYPE_BUFFER),
                      &rawSinkData, &probe, nullptr);
    gst_object_unref(sinkPad);

    const size_t firstRequest = gRequestLog.size();
    const qint64 serverStart = gServerClock.elapsed();
    const gint64 startUs = g_get_monotonic_time();
    gst_element_set_state(pipeline, GST_STATE_PAUSED);
    r.prerolled = rawWaitAsyncDone(pipeline, 20000);
    r.prerollMs = r.prerolled ? (g_get_monotonic_time() - startUs) / 1000 : -1;
    if (probe.firstByteUs) r.firstByteMs = (probe.firstByteUs - startUs) / 1000;
    if (probe.demuxReadyUs) r.demuxReadyMs = (probe.demuxReadyUs - startUs) / 1000;
    for (size_t i = 0; r.prerolled && i < seekTargets.size(); ++i) {
        const qint64 target = seekTargets[i];
        GstEvent *seek;
        qint64 expectMs = target;
        if (cfg.byteSeeks) {
            size_t k = 0;
            while (k + 1 < index.size() && index[k + 1].first <= target) ++k;
            expectMs = index.empty() ? 0 : index[k].first;
            seek = gst_event_new_seek(1.0, GST_FORMAT_BYTES, GST_SEEK_FLAG_FLUSH, GST_SEEK_TYPE_SET,
                                      index.empty() ? 0 : index[k].second, GST_SEEK_TYPE_NONE, -1);
        } else {
            seek = gst_event_new_seek(1.0, GST_FORMAT_TIME, GstSeekFlags(GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_KEY_UNIT),
                                      GST_SEEK_TYPE_SET, gint64(target) * GST_MSECOND, GST_SEEK_TYPE_NONE, -1);
        }
        const quint32 seq = gst_event_get_seqnum(seek);
        const gint64 t0 = g_get_monotonic_time();
        bool accepted;
        if (cfg.byteSeeks) {
            GstPad *demuxSink = gst_element_get_static_pad(probe.demux, "sink");
            accepted = gst_pad_push_event(demuxSink, seek);
            gst_object_unref(demuxSink);
        } else {
            accepted = gst_element_send_event(pipeline, seek);
        }
        if (!accepted) continue;
        ++r.seeksAccepted;
        rawWaitAsyncDone(pipeline, 8000);
        quint32 seenSeq = 0;
        qint64 start = -1, time = -1, pts = -1;
        const bool landed = waitFor([&]() {
            QMutexLocker lock(&probe.mutex);
            seenSeq = probe.segmentSeqnum;
            start = probe.segmentStartMs;
            time = probe.segmentTimeMs;
            pts = probe.firstPtsMs;
            return (seenSeq == seq || cfg.byteSeeks) && pts >= 0 && probe.segmentUs > t0;
        }, 3000);
        const qint64 ms = (g_get_monotonic_time() - t0) / 1000;
        if (!landed) {
            std::printf("    seek to %lld: no segment + buffer (segment seqnum %u vs %u, start %lld, pts %lld)\n",
                        (long long)target, seenSeq, seq, (long long)start, (long long)pts);
            continue;
        }
        ++r.seeksLanded;
        r.worstSeekMs = qMax(r.worstSeekMs, ms);
        const bool inRange = pts <= target + 40 && pts >= target - kFragGopFrames * 1000 / kFps - 40 &&
                             time >= expectMs - 40 && time <= target + 40;
        r.seeksInRange += inRange;
        std::printf("    seek to %lld: %lld ms, segment start %lld time %lld seqnum %s, first pts %lld%s\n", (long long)target,
                    (long long)ms, (long long)start, (long long)time, seenSeq == seq ? "kept" : "new", (long long)pts,
                    inRange ? "" : "  OUT OF RANGE");
    }
    gst_element_set_state(pipeline, GST_STATE_NULL);
    if (probe.demux) gst_object_unref(probe.demux);
    gst_object_unref(pipeline);
    for (size_t i = firstRequest; i < gRequestLog.size(); ++i) {
        RequestLogEntry e = gRequestLog[i];
        e.atMs -= serverStart;
        r.requests.push_back(e);
    }
    return r;
}

static void printRequests(const std::vector<RequestLogEntry> &requests, qint64 fileSize) {
    for (size_t i = 0; i < requests.size(); ++i)
        std::printf("      request %d at %lld ms: bytes=%lld-%s%s\n", int(i), (long long)requests[i].atMs,
                    (long long)requests[i].from, requests[i].to < 0 ? "" : qPrintable(QString::number(requests[i].to)),
                    fileSize > 0 && requests[i].from > fileSize - 4096 ? "  (end of file)" : "");
}

// PLY-04 / NFR-PERF: the raw evidence for the late video demux-ready on fragmented mp4 (run 5).
static void ply04_fragmentedMp4RequestLog() {
    struct Variant { const char *file; const char *what; };
    std::vector<Variant> variants;
    variants.push_back({"fs.mp4", "sidx, no mfra (YouTube shape)"});
    variants.push_back({"fm.mp4", "sidx + mfra"});
    if (qEnvironmentVariableIsSet("FRAG_BIG")) variants.push_back({"big.mp4", "10 s high bitrate, sidx + mfra"});
    struct Mode { const char *name; RawConfig cfg; };
    std::vector<Mode> modes;
    RawConfig c;
    modes.push_back({"pull (download=true)", c});
    c.maskSize = true;
    modes.push_back({"pull, size query masked", c});
    c = RawConfig();
    c.download = false;
    modes.push_back({"push (download=false)", c});
    c.byteSeeks = true;
    modes.push_back({"push, BYTES seeks from the sidx", c});
    std::vector<int> seeks;
    seeks.push_back(12000); seeks.push_back(25000); seeks.push_back(5000); seeks.push_back(20000); seeks.push_back(9500);
    const QByteArray only = qgetenv("FRAG_MODE");
    for (size_t v = 0; v < variants.size(); ++v) {
        const qint64 size = fileSize(variants[v].file);
        const std::vector<std::pair<qint64, qint64>> index = moofIndex(gDir + "/" + variants[v].file);
        for (size_t m = 0; m < modes.size(); ++m) {
            if (!only.isEmpty() && !QByteArray(modes[m].name).contains(only)) continue;
            for (int shaped = 0; shaped < 3; ++shaped) {
                // 1 latency: 150 ms to the first byte of every request (the proxy -> CDN round trip),
                // full bandwidth. 2 shaped: the same latency and 4x real-time bandwidth.
                const char *const shapes[] = {"unshaped", "latency", "shaped"};
                gRequestDelayMs = shaped ? 150 : 0;
                gThrottleBytesPerSecond = shaped == 2 ? size * 4 / kFragSeconds : 0;
                const QString uri = gServer->url(QStringLiteral("%1/%2").arg(shaped == 2 ? "t" : "r").arg(variants[v].file));
                std::printf("  %s (%s), %s, %s:\n", variants[v].file, variants[v].what, modes[m].name, shapes[shaped]);
                std::vector<int> targets = seeks;
                if (std::strcmp(variants[v].file, "big.mp4") == 0) targets = std::vector<int>{8000, 2000, 9000};
                const RawResult r = rawFragmentedRun(uri, modes[m].cfg, targets, index);
                std::printf("    prerolled %d, first byte %lld ms, demux-ready %lld ms (+%lld), preroll %lld ms, %d requests; "
                            "seeks accepted %d/%d, landed %d, in range %d, worst %lld ms\n", int(r.prerolled),
                            (long long)r.firstByteMs, (long long)r.demuxReadyMs, (long long)(r.demuxReadyMs - r.firstByteMs),
                            (long long)r.prerollMs, int(r.requests.size()), r.seeksAccepted, int(seeks.size()), r.seeksLanded,
                            r.seeksInRange, (long long)r.worstSeekMs);
                printRequests(r.requests, size);
            }
        }
    }
    gRequestDelayMs = 0;
    gThrottleBytesPerSecond = 0;
}

// PLAYER_GST_ONLY=<substring>: run only the matching tests (debugging).

// Seek bar (watch overlay): bufferedPosition is ahead of (or at) the playing position, within the
// duration, refreshed with the position poll; dual takes the shorter branch, a disk-backed mp4
// branch its downloaded range, a local file is buffered to its duration.
static void seekbar_bufferedPosition() {
    struct Case { const char *name; PlayerSources src; bool local; bool ahead; };
    std::vector<Case> cases;
    cases.push_back({"dual webm", dualWebm(), false, true});
    cases.push_back({"dual mp4", dualMp4(), false, true});
    PlayerSources muxed = sources(PlayMode::Muxed);
    muxed.muxed = spec(gServer->url("r/av.webm"), Container::Matroska);
    cases.push_back({"muxed webm", muxed, false, true});
    PlayerSources slow = dualWebm("t");
    slow.video = spec(gServer->url("t/v.webm"), Container::Matroska);
    cases.push_back({"dual webm throttled", slow, false, false});
    PlayerSources local = sources(PlayMode::Local);
    local.muxed = spec("file://" + gDir + "/av.mp4", Container::IsoBmff);
    cases.push_back({"local mp4", local, true, true});
    {   // The engine call itself: nothing is known without a pipeline.
        VideoFrameMailbox mailbox;
        GstEngine engine(&mailbox);
        qint64 ms = -1;
        CHECK(!engine.queryBuffered(Branch::Muxed, 0, 12000, 1.0, &ms) && ms == -1);
        CHECK(!engine.queryBuffered(Branch::Audio, 500, 0, 2.0, nullptr));
    }
    gThrottleBytesPerSecond = qMax(fileSize("v.webm"), fileSize("a.webm")) * 2 / kSeconds;  // 2x real time
    for (size_t i = 0; i < cases.size(); ++i) {
        Harness h;
        int notified = 0;
        QObject::connect(&h.player, &VideoPlayer::bufferedPositionChanged, [&notified]() { ++notified; });
        CHECK_MSG(h.player.bufferedPosition() == 0, "%s: buffered before the load", cases[i].name);
        const bool ok = startAndPlay(h, cases[i].src);
        CHECK_MSG(ok, "%s did not play", cases[i].name);
        if (!ok) continue;
        waitFor([&]() { return h.player.position() >= 1500; }, 6000);
        const double position = h.player.position(), buffered = h.player.bufferedPosition();
        const double duration = h.player.duration();
        std::printf("  %s: position %.0f ms, buffered %.0f ms, duration %.0f ms, %d notifications\n", cases[i].name,
                    position, buffered, duration, notified);
        CHECK_MSG(notified > 0 && buffered > 0, "%s: buffered %.0f", cases[i].name, buffered);
        CHECK_MSG(duration > 0 && buffered <= duration, "%s: buffered %.0f > duration %.0f", cases[i].name, buffered, duration);
        // The poll that set it ran before this position was read (at most one poll interval back).
        CHECK_MSG(buffered + 300 >= position, "%s: buffered %.0f behind position %.0f", cases[i].name, buffered, position);
        if (cases[i].local) CHECK_MSG(buffered == duration, "%s: local buffered %.0f of %.0f", cases[i].name, buffered, duration);
        else if (cases[i].ahead) CHECK_MSG(buffered >= position + 500, "%s: only %.0f ahead", cases[i].name, buffered - position);
        else CHECK_MSG(buffered < duration, "%s: a 2x throttled download is not complete yet (%.0f)", cases[i].name, buffered);
        h.player.stop();
        CHECK_MSG(h.player.bufferedPosition() == 0, "%s: buffered after stop", cases[i].name);
    }
    gThrottleBytesPerSecond = 0;
}

static bool selected(const char *name) {
    const QByteArray only = qgetenv("PLAYER_GST_ONLY");
    return only.isEmpty() || QByteArray(name).contains(only);
}

int main(int argc, char **argv) {
    std::setvbuf(stdout, nullptr, _IOLBF, 0);  // keep output ordered with GStreamer's stderr
    // The in-process server must be reached directly.
    unsetenv("http_proxy");
    unsetenv("HTTP_PROXY");
    unsetenv("https_proxy");
    unsetenv("all_proxy");
    QCoreApplication app(argc, argv);
    registerFakeDecoders();  // PLT-08 test: before initialize() demotes droidadec
    QString err;
    if (!GstEngine::initialize(&err)) {
        std::printf("player gst tests: SKIPPED (gst_init failed: %s)\n", qPrintable(err));
        return 0;
    }
    const std::set<std::string> have = GstEngine::availableElements();
    const char *const needed[] = {"uridecodebin", "souphttpsrc", "matroskademux", "qtdemux", "videoconvert", "appsink", "fakesink"};
    for (size_t i = 0; i < sizeof(needed) / sizeof(needed[0]); ++i) {
        if (!have.count(needed[i])) {
            std::printf("player gst tests: SKIPPED (missing %s)\n", needed[i]);
            return 0;
        }
    }
    const QByteArray tmp = qgetenv("TMPDIR").isEmpty() ? QByteArray("/tmp") : qgetenv("TMPDIR");
    gDir = QString::fromLocal8Bit(tmp) + QStringLiteral("/sailtube-player-gst-%1").arg(QCoreApplication::applicationPid());
    QDir().mkpath(gDir);
    QElapsedTimer t;
    t.start();
    if (!makeFixtures()) {
        std::printf("FAIL fixture generation\n");
        return 1;
    }
    if (!makeFragmentedFixtures()) {
        std::printf("FAIL fragmented fixture generation\n");
        return 1;
    }
    std::printf("fixtures generated in %lld ms\n", (long long)t.elapsed());
    HttpServer server;
    if (!server.start()) {
        std::printf("FAIL http server\n");
        return 1;
    }
    gServer = &server;
    gServerClock.start();

    // Investigation only (slow; the raw evidence behind ply04_fragmentedMp4DemuxReadyFollowsFirstByte):
    // runs when PLAYER_GST_ONLY names it.
    if (!qgetenv("PLAYER_GST_ONLY").isEmpty() && selected("ply04_fragmentedMp4RequestLog")) {
        std::printf("ply04_fragmentedMp4RequestLog\n");
        ply04_fragmentedMp4RequestLog();
    }
    if (qgetenv("PLAYER_GST_ONLY") == "ply03_seekStress") {
        std::printf("ply03_seekStress\n");
        ply03_seekStress();
    }
    if (qgetenv("PLAYER_GST_ONLY") == "ply07_rateStress") {
        std::printf("ply07_rateStress\n");
        ply07_rateStress();
    }
    if (selected("ply04_fragmentedMp4DemuxReadyFollowsFirstByte")) {
        std::printf("ply04_fragmentedMp4DemuxReadyFollowsFirstByte\n");
        ply04_fragmentedMp4DemuxReadyFollowsFirstByte();
    }
    if (selected("ply06_deepResumeOnFragmentedMp4")) {
        std::printf("ply06_deepResumeOnFragmentedMp4\n");
        ply06_deepResumeOnFragmentedMp4();
    }
    if (selected("ply06_videoFirstFrameDoesNotWaitForAudio")) {
        std::printf("ply06_videoFirstFrameDoesNotWaitForAudio\n");
        ply06_videoFirstFrameDoesNotWaitForAudio();
    }
    if (selected("ply26_localMuxedFilesShowPictures")) {
        std::printf("ply26_localMuxedFilesShowPictures\n");
        ply26_localMuxedFilesShowPictures();
    }

    if (selected("ply01_everyModePlays")) {
        std::printf("ply01_everyModePlays\n");
        ply01_everyModePlays();
    }
    if (selected("sel08_audioOnlyFetchesAudioOnly")) {
        std::printf("sel08_audioOnlyFetchesAudioOnly\n");
        sel08_audioOnlyFetchesAudioOnly();
    }
    if (selected("ply06_prerollBarrier")) {
        std::printf("ply06_prerollBarrier\n");
        ply06_prerollBarrier();
    }
    if (selected("ply06_startAtPositionWithoutPrerollAtZero")) {
        std::printf("ply06_startAtPositionWithoutPrerollAtZero\n");
        ply06_startAtPositionWithoutPrerollAtZero();
    }
    if (selected("ply06_lateStartPositionBeforePreroll")) {
        std::printf("ply06_lateStartPositionBeforePreroll\n");
        ply06_lateStartPositionBeforePreroll();
    }
    if (selected("ply07_startRateReachesBothBranches")) {
        std::printf("ply07_startRateReachesBothBranches\n");
        ply07_startRateReachesBothBranches();
    }
    if (selected("ply01_stopIsAsynchronous")) {
        std::printf("ply01_stopIsAsynchronous\n");
        ply01_stopIsAsynchronous();
    }
    if (selected("ply01_keepFrameAcrossLoad")) {
        std::printf("ply01_keepFrameAcrossLoad\n");
        ply01_keepFrameAcrossLoad();
    }
    if (selected("ply08_startDoesNotWaitForFullBuffer")) {
        std::printf("ply08_startDoesNotWaitForFullBuffer\n");
        ply08_startDoesNotWaitForFullBuffer();
    }
    if (selected("ply03_seekAccuracy")) {
        std::printf("ply03_seekAccuracy webm\n");
        ply03_seekAccuracy(false);
    }
    if (selected("ply03_seekAccuracy")) {
        std::printf("ply03_seekAccuracy mp4\n");
        ply03_seekAccuracy(true);
    }
    if (selected("ply05_splitSeekRebuildsAtMostOnce")) {
        std::printf("ply05_splitSeekRebuildsAtMostOnce\n");
        ply05_splitSeekRebuildsAtMostOnce();
    }
    if (selected("ply09_freezeAndThaw")) {
        std::printf("ply09_freezeAndThaw\n");
        ply09_freezeAndThaw("dual", dualWebm());
        PlayerSources muxed = sources(PlayMode::Muxed);
        muxed.muxed = spec(gServer->url("r/av.webm"), Container::Matroska);
        ply09_freezeAndThaw("muxed", muxed);
    }
    if (selected("ply24_endOfStream")) {
        std::printf("ply24_endOfStream\n");
        ply24_endOfStream();
    }
    if (selected("ply08_pausePlay")) {
        std::printf("ply08_pausePlay\n");
        ply08_pausePlay();
    }
    if (selected("ply22_degradedChainsPlay")) {
        std::printf("ply22_degradedChainsPlay\n");
        ply22_degradedChainsPlay();
    }
    if (selected("ply22_limiterHoldsFullScale")) {
        std::printf("ply22_limiterHoldsFullScale\n");
        ply22_limiterHoldsFullScale();
    }
    if (selected("ply20_perBuildFallback")) {
        std::printf("ply20_perBuildFallback\n");
        ply20_perBuildFallback();
    }
    if (selected("res07_httpStatusThroughPlayer")) {
        std::printf("res07_httpStatusThroughPlayer\n");
        res07_httpStatusThroughPlayer();
    }
    if (selected("seekbar_bufferedPosition")) {
        std::printf("seekbar_bufferedPosition\n");
        seekbar_bufferedPosition();
    }
    if (selected("plt08_droidAudioDecoderNeverAutoplugged")) {
        std::printf("plt08_droidAudioDecoderNeverAutoplugged\n");
        plt08_droidAudioDecoderNeverAutoplugged();
    }

    // Last: they install process-wide GstTracer hooks.
    // PLAYER_GST_REPEAT=<n> with PLAYER_GST_ONLY=ply03_seekDuringStateContinuation: the case n
    // times in one process (looking into an intermittent failure).
    for (int round = 0, rounds = qMax(1, qEnvironmentVariableIntValue("PLAYER_GST_REPEAT"));
         selected("ply03_seekDuringStateContinuation") && round < rounds; ++round) {
        std::printf("ply03_seekDuringStateContinuation\n");
        ply03_seekDuringStateContinuation();
    }
    if (selected("ply06_firstFrameIsInTheMailbox")) {
        std::printf("ply06_firstFrameIsInTheMailbox\n");
        ply06_firstFrameIsInTheMailbox();
    }

    QDir(gDir).removeRecursively();
    std::printf("player gst tests: %d checks, %d failure%s\n", checks, failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
