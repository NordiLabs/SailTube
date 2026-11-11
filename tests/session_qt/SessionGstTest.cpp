// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// The real PlaybackSession on the real VideoPlayer + GStreamer engine, driven through the shared
// stream source interface (Shared/Playback/StreamSource.h): a fake StreamResolver answers a
// StreamSet of https formats, a fake StreamFetcher maps them to generated fixtures served by an
// in-process HTTP server (standing in for MediaProxy's loopback URLs), audio to fakesink.
// Covers: open -> one resolve -> dual plan with both branches opened through the fetcher -> first
// frame and the timing chain (PLY-01, NFR-PERF-02); a byte-0 refusal -> reportRefused + the next
// candidate (RES-07/12); a refusal at open -> the next candidate (RES-06); quality / listen-only
// switches from the same set without a resolve, the kept stream reused (SEL-07/08); the audioOnly
// hint -> one AudioOnly resolve (SEL-08); a final error -> no retry, a retryable one -> one fresh
// re-resolve (RES-09/10); the warm-up hook runs once after the first swap (RES-21); E12: a press
// speculation reused by the open (one resolve, path=prefetch), latest press wins, cancel before
// and after completion, rows prefetched on unmetered networks only, nothing in the background;
// the HTTP status of a byte-0 refusal decides refusalAtByteZero (RES-07). Batch 3: a downloaded
// video opens from the device (no resolve, link start, saved segments and captions; preferStream
// and file_missing stream), PO-token readiness in the request (RES-14), errorSettingKey, the queue
// read-out signal, and the resolve dispatched before the previous player stops (NFR-PERF-02).
// Cover video: one pipeline while the cover shows the video, the freeze lifted while the cover is
// visible and back after it hides; videoAspect from the plan before the first frame. Shorts pager
// (DSC-17 MAY): likelyNext preloads (answer and held stream jobs reused by the open, faster first
// frame), its bounds and replacement; loop items (restart at 0, no `ended`, no resume point);
// pager <-> watch page context switches of the same Short; one Short plays at a time. Watch
// overlay: bufferedPosition, the dub picker switching from the same set (no resolve, the video
// stream kept), stable volume, hold-to-speed, a writable loop, the sleep timer (minutes, end of
// video, off; a 1 s label tick only while bound), the skipper's segments, the scrub preview, and
// SEL-06 captions through the catalog's caption models (a json3 fixture cue at the position).
// Stall watchdog (PLY-18's half without an error): the server stops sending mid-stream (the
// fixture server's /stall/ path) -> detected, the candidate reopened through new fetcher jobs
// plays on, the picture kept, no resolve and no refusal report; a refused reopen -> one fresh
// resolve; a pipeline stuck behind the session's back -> the seek in place; the budget used up ->
// "Playback stopped responding." and Retry; paused and in the background (PLY-09 freeze) -> no
// false stall.
// Build: tests/session_qt/run.sh
#include "Clients/Sailfish/Presentation/AppSettings.h"
#include "Clients/Sailfish/Presentation/CaptionCues.h"
#include "Clients/Sailfish/Presentation/CaptionTracksModel.h"
#include "Clients/Sailfish/Presentation/PlaybackCaptions.h"
#include "Clients/Sailfish/Presentation/PlaybackSession.h"
#include "Core/Catalog/Infrastructure/CaptionService.h"
#include "Clients/Sailfish/Presentation/PlaybackTiming.h"
#include "Core/Player/Infrastructure/GstEngine.h"
#include "Core/Player/Infrastructure/HardwareDecodeGate.h"
#include "Core/Player/Infrastructure/VideoPlayer.h"

#include <gst/gst.h>

#include <QAtomicInt>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QHash>
#include <QSettings>
#include <QTcpServer>
#include <QTcpSocket>
#include <QThread>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <functional>
#include <memory>
#include <unistd.h>
#include <dirent.h>
#include "Core/Player/Infrastructure/MemoryTrim.h"

using namespace nordtile::video;
using namespace nordtile::video::sailfish;
namespace player = nordtile::player;

static int checks = 0;
static int failures = 0;
#define CHECK(c) do { ++checks; if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)
#define CHECK_MSG_SESSION(c, ...) do { ++checks; if (!(c)) { std::printf("FAIL %s:%d: ", __FILE__, __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); ++failures; } } while (0)

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

static QString gDir;

static bool runPipeline(const QString &description) {
    GError *err = nullptr;
    GstElement *p = gst_parse_launch(description.toUtf8().constData(), &err);
    if (!p || err) {
        std::printf("fixture parse error: %s\n", err ? err->message : "?");
        if (err) g_error_free(err);
        if (p) gst_object_unref(p);
        return false;
    }
    gst_element_set_state(p, GST_STATE_PLAYING);
    GstBus *bus = gst_element_get_bus(p);
    GstMessage *m = gst_bus_timed_pop_filtered(bus, 120 * GST_SECOND, GstMessageType(GST_MESSAGE_EOS | GST_MESSAGE_ERROR));
    const bool ok = m && GST_MESSAGE_TYPE(m) == GST_MESSAGE_EOS;
    if (m) gst_message_unref(m);
    gst_object_unref(bus);
    gst_element_set_state(p, GST_STATE_NULL);
    gst_object_unref(p);
    return ok;
}

static bool makeFixtures() {
    const QString d = gDir + "/";
    const QString video = "videotestsrc num-buffers=120 pattern=ball ! video/x-raw,width=320,height=180,framerate=30/1 ! "
                          "vp9enc deadline=1 cpu-used=8 keyframe-max-dist=30 ! vp9parse ! ";
    const QString audio = "audiotestsrc num-buffers=200 samplesperbuffer=960 wave=sine ! audio/x-raw,rate=48000,channels=2 ! opusenc ! ";
    // Stall watchdog: 12 s clips, long enough to stand still, be recovered and play on.
    const QString longVideo = QString(video).replace(QLatin1String("num-buffers=120"), QLatin1String("num-buffers=360"));
    const QString longAudio = QString(audio).replace(QLatin1String("num-buffers=200"), QLatin1String("num-buffers=600"));
    return runPipeline(longVideo + "webmmux ! filesink location=" + d + "sv.webm") &&
           runPipeline(longAudio + "webmmux ! filesink location=" + d + "sa.webm") &&
           runPipeline(video + "webmmux ! filesink location=" + d + "v.webm") &&
           runPipeline(audio + "webmmux ! filesink location=" + d + "a.webm") &&
           runPipeline("webmmux name=m ! filesink location=" + d + "av.webm " + video + "m. " + audio + "m.") &&
           // PLY-26 (run 5): a flat downloaded mp4, moov before the one mdat.
           runPipeline("mp4mux faststart=true name=m ! filesink location=" + d + "av.mp4 " + video + "m. " + audio + "m.");
}

// GET /<file> with Range support; counts requests per path. GET /stall/<file> is the same file
// behind a source that goes silent: from `stallFraction` of the file up to the index at its end
// (the WebM Cues, which the demuxer reads at the start) nothing is ever delivered; a response that
// reaches that hole stops there and its connection stays open, with no error and no end.
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
    std::string url(const char *file) const {
        return QStringLiteral("http://127.0.0.1:%1/%2").arg(server_.serverPort()).arg(QLatin1String(file)).toStdString();
    }
    int requests(const char *file) const { return requests_.value(QStringLiteral("/") + QLatin1String(file)); }
    void reset() { requests_.clear(); stalled_ = 0; }
    double stallFraction = 0.25;
    int stalled() const { return stalled_; }  // responses left hanging at the hole

private:
    void respond(QTcpSocket *s, const QByteArray &request) {
        const QList<QByteArray> lines = request.split('\n');
        const QString path = QString::fromLatin1(lines.value(0).trimmed().split(' ').value(1));
        requests_[path] += 1;
        const bool stall = path.startsWith(QLatin1String("/stall/"));
        QFile f(gDir + (stall ? path.mid(6) : path));
        if (!f.open(QIODevice::ReadOnly)) {
            s->write("HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
            s->disconnectFromHost();
            return;
        }
        const QByteArray data = f.readAll();
        qint64 from = 0, to = data.size() - 1;
        bool partial = false;
        for (int i = 1; i < lines.size(); ++i) {
            const QByteArray l = lines[i].trimmed();
            if (l.toLower().startsWith("range: bytes=")) {
                const QByteArray spec = l.mid(13);
                const int dash = spec.indexOf('-');
                from = spec.left(dash).toLongLong();
                const QByteArray end = spec.mid(dash + 1);
                if (!end.isEmpty()) to = qMin<qint64>(end.toLongLong(), data.size() - 1);
                partial = true;
            }
        }
        QByteArray head = partial ? QByteArray("HTTP/1.1 206 Partial Content\r\nContent-Range: bytes ") +
                                        QByteArray::number(from) + "-" + QByteArray::number(to) + "/" +
                                        QByteArray::number(data.size()) + "\r\n"
                                  : QByteArray("HTTP/1.1 200 OK\r\n");
        head += "Accept-Ranges: bytes\r\nContent-Type: application/octet-stream\r\nConnection: close\r\nContent-Length: " +
                QByteArray::number(to - from + 1) + "\r\n\r\n";
        s->write(head);
        if (stall) {
            const qint64 holeStart = qint64(data.size() * stallFraction);
            const int cues = data.lastIndexOf(QByteArray("\x1C\x53\xBB\x6B", 4));  // the Cues element
            const qint64 holeEnd = cues > holeStart ? cues : data.size();
            if (from < holeEnd && to >= holeStart) {  // the range runs into the hole: silence from there
                if (from < holeStart) s->write(data.mid(int(from), int(holeStart - from)));
                s->flush();
                ++stalled_;
                return;
            }
        }
        s->write(data.mid(int(from), int(to - from + 1)));
        s->disconnectFromHost();
    }
    QTcpServer server_;
    QHash<QString, int> requests_;
    int stalled_ = 0;
};

static HttpServer *gServer = nullptr;

// Timing marker lines (sailtube-timing: ...) printed through qInfo.
static QStringList gMarkers;
static void collectMessages(QtMsgType type, const QMessageLogContext &, const QString &message) {
    if (message.startsWith(QLatin1String("sailtube-timing: "))) { gMarkers << message.mid(17); return; }
    if (type != QtDebugMsg && type != QtInfoMsg) std::fprintf(stderr, "%s\n", qPrintable(message));
}
static int markerIndex(const QString &prefix, int from = 0) {
    for (int i = from; i < gMarkers.size(); ++i)
        if (gMarkers[i].startsWith(prefix)) return i;
    return -1;
}
static bool markerHas(int index, const char *key) { return index >= 0 && gMarkers[index].contains(QLatin1String(key)); }

using nordtile::playback::StreamError;
using nordtile::playback::StreamErrorClass;
using nordtile::playback::StreamFormat;
using nordtile::playback::StreamRequest;
using nordtile::playback::StreamSet;

static StreamFormat format(const char *id, const char *file, const char *vcodec, const char *acodec, int height) {
    StreamFormat f;
    f.formatId = QLatin1String(id);
    f.container = QStringLiteral("webm");
    f.url = QUrl(QStringLiteral("https://media.test/%1?itag=%2").arg(QLatin1String(file)).arg(QLatin1String(id)));
    f.videoCodec = QLatin1String(vcodec);
    f.audioCodec = QLatin1String(acodec);
    f.height = height;
    f.width = height * 16 / 9;
    f.fps = height ? 30 : 0;
    f.bitrateKbps = height ? 800 : 128;
    f.contentLength = 100000;
    f.originalAudio = !height;
    f.headers << qMakePair(QByteArray("User-Agent"), QByteArray("SailTube-session-test"));
    return f;
}

static StreamSet fixtureSet(bool brokenDualVideo) {
    StreamSet s;
    s.videoId = QStringLiteral("FIXTURE0001");
    s.video << format("247", brokenDualVideo ? "missing.webm" : "v.webm", "vp09.00.10.08", "none", 360);
    s.audio << format("251", "a.webm", "none", "opus", 0);
    s.muxed << format("43", "av.webm", "vp09.00.10.08", "opus", 240);
    s.path = nordtile::playback::ResolvePath::FastPath;
    s.resolveMs = 40;
    s.phases.probeMs = 12;
    return s;
}

// A StreamResolver answering the fixture set (or an error) on the next event-loop turn.
class FakeResolver final : public QObject, public nordtile::playback::StreamResolver, public nordtile::player::SourceFaultSink {
public:
    bool broken = false;
    int delayMs = 5;
    StreamErrorClass errorClass = StreamErrorClass::None;
    bool errorNotFinal = false;       // the error's final flag cleared (a bare "unavailable", RES-09)
    qint64 scheduledStartUnixMs = 0;  // RES-19: carried by an Upcoming answer
    std::vector<StreamRequest> requests, prefetches;
    std::vector<quint64> cancelled;
    std::function<void()> onResolve;  // runs inside resolve() (dispatch-order checks)
    std::function<void(StreamSet &)> shape;  // edits the answered set (format sizes)
    QStringList invalidated, refused;  // refused: format ids
    quint64 resolve(const StreamRequest &r, Completion done) override {
        requests.push_back(r);
        if (onResolve) onResolve();
        const quint64 op = next_++;
        pending_[op] = done;
        StreamSet set = fixtureSet(broken);
        set.videoId = r.videoId;
        set.scheduledStartUnixMs = scheduledStartUnixMs;
        if (shape) shape(set);
        StreamError e = errorClass == StreamErrorClass::None ? StreamError()
                                                             : nordtile::playback::streamError(errorClass, r.signedIn);
        if (errorNotFinal) e.final = false;
        QTimer::singleShot(delayMs, this, [this, op, set, e]() { finish(op, set, e); });
        return op;
    }
    void prefetch(const StreamRequest &r) override { prefetches.push_back(r); }
    void cancel(quint64 op) override {
        cancelled.push_back(op);
        QTimer::singleShot(0, this, [this, op]() { finish(op, StreamSet(), nordtile::playback::streamError(StreamErrorClass::Cancelled, false)); });
    }
    int answered() const { return static_cast<int>(requests.size()) - static_cast<int>(pending_.size()); }
    void invalidate(const QString &id) override { invalidated << id; }
    // RES-07: the session reports a typed fault instead of invalidate() to a resolver that takes one.
    std::vector<nordtile::player::SourceFault> faults;
    void reportFault(const std::string &id, nordtile::player::SourceFault fault) override {
        invalidated << QString::fromStdString(id);
        faults.push_back(fault);
    }
    void reportRefused(const QString &, const StreamFormat &f) override { refused << f.formatId; }
    QVariantMap diagnostics() const override { return QVariantMap(); }
    int resolves() const { return static_cast<int>(requests.size()); }

private:
    void finish(quint64 op, const StreamSet &s, const StreamError &e) {
        const auto it = pending_.find(op);
        if (it == pending_.end()) return;
        const Completion c = it->second;
        pending_.erase(it);
        c(s, e);
    }
    quint64 next_ = 1;
    std::map<quint64, Completion> pending_;
};

// A StreamFetcher standing in for MediaProxy: https://media.test/<file> -> the local server.
class FakeFetcher final : public QObject, public nordtile::playback::StreamFetcher {
public:
    QStringList opened, closed;   // format ids
    QStringList refuseAtOpen;     // format ids answered NoPlayableStream (the source's probe refused)
    // Stall watchdog seams. healOnReopen: the second and later jobs of a format serve the healthy
    // file where the first served /stall/ (a new proxy job reaches the media again).
    // refuseReopen: an address opened before is refused the second time (the job cannot be had again).
    bool healOnReopen = false;
    bool refuseReopen = false;
    int open_ = 0;
    int openDelayMs = 10;         // how long an open takes to answer
    quint64 open(const StreamFormat &f, Opened done) override {
        const bool again = openedUrls_.contains(f.url);
        openedUrls_ << f.url;
        opened << f.formatId;
        const quint64 h = next_++;
        handles_[h] = f.formatId;
        ++open_;
        const bool refuse = refuseAtOpen.contains(f.formatId) || (refuseReopen && again);
        QString file = f.url.path().mid(1);
        if (healOnReopen && again && file.startsWith(QLatin1String("stall/"))) file = file.mid(6);
        const QUrl local(QString::fromStdString(gServer->url(file.toLatin1().constData())));
        QTimer::singleShot(openDelayMs, this, [this, h, done, refuse, local]() {
            if (!handles_.count(h)) return;  // closed: Cancelled already queued
            if (refuse) { handles_.erase(h); --open_; done(QUrl(), nordtile::playback::streamError(StreamErrorClass::NoPlayableStream, false)); }
            else done(local, StreamError());
        });
        return h;
    }
    void close(quint64 h) override {
        const auto it = handles_.find(h);
        if (it == handles_.end()) return;
        closed << it->second;
        handles_.erase(it);
        --open_;
    }
    QVariantMap diagnostics() const override { return QVariantMap(); }

private:
    quint64 next_ = 1;
    std::map<quint64, QString> handles_;
    QList<QUrl> openedUrls_;
};

static media::DecoderCapabilities capabilities() {
    media::DecoderCapabilities c;
    c.vp9.available = c.vp9.software = true;
    c.h264.available = c.h264.software = true;
    return c;
}

struct Harness {
    FakeResolver resolver;
    FakeFetcher fetcher;
    AppSettings settings;
    PlaybackSession session;
    int networkClass = 0;  // SEL-02 probe: 0 unmetered, 1 mobile
    int warmUps = 0;
    explicit Harness(bool broken = false) : session(&resolver, &fetcher, &settings, capabilities(), false) {
        resolver.broken = broken;
        session.setNetworkProbe([this]() { return networkClass; });
        session.setWarmUp([this]() { ++warmUps; });
        player::SinkOverride audio;
        audio.factory = "fakesink";
        player::PropertySetting sync;
        sync.name = "sync";
        sync.value = "true";
        audio.properties.push_back(sync);
        session.setSinkOverrides(audio, player::SinkOverride());
    }
    bool ready() const { return session.state() == PlaybackSession::Ready; }
};

static int sourceBranches(const player::PipelinePlan &plan) { return static_cast<int>(plan.branches.size()); }

// Every source branch opens what the fetcher returned (the loopback server), never the set URL.
static bool loopbackSources(const player::PipelinePlan &plan) {
    if (plan.branches.empty()) return false;
    for (const player::BranchPlan &b : plan.branches)
        if (b.source.property("uri").compare(0, 17, "http://127.0.0.1:") != 0) return false;
    return true;
}

