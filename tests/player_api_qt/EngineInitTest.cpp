// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// NFR-PERF-01 / NFR-PERF-02: GStreamer start-up off the GUI thread and the decoder preload
// (GstEngine::startInitialize, waitInitialized, preloadDecoders, GstEngineInit). gst_init runs
// once per process, so every check that needs an uninitialised GStreamer starts in a fresh
// process: the parent (the test proper) and three child modes of this binary:
//   --sync                  initialize() on the GUI thread (reference timing)
//   --responsive            startInitialize() + preloadDecoders() while a 1 ms GUI timer runs
//   --load-first <dir>      VideoPlayer constructed and load()ed before the init finished
// Children print "child: key=value ..." and exit non-zero on a failed check.
// Run with GOBJECT_DEBUG=instance-count (run.sh) for the "no element created" check.
// Build: tests/player_api_qt/run.sh (part "init").
#include "Core/Player/Infrastructure/GstEngine.h"
#include "Core/Player/Infrastructure/VideoFrameMailbox.h"
#include "Core/Player/Infrastructure/VideoPlayer.h"

#include <gst/gst.h>

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QProcess>
#include <QThread>
#include <QTimer>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>

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
        QThread::msleep(1);
    }
}

static bool waitFor(const std::function<bool()> &pred, int timeoutMs) {
    QElapsedTimer t;
    t.start();
    while (!pred()) {
        if (t.elapsed() > timeoutMs) return false;
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(1);
    }
    return true;
}

static long long usSince(const QElapsedTimer &t) { return static_cast<long long>(t.nsecsElapsed() / 1000); }

// ---- Fixture and inputs ---------------------------------------------------------------

static QString gDir;

static bool makeFixture(const QString &dir) {
    const QString d = QStringLiteral(
        "webmmux name=m ! filesink location=%1/av.webm "
        "videotestsrc num-buffers=60 pattern=ball ! video/x-raw,width=320,height=180,framerate=30/1 ! "
        "vp8enc deadline=1 keyframe-max-dist=30 ! m. "
        "audiotestsrc num-buffers=100 samplesperbuffer=960 wave=sine ! audio/x-raw,rate=48000,channels=2 ! "
        "opusenc ! m.").arg(dir);
    GError *err = nullptr;
    GstElement *p = gst_parse_launch(d.toUtf8().constData(), &err);
    if (!p || err) {
        std::printf("fixture parse error: %s\n", err ? err->message : "?");
        if (err) g_error_free(err);
        if (p) gst_object_unref(p);
        return false;
    }
    gst_element_set_state(p, GST_STATE_PLAYING);
    GstBus *bus = gst_element_get_bus(p);
    GstMessage *m = gst_bus_timed_pop_filtered(bus, 60 * GST_SECOND, GstMessageType(GST_MESSAGE_EOS | GST_MESSAGE_ERROR));
    const bool ok = m && GST_MESSAGE_TYPE(m) == GST_MESSAGE_EOS;
    if (m) gst_message_unref(m);
    gst_object_unref(bus);
    gst_element_set_state(p, GST_STATE_NULL);
    gst_object_unref(p);
    return ok;
}

static SinkOverride fakeAudioSink() {
    SinkOverride o;
    o.factory = "fakesink";
    PropertySetting sync;
    sync.name = "sync";
    sync.value = "true";
    o.properties.push_back(sync);
    return o;
}

static SourceSpec localFile(const QString &dir) {
    SourceSpec s;
    s.uri = ("file://" + dir + "/av.webm").toStdString();
    s.container = Container::Matroska;
    return s;
}

// The plan's inputs without asking GStreamer (availableElements() would wait itself), so
// build() is the first call that needs it.
static PlanInput localInputWithoutRegistry(const QString &dir) {
    PlanInput in;
    in.mode = PlayMode::Local;
    in.muxed = localFile(dir);
    const char *const names[] = {"uridecodebin", "filesrc", "matroskademux", "videoconvert", "appsink",
                                 "fakesink", "audioconvert", "audioresample"};
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i) in.available.insert(names[i]);
    in.audioSink = fakeAudioSink();
    return in;
}

// ---- Registry observations -------------------------------------------------------------

// Plugin of a registry feature is loaded (dlopen + plugin_init done). -1 = feature absent.
static int pluginLoaded(const std::string &name) {
    GstPluginFeature *f = gst_registry_lookup_feature(gst_registry_get(), name.c_str());
    if (!f) return -1;
    int loaded = 0;
    if (GstPlugin *plugin = gst_plugin_feature_get_plugin(f)) {
        loaded = gst_plugin_is_loaded(plugin) ? 1 : 0;
        gst_object_unref(plugin);
    } else {
        loaded = 1;  // a static feature (core elements registered without a plugin file)
    }
    gst_object_unref(f);
    return loaded;
}

