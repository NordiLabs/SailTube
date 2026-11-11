// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// PLT-09 host test: /tmp is RAM. With TMPDIR prepared by prepareFlashTempDir (emptied at start,
// owner-only, exported before GLib caches it), the player plays dual mp4 (the download-buffer mode:
// uridecodebin download=true, a downloadbuffer temp file, which by default would go to
// g_get_user_cache_dir(); the engine moves it into TMPDIR) and dual WebM (RAM queues) over HTTP, and no file
// this process holds open is in /tmp, /dev/shm or /run; the mp4 download buffer is under TMPDIR,
// and TMPDIR is empty again after stop. The plans never point a spool at a RAM tree.
// The 2-hour device check ("/tmp usage by the process stays under 1 MiB") remains a §6 step.
// Build: tests/tmpdir/run.sh
#include "Core/Player/Domain/PipelinePlan.h"
#include "Core/Player/Domain/TempDirPolicy.h"
#include "Core/Player/Infrastructure/GstEngine.h"
#include "Core/Player/Infrastructure/TempDirSetup.h"
#include "Core/Player/Infrastructure/VideoPlayer.h"

#include <gst/gst.h>

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QSet>
#include <QTcpServer>
#include <QTcpSocket>
#include <QThread>

#include <cstdio>
#include <cstdlib>
#include <functional>
#include <memory>
#include <sys/stat.h>
#include <unistd.h>

using namespace nordtile::player;

static int checks = 0, failures = 0;
#define CHECK(c) do { ++checks; if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

static QString gMedia;  // fixtures (flash)

// Every path this process opened (/proc/self/fd), "(deleted)" suffix kept. Standard input, output
// and error are the caller's (a log file in /tmp, say), not something the process wrote there.
static QStringList openPaths() {
    QStringList out;
    QDir fds(QStringLiteral("/proc/self/fd"));
    const QStringList names = fds.entryList(QDir::Files | QDir::System | QDir::NoDotAndDotDot);
    for (const QString &n : names) {
        if (n.toInt() <= 2) continue;
        char buffer[4096];
        const QByteArray link = QFile::encodeName(fds.absoluteFilePath(n));
        const ssize_t len = readlink(link.constData(), buffer, sizeof(buffer) - 1);
        if (len <= 0) continue;
        buffer[len] = 0;
        if (buffer[0] == '/') out << QString::fromLocal8Bit(buffer);
    }
    return out;
}

static bool inRamTree(const QString &path) {
    QString p = path;
    if (p.endsWith(QLatin1String(" (deleted)"))) p.chop(10);
    // /tmp, /run and /dev/shm hold RAM (TempDirPolicy); memfd and sockets are not paths there.
    return !isAllowedSpoolLocation(p.toStdString());
}

static bool waitFor(const std::function<bool()> &pred, int timeoutMs, const std::function<void()> &each = nullptr) {
    QElapsedTimer t;
    t.start();
    while (!pred()) {
        if (t.elapsed() > timeoutMs) return false;
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        if (each) each();
        QThread::msleep(5);
    }
    return true;
}

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

// GET /<file> with Range support, from gMedia.
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
    QString url(const QString &file) const { return QStringLiteral("http://127.0.0.1:%1/%2").arg(server_.serverPort()).arg(file); }

private:
    void respond(QTcpSocket *s, const QByteArray &request) {
        const QList<QByteArray> lines = request.split('\n');
        const QString path = QString::fromLatin1(lines.value(0).trimmed().split(' ').value(1));
        QFile f(gMedia + path);
        if (path.contains(QLatin1String("..")) || !f.open(QIODevice::ReadOnly)) {
            s->write("HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
            s->disconnectFromHost();
            return;
        }
        const QByteArray data = f.readAll();
        qint64 from = 0, to = data.size() - 1;
        bool partial = false;
        for (int i = 1; i < lines.size(); ++i) {
            const QByteArray l = lines[i].trimmed();
            if (!l.toLower().startsWith("range: bytes=")) continue;
            const QByteArray spec = l.mid(13);
            const int dash = spec.indexOf('-');
            from = spec.left(dash).toLongLong();
            const QByteArray end = spec.mid(dash + 1);
            if (!end.isEmpty()) to = qMin<qint64>(end.toLongLong(), data.size() - 1);
            partial = true;
        }
        QByteArray head = partial ? QByteArray("HTTP/1.1 206 Partial Content\r\nContent-Range: bytes ") + QByteArray::number(from) +
                                        "-" + QByteArray::number(to) + "/" + QByteArray::number(data.size()) + "\r\n"
                                  : QByteArray("HTTP/1.1 200 OK\r\n");
        head += "Accept-Ranges: bytes\r\nContent-Type: application/octet-stream\r\nConnection: close\r\nContent-Length: " +
                QByteArray::number(to - from + 1) + "\r\n\r\n";
        s->write(head);
        s->write(data.mid(int(from), int(to - from + 1)));
        s->disconnectFromHost();
    }
    QTcpServer server_;
};