// PLY-01 / RES-01 / RES-16..17 / NFR-PERF-02: one resolve, both branches opened through the
// fetcher, the player loads the local URLs, the timing chain is complete.
static void ply01_openDualPlanFirstFrame() {
    gServer->reset();
    gMarkers.clear();
    Harness h;
    int firstFrames = 0;
    QObject::connect(h.session.player(), &player::VideoPlayer::firstFrame, [&firstFrames]() { ++firstFrames; });
    QVariantMap hints;
    hints.insert(QStringLiteral("title"), QStringLiteral("Fixture"));
    hints.insert(QStringLiteral("channel"), QStringLiteral("Channel"));
    hints.insert(QStringLiteral("durationSeconds"), 4);
    h.session.open(QStringLiteral("FIXTURE0001"), hints);
    CHECK(h.resolver.resolves() == 1);
    CHECK(waitFor([&]() { return h.ready() && h.session.playing(); }, 15000));
    CHECK(waitFor([&]() { return firstFrames > 0; }, 5000));
    CHECK(h.resolver.resolves() == 1);
    const StreamRequest &r = h.resolver.requests[0];
    CHECK(r.videoId == QLatin1String("FIXTURE0001") && r.purpose == nordtile::playback::StreamPurpose::Video);
    CHECK(!r.signedIn && !r.speculative && r.maxHeight == 720);   // RES-20, SEL-02
    CHECK(r.videoCodecs.value(0) == QLatin1String("avc1"));       // software decode: H.264 first
    CHECK(h.fetcher.opened.contains(QStringLiteral("247")) && h.fetcher.opened.contains(QStringLiteral("251")));
    CHECK(!h.fetcher.opened.contains(QStringLiteral("43")));
    const player::PipelinePlan &plan = h.session.player()->plan();
    CHECK(plan.mode == player::PlayMode::Dual);
    CHECK(sourceBranches(plan) == 2);
    CHECK(loopbackSources(plan));
    CHECK(gServer->requests("v.webm") > 0 && gServer->requests("a.webm") > 0);
    CHECK(gServer->requests("av.webm") == 0);
    CHECK(h.session.title() == QLatin1String("Fixture"));        // from the hints, no details call
    CHECK(h.session.duration() > 0);
    CHECK(h.session.diagnostics().value(QStringLiteral("resolvePath")).toString() == QLatin1String("fast"));
    // NFR-PERF-02 chain for run 1: tap -> resolve-start -> resolve-done path=fast -> probe-done ->
    // first-byte -> preroll-done (first-frame needs a window swap; unit-checked below).
    const int tap = markerIndex(QStringLiteral("tap "));
    const int rs = markerIndex(QStringLiteral("resolve-start "), tap);
    const int rd = markerIndex(QStringLiteral("resolve-done "), rs);
    const int pd = markerIndex(QStringLiteral("probe-done "), rd);
    const int fb = markerIndex(QStringLiteral("first-byte "), pd);
    const int pr = markerIndex(QStringLiteral("preroll-done "), fb);
    CHECK(tap >= 0 && rs > tap && rd > rs && pd > rd && fb > pd && pr > fb);
    CHECK(markerHas(rd, "run=1") && markerHas(rd, "path=fast") && markerHas(fb, "run=1"));
    h.session.surfaceFrameReady();
    h.session.frameSwapped();
    CHECK(markerHas(markerIndex(QStringLiteral("first-frame ")), "run=1"));
    // MPR-01 Stop really stops: no resolve, no stream job.
    h.session.stop();
    CHECK(h.session.state() == PlaybackSession::Idle);
    CHECK(h.session.player()->state() == player::VideoPlayer::Idle);
    CHECK(h.session.diagnostics().value(QStringLiteral("streamJobs")).toInt() == 0);
    CHECK(h.fetcher.open_ == 0);
}

// RES-07 / RES-12 / PLY-18: the dual candidate's video is refused at byte 0 (404) -> the session
// reports that format refused, then the muxed candidate plays; no second resolve.
static void res07_byteZeroRefusalReportsAndFallsBack() {
    gServer->reset();
    Harness h(true);
    h.session.open(QStringLiteral("FIXTURE0001"));
    CHECK(waitFor([&]() { return h.ready() && h.session.playing(); }, 20000));
    CHECK(h.session.controller().attempt() >= 1);
    CHECK(h.session.player()->plan().mode == player::PlayMode::Muxed);
    CHECK(gServer->requests("missing.webm") > 0 && gServer->requests("av.webm") > 0);
    CHECK(h.resolver.refused == QStringList() << QStringLiteral("247"));
    CHECK(h.resolver.resolves() == 1);
    CHECK(h.session.controller().recovery().triesUsed() == 0);
    h.session.stop();
}

// RES-06: the source refuses the video format at open (its probe) -> the next candidate, with no
// extra report (the source counted it) and no second resolve.
static void res06_openRefusalNextCandidate() {
    gServer->reset();
    Harness h;
    h.fetcher.refuseAtOpen << QStringLiteral("247");
    h.session.open(QStringLiteral("FIXTURE0001"));
    CHECK(waitFor([&]() { return h.ready() && h.session.playing(); }, 15000));
    CHECK(h.session.player()->plan().mode == player::PlayMode::Muxed);
    CHECK(gServer->requests("v.webm") == 0);
    CHECK(h.resolver.refused.isEmpty() && h.resolver.resolves() == 1);
    h.session.stop();
}

// SEL-07 / SEL-08: a quality switch and listen-only re-select from the same set (no resolve);
// the stream job of a kept format is reused, not opened again.
static void sel07_switchesFromSameSet() {
    gServer->reset();
    Harness h;
    h.session.open(QStringLiteral("FIXTURE0001"));
    CHECK(waitFor([&]() { return h.ready() && h.session.playing(); }, 15000));
    CHECK(h.fetcher.opened.count(QStringLiteral("251")) == 1);
    CHECK(waitFor([&]() { return h.session.player()->frameMailbox()->latest().valid(); }, 5000));
    h.session.setQuality(360, false);
    // keepFrame: the last picture stays on screen through the switch (no black frame).
    int emptyPolls = 0;
    QElapsedTimer switchClock;
    switchClock.start();
    while (!(h.ready() && h.session.playing()) && switchClock.elapsed() < 15000) {
        if (!h.session.player()->frameMailbox()->latest().valid()) ++emptyPolls;
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
    }
    CHECK(h.ready() && h.session.playing());
    if (emptyPolls) std::printf("  picture empty during the quality switch (%d polls)\n", emptyPolls);
    CHECK(emptyPolls == 0);
    CHECK(h.resolver.resolves() == 1);
    CHECK(h.session.player()->plan().mode == player::PlayMode::Dual);
    CHECK(h.fetcher.opened.count(QStringLiteral("251")) == 1 && h.fetcher.opened.count(QStringLiteral("247")) == 1);
    h.session.setAudioOnlyMode(true);
    CHECK(waitFor([&]() { return h.ready() && h.session.playing(); }, 15000));
    CHECK(h.resolver.resolves() == 1);
    CHECK(h.session.player()->plan().mode == player::PlayMode::AudioOnly);
    CHECK(h.fetcher.opened.count(QStringLiteral("251")) == 1);
    CHECK(waitFor([&]() { return h.fetcher.closed.contains(QStringLiteral("247")); }, 2000));  // the dropped video job
    h.session.stop();
    h.session.setAudioOnlyMode(false);
}

// SEL-08: listen-only builds no video branch and fetches only the audio format.
static void sel08_listenOnlyFetchesAudioOnly() {
    gServer->reset();
    Harness h;
    h.session.setAudioOnlyMode(true);
    h.session.open(QStringLiteral("FIXTURE0001"));
    CHECK(waitFor([&]() { return h.ready() && h.session.playing(); }, 15000));
    CHECK(h.session.player()->plan().mode == player::PlayMode::AudioOnly);
    CHECK(!h.session.player()->plan().hasVideo());
    CHECK(!h.session.hasVideo());
    CHECK(gServer->requests("a.webm") > 0);
    CHECK(gServer->requests("v.webm") == 0 && gServer->requests("av.webm") == 0);
    CHECK(h.fetcher.opened == QStringList() << QStringLiteral("251"));
    h.session.stop();
    h.session.setAudioOnlyMode(false);
}

// Hints contract (ARCHITECTURE.md §8): an `audioOnly` hint starts listen-only before the first
// resolve: one resolve with purpose AudioOnly, audio only; a `position` hint is never a seek.
static void sel08_audioOnlyHintBeforeFirstResolve() {
    gServer->reset();
    Harness h;
    QVariantMap hints;
    hints.insert(QStringLiteral("audioOnly"), true);
    hints.insert(QStringLiteral("position"), 30000.0);  // ms, resume is the library bridge's job
    h.session.open(QStringLiteral("FIXTURE0001"), hints);
    CHECK(waitFor([&]() { return h.ready() && h.session.playing(); }, 15000));
    CHECK(h.resolver.resolves() == 1);
    CHECK(h.resolver.resolves() == 1 && h.resolver.requests[0].purpose == nordtile::playback::StreamPurpose::AudioOnly);
    CHECK(h.session.player()->plan().mode == player::PlayMode::AudioOnly);
    CHECK(gServer->requests("v.webm") == 0 && gServer->requests("av.webm") == 0);
    CHECK(h.session.position() < 20000);  // the position hint did not seek
    h.session.stop();
    h.session.setAudioOnlyMode(false);
    // A wrongly typed hint means "not known": the Settings default (video) stays.
    gServer->reset();
    Harness v;
    QVariantMap bad;
    bad.insert(QStringLiteral("audioOnly"), QStringLiteral("yes"));
    v.session.open(QStringLiteral("FIXTURE0001"), bad);
    CHECK(waitFor([&]() { return v.ready() && v.session.playing(); }, 15000));
    CHECK(v.session.player()->plan().mode != player::PlayMode::AudioOnly);
    CHECK(v.resolver.requests[0].purpose == nordtile::playback::StreamPurpose::Video);
    v.session.stop();
}

// RES-09: a final class fails at once with its reason (no retry); a retryable class gets one fresh
// re-resolve (invalidate + resolve, RES-10), then fails.
static void res09_finalErrorNoRetry() {
    Harness f;
    f.resolver.errorClass = StreamErrorClass::Removed;
    f.session.open(QStringLiteral("FIXTURE0001"));
    CHECK(waitFor([&]() { return f.session.state() == PlaybackSession::Failed; }, 5000));

    for (int i = 0; i < 10; ++i) { QCoreApplication::processEvents(); QThread::msleep(5); }
    CHECK(f.resolver.resolves() == 1 && f.resolver.invalidated.isEmpty());
    CHECK(f.session.errorText() == streamErrorText(StreamErrorClass::Removed) && !f.session.errorText().isEmpty());
    CHECK(f.session.retryAvailable());
    Harness n;
    n.resolver.errorClass = StreamErrorClass::Network;
    n.session.open(QStringLiteral("FIXTURE0001"));
    CHECK(waitFor([&]() { return n.session.state() == PlaybackSession::Failed; }, 5000));
    CHECK(n.resolver.resolves() == 2);
    CHECK(n.resolver.invalidated == QStringList() << QStringLiteral("FIXTURE0001"));
    CHECK(!n.session.errorText().isEmpty());
}

// RES-09 (causes 1, 4, 6): a bare (non-final) Unavailable gets the one fresh re-resolve; a final one
// does not; rate limiting and a changed delivery are retryable with their own texts, which never
// mention a device limit or a reinstall; the settings key stays empty for them.
static void res09_retryableClassesAndTexts() {
    using C = StreamErrorClass;
    StreamError bare = nordtile::playback::streamError(C::Unavailable, false);
    bare.final = false;
    CHECK(resolveErrorKind(bare) == ResolveAnswer::Retryable);
    CHECK(resolveErrorKind(nordtile::playback::streamError(C::Unavailable, false)) == ResolveAnswer::Final);
    CHECK(resolveErrorKind(nordtile::playback::streamError(C::TryLater, false)) == ResolveAnswer::Retryable);
    CHECK(resolveErrorKind(nordtile::playback::streamError(C::DeliveryChanged, false)) == ResolveAnswer::Retryable);
    CHECK(resolveErrorKind(nordtile::playback::streamError(C::Failed, false)) == ResolveAnswer::Retryable);
    // The runtime's temporary-file budget: no automatic re-resolve (the same budget is met again).
    StreamError budget = nordtile::playback::streamError(C::Failed, false);
    budget.label = QStringLiteral("storage_limit");
    CHECK(resolveErrorKind(budget) == ResolveAnswer::Final);
    CHECK(resolveErrorKind(nordtile::playback::streamError(C::Removed, false)) == ResolveAnswer::Final);
    CHECK(resolveErrorKind(nordtile::playback::streamError(C::GeoBlocked, false)) == ResolveAnswer::Final);
    CHECK(streamErrorText(C::TryLater) == QLatin1String("YouTube is limiting requests right now. Try again in a minute."));
    CHECK(streamErrorText(C::DeliveryChanged).startsWith(QLatin1String("YouTube changed how this video is delivered.")));
    CHECK(streamErrorText(C::Failed).contains(QLatin1String("resolver may need an update")));
    for (C k : {C::TryLater, C::DeliveryChanged, C::Failed, C::Unavailable}) {
        CHECK(!streamErrorText(k).contains(QLatin1String("Reinstall")));
        CHECK(streamErrorText(k) != streamErrorText(C::NoPlayableStream));
    }

    Harness u;
    u.resolver.errorClass = C::Unavailable;
    u.resolver.errorNotFinal = true;
    u.session.open(QStringLiteral("FIXTURE0001"));
    CHECK(waitFor([&]() { return u.session.state() == PlaybackSession::Failed; }, 5000));
    CHECK(u.resolver.resolves() == 2 && u.resolver.invalidated == QStringList() << QStringLiteral("FIXTURE0001"));
    CHECK(u.session.errorText() == streamErrorText(C::Unavailable) && u.session.errorSettingKey().isEmpty());

    Harness d;
    d.resolver.errorClass = C::DeliveryChanged;
    d.resolver.delayMs = 150;
    d.session.open(QStringLiteral("FIXTURE0001"));
    CHECK(waitFor([&]() { return d.resolver.resolves() == 2; }, 5000));
    CHECK(d.session.statusText().startsWith(QLatin1String("YouTube changed how this video is delivered. Retrying")));
    CHECK(waitFor([&]() { return d.session.state() == PlaybackSession::Failed; }, 5000));
    CHECK(d.session.errorText() == streamErrorText(C::DeliveryChanged) && d.session.errorSettingKey().isEmpty());

    // Rate limited: the one re-resolve waits (PlaybackController::kRetryLaterMs), with its status.
    Harness t;
    t.resolver.errorClass = C::TryLater;
    t.session.open(QStringLiteral("FIXTURE0001"));
    CHECK(waitFor([&]() { return t.resolver.answered() == 1; }, 5000));
    for (int i = 0; i < 20; ++i) { QCoreApplication::processEvents(); QThread::msleep(5); }
    CHECK(t.resolver.resolves() == 1 && t.session.state() == PlaybackSession::Loading);
    CHECK(t.session.statusText() == QString::fromUtf8("YouTube is limiting requests. Trying again shortly…"));
    CHECK(waitFor([&]() { return t.session.state() == PlaybackSession::Failed; }, PlaybackController::kRetryLaterMs + 5000));
    CHECK(t.resolver.resolves() == 2 && t.session.errorText() == streamErrorText(C::TryLater));
}

// RES-21 hook (main.cpp): the warm-up runs once, queued after the first emission, never inside it.
static void res21_warmUpOnceAfterFirstSwap() {
    QObject window;  // objectNameChanged stands in for QQuickWindow::frameSwapped
    QObject context;
    int warmUps = 0;
    runOnceAfterFirst(&window, &QObject::objectNameChanged, &context, [&warmUps]() { ++warmUps; });
    for (int i = 0; i < 5; ++i) QCoreApplication::processEvents();
    CHECK(warmUps == 0);                       // not before the first frame
    window.setObjectName(QStringLiteral("a"));
    CHECK(warmUps == 0);                       // not inside the emission
    window.setObjectName(QStringLiteral("b"));
    for (int i = 0; i < 5; ++i) QCoreApplication::processEvents();
    CHECK(warmUps == 1);
    window.setObjectName(QStringLiteral("c"));
    for (int i = 0; i < 5; ++i) QCoreApplication::processEvents();
    CHECK(warmUps == 1);
}


// ---- E12 speculative resolve (RES-11 MAY), row prefetch (RES-11, RES-21) ---------------------

static int diag(const Harness &h, const char *key) { return h.session.diagnostics().value(QLatin1String(key)).toInt(); }
static void spinFor(int ms) {
    QElapsedTimer t;
    t.start();
    while (t.elapsed() < ms) { QCoreApplication::processEvents(QEventLoop::AllEvents, 5); QThread::msleep(1); }
}
static QVariantMap rowHints() {
    QVariantMap hints;
    hints.insert(QStringLiteral("title"), QStringLiteral("Fixture"));
    hints.insert(QStringLiteral("channel"), QStringLiteral("Channel"));
    return hints;
}

// touchDown -> opened -> open(): the open reuses the speculation (in flight, then already answered):
// one resolve in all, speculative = true and otherwise the open's request, resolve-done
// path=prefetch, markers speculative-start < tap < speculative-used < resolve-done < first-byte.
static void e12_pressThenOpenReusesSpeculation() {
    for (int answeredFirst = 0; answeredFirst < 2; ++answeredFirst) {
        gServer->reset();
        gMarkers.clear();
        Harness h;
        h.resolver.delayMs = answeredFirst ? 5 : 150;
        const QVariantMap hints = rowHints();
        h.session.pressStarted(QStringLiteral("FIXTURE0001"), hints, 7);
        CHECK(h.resolver.resolves() == 1 && h.resolver.requests[0].speculative);
        CHECK(h.resolver.requests[0].videoId == QLatin1String("FIXTURE0001") && !h.resolver.requests[0].signedIn);
        if (answeredFirst) CHECK(waitFor([&]() { return h.resolver.answered() == 1; }, 2000));
        spinFor(20);
        h.session.openingFromPress(QStringLiteral("FIXTURE0001"), 7);
        h.session.open(QStringLiteral("FIXTURE0001"), hints);
        CHECK(h.resolver.resolves() == 1);
        CHECK(waitFor([&]() { return h.ready() && h.session.playing(); }, 15000));
        CHECK(h.resolver.resolves() == 1 && h.resolver.cancelled.empty());
        // The speculative request is the one the open would have made (RES-10 key, speculative aside).
        StreamRequest asOpen = h.resolver.requests[0];
        asOpen.speculative = false;
        CHECK(asOpen.maxHeight == 720 && asOpen.videoCodecs.value(0) == QLatin1String("avc1"));
        CHECK(asOpen.network == nordtile::playback::NetworkClass::Unmetered);
        const int ss = markerIndex(QStringLiteral("speculative-start "));
        const int tap = markerIndex(QStringLiteral("tap "), ss);
        const int su = markerIndex(QStringLiteral("speculative-used "), tap);
        const int rd = markerIndex(QStringLiteral("resolve-done "), su);
        const int fb = markerIndex(QStringLiteral("first-byte "), rd);
        CHECK(ss >= 0 && tap > ss && su > tap && rd > su && fb > rd);
        CHECK(markerHas(ss, "run=7") && markerHas(su, "run=7") && markerHas(rd, "path=prefetch") && markerHas(rd, "run=1"));
        CHECK(markerIndex(QStringLiteral("speculative-cancel ")) < 0);
        CHECK(markerIndex(QStringLiteral("probe-done ")) < 0);  // the probe ran before the tap
        CHECK(h.session.diagnostics().value(QStringLiteral("resolvePath")).toString() == QLatin1String("prefetch"));
        CHECK(diag(h, "speculativeStarted") == 1 && diag(h, "speculativeUsed") == 1 && diag(h, "speculativeWasted") == 0);
        h.session.stop();
        CHECK(h.session.diagnostics().value(QStringLiteral("streamJobs")).toInt() == 0);
    }
}

// Latest press wins: a second press cancels the first one's resolve; the open of the second
// video reuses its answer; one wasted, one used.
static void e12_latestPressWins() {
    gServer->reset();
    gMarkers.clear();
    Harness h;
    h.resolver.delayMs = 200;
    h.session.pressStarted(QStringLiteral("FIXTURE0001"), QVariantMap(), 1);
    h.session.pressStarted(QStringLiteral("FIXTURE0002"), QVariantMap(), 2);
    CHECK(h.resolver.resolves() == 2);
    CHECK(h.resolver.cancelled.size() == 1 && h.resolver.cancelled[0] == 1);
    CHECK(markerHas(markerIndex(QStringLiteral("speculative-cancel ")), "run=1"));
    h.session.openingFromPress(QStringLiteral("FIXTURE0002"), 2);
    h.session.open(QStringLiteral("FIXTURE0002"));
    CHECK(h.resolver.resolves() == 2);
    CHECK(waitFor([&]() { return h.ready() && h.session.playing(); }, 15000));
    CHECK(h.resolver.resolves() == 2 && h.session.videoId() == QLatin1String("FIXTURE0002"));
    CHECK(markerHas(markerIndex(QStringLiteral("speculative-used ")), "run=2"));
    CHECK(diag(h, "speculativeStarted") == 2 && diag(h, "speculativeUsed") == 1 && diag(h, "speculativeWasted") == 1);
    // A press during the first 2 s of a start speculates nothing.
    h.session.stop();
    h.session.open(QStringLiteral("FIXTURE0001"));
    const int resolves = h.resolver.resolves();
    h.session.pressStarted(QStringLiteral("FIXTURE0002"), QVariantMap(), 3);
    CHECK(h.resolver.resolves() == resolves && diag(h, "speculativeStarted") == 2);
    h.session.stop();
}