static bool instanceCountingOn() {
    const char *v = std::getenv("GOBJECT_DEBUG");
    return v && std::strstr(v, "instance-count");
}

// Live GstElement instances (pipelines and bins included) of every loaded element type.
static long long liveElements() {
    long long n = g_type_get_instance_count(GST_TYPE_PIPELINE) + g_type_get_instance_count(GST_TYPE_BIN);
    GList *list = gst_registry_get_feature_list(gst_registry_get(), GST_TYPE_ELEMENT_FACTORY);
    for (GList *l = list; l; l = l->next) {
        // 0 for a factory whose plugin is not loaded (no instance can exist).
        const GType type = gst_element_factory_get_element_type(GST_ELEMENT_FACTORY(l->data));
        if (type) n += g_type_get_instance_count(type);
    }
    gst_plugin_feature_list_free(list);
    return n;
}

// ---- Notifier record ---------------------------------------------------------------------

struct InitRecord {
    int ready = 0, preloaded = 0;
    bool readyOk = false, readyOnGui = false, preloadOnGui = false;
    QString readyError;
    qint64 readyMs = -1, preloadMs = -1;
    int preloadLoaded = -1;
    qint64 readyAtMs = -1, preloadAtMs = -1;
    QElapsedTimer clock;
    InitRecord() {
        clock.start();
        GstEngineInit *n = GstEngineInit::instance();
        QObject::connect(n, &GstEngineInit::ready, [this](bool ok, const QString &error, qint64 ms) {
            ++ready;
            readyOk = ok;
            readyError = error;
            readyMs = ms;
            readyAtMs = clock.elapsed();
            readyOnGui = QThread::currentThread() == QCoreApplication::instance()->thread();
        });
        QObject::connect(n, &GstEngineInit::decodersPreloaded, [this](int loaded, qint64 ms) {
            ++preloaded;
            preloadLoaded = loaded;
            preloadMs = ms;
            preloadAtMs = clock.elapsed();
            preloadOnGui = QThread::currentThread() == QCoreApplication::instance()->thread();
        });
    }
};

// ---- Child modes ---------------------------------------------------------------------

// Reference: the old start-up, initialize() on the GUI thread.
static int childSync() {
    QElapsedTimer t;
    t.start();
    QString error;
    const bool ok = GstEngine::initialize(&error);
    std::printf("child: mode=sync ok=%d gui-blocked-ms=%lld\n", ok, static_cast<long long>(t.elapsed()));
    return ok ? 0 : 1;
}

// startInitialize() + preloadDecoders(): the GUI thread keeps running a 1 ms timer.
static int childResponsive() {
    InitRecord r;
    QElapsedTimer tick;
    qint64 maxGap = 0;
    QTimer timer;
    timer.setTimerType(Qt::PreciseTimer);
    timer.setInterval(1);
    QObject::connect(&timer, &QTimer::timeout, [&]() {
        maxGap = qMax(maxGap, tick.elapsed());
        tick.restart();
    });
    tick.start();
    timer.start();
    QElapsedTimer call;
    call.start();
    GstEngine::startInitialize();
    const long long startUs = usSince(call);
    const bool ready = waitFor([&]() { return r.ready > 0; }, 30000);
    const qint64 initGap = maxGap;
    maxGap = 0;
    tick.restart();
    call.restart();
    GstEngine::preloadDecoders();
    const long long preloadUs = usSince(call);
    const bool preloaded = waitFor([&]() { return r.preloaded > 0; }, 30000);
    std::printf("child: mode=responsive start-call-us=%lld ready-after-ms=%lld init-ms=%lld max-gui-gap-init-ms=%lld "
                "preload-call-us=%lld preload-ms=%lld preload-loaded=%d max-gui-gap-preload-ms=%lld\n",
                startUs, static_cast<long long>(r.readyAtMs), static_cast<long long>(r.readyMs),
                static_cast<long long>(initGap), preloadUs, static_cast<long long>(r.preloadMs), r.preloadLoaded,
                static_cast<long long>(maxGap));
    int bad = 0;
    if (!ready || !r.readyOk) { std::printf("FAIL child responsive: ready %d ok %d\n", ready, r.readyOk); ++bad; }
    if (!preloaded) { std::printf("FAIL child responsive: no decodersPreloaded\n"); ++bad; }
    if (startUs >= 5000 || preloadUs >= 5000) { std::printf("FAIL child responsive: a call blocked\n"); ++bad; }
    // The GUI thread never waits for gst_init or the plugin loads (generous bound for a loaded host).
    if (initGap >= 100 || maxGap >= 100) { std::printf("FAIL child responsive: GUI gap %lld / %lld ms\n",
                                                       static_cast<long long>(initGap), static_cast<long long>(maxGap)); ++bad; }
    return bad ? 1 : 0;
}

