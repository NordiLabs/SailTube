// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// NFR-TST-06: the GStreamer engine and VideoPlayer API that had no test yet,
// on WebM fixtures generated at start-up (local files, and one HTTP source
// from an in-process server for buffering). Audio goes to fakesink
// sync=true; video to the real appsink so frames reach the mailbox. The live
// chain values are read back from the running pipeline through a handoff
// emission hook on the audio fakesink. Build: tests/player_api_qt/run.sh
#include "Core/Player/Infrastructure/GstEngine.h"
#include "Core/Player/Infrastructure/VideoFrameMailbox.h"
#include "Core/Player/Infrastructure/VideoPlayer.h"

#include <gst/gst.h>

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QMutex>
#include <QMutexLocker>
#include <QTcpServer>
#include <QTcpSocket>
#include <QThread>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <memory>
#include <vector>

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

// ---- Fixtures ----------------------------------------------------------------------

static QString gDir;
static const int kSeconds = 12;
static const int kFps = 30;

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
    if (m) gst_message_unref(m);
    gst_object_unref(bus);
    gst_element_set_state(p, GST_STATE_NULL);
    gst_object_unref(p);
    if (!ok) std::printf("fixture failed: %s\n", qPrintable(description));
    return ok;
}

static QString videoSrc() {
    return QStringLiteral("videotestsrc num-buffers=%1 pattern=ball ! video/x-raw,width=320,height=180,framerate=%2/1 ! ")
        .arg(kSeconds * kFps).arg(kFps);
}
static QString audioSrc() {
    return QStringLiteral("audiotestsrc num-buffers=%1 samplesperbuffer=960 wave=sine ! audio/x-raw,rate=48000,channels=2 ! ")
        .arg(kSeconds * 50);
}
static QString vp8() { return QStringLiteral("vp8enc deadline=1 keyframe-max-dist=30 ! "); }

static bool makeFixtures() {
    const QString d = gDir + "/";
    bool ok = true;
    ok = ok && runPipeline(videoSrc() + vp8() + "webmmux ! filesink location=" + d + "v.webm");
    ok = ok && runPipeline(audioSrc() + "opusenc ! webmmux ! filesink location=" + d + "a.webm");
    ok = ok && runPipeline("webmmux name=m ! filesink location=" + d + "av.webm " + videoSrc() + vp8() + "m. " +
                           audioSrc() + "opusenc ! m.");
    return ok;
}

// ---- Minimal HTTP server: GET /<file>, Range honoured ----------------------------------

