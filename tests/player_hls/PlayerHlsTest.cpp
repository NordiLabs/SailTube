// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// RES-19 / PLY-01 host test: VideoPlayer in Hls mode end to end on local HLS fixtures (no network
// beyond loopback). The container's GStreamer has no MPEG-TS muxer (and no H.264 / AAC), so the
// fixtures are fragmented-MP4 HLS (EXT-X-MAP init segment + .m4s media segments, the CMAF form
// hlsdemux2 reads), VP9 + Opus in one muxed rendition, cut from mp4mux's fragmented output at its
// moof boxes (one 1 s segment = the video and the audio fragment). Cases: a VOD playlist, a live-style playlist (no ENDLIST, liveHls on) and a master
// playlist naming the muxed variant. Each must reach Playing with audio running, the video's first
// frame in the mailbox and both tracks in the one muxed branch (A/V joined). Served by a tiny
// in-process HTTP server. Build: tests/player_hls/run.sh
#include "Core/Player/Infrastructure/GstEngine.h"
#include "Core/Player/Infrastructure/VideoPlayer.h"

#include <gst/gst.h>

#include <QByteArray>
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QHash>
#include <QTcpServer>
#include <QTcpSocket>
#include <QThread>

#include <cstdio>
#include <cstdlib>
#include <functional>
#include <memory>
#include <vector>

using namespace nordtile::player;

static int checks = 0;
static int failures = 0;
#define CHECK_MSG(c, ...) do { ++checks; if (!(c)) { std::printf("FAIL %s:%d: %s -- ", __FILE__, __LINE__, #c); std::printf(__VA_ARGS__); std::printf("\n"); ++failures; } } while (0)
#define CHECK(c) CHECK_MSG(c, "%s", "")

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

static void spin(int ms) { waitFor([]() { return false; }, ms); }

static QString gDir;

// ---- Fixtures -------------------------------------------------------------------------------

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
    const bool ok = m && GST_MESSAGE_TYPE(m) == GST_MESSAGE_EOS;
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

static const int kSeconds = 8;
static const int kFps = 30;

static quint32 rd32(const QByteArray &d, qint64 at) {
    const unsigned char *p = reinterpret_cast<const unsigned char *>(d.constData()) + at;
    return (quint32(p[0]) << 24) | (quint32(p[1]) << 16) | (quint32(p[2]) << 8) | quint32(p[3]);
}

struct Box {
    qint64 offset = 0, size = 0;
    QByteArray type;
};

static std::vector<Box> topBoxes(const QByteArray &d) {
    std::vector<Box> out;
    qint64 at = 0;
    while (at + 8 <= d.size()) {
        Box b;
        b.offset = at;
        b.size = rd32(d, at);
        b.type = d.mid(int(at + 4), 4);
        if (b.size == 1 && at + 16 <= d.size())
            b.size = qint64((quint64(rd32(d, at + 8)) << 32) | rd32(d, at + 12));
        if (b.size == 0) b.size = d.size() - at;
        if (b.size < 8 || at + b.size > d.size()) break;
        out.push_back(b);
        at += b.size;
    }
    return out;
}

static bool writeFile(const QString &path, const QByteArray &data) {
    QFile f(path);
    return f.open(QIODevice::WriteOnly) && f.write(data) == data.size();
}

struct HlsFixture {
    int tracks = 0;
    int segments = 0;
    double segmentSeconds = 0;
    QString layout;  // the top-level boxes, for the log
};