// VideoPlayer::load() right after startInitialize(): it waits for the init, then plays.
static int childLoadFirst(const QString &dir) {
    int bad = 0;
    VideoPlayer *player = new VideoPlayer;
    if (gst_is_initialized()) { std::printf("FAIL child load-first: the VideoPlayer constructor initialised GStreamer\n"); ++bad; }
    InitRecord r;
    int failed = 0, frames = 0;
    QObject::connect(player, &VideoPlayer::failed, [&](const QString &reason, bool, int, int) {
        ++failed;
        std::printf("  player failed: %s\n", qPrintable(reason));
    });
    QObject::connect(player, &VideoPlayer::firstFrame, [&]() { ++frames; });
    GstEngine::startInitialize();
    PlayerSources p;
    p.mode = PlayMode::Local;
    p.videoId = "fixture";
    p.muxed = localFile(dir);
    p.audioSink = fakeAudioSink();
    QElapsedTimer t;
    t.start();
    player->load(p);  // first GStreamer user: waits inside
    const qint64 loadMs = t.elapsed();
    const bool readyBeforeLoadReturned = r.ready > 0;  // no event processed yet: must be 0
    const bool playing = waitFor([&]() { return player->state() == VideoPlayer::Playing || failed > 0; }, 10000)
        && failed == 0;
    const bool framed = waitFor([&]() { return frames > 0 || failed > 0; }, 5000) && frames > 0;
    std::printf("child: mode=load-first load-call-ms=%lld playing-after-ms=%lld playing=%d frame=%d\n",
                static_cast<long long>(loadMs), static_cast<long long>(t.elapsed()), playing, framed);
    if (readyBeforeLoadReturned) { std::printf("FAIL child load-first: ready delivered inside load()\n"); ++bad; }
    if (!GstEngine::initialized()) { std::printf("FAIL child load-first: not initialised after load()\n"); ++bad; }
    if (!playing || !framed) { std::printf("FAIL child load-first: no playback\n"); ++bad; }
    player->stop();
    delete player;
    return bad ? 1 : 0;
}

// Runs this binary in a child mode; returns its exit code and its "child:" keys.
static int runChild(const QStringList &args, std::map<std::string, long long> *values) {
    QProcess child;
    child.setProcessChannelMode(QProcess::MergedChannels);
    child.start(QCoreApplication::applicationFilePath(), args);
    if (!child.waitForFinished(90000)) {
        child.kill();
        child.waitForFinished(5000);
        std::printf("FAIL child %s: timeout\n", qPrintable(args.join(QLatin1Char(' '))));
        return -1;
    }
    const QList<QByteArray> lines = child.readAll().split('\n');
    for (int i = 0; i < lines.size(); ++i) {
        const QByteArray line = lines[i].trimmed();
        if (line.isEmpty()) continue;
        std::printf("  [%s] %s\n", qPrintable(args.value(0)), line.constData());
        if (!line.startsWith("child:")) continue;
        const QList<QByteArray> parts = line.mid(6).trimmed().split(' ');
        for (int k = 0; k < parts.size(); ++k) {
            const int eq = parts[k].indexOf('=');
            if (eq > 0) (*values)[parts[k].left(eq).toStdString()] = parts[k].mid(eq + 1).toLongLong();
        }
    }
    return child.exitStatus() == QProcess::NormalExit ? child.exitCode() : -1;
}

// ---- Tests (parent process) -----------------------------------------------------------

struct EngineHarness {
    VideoFrameMailbox mailbox;
    GstEngine engine;
    int asyncDones = 0, errors = 0, teardowns = 0;
    EngineHarness() : engine(&mailbox) {
        QObject::connect(&engine, &GstEngine::asyncDone, [this]() { ++asyncDones; });
        QObject::connect(&engine, &GstEngine::teardownDone, [this](qint64) { ++teardowns; });
        QObject::connect(&engine, &GstEngine::errorOccurred, [this](const QString &m, const QString &, int, bool, int) {
            ++errors;
            std::printf("  engine error: %s\n", qPrintable(m));
        });
    }
};

struct HookRecord {
    bool ran = false, onWorker = false, gstReady = false;
    int readyCountAtHook = -1;
};