// touchCancelled before the answer cancels the resolve; after the answer nothing is cancelled or
// invalidated (the answer stays cached for its TTL). A later plain open resolves normally.
static void e12_pressCancelled() {
    gMarkers.clear();
    Harness h;
    h.resolver.delayMs = 300;
    h.session.pressStarted(QStringLiteral("FIXTURE0001"), QVariantMap(), 4);
    h.session.pressCancelled(QStringLiteral("FIXTURE0001"), 4);
    CHECK(h.resolver.cancelled.size() == 1 && h.resolver.cancelled[0] == 1);
    CHECK(markerHas(markerIndex(QStringLiteral("speculative-cancel ")), "run=4"));
    CHECK(diag(h, "speculativeWasted") == 1 && !h.session.diagnostics().value(QStringLiteral("speculationInFlight")).toBool());
    h.session.open(QStringLiteral("FIXTURE0001"));
    CHECK(h.resolver.resolves() == 2 && !h.resolver.requests[1].speculative);
    h.session.stop();

    Harness k;
    k.resolver.delayMs = 5;
    k.session.pressStarted(QStringLiteral("FIXTURE0001"), QVariantMap(), 5);
    CHECK(waitFor([&]() { return k.resolver.answered() == 1; }, 2000));
    spinFor(20);
    k.session.pressCancelled(QStringLiteral("FIXTURE0001"), 4);  // another press: ignored
    CHECK(diag(k, "speculativeWasted") == 0);
    k.session.pressCancelled(QStringLiteral("FIXTURE0001"), 5);
    spinFor(20);
    CHECK(k.resolver.cancelled.empty() && k.resolver.invalidated.isEmpty());
    CHECK(diag(k, "speculativeWasted") == 1);
    k.session.openingFromPress(QStringLiteral("FIXTURE0001"), 5);  // forgotten: no reuse
    k.session.open(QStringLiteral("FIXTURE0001"));
    CHECK(k.resolver.resolves() == 2);
    k.session.stop();
}

// RES-11 / RES-21 rows: on mobile data nothing at all; unmetered: one prefetch of the first row
// (speculative, the open's request), one keep-alive; repeats and calls while a play starts do nothing.
static void res11_rowsVisible() {
    Harness h;
    h.networkClass = 1;
    const QStringList rows = QStringList() << QStringLiteral("FIXTURE0003") << QStringLiteral("FIXTURE0004");
    h.session.rowsVisible(rows);
    CHECK(h.resolver.resolves() == 0 && h.resolver.prefetches.empty() && h.warmUps == 0);
    h.networkClass = 0;
    h.session.rowsVisible(rows);
    CHECK(h.resolver.prefetches.size() == 1 && h.warmUps == 1 && h.resolver.resolves() == 0);
    CHECK(h.resolver.prefetches[0].videoId == QLatin1String("FIXTURE0003") && h.resolver.prefetches[0].speculative);
    CHECK(h.resolver.prefetches[0].network == nordtile::playback::NetworkClass::Unmetered && !h.resolver.prefetches[0].signedIn);
    h.session.rowsVisible(rows);  // in flight: nothing new, keep-alive not yet due
    CHECK(h.resolver.prefetches.size() == 1 && h.warmUps == 1);
    CHECK(diag(h, "rowPrefetches") == 1 && diag(h, "keepAliveWarmUps") == 1);
    h.session.open(QStringLiteral("FIXTURE0001"));  // a play starts
    h.session.rowsVisible(QStringList() << QStringLiteral("FIXTURE0004"));
    CHECK(h.resolver.prefetches.size() == 1);
    h.session.stop();
}

// Never in the background: no press speculation, no row prefetch; a speculation running when the
// app goes to the background is cancelled.
static void e12_noSpeculationInBackground() {
    Harness h;
    h.resolver.delayMs = 300;
    h.session.setAppActive(false);
    h.session.pressStarted(QStringLiteral("FIXTURE0001"), QVariantMap(), 1);
    h.session.rowsVisible(QStringList() << QStringLiteral("FIXTURE0003"));
    CHECK(h.resolver.resolves() == 0 && h.resolver.prefetches.empty() && h.warmUps == 0);
    h.session.setAppActive(true);
    h.session.pressStarted(QStringLiteral("FIXTURE0001"), QVariantMap(), 2);
    CHECK(h.resolver.resolves() == 1);
    h.session.setAppActive(false);
    CHECK(h.resolver.cancelled.size() == 1 && diag(h, "speculativeWasted") == 1);
    h.session.setAppActive(true);
}

// RES-07: the HTTP status from the player decides; the text is the fallback.
static void res07_refusalUsesHttpStatus() {
    CHECK(refusalAtByteZero(QStringLiteral("Forbidden"), true, false, 403));
    CHECK(refusalAtByteZero(QStringLiteral("Could not read"), true, false, 410));   // no phrase, status known
    CHECK(!refusalAtByteZero(QStringLiteral("Forbidden"), true, false, 200));      // the status wins over the text
    CHECK(!refusalAtByteZero(QStringLiteral("Forbidden"), true, true, 403));       // after the first byte
    CHECK(!refusalAtByteZero(QStringLiteral("Forbidden"), false, false, 403));     // not a network branch
    CHECK(refusalAtByteZero(QStringLiteral("Forbidden"), true, false, 0));         // fallback: phrase
    CHECK(refusalAtByteZero(QStringLiteral("stream error (403)"), true, false));   // fallback: "(4xx)"
    CHECK(!refusalAtByteZero(QStringLiteral("Could not connect"), true, false, 0));
}

// RES-19: an Upcoming answer with a start shows it and holds Retry, speculation and row prefetch.
static void res19_upcomingShowsStartAndHolds() {
    Harness h;
    h.resolver.errorClass = StreamErrorClass::Upcoming;
    const qint64 start = QDateTime::currentMSecsSinceEpoch() + 3600000;
    h.resolver.scheduledStartUnixMs = start;
    h.session.open(QStringLiteral("FIXTURE0001"));
    CHECK(waitFor([&]() { return h.session.state() == PlaybackSession::Failed; }, 5000));
    CHECK(h.session.upcoming() && h.session.scheduledStart() == static_cast<double>(start));
    CHECK(h.session.errorText().startsWith(streamErrorText(StreamErrorClass::Upcoming)));
    CHECK(h.session.errorText().contains(QLatin1String("Scheduled to start")));
    CHECK(!h.session.retryAvailable());
    h.session.retry();
    spinFor(kSpeculationQuietMs + 100);  // past E12's quiet window, so only RES-19 holds them
    h.session.pressStarted(QStringLiteral("FIXTURE0001"), QVariantMap(), 1);
    h.session.rowsVisible(QStringList() << QStringLiteral("FIXTURE0001"));
    for (int i = 0; i < 10; ++i) { QCoreApplication::processEvents(); QThread::msleep(5); }
    CHECK(h.resolver.resolves() == 1 && h.resolver.prefetches.empty() && h.resolver.invalidated.isEmpty());
    h.session.stop();
}

// PLY-20: the session gives the hardware-decode setting to the probe gate and feeds its verdict into
// the selection (the next resolve asks for the hardware codec order) and playback.diagnostics.
class InstantProbeRunner final : public player::HardwareProbeRunner {
public:
    int starts = 0;
    bool start(const std::string &, const Listener &l, std::string *) override {
        ++starts;
        std::weak_ptr<int> alive = token_ = std::make_shared<int>(0);
        QTimer::singleShot(10, [alive, l]() { if (alive.lock()) l.frame(); });
        return true;
    }
    void stop() override { token_.reset(); }
private:
    std::shared_ptr<int> token_;
};

static void ply20_sessionFeedsProbeVerdict() {
    Harness h;
    h.settings.setHardwareDecoding(false);
    InstantProbeRunner runner;
    player::HardwareDecodeGate::Options o;
    o.buildKey = "test|os|droid|app";
    o.droidvdecPresent = o.droideglsinkPresent = true;
    o.clipUri = QStringLiteral("file:///probe.mp4");
    player::HardwareDecodeGate gate(&runner, o);
    h.session.setHardwareDecodeGate(&gate);
    QVariantMap d = h.session.diagnostics();
    CHECK(d.value(QStringLiteral("hardwareDecodeProbe")).toString() == QLatin1String("not run (hardware decode off)"));
    CHECK(!d.value(QStringLiteral("hardwareDecodeVerified")).toBool());
    h.settings.setHardwareDecoding(true);  // the first enable probes
    CHECK(waitFor([&]() { return gate.verified(); }, 3000) && runner.starts == 1);
    d = h.session.diagnostics();
    CHECK(d.value(QStringLiteral("hardwareDecodeVerified")).toBool());
    CHECK(d.value(QStringLiteral("hardwareDecodeProbe")).toString().startsWith(QLatin1String("passed")));
    h.session.open(QStringLiteral("FIXTURE0001"));
    CHECK(waitFor([&]() { return h.resolver.resolves() >= 1; }, 3000));
    CHECK(!h.resolver.requests.empty() && h.resolver.requests.front().hardwareDecode);
    CHECK(!h.resolver.requests.empty() && h.resolver.requests.front().videoCodecs.value(0) == QLatin1String("vp09"));
    h.session.stop();
    h.settings.setHardwareDecoding(false);
    h.session.setHardwareDecodeGate(nullptr);
}

// ---- Batch 3: downloads, PO token, error setting, queue, start ordering ----------------------

static QVariantMap downloadRecord(const QString &videoId) {
    QVariantMap m;
    m.insert(QStringLiteral("available"), true);
    m.insert(QStringLiteral("videoId"), videoId);
    m.insert(QStringLiteral("kind"), QStringLiteral("video"));
    m.insert(QStringLiteral("layout"), QStringLiteral("single"));
    m.insert(QStringLiteral("filePath"), gDir + QStringLiteral("/av.webm"));
    m.insert(QStringLiteral("videoPath"), gDir + QStringLiteral("/av.webm"));
    m.insert(QStringLiteral("audioPath"), QString());
    m.insert(QStringLiteral("thumbnailPath"), QString());
    m.insert(QStringLiteral("title"), QStringLiteral("Downloaded fixture"));
    m.insert(QStringLiteral("channel"), QStringLiteral("Channel"));
    m.insert(QStringLiteral("durationSeconds"), 4);
    m.insert(QStringLiteral("error"), QString());
    QVariantMap caption;
    caption.insert(QStringLiteral("languageCode"), QStringLiteral("en"));
    caption.insert(QStringLiteral("name"), QStringLiteral("English"));
    caption.insert(QStringLiteral("autoGenerated"), false);
    caption.insert(QStringLiteral("path"), gDir + QStringLiteral("/en.json3"));
    m.insert(QStringLiteral("captions"), QVariantList() << caption);
    QVariantMap sponsor;
    sponsor.insert(QStringLiteral("category"), QStringLiteral("sponsor"));
    sponsor.insert(QStringLiteral("actionType"), QStringLiteral("skip"));
    sponsor.insert(QStringLiteral("start"), 0.3);
    sponsor.insert(QStringLiteral("end"), 2.5);
    m.insert(QStringLiteral("segments"), QVariantList() << sponsor);
    m.insert(QStringLiteral("sidecarsPending"), false);
    return m;
}

// SB-02 / DL-07: segment maps -> skipper segments (seconds for records, ms for the model).
static void sb02_segmentsFromRecord() {
    QVariantList list;
    QVariantMap a;
    a.insert(QStringLiteral("category"), QStringLiteral("sponsor"));
    a.insert(QStringLiteral("actionType"), QStringLiteral("skip"));
    a.insert(QStringLiteral("start"), 1.5);
    a.insert(QStringLiteral("end"), 4.0);
    QVariantMap mute = a;
    mute.insert(QStringLiteral("actionType"), QStringLiteral("mute"));
    QVariantMap poi;
    poi.insert(QStringLiteral("category"), QStringLiteral("poi_highlight"));
    poi.insert(QStringLiteral("actionType"), QStringLiteral("poi"));
    poi.insert(QStringLiteral("start"), 10.0);
    poi.insert(QStringLiteral("end"), 10.0);
    QVariantMap unknown = a;
    unknown.insert(QStringLiteral("category"), QStringLiteral("nonsense"));
    QVariantMap backwards = a;
    backwards.insert(QStringLiteral("end"), 1.0);
    list << a << mute << poi << unknown << backwards;
    const std::vector<player::SponsorSegment> seconds = sponsorSegmentsFor(list, 1000.0);
    CHECK(seconds.size() == 2);
    CHECK(seconds.size() == 2 && seconds[0].startMs == 1500 && seconds[0].endMs == 4000);
    CHECK(seconds.size() == 2 && seconds[1].category == player::SponsorCategory::PoiHighlight &&
          seconds[1].startMs == 10000 && seconds[1].endMs == 10000);
    QVariantMap ms;  // SponsorSegmentsModel shape: ms, no actionType
    ms.insert(QStringLiteral("category"), QStringLiteral("selfpromo"));
    ms.insert(QStringLiteral("start"), 2000);
    ms.insert(QStringLiteral("end"), 3000);
    const std::vector<player::SponsorSegment> fromModel = sponsorSegmentsFor(QVariantList() << ms, 1.0);
    CHECK(fromModel.size() == 1 && fromModel[0].startMs == 2000 && fromModel[0].endMs == 3000);
}

// PLY-26 / DL-07 / SB-02 / LNK-02: a downloaded id opens the local file (no resolve, no fetcher,
// no speculation on the press); its saved segments drive the skipper (a later empty SponsorBlock
// answer does not replace them); captions are exposed; startSeconds is carried.
static void ply26_downloadedVideoPlaysLocally() {
    gServer->reset();
    Harness h;
    int lookups = 0;
    h.session.setLocalMediaLookup([&lookups](const QString &id) {
        ++lookups;
        return id == QLatin1String("FIXTURE0001") ? downloadRecord(id) : QVariantMap();
    });
    h.session.pressStarted(QStringLiteral("FIXTURE0001"), QVariantMap(), 1);
    CHECK(h.resolver.resolves() == 0 && diag(h, "speculativeStarted") == 0);
    h.session.open(QStringLiteral("FIXTURE0001"), rowHints());
    CHECK(lookups >= 1);
    CHECK(h.resolver.resolves() == 0 && h.resolver.prefetches.empty());
    CHECK(h.session.controller().local() && h.session.videoId() == QLatin1String("FIXTURE0001"));
    CHECK(waitFor([&]() { return h.ready() && h.session.playing(); }, 15000));
    const player::PipelinePlan &plan = h.session.player()->plan();
    CHECK(plan.mode == player::PlayMode::Local);
    CHECK(!plan.branches.empty() && plan.branches[0].source.property("uri").compare(0, 7, "file://") == 0);
    CHECK(h.fetcher.opened.isEmpty() && gServer->requests("av.webm") == 0);   // no network at all
    CHECK(h.session.localCaptions().size() == 1);
    CHECK(h.session.localCaptions().value(0).toMap().value(QStringLiteral("languageCode")).toString() == QLatin1String("en"));
    CHECK(h.session.title() == QLatin1String("Downloaded fixture"));
    h.session.setSponsorSegments(QVariantList());  // the offline SponsorBlock answer
    CHECK(h.session.diagnostics().value(QStringLiteral("localSegments")).toBool());
    CHECK(waitFor([&]() { return h.session.sponsorNotice() == QLatin1String("sponsor"); }, 6000));  // SB-02 auto-skip
    CHECK(h.resolver.resolves() == 0);
    h.session.stop();
    CHECK(h.session.localCaptions().size() == 1 || !h.session.controller().local());

    // LNK-02: a t= link start on a downloaded video.
    Harness k;
    k.session.setLocalMediaLookup([](const QString &id) { return downloadRecord(id); });
    QVariantMap hints;
    hints.insert(QStringLiteral("startSeconds"), 2);
    k.session.open(QStringLiteral("FIXTURE0001"), hints);
    CHECK(k.resolver.resolves() == 0);
    CHECK(waitFor([&]() { return k.ready(); }, 15000));
    CHECK(k.session.position() >= 1500);
    k.session.setStartPosition(0);  // the library bridge's resume does not override the link
    CHECK(k.session.position() >= 1500);
    k.session.stop();
}

// PLY-26: next() on a queue whose next item is downloaded plays it locally (no resolve), like open().
static void ply26_queueNextPlaysDownloadLocally() {
    gServer->reset();
    Harness h;
    h.session.setLocalMediaLookup([](const QString &id) {
        return id == QLatin1String("FIXTURE0002") ? downloadRecord(id) : QVariantMap();
    });
    h.session.setQueue(QStringList() << QStringLiteral("FIXTURE0001") << QStringLiteral("FIXTURE0002"), 0);
    h.session.open(QStringLiteral("FIXTURE0001"), rowHints());
    CHECK(waitFor([&]() { return h.ready(); }, 15000));
    const int resolvesBefore = h.resolver.resolves();
    CHECK(resolvesBefore == 1);
    h.session.next();
    CHECK(h.session.videoId() == QLatin1String("FIXTURE0002"));
    CHECK(h.session.controller().local());
    CHECK(h.resolver.resolves() == resolvesBefore);   // no resolve for the downloaded item
    CHECK(waitFor([&]() { return h.ready(); }, 15000));
    CHECK(h.session.player()->plan().mode == player::PlayMode::Local);
    h.session.stop();
}

// LIB-05 / PLY-06: the library bridge's resume point arrives right after a local open (the load
// starts in the same call); it still lands before the first frame, with no frame from 0.
static void ply26_localResumeAfterOpen() {
    gServer->reset();
    Harness h;
    h.session.setLocalMediaLookup([](const QString &id) { return downloadRecord(id); });
    h.session.open(QStringLiteral("FIXTURE0001"), rowHints());
    h.session.setStartPosition(2000);
    CHECK(waitFor([&]() { return h.ready(); }, 15000));
    CHECK(h.resolver.resolves() == 0);
    CHECK(h.session.position() >= 1500);
    h.session.stop();
}