// mp4mux's fragmented output (ftyp, moov with mvex, then moof + mdat pairs) cut into an init
// segment (everything before the first moof) and one media segment per fragment (both tracks);
// the media playlists list them (VOD with ENDLIST; a live window without it) and a master playlist
// names the muxed variant with both codecs.
static bool makeHlsFixtures(HlsFixture *info) {
    const QString d = gDir + "/";
    // 1 s keyframe interval and 1 s fragments: every segment starts on a keyframe.
    const bool encoded = runPipeline(
        QStringLiteral("mp4mux fragment-duration=1000 streamable=true name=m ! filesink location=") + d + "frag.mp4 "
        "videotestsrc num-buffers=" + QString::number(kSeconds * kFps) + " pattern=ball ! video/x-raw,width=320,height=180,"
        "framerate=" + QString::number(kFps) + "/1 ! vp9enc deadline=1 cpu-used=8 keyframe-max-dist=" + QString::number(kFps) +
        " ! vp9parse ! queue ! m. "
        "audiotestsrc num-buffers=" + QString::number(kSeconds * 50) + " samplesperbuffer=960 wave=sine ! "
        "audio/x-raw,rate=48000,channels=2 ! opusenc ! queue ! m.");
    if (!encoded) return false;
    QFile f(d + "frag.mp4");
    if (!f.open(QIODevice::ReadOnly)) return false;
    const QByteArray data = f.readAll();
    const std::vector<Box> boxes = topBoxes(data);
    // mp4mux writes one moof + mdat pair per track and fragment: a media segment takes one pair per
    // track (trak boxes in the moov), so every segment carries audio and video, as a muxed rendition does.
    int tracks = 0;
    for (const Box &b : boxes) {
        if (b.type != "moov") continue;
        const std::vector<Box> kids = topBoxes(data.mid(int(b.offset + 8), int(b.size - 8)));
        for (const Box &k : kids) tracks += k.type == "trak";
    }
    info->tracks = tracks;
    qint64 firstMoof = -1;
    std::vector<QByteArray> segments;
    QByteArray current;
    int pairs = 0;
    for (std::size_t i = 0; i < boxes.size(); ++i) {
        info->layout += QString::fromLatin1(boxes[i].type) + QLatin1Char(' ');
        if (boxes[i].type != "moof") continue;
        if (firstMoof < 0) firstMoof = boxes[i].offset;
        if (i + 1 >= boxes.size() || boxes[i + 1].type != "mdat") continue;
        current += data.mid(int(boxes[i].offset), int(boxes[i].size + boxes[i + 1].size));
        if (++pairs == tracks) {
            segments.push_back(current);
            current.clear();
            pairs = 0;
        }
    }
    if (!current.isEmpty()) segments.push_back(current);
    if (tracks != 2 || firstMoof <= 0 || segments.size() < 3) return false;
    if (!writeFile(d + "init.mp4", data.left(int(firstMoof)))) return false;
    info->segments = int(segments.size());
    info->segmentSeconds = double(kSeconds) / double(segments.size());
    const int target = int(info->segmentSeconds + 0.999);
    QByteArray vod = "#EXTM3U\n#EXT-X-VERSION:7\n#EXT-X-TARGETDURATION:" + QByteArray::number(target) +
                     "\n#EXT-X-MEDIA-SEQUENCE:0\n#EXT-X-PLAYLIST-TYPE:VOD\n#EXT-X-INDEPENDENT-SEGMENTS\n#EXT-X-MAP:URI=\"init.mp4\"\n";
    QByteArray live = "#EXTM3U\n#EXT-X-VERSION:7\n#EXT-X-TARGETDURATION:" + QByteArray::number(target) +
                      "\n#EXT-X-MEDIA-SEQUENCE:0\n#EXT-X-INDEPENDENT-SEGMENTS\n#EXT-X-MAP:URI=\"init.mp4\"\n";
    for (std::size_t i = 0; i < segments.size(); ++i) {
        const QByteArray name = "seg" + QByteArray::number(int(i)) + ".m4s";
        if (!writeFile(d + QString::fromLatin1(name), segments[i])) return false;
        const QByteArray entry = "#EXTINF:" + QByteArray::number(info->segmentSeconds, 'f', 3) + ",\n" + name + "\n";
        vod += entry;
        live += entry;
    }
    vod += "#EXT-X-ENDLIST\n";
    const QByteArray master = "#EXTM3U\n#EXT-X-VERSION:7\n#EXT-X-INDEPENDENT-SEGMENTS\n"
                              "#EXT-X-STREAM-INF:BANDWIDTH=600000,RESOLUTION=320x180,CODECS=\"vp09.00.10.08,opus\"\nvod.m3u8\n";
    return writeFile(d + "vod.m3u8", vod) && writeFile(d + "live.m3u8", live) && writeFile(d + "master.m3u8", master);
}