// NFR-PERF-01: startInitialize() returns in < 5 ms with no GStreamer work on the caller; build()
// called before ready waits for the init and then works (prerolls a fixture); ready arrives once,
// on the GUI thread, after the afterInit job (which ran on the worker with GStreamer ready).
static void nfrPerf01_startInitializeOffTheGuiThread(InitRecord &r, HookRecord &hook) {
    CHECK(!gst_is_initialized());
    CHECK(!GstEngine::initialized());
    QThread *gui = QThread::currentThread();
    InitRecord *record = &r;
    HookRecord *h = &hook;
    QElapsedTimer t;
    t.start();
    GstEngine::startInitialize([gui, record, h]() {
        h->ran = true;
        h->onWorker = QThread::currentThread() != gui;
        h->gstReady = gst_is_initialized() && GstEngine::initialized();
        h->readyCountAtHook = record->ready;
    });
    const long long startUs = usSince(t);
    CHECK_MSG(startUs < 5000, "startInitialize took %lld us", startUs);
    std::printf("  startInitialize returned after %lld us (initialized=%d)\n", startUs, GstEngine::initialized());

    // build() is the first GStreamer user; no event is processed before it returns.
    EngineHarness e;
    QString error;
    t.restart();
    const bool built = e.engine.build(localInputWithoutRegistry(gDir), &error);
    std::printf("  build() before ready returned after %lld ms (waited for the init)\n",
                static_cast<long long>(t.elapsed()));
    CHECK_MSG(built, "%s", qPrintable(error));
    CHECK(GstEngine::initialized());
    CHECK(GstEngine::waitInitialized(0));
    CHECK(r.ready == 0);  // delivered through the event loop only

    // ...and the pipeline works once the file exists.
    CHECK(makeFixture(gDir));
    if (built) {
        CHECK(e.engine.setState(PipelineState::Paused));
        CHECK(waitFor([&]() { return e.asyncDones > 0 || e.errors > 0; }, 8000) && e.errors == 0);
    }
    CHECK(waitFor([&]() { return r.ready > 0; }, 10000));
    CHECK(r.ready == 1);
    CHECK(r.readyOk);
    CHECK(r.readyError.isEmpty());
    CHECK(r.readyOnGui);
    CHECK(r.readyMs >= 0);
    CHECK(hook.ran);
    CHECK(hook.onWorker);
    CHECK(hook.gstReady);
    CHECK(hook.readyCountAtHook == 0);  // afterInit runs before ready
    std::printf("  ready ok=%d init-ms=%lld\n", r.readyOk, static_cast<long long>(r.readyMs));
    e.engine.teardown();
    CHECK(waitFor([&]() { return e.teardowns > 0; }, 5000));
}

// NFR-PERF-01: repeated startInitialize() calls are no-ops: no second ready, no second hook.
static void nfrPerf01_startInitializeIsIdempotent(InitRecord &r) {
    bool secondHook = false;
    GstEngine::startInitialize([&secondHook]() { secondHook = true; });
    GstEngine::startInitialize();
    spin(300);
    CHECK(r.ready == 1);
    CHECK(!secondHook);
    QString error;
    CHECK(GstEngine::initialize(&error));  // synchronous form still works (idempotent)
    CHECK(GstEngine::initialized());
    CHECK(!GstEngine::availableElements().empty());
}