// PLY-26 / NFR-PERF-02 (run 5: a downloaded flat mp4 played through filesrc advanced its position
// but no first-frame was logged and the surface kept the thumbnail): a local muxed WebM and a flat
// mp4 opened from the device, from 0, with a link start (startSeconds) and with the library
// bridge's setStartPosition() right after the open: the player's first frame reaches the mailbox,
// frames keep coming, the surface's first frame swap prints first-frame for the run, and the
// start-up split (demux-ready branch=muxed) is printed.
static void ply26_localMuxedFirstFrameMarker() {
    const char *const files[] = {"av.webm", "av.mp4"};
    const char *const starts[] = {"from 0", "link start 2 s", "setStartPosition(2 s) after open"};
    for (size_t f = 0; f < sizeof(files) / sizeof(files[0]); ++f) {
        for (int st = 0; st < 3; ++st) {
            gMarkers.clear();
            Harness h;
            const QString file = gDir + QStringLiteral("/") + QLatin1String(files[f]);
            h.session.setLocalMediaLookup([file](const QString &id) {
                QVariantMap r = downloadRecord(id);
                r.insert(QStringLiteral("filePath"), file);
                r.insert(QStringLiteral("videoPath"), file);
                return r;
            });
            int firstFrames = 0;
            QObject::connect(h.session.player(), &player::VideoPlayer::firstFrame, [&firstFrames]() { ++firstFrames; });
            QAtomicInt posts;
            player::VideoFrameMailbox *mailbox = h.session.player()->frameMailbox();
            const int listener = mailbox->addListener([&posts]() { posts.ref(); });
            QVariantMap hints = rowHints();
            if (st == 1) hints.insert(QStringLiteral("startSeconds"), 2);
            h.session.open(QStringLiteral("FIXTURE0001"), hints);
            if (st == 2) h.session.setStartPosition(2000);
            const bool ready = waitFor([&]() { return h.ready() && h.session.playing(); }, 15000);
            const bool frame = waitFor([&]() { return firstFrames > 0 && mailbox->latest().valid(); }, 5000);
            if (!ready || !frame)
                std::printf("FAIL %s, %s: ready %d, player first frame %d, mailbox frame %d\n", files[f], starts[st],
                            int(ready), firstFrames, int(mailbox->latest().valid()));
            CHECK(ready && frame);
            CHECK(h.session.player()->plan().mode == player::PlayMode::Local);
            // The surface (VideoSurfaceItem) got the frame: hasFrame -> surfaceFrameReady, then the swap.
            h.session.surfaceFrameReady();
            h.session.frameSwapped();
            const int ff = markerIndex(QStringLiteral("first-frame "));
            CHECK(ff >= 0);
            const int dr = markerIndex(QStringLiteral("demux-ready "));
            CHECK(markerHas(dr, "branch=muxed"));
            const int posts0 = posts.load();
            waitFor([&]() { return posts.load() - posts0 >= 10; }, 2000);
            const int moved = posts.load() - posts0;
            CHECK(moved >= 10);
            if (st > 0) CHECK(h.session.position() >= 1500);
            mailbox->removeListener(listener);
            QString split;
            for (const QString &m : gMarkers)
                if (m.startsWith(QLatin1String("demux-ready ")) || m.startsWith(QLatin1String("start-seek ")) ||
                    m.startsWith(QLatin1String("first-decoded ")) || m.startsWith(QLatin1String("preroll-done ")) ||
                    m.startsWith(QLatin1String("first-frame ")))
                    split += QStringLiteral(" [") + m + QStringLiteral("]");
            std::printf("  %s, %s: first frame %d, %d frames after it, position %.0f;%s\n", files[f], starts[st], firstFrames,
                        moved, h.session.position(), qPrintable(split));
            h.session.stop();
        }
    }
}

// preferStream: true streams a downloaded video; an unavailable record (file_missing) streams;
// a stream open clears the previous download's captions.
static void ply26_preferStreamAndMissingFileStream() {
    Harness h;
    h.session.setLocalMediaLookup([](const QString &id) { return downloadRecord(id); });
    QVariantMap hints;
    hints.insert(QStringLiteral("preferStream"), true);
    h.session.open(QStringLiteral("FIXTURE0001"), hints);
    CHECK(h.resolver.resolves() == 1 && !h.session.controller().local());
    CHECK(waitFor([&]() { return h.ready(); }, 15000));
    CHECK(h.session.player()->plan().mode == player::PlayMode::Dual);
    CHECK(h.session.localCaptions().isEmpty());
    h.session.stop();
    Harness m;
    m.session.setLocalMediaLookup([](const QString &id) {
        QVariantMap r;
        r.insert(QStringLiteral("available"), false);
        r.insert(QStringLiteral("videoId"), id);
        r.insert(QStringLiteral("error"), QStringLiteral("file_missing"));
        return r;
    });
    m.session.open(QStringLiteral("FIXTURE0001"));
    CHECK(m.resolver.resolves() == 1 && !m.session.controller().local());
    CHECK(waitFor([&]() { return m.ready(); }, 15000));
    m.session.stop();
}

// RES-14: the request carries poProvider only while the helper reports ready.
static void res14_poProviderOnlyWhenReady() {
    Harness h;
    h.session.open(QStringLiteral("FIXTURE0001"));
    CHECK(h.resolver.resolves() == 1 && !h.resolver.requests[0].poProvider);  // default: never
    h.session.stop();
    bool ready = false;
    h.session.setPoProviderReady([&ready]() { return ready; });
    h.session.open(QStringLiteral("FIXTURE0002"));
    CHECK(h.resolver.resolves() == 2 && !h.resolver.requests[1].poProvider);
    h.session.stop();
    ready = true;
    h.session.open(QStringLiteral("FIXTURE0003"));
    CHECK(h.resolver.resolves() == 3 && h.resolver.requests[2].poProvider);
    h.session.stop();
    // RES-10: the speculative request (and so its cache key) carries it the same way.
    spinFor(kSpeculationQuietMs + 100);
    h.session.pressStarted(QStringLiteral("FIXTURE0004"), QVariantMap(), 9);
    CHECK(h.resolver.resolves() == 4 && h.resolver.requests[3].poProvider && h.resolver.requests[3].speculative);
    h.session.pressCancelled(QStringLiteral("FIXTURE0004"), 9);
}

// PLY-18 / ux: errorSettingKey names the setting that would fix the error, "" for network ones.
static void ply18_errorSettingKey() {
    Harness h;
    int states = 0;
    QObject::connect(&h.session, &PlaybackSession::stateChanged, [&states]() { ++states; });
    h.resolver.errorClass = StreamErrorClass::NoPlayableStream;
    h.session.open(QStringLiteral("FIXTURE0001"));
    CHECK(waitFor([&]() { return h.session.state() == PlaybackSession::Failed; }, 5000));
    CHECK(h.session.errorSettingKey() == QLatin1String("playback/maxHeight"));  // 720p ceiling asked
    CHECK(states > 0);
    Harness n;
    n.resolver.errorClass = StreamErrorClass::Network;
    n.session.open(QStringLiteral("FIXTURE0001"));
    CHECK(waitFor([&]() { return n.session.state() == PlaybackSession::Failed; }, 5000));
    CHECK(n.session.errorSettingKey().isEmpty());
    Harness ok;
    CHECK(ok.session.errorSettingKey().isEmpty());
}

// PLY-25 / MPR-01: queueLength / queueIndex with queueChanged.
static void ply25_queueReadOut() {
    Harness h;
    int spy = 0;
    QObject::connect(&h.session, &PlaybackSession::queueChanged, [&spy]() { ++spy; });
    CHECK(h.session.queueLength() == -1 && h.session.queueIndex() == -1);
    h.session.setQueue(QStringList() << QStringLiteral("FIXTURE0001") << QStringLiteral("FIXTURE0002"), 1);
    CHECK(spy == 1 && h.session.queueLength() == 2 && h.session.queueIndex() == 1);
    h.session.setQueue(QStringList() << QStringLiteral("FIXTURE0001") << QStringLiteral("FIXTURE0002"), 1);
    CHECK(spy == 1);  // unchanged: no signal
    h.session.clearQueue();
    CHECK(spy == 2 && h.session.queueLength() == -1 && h.session.queueIndex() == -1);
}

// NFR-PERF-02 tap -> resolve-start: the next video's resolve is dispatched while the previous
// player still plays; the player is stopped right after (before open() returns).
static void perf02_resolveDispatchedBeforeStop() {
    gMarkers.clear();
    Harness h;
    h.session.open(QStringLiteral("FIXTURE0001"));
    CHECK(waitFor([&]() { return h.ready() && h.session.playing(); }, 15000));
    player::VideoPlayer::State atDispatch = player::VideoPlayer::Idle;
    h.resolver.onResolve = [&]() { atDispatch = h.session.player()->state(); };
    h.session.open(QStringLiteral("FIXTURE0002"));
    h.resolver.onResolve = nullptr;
    CHECK(h.resolver.resolves() == 2);
    CHECK(atDispatch == player::VideoPlayer::Playing || atDispatch == player::VideoPlayer::Paused);
    CHECK(h.session.state() == PlaybackSession::Loading);
    int tap2 = -1;
    for (int i = 0; i < gMarkers.size() && tap2 < 0; ++i)
        if (gMarkers[i].startsWith(QLatin1String("tap ")) && gMarkers[i].contains(QLatin1String("run=2"))) tap2 = i;
    CHECK(tap2 >= 0 && markerIndex(QStringLiteral("resolve-start "), tap2) > tap2);
    CHECK(waitFor([&]() { return h.ready() && h.session.videoId() == QLatin1String("FIXTURE0002"); }, 15000));
    h.session.stop();
}

// ---- Cover video -----------------------------------------------------------------------------

// Cover video: in the background with the cover visible the video keeps decoding (no PLY-09
// freeze) on the ONE player and pipeline the watch page uses; hidden again, the freeze follows
// after the grace. Markers cover-video-on / -off, coverDecodeSeconds.
static void cover_onePipelineWhileCoverVideoOn() {
    gServer->reset();
    gMarkers.clear();
    Harness h;
    int firstFrames = 0;
    QObject::connect(h.session.player(), &player::VideoPlayer::firstFrame, [&firstFrames]() { ++firstFrames; });
    CHECK(h.session.coverVideoEnabled());  // FULL default on
    h.session.setCoverVideoEnabled(false);
    h.session.setCoverVideoEnabled(true);
    h.session.open(QStringLiteral("FIXTURE0001"));
    CHECK(waitFor([&]() { return h.ready() && firstFrames > 0; }, 15000));
    CHECK(!h.session.coverVideoActive());  // foreground
    h.session.setAppActive(false);
    CHECK(waitFor([&]() { return h.session.coverVideoActive(); }, 2000));
    h.session.setCoverVisible(true);
    player::VideoPlayer *p = h.session.player();
    const quint64 serial = p->frameMailbox()->serial();
    spinFor(1100);  // past the 800 ms grace
    CHECK(!p->frozen());
    CHECK(p->frameMailbox()->serial() >= serial + 10);  // frames keep coming (30 fps fixture)
    CHECK(h.session.findChildren<player::VideoPlayer *>().size() == 1);  // no second player / engine
    CHECK(h.session.mediaObject() == p);
    CHECK(sourceBranches(p->plan()) == 2 && p->plan().mode == player::PlayMode::Dual);
    CHECK(markerIndex(QStringLiteral("cover-video-on ")) >= 0);
    CHECK(h.session.diagnostics().value(QStringLiteral("coverDecodeSeconds")).toDouble() >= 1.0);
    // Hidden: the freeze applies as before.
    h.session.setCoverVisible(false);
    CHECK(markerIndex(QStringLiteral("cover-video-off ")) >= 0);
    CHECK(waitFor([&]() { return p->frozen(); }, 2000));
    CHECK(markerIndex(QStringLiteral("video-decode-stopped ")) >= 0);
    const double seconds = h.session.diagnostics().value(QStringLiteral("coverDecodeSeconds")).toDouble();
    spinFor(200);
    CHECK(h.session.diagnostics().value(QStringLiteral("coverDecodeSeconds")).toDouble() == seconds);
    h.session.setAppActive(true);
    CHECK(!p->frozen() && !h.session.coverVideoActive());
    h.session.stop();
}

// videoAspect: the chosen plan's format before the first frame (a 9:16 Shorts-style format ->
// 0.5625), then the decoded picture (the fixture is 320x180); 0 for listen-only.
static void cover_videoAspectBeforeFirstFrame() {
    gServer->reset();
    Harness h;
    h.resolver.shape = [](StreamSet &set) {
        for (int i = 0; i < set.video.size(); ++i) { set.video[i].width = 180; set.video[i].height = 320; }
    };
    int firstFrames = 0;
    double firstAspect = -1;
    int framesAtFirstAspect = -1;
    QObject::connect(h.session.player(), &player::VideoPlayer::firstFrame, [&firstFrames]() { ++firstFrames; });
    QObject::connect(&h.session, &PlaybackSession::videoAspectChanged, [&]() {
        if (firstAspect >= 0) return;
        firstAspect = h.session.videoAspect();
        framesAtFirstAspect = firstFrames;
    });
    CHECK(h.session.videoAspect() == 0);
    h.session.open(QStringLiteral("FIXTURE0001"));
    CHECK(waitFor([&]() { return firstAspect >= 0; }, 5000));
    CHECK(std::fabs(firstAspect - 0.5625) < 1e-9);
    CHECK(framesAtFirstAspect == 0);  // set when the plan was chosen, before the first frame
    CHECK(waitFor([&]() { return firstFrames > 0; }, 15000));
    CHECK(waitFor([&]() { return std::fabs(h.session.videoAspect() - 320.0 / 180.0) < 1e-6; }, 3000));
    h.session.setAudioOnlyMode(true);
    CHECK(h.session.videoAspect() == 0);
    h.session.stop();
}

// ---- PLY-26 / SEL-07 stream instead ---------------------------------------------------------

// A download of FIXTURE0001 playing locally from the device, at least `minPositionMs` in.
static bool playDownload(Harness &h, double minPositionMs) {
    h.session.setLocalMediaLookup([](const QString &id) { return downloadRecord(id); });
    h.session.open(QStringLiteral("FIXTURE0001"), rowHints());
    return h.session.controller().local() &&
           waitFor([&]() { return h.ready() && h.session.playing() && h.session.position() >= minPositionMs; }, 15000);
}

// PLY-26 / SEL-07: local -> streamInstead -> one resolve (open()'s request, markers), the file
// plays on meanwhile, then the dual stream plan at the captured position (+-1 s); no Idle state,
// no `ended`, the owner and title stay; afterwards a stream item; a later open() re-attaches.
static void ply26_streamInsteadSwitchesSeamlessly() {
    gServer->reset();
    gMarkers.clear();
    Harness h;
    CHECK(playDownload(h, 1000));
    CHECK(h.session.canStreamInstead());
    const int owner = diag(h, "owner");
    QList<int> states;
    int ended = 0;
    QObject::connect(&h.session, &PlaybackSession::stateChanged, [&]() { states << h.session.state(); });
    QObject::connect(&h.session, &PlaybackSession::ended, [&ended]() { ++ended; });
    const double captured = h.session.position();
    h.session.streamInstead();
    CHECK(h.resolver.resolves() == 1);
    if (h.resolver.resolves() == 1) {
        const StreamRequest &q = h.resolver.requests[0];
        CHECK(q.videoId == QLatin1String("FIXTURE0001") && !q.speculative && !q.signedIn);
        CHECK(q.purpose == nordtile::playback::StreamPurpose::Video && q.network == nordtile::playback::NetworkClass::Unmetered);
    }
    CHECK(h.resolver.invalidated.isEmpty());
    CHECK(h.session.controller().local() && h.ready() && !h.session.canStreamInstead());  // the file plays on
    const bool switched = waitFor([&]() { return !h.session.controller().local() && h.ready() && h.session.playing(); }, 15000);
    CHECK(switched);
    const double landed = h.session.position();
    if (std::fabs(landed - captured) > 1000)
        std::printf("FAIL stream instead landed at %.0f ms, captured %.0f ms\n", landed, captured);
    CHECK(std::fabs(landed - captured) <= 1000);
    CHECK(h.session.player()->plan().mode == player::PlayMode::Dual && loopbackSources(h.session.player()->plan()));
    CHECK(h.fetcher.opened.contains(QStringLiteral("247")) && h.fetcher.opened.contains(QStringLiteral("251")));
    CHECK(!states.contains(PlaybackSession::Idle) && !states.contains(PlaybackSession::Failed));
    CHECK(ended == 0);
    CHECK(diag(h, "owner") == owner);
    CHECK(h.session.title() == QLatin1String("Downloaded fixture") && h.session.videoId() == QLatin1String("FIXTURE0001"));
    CHECK(!h.session.diagnostics().value(QStringLiteral("local")).toBool());
    CHECK(!h.session.diagnostics().value(QStringLiteral("localSegments")).toBool());
    CHECK(h.session.localCaptions().isEmpty() && !h.session.qualities().isEmpty());
    CHECK(h.session.errorText().isEmpty() && !h.session.canStreamInstead());
    const int rs = markerIndex(QStringLiteral("resolve-start "));
    const int rd = markerIndex(QStringLiteral("resolve-done "), rs + 1);
    const int ss = markerIndex(QStringLiteral("switch-start "), rd + 1);
    const int sr = markerIndex(QStringLiteral("switch-resumed "), ss + 1);
    CHECK(rs >= 0 && rd > rs && ss > rd && sr > ss);
    CHECK(markerHas(ss, "kind=stream"));
    h.session.open(QStringLiteral("FIXTURE0001"), rowHints());  // re-attach: no resolve, no download
    CHECK(h.resolver.resolves() == 1 && !h.session.controller().local());
    h.session.stop();
}

// PLY-26: a failed resolve keeps the file playing; a transient English notice, no setting key.
static void ply26_streamInsteadFailureStaysLocal() {
    gServer->reset();
    Harness h;
    h.resolver.errorClass = StreamErrorClass::Network;
    CHECK(playDownload(h, 500));
    h.session.streamInstead();
    CHECK(h.resolver.resolves() == 1);
    CHECK(waitFor([&]() { return !h.session.errorText().isEmpty(); }, 5000));
    CHECK(h.session.errorText() == QLatin1String("Streaming is not available right now."));
    CHECK(h.session.errorSettingKey().isEmpty() && !h.session.retryAvailable());
    CHECK(h.session.controller().local() && h.ready() && h.session.playing());
    CHECK(h.session.player()->plan().mode == player::PlayMode::Local && h.resolver.resolves() == 1);
    CHECK(h.session.canStreamInstead() && h.fetcher.opened.isEmpty());
    h.session.stop();
}

// PLY-26: a no-op for a stream; a paused download switches and stays paused.
static void ply26_streamInsteadNoOpAndPaused() {
    gServer->reset();
    Harness s;
    s.session.open(QStringLiteral("FIXTURE0001"), rowHints());
    CHECK(waitFor([&]() { return s.ready(); }, 15000));
    CHECK(!s.session.canStreamInstead());
    s.session.streamInstead();
    CHECK(s.resolver.resolves() == 1);
    s.session.stop();

    Harness h;
    CHECK(playDownload(h, 1000));
    h.session.pause();
    CHECK(waitFor([&]() { return !h.session.playing(); }, 5000));
    const double captured = h.session.position();
    h.session.streamInstead();
    CHECK(waitFor([&]() { return !h.session.controller().local() && h.ready(); }, 15000));
    spinFor(500);
    CHECK(!h.session.playing() && h.ready());
    if (h.session.player()->plan().mode != player::PlayMode::Dual) {
        const QVariantMap d = h.session.diagnostics();
        // Seen once on a shared gate and not reproduced since. A dual candidate that is lost before
        // its preroll gives exactly this (the chain's next candidate plays): what lost it is the
        // last fault and the markers below.
        std::printf("  DIAG paused stream-instead: plan mode %d, player state %d, attempt %d of %d, status %d, opened %s, "
                    "last fault '%s', error '%s', resolves %d, position %.0f (captured %.0f)\n",
                    int(h.session.player()->plan().mode), int(h.session.player()->state()), h.session.controller().attempt(),
                    int(h.session.controller().chain().size()), int(h.session.controller().status()),
                    qPrintable(h.fetcher.opened.join(QLatin1Char(','))), h.session.controller().failureReason().c_str(),
                    qPrintable(h.session.errorText()), h.resolver.resolves(), h.session.position(), captured);
        for (int i = qMax(0, gMarkers.size() - 14); i < gMarkers.size(); ++i) std::printf("    %s\n", qPrintable(gMarkers.at(i)));
        const bool later = waitFor([&]() { return h.session.player()->plan().mode == player::PlayMode::Dual; }, 8000);
        std::printf("  DIAG the dual plan %s\n", later ? "came later" : "never came");
    }
    CHECK(h.session.player()->plan().mode == player::PlayMode::Dual);
    CHECK(std::fabs(h.session.position() - captured) <= 1000);
    h.session.play();
    CHECK(waitFor([&]() { return h.session.playing(); }, 5000));
    h.session.stop();
}

