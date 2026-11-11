// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// DL-03 / E6 host side: GstRemuxer against the container's GStreamer. Fixtures
// are encoded at run time by GStreamer itself; a container case whose encoders
// are absent prints SKIPPED for that case only. Build: tests/remux/run.sh
#include "Core/Downloads/Infrastructure/GstRemuxer.h"
#include "Core/Downloads/Domain/Mp4Layout.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QLibrary>
#include <QMutex>
#include <QStringList>
#include <QThread>
#include <cstdio>
#include <cstring>

using namespace nordtile::video::downloads;

static int failures = 0;
static int skipped = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

namespace {

// Test-side mirror of the public GStreamer C ABI (no headers in the build).
typedef int gboolean;
typedef unsigned long long GstClockTime;
struct GError { unsigned domain; int code; char *message; };
typedef void (*GCallback)();

struct TestGst {
    gboolean (*initCheck)(int *, char ***, GError **);
    void (*version)(unsigned *, unsigned *, unsigned *, unsigned *);
    void *(*factoryFind)(const char *);
    void *(*featureLoad)(void *);
    void (*objectUnref)(void *);
    void *(*parseLaunch)(const char *, GError **);
    void *(*make)(const char *, const char *);
    void *(*pipelineNew)(const char *);
    gboolean (*binAdd)(void *, void *);
    gboolean (*link)(void *, void *);
    gboolean (*syncState)(void *);
    void *(*staticPad)(void *, const char *);
    int (*padLink)(void *, void *);
    void *(*currentCaps)(void *);
    char *(*capsToString)(const void *);
    int (*setState)(void *, int);
    void *(*getBus)(void *);
    void *(*pop)(void *, GstClockTime, int);
    const void *(*messageStructure)(void *);
    const char *(*structureName)(const void *);
    void (*parseError)(void *, GError **, char **);
    void (*miniUnref)(void *);
    gboolean (*queryDuration)(void *, int, long long *);
    void (*errorFree)(GError *);
    void (*gFree)(void *);
    void (*objectSet)(void *, const char *, ...);
    unsigned long (*connect)(void *, const char *, GCallback, void *, void (*)(void *, void *), int);
};

template <typename T> bool res(QLibrary &l, const char *s, T &t) { t = reinterpret_cast<T>(l.resolve(s)); return t != nullptr; }

bool loadGst(TestGst &g, QString &version) {
    QLibrary gst(QStringLiteral("gstreamer-1.0"), 0), glib(QStringLiteral("glib-2.0"), 0), gobject(QStringLiteral("gobject-2.0"), 0);
    if (!gst.load() || !glib.load() || !gobject.load()) return false;
    const bool ok = res(gst, "gst_init_check", g.initCheck) && res(gst, "gst_version", g.version)
        && res(gst, "gst_element_factory_find", g.factoryFind) && res(gst, "gst_plugin_feature_load", g.featureLoad)
        && res(gst, "gst_object_unref", g.objectUnref) && res(gst, "gst_parse_launch", g.parseLaunch)
        && res(gst, "gst_element_factory_make", g.make) && res(gst, "gst_pipeline_new", g.pipelineNew)
        && res(gst, "gst_bin_add", g.binAdd) && res(gst, "gst_element_link", g.link)
        && res(gst, "gst_element_sync_state_with_parent", g.syncState)
        && res(gst, "gst_element_get_static_pad", g.staticPad) && res(gst, "gst_pad_link", g.padLink)
        && res(gst, "gst_pad_get_current_caps", g.currentCaps) && res(gst, "gst_caps_to_string", g.capsToString)
        && res(gst, "gst_element_set_state", g.setState) && res(gst, "gst_element_get_bus", g.getBus)
        && res(gst, "gst_bus_timed_pop_filtered", g.pop) && res(gst, "gst_message_get_structure", g.messageStructure)
        && res(gst, "gst_structure_get_name", g.structureName) && res(gst, "gst_message_parse_error", g.parseError)
        && res(gst, "gst_mini_object_unref", g.miniUnref) && res(gst, "gst_element_query_duration", g.queryDuration)
        && res(glib, "g_error_free", g.errorFree) && res(glib, "g_free", g.gFree)
        && res(gobject, "g_object_set", g.objectSet) && res(gobject, "g_signal_connect_data", g.connect);
    if (!ok || !g.initCheck(nullptr, nullptr, nullptr)) return false;
    unsigned a = 0, b = 0, c = 0, d = 0;
    g.version(&a, &b, &c, &d);
    version = QStringLiteral("%1.%2.%3").arg(a).arg(b).arg(c);
    return true;
}

TestGst G;

bool present(const char *name) {
    void *f = G.factoryFind(name);
    if (!f) return false;
    void *loaded = G.featureLoad(f);
    if (loaded) G.objectUnref(loaded);
    G.objectUnref(f);
    return loaded != nullptr;
}

const char *firstPresent(const char *const *names) {
    for (; *names; ++names) if (present(*names)) return *names;
    return nullptr;
}

// Runs the pipeline to EOS; empty string on success.
QString awaitEos(void *pipeline, int seconds, long long *durationNs = nullptr) {
    QString error;
    if (G.setState(pipeline, 4) == 0) error = QStringLiteral("state change failed");
    void *bus = G.getBus(pipeline);
    void *msg = G.pop(bus, GstClockTime(seconds) * 1000000000ull, 1 | 2);
    if (!msg) {
        if (error.isEmpty()) error = QStringLiteral("timeout");
    } else {
        const void *s = G.messageStructure(msg);
        if (s && std::strcmp(G.structureName(s), "GstMessageError") == 0) {
            GError *e = nullptr; char *dbg = nullptr;
            G.parseError(msg, &e, &dbg);
            error = QString::fromUtf8(e ? e->message : "error") + QStringLiteral(" / ") + QString::fromUtf8(dbg ? dbg : "");
            if (e) G.errorFree(e);
            G.gFree(dbg);
        }
        G.miniUnref(msg);
    }
    if (durationNs && !G.queryDuration(pipeline, 3 /* GST_FORMAT_TIME */, durationNs)) *durationNs = -1;
    G.objectUnref(bus);
    G.setState(pipeline, 1);
    return error;
}

QString launch(const QString &description) {
    GError *e = nullptr;
    void *p = G.parseLaunch(description.toUtf8().constData(), &e);
    if (e) {
        const QString m = QString::fromUtf8(e->message);
        G.errorFree(e);
        if (p) G.objectUnref(p);
        return m;
    }
    const QString error = awaitEos(p, 300);
    G.objectUnref(p);
    return error;
}

// Demuxes a file back with fakesinks and reports each stream's caps.
struct Probe { QString error; QStringList caps; long long durationNs = -1; };
struct ProbeCtx { void *pipeline; QMutex mutex; QStringList caps; };

void onProbePad(void *, void *pad, void *data) {
    ProbeCtx *ctx = static_cast<ProbeCtx *>(data);
    if (void *caps = G.currentCaps(pad)) {
        char *s = G.capsToString(caps);
        { QMutexLocker l(&ctx->mutex); ctx->caps << QString::fromUtf8(s); }
        G.gFree(s);
        G.miniUnref(caps);
    }
    void *sink = G.make("fakesink", nullptr);
    G.objectSet(sink, "sync", gboolean(0), static_cast<void *>(nullptr));
    G.binAdd(ctx->pipeline, sink);
    G.syncState(sink);
    void *sp = G.staticPad(sink, "sink");
    G.padLink(pad, sp);
    G.objectUnref(sp);
}

Probe probe(const QString &path, const char *demuxer) {
    Probe r;
    ProbeCtx ctx;
    ctx.pipeline = G.pipelineNew("probe");
    void *src = G.make("filesrc", nullptr), *demux = G.make(demuxer, nullptr);
    G.binAdd(ctx.pipeline, src);
    G.binAdd(ctx.pipeline, demux);
    G.objectSet(src, "location", QFile::encodeName(path).constData(), static_cast<void *>(nullptr));
    G.link(src, demux);
    G.connect(demux, "pad-added", reinterpret_cast<GCallback>(&onProbePad), &ctx, nullptr, 0);
    r.error = awaitEos(ctx.pipeline, 60, &r.durationNs);
    G.objectUnref(ctx.pipeline);
    r.caps = ctx.caps;
    return r;
}

bool hasCaps(const Probe &p, const char *prefix) {
    for (const QString &c : p.caps) if (c.startsWith(QLatin1String(prefix))) return true;
    return false;
}

// Top-level ISO-BMFF box types, to check faststart put moov before mdat.
QStringList topBoxes(const QString &path) {
    QStringList out;
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return out;
    qint64 pos = 0;
    while (pos + 8 <= f.size() && out.size() < 64) {
        f.seek(pos);
        const QByteArray h = f.read(16);
        if (h.size() < 8) break;
        quint64 size = (quint64(quint8(h[0])) << 24) | (quint64(quint8(h[1])) << 16) | (quint64(quint8(h[2])) << 8) | quint8(h[3]);
        out << QString::fromLatin1(h.mid(4, 4));
        if (size == 1 && h.size() >= 16) { size = 0; for (int i = 8; i < 16; ++i) size = (size << 8) | quint8(h[i]); }
        if (size == 0) break;
        if (size < 8) break;
        pos += qint64(size);
    }
    return out;
}

// A tiny H.264 Baseline stream built by hand (no H.264 encoder on the host):
// 64x64 IDR frames whose macroblocks are all I_PCM (raw samples), CAVLC, Annex B.
struct Bits {
    QByteArray bytes; int used = 0; unsigned char cur = 0;
    void bit(int b) { cur = static_cast<unsigned char>((cur << 1) | (b & 1)); if (++used == 8) { bytes.append(char(cur)); used = 0; cur = 0; } }
    void u(int n, unsigned v) { while (n--) bit(int((v >> n) & 1)); }
    void ue(unsigned v) { unsigned x = v + 1; int len = 0; while ((x >> len) > 1) ++len; u(len, 0); u(len + 1, x); }
    void se(int v) { ue(v > 0 ? unsigned(2 * v - 1) : unsigned(-2 * v)); }
    void align() { while (used) bit(0); }
    void trailing() { bit(1); align(); }
};

QByteArray nal(int refIdc, int type, const QByteArray &rbsp) {
    QByteArray out("\x00\x00\x00\x01", 4);
    out.append(char((refIdc << 5) | type));
    int zeros = 0;
    for (char c : rbsp) {
        if (zeros >= 2 && quint8(c) <= 3) { out.append('\x03'); zeros = 0; } // emulation prevention
        out.append(c);
        zeros = c == 0 ? zeros + 1 : 0;
    }
    return out;
}

// One access unit per frame, each starting with SPS and PPS.
QList<QByteArray> handMadeH264(int frames) {
    const int mbs = 4; // 64 px = 4 macroblocks per side
    Bits sps;
    sps.u(8, 66); sps.u(8, 0xC0); sps.u(8, 30); sps.ue(0); // Baseline, level 3.0, sps 0
    sps.ue(0); sps.ue(2); sps.ue(1); sps.u(1, 0);         // frame_num 4 bits, POC type 2, 1 ref
    sps.ue(mbs - 1); sps.ue(mbs - 1); sps.u(1, 1); sps.u(1, 1); sps.u(1, 0); sps.u(1, 1); // VUI follows
    sps.u(4, 0); sps.u(1, 1); sps.u(32, 1); sps.u(32, 60); sps.u(1, 1); // timing: 30 fps, fixed
    sps.u(1, 0); sps.u(1, 0); sps.u(1, 0); sps.u(1, 0);                 // no HRD, pic_struct, restrictions
    sps.trailing();
    Bits pps;
    pps.ue(0); pps.ue(0); pps.u(1, 0); pps.u(1, 0); pps.ue(0); pps.ue(0); pps.ue(0);
    pps.u(1, 0); pps.u(2, 0); pps.se(0); pps.se(0); pps.se(0); pps.u(1, 1); pps.u(1, 0); pps.u(1, 0);
    pps.trailing();
    const QByteArray headers = nal(3, 7, sps.bytes) + nal(3, 8, pps.bytes);
    QList<QByteArray> out;
    for (int f = 0; f < frames; ++f) {
        Bits sl;
        sl.ue(0); sl.ue(7); sl.ue(0); sl.u(4, 0); sl.ue(unsigned(f % 2)); // I slice, IDR, alternating idr_pic_id
        sl.u(1, 0); sl.u(1, 0); sl.se(0); sl.ue(1);                        // ref marking, qp delta, no deblocking
        for (int mb = 0; mb < mbs * mbs; ++mb) {
            sl.ue(25); // I_PCM
            sl.align();
            for (int i = 0; i < 384; ++i) sl.u(8, unsigned(i < 256 ? 16 + (f * 2 + mb * 9) % 220 : 128));
        }
        sl.trailing();
        out << headers + nal(3, 5, sl.bytes);
    }
    return out;
}

struct Result { bool done = false; bool ok = false; QString error; QThread *thread = nullptr; };

Remuxer::Completion collect(Result &r) {
    return [&r](bool ok, const QString &error) { r.done = true; r.ok = ok; r.error = error; r.thread = QThread::currentThread(); };
}

bool waitFor(const Result &r, int ms) {
    QElapsedTimer t;
    t.start();
    while (!r.done && t.elapsed() < ms) { QCoreApplication::processEvents(QEventLoop::AllEvents, 10); QThread::msleep(1); }
    return r.done;
}

QStringList partFiles(const QString &dir) {
    return QDir(dir).entryList(QStringList() << QStringLiteral("*.part"), QDir::Files | QDir::Hidden);
}

QString in, out;

Mp4Layout layoutOf(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return Mp4Layout();
    return scanMp4([&file](std::uint64_t off, char *buf, std::size_t len) -> std::size_t {
        if (!file.seek(static_cast<qint64>(off))) return 0;
        const qint64 n = file.read(buf, static_cast<qint64>(len));
        return n > 0 ? static_cast<std::size_t>(n) : 0;
    }, static_cast<std::uint64_t>(file.size()));
}

// DL-01/DL-03: the device player cannot read DASH fragments from disk, so an MP4
// output must be flat: one moov before one mdat, no moof, no sidx.
void checkFlat(const char *label, const QString &path) {
    const Mp4Layout l = layoutOf(path);
    if (!isFlatMp4(l))
        std::printf("  %s: not flat (moov %d, mdat %d, moof %d, sidx %d, brand %s)\n", label, l.moov, l.mdat, l.moof, l.sidx,
                    l.majorBrand.c_str());
    CHECK(l.valid);
    CHECK(l.moof == 0 && l.sidx == 0);
    CHECK(isFlatMp4(l));
    CHECK(l.moovBeforeMdat);
}

// Remuxes one pair and checks the result demuxes back to one video and one
// audio stream of the expected types and duration.
void remuxCase(GstRemuxer &remuxer, const char *label, const QString &video, const QString &audio, const QString &target,
               MuxContainer container, const char *demuxer, const char *videoCaps, const char *audioCaps) {
    Result r;
    QElapsedTimer clock;
    clock.start();
    remuxer.remux(video, audio, target, container, collect(r));
    CHECK(!r.done); // never completes inside remux()
    CHECK(waitFor(r, 120000));
    if (!r.ok) std::printf("  %s error: %s\n", label, qPrintable(r.error));
    CHECK(r.ok);
    CHECK(r.thread == QThread::currentThread());
    CHECK(QFileInfo(target).isFile());
    CHECK(partFiles(QFileInfo(target).absolutePath()).isEmpty());
    const Probe p = probe(target, demuxer);
    if (!p.error.isEmpty()) std::printf("  %s probe error: %s\n", label, qPrintable(p.error));
    CHECK(p.error.isEmpty());
    CHECK(p.caps.size() == 2);
    CHECK(hasCaps(p, videoCaps));
    CHECK(hasCaps(p, audioCaps));
    const double seconds = p.durationNs / 1e9;
    CHECK(seconds > 2.7 && seconds < 3.3); // inputs are 3.0 s each
    std::printf("  %s: ok in %lld ms, %.2f s, %lld bytes, streams: %s\n", label, (long long)clock.elapsed(), seconds,
                (long long)QFileInfo(target).size(), qPrintable(p.caps.join(QStringLiteral(" | ")).left(400)));
    if (container == MuxContainer::Mp4) {
        const QStringList boxes = topBoxes(target);
        std::printf("  %s top-level boxes: %s\n", label, qPrintable(boxes.join(QLatin1Char(' '))));
        CHECK(boxes.indexOf(QStringLiteral("moov")) >= 0);
        CHECK(boxes.indexOf(QStringLiteral("moov")) < boxes.indexOf(QStringLiteral("mdat"))); // faststart
        CHECK(!QFileInfo(target + QStringLiteral(".faststart.part")).exists());
        checkFlat(label, target);
    }
}

// One-track flatten: a fragmented (DASH) MP4 track in, a flat MP4 with that one
// stream out.
void flattenCase(GstRemuxer &remuxer, const char *label, const QString &input, bool audio, const char *caps) {
    CHECK(isFragmentedMp4(layoutOf(input))); // the fixture really is fragmented, like YouTube's DASH tracks
    const QString target = out + QLatin1Char('/') + QFileInfo(input).fileName() + QStringLiteral(".flat");
    Result r;
    remuxer.remux(audio ? QString() : input, audio ? input : QString(), target, MuxContainer::Mp4, collect(r));
    CHECK(waitFor(r, 120000));
    if (!r.ok) std::printf("  %s error: %s\n", label, qPrintable(r.error));
    CHECK(r.ok);
    CHECK(partFiles(out).isEmpty());
    checkFlat(label, target);
    const Probe p = probe(target, "qtdemux");
    CHECK(p.error.isEmpty());
    CHECK(p.caps.size() == 1 && hasCaps(p, caps));
    const double seconds = p.durationNs / 1e9;
    CHECK(seconds > 2.7 && seconds < 3.3);
    const Mp4Layout l = layoutOf(target);
    std::printf("  %s: flat in, %lld bytes, boxes:", label, (long long)QFileInfo(target).size());
    for (const Mp4Box &b : l.boxes) std::printf(" %s", b.type.c_str());
    std::printf("\n");
}

void skip(const char *label, const QString &why) {
    ++skipped;
    std::printf("  %s: SKIPPED (%s)\n", label, qPrintable(why));
}

} // namespace

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);

    CHECK(GstRemuxer::timeoutMsFor(0) == 60000);
    CHECK(GstRemuxer::timeoutMsFor(100ll * 1024 * 1024) == 70000);
    CHECK(GstRemuxer::timeoutMsFor(1024ll * 1024 * 1024) == 162400);

    const QString root = QDir(QString::fromLocal8Bit(qgetenv("TMPDIR"))).absoluteFilePath(
        QStringLiteral("remux-test.%1").arg(QCoreApplication::applicationPid()));
    in = root + QStringLiteral("/in");
    out = root + QStringLiteral("/out");
    QDir().mkpath(in);
    QDir().mkpath(out);
    // GLib caches its temp dir on first use: point it at an empty folder before
    // GStreamer loads, so the test can show the remuxer spools nothing there (PLT-09).
    const QString glibTmp = root + QStringLiteral("/glib-tmp");
    QDir().mkpath(glibTmp);
    qputenv("TMPDIR", QFile::encodeName(glibTmp));
    QFile dummy(in + QStringLiteral("/dummy.bin"));
    dummy.open(QIODevice::WriteOnly);
    dummy.write("not a media file");
    dummy.close();

    // Unavailable GStreamer: immediate error, on the caller's thread, both before
    // and after the (cached) probe.
    {
        GstRemuxer missing(QStringLiteral("sailtube-no-such-gstreamer"));
        Result first;
        QElapsedTimer t;
        t.start();
        missing.remux(dummy.fileName(), dummy.fileName(), out + QStringLiteral("/x.mp4"), MuxContainer::Mp4, collect(first));
        CHECK(!first.done);
        CHECK(waitFor(first, 5000));
        CHECK(!first.ok && !first.error.isEmpty());
        CHECK(first.thread == QThread::currentThread());
        const MuxerAvailability a = missing.availability();
        CHECK(!a.loaded && !a.mp4 && !a.webm && !a.matroska);
        Result second;
        missing.remux(dummy.fileName(), dummy.fileName(), out + QStringLiteral("/x.webm"), MuxContainer::WebM, collect(second));
        CHECK(!second.done);
        CHECK(waitFor(second, 5000));
        CHECK(!second.ok && second.error.contains(QStringLiteral("not available")));
        CHECK(t.elapsed() < 3000);
        CHECK(QDir(out).entryList(QDir::Files | QDir::Hidden).isEmpty());
        std::printf("  unavailable: \"%s\" / \"%s\" in %lld ms\n", qPrintable(first.error), qPrintable(a.detail),
                    (long long)t.elapsed());
    }

    QString version;
    if (!loadGst(G, version)) {
        std::printf("remux tests: GStreamer runtime absent, device cases SKIPPED\n");
        std::printf("remux tests: %s (%d failure%s)\n", failures ? "FAIL" : "PASS", failures, failures == 1 ? "" : "s");
        QDir(root).removeRecursively();
        return failures ? 1 : 0;
    }

    // E6 host inventory.
    const char *const inventory[] = { "mp4mux", "qtmux", "qtdemux", "webmmux", "matroskamux", "matroskademux",
        "h264parse", "multifilesrc", "avimux", "avidemux", "aacparse", "vp9parse", "opusparse", "x264enc", "openh264enc", "avenc_h264", "v4l2h264enc",
        "avenc_aac", "voaacenc", "fdkaacenc", "faac", "vp8enc", "vp9enc", "opusenc", "vorbisenc",
        "videotestsrc", "audiotestsrc", "droidvenc", "droidvdec", nullptr };
    QStringList have, lack;
    for (const char *const *n = inventory; *n; ++n) (present(*n) ? have : lack) << QString::fromLatin1(*n);
    std::printf("  GStreamer %s\n  present: %s\n  absent: %s\n", qPrintable(version),
                qPrintable(have.join(QLatin1Char(' '))), qPrintable(lack.join(QLatin1Char(' '))));

    GstRemuxer remuxer;
    const MuxerAvailability a = remuxer.availability();
    std::printf("  availability: loaded=%d mp4=%d webm=%d matroska=%d (%s)\n", a.loaded, a.mp4, a.webm, a.matroska,
                qPrintable(a.detail));
    CHECK(a.loaded);
    CHECK(a.detail.contains(version));
    CHECK(a.mp4 == ((present("mp4mux") || present("qtmux")) && present("qtdemux")));
    CHECK(a.webm == (present("webmmux") && present("matroskademux")));
    CHECK(a.matroska == (present("matroskamux") && present("matroskademux")));
    const MuxerAvailability again = remuxer.availability();
    CHECK(again.detail == a.detail && again.mp4 == a.mp4 && again.webm == a.webm);

    // Fixtures: 3.0 s each (90 frames at 30 fps; 30 x 100 ms of 48 kHz audio).
    // MP4 tracks are fragmented, like YouTube's DASH tracks.
    const QString raw = QStringLiteral("videotestsrc num-buffers=90 ! video/x-raw,format=I420,width=320,height=240,framerate=30/1 ! ");
    const QString tone = QStringLiteral("audiotestsrc num-buffers=30 samplesperbuffer=4800 ! audio/x-raw,rate=48000,channels=2 ! audioconvert ! audioresample ! ");
    const QString frag = QStringLiteral(" ! mp4mux fragment-duration=1000 ! filesink location=\"%1\"");
    const QString webm = QStringLiteral(" ! webmmux ! filesink location=\"%1\"");
    const auto fixture = [&](const QString &name, const QString &pipeline) -> QString {
        const QString path = in + QLatin1Char('/') + name;
        const QString e = launch(pipeline.arg(path));
        if (!e.isEmpty()) { std::printf("  fixture %s not built: %s\n", qPrintable(name), qPrintable(e.left(300))); return QString(); }
        return path;
    };
    const bool vp9 = present("vp9enc"), vp8 = present("vp8enc"), opus = present("opusenc");
    const QString vp9Webm = vp9 ? fixture(QStringLiteral("v-vp9.webm"), raw + QStringLiteral("vp9enc deadline=1 cpu-used=8") + webm) : QString();
    const QString vp8Webm = vp8 ? fixture(QStringLiteral("v-vp8.webm"), raw + QStringLiteral("vp8enc deadline=1") + webm) : QString();
    const QString opusWebm = opus ? fixture(QStringLiteral("a-opus.webm"), tone + QStringLiteral("opusenc") + webm) : QString();
    const QString vp9Mp4 = vp9 ? fixture(QStringLiteral("v-vp9.mp4"), raw + QStringLiteral("vp9enc deadline=1 cpu-used=8 ! vp9parse") + frag) : QString();
    const QString opusMp4 = opus ? fixture(QStringLiteral("a-opus.mp4"), tone + QStringLiteral("opusenc") + frag) : QString();
    const char *const h264Encoders[] = { "x264enc", "openh264enc", "avenc_h264", nullptr };
    const char *const aacEncoders[] = { "fdkaacenc", "voaacenc", "avenc_aac", "faac", nullptr };
    const char *h264 = firstPresent(h264Encoders), *aac = firstPresent(aacEncoders);
    const QString h264Mp4 = h264 ? fixture(QStringLiteral("v-h264.mp4"), raw + QLatin1String(h264) + frag) : QString();
    // Without an encoder, H.264 video comes from the hand-made I_PCM stream.
    QString pcmH264Mp4;
    // One frame per file. multifilesrc sets only DTS and mp4mux needs PTS, so
    // the frames pass through AVI first: avidemux timestamps by frame index.
    if (present("h264parse") && present("multifilesrc") && present("avimux") && present("avidemux")) {
        const QList<QByteArray> frames = handMadeH264(90);
        for (int i = 0; i < frames.size(); ++i) {
            QFile f(in + QStringLiteral("/pcm-%1.h264").arg(i));
            f.open(QIODevice::WriteOnly);
            f.write(frames.at(i));
        }
        const QString avi = fixture(QStringLiteral("v-pcm-h264.avi"), QStringLiteral("multifilesrc location=\"") + in
            + QStringLiteral("/pcm-%d.h264\" index=0 stop-index=89 caps=\"video/x-h264,stream-format=byte-stream,alignment=au,width=64,height=64,framerate=30/1\" ! h264parse ! avimux ! filesink location=\"%1\""));
        if (!avi.isEmpty())
            pcmH264Mp4 = fixture(QStringLiteral("v-pcm-h264.mp4"), QStringLiteral("filesrc location=\"") + avi
                + QStringLiteral("\" ! avidemux ! h264parse ! video/x-h264,stream-format=avc,alignment=au") + frag);
    }
    const QString aacMp4 = aac ? fixture(QStringLiteral("a-aac.m4a"), tone + QLatin1String(aac) + frag) : QString();

    // MP4: H.264 + AAC, the DL-03 case.
    if (!a.mp4) skip("mp4 H.264+AAC", QStringLiteral("no MP4 muxer"));
    else if (h264Mp4.isEmpty() || aacMp4.isEmpty())
        skip("mp4 H.264+AAC", QStringLiteral("no H.264 or AAC encoder to build fixtures; muxing is device step E6"));
    else remuxCase(remuxer, "mp4 H.264+AAC", h264Mp4, aacMp4, out + QStringLiteral("/h264.mp4"), MuxContainer::Mp4,
                   "qtdemux", "video/x-h264", "audio/mpeg");

    // MP4 path (qtdemux -> mp4mux, faststart) with H.264 video as on YouTube,
    // here the hand-made stream, and Opus audio (no AAC encoder on the host).
    if (!a.mp4 || pcmH264Mp4.isEmpty() || opusMp4.isEmpty()) skip("mp4 H.264(I_PCM)+Opus", QStringLiteral("no fixtures"));
    else remuxCase(remuxer, "mp4 H.264(I_PCM)+Opus", pcmH264Mp4, opusMp4, out + QStringLiteral("/h264-opus.mp4"),
                   MuxContainer::Mp4, "qtdemux", "video/x-h264", "audio/x-opus");

    // One-track flatten of DASH-style fragmented tracks (itag 140 audio, video-only tracks).
    if (!a.mp4) skip("flatten audio", QStringLiteral("no MP4 muxer"));
    else if (!aacMp4.isEmpty()) flattenCase(remuxer, "flatten AAC audio", aacMp4, true, "audio/mpeg");
    else if (!opusMp4.isEmpty()) flattenCase(remuxer, "flatten Opus-in-MP4 audio", opusMp4, true, "audio/x-opus");
    else skip("flatten audio", QStringLiteral("no AAC or Opus encoder for a fragmented fixture"));
    const QString videoFixture = !h264Mp4.isEmpty() ? h264Mp4 : pcmH264Mp4;
    if (!a.mp4 || videoFixture.isEmpty()) skip("flatten H.264 video", QStringLiteral("no fixture"));
    else flattenCase(remuxer, "flatten H.264 video", videoFixture, false, "video/x-h264");
    {
        Result r;
        remuxer.remux(QString(), QString(), out + QStringLiteral("/none.mp4"), MuxContainer::Mp4, collect(r));
        CHECK(waitFor(r, 5000));
        CHECK(!r.ok && r.error.contains(QStringLiteral("no input")));
    }

    // VP9 is refused for MP4 output, before it reaches mp4mux (which crashes
    // on 4:2:0 VP9 from qtdemux in GStreamer 1.26.11).
    if (!a.mp4 || vp9Mp4.isEmpty() || opusMp4.isEmpty()) skip("mp4 refuses VP9", QStringLiteral("no VP9/Opus MP4 fixtures"));
    else {
        Result r;
        remuxer.remux(vp9Mp4, opusMp4, out + QStringLiteral("/vp9.mp4"), MuxContainer::Mp4, collect(r));
        CHECK(waitFor(r, 30000));
        CHECK(!r.ok && r.error.contains(QStringLiteral("video/x-vp9")));
        CHECK(!QFileInfo(out + QStringLiteral("/vp9.mp4")).exists());
        CHECK(partFiles(out).isEmpty());
        std::printf("  mp4 refuses VP9: \"%s\"\n", qPrintable(r.error));
    }

    // WebM VP9 + Opus; the output name has spaces and quotes: paths are
    // properties, never parsed.
    const QString odd = out + QStringLiteral("/my \"odd\" name's.webm");
    if (!a.webm || vp9Webm.isEmpty() || opusWebm.isEmpty()) skip("webm VP9+Opus", QStringLiteral("no VP9/Opus fixtures"));
    else remuxCase(remuxer, "webm VP9+Opus", vp9Webm, opusWebm, odd, MuxContainer::WebM, "matroskademux",
                   "video/x-vp9", "audio/x-opus");

    if (!a.webm || vp8Webm.isEmpty() || opusWebm.isEmpty()) skip("webm VP8+Opus", QStringLiteral("no VP8/Opus fixtures"));
    else remuxCase(remuxer, "webm VP8+Opus", vp8Webm, opusWebm, out + QStringLiteral("/vp8.webm"), MuxContainer::WebM,
                   "matroskademux", "video/x-vp8", "audio/x-opus");

    // Matroska from mixed inputs (WebM video, MP4 audio): demuxers chosen by signature.
    const QString mixedAudio = opusMp4.isEmpty() ? opusWebm : opusMp4;
    if (!a.matroska || vp9Webm.isEmpty() || mixedAudio.isEmpty()) skip("matroska mixed", QStringLiteral("no fixtures"));
    else remuxCase(remuxer, "matroska mixed", vp9Webm, mixedAudio, out + QStringLiteral("/mixed.mkv"),
                   MuxContainer::Matroska, "matroskademux", "video/x-vp9", "audio/x-opus");

    // Missing input: error, nothing written.
    {
        const QStringList before = QDir(out).entryList(QDir::Files | QDir::Hidden);
        Result r;
        remuxer.remux(in + QStringLiteral("/absent.webm"), opusWebm, out + QStringLiteral("/absent.webm"),
                      MuxContainer::WebM, collect(r));
        CHECK(!r.done);
        CHECK(waitFor(r, 5000));
        CHECK(!r.ok && r.error.contains(QStringLiteral("not found")));
        CHECK(QDir(out).entryList(QDir::Files | QDir::Hidden) == before);
        std::printf("  missing input: \"%s\"\n", qPrintable(r.error));
    }

    // An audio-only file given as the video track fails fast, not by timeout.
    if (!opusWebm.isEmpty() && a.webm) {
        Result r;
        QElapsedTimer t;
        t.start();
        remuxer.remux(opusWebm, opusWebm, out + QStringLiteral("/wrong.webm"), MuxContainer::WebM, collect(r));
        CHECK(waitFor(r, 30000));
        CHECK(!r.ok);
        CHECK(t.elapsed() < 10000);
        CHECK(!QFileInfo(out + QStringLiteral("/wrong.webm")).exists());
        CHECK(partFiles(out).isEmpty());
        std::printf("  wrong track kind: \"%s\" in %lld ms\n", qPrintable(r.error.left(200)), (long long)t.elapsed());
    }

    // Not a media file: refused before any pipeline runs.
    {
        Result r;
        remuxer.remux(dummy.fileName(), opusWebm.isEmpty() ? dummy.fileName() : opusWebm,
                      out + QStringLiteral("/dummy.webm"), MuxContainer::WebM, collect(r));
        CHECK(waitFor(r, 5000));
        CHECK(!r.ok);
        CHECK(!QFileInfo(out + QStringLiteral("/dummy.webm")).exists());
    }

    // Cancel mid-job: wait until the muxer writes, then cancel; no output stays.
    if (a.webm && vp8 && opus) {
        const QString longVideo = fixture(QStringLiteral("v-long.webm"),
            QStringLiteral("videotestsrc num-buffers=3600 ! video/x-raw,format=I420,width=160,height=120,framerate=30/1 ! vp8enc deadline=1") + webm);
        const QString longAudio = fixture(QStringLiteral("a-long.webm"),
            QStringLiteral("audiotestsrc num-buffers=1200 samplesperbuffer=4800 ! audio/x-raw,rate=48000,channels=2 ! audioconvert ! audioresample ! opusenc") + webm);
        if (longVideo.isEmpty() || longAudio.isEmpty()) skip("cancel", QStringLiteral("no long fixtures"));
        else {
            const QString target = out + QStringLiteral("/cancelled.webm");
            Result r;
            const quint64 id = remuxer.remux(longVideo, longAudio, target, MuxContainer::WebM, collect(r));
            QElapsedTimer t;
            t.start();
            while (!r.done && !QFileInfo(target + QStringLiteral(".part")).exists() && t.elapsed() < 30000) {
                QCoreApplication::processEvents();
                QThread::usleep(200);
            }
            const bool started = QFileInfo(target + QStringLiteral(".part")).exists();
            remuxer.cancel(id);
            CHECK(waitFor(r, 30000));
            CHECK(started);
            CHECK(!r.ok && r.error == QStringLiteral("cancelled"));
            CHECK(!QFileInfo(target).exists());
            CHECK(partFiles(out).isEmpty());
            std::printf("  cancel: started=%d, \"%s\" after %lld ms\n", started, qPrintable(r.error), (long long)t.elapsed());
        }
    } else skip("cancel", QStringLiteral("no WebM muxer or VP8/Opus encoder"));

    // Destroying the remuxer with a job in flight returns promptly and leaves nothing.
    if (a.webm && !vp9Webm.isEmpty() && !opusWebm.isEmpty()) {
        QElapsedTimer t;
        t.start();
        Result r;
        {
            GstRemuxer shortLived;
            shortLived.remux(vp9Webm, opusWebm, out + QStringLiteral("/dropped.webm"), MuxContainer::WebM, collect(r));
        }
        QCoreApplication::processEvents();
        CHECK(!r.done);
        CHECK(t.elapsed() < 10000);
        CHECK(partFiles(out).isEmpty());
    }

    std::printf("  outputs: %s\n", qPrintable(QDir(out).entryList(QDir::Files | QDir::Hidden).join(QStringLiteral(", "))));
    const QStringList spooled = QDir(glibTmp).entryList(QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot);
    if (!spooled.isEmpty()) std::printf("  temp dir not empty: %s\n", qPrintable(spooled.join(QStringLiteral(", "))));
    CHECK(spooled.isEmpty());
    QDir(root).removeRecursively();
    std::printf("remux tests: %s (%d failure%s, %d case%s skipped)\n", failures ? "FAIL" : "PASS", failures,
                failures == 1 ? "" : "s", skipped, skipped == 1 ? "" : "s");
    return failures ? 1 : 0;
}