// NFR-PERF-02: preloadDecoders() loads the plugin of every present preloadFeatures() entry on
// the worker, creates no element or pipeline, reports once on the GUI thread; repeats are no-ops.
static void nfrPerf02_preloadDecodersLoadsPresentFactories(InitRecord &r) {
    const std::vector<std::string> names = GstEngine::preloadFeatures();
    CHECK(names.size() >= 20);
    int present = 0, unloadedBefore = 0;
    std::string unloadedNames, missing;
    for (size_t i = 0; i < names.size(); ++i) {
        const int s = pluginLoaded(names[i]);
        if (s < 0) { missing += " " + names[i]; continue; }
        ++present;
        if (s == 0) { ++unloadedBefore; unloadedNames += " " + names[i]; }
    }
    std::printf("  preload: %d of %zu present; not loaded before:%s\n  missing (skipped):%s\n", present, names.size(),
                unloadedNames.empty() ? " none" : unloadedNames.c_str(), missing.empty() ? " none" : missing.c_str());
    const bool counting = instanceCountingOn();
    long long before = counting ? liveElements() : 0;
    QElapsedTimer t;
    t.start();
    GstEngine::preloadDecoders();
    GstEngine::preloadDecoders();  // no-op
    const long long callUs = usSince(t);
    CHECK_MSG(callUs < 5000, "preloadDecoders calls took %lld us", callUs);
    CHECK(waitFor([&]() { return r.preloaded > 0; }, 20000));
    spin(300);
    GstEngine::preloadDecoders();  // still a no-op afterwards
    spin(200);
    CHECK(r.preloaded == 1);
    CHECK(r.preloadOnGui);
    CHECK_MSG(r.preloadLoaded == present, "loaded %d, present %d", r.preloadLoaded, present);
    int stillUnloaded = 0;
    for (size_t i = 0; i < names.size(); ++i) {
        if (pluginLoaded(names[i]) == 0) {
            ++stillUnloaded;
            std::printf("  not loaded after preload: %s\n", names[i].c_str());
        }
    }
    CHECK(stillUnloaded == 0);
    std::printf("  preload: loaded=%d ms=%lld (calls %lld us)\n", r.preloadLoaded, static_cast<long long>(r.preloadMs),
                callUs);
    if (counting) {
        const long long after = liveElements();
        CHECK_MSG(after == before, "live elements %lld -> %lld", before, after);
        // The counter itself works: one element is seen.
        GstElement *probe = gst_element_factory_make("fakesink", nullptr);
        CHECK(probe && liveElements() == after + 1);
        if (probe) gst_object_unref(probe);
        CHECK(liveElements() == after);
    } else {
        std::printf("  (GOBJECT_DEBUG=instance-count not set: element count check skipped)\n");
    }
}

// NFR-PERF-01/02 host timings and the GUI-thread paths that needed GStreamer, each in a fresh
// process: synchronous reference, responsiveness during init + preload, VideoPlayer::load first.
static void nfrPerf01_freshProcessTimingsAndLoadFirst() {
    std::map<std::string, long long> sync, responsive, loadFirst;
    CHECK(runChild(QStringList() << QStringLiteral("--sync"), &sync) == 0);
    CHECK(runChild(QStringList() << QStringLiteral("--responsive"), &responsive) == 0);
    CHECK(runChild(QStringList() << QStringLiteral("--load-first") << gDir, &loadFirst) == 0);
    CHECK(responsive.count("start-call-us") && responsive["start-call-us"] < 5000);
    CHECK(loadFirst.count("playing") && loadFirst["playing"] == 1);
    std::printf("  timings: GUI blocked by initialize() on the GUI thread %lld ms; with startInitialize() the "
                "call takes %lld us, init on the worker %lld ms, worst GUI gap %lld ms; preload %lld ms "
                "(worst GUI gap %lld ms); load() before ready blocked %lld ms\n",
                sync["gui-blocked-ms"], responsive["start-call-us"], responsive["init-ms"],
                responsive["max-gui-gap-init-ms"], responsive["preload-ms"], responsive["max-gui-gap-preload-ms"],
                loadFirst["load-call-ms"]);
}

int main(int argc, char **argv) {
    std::setvbuf(stdout, nullptr, _IOLBF, 0);
    QCoreApplication app(argc, argv);
    const QStringList args = app.arguments();
    if (args.value(1) == QLatin1String("--sync")) return childSync();
    if (args.value(1) == QLatin1String("--responsive")) return childResponsive();
    if (args.value(1) == QLatin1String("--load-first")) return childLoadFirst(args.value(2));

    const QByteArray tmp = qgetenv("TMPDIR").isEmpty() ? QByteArray("/tmp") : qgetenv("TMPDIR");
    gDir = QString::fromLocal8Bit(tmp) + QStringLiteral("/sailtube-engine-init-%1").arg(QCoreApplication::applicationPid());
    QDir().mkpath(gDir);

    InitRecord record;
    HookRecord hook;
    std::printf("nfrPerf01_startInitializeOffTheGuiThread\n");
    nfrPerf01_startInitializeOffTheGuiThread(record, hook);
    if (!record.readyOk) {
        // No usable GStreamer on this host: nothing below can run.
        QDir(gDir).removeRecursively();
        std::printf("engine init tests: SKIPPED (gst_init failed: %s)\n", qPrintable(record.readyError));
        return 0;
    }
    std::printf("nfrPerf01_startInitializeIsIdempotent\n");
    nfrPerf01_startInitializeIsIdempotent(record);
    std::printf("nfrPerf02_preloadDecodersLoadsPresentFactories\n");
    nfrPerf02_preloadDecodersLoadsPresentFactories(record);
    std::printf("nfrPerf01_freshProcessTimingsAndLoadFirst\n");
    nfrPerf01_freshProcessTimingsAndLoadFirst();

    QDir(gDir).removeRecursively();
    std::printf("engine init tests: %d checks, %d failure%s\n", checks, failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