// ---- Shorts pager (DSC-17 MAY swipe viewer) ---------------------------------------------------

// Every format URL carries the video id (as real answers do), so streams of one Short are never
// mistaken for another's (SEL-07 reuse compares URLs).
static void perVideoUrls(StreamSet &set) {
    for (QList<StreamFormat> *list : {&set.video, &set.audio, &set.muxed})
        for (int i = 0; i < list->size(); ++i) {
            QUrl url = (*list)[i].url;
            url.setQuery(url.query() + QStringLiteral("&id=") + set.videoId);
            (*list)[i].url = url;
        }
}

static QVariantMap shortHints(bool loop) {
    QVariantMap hints = rowHints();
    hints.insert(QStringLiteral("loop"), loop);
    return hints;
}

static int markerWith(const QString &prefix, const char *key, int from = 0);

// ---- Swipe sequence: many Shorts in a row (memory and threads stay bounded) ----------------------

static long residentKb() {  // resident set, kB (/proc/self/statm), 0 when unreadable
    FILE *f = std::fopen("/proc/self/statm", "r");
    if (!f) return 0;
    long size = 0, resident = 0;
    const int n = std::fscanf(f, "%ld %ld", &size, &resident);
    std::fclose(f);
    return n == 2 ? resident * (sysconf(_SC_PAGESIZE) / 1024) : 0;
}

static int threadCount() {
    int n = 0;
    if (DIR *d = opendir("/proc/self/task")) {
        while (dirent *e = readdir(d)) if (e->d_name[0] != '.') ++n;
        closedir(d);
    }
    return n;
}

static QString fixtureIdN(int n) { return QStringLiteral("FIXTURE%1").arg(n, 4, 10, QLatin1Char('0')); }

// The pager's API (openShort, shortsAhead, holdShort / releaseShort) on the real session: a Short
// loops and is marked (shortActive); a hold pauses it and the last release resumes it; the app in the
// background holds it by itself even with background playback on; closing its page ends it with no
// mini-player; Stop ends the holds; and a plain open() of the same video re-attaches without a mark.
static void shorts_openHoldAndBackground() {
    gServer->reset();
    Harness h;
    h.resolver.shape = perVideoUrls;
    CHECK(h.session.controller().backgroundPlayback());
    h.session.openShort(QStringLiteral("FIXTURE0001"), rowHints());
    const int ctx = h.session.claimContext();  // the page claims the context its open created
    CHECK(h.session.shortActive() && h.session.loop() && !h.session.shortHeld());
    CHECK(waitFor([&]() { return h.ready() && h.session.playing(); }, 15000));
    CHECK(h.session.diagnostics().value(QStringLiteral("shortActive")).toBool());
    // holdShort / releaseShort.
    h.session.holdShort(QStringLiteral("tab"));
    CHECK(h.session.shortHeld() && waitFor([&]() { return !h.session.playing(); }, 5000));
    CHECK(h.session.diagnostics().value(QStringLiteral("shortHolds")).toInt() == 1);
    h.session.holdShort(QStringLiteral("comments"));
    h.session.releaseShort(QStringLiteral("tab"));
    spinFor(200);
    CHECK(h.session.shortHeld() && !h.session.playing());
    h.session.releaseShort(QStringLiteral("comments"));
    CHECK(!h.session.shortHeld() && waitFor([&]() { return h.session.playing(); }, 5000));
    // The app leaves the foreground (screen lock): a Short is held, the sound does not play on.
    h.session.setAppActive(false);
    CHECK(h.session.shortHeld() && waitFor([&]() { return !h.session.playing(); }, 5000));
    h.session.setAppActive(true);
    CHECK(!h.session.shortHeld() && waitFor([&]() { return h.session.playing(); }, 5000));
    // The user pauses (tap): a later release does not play it again.
    h.session.holdShort(QStringLiteral("comments"));
    h.session.pause();
    h.session.releaseShort(QStringLiteral("comments"));
    spinFor(300);
    CHECK(!h.session.playing() && !h.session.shortHeld());
    h.session.play();
    CHECK(waitFor([&]() { return h.session.playing(); }, 5000));
    // Re-attaching with a plain open() of the playing Short keeps the mark.
    h.session.open(QStringLiteral("FIXTURE0001"), rowHints());
    const int reattached = h.session.claimContext();  // the page that re-attached owns the player now
    CHECK(h.session.shortActive() && h.session.loop() && reattached != ctx);
    // Closing the page ends a Short: no mini-player (no parking), the player is stopped.
    h.session.closeContext(reattached);
    CHECK(!h.session.parked() && waitFor([&]() { return !h.session.playing(); }, 5000));
    CHECK(!h.session.shortActive() && !h.session.shortHeld());
    // A plain video after a Short: no mark, no loop, and the background keeps playing it.
    h.session.open(QStringLiteral("FIXTURE0002"), rowHints());
    CHECK(waitFor([&]() { return h.ready() && h.session.playing(); }, 15000));
    CHECK(!h.session.shortActive() && !h.session.loop());
    h.session.setAppActive(false);
    spinFor(300);
    CHECK(h.session.playing() && !h.session.shortHeld());
    h.session.setAppActive(true);
    h.session.stop();
}

// What the pager's surface relies on: mediaObject is the same player object across the swipes and never
// notified; the old Short's frame is gone when openShort returns (the surface's hasFrame drops to false
// with it) and a frame of the new Short arrives later; the preload's stream jobs are held for 10 minutes
// for a Short; a failed Short reports Failed with its text, does not stop the pager (the next openShort
// plays) and Retry keeps the loop.
static void shorts_surfaceContractAndFailure() {
    gServer->reset();
    Harness h;
    h.resolver.shape = perVideoUrls;
    QObject *player = h.session.mediaObject();
    int mediaObjectChanges = 0;
    QObject::connect(&h.session, &PlaybackSession::mediaObjectChanged, [&mediaObjectChanges]() { ++mediaObjectChanges; });
    h.session.openShort(fixtureIdN(1), shortHints(true));
    CHECK(waitFor([&]() { return h.ready() && h.session.playing(); }, 15000));
    player::VideoFrameMailbox *mailbox = h.session.player()->frameMailbox();
    CHECK(waitFor([&]() { return mailbox->latest().valid(); }, 5000));
    h.session.shortsAhead(QStringList() << fixtureIdN(2) << fixtureIdN(3));
    CHECK(waitFor([&]() { return diag(h, "likelyNextHeldJobs") == 2; }, 3000));
    CHECK(diag(h, "likelyHoldRemainingMs") > 300000);             // a Short: held for minutes, not 60 s
    const quint64 serialBefore = mailbox->serial();
    h.session.openShort(fixtureIdN(2), shortHints(true));
    CHECK(!mailbox->latest().valid());                              // the left Short's frame is gone at once
    CHECK(waitFor([&]() { return h.ready() && h.session.playing() && mailbox->latest().valid(); }, 15000));
    CHECK(mailbox->serial() != serialBefore);
    CHECK(h.session.mediaObject() == player && mediaObjectChanges == 0);
    // A plain item keeps the 60 s hold.
    h.session.open(fixtureIdN(4), rowHints());
    CHECK(waitFor([&]() { return h.ready() && h.session.playing(); }, 15000));
    h.session.likelyNext(fixtureIdN(5), rowHints());
    CHECK(waitFor([&]() { return diag(h, "likelyNextHeldJobs") == 2; }, 3000));
    // The timer is a coarse one (Qt rounds a 60 s interval by up to 5 %: remainingTime() showed 60.0 - 60.5 s).
    CHECK(diag(h, "likelyHoldRemainingMs") > 0 && diag(h, "likelyHoldRemainingMs") <= 65000);
    // A Short that cannot play: Failed with its text; the pager moves on; Retry keeps the loop.
    h.resolver.errorClass = StreamErrorClass::Removed;
    h.session.openShort(fixtureIdN(6), shortHints(true));
    CHECK(waitFor([&]() { return h.session.state() == PlaybackSession::Failed; }, 15000));
    CHECK(!h.session.errorText().isEmpty() && h.session.shortActive() && !h.session.playing());
    h.resolver.errorClass = StreamErrorClass::None;
    h.session.openShort(fixtureIdN(7), shortHints(true));        // swiping on
    CHECK(waitFor([&]() { return h.ready() && h.session.playing(); }, 15000));
    CHECK(h.session.errorText().isEmpty() && h.session.videoId() == fixtureIdN(7));
    h.resolver.errorClass = StreamErrorClass::Network;
    h.session.openShort(fixtureIdN(8), shortHints(true));
    CHECK(waitFor([&]() { return h.session.state() == PlaybackSession::Failed; }, 60000));
    h.resolver.errorClass = StreamErrorClass::None;
    CHECK(h.session.retryAvailable());
    h.session.retry();
    CHECK(waitFor([&]() { return h.ready() && h.session.playing(); }, 15000));
    CHECK(h.session.loop() && h.session.shortActive() && h.session.videoId() == fixtureIdN(8));
    h.session.stop();
}

// shortsAhead: the first id is prepared like likelyNext (speculative resolve, held jobs), the second is
// resolved into the cache only (a prefetch: no jobs); the same ids again start nothing new; after the
// swipe the pair moves on by one; the swipe's tap marker says kind=short ahead=1.
static void shorts_aheadPreparesTwo() {
    gServer->reset();
    gMarkers.clear();
    Harness h;
    h.resolver.shape = perVideoUrls;
    h.session.openShort(QStringLiteral("FIXTURE0001"), shortHints(true));
    CHECK(waitFor([&]() { return h.ready() && h.session.playing(); }, 15000));
    const int resolves = h.resolver.resolves();
    h.session.shortsAhead(QStringList() << fixtureIdN(2) << fixtureIdN(3) << fixtureIdN(4));  // a third is ignored
    CHECK(h.resolver.resolves() == resolves + 1 && h.resolver.requests.back().speculative);
    CHECK(h.resolver.requests.back().videoId == fixtureIdN(2));
    CHECK(h.resolver.prefetches.size() == 1 && h.resolver.prefetches[0].videoId == fixtureIdN(3));
    CHECK(waitFor([&]() { return diag(h, "likelyNextHeldJobs") == 2; }, 3000));  // the first one only holds jobs
    CHECK(diag(h, "shortsAheadCalls") == 1 && diag(h, "shortsAheadCached") == 1);
    CHECK(markerHas(markerWith(QStringLiteral("shorts-ahead "), "n=2"), qPrintable(QStringLiteral("second=") + fixtureIdN(3))));
    // The same call again (the pager settles on the same page): nothing new.
    h.session.shortsAhead(QStringList() << fixtureIdN(2) << fixtureIdN(3));
    CHECK(h.resolver.resolves() == resolves + 1 && h.resolver.prefetches.size() == 1 && diag(h, "shortsAheadCached") == 1);
    // The current id and invalid ids are skipped.
    h.session.shortsAhead(QStringList() << fixtureIdN(1) << QStringLiteral("bad") << fixtureIdN(2));
    CHECK(h.resolver.prefetches.size() == 1);
    // The swipe to the prepared Short: its resolve and jobs are reused, then the pair moves on.
    const int tapFrom = gMarkers.size();
    h.session.openShort(fixtureIdN(2), shortHints(true));
    CHECK(waitFor([&]() { return h.ready() && h.session.playing(); }, 15000));
    CHECK(h.resolver.resolves() == resolves + 1);  // no new resolve for the open itself
    const int tap = markerWith(QStringLiteral("tap "), "kind=short", tapFrom);
    CHECK(tap >= 0 && markerHas(tap, "ahead=1"));
    h.session.shortsAhead(QStringList() << fixtureIdN(3) << fixtureIdN(4));
    CHECK(h.resolver.requests.back().videoId == fixtureIdN(3) && h.resolver.requests.back().speculative);
    CHECK(h.resolver.prefetches.size() == 2 && h.resolver.prefetches[1].videoId == fixtureIdN(4));
    // Nothing in the background.
    h.session.setAppActive(false);
    const int before = h.resolver.prefetches.size();
    h.session.shortsAhead(QStringList() << fixtureIdN(5) << fixtureIdN(6));
    CHECK(static_cast<int>(h.resolver.prefetches.size()) == before);
    h.session.setAppActive(true);
    h.session.stop();
}

// The swipe a Shorts pager makes, repeated: the next two Shorts are prepared while the current one
// plays (shortsAhead), then the next one is opened (openShort); the Short left behind is stopped at
// once. Each open builds and tears down a pipeline. After a warm-up the resident size and the thread
// count stay flat (no pipeline left behind, no growth per open), and each open is the only player
// with only its own two jobs.
static void shorts_swipeSequenceStaysBounded() {
    gServer->reset();
    Harness h;
    h.resolver.shape = perVideoUrls;
    h.resolver.delayMs = 40;
    const int swipes = std::getenv("SHORTS_SWIPES") ? std::max(5, std::atoi(std::getenv("SHORTS_SWIPES"))) : 50;
    const int warmup = std::min(10, swipes / 2);
    int frames = 0;
    QObject::connect(h.session.player(), &player::VideoPlayer::firstFrame, [&frames]() { ++frames; });
    std::vector<qint64> firstFrameMs;
    std::vector<long> rss;
    std::vector<int> threads;
    QElapsedTimer clock;
    for (int i = 1; i <= swipes; ++i) {
        const int before = frames;
        clock.restart();
        h.session.openShort(fixtureIdN(i), shortHints(true));
        const bool shown = waitFor([&]() { return h.ready() && h.session.playing() && frames > before; }, 15000);
        CHECK_MSG_SESSION(shown, "swipe %d: no first frame", i);
        if (!shown) break;
        firstFrameMs.push_back(clock.elapsed());
        h.session.shortsAhead(QStringList() << fixtureIdN(i + 1) << fixtureIdN(i + 2));
        spinFor(200);  // the Short plays for a moment before the next swipe
        rss.push_back(residentKb());
        threads.push_back(threadCount());
    }
    if (static_cast<int>(rss.size()) < swipes) return;
    // One player, only the playing Short's jobs (the preload for the next one is dropped at the open).
    CHECK(h.session.findChildren<player::VideoPlayer *>().size() == 1);
    h.session.stop();
    spinFor(1500);  // the last teardown
    CHECK(waitFor([&]() { return h.fetcher.open_ == 0; }, 3000));
    const long afterStop = residentKb();
    // Freed memory the allocator still holds is not a leak: what malloc_trim(0) returns shows it.
    long trimBefore = 0, trimAfter = 0;
    const bool trimmed = player::trimMalloc(&trimBefore, &trimAfter);
    std::vector<qint64> sorted = firstFrameMs;
    std::sort(sorted.begin(), sorted.end());
    std::printf("  %d swipes: first frame median %lld ms, p90 %lld ms; resident %ld kB after the warm-up (swipe %d), %ld kB "
                "at the end, %ld kB after stop; threads %d -> %d\n", swipes, (long long)sorted[sorted.size() / 2],
                (long long)sorted[sorted.size() * 9 / 10], rss[warmup - 1], warmup, rss.back(), afterStop, threads[warmup - 1],
                threads.back());
    for (int i = warmup; i <= swipes; i += std::max(10, swipes / 10))
        std::printf("    swipe %2d: resident %ld kB, threads %d\n", i, rss[i - 1], threads[i - 1]);
    std::printf("    malloc_trim after the last stop: %ld kB -> %ld kB (%s)\n", trimBefore, trimAfter, trimmed ? "ran" : "not available");
    // The allocator keeps freed memory (a plateau that differs from run to run: 125 -> 136 MB in one run
    // of 50 swipes, 206 -> 207 MB in one of 200), so the bounds catch a real per-open growth (the phone
    // showed 6-8 MB per open without the arena cap), not that retention: 1 MB per swipe after the
    // warm-up, and what malloc_trim(0) cannot give back (live memory) not above the warm-up level + 20 MB.
    const long growthKb = rss.back() - rss[warmup - 1];
    const long perSwipeKb = swipes > warmup ? growthKb / (swipes - warmup) : 0;
    CHECK_MSG_SESSION(perSwipeKb <= 1000, "resident size grows %ld kB per swipe after the warm-up (%ld kB in %d swipes)", perSwipeKb,
                      growthKb, swipes - warmup);
    CHECK_MSG_SESSION(!trimmed || trimAfter <= rss[warmup - 1] + 20 * 1024,
                      "live memory after the sequence %ld kB, resident after the warm-up %ld kB", trimAfter, rss[warmup - 1]);
    CHECK_MSG_SESSION(threads.back() <= threads[warmup - 1] + 4, "threads grew from %d to %d", threads[warmup - 1], threads.back());
}

static int markerWith(const QString &prefix, const char *key, int from) {
    for (int i = from; i < gMarkers.size(); ++i)
        if (gMarkers[i].startsWith(prefix) && gMarkers[i].contains(QLatin1String(key))) return i;
    return -1;
}

// likelyNext -> open: one resolve in all (the preload's, speculative), resolve-done path=prefetch,
// the held stream jobs reused (no new fetcher open for those formats), and the first frame sooner
// than a cold open in the same harness.
static void shorts_likelyNextThenOpenReusesPreload() {
    gServer->reset();
    gMarkers.clear();
    Harness h;
    h.resolver.delayMs = 250;
    h.resolver.shape = perVideoUrls;
    qint64 firstFrameAt = -1;
    QElapsedTimer clock;
    clock.start();
    QObject::connect(h.session.player(), &player::VideoPlayer::firstFrame, [&]() { if (firstFrameAt < 0) firstFrameAt = clock.elapsed(); });
    h.session.open(QStringLiteral("FIXTURE0001"), shortHints(true));  // warm-up (first pipeline of the process)
    CHECK(waitFor([&]() { return h.ready() && h.session.playing() && firstFrameAt >= 0; }, 15000));
    // Cold: a Short nobody preloaded.
    clock.restart();
    firstFrameAt = -1;
    h.session.open(QStringLiteral("FIXTURE0002"), shortHints(true));
    CHECK(waitFor([&]() { return h.ready() && h.session.playing() && firstFrameAt >= 0; }, 15000));
    const qint64 cold = firstFrameAt;
    // Preload the next one while this one plays.
    const int resolves = h.resolver.resolves();
    h.session.likelyNext(QStringLiteral("FIXTURE0003"), shortHints(true));
    CHECK(h.resolver.resolves() == resolves + 1 && h.resolver.requests.back().speculative);
    CHECK(h.resolver.requests.back().videoId == QLatin1String("FIXTURE0003") && !h.resolver.requests.back().signedIn);
    CHECK(markerHas(markerWith(QStringLiteral("likely-next "), "video=FIXTURE0003"), "decision=start"));
    CHECK(waitFor([&]() { return diag(h, "likelyNextHeldJobs") == 2; }, 3000));
    spinFor(50);  // the held opens answered
    const int opened247 = h.fetcher.opened.count(QStringLiteral("247")), opened251 = h.fetcher.opened.count(QStringLiteral("251"));
    const int openedAll = h.fetcher.opened.size();
    CHECK(h.session.videoId() == QLatin1String("FIXTURE0002") && h.session.playing());  // the current Short plays on
    CHECK(h.session.findChildren<player::VideoPlayer *>().size() == 1);                  // no second pipeline
    // The swipe.
    const int tapFrom = gMarkers.size();
    clock.restart();
    firstFrameAt = -1;
    h.session.open(QStringLiteral("FIXTURE0003"), shortHints(true));
    CHECK(h.resolver.resolves() == resolves + 1);
    const bool warmStarted = waitFor([&]() { return h.ready() && h.session.playing() && firstFrameAt >= 0; }, 15000);
    if (!warmStarted)
        std::printf("FAIL warm open: state %d playing %d first frame %lld player state %d reason %s\n", h.session.state(),
                    int(h.session.playing()), (long long)firstFrameAt, int(h.session.player()->state()),
                    qPrintable(h.session.diagnostics().value(QStringLiteral("failureReason")).toString()));
    CHECK(warmStarted);
    const qint64 warm = firstFrameAt;
    std::printf("  first frame: cold open %lld ms, after likelyNext %lld ms\n", (long long)cold, (long long)warm);
    CHECK(warm < cold);
    CHECK(h.resolver.resolves() == resolves + 1 && h.resolver.cancelled.empty());
    CHECK(h.fetcher.opened.count(QStringLiteral("247")) == opened247 && h.fetcher.opened.count(QStringLiteral("251")) == opened251);
    CHECK(h.fetcher.opened.size() == openedAll);
    CHECK(h.session.player()->plan().mode == player::PlayMode::Dual && loopbackSources(h.session.player()->plan()));
    const int tap = markerWith(QStringLiteral("tap "), "run=3", tapFrom);
    const int su = markerIndex(QStringLiteral("speculative-used "), tap);
    const int rd = markerIndex(QStringLiteral("resolve-done "), su);
    CHECK(tap >= 0 && su > tap && rd > su);
    CHECK(markerHas(su, "kind=likely") && markerHas(su, "run=3") && markerHas(rd, "path=prefetch") && markerHas(rd, "run=3"));
    CHECK(h.session.diagnostics().value(QStringLiteral("resolvePath")).toString() == QLatin1String("prefetch"));
    CHECK(diag(h, "likelyNextStarted") == 1 && diag(h, "likelyNextUsed") == 1 && diag(h, "likelyNextWasted") == 0);
    CHECK(diag(h, "likelyNextHeldJobs") == 0);
    // One Short plays at a time: the previous one's stream jobs are closed, one player.
    CHECK(h.session.findChildren<player::VideoPlayer *>().size() == 1);
    CHECK(h.session.player()->plan().branches.size() == 2);
    CHECK(waitFor([&]() { return h.fetcher.open_ == 2; }, 2000));  // only the playing Short's two jobs
    h.session.stop();
    CHECK(h.fetcher.open_ == 0 && diag(h, "streamJobs") == 0);
}