// ---- Tests -----------------------------------------------------------------------------------

static QString gTmp;  // the prepared TMPDIR

// PLT-09: emptied at start-up, owner-only, exported; never a RAM tree.
static void plt09_prepareEmptiesAndExports(const QString &cache) {
    const QString dir = cache + QStringLiteral("/tmp");
    QDir().mkpath(dir + QStringLiteral("/stale-spool"));
    QFile stale(dir + QStringLiteral("/old-download.part"));
    CHECK(stale.open(QIODevice::WriteOnly) && stale.write("x") == 1);
    stale.close();
    int removed = -1;
    gTmp = prepareFlashTempDir(cache, &removed);
    CHECK(gTmp == dir);
    CHECK(removed == 2);
    CHECK(QDir(gTmp).entryList(QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot).isEmpty());
    CHECK(qgetenv("TMPDIR") == QFile::encodeName(gTmp));
    struct stat st;
    CHECK(stat(QFile::encodeName(gTmp).constData(), &st) == 0 && (st.st_mode & 0777) == 0700);
    // A cache in a RAM tree is refused and TMPDIR stays as it was.
    CHECK(prepareFlashTempDir(QStringLiteral("/tmp/sailtube-cache")).isEmpty());
    CHECK(prepareFlashTempDir(QStringLiteral("/dev/shm/x")).isEmpty());
    CHECK(qgetenv("TMPDIR") == QFile::encodeName(gTmp));
}

// PLT-09: no plan points a spool at a RAM tree; only split network mp4 (dual, listen-only) uses the
// download buffer (a file the engine keeps in TMPDIR); every other mode buffers in RAM queues.
static void plt09_plansKeepSpoolsOffRam() {
    const PlayMode modes[] = {PlayMode::Dual, PlayMode::Muxed, PlayMode::Hls, PlayMode::AudioOnly};
    const Container containers[] = {Container::IsoBmff, Container::Matroska};
    for (PlayMode mode : modes) {
        for (Container c : containers) {
            PlanInput in;
            in.mode = mode;
            in.available = GstEngine::availableElements();
            SourceSpec s;
            s.uri = "http://127.0.0.1:1/x";
            s.container = c;
            in.video = in.audio = in.muxed = s;
            const PipelinePlan plan = buildPipelinePlan(in);
            for (const BranchPlan &b : plan.branches) {
                const bool download = b.source.property("download") == "true";
                // The download buffer only for split network mp4 (PLY-04); muxed, HLS and WebM never.
                const bool split = mode == PlayMode::Dual || mode == PlayMode::AudioOnly;
                CHECK(download == (split && c == Container::IsoBmff));
                for (const PropertySetting &p : b.source.properties)
                    CHECK(p.value.compare(0, 4, "/tmp") != 0 && p.name != "temp-template" && p.name != "temp-location");
                CHECK(b.source.property("ring-buffer-max-size").empty());
            }
        }
    }
}

static void playAndWatch(HttpServer &server, const char *video, const char *audio, Container c, bool expectSpool) {
    VideoPlayer player;
    int firstFrames = 0, failed = 0;
    QObject::connect(&player, &VideoPlayer::firstFrame, [&]() { ++firstFrames; });
    QObject::connect(&player, &VideoPlayer::failed, [&](const QString &r, bool, int, int) {
        ++failed;
        std::printf("  player failed: %s\n", qPrintable(r));
    });
    PlayerSources p;
    p.mode = PlayMode::Dual;
    p.videoId = "fixture";
    p.video.uri = server.url(QLatin1String(video)).toStdString();
    p.video.container = c;
    p.audio.uri = server.url(QLatin1String(audio)).toStdString();
    p.audio.container = c;
    p.audioSink.factory = "fakesink";
    PropertySetting sync;
    sync.name = "sync";
    sync.value = "true";
    p.audioSink.properties.push_back(sync);
    p.videoSink = p.audioSink;
    p.videoSink.factory = "fakesink";
    player.load(p);
    QSet<QString> ram, spool;
    const auto scan = [&]() {
        for (const QString &path : openPaths()) {
            if (inRamTree(path)) ram << path;
            if (path.startsWith(gTmp + QLatin1Char('/'))) spool << path;
        }
    };
    CHECK(waitFor([&]() { return firstFrames > 0 || failed > 0; }, 10000, scan));
    CHECK(waitFor([&]() { return player.position() >= 1500 || failed > 0; }, 10000, scan));
    CHECK(failed == 0);
    scan();
    for (const QString &path : ram) std::printf("  open in a RAM tree: %s\n", qPrintable(path));
    CHECK(ram.isEmpty());
    if (expectSpool) CHECK(!spool.isEmpty());  // the download buffer went to TMPDIR, not /tmp
    for (const QString &path : spool) std::printf("  spool under TMPDIR: %s\n", qPrintable(path));
    if (getenv("TMPDIR_TEST_DEBUG")) for (const QString &path : openPaths()) std::printf("  open: %s\n", qPrintable(path));
    player.stop();
    // The old pipeline reaches NULL on the engine's teardown worker (asynchronous since the
    // start-up work); the spool file goes with it a few ms later.
    QElapsedTimer waited;
    waited.start();
    while (!QDir(gTmp).entryList(QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot).isEmpty() && waited.elapsed() < 2000) {
        QCoreApplication::processEvents();
        QThread::msleep(10);
    }
    CHECK(QDir(gTmp).entryList(QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot).isEmpty());
}