class HttpServer {
public:
    bool start() {
        QObject::connect(&server_, &QTcpServer::newConnection, [this]() {
            while (QTcpSocket *s = server_.nextPendingConnection()) {
                std::shared_ptr<QByteArray> buffer(new QByteArray);
                QObject::connect(s, &QTcpSocket::readyRead, [s, buffer]() {
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
    static void respond(QTcpSocket *s, const QByteArray &request) {
        const QList<QByteArray> lines = request.split('\n');
        const QString path = QString::fromLatin1(lines.value(0).trimmed().split(' ').value(1));
        QFile f(gDir + path);
        if (path.contains("..") || !f.open(QIODevice::ReadOnly)) {
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

static HttpServer *gServer = nullptr;

// ---- Reading the live pipeline back ------------------------------------------------------
// GstEngine keeps its pipeline private. The audio sink is a fakesink with
// signal-handoffs=true; an emission hook on its "handoff" signal captures it
// (from the streaming thread), and its top-level parent is the pipeline.

static QMutex gSinkMutex;
static GstElement *gAudioSink = nullptr;  // ref held

static gboolean handoffHook(GSignalInvocationHint *, guint n, const GValue *values, gpointer) {
    if (n < 1) return TRUE;
    GObject *object = static_cast<GObject *>(g_value_get_object(&values[0]));
    if (!object || !GST_IS_ELEMENT(object)) return TRUE;
    GstElement *e = GST_ELEMENT(object);
    gchar *name = gst_object_get_name(GST_OBJECT(e));
    const bool audioSink = name && std::string(name) == elementnames::kAudioSink;
    g_free(name);
    if (!audioSink) return TRUE;
    QMutexLocker lock(&gSinkMutex);
    if (gAudioSink != e) {
        if (gAudioSink) gst_object_unref(gAudioSink);
        gAudioSink = GST_ELEMENT(gst_object_ref(e));
    }
    return TRUE;
}

static void installHook() {
    GstElement *probe = gst_element_factory_make("fakesink", nullptr);
    const guint id = g_signal_lookup("handoff", G_OBJECT_TYPE(probe));
    gst_object_unref(probe);
    g_signal_add_emission_hook(id, 0, &handoffHook, nullptr, nullptr);
}

static void forgetSink() {
    QMutexLocker lock(&gSinkMutex);
    if (gAudioSink) gst_object_unref(gAudioSink);
    gAudioSink = nullptr;
}

// The element `name` of the pipeline the captured audio sink lives in (ref), or null.
static GstElement *pipelineElement(const char *name) {
    GstObject *top = nullptr;
    {
        QMutexLocker lock(&gSinkMutex);
        if (!gAudioSink) return nullptr;
        top = GST_OBJECT(gst_object_ref(gAudioSink));
    }
    for (GstObject *parent = gst_object_get_parent(top); parent; parent = gst_object_get_parent(top)) {
        gst_object_unref(top);
        top = parent;
    }
    GstElement *e = GST_IS_BIN(top) ? gst_bin_get_by_name(GST_BIN(top), name) : nullptr;
    gst_object_unref(top);
    return e;
}

// The hook has seen the running audio sink and its pipeline holds the boost stage.
static bool chainReachable() {
    GstElement *e = pipelineElement(elementnames::kAudioBoost);
    if (!e) return false;
    gst_object_unref(e);
    return true;
}

static double doubleProperty(const char *element, const char *property) {
    GstElement *e = pipelineElement(element);
    if (!e) return -999;
    gdouble v = -999;
    g_object_get(e, property, &v, nullptr);
    gst_object_unref(e);
    return v;
}

static int boolProperty(const char *element, const char *property) {
    GstElement *e = pipelineElement(element);
    if (!e) return -1;
    gboolean v = FALSE;
    g_object_get(e, property, &v, nullptr);
    gst_object_unref(e);
    return v ? 1 : 0;
}

// ---- Inputs -----------------------------------------------------------------------------

static SourceSpec file(const char *name) {
    SourceSpec s;
    s.uri = ("file://" + gDir + "/" + name).toStdString();
    s.container = Container::Matroska;
    return s;
}

static SinkOverride fakeAudioSink() {
    SinkOverride o;
    o.factory = "fakesink";
    PropertySetting sync;
    sync.name = "sync";
    sync.value = "true";
    PropertySetting handoffs;
    handoffs.name = "signal-handoffs";
    handoffs.value = "true";
    o.properties.push_back(sync);
    o.properties.push_back(handoffs);
    return o;
}

static PlanInput dualInput() {
    PlanInput in;
    in.mode = PlayMode::Dual;
    in.video = file("v.webm");
    in.audio = file("a.webm");
    in.available = GstEngine::availableElements();
    in.audioSink = fakeAudioSink();
    return in;
}

static PlanInput localInput() {
    PlanInput in;
    in.mode = PlayMode::Local;
    in.muxed = file("av.webm");
    in.available = GstEngine::availableElements();
    in.audioSink = fakeAudioSink();
    return in;
}

struct EngineHarness {
    VideoFrameMailbox mailbox;
    GstEngine engine;
    int asyncDones = 0, firstFrames = 0, errors = 0;
    QString lastError;
    EngineHarness() : engine(&mailbox) {
        QObject::connect(&engine, &GstEngine::asyncDone, [this]() { ++asyncDones; });
        QObject::connect(&engine, &GstEngine::firstVideoFrame, [this]() { ++firstFrames; });
        QObject::connect(&engine, &GstEngine::errorOccurred, [this](const QString &m, const QString &, int, bool, int) {
            ++errors;
            lastError = m;
            std::printf("  engine error: %s\n", qPrintable(m));
        });
    }
    bool prerolled(int timeoutMs = 8000) {
        const int before = asyncDones;
        return engine.setState(PipelineState::Paused) &&
               waitFor([&]() { return asyncDones > before || errors > 0; }, timeoutMs) && errors == 0;
    }
    bool playing(int timeoutMs = 8000) {
        if (!prerolled(timeoutMs) || !engine.setState(PipelineState::Playing)) return false;
        qint64 ms = 0;
        return waitFor([&]() { return engine.queryPosition(Branch::Muxed, &ms) && ms > 200; }, timeoutMs);
    }
};

// ---- GstEngine ----------------------------------------------------------------------------

// PLY-01: lifecycle without and with a pipeline; software builds have no
// hardware sink (PLY-20); teardown() is idempotent and drops the pipeline.
static void ply01_engineLifecycle() {
    EngineHarness h;
    qint64 ms = -1;
    bool seekable = true;
    CHECK(!h.engine.built());
    CHECK(h.engine.setState(PipelineState::Null));       // nothing to do: success
    CHECK(!h.engine.setState(PipelineState::Playing));   // no pipeline
    CHECK(!h.engine.queryPosition(Branch::Muxed, &ms) && ms == -1);
    CHECK(!h.engine.querySeekable(&seekable) && seekable);
    CHECK(h.engine.hardwareVideoSink() == nullptr);
    h.engine.teardown();                                  // no pipeline: no-op
    CHECK(!h.engine.built());

    QString error;
    CHECK(h.engine.build(dualInput(), &error));
    CHECK_MSG(h.engine.built(), "%s", qPrintable(error));
    CHECK(h.engine.plan().decode == DecodeMode::Software);
    CHECK(h.engine.hardwareVideoSink() == nullptr);      // PLY-20: null in software mode
    const quint64 generation = h.engine.generation();
    CHECK(h.engine.setState(PipelineState::Ready));
    CHECK(h.playing());
    CHECK(h.engine.queryPosition(Branch::Video, &ms) && ms >= 0);
    CHECK(h.engine.queryPosition(Branch::Audio, &ms) && ms >= 0);
    CHECK(h.engine.querySeekable(&seekable) && seekable); // local Matroska is seekable
    CHECK(h.engine.hardwareVideoSink() == nullptr);
    CHECK(h.firstFrames == 1);

    h.engine.teardown();
    CHECK(!h.engine.built());
    CHECK(h.engine.generation() > generation);           // anything still queued is stale
    CHECK(!h.engine.queryPosition(Branch::Audio, &ms));
    CHECK(!h.engine.querySeekable(&seekable));
    CHECK(!h.engine.setState(PipelineState::Playing));
    const quint64 after = h.engine.generation();
    h.engine.teardown();                                  // idempotent
    CHECK(h.engine.generation() == after);
    // An AudioOnly build has no video sink at all.
    PlanInput audio;
    audio.mode = PlayMode::AudioOnly;
    audio.audio = file("a.webm");
    audio.available = GstEngine::availableElements();
    audio.audioSink = fakeAudioSink();
    CHECK(h.engine.build(audio, &error));
    CHECK(h.prerolled());
    CHECK(!h.engine.queryPosition(Branch::Video, &ms));  // no video sink
    CHECK(h.engine.queryPosition(Branch::Audio, &ms));
    h.engine.teardown();
    // A plan that is not ok is refused with its reason (Local needs file://).
    PlanInput bad = localInput();
    bad.muxed.uri = "http://127.0.0.1:1/x.webm";
    CHECK(!h.engine.build(bad, &error) && !error.isEmpty() && !h.engine.built());
}

// PLY-03: a seek event goes to ONE sink in Dual mode; the sink's SEGMENT with
// the seek's seqnum tells where it landed.
static void ply03_engineSeekReachesItsSink() {
    EngineHarness h;
    QString error;
    CHECK(h.engine.build(dualInput(), &error));
    CHECK(h.playing());
    SeekCommand c;
    c.branch = Branch::Video;
    c.flags = seekFlagsFor(Container::Matroska);
    c.positionNs = msToNs(3000);
    quint32 vseq = 0;
    CHECK(h.engine.sendSeek(c, &vseq));
    CHECK(vseq != 0);
    CHECK(waitFor([&]() { return h.engine.sinkSawSegment(Branch::Video, vseq); }, 5000));
    qint64 landed = -1;
    CHECK(h.engine.sinkSegmentStart(Branch::Video, vseq, &landed));
    CHECK_MSG(std::llabs(landed - 3000) <= 100, "video segment start %lld", (long long)landed);
    CHECK(!h.engine.sinkSawSegment(Branch::Audio, vseq));       // the audio branch was not seeked
    CHECK(!h.engine.sinkSegmentStart(Branch::Audio, vseq, &landed));
    CHECK(!h.engine.sinkSawSegment(Branch::Video, vseq + 1000));  // another seek
    qint64 untouched = 42;
    CHECK(!h.engine.sinkSegmentStart(Branch::Video, vseq + 1000, &untouched) && untouched == 42);

    c.branch = Branch::Audio;
    quint32 aseq = 0;
    CHECK(h.engine.sendSeek(c, &aseq));
    CHECK(aseq != 0 && aseq != vseq);
    CHECK(waitFor([&]() { return h.engine.sinkSawSegment(Branch::Audio, aseq); }, 5000));
    CHECK(h.engine.sinkSegmentStart(Branch::Audio, aseq, &landed));
    CHECK_MSG(std::llabs(landed - 3000) <= 100, "audio segment start %lld", (long long)landed);
    CHECK(h.engine.sinkSawSegment(Branch::Video, vseq));        // unchanged by the audio seek
    qint64 pos = 0;
    CHECK(waitFor([&]() { return h.engine.queryPosition(Branch::Audio, &pos) && pos >= 3000; }, 5000));
    CHECK(h.engine.queryPosition(Branch::Video, &pos) && pos >= 2900);
    CHECK(h.engine.sendSeek(c));                                // seqnum is optional
    h.engine.teardown();
    // A sink the pipeline does not have refuses the seek.
    PlanInput audio;
    audio.mode = PlayMode::AudioOnly;
    audio.audio = file("a.webm");
    audio.available = GstEngine::availableElements();
    audio.audioSink = fakeAudioSink();
    CHECK(h.engine.build(audio, &error));
    CHECK(h.prerolled());
    c.branch = Branch::Video;
    CHECK(!h.engine.sendSeek(c));
    h.engine.teardown();
    CHECK(!h.engine.sendSeek(c));                               // no pipeline
}

// PLY-02 diagnostics: rendered frames, flushes and segment rates; resetStats()
// zeroes the counters but keeps the current segment rates.
static void ply02_engineStatsAndReset() {
    EngineHarness h;
    QString error;
    CHECK(h.engine.build(localInput(), &error));
    CHECK(h.engine.stats().framesRendered == 0 && h.engine.stats().videoSinkFlushes == 0);
    CHECK(h.playing());
    spin(700);
    const EngineStats s0 = h.engine.stats();
    CHECK_MSG(s0.framesRendered >= 10, "frames %lld", (long long)s0.framesRendered);
    CHECK(s0.videoSegmentRate == 1.0 && s0.audioSegmentRate == 1.0);
    // One pipeline-level rate-carrying flush seek (Muxed / Local, PLY-03 + PLY-07).
    SeekCommand c;
    c.branch = Branch::Muxed;
    c.flags = SeekFlags::flushKeyUnit();
    c.positionNs = msToNs(2000);
    c.rate = 1.5;
    CHECK(h.engine.sendSeek(c));
    CHECK(waitFor([&]() { return h.engine.stats().videoSinkFlushes >= 1 && h.engine.stats().audioSinkFlushes >= 1; }, 5000));
    CHECK(waitFor([&]() { return std::fabs(h.engine.stats().videoSegmentRate - 1.5) < 1e-6; }, 5000));
    CHECK(waitFor([&]() { return std::fabs(h.engine.stats().audioSegmentRate - 1.5) < 1e-6; }, 5000));
    spin(300);
    const EngineStats s1 = h.engine.stats();
    CHECK(s1.videoSinkFlushes == 1);                           // one flush per seek (seqnum)
    h.engine.resetStats();
    const EngineStats s2 = h.engine.stats();
    CHECK(s2.videoSinkFlushes == 0 && s2.audioSinkFlushes == 0);
    CHECK(s2.framesRendered < s1.framesRendered && s2.framesRendered <= 3);
    CHECK(std::fabs(s2.videoSegmentRate - 1.5) < 1e-6 && std::fabs(s2.audioSegmentRate - 1.5) < 1e-6);  // kept
    spin(500);
    CHECK(h.engine.stats().framesRendered > s2.framesRendered);  // counting goes on
    // build() starts from zero.
    CHECK(h.engine.build(localInput(), &error));
    const EngineStats s3 = h.engine.stats();
    CHECK(s3.framesRendered == 0 && s3.videoSinkFlushes == 0 && s3.videoSegmentRate == 1.0);
    h.engine.teardown();
}

// PLY-06: while video output is disabled the preroll frame is held (no
// mailbox post, no first frame); enabling posts it and announces the first
// frame once. build() re-enables.
static void ply06_engineVideoOutputGate() {
    EngineHarness h;
    QString error;
    CHECK(h.engine.build(localInput(), &error));
    h.engine.setVideoOutputEnabled(false);
    CHECK(h.prerolled());
    spin(300);
    CHECK(h.mailbox.serial() == 0 && !h.mailbox.latest().valid());
    CHECK(h.firstFrames == 0);
    h.engine.setVideoOutputEnabled(true);
    CHECK(h.firstFrames == 1);                                  // emitted from the call itself
    CHECK(waitFor([&]() { return h.mailbox.latest().valid(); }, 2000));
    CHECK(h.mailbox.latest().width == 320 && h.mailbox.latest().height == 180);
    const quint64 serial = h.mailbox.serial();
    h.engine.setVideoOutputEnabled(true);                       // no change: nothing
    spin(100);
    CHECK(h.firstFrames == 1 && h.mailbox.serial() == serial);
    // Disabled while playing: no new frames reach the mailbox.
    CHECK(h.engine.setState(PipelineState::Playing));
    spin(300);
    h.engine.setVideoOutputEnabled(false);
    spin(100);
    const quint64 frozenSerial = h.mailbox.serial();
    spin(500);
    CHECK(h.mailbox.serial() == frozenSerial);
    // A new build starts enabled.
    h.mailbox.clear();
    CHECK(h.engine.build(localInput(), &error));
    CHECK(h.prerolled());
    CHECK(waitFor([&]() { return h.mailbox.latest().valid() && h.firstFrames == 2; }, 3000));
    h.engine.teardown();
}

// PLY-22: applyAudioValues() pushes EQ, boost, normalisation and limiter into
// the running chain without a rebuild; no pipeline = nothing happens.
static void ply22_engineAppliesAudioValuesLive() {
    EngineHarness h;
    h.engine.applyAudioValues(AudioChainValues());             // no pipeline: no-op
    forgetSink();
    QString error;
    CHECK(h.engine.build(dualInput(), &error));
    CHECK(h.playing());
    CHECK(waitFor(&chainReachable, 3000));
    CHECK(std::fabs(doubleProperty(elementnames::kAudioBoost, "volume") - 1.0) < 1e-9);
    CHECK(std::fabs(doubleProperty(elementnames::kAudioEq, "band0")) < 1e-9);
    const quint64 generation = h.engine.generation();
    AudioSettings settings;
    settings.boostPercent = 300;
    settings.eqGainsDb[0] = 6;
    settings.eqGainsDb[9] = -20;                               // clamped to -12
    settings.normalizationEnabled = true;
    StreamLoudness loud;
    loud.known = true;
    loud.loudnessDb = 6;                                       // 6 dB too loud: about 0.5
    const AudioChainValues values = computeAudioChainValues(settings, loud);
    h.engine.applyAudioValues(values);
    CHECK(std::fabs(doubleProperty(elementnames::kAudioBoost, "volume") - 3.0) < 1e-6);
    CHECK(std::fabs(doubleProperty(elementnames::kAudioEq, "band0") - 6.0) < 1e-6);
    CHECK(std::fabs(doubleProperty(elementnames::kAudioEq, "band9") + 12.0) < 1e-6);
    CHECK(std::fabs(doubleProperty(elementnames::kAudioLoudness, "volume") - values.loudnessGain) < 1e-6);
    CHECK(values.loudnessGain < 0.6 && values.loudnessGain > 0.4);
    if (h.engine.plan().limiterFactory == "rglimiter") CHECK(boolProperty(elementnames::kAudioLimiter, "enabled") == 1);
    CHECK(h.engine.generation() == generation && h.engine.built());  // live, no rebuild
    // Back to neutral.
    h.engine.applyAudioValues(computeAudioChainValues(AudioSettings(), StreamLoudness()));
    CHECK(std::fabs(doubleProperty(elementnames::kAudioBoost, "volume") - 1.0) < 1e-6);
    CHECK(std::fabs(doubleProperty(elementnames::kAudioLoudness, "volume") - 1.0) < 1e-6);
    if (h.engine.plan().limiterFactory == "rglimiter") CHECK(boolProperty(elementnames::kAudioLimiter, "enabled") == 0);
    qint64 a0 = 0, a1 = 0;
    CHECK(h.engine.queryPosition(Branch::Audio, &a0));
    spin(400);
    CHECK(h.engine.queryPosition(Branch::Audio, &a1) && a1 > a0);  // still playing
    // A degraded chain (no equalizer) skips the missing element.
    h.engine.teardown();
    forgetSink();
    PlanInput degraded = dualInput();
    degraded.available.erase("equalizer-10bands");
    CHECK(h.engine.build(degraded, &error));
    CHECK(h.playing());
    CHECK(waitFor(&chainReachable, 3000));
    h.engine.applyAudioValues(values);
    CHECK(std::fabs(doubleProperty(elementnames::kAudioBoost, "volume") - 3.0) < 1e-6);
    CHECK(doubleProperty(elementnames::kAudioEq, "band0") == -999);  // absent
    h.engine.teardown();
    forgetSink();
}

// ---- VideoPlayer ------------------------------------------------------------------------------

static PlayerSources playerSources(PlayMode mode) {
    PlayerSources p;
    p.mode = mode;
    p.videoId = "fixture";
    p.audioSink = fakeAudioSink();
    return p;
}

struct PlayerHarness {
    VideoPlayer player;
    int positionChanges = 0, failures = 0;
    std::vector<int> bufferingSeen;
    PlayerHarness() {
        QObject::connect(&player, &VideoPlayer::positionChanged, [this]() { ++positionChanges; });
        QObject::connect(&player, &VideoPlayer::bufferingChanged, [this]() { bufferingSeen.push_back(player.bufferingPercent()); });
        QObject::connect(&player, &VideoPlayer::failed, [this](const QString &r, bool, int, int) {
            ++failures;
            std::printf("  player failed: %s\n", qPrintable(r));
        });
    }
    bool start(const PlayerSources &p) {
        player.load(p);
        return waitFor([&]() { return player.state() == VideoPlayer::Playing || failures > 0; }, 10000) && failures == 0;
    }
    int changesDuring(int ms) {
        const int before = positionChanges;
        spin(ms);
        return positionChanges - before;
    }
};

// PLY-24: the position is polled every 250 ms with controls visible, every
// 1000 ms hidden, and not at all without a consumer (while playback goes on).
static void ply24_playerPollCadence() {
    PlayerHarness h;
    PlayerSources p = playerSources(PlayMode::Dual);
    p.video = file("v.webm");
    p.audio = file("a.webm");
    CHECK(h.start(p));
    spin(300);
    int n = h.changesDuring(2000);
    CHECK_MSG(n >= 6 && n <= 10, "visible: %d position updates in 2 s", n);
    h.player.setControlsVisible(false);
    n = h.changesDuring(3000);
    CHECK_MSG(n >= 2 && n <= 4, "hidden: %d position updates in 3 s", n);
    h.player.setPositionConsumer(false);
    spin(50);
    qint64 a0 = 0, a1 = 0;
    CHECK(h.player.sinkPosition(Branch::Audio, &a0));
    n = h.changesDuring(1500);
    CHECK_MSG(n == 0, "no consumer: %d position updates", n);
    CHECK(h.player.sinkPosition(Branch::Audio, &a1) && a1 - a0 >= 1000);  // playback continued
    CHECK(h.player.playing());
    h.player.setPositionConsumer(true);
    h.player.setControlsVisible(true);
    n = h.changesDuring(1000);
    CHECK_MSG(n >= 3, "consumer back: %d position updates in 1 s", n);
    CHECK(std::fabs(h.player.position() - double(a1)) < 2500);
    // Through QML-style invocation too.
    CHECK(QMetaObject::invokeMethod(&h.player, "setControlsVisible", Q_ARG(bool, false)));
    CHECK(QMetaObject::invokeMethod(&h.player, "setPositionConsumer", Q_ARG(bool, false)));
    CHECK(h.changesDuring(1200) == 0);
    h.player.stop();
}

// PLY-08: bufferingPercent is 100 at rest, follows the buffering messages of
// a network source within 0..100 and is 100 once playing.
static void ply08_playerBufferingPercent() {
    PlayerHarness h;
    CHECK(h.player.bufferingPercent() == 100);
    CHECK(h.player.property("bufferingPercent").toInt() == 100);
    PlayerSources p = playerSources(PlayMode::Dual);
    p.video = file("v.webm");
    p.audio = file("a.webm");
    p.video.uri = gServer->url("v.webm").toStdString();
    p.audio.uri = gServer->url("a.webm").toStdString();
    CHECK(h.start(p));
    CHECK(waitFor([&]() { return h.player.bufferingPercent() == 100 && !h.player.buffering(); }, 5000));
    bool inRange = true;
    for (size_t i = 0; i < h.bufferingSeen.size(); ++i) inRange = inRange && h.bufferingSeen[i] >= 0 && h.bufferingSeen[i] <= 100;
    CHECK(inRange);
    if (h.bufferingSeen.empty()) std::printf("  NOTE no buffering message from the HTTP source\n");
    else std::printf("  %d buffering updates, first %d%%\n", int(h.bufferingSeen.size()), h.bufferingSeen.front());
    CHECK(h.player.property("bufferingPercent").toInt() == 100);
    // A local file never buffers: the value stays at 100 after a reload.
    PlayerSources local = playerSources(PlayMode::Local);
    local.muxed = file("av.webm");
    h.bufferingSeen.clear();
    CHECK(h.start(local));
    CHECK(h.player.bufferingPercent() == 100);
    for (size_t i = 0; i < h.bufferingSeen.size(); ++i) CHECK(h.bufferingSeen[i] == 100);
    h.player.stop();
}

// PLY-20: the player's zero-copy sink is null unless a hardware build runs.
static void ply20_playerHardwareVideoSinkNullInSoftware() {
    PlayerHarness h;
    CHECK(h.player.hardwareVideoSink() == nullptr);            // nothing loaded
    PlayerSources p = playerSources(PlayMode::Local);
    p.muxed = file("av.webm");
    CHECK(h.start(p));
    CHECK(h.player.decodeMode() == int(DecodeMode::Software));
    CHECK(h.player.hardwareVideoSink() == nullptr);
    // Hardware asked for without droid elements: per-build fallback, still null.
    PlayerSources hw = p;
    hw.decode = DecodeMode::Hardware;
    hw.unavailableElements.insert("droidvdec");
    hw.unavailableElements.insert("droideglsink");
    CHECK(h.start(hw));
    CHECK(h.player.decodeMode() == int(DecodeMode::Software));
    CHECK(h.player.hardwareVideoSink() == nullptr);
    h.player.stop();
    CHECK(h.player.hardwareVideoSink() == nullptr);
}

// PLY-06 start seek before preroll, engine API: holdForStartSeek() holds each branch's first
// buffer at its decoder (no preroll while held), sendStartSeek() seeks from there and releases
// the branch (no flush reaches a sink, the first sink buffer is at the start), releaseStartHolds()
// lets held branches run unchanged, and a branch that already decoded reports branchNotHeld.
// teardown() returns at once; teardownDone() follows from the worker.
static void ply06_engineStartSeekHold() {
    EngineHarness h;
    int held = 0, notHeld = 0, segments = 0, teardowns = 0;
    qint64 videoLanded = -1;
    QObject::connect(&h.engine, &GstEngine::decoderHeld, [&](int) { ++held; });
    QObject::connect(&h.engine, &GstEngine::branchNotHeld, [&](int) { ++notHeld; });
    QObject::connect(&h.engine, &GstEngine::startSegment, [&](int branch, quint32, qint64 ms) {
        ++segments;
        if (branch == int(Branch::Video)) videoLanded = ms;
    });
    QObject::connect(&h.engine, &GstEngine::teardownDone, [&](qint64) { ++teardowns; });
    SeekCommand c;
    c.branch = Branch::Video;
    c.flags = seekFlagsFor(Container::Matroska);
    c.positionNs = msToNs(3000);
    // No pipeline: nothing to hold or seek.
    h.engine.holdForStartSeek();
    CHECK(!h.engine.sendStartSeek(c));
    h.engine.releaseStartHolds();

    QString error;
    CHECK(h.engine.build(dualInput(), &error));
    h.engine.holdForStartSeek();
    CHECK(h.engine.setState(PipelineState::Paused));
    CHECK(waitFor([&]() { return held == 2; }, 8000));
    spin(200);
    CHECK(h.asyncDones == 0);                       // held: no preroll at 0
    SeekCommand muxed = c;
    muxed.branch = Branch::Muxed;
    CHECK(!h.engine.sendStartSeek(muxed));          // a dual build has no muxed branch
    quint32 vseq = 0, aseq = 0;
    CHECK(h.engine.sendStartSeek(c, &vseq) && vseq != 0);
    CHECK(!h.engine.sendStartSeek(c));              // released already
    c.branch = Branch::Audio;
    CHECK(h.engine.sendStartSeek(c, &aseq) && aseq != 0 && aseq != vseq);
    CHECK(waitFor([&]() { return h.asyncDones > 0; }, 8000));
    CHECK(waitFor([&]() { return segments == 2; }, 2000));
    CHECK_MSG(std::llabs(videoLanded - 3000) <= 100, "video landed %lld", (long long)videoLanded);
    EngineStats st = h.engine.stats();
    CHECK_MSG(st.firstVideoSinkMs >= 2900, "first sink buffer %lld", (long long)st.firstVideoSinkMs);
    CHECK(st.videoSinkFlushes == 0 && st.audioSinkFlushes == 0);
    CHECK(st.videoDecoder == "vp8dec" && st.audioDecoder == "opusdec");
    QElapsedTimer t;
    t.start();
    h.engine.teardown();
    const qint64 teardownUs = t.nsecsElapsed() / 1000;
    CHECK_MSG(teardownUs < 5000, "teardown took %lld us", (long long)teardownUs);
    CHECK(waitFor([&]() { return teardowns == 1; }, 5000));

    // Released unchanged: the preroll is at 0.
    held = 0;
    CHECK(h.engine.build(dualInput(), &error));
    h.engine.holdForStartSeek();
    const int dones = h.asyncDones;
    CHECK(h.engine.setState(PipelineState::Paused));
    CHECK(waitFor([&]() { return held == 2; }, 8000));
    h.engine.releaseStartHolds();
    CHECK(waitFor([&]() { return h.asyncDones > dones; }, 8000));
    st = h.engine.stats();
    CHECK_MSG(st.firstVideoSinkMs >= 0 && st.firstVideoSinkMs < 100, "first sink buffer %lld", (long long)st.firstVideoSinkMs);
    h.engine.teardown();

    // Too late: both branches decoded already.
    CHECK(h.engine.build(dualInput(), &error));
    CHECK(h.prerolled());
    h.engine.holdForStartSeek();
    CHECK(waitFor([&]() { return notHeld == 2; }, 3000));
    c.branch = Branch::Video;
    CHECK(!h.engine.sendStartSeek(c));
    h.engine.teardown();
    CHECK(waitFor([&]() { return teardowns == 3; }, 5000));
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
        std::printf("player api engine tests: SKIPPED (gst_init failed: %s)\n", qPrintable(err));
        return 0;
    }
    const std::set<std::string> have = GstEngine::availableElements();
    const char *const needed[] = {"uridecodebin", "souphttpsrc", "matroskademux", "videoconvert", "appsink", "fakesink",
                                  "volume", "equalizer-10bands"};
    for (size_t i = 0; i < sizeof(needed) / sizeof(needed[0]); ++i) {
        if (!have.count(needed[i])) {
            std::printf("player api engine tests: SKIPPED (missing %s)\n", needed[i]);
            return 0;
        }
    }
    const QByteArray tmp = qgetenv("TMPDIR").isEmpty() ? QByteArray("/tmp") : qgetenv("TMPDIR");
    gDir = QString::fromLocal8Bit(tmp) + QStringLiteral("/sailtube-player-api-%1").arg(QCoreApplication::applicationPid());
    QDir().mkpath(gDir);
    if (!makeFixtures()) {
        std::printf("FAIL fixture generation\n");
        QDir(gDir).removeRecursively();
        return 1;
    }
    HttpServer server;
    if (!server.start()) {
        std::printf("FAIL http server\n");
        QDir(gDir).removeRecursively();
        return 1;
    }
    gServer = &server;
    installHook();

    std::printf("ply01_engineLifecycle\n");
    ply01_engineLifecycle();
    std::printf("ply03_engineSeekReachesItsSink\n");
    ply03_engineSeekReachesItsSink();
    std::printf("ply02_engineStatsAndReset\n");
    ply02_engineStatsAndReset();
    std::printf("ply06_engineVideoOutputGate\n");
    ply06_engineVideoOutputGate();
    std::printf("ply06_engineStartSeekHold\n");
    ply06_engineStartSeekHold();
    std::printf("ply22_engineAppliesAudioValuesLive\n");
    ply22_engineAppliesAudioValuesLive();
    std::printf("ply24_playerPollCadence\n");
    ply24_playerPollCadence();
    std::printf("ply08_playerBufferingPercent\n");
    ply08_playerBufferingPercent();
    std::printf("ply20_playerHardwareVideoSinkNullInSoftware\n");
    ply20_playerHardwareVideoSinkNullInSoftware();

    forgetSink();
    QDir(gDir).removeRecursively();
    std::printf("player api engine tests: %d checks, %d failure%s\n", checks, failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