// The swipe comes early: an open while the preload's resolve still runs joins it (one resolve,
// path=prefetch); an open while the held jobs are still opening adopts them (no second open of
// those formats; the load starts when they answer).
static void shorts_openJoinsPreloadInFlight() {
    gServer->reset();
    gMarkers.clear();
    Harness h;
    h.resolver.shape = perVideoUrls;
    h.session.open(QStringLiteral("FIXTURE0001"), shortHints(true));
    CHECK(waitFor([&]() { return h.ready() && h.session.playing(); }, 15000));
    const int resolves = h.resolver.resolves();
    h.resolver.delayMs = 300;
    h.session.likelyNext(QStringLiteral("FIXTURE0002"));
    CHECK(h.resolver.resolves() == resolves + 1);
    const int from = gMarkers.size();
    h.session.open(QStringLiteral("FIXTURE0002"), shortHints(true));
    CHECK(h.resolver.resolves() == resolves + 1);
    h.session.likelyNext(QStringLiteral("FIXTURE0003"));  // while that open waits: deferred
    CHECK(h.resolver.resolves() == resolves + 1);
    CHECK(waitFor([&]() { return h.ready() && h.session.playing() && h.session.videoId() == QLatin1String("FIXTURE0002"); }, 15000));
    CHECK(h.resolver.cancelled.empty());
    CHECK(markerHas(markerIndex(QStringLiteral("resolve-done "), from), "path=prefetch"));
    CHECK(diag(h, "likelyNextUsed") == 1);
    // The deferred preload starts once FIXTURE0002 prerolled; this time the opens are slow.
    h.fetcher.openDelayMs = 400;
    CHECK(waitFor([&]() { return h.resolver.resolves() == resolves + 2 && diag(h, "likelyNextHeldJobs") == 2; }, 3000));
    CHECK(h.resolver.requests.back().videoId == QLatin1String("FIXTURE0003"));
    const int opened = h.fetcher.opened.size();
    h.session.open(QStringLiteral("FIXTURE0003"), shortHints(true));  // the held opens have not answered yet
    CHECK(waitFor([&]() { return h.ready() && h.session.playing() && h.session.videoId() == QLatin1String("FIXTURE0003"); }, 15000));
    CHECK(h.fetcher.opened.size() == opened && h.resolver.resolves() == resolves + 2);
    CHECK(loopbackSources(h.session.player()->plan()) && diag(h, "likelyNextUsed") == 2);
    h.session.stop();
    CHECK(diag(h, "streamJobs") == 0 && h.fetcher.open_ == 0);
}

// likelyNext bounds: a new id replaces the old one (its resolve cancelled, its held jobs closed);
// never for the playing id, a downloaded id or in the background (going there drops the preload);
// the same id again keeps it; rowsVisible and a press skip the preloaded id; an open of another id
// and Stop drop it; while a play starts it waits until that play has prerolled.
static void shorts_likelyNextBounds() {
    gServer->reset();
    gMarkers.clear();
    Harness h;
    h.resolver.shape = perVideoUrls;
    h.session.setLocalMediaLookup([](const QString &id) {
        return id == QLatin1String("FIXTURE00LL") ? downloadRecord(id) : QVariantMap();
    });
    h.session.open(QStringLiteral("FIXTURE0001"), shortHints(true));
    CHECK(waitFor([&]() { return h.ready() && h.session.playing(); }, 15000));
    int resolves = h.resolver.resolves();
    // Replaced while in flight: its resolve is cancelled.
    h.resolver.delayMs = 300;
    h.session.likelyNext(QStringLiteral("FIXTURE0002"));
    CHECK(h.resolver.resolves() == resolves + 1);
    h.session.likelyNext(QStringLiteral("FIXTURE0003"));
    CHECK(h.resolver.resolves() == resolves + 2 && h.resolver.cancelled.size() == 1);
    CHECK(markerHas(markerWith(QStringLiteral("likely-drop "), "video=FIXTURE0002"), "reason=replaced"));
    CHECK(waitFor([&]() { return diag(h, "likelyNextHeldJobs") == 2; }, 3000));
    spinFor(50);
    // Replaced while held: its jobs are closed.
    h.resolver.delayMs = 5;
    const int open = h.fetcher.open_, closed = h.fetcher.closed.size();
    h.session.likelyNext(QStringLiteral("FIXTURE0004"));
    CHECK(h.fetcher.open_ == open - 2 && h.fetcher.closed.size() == closed + 2);
    CHECK(markerHas(markerWith(QStringLiteral("likely-drop "), "video=FIXTURE0003"), "jobs=2"));
    CHECK(waitFor([&]() { return diag(h, "likelyNextHeldJobs") == 2; }, 3000));
    resolves = h.resolver.resolves();
    // Never for the playing id, a download or an invalid id; the same id again keeps the preload.
    h.session.likelyNext(QStringLiteral("FIXTURE0001"));
    h.session.likelyNext(QStringLiteral("FIXTURE00LL"));
    h.session.likelyNext(QStringLiteral("not an id"));
    h.session.likelyNext(QStringLiteral("FIXTURE0004"));
    CHECK(h.resolver.resolves() == resolves);
    CHECK(h.session.diagnostics().value(QStringLiteral("likelyNextVideo")).toString() == QLatin1String("FIXTURE0004"));
    CHECK(markerHas(markerWith(QStringLiteral("likely-next "), "video=FIXTURE0001"), "reason=playing"));
    CHECK(markerHas(markerWith(QStringLiteral("likely-next "), "video=FIXTURE00LL"), "reason=local"));
    CHECK(markerHas(markerWith(QStringLiteral("likely-next "), "decision=keep"), "video=FIXTURE0004"));
    // rowsVisible (likelyVisible of the same row) and a press of it start nothing more.
    spinFor(kSpeculationQuietMs + 100);
    h.session.rowsVisible(QStringList() << QStringLiteral("FIXTURE0004") << QStringLiteral("FIXTURE0005"));
    CHECK(h.resolver.prefetches.empty() && h.resolver.resolves() == resolves);
    CHECK(markerHas(markerIndex(QStringLiteral("rows-visible ")), "reason=likely-next"));
    h.session.pressStarted(QStringLiteral("FIXTURE0004"), QVariantMap(), 21);
    CHECK(h.resolver.resolves() == resolves && diag(h, "speculativeStarted") == 0);
    // The background drops it and starts nothing.
    h.session.setAppActive(false);
    CHECK(diag(h, "likelyNextHeldJobs") == 0 && h.session.diagnostics().value(QStringLiteral("likelyNextVideo")).toString().isEmpty());
    h.session.likelyNext(QStringLiteral("FIXTURE0006"));
    CHECK(h.resolver.resolves() == resolves);
    CHECK(markerHas(markerWith(QStringLiteral("likely-next "), "video=FIXTURE0006"), "reason=background"));
    h.session.setAppActive(true);
    // An open of another id drops it (jobs closed); Stop drops it too.
    h.session.likelyNext(QStringLiteral("FIXTURE0007"));
    CHECK(waitFor([&]() { return diag(h, "likelyNextHeldJobs") == 2; }, 3000));
    spinFor(30);
    const int closedBefore = h.fetcher.closed.size();
    h.session.open(QStringLiteral("FIXTURE0008"), shortHints(true));
    CHECK(h.fetcher.closed.size() >= closedBefore + 2 && diag(h, "likelyNextHeldJobs") == 0);
    CHECK(markerHas(markerWith(QStringLiteral("likely-drop "), "video=FIXTURE0007"), "reason=open"));
    // While FIXTURE0008 starts, a preload waits for its preroll.
    resolves = h.resolver.resolves();
    h.session.likelyNext(QStringLiteral("FIXTURE0009"));
    CHECK(h.resolver.resolves() == resolves);
    CHECK(markerHas(markerWith(QStringLiteral("likely-next "), "video=FIXTURE0009"), "decision=defer"));
    CHECK(waitFor([&]() { return h.ready() && h.session.playing(); }, 15000));
    CHECK(waitFor([&]() { return h.resolver.resolves() == resolves + 1; }, 3000));
    CHECK(h.resolver.requests.back().videoId == QLatin1String("FIXTURE0009") && h.resolver.requests.back().speculative);
    CHECK(waitFor([&]() { return diag(h, "likelyNextHeldJobs") == 2; }, 3000));
    spinFor(30);
    h.session.stop();
    CHECK(diag(h, "streamJobs") == 0 && h.fetcher.open_ == 0);
    CHECK(markerHas(markerWith(QStringLiteral("likely-drop "), "video=FIXTURE0009"), "reason=stop"));
    CHECK(diag(h, "likelyNextUsed") == 0 && diag(h, "likelyNextWasted") == diag(h, "likelyNextStarted"));
}

// Shorts loop item: at the end of stream it restarts near 0 in the same pipeline and keeps
// playing: no `ended`, no up-next, no related autoplay or prefetch, no resolve; the end is saved
// once; the library bridge's resume (setStartPosition) is ignored, a link start is not.
// open(sameId, {loop: false}) switches loop off without a reload; the next end is a normal one.
static void shorts_loopRestartsWithoutEnded() {
    gServer->reset();
    gMarkers.clear();
    Harness h;
    const bool related = h.settings.autoplayRelated();
    h.settings.setAutoplayRelated(true);
    int ended = 0, loopChanges = 0;
    QList<double> saves;
    QObject::connect(&h.session, &PlaybackSession::ended, [&ended]() { ++ended; });
    QObject::connect(&h.session, &PlaybackSession::loopChanged, [&loopChanges]() { ++loopChanges; });
    QObject::connect(&h.session, &PlaybackSession::positionSaveRequested, [&saves](const QString &, double ms) { saves << ms; });
    CHECK(!h.session.loop());
    h.session.open(QStringLiteral("FIXTURE0001"), shortHints(true));
    CHECK(h.session.loop() && loopChanges == 1);
    h.session.setStartPosition(2000);  // the library bridge's resume point: ignored
    h.session.setRelatedCandidate(QStringLiteral("FIXTURE0009"));
    CHECK(waitFor([&]() { return h.ready() && h.session.playing(); }, 15000));
    CHECK(h.session.position() < 1500);
    const int opened = h.fetcher.opened.size();
    for (int round = 1; round <= 2; ++round) {
        h.session.seek(3300);
        const bool looped = waitFor([&]() { return markerIndex(QStringLiteral("loop-restart ")) >= 0 &&
                                                   gMarkers.filter(QStringLiteral("loop-restart ")).size() >= round; }, 8000);
        CHECK(looped);
        CHECK(waitFor([&]() { return h.session.playing() && h.session.position() > 0 && h.session.position() < 1500; }, 5000));
        spinFor(300);
        CHECK(h.session.playing() && h.ready() && h.session.position() < 2000);
    }
    CHECK(ended == 0 && h.session.upNextVideoId().isEmpty());
    CHECK(h.resolver.resolves() == 1 && h.resolver.prefetches.empty());
    CHECK(h.fetcher.opened.size() == opened);  // no teardown, no reload
    CHECK(saves.size() == 1 && saves.value(0) == -1.0);
    CHECK(h.session.videoId() == QLatin1String("FIXTURE0001"));
    // The watch page re-attaches with loop: false: loop off, no reload, playing on.
    h.session.open(QStringLiteral("FIXTURE0001"), shortHints(false));
    CHECK(!h.session.loop() && loopChanges == 2);
    CHECK(h.session.playing() && h.resolver.resolves() == 1 && h.fetcher.opened.size() == opened);
    h.session.seek(3300);
    CHECK(waitFor([&]() { return ended == 1; }, 8000));
    CHECK(gMarkers.filter(QStringLiteral("loop-restart ")).size() == 2);
    h.session.stop();
    h.settings.setAutoplayRelated(related);

    // A link start still applies to a loop item (the resume point does not).
    Harness k;
    QVariantMap hints = shortHints(true);
    hints.insert(QStringLiteral("startSeconds"), 2);
    k.session.open(QStringLiteral("FIXTURE0001"), hints);
    k.session.setStartPosition(0);
    CHECK(waitFor([&]() { return k.ready(); }, 15000));
    CHECK(k.session.position() >= 1500);
    k.session.stop();
}

// Shorts contexts (PLY-10): the pager and the full watch page of the SAME Short switch back and
// forth without a reload (one resolve, no new stream job), playing on; a pause() from the pager
// while the watch page owns the player is honoured.
static void shorts_contextSwitchSameShortKeepsPlaying() {
    gServer->reset();
    Harness h;
    h.resolver.shape = perVideoUrls;
    h.session.open(QStringLiteral("FIXTURE0001"), shortHints(true));
    const int pager = h.session.claimContext();
    CHECK(waitFor([&]() { return h.ready() && h.session.playing(); }, 15000));
    const int opened = h.fetcher.opened.size();
    const quint64 serial = h.session.player()->frameMailbox()->serial();
    // The full watch page opens on the same Short (re-attach) and claims its context.
    h.session.open(QStringLiteral("FIXTURE0001"), rowHints());
    const int watch = h.session.claimContext();
    CHECK(watch != pager && diag(h, "owner") == watch);
    CHECK(h.session.loop());  // the watch page's hints carry no loop: kept
    spinFor(200);
    CHECK(h.session.playing() && h.resolver.resolves() == 1 && h.fetcher.opened.size() == opened);
    CHECK(h.session.player()->frameMailbox()->serial() > serial);  // frames kept coming
    // Back to the pager: it becomes active, the watch page closes.
    h.session.activateContext(pager);
    h.session.closeContext(watch);
    CHECK(diag(h, "owner") == pager);
    spinFor(200);
    CHECK(h.session.playing() && h.ready() && h.resolver.resolves() == 1 && h.fetcher.opened.size() == opened);
    // The watch page again; the pager's section is hidden meanwhile and pauses: honoured.
    h.session.open(QStringLiteral("FIXTURE0001"), rowHints());
    const int watch2 = h.session.claimContext();
    CHECK(diag(h, "owner") == watch2);
    h.session.pause();
    CHECK(waitFor([&]() { return !h.session.playing(); }, 5000));
    h.session.activateContext(pager);
    h.session.closeContext(watch2);
    spinFor(200);
    CHECK(!h.session.playing() && h.session.videoId() == QLatin1String("FIXTURE0001"));
    h.session.play();
    CHECK(waitFor([&]() { return h.session.playing(); }, 5000));
    CHECK(h.resolver.resolves() == 1 && h.fetcher.opened.size() == opened);
    h.session.stop();
}


// ---- Watch overlay ---------------------------------------------------------------------------

static StreamFormat dub(const char *id, const char *language) {
    StreamFormat f = format(id, "a.webm", "none", "opus", 0);
    f.language = QLatin1String(language);
    f.originalAudio = false;
    f.languagePreference = -1;
    return f;
}

// Seek bar: bufferedPosition while a dual stream plays (ahead of the position, within the
// duration), notified; 0 after Stop; a download is buffered to its duration.
static void seekbar_bufferedPosition() {
    gServer->reset();
    Harness h;
    int notified = 0;
    QObject::connect(&h.session, &PlaybackSession::bufferedPositionChanged, [&notified]() { ++notified; });
    CHECK(h.session.bufferedPosition() == 0);
    h.session.open(QStringLiteral("FIXTURE0001"), rowHints());
    CHECK(waitFor([&]() { return h.ready() && h.session.playing() && h.session.bufferedPosition() > 0; }, 15000));
    spinFor(400);
    const double buffered = h.session.bufferedPosition(), position = h.session.position();
    std::printf("  dual stream: position %.0f ms, buffered %.0f ms of %.0f ms\n", position, buffered, h.session.duration());
    CHECK(notified > 0 && buffered + 300 >= position && buffered <= h.session.duration() + 1);
    h.session.stop();
    CHECK(h.session.bufferedPosition() == 0);
    Harness k;
    k.session.setLocalMediaLookup([](const QString &id) { return downloadRecord(id); });
    k.session.open(QStringLiteral("FIXTURE0001"));
    CHECK(waitFor([&]() { return k.ready() && k.session.bufferedPosition() > 0; }, 15000));
    CHECK(std::fabs(k.session.bufferedPosition() - k.session.player()->duration()) < 1);
    k.session.stop();
}