int main(int argc, char **argv) {
    std::setvbuf(stdout, nullptr, _IOLBF, 0);
    if (!QDir(QStringLiteral("/proc/self/fd")).exists()) {
        std::printf("tmpdir tests: SKIPPED (no /proc/self/fd)\n");
        return 0;
    }
    // A flash-backed private root (not /tmp): the run's TMPDIR when that is on flash, else ~/.cache.
    QString base = QString::fromLocal8Bit(qgetenv("TMPDIR"));
    if (base.isEmpty() || !isAllowedSpoolLocation(QDir(base).absolutePath().toStdString()))
        base = QDir::homePath() + QStringLiteral("/.cache");
    const QString root = base + QStringLiteral("/sailtube-plt09-%1").arg(getpid());
    gMedia = root + QStringLiteral("/media");
    QDir().mkpath(gMedia);
    // Before GLib or GStreamer: g_get_tmp_dir() is cached on first use.
    plt09_prepareEmptiesAndExports(root + QStringLiteral("/cache"));
    QCoreApplication app(argc, argv);
    QString err;
    if (!GstEngine::initialize(&err)) {
        std::printf("tmpdir tests: SKIPPED (gst_init failed: %s)\n", qPrintable(err));
        QDir(root).removeRecursively();
        return 0;
    }
    CHECK(QString::fromLocal8Bit(g_get_tmp_dir()) == gTmp);
    const char *const needed[] = {"videotestsrc", "audiotestsrc", "vp9enc", "opusenc", "mp4mux", "webmmux", "qtdemux",
                                  "matroskademux", "vp9dec", "opusdec", "souphttpsrc", "queue2", "fakesink"};
    for (const char *n : needed)
        if (!gst_element_factory_find(n)) {
            std::printf("tmpdir tests: SKIPPED (missing %s)\n", n);
            QDir(root).removeRecursively();
            return 0;
        }
    const QString d = gMedia + QLatin1Char('/');
    const QString v = QStringLiteral("videotestsrc num-buffers=150 ! video/x-raw,width=320,height=180,framerate=30/1 ! "
                                     "vp9enc deadline=1 cpu-used=8 keyframe-max-dist=30 ! ");
    const QString a = QStringLiteral("audiotestsrc num-buffers=250 samplesperbuffer=960 ! audio/x-raw,rate=48000,channels=2 ! opusenc ! ");
    if (!runPipeline(v + "vp9parse ! mp4mux faststart=true ! filesink location=" + d + "v.mp4") ||
        !runPipeline(a + "mp4mux faststart=true ! filesink location=" + d + "a.mp4") ||
        !runPipeline(v + "webmmux ! filesink location=" + d + "v.webm") ||
        !runPipeline(a + "webmmux ! filesink location=" + d + "a.webm")) {
        std::printf("FAIL fixture generation\n");
        return 1;
    }
    HttpServer server;
    if (!server.start()) {
        std::printf("FAIL http server\n");
        return 1;
    }
    unsetenv("http_proxy");
    unsetenv("HTTP_PROXY");
    plt09_plansKeepSpoolsOffRam();
    std::printf("plt09_dualMp4DownloadBufferUnderTmpdir\n");
    playAndWatch(server, "v.mp4", "a.mp4", Container::IsoBmff, true);
    std::printf("plt09_dualWebmInRam\n");
    playAndWatch(server, "v.webm", "a.webm", Container::Matroska, false);
    QDir(root).removeRecursively();
    std::printf("tmpdir tests: %d checks, %d failure%s\n", checks, failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