// ---- Tiny HTTP server: GET /<file> from gDir (200, Content-Length, Connection: close) ----------

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
    int requests(const QString &suffix) const {
        int n = 0;
        for (auto it = requests_.constBegin(); it != requests_.constEnd(); ++it) if (it.key().endsWith(suffix)) n += it.value();
        return n;
    }
    void reset() { requests_.clear(); }

private:
    void respond(QTcpSocket *s, const QByteArray &request) {
        const QList<QByteArray> first = request.split('\n').value(0).trimmed().split(' ');
        const QString path = QString::fromLatin1(first.value(1)).mid(1);
        requests_[path] += 1;
        QFile f(gDir + QLatin1Char('/') + path);
        if (path.contains(QLatin1String("..")) || !f.open(QIODevice::ReadOnly)) {
            s->write("HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
            s->disconnectFromHost();
            return;
        }
        const QByteArray data = f.readAll();
        const QByteArray type = path.endsWith(QLatin1String(".m3u8")) ? "application/vnd.apple.mpegurl"
                              : path.endsWith(QLatin1String(".m4s")) ? "video/iso.segment" : "video/mp4";
        s->write("HTTP/1.1 200 OK\r\nContent-Type: " + type + "\r\nConnection: close\r\nContent-Length: " +
                 QByteArray::number(data.size()) + "\r\n\r\n");
        s->write(data);
        s->disconnectFromHost();
    }
    QTcpServer server_;
    QHash<QString, int> requests_;
};

static HttpServer *gServer = nullptr;

// ---- Cases ----------------------------------------------------------------------------------

struct Harness {
    VideoPlayer player;
    int firstFrames = 0;
    int failures = 0;
    QString lastError;
    qint64 firstFrameMs = -1;
    QElapsedTimer clock;
    Harness() {
        clock.start();
        QObject::connect(&player, &VideoPlayer::firstFrame, [this]() {
            if (firstFrames++ == 0) firstFrameMs = clock.elapsed();
        });
        QObject::connect(&player, &VideoPlayer::failed, [this](const QString &r, bool, int, int) {
            ++failures;
            lastError = r;
            std::printf("  player failed: %s\n", qPrintable(r));
        });
    }
};

static PlayerSources hlsSources(const QString &playlist, bool live) {
    PlayerSources p;
    p.mode = PlayMode::Hls;
    p.videoId = "hlsfixture";
    p.liveHls = live;
    p.muxed.uri = gServer->url(playlist).toStdString();
    p.muxed.container = Container::Unknown;
    p.muxed.userAgent = "SailTube-test";
    p.audioSink.factory = "fakesink";
    PropertySetting sync;
    sync.name = "sync";
    sync.value = "true";
    p.audioSink.properties.push_back(sync);
    return p;
}

static qint64 position(Harness &h) {
    qint64 ms = -1;
    h.player.sinkPosition(Branch::Muxed, &ms);
    return ms;
}