// SEL-05 / SEL-07: the dub picker of a set with a German dub; a pick plays from the same set (no
// resolve, marker switch-start kind=dub), the video job is kept and only the new audio is opened.
static void sel05_audioTrackSwitchFromSameSet() {
    gServer->reset();
    gMarkers.clear();
    Harness h;
    h.resolver.shape = [](StreamSet &set) { set.audio << dub("251de", "de"); };
    QVariantMap hints = rowHints();
    h.session.open(QStringLiteral("FIXTURE0001"), hints);
    CHECK(waitFor([&]() { return h.ready() && h.session.playing(); }, 15000));
    QVariantList tracks = h.session.audioTracks();
    CHECK(tracks.size() == 2);
    if (tracks.size() != 2) { h.session.stop(); return; }
    const QVariantMap original = tracks[0].toMap(), german = tracks[1].toMap();
    std::printf("  audio tracks: \"%s\", \"%s\"\n", qPrintable(original.value(QStringLiteral("label")).toString()),
                qPrintable(german.value(QStringLiteral("label")).toString()));
    CHECK(original.value(QStringLiteral("id")).toString() == QLatin1String("und"));  // the fixture's original is untagged
    CHECK(original.value(QStringLiteral("original")).toBool() && original.value(QStringLiteral("selected")).toBool());
    CHECK(german.value(QStringLiteral("id")).toString() == QLatin1String("de") && !german.value(QStringLiteral("selected")).toBool());
    CHECK(german.value(QStringLiteral("language")).toString() == QLatin1String("de"));
    CHECK(german.value(QStringLiteral("label")).toString() == QStringLiteral("Deutsch"));
    CHECK(!german.value(QStringLiteral("descriptive")).toBool());
    spinFor(300);
    h.session.setAudioTrack(QStringLiteral("de"));
    CHECK(markerIndex(QStringLiteral("switch-start ")) >= 0 && markerHas(markerIndex(QStringLiteral("switch-start ")), "kind=dub"));
    CHECK(waitFor([&]() { return h.ready() && h.session.playing(); }, 15000));
    CHECK(h.resolver.resolves() == 1);
    CHECK(h.fetcher.opened.count(QStringLiteral("251de")) == 1 && h.fetcher.opened.count(QStringLiteral("247")) == 1);
    tracks = h.session.audioTracks();
    CHECK(tracks.size() == 2 && tracks.value(1).toMap().value(QStringLiteral("selected")).toBool());
    CHECK(h.session.controller().rememberedDub() == "de");
    // SEL-05: the pick is stored (AppSettings playback/dubLanguage, remembered without a row).
    CHECK(h.settings.value(QStringLiteral("playback/dubLanguage")).toString() == QLatin1String("de"));
    // Stable volume: no DRC track in this set, so nothing switches; the flag is kept and stored.
    const int opened = h.fetcher.opened.size();
    CHECK(!h.session.stableVolumeAvailable() && !h.session.property("stableVolumeAvailable").toBool());
    h.session.setStableVolume(true);
    CHECK(h.session.stableVolume() && h.fetcher.opened.size() == opened);
    CHECK(h.settings.value(QStringLiteral("playback/stableVolume")).toBool());
    {   // A later session starts from the stored values.
        Harness later;
        CHECK(later.session.controller().rememberedDub() == "de" && later.session.stableVolume());
    }
    // The next video asks for the remembered dub.
    h.session.open(QStringLiteral("FIXTURE0002"), hints);
    CHECK(h.resolver.resolves() == 2 && h.resolver.requests.back().dubLanguage == QLatin1String("de"));
    // Picking the original stores "no preference"; the Sound switch off is stored too.
    CHECK(waitFor([&]() { return h.ready() && h.session.audioTracks().size() == 2; }, 15000));
    h.session.setAudioTrack(QStringLiteral("und"));
    CHECK(h.settings.value(QStringLiteral("playback/dubLanguage")).toString().isEmpty());
    h.session.setStableVolume(false);
    CHECK(!h.settings.value(QStringLiteral("playback/stableVolume")).toBool());
    h.session.stop();
    // A local item has no picker.
    Harness k;
    k.session.setLocalMediaLookup([](const QString &id) { return downloadRecord(id); });
    k.session.open(QStringLiteral("FIXTURE0001"));
    CHECK(waitFor([&]() { return k.ready(); }, 15000));
    CHECK(k.session.audioTracks().isEmpty());
    k.session.stop();
    // Labels.
    media::AudioTrackOption o;
    o.language = "pt-br";
    o.original = true;
    CHECK(audioTrackLabel(o).endsWith(QStringLiteral(" (original)")) && !audioTrackLabel(o).startsWith(QStringLiteral("pt-br")));
    o.language = "xx-unknown";
    o.original = false;
    o.descriptive = true;
    CHECK(audioTrackLabel(o) == QStringLiteral("xx-unknown (audio description)"));
    o.language.clear();
    o.descriptive = false;
    CHECK(audioTrackLabel(o) == QStringLiteral("Default"));
}

// Hold-to-speed, the writable loop and the skipper's segments on a playing session.
static void ovl_holdSpeedLoopSegments() {
    gServer->reset();
    Harness h;
    const double remembered = h.settings.playbackSpeed();
    h.session.open(QStringLiteral("FIXTURE0001"), rowHints());
    CHECK(waitFor([&]() { return h.ready() && h.session.playing(); }, 15000));
    h.session.holdSpeed(true);
    CHECK(h.session.rate() == 2.0 && waitFor([&]() { return h.session.player()->rate() == 2.0; }, 2000));
    CHECK(h.settings.playbackSpeed() == remembered);                 // never remembered
    h.session.holdSpeed(false);
    CHECK(h.session.rate() == remembered && waitFor([&]() { return h.session.player()->rate() == remembered; }, 2000));
    int loopChanges = 0, segmentChanges = 0;
    QObject::connect(&h.session, &PlaybackSession::loopChanged, [&loopChanges]() { ++loopChanges; });
    QObject::connect(&h.session, &PlaybackSession::sponsorSegmentsChanged, [&segmentChanges]() { ++segmentChanges; });
    h.session.setProperty("loop", true);                             // the QML write path
    CHECK(h.session.loop() && loopChanges == 1);
    h.session.setLoop(false);
    CHECK(!h.session.loop() && loopChanges == 2);
    CHECK(h.session.sponsorSegments().isEmpty());
    QVariantMap seg;
    seg.insert(QStringLiteral("category"), QStringLiteral("selfpromo"));
    seg.insert(QStringLiteral("start"), 2500);
    seg.insert(QStringLiteral("end"), 3500);
    h.session.setSponsorSegments(QVariantList() << seg);
    const QVariantList segments = h.session.sponsorSegments();
    CHECK(segmentChanges >= 1 && segments.size() == 1);
    const QVariantMap first = segments.value(0).toMap();
    CHECK(first.value(QStringLiteral("startMs")).toDouble() == 2500 && first.value(QStringLiteral("endMs")).toDouble() == 3500);
    CHECK(first.value(QStringLiteral("category")).toString() == QLatin1String("selfpromo"));
    h.session.stop();
}

// Sleep timer: minutes (the label ticks once a second only while bound), off, and the end of
// the video, which pauses and keeps the session.
static void ovl_sleepTimer() {
    gServer->reset();
    Harness h;
    h.session.open(QStringLiteral("FIXTURE0001"), rowHints());
    CHECK(waitFor([&]() { return h.ready() && h.session.playing(); }, 15000));
    CHECK(h.session.sleepTimerSecondsLeft() == -1);
    int changes = 0;
    QObject::connect(&h.session, &PlaybackSession::sleepTimerChanged, [&changes]() { ++changes; });
    h.session.setSleepTimer(10);
    CHECK(changes == 1 && h.session.sleepTimerSecondsLeft() == 600 && !h.session.sleepTimerAtEnd());
    spinFor(2600);
    std::printf("  sleep timer: %d notifications in 2.6 s, %d s left\n", changes, h.session.sleepTimerSecondsLeft());
    CHECK(changes >= 3 && changes <= 5 && h.session.sleepTimerSecondsLeft() <= 598);
    h.session.setSleepTimer(0);
    CHECK(h.session.sleepTimerSecondsLeft() == -1);
    int ended = 0;
    QObject::connect(&h.session, &PlaybackSession::ended, [&ended]() { ++ended; });
    h.session.setSleepTimer(-1);
    CHECK(h.session.sleepTimerAtEnd() && h.session.sleepTimerSecondsLeft() >= 0);
    h.session.seek(3300);
    CHECK(waitFor([&]() { return ended == 1; }, 8000));
    spinFor(200);
    CHECK(!h.session.playing() && h.session.sleepTimerSecondsLeft() == -1 && !h.session.sleepTimerAtEnd());
    CHECK(h.session.state() == PlaybackSession::Ready && h.session.videoId() == QLatin1String("FIXTURE0001"));
    CHECK(!h.session.controller().wantsPlay() && h.session.controller().arbiter().hasSession());
    h.session.stop();
}

// Settings that had no reader: the mobile-data ceiling, background playback, volume boost and
// loudness normalisation (PLY-22) reach the selection, the controller and the audio chain.
static void set01_settingsReachThePlayer() {
    gServer->reset();
    Harness h;
    // SEL-02: "Maximum quality on mobile data" is the selection's mobile ceiling.
    CHECK(h.settings.preferences().mobileMaxHeight == 480);
    h.settings.setValue(QStringLiteral("playback/maxHeightMobile"), 720);
    CHECK(h.settings.preferences().mobileMaxHeight == 720);
    h.settings.setValue(QStringLiteral("playback/maxHeightMobile"), 480);
    // PLY-22: boost and normalisation, with the answer's loudness (6 dB above the target).
    h.resolver.shape = [](StreamSet &set) { set.hasLoudness = true; set.loudnessDb = 6.0; };
    h.settings.setValue(QStringLiteral("audio/boost"), 200);
    h.settings.setValue(QStringLiteral("audio/loudness"), true);
    h.session.open(QStringLiteral("FIXTURE0001"), rowHints());
    CHECK(waitFor([&]() { return h.ready() && h.session.playing(); }, 15000));
    const player::AudioChainValues loaded = h.session.player()->audioValues();
    std::printf("  audio chain: boost x%.2f, loudness x%.3f, limiter %d\n", loaded.boostGain, loaded.loudnessGain, int(loaded.limiterEnabled));
    CHECK(loaded.boostGain > 1.99 && loaded.boostGain < 2.01 && loaded.limiterEnabled);
    CHECK(loaded.loudnessGain > 0.49 && loaded.loudnessGain < 0.51);     // 10^(-6/20)
    // Live: the settings change while it plays, without a reload.
    const int opened = h.fetcher.opened.size();
    h.settings.setValue(QStringLiteral("audio/boost"), 100);
    h.settings.setValue(QStringLiteral("audio/loudness"), false);
    CHECK(waitFor([&]() { return h.session.player()->audioValues().boostGain < 1.01; }, 3000));
    CHECK(h.session.player()->audioValues().loudnessGain > 0.99 && !h.session.player()->audioValues().limiterEnabled);
    CHECK(h.fetcher.opened.size() == opened);   // no reload
    // Background playback off: leaving the foreground pauses; on (default): it keeps playing.
    CHECK(h.session.controller().backgroundPlayback());
    h.session.setAppActive(false);
    spinFor(200);
    CHECK(h.session.playing());
    h.session.setAppActive(true);
    h.settings.setValue(QStringLiteral("playback/background"), false);
    CHECK(waitFor([&]() { return !h.session.controller().backgroundPlayback(); }, 3000));
    h.session.setAppActive(false);
    CHECK(waitFor([&]() { return !h.session.playing(); }, 5000));
    CHECK(!h.session.controller().wantsPlay());
    h.session.setAppActive(true);
    spinFor(200);
    CHECK(!h.session.playing());
    h.settings.setValue(QStringLiteral("playback/background"), true);
    h.session.stop();
}

// Scrub preview: nothing until a storyboard is known; then the cell of a position; another video
// drops it; a malformed or non-https storyboard gives none.
static void ovl_scrubPreview() {
    Harness h;
    h.session.open(QStringLiteral("FIXTURE0001"), rowHints());
    CHECK(h.session.scrubPreview(1000).isEmpty() && h.session.scrubPreviewUrl(1000).isEmpty());
    QVariantMap board;
    board.insert(QStringLiteral("width"), 80);
    board.insert(QStringLiteral("height"), 45);
    board.insert(QStringLiteral("columns"), 10);
    board.insert(QStringLiteral("rows"), 10);
    QVariantMap fragment;
    fragment.insert(QStringLiteral("url"), QStringLiteral("https://i.ytimg.test/sb/FIXTURE0001/M0.jpg"));
    fragment.insert(QStringLiteral("durationMs"), 100000);
    board.insert(QStringLiteral("fragments"), QVariantList() << fragment);
    h.session.setStoryboard(board);
    const QVariantMap cell = h.session.scrubPreview(23500);  // cell 23 of 1 s cells
    CHECK(cell.value(QStringLiteral("url")).toString() == QLatin1String("https://i.ytimg.test/sb/FIXTURE0001/M0.jpg"));
    CHECK(cell.value(QStringLiteral("x")).toInt() == 240 && cell.value(QStringLiteral("y")).toInt() == 90);
    CHECK(cell.value(QStringLiteral("width")).toInt() == 80 && cell.value(QStringLiteral("height")).toInt() == 45);
    CHECK(h.session.scrubPreviewUrl(23500) == cell.value(QStringLiteral("url")).toString());
    h.session.open(QStringLiteral("FIXTURE0002"), rowHints());
    CHECK(h.session.scrubPreview(23500).isEmpty());
    fragment.insert(QStringLiteral("url"), QStringLiteral("http://i.ytimg.test/sb/M0.jpg"));
    board.insert(QStringLiteral("fragments"), QVariantList() << fragment);
    CHECK(storyboardFor(board).fragments.empty());
    h.session.setStoryboard(board);
    CHECK(h.session.scrubPreview(0).isEmpty());
    h.session.stop();
    // The resolve answer's storyboard (the yt-dlp path carries one): known when the answer is in.
    // A fresh answer for the same video without one keeps it; the next video starts without.
    Harness a;
    bool carry = true;
    a.resolver.shape = [&carry](StreamSet &set) {
        if (!carry) return;
        set.storyboard.width = 160;
        set.storyboard.height = 90;
        set.storyboard.columns = 5;
        set.storyboard.rows = 5;
        nordtile::playback::StoryboardSheet sheet;
        sheet.url = QUrl(QStringLiteral("https://i.ytimg.test/sb/FIXTURE0001/L2/M0.jpg?sigh=y"));
        sheet.durationMs = 50000;
        set.storyboard.sheets << sheet;
    };
    a.session.open(QStringLiteral("FIXTURE0001"), rowHints());
    CHECK(waitFor([&]() { return a.ready(); }, 15000));
    const QVariantMap answered = a.session.scrubPreview(7000);  // cell 3 of 2 s cells
    CHECK(answered.value(QStringLiteral("url")).toString() == QLatin1String("https://i.ytimg.test/sb/FIXTURE0001/L2/M0.jpg?sigh=y"));
    CHECK(answered.value(QStringLiteral("x")).toInt() == 480 && answered.value(QStringLiteral("y")).toInt() == 0);
    CHECK(answered.value(QStringLiteral("width")).toInt() == 160 && answered.value(QStringLiteral("height")).toInt() == 90);
    carry = false;
    a.session.retry();
    CHECK(waitFor([&]() { return a.resolver.resolves() == 2 && a.ready(); }, 15000));
    CHECK(a.session.scrubPreview(7000).value(QStringLiteral("x")).toInt() == 480);
    a.session.open(QStringLiteral("FIXTURE0002"), rowHints());
    CHECK(waitFor([&]() { return a.resolver.resolves() == 3 && a.ready(); }, 15000));
    CHECK(a.session.scrubPreview(7000).isEmpty());
    a.session.stop();
    // The conversion alone: an empty answer, a sheet that is not https, a sheet without a duration.
    nordtile::playback::StreamStoryboard typed;
    CHECK(storyboardFor(typed).fragments.empty());
    typed.width = 160; typed.height = 90; typed.columns = 5; typed.rows = 5;
    nordtile::playback::StoryboardSheet sheet;
    sheet.url = QUrl(QStringLiteral("https://i.ytimg.test/sb/a/M0.jpg"));
    sheet.durationMs = 50000;
    typed.sheets << sheet;
    CHECK(storyboardFor(typed).fragments.size() == 1 && storyboardFor(typed).columns == 5);
    typed.sheets[0].url = QUrl(QStringLiteral("http://i.ytimg.test/sb/a/M0.jpg"));
    CHECK(storyboardFor(typed).fragments.empty());
    typed.sheets[0].url = sheet.url;
    typed.sheets[0].durationMs = 0;
    CHECK(storyboardFor(typed).fragments.empty());
}

// SEL-06 / DL-07: captions through the catalog's caption models: a downloaded video's saved json3
// track is listed, a pick shows the cue at the playing position, "" turns it off (persisted).
static void sel06_captionCueAtPosition() {
    QFile json3(gDir + QStringLiteral("/en.json3"));
    CHECK(json3.open(QIODevice::WriteOnly));
    json3.write("{\"wireMagic\":\"pb3\",\"events\":["
                "{\"tStartMs\":0,\"dDurationMs\":1200,\"segs\":[{\"utf8\":\"First\\nline\"}]},"
                "{\"tStartMs\":1200,\"dDurationMs\":3000,\"segs\":[{\"utf8\":\"Second \"},{\"utf8\":\"line\"}]}]}");
    json3.close();
    QSettings settings(gDir + QStringLiteral("/captions.ini"), QSettings::IniFormat);
    settings.clear();
    catalog::CaptionService service(nullptr, nullptr);
    CaptionTracksModel tracks(&settings);
    CaptionCues cues(&service);
    cues.follow(&tracks);
    Harness h;
    CatalogCaptionSource source(&tracks, &cues);
    int changes = 0;
    QStringList texts;
    QObject::connect(&h.session, &PlaybackSession::captionsChanged, [&changes]() { ++changes; });
    QObject::connect(&h.session, &PlaybackSession::captionTextChanged, [&]() { texts << h.session.captionText(); });
    h.session.setCaptionSource(&source);
    CHECK(h.session.captionTracks().isEmpty() && h.session.captionTrackId().isEmpty());
    QVariantMap record = downloadRecord(QStringLiteral("FIXTURE0001"));
    record.insert(QStringLiteral("segments"), QVariantList());  // no skip over the first cue
    h.session.setLocalMediaLookup([record](const QString &) { return record; });
    h.session.open(QStringLiteral("FIXTURE0001"));
    const QVariantList list = h.session.captionTracks();
    CHECK(list.size() == 1 && changes >= 1);
    const QVariantMap en = list.value(0).toMap();
    CHECK(en.value(QStringLiteral("id")).toString() == QLatin1String("en") && en.value(QStringLiteral("label")).toString() == QLatin1String("English"));
    CHECK(en.value(QStringLiteral("language")).toString() == QLatin1String("en"));
    CHECK(!en.value(QStringLiteral("autoGenerated")).toBool() && !en.value(QStringLiteral("translated")).toBool());
    CHECK(h.session.captionTrackId().isEmpty());          // never chosen: captions start off (SEL-06)
    CHECK(waitFor([&]() { return h.ready() && h.session.playing(); }, 15000));
    h.session.setCaptionTrack(QStringLiteral("en"));
    CHECK(h.session.captionTrackId() == QLatin1String("en") && tracks.preferred() == QLatin1String("en"));
    CHECK(waitFor([&]() { return h.session.captionText() == QLatin1String("Second line"); }, 6000));
    CHECK(texts.contains(QStringLiteral("First line")) || h.session.position() >= 1200);
    CHECK(source.textAt(500) == QLatin1String("First line") && source.textAt(5000).isEmpty());
    h.session.setCaptionTrack(QString());
    CHECK(h.session.captionTrackId().isEmpty() && h.session.captionText().isEmpty());
    CHECK(tracks.preferred() == QLatin1String("off"));
    CHECK(!source.select(QStringLiteral("OTHERVIDEO1"), QStringLiteral("en")));   // another video's tracks
    CHECK(source.tracks(QStringLiteral("OTHERVIDEO1")).isEmpty() && source.currentTrackId(QStringLiteral("OTHERVIDEO1")).isEmpty());
    // A stream of another video: its tracks are not the download's.
    h.session.setLocalMediaLookup(PlaybackSession::LocalMediaLookup());
    h.session.open(QStringLiteral("FIXTURE0002"), rowHints());
    CHECK(h.session.captionTracks().isEmpty());
    h.session.stop();
    h.session.setCaptionSource(nullptr);
    CHECK(h.session.captionTracks().isEmpty());
    // The seam on its own: local tracks of another video and the change callback.
    int sourceChanges = 0;
    source.setChangedCallback([&sourceChanges]() { ++sourceChanges; });
    source.setLocalTracks(QStringLiteral("OTHERVIDEO1"), record.value(QStringLiteral("captions")).toList());
    CHECK(sourceChanges >= 1 && source.tracks(QStringLiteral("OTHERVIDEO1")).size() == 1);
    CHECK(source.select(QStringLiteral("OTHERVIDEO1"), QStringLiteral("en")) && source.currentTrackId(QStringLiteral("OTHERVIDEO1")) == QLatin1String("en"));
    CHECK(!source.select(QStringLiteral("OTHERVIDEO1"), QStringLiteral("xx")));
    source.setChangedCallback(std::function<void()>());
}

// ---- Stall watchdog (PLY-18: a stream that stops delivering without an error) ----------------

// The 12 s clips; `stalled`: behind the server's /stall/ path (silent from a quarter on).
static void longFixture(StreamSet &set, bool stalled) {
    const QString prefix = QLatin1String(stalled ? "https://media.test/stall/" : "https://media.test/");
    set.video[0].url = QUrl(prefix + QStringLiteral("sv.webm?itag=247"));
    set.audio[0].url = QUrl(prefix + QStringLiteral("sa.webm?itag=251"));
}
static QVariantMap longHints() {
    QVariantMap hints = rowHints();
    hints.insert(QStringLiteral("durationSeconds"), 12);
    return hints;
}
// Host thresholds: 1.5 s frozen / 0.8 s starved instead of 8 s / 4 s.
static player::StallConfig fastStall() {
    player::StallConfig c;
    c.noProgressMs = 1500;
    c.starvedMs = 800;
    c.stepGraceMs = 1200;
    c.reloadGraceMs = 6000;
    return c;
}
static bool markerWith(const char *event, const char *key) {
    const QString prefix = QLatin1String(event) + QLatin1Char(' ');
    for (const QString &m : gMarkers)
        if (m.startsWith(prefix) && m.contains(QLatin1String(key))) return true;
    return false;
}
static void printStallMarkers() {
    for (const QString &m : gMarkers)
        if (m.startsWith(QLatin1String("stall-"))) std::printf("  %s\n", qPrintable(m));
}

// (a) The server stops sending both branches mid-stream (a proxy job whose upstream went silent):
// no error, the position stands still. The watchdog finds it; the seek in place meets the same
// silent jobs; new fetcher jobs for the same candidate reach the media again and playback goes on
// past the point where it stood, with the picture kept, no resolve and no refusal report.
static void ply18_stallServerStopsMidStreamReopenRecovers() {
    gServer->reset();
    gMarkers.clear();
    Harness h;
    h.resolver.shape = [](StreamSet &set) { longFixture(set, true); };
    h.fetcher.healOnReopen = true;
    h.session.setStallConfig(fastStall());
    h.session.open(QStringLiteral("FIXTURE0001"), longHints());
    CHECK(waitFor([&]() { return h.ready() && h.session.playing(); }, 15000));
    CHECK(waitFor([&]() { return h.session.player()->frameMailbox()->latest().valid(); }, 5000));
    CHECK(waitFor([&]() { return markerIndex(QStringLiteral("stall-detected ")) >= 0; }, 20000));
    const double stoodAt = h.session.position();
    CHECK(gServer->stalled() >= 2 && diag(h, "stalls") >= 1);
    CHECK(h.session.recovering() && h.session.buffering());  // the page shows it is working on it
    CHECK(h.session.state() != PlaybackSession::Failed && h.session.errorText().isEmpty());
    // keepFrame: the picture stays through the seek and the reload.
    int emptyPolls = 0;
    QElapsedTimer clock;
    clock.start();
    while (!markerWith("stall-recovered", "step=2") && clock.elapsed() < 40000) {
        if (!h.session.player()->frameMailbox()->latest().valid()) ++emptyPolls;
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
    }
    printStallMarkers();
    CHECK(markerWith("stall-recovered", "step=2"));
    CHECK(markerIndex(QStringLiteral("stall-recover ")) > markerIndex(QStringLiteral("stall-detected ")));
    if (emptyPolls) std::printf("  picture empty during the stall recovery (%d polls)\n", emptyPolls);
    CHECK(emptyPolls == 0);
    CHECK(diag(h, "lastStallStep") == 2 && diag(h, "stallRecoveries") >= 1);
    CHECK(!h.session.recovering());
    // The position continues, past where the server went silent (a quarter of 12 s).
    CHECK(waitFor([&]() { return h.session.position() >= stoodAt + 2500 && h.session.position() >= 6000; }, 20000));
    std::printf("  stood at %.0f ms, plays on at %.0f ms; %d stall(s), %d recovered, %d hanging responses\n", stoodAt,
                h.session.position(), diag(h, "stalls"), diag(h, "stallRecoveries"), gServer->stalled());
    CHECK(h.ready() && h.session.playing() && h.session.player()->plan().mode == player::PlayMode::Dual);
    // The same candidate through new jobs: no resolve, no other candidate, the silent jobs closed.
    CHECK(h.resolver.resolves() == 1 && h.resolver.invalidated.isEmpty());
    CHECK(h.fetcher.opened.count(QStringLiteral("247")) == 2 && h.fetcher.opened.count(QStringLiteral("251")) == 2);
    CHECK(!h.fetcher.opened.contains(QStringLiteral("43")));
    CHECK(h.fetcher.closed.contains(QStringLiteral("247")) && h.fetcher.closed.contains(QStringLiteral("251")));
    CHECK(gServer->requests("sv.webm") > 0 && gServer->requests("sa.webm") > 0);
    // RES-07: a stall is not a strike.
    CHECK(h.resolver.refused.isEmpty() && diag(h, "refusedReports") == 0);
    CHECK(diag(h, "recoveryTries") == 0);  // and not a PLY-18 error recovery
    h.session.stop();
    CHECK(h.fetcher.open_ == 0);
}

// (a) again, where the candidate cannot be opened a second time: the reopen fails before its
// preroll -> one fresh resolve (invalidate first) -> its new addresses play on at the position.
// Never the stale set's other candidate, never a refusal report.
static void ply18_stallReopenRefusedFreshResolveRecovers() {
    gServer->reset();
    gMarkers.clear();
    Harness h;
    FakeResolver *resolver = &h.resolver;
    h.resolver.shape = [resolver](StreamSet &set) { longFixture(set, resolver->invalidated.isEmpty()); };
    h.fetcher.refuseReopen = true;
    h.session.setStallConfig(fastStall());
    h.session.open(QStringLiteral("FIXTURE0001"), longHints());
    CHECK(waitFor([&]() { return h.ready() && h.session.playing(); }, 15000));
    CHECK(waitFor([&]() { return markerIndex(QStringLiteral("stall-detected ")) >= 0; }, 20000));
    const double stoodAt = h.session.position();
    CHECK(waitFor([&]() { return markerWith("stall-recovered", "step=3"); }, 40000));
    printStallMarkers();
    CHECK(diag(h, "lastStallStep") == 3 && diag(h, "stallRecoveries") >= 1);
    CHECK(waitFor([&]() { return h.session.position() >= stoodAt + 2500 && h.session.position() >= 6000; }, 20000));
    CHECK(h.ready() && h.session.playing() && h.session.player()->plan().mode == player::PlayMode::Dual);
    CHECK(h.resolver.resolves() == 2);
    CHECK(h.resolver.invalidated == QStringList() << QStringLiteral("FIXTURE0001"));  // fresh: the cache dropped first
    // ... reported as a stall, which the resolver's strike rules do not count (RES-07).
    CHECK(h.resolver.faults.size() == 1 && h.resolver.faults[0] == nordtile::player::SourceFault::Stalled);
    CHECK(!h.fetcher.opened.contains(QStringLiteral("43")));
    CHECK(h.resolver.refused.isEmpty() && diag(h, "refusedReports") == 0);
    CHECK(h.session.diagnostics().value(QStringLiteral("fullProfile")).toBool());
    h.session.stop();
}

// (b) A stuck state: the pipeline sits in PAUSED behind the session's back (the seam:
// VideoPlayer::pause() directly), the session still wants to play and no error comes. Step 1 (a
// seek in place, PLAYING asked again) frees it: no reload, no resolve, no user-seek markers.
static void ply18_stallStuckPipelineRecoveredBySeek() {
    gServer->reset();
    gMarkers.clear();
    Harness h;
    h.resolver.shape = [](StreamSet &set) { longFixture(set, false); };
    h.session.setStallConfig(fastStall());
    h.session.open(QStringLiteral("FIXTURE0001"), longHints());
    CHECK(waitFor([&]() { return h.ready() && h.session.playing() && h.session.position() > 500; }, 15000));
    qint64 position = -1, buffered = -1;
    CHECK(h.session.player()->queryProgress(&position, &buffered) && position > 0 && buffered >= 0);
    CHECK(!h.session.player()->seeking());
    spinFor(2200);  // playing: longer than the threshold, nothing is detected
    CHECK(diag(h, "stalls") == 0 && markerIndex(QStringLiteral("stall-")) < 0);
    h.session.player()->pause();  // stuck
    CHECK(waitFor([&]() { return !h.session.playing(); }, 3000));
    const double stoodAt = h.session.position();
    CHECK(waitFor([&]() { return markerWith("stall-detected", "reason=frozen"); }, 8000));
    CHECK(waitFor([&]() { return markerWith("stall-recovered", "step=1"); }, 8000));
    printStallMarkers();
    CHECK(waitFor([&]() { return h.session.playing() && h.session.position() >= stoodAt + 800; }, 8000));
    CHECK(diag(h, "stalls") == 1 && diag(h, "stallRecoveries") == 1 && diag(h, "lastStallStep") == 1);
    CHECK(markerWith("stall-recover", "step=1") && !markerWith("stall-recover", "step=2"));
    CHECK(h.fetcher.opened.count(QStringLiteral("247")) == 1 && h.fetcher.opened.count(QStringLiteral("251")) == 1);
    CHECK(h.resolver.resolves() == 1 && h.resolver.refused.isEmpty());
    CHECK(markerIndex(QStringLiteral("seek-start ")) < 0 && markerIndex(QStringLiteral("seek-done ")) < 0);
    CHECK(!h.session.recovering() && h.session.state() == PlaybackSession::Ready);
    h.session.stop();
    CHECK(!h.session.player()->queryProgress(&position, &buffered));
}

// (c) The budget is used up (here: one episode per 10 min): the next stall fails at once with the
// retryable error; Retry resolves again and plays on from where it stood.
static void ply18_stallBudgetExhaustedShowsRetry() {
    gServer->reset();
    gMarkers.clear();
    Harness h;
    h.resolver.shape = [](StreamSet &set) { longFixture(set, false); };
    player::StallConfig config = fastStall();
    config.maxEpisodes = 1;
    h.session.setStallConfig(config);
    h.session.open(QStringLiteral("FIXTURE0001"), longHints());
    CHECK(waitFor([&]() { return h.ready() && h.session.playing() && h.session.position() > 500; }, 15000));
    h.session.player()->pause();  // the first stall: recovered by the seek
    CHECK(waitFor([&]() { return markerWith("stall-recovered", "step=1"); }, 12000));
    CHECK(waitFor([&]() { return h.session.playing(); }, 5000));
    spinFor(600);
    CHECK(h.session.errorText().isEmpty() && !h.session.retryAvailable());
    h.session.player()->pause();  // the second: over the budget
    const double stoodAt = h.session.position();
    CHECK(waitFor([&]() { return h.session.state() == PlaybackSession::Failed; }, 12000));
    printStallMarkers();
    CHECK(h.session.errorText() == QLatin1String("Playback stopped responding."));
    CHECK(h.session.retryAvailable() && h.session.errorSettingKey().isEmpty() && !h.session.recovering());
    CHECK(markerWith("stall-failed", "reason=budget"));
    CHECK(diag(h, "stalls") == 2 && diag(h, "stallRecoveries") == 1);
    CHECK(h.session.player()->state() == player::VideoPlayer::Idle);
    CHECK(h.resolver.refused.isEmpty() && h.resolver.resolves() == 1);
    h.session.retry();
    CHECK(waitFor([&]() { return h.ready() && h.session.playing(); }, 15000));
    CHECK(h.resolver.resolves() == 2 && h.session.errorText().isEmpty());
    CHECK(h.session.position() + 1500 >= stoodAt);  // from where it stood
    CHECK(waitFor([&]() { return h.session.position() >= stoodAt + 500; }, 8000));
    h.session.stop();
}

// (d) No false stall: a user pause, and the background with the picture frozen (PLY-09) while the
// audio plays on, both far longer than the thresholds.
static void ply18_noFalseStallPausedOrInBackground() {
    gServer->reset();
    gMarkers.clear();
    Harness h;
    h.resolver.shape = [](StreamSet &set) { longFixture(set, false); };
    h.session.setStallConfig(fastStall());
    h.session.open(QStringLiteral("FIXTURE0001"), longHints());
    CHECK(waitFor([&]() { return h.ready() && h.session.playing() && h.session.position() > 300; }, 15000));
    h.session.pause();
    CHECK(waitFor([&]() { return !h.session.playing(); }, 3000));
    spinFor(4000);
    CHECK(diag(h, "stalls") == 0 && !h.session.recovering() && h.session.state() == PlaybackSession::Ready);
    h.session.play();
    CHECK(waitFor([&]() { return h.session.playing(); }, 5000));
    h.session.setAppActive(false);
    CHECK(waitFor([&]() { return h.session.player()->frozen(); }, 3000));
    const double hiddenAt = h.session.position();
    spinFor(4000);
    CHECK(h.session.player()->frozen() && h.session.position() >= hiddenAt + 3000);  // the audio went on
    CHECK(diag(h, "stalls") == 0 && !h.session.recovering());
    CHECK(markerIndex(QStringLiteral("stall-")) < 0);
    h.session.setAppActive(true);
    CHECK(h.ready() && h.session.playing() && h.resolver.resolves() == 1);
    CHECK(h.fetcher.opened.count(QStringLiteral("247")) == 1);
    h.session.stop();
}

int main(int argc, char **argv) {
    std::setvbuf(stdout, nullptr, _IOLBF, 0);
    player::limitMallocArenas();  // as the app does, before any thread exists (the memory checks below)
    qputenv("SAILTUBE_TIMING", "1");  // the NFR-PERF-02 markers are checked
    qInstallMessageHandler(collectMessages);
    unsetenv("http_proxy");
    unsetenv("HTTP_PROXY");
    unsetenv("https_proxy");
    unsetenv("all_proxy");
    const QByteArray tmp = qgetenv("TMPDIR").isEmpty() ? QByteArray("/tmp") : qgetenv("TMPDIR");
    const QString root = QString::fromLocal8Bit(tmp) + QStringLiteral("/sailtube-session-gst-%1").arg(getpid());
    qputenv("XDG_CONFIG_HOME", QFile::encodeName(root + QStringLiteral("/config")));
    qputenv("XDG_CACHE_HOME", QFile::encodeName(root + QStringLiteral("/cache")));
    QCoreApplication app(argc, argv);
    app.setOrganizationName(QStringLiteral("org.nordilabs.sailtube.test"));
    app.setApplicationName(QStringLiteral("SessionTest"));
    QString err;
    if (!player::GstEngine::initialize(&err)) {
        std::printf("session gst tests: SKIPPED (gst_init failed: %s)\n", qPrintable(err));
        return 0;
    }
    const std::set<std::string> have = player::GstEngine::availableElements();
    const char *const needed[] = {"uridecodebin", "souphttpsrc", "matroskademux", "vp9dec", "opusdec", "appsink", "fakesink"};
    for (size_t i = 0; i < sizeof(needed) / sizeof(needed[0]); ++i)
        if (!have.count(needed[i]) && !gst_element_factory_find(needed[i])) {
            std::printf("session gst tests: SKIPPED (missing %s)\n", needed[i]);
            return 0;
        }
    gDir = root + QStringLiteral("/media");
    QDir().mkpath(gDir);
    if (!makeFixtures()) {
        std::printf("FAIL fixture generation\n");
        return 1;
    }
    HttpServer server;
    if (!server.start()) {
        std::printf("FAIL http server\n");
        return 1;
    }
    gServer = &server;
    const char *only = std::getenv("SESSION_TEST_ONLY");
    // SESSION_TEST_REPEAT=<n> (with SESSION_TEST_ONLY): the selected tests, in their order, n times
    // in one process (looking into an intermittent failure in the context of its neighbours).
    const int repeat = only && std::getenv("SESSION_TEST_REPEAT") ? std::max(1, std::atoi(std::getenv("SESSION_TEST_REPEAT"))) : 1;
#define RUN(t) do { if (!only || std::strstr(#t, only)) { std::printf("%s\n", #t); t(); } } while (0)
    for (int round = 0; round < repeat; ++round) {
    RUN(res21_warmUpOnceAfterFirstSwap);
    RUN(ply01_openDualPlanFirstFrame);
    RUN(res07_byteZeroRefusalReportsAndFallsBack);
    RUN(res06_openRefusalNextCandidate);
    RUN(sel07_switchesFromSameSet);
    RUN(sel08_listenOnlyFetchesAudioOnly);
    RUN(sel08_audioOnlyHintBeforeFirstResolve);
    RUN(res09_finalErrorNoRetry);
    RUN(res09_retryableClassesAndTexts);
    RUN(res19_upcomingShowsStartAndHolds);
    RUN(ply20_sessionFeedsProbeVerdict);
    RUN(res07_refusalUsesHttpStatus);
    RUN(e12_pressThenOpenReusesSpeculation);
    RUN(e12_latestPressWins);
    RUN(e12_pressCancelled);
    RUN(res11_rowsVisible);
    RUN(e12_noSpeculationInBackground);
    RUN(sb02_segmentsFromRecord);
    RUN(ply26_downloadedVideoPlaysLocally);
    RUN(ply26_queueNextPlaysDownloadLocally);
    RUN(ply26_localResumeAfterOpen);
    RUN(ply26_localMuxedFirstFrameMarker);
    RUN(ply26_preferStreamAndMissingFileStream);
    RUN(res14_poProviderOnlyWhenReady);
    RUN(ply18_errorSettingKey);
    RUN(ply25_queueReadOut);
    RUN(perf02_resolveDispatchedBeforeStop);
    RUN(cover_onePipelineWhileCoverVideoOn);
    RUN(cover_videoAspectBeforeFirstFrame);
    RUN(ply26_streamInsteadSwitchesSeamlessly);
    RUN(ply26_streamInsteadFailureStaysLocal);
    RUN(ply26_streamInsteadNoOpAndPaused);
    RUN(shorts_likelyNextThenOpenReusesPreload);
    RUN(shorts_openJoinsPreloadInFlight);
    RUN(shorts_likelyNextBounds);
    RUN(shorts_loopRestartsWithoutEnded);
    RUN(shorts_contextSwitchSameShortKeepsPlaying);
    RUN(shorts_openHoldAndBackground);
    RUN(shorts_aheadPreparesTwo);
    RUN(shorts_surfaceContractAndFailure);
    RUN(shorts_swipeSequenceStaysBounded);
    RUN(seekbar_bufferedPosition);
    RUN(sel05_audioTrackSwitchFromSameSet);
    RUN(ovl_holdSpeedLoopSegments);
    RUN(ovl_sleepTimer);
    RUN(ovl_scrubPreview);
    RUN(set01_settingsReachThePlayer);
    RUN(sel06_captionCueAtPosition);
    RUN(ply18_stallServerStopsMidStreamReopenRecovers);
    RUN(ply18_stallReopenRefusedFreshResolveRecovers);
    RUN(ply18_stallStuckPipelineRecoveredBySeek);
    RUN(ply18_stallBudgetExhaustedShowsRetry);
    RUN(ply18_noFalseStallPausedOrInBackground);
    }
#undef RUN
    QDir(root).removeRecursively();
    std::printf("session gst tests: %d checks, %d failure%s\n", checks, failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