// RES-19 / PLY-01: HLS through VideoPlayer: one muxed branch with the audio and the video routes,
// Playing, audio position advancing (the audio sink syncs), the first video frame in the mailbox at
// the fixture's size, and frames rendered (A/V joined in one pipeline).
static void res19_hlsPlaysEndToEnd(const char *name, const QString &playlist, bool live) {
    gServer->reset();
    Harness h;
    h.player.load(hlsSources(playlist, live));
    const bool playing = waitFor([&]() { return h.player.state() == VideoPlayer::Playing || h.failures > 0; }, 20000) &&
                         h.failures == 0;
    CHECK_MSG(playing, "%s did not reach Playing (%s)", name, qPrintable(h.lastError));
    if (!playing) return;
    CHECK_MSG(h.player.plan().mode == PlayMode::Hls, "%s plan mode", name);
    CHECK_MSG(h.player.plan().branches.size() == 1, "%s branches %d (HLS is one muxed branch)", name,
              int(h.player.plan().branches.size()));
    CHECK_MSG(waitFor([&]() { return h.firstFrames > 0; }, 10000), "%s no first frame", name);
    const qint64 p0 = position(h);
    spin(1500);
    const qint64 p1 = position(h);
    CHECK_MSG(p0 >= 0 && p1 - p0 >= 900, "%s position %lld -> %lld", name, (long long)p0, (long long)p1);
    CHECK_MSG(h.player.hasVideo(), "%s hasVideo", name);
    const VideoFrame f = h.player.frameMailbox()->latest();
    CHECK_MSG(f.valid() && f.width == 320 && f.height == 180, "%s mailbox frame %dx%d", name, f.width, f.height);
    const EngineStats es = h.player.diagnostics().engine;
    CHECK_MSG(es.framesRendered + es.framesDropped > 15 && es.framesRendered >= 5, "%s frames rendered %lld, dropped %lld",
              name, (long long)es.framesRendered, (long long)es.framesDropped);
    CHECK_MSG(gServer->requests(QStringLiteral("init.mp4")) >= 1 && gServer->requests(QStringLiteral(".m4s")) >= 1,
              "%s requests: init %d, segments %d", name, gServer->requests(QStringLiteral("init.mp4")),
              gServer->requests(QStringLiteral(".m4s")));
    std::printf("  %s: first frame %lld ms, position %lld -> %lld ms, frames %lld\n", name, (long long)h.firstFrameMs,
                (long long)p0, (long long)p1, (long long)es.framesRendered);
    h.player.stop();
}

int main(int argc, char **argv) {
    std::setvbuf(stdout, nullptr, _IOLBF, 0);
    unsetenv("http_proxy");
    unsetenv("HTTP_PROXY");
    unsetenv("https_proxy");
    unsetenv("all_proxy");
    QCoreApplication app(argc, argv);
    QString err;
    if (!GstEngine::initialize(&err)) {
        std::printf("player hls tests: SKIPPED (gst_init failed: %s)\n", qPrintable(err));
        return 0;
    }
    const std::set<std::string> have = GstEngine::availableElements();
    if (!have.count("hlsdemux2") && !have.count("hlsdemux")) {
        std::printf("player hls tests: SKIPPED (no hlsdemux2 / hlsdemux)\n");
        return 0;
    }
    const char *const needed[] = {"mp4mux", "vp9enc", "vp9parse", "opusenc", "videotestsrc", "audiotestsrc",
                                  "uridecodebin", "qtdemux", "souphttpsrc", "appsink", "fakesink"};
    for (const char *e : needed) {
        if (!gst_element_factory_find(e)) {
            std::printf("player hls tests: SKIPPED (missing %s)\n", e);
            return 0;
        }
    }
    const QByteArray tmp = qgetenv("TMPDIR").isEmpty() ? QByteArray("/tmp") : qgetenv("TMPDIR");
    gDir = QString::fromLocal8Bit(tmp) + QStringLiteral("/sailtube-player-hls-%1").arg(QCoreApplication::applicationPid());
    QDir().mkpath(gDir);
    HlsFixture fixture;
    if (!makeHlsFixtures(&fixture)) {
        std::printf("FAIL HLS fixture generation (boxes: %s)\n", qPrintable(fixture.layout));
        QDir(gDir).removeRecursively();
        return 1;
    }
    std::printf("hls fixtures: %d tracks, %d segments of %.3f s (boxes: %s)\n", fixture.tracks, fixture.segments,
                fixture.segmentSeconds, qPrintable(fixture.layout.left(80)));
    HttpServer server;
    if (!server.start()) {
        std::printf("FAIL http server\n");
        return 1;
    }
    gServer = &server;
    res19_hlsPlaysEndToEnd("vod", QStringLiteral("vod.m3u8"), false);
    res19_hlsPlaysEndToEnd("live window", QStringLiteral("live.m3u8"), true);
    res19_hlsPlaysEndToEnd("master", QStringLiteral("master.m3u8"), false);
    QDir(gDir).removeRecursively();
    std::printf("player hls tests: %d checks, %d failure%s\n", checks, failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
