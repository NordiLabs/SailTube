// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// PLY-20 host tests of the per-device hardware-decode probe and the hardware render path, with
// fake "droidvdec" / "droideglsink" factories registered in a static plugin (no droid elements,
// no GL):
//   droidvdec      a bin around vp9dec (decodes), identity (passes the parsed stream), identity
//                  error-after=1 (decoder error) or a stall (swallows buffers and EOS);
//   droideglsink   fakesink with the "show-frame" signal of the buffer-mode zero-copy sink.
// Probe: pass / error / timeout, the bundled H.264 clip demuxes to the stop caps, the verdict is
// persisted per build. Player: a hardware build renders through EglSinkBridge; a stalled hardware
// decoder falls back to software within the 3 s watchdog. Build: tests/player_hw/run.sh
#include "Core/Player/Domain/HardwareProbe.h"
#include "Core/Player/Infrastructure/GstEngine.h"
#include "Core/Player/Infrastructure/HardwareDecodeGate.h"
#include "Core/Player/Infrastructure/HardwareProbeClip.h"
#include "Core/Player/Infrastructure/HardwareProbeRunner.h"
#include "Core/Player/Infrastructure/VideoPlayer.h"

#include <gst/gst.h>

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QThread>
#include <QUrl>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <functional>
#include <unistd.h>

using namespace nordtile::player;

static int checks = 0, failures = 0;
#define CHECK(c) do { ++checks; if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

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

// ---- Fake droid elements -----------------------------------------------------------------

enum FakeMode { kDecode, kPass, kError, kStall };
static std::atomic<int> gMode(kDecode);
static std::atomic<int> gDecodersMade(0);

static GstPadProbeReturn dropAll(GstPad *, GstPadProbeInfo *info, gpointer) {
    if (GST_PAD_PROBE_INFO_TYPE(info) & GST_PAD_PROBE_TYPE_BUFFER) return GST_PAD_PROBE_DROP;
    GstEvent *e = GST_PAD_PROBE_INFO_EVENT(info);
    return e && GST_EVENT_TYPE(e) == GST_EVENT_EOS ? GST_PAD_PROBE_DROP : GST_PAD_PROBE_OK;
}

static void ghost(GstElement *bin, GstElement *inner, const char *name) {
    GstPad *target = gst_element_get_static_pad(inner, name);
    gst_element_add_pad(bin, gst_ghost_pad_new(name, target));
    gst_object_unref(target);
}

static void fdInit(GTypeInstance *instance, gpointer) {
    ++gDecodersMade;
    GstElement *bin = GST_ELEMENT(instance);
    const int mode = gMode.load();
    GstElement *inner = gst_element_factory_make(mode == kDecode ? "vp9dec" : "identity", nullptr);
    if (mode == kError) g_object_set(inner, "error-after", 1, nullptr);
    gst_bin_add(GST_BIN(bin), inner);
    ghost(bin, inner, "sink");
    ghost(bin, inner, "src");
    if (mode == kStall) {
        GstPad *src = gst_element_get_static_pad(inner, "src");
        gst_pad_add_probe(src, GstPadProbeType(GST_PAD_PROBE_TYPE_BUFFER | GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM), dropAll,
                          nullptr, nullptr);
        gst_object_unref(src);
    }
}

static GType registerDerived(GType parent, const char *name, GClassInitFunc classInit, GInstanceInitFunc init) {
    GTypeQuery q;
    g_type_query(parent, &q);
    GTypeInfo info;
    std::memset(&info, 0, sizeof(info));
    info.class_size = static_cast<guint16>(q.class_size);
    info.class_init = classInit;
    info.instance_size = static_cast<guint16>(q.instance_size);
    info.instance_init = init;
    return g_type_register_static(parent, name, &info, GTypeFlags(0));
}

static guint gShowFrame = 0;
static std::atomic<int> gShown(0);

static void fsClassInit(gpointer klass, gpointer) {
    const GType t = G_TYPE_FROM_CLASS(klass);
    gShowFrame = g_signal_new("show-frame", t, G_SIGNAL_RUN_LAST, 0, nullptr, nullptr, nullptr, G_TYPE_NONE, 1, GST_TYPE_BUFFER);
    g_signal_new("buffers-invalidated", t, G_SIGNAL_RUN_LAST, 0, nullptr, nullptr, nullptr, G_TYPE_NONE, 0);
}
static void fsHandoff(GstElement *sink, GstBuffer *buffer, GstPad *, gpointer) {
    ++gShown;
    g_signal_emit(sink, gShowFrame, 0, buffer);
}
static void fsInit(GTypeInstance *instance, gpointer) {
    g_object_set(instance, "signal-handoffs", TRUE, nullptr);
    g_signal_connect(instance, "handoff", G_CALLBACK(fsHandoff), nullptr);
    g_signal_connect(instance, "preroll-handoff", G_CALLBACK(fsHandoff), nullptr);
}

static GType parentOf(const char *factory) {
    GstElementFactory *f = gst_element_factory_find(factory);
    if (!f) return 0;
    GstPluginFeature *loaded = gst_plugin_feature_load(GST_PLUGIN_FEATURE(f));
    gst_object_unref(f);
    if (!loaded) return 0;
    const GType t = gst_element_factory_get_element_type(GST_ELEMENT_FACTORY(loaded));
    gst_object_unref(loaded);
    return t;
}

static gboolean fakePluginInit(GstPlugin *plugin) {
    const GType sinkParent = parentOf("fakesink");
    if (!sinkParent) return FALSE;
    const GType decoder = registerDerived(GST_TYPE_BIN, "SailTubeFakeDroidVDec", nullptr, fdInit);
    const GType sink = registerDerived(sinkParent, "SailTubeFakeDroidEglSink", fsClassInit, fsInit);
    return gst_element_register(plugin, "droidvdec", GST_RANK_NONE, decoder) &&
           gst_element_register(plugin, "droideglsink", GST_RANK_NONE, sink);
}

// ---- Fixtures ------------------------------------------------------------------------------

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
    GstMessage *m = gst_bus_timed_pop_filtered(bus, 60 * GST_SECOND, GstMessageType(GST_MESSAGE_EOS | GST_MESSAGE_ERROR));
    const bool ok = m && GST_MESSAGE_TYPE(m) == GST_MESSAGE_EOS;
    if (m) gst_message_unref(m);
    gst_object_unref(bus);
    gst_element_set_state(p, GST_STATE_NULL);
    gst_object_unref(p);
    return ok;
}

static QString fileUri(const QString &name) { return QUrl::fromLocalFile(gDir + QLatin1Char('/') + name).toString(); }

// ---- Probe -----------------------------------------------------------------------------------

// Counts start() calls of the real runner.
class CountingRunner final : public HardwareProbeRunner {
public:
    GstHardwareProbeRunner real;
    int starts = 0;
    bool start(const std::string &uri, const Listener &l, std::string *why) override {
        ++starts;
        return real.start(uri, l, why);
    }
    void stop() override { real.stop(); }
};

struct MemoryStore {
    QString line;
    int saves = 0;
    HardwareDecodeGate::Storage storage() {
        HardwareDecodeGate::Storage s;
        s.load = [this]() { return line; };
        s.save = [this](const QString &v) { line = v; ++saves; };
        return s;
    }
};

static HardwareDecodeGate::Options options(MemoryStore *store, const QString &clip, const std::string &key = "dev|os|droid|app") {
    HardwareDecodeGate::Options o;
    o.buildKey = key;
    o.droidvdecPresent = GstHardwareProbeRunner::elementPresent("droidvdec");
    o.droideglsinkPresent = GstHardwareProbeRunner::elementPresent("droideglsink");
    o.clipUri = clip;
    o.clipDirectory = gDir + QStringLiteral("/probe");
    if (store) o.storage = store->storage();
    return o;
}

static bool settled(const HardwareDecodeGate &g) {
    return g.probeState() == ProbeState::Passed || g.probeState() == ProbeState::Failed;
}

static void ply20_probePassesWithFrameWithin2s() {
    gMode = kDecode;
    MemoryStore store;
    CountingRunner runner;
    HardwareDecodeGate gate(&runner, options(&store, fileUri("v.webm")));
    CHECK(gate.probeState() == ProbeState::NotRun && !gate.verified());
    CHECK(gate.resultLine() == QLatin1String("not run (hardware decode off)"));
    int changes = 0;
    QObject::connect(&gate, &HardwareDecodeGate::changed, [&]() { ++changes; });
    gate.setHardwareRequested(true);
    CHECK(runner.starts == 0);  // next event-loop turn, never inside the setting change
    CHECK(waitFor([&]() { return settled(gate); }, 4000));
    CHECK(runner.starts == 1 && gate.probeState() == ProbeState::Passed && gate.verified());
    CHECK(gate.verdict().firstFrameMs >= 0 && gate.verdict().firstFrameMs <= DecodeModePolicy::kProbeFrameDeadlineMs);
    CHECK(gate.resultLine().startsWith(QLatin1String("passed (first frame ")));
    CHECK(changes >= 2);
    HardwareProbeVerdict saved;
    CHECK(store.saves == 1 && decodeProbeVerdict(store.line.toStdString(), &saved));
    CHECK(saved.record.state == ProbeState::Passed && saved.record.buildKey == "dev|os|droid|app");
    // Turning it off and on again does not probe again on this build.
    gate.setHardwareRequested(false);
    gate.setHardwareRequested(true);
    QCoreApplication::processEvents();
    CHECK(runner.starts == 1 && gate.verified());
}

static void ply20_probeFailsOnDecoderError() {
    gMode = kError;
    MemoryStore store;
    CountingRunner runner;
    HardwareDecodeGate gate(&runner, options(&store, fileUri("v.webm")));
    gate.setHardwareRequested(true);
    CHECK(waitFor([&]() { return settled(gate); }, 4000));
    CHECK(gate.probeState() == ProbeState::Failed && !gate.verified());
    CHECK(!gate.verdict().detail.empty() && gate.resultLine().startsWith(QLatin1String("failed: ")));
    std::printf("  error verdict: %s\n", qPrintable(gate.resultLine()));
    HardwareProbeVerdict saved;
    CHECK(decodeProbeVerdict(store.line.toStdString(), &saved) && saved.record.state == ProbeState::Failed);
}

static void ply20_probeTimesOutWithoutFrame() {
    gMode = kStall;
    MemoryStore store;
    CountingRunner runner;
    HardwareDecodeGate gate(&runner, options(&store, fileUri("v.webm")));
    QElapsedTimer t;
    t.start();
    gate.setHardwareRequested(true);
    CHECK(waitFor([&]() { return gate.probeState() == ProbeState::Running; }, 1000));
    CHECK(gate.resultLine() == QLatin1String("running"));
    CHECK(waitFor([&]() { return settled(gate); }, 5000));
    const qint64 elapsed = t.elapsed();
    CHECK(gate.probeState() == ProbeState::Failed && !gate.verified());
    CHECK(elapsed >= DecodeModePolicy::kProbeFrameDeadlineMs && elapsed < DecodeModePolicy::kProbeFrameDeadlineMs + 1000);
    CHECK(gate.resultLine() == QLatin1String("failed: no frame within 2 s"));
}

// The bundled clip: written to flash only, and it demuxes to the hardware stop caps (H.264 avc/au)
// so the probe's chain gets it (here a pass-through "decoder").
static void ply20_bundledClipReachesTheDecoder() {
    const QString dir = gDir + QStringLiteral("/probe");
    const QString path = writeHardwareProbeClip(dir);
    CHECK(!path.isEmpty() && QFile(path).size() == hardwareProbeClip().size() && hardwareProbeClip().size() > 4000);
    CHECK(writeHardwareProbeClip(dir) == path);  // identical file kept
    CHECK(writeHardwareProbeClip(QStringLiteral("/tmp/sailtube-probe")).isEmpty());  // PLT-09
    if (!gst_element_factory_find("qtdemux")) {
        std::printf("  (qtdemux missing: bundled clip demux not checked)\n");
        return;
    }
    gMode = kPass;
    CountingRunner runner;
    HardwareDecodeGate gate(&runner, options(nullptr, QString()));  // "" = the bundled clip
    gate.setHardwareRequested(true);
    CHECK(waitFor([&]() { return settled(gate); }, 4000));
    CHECK(gate.probeState() == ProbeState::Passed);
    if (gate.probeState() != ProbeState::Passed) std::printf("  bundled clip: %s\n", qPrintable(gate.resultLine()));
}

// Once per build: a stored verdict of this build is used as it is; another build probes again;
// an unreadable line is ignored; "probe again" forgets it; no runner (STORE) never verifies.
static void ply20_verdictPersistedPerBuild() {
    gMode = kDecode;
    MemoryStore store;
    HardwareProbeVerdict v;
    v.record.buildKey = "dev|os|droid|app";
    v.record.state = ProbeState::Passed;
    v.firstFrameMs = 300;
    store.line = QString::fromStdString(encodeProbeVerdict(v));
    {
        CountingRunner runner;
        HardwareDecodeGate gate(&runner, options(&store, fileUri("v.webm")));
        CHECK(gate.verified() && gate.probeState() == ProbeState::Passed);
        gate.setHardwareRequested(true);
        QCoreApplication::processEvents();
        CHECK(runner.starts == 0 && gate.resultLine() == QLatin1String("passed (first frame 300 ms)"));
        gate.retryProbe();
        CHECK(store.line.isEmpty() && !gate.verified());
        CHECK(waitFor([&]() { return settled(gate); }, 4000) && runner.starts == 1 && gate.verified());
    }
    {
        CountingRunner runner;
        HardwareDecodeGate gate(&runner, options(&store, fileUri("v.webm"), "dev|os 2|droid|app"));
        CHECK(gate.probeState() == ProbeState::NotRun && !gate.verified());
        gate.setHardwareRequested(true);
        CHECK(waitFor([&]() { return settled(gate); }, 4000) && runner.starts == 1);
        HardwareProbeVerdict saved;
        CHECK(decodeProbeVerdict(store.line.toStdString(), &saved) && saved.record.buildKey == "dev|os 2|droid|app");
    }
    {
        MemoryStore junk;
        junk.line = QStringLiteral("passed");
        CountingRunner runner;
        HardwareDecodeGate gate(&runner, options(&junk, fileUri("v.webm")));
        CHECK(gate.probeState() == ProbeState::NotRun);
    }
    {
        HardwareDecodeGate gate(nullptr, options(nullptr, fileUri("v.webm")));
        gate.setHardwareRequested(true);
        QCoreApplication::processEvents();
        CHECK(!gate.verified() && gate.resultLine() == QLatin1String("unavailable: not in this build"));
    }
    {
        HardwareDecodeGate::Options o = options(nullptr, fileUri("v.webm"));
        o.droideglsinkPresent = false;  // per-build fallback: an element missing
        CountingRunner runner;
        HardwareDecodeGate gate(&runner, o);
        gate.setHardwareRequested(true);
        QCoreApplication::processEvents();
        CHECK(runner.starts == 0 && gate.resultLine() == QLatin1String("unavailable: droideglsink missing"));
    }
    // The ini store round-trips the line.
    const QString ini = gDir + QStringLiteral("/hardware-probe.conf");
    HardwareDecodeGate::Storage s = HardwareDecodeGate::iniStorage(ini);
    s.save(store.line);
    CHECK(HardwareDecodeGate::iniStorage(ini).load() == store.line && !store.line.isEmpty());
    s.save(QString());
    CHECK(s.load().isEmpty());
    // This device's key names the fake gst-droid plugin version.
    const std::string key = GstHardwareProbeRunner::currentBuildKey(QStringLiteral("9.9"));
    CHECK(key.find("|1.0|9.9") != std::string::npos);
}

// ---- Player: render path and watchdog -------------------------------------------------------------

struct PlayerRig {
    VideoPlayer player;
    int firstFrames = 0;
    int fallbacks = 0;
    int failures = 0;
    QString fallbackReason;
    QElapsedTimer sinceLoad;
    qint64 fallbackAtMs = -1;
    PlayerRig() {
        QObject::connect(&player, &VideoPlayer::firstFrame, [this]() { ++firstFrames; });
        QObject::connect(&player, &VideoPlayer::decodeFallback, [this](const QString &r) {
            ++fallbacks;
            fallbackReason = r;
            fallbackAtMs = sinceLoad.elapsed();
        });
        QObject::connect(&player, &VideoPlayer::failed, [this](const QString &r, bool, int, int) {
            ++failures;
            std::printf("  player failed: %s\n", qPrintable(r));
        });
    }
    void load(DecodeMode decode) {
        PlayerSources p;
        p.mode = PlayMode::Local;
        p.videoId = "fixture";
        p.muxed.uri = fileUri("v.webm").toStdString();
        p.muxed.container = Container::Matroska;
        p.decode = decode;
        p.audioSink.factory = "fakesink";
        sinceLoad.start();
        player.load(p);
    }
};

static void ply20_hardwarePlanRendersThroughBridge() {
    gMode = kDecode;
    gShown = 0;
    PlayerRig r;
    r.load(DecodeMode::Hardware);
    const PipelinePlan &plan = r.player.plan();
    CHECK(plan.decode == DecodeMode::Hardware && containsDroidElement(plan));
    CHECK(r.player.decodeMode() == int(DecodeMode::Hardware));
    CHECK(r.player.hardwareVideoSink() != nullptr);
    CHECK(r.player.frameMailbox()->hardwareSource() != nullptr);  // the surface draws from the bridge
    CHECK(waitFor([&]() { return r.firstFrames > 0; }, 4000));
    CHECK(gShown.load() > 0 && r.fallbacks == 0 && r.failures == 0);
    r.player.stop();
}

static void ply20_watchdogFallsBackToSoftware() {
    gMode = kStall;
    PlayerRig r;
    r.load(DecodeMode::Hardware);
    CHECK(r.player.decodeMode() == int(DecodeMode::Hardware));
    CHECK(waitFor([&]() { return r.fallbacks > 0; }, 5000));
    CHECK(r.fallbackReason.contains(QLatin1String("3 s")));
    CHECK(r.fallbackAtMs >= DecodeModePolicy::kWatchdogMs && r.fallbackAtMs < DecodeModePolicy::kWatchdogMs + 1500);
    CHECK(waitFor([&]() { return r.firstFrames > 0; }, 5000));  // the software rebuild shows a picture
    CHECK(r.player.decodeMode() == int(DecodeMode::Software) && !containsDroidElement(r.player.plan()));
    CHECK(r.player.diagnostics().decodeFallbacks == 1 && r.failures == 0);
    // The same video stays software for this session; the next load does not try hardware again.
    r.fallbacks = 0;
    r.firstFrames = 0;
    r.load(DecodeMode::Hardware);
    CHECK(r.player.decodeMode() == int(DecodeMode::Software));
    CHECK(waitFor([&]() { return r.firstFrames > 0; }, 4000) && r.fallbacks == 0);
    r.player.stop();
}

int main(int argc, char **argv) {
    std::setvbuf(stdout, nullptr, _IOLBF, 0);
    gst_init(&argc, &argv);
    if (!gst_plugin_register_static(GST_VERSION_MAJOR, GST_VERSION_MINOR, "sailtubefakedroid",
                                    "Fake droid elements for SailTube host tests", fakePluginInit, "1.0", "unknown",
                                    "sailtube-tests", "sailtube-tests", "https://example.invalid/")) {
        std::printf("FAIL fake droid plugin\n");
        return 1;
    }
    QCoreApplication app(argc, argv);
    QString err;
    if (!GstEngine::initialize(&err)) {
        std::printf("hardware probe tests: SKIPPED (%s)\n", qPrintable(err));
        return 0;
    }
    const char *const needed[] = {"videotestsrc", "vp9enc", "webmmux", "vp9dec", "matroskademux", "identity",
                                  "fakesink", "uridecodebin", "videoconvert", "appsink"};
    for (size_t i = 0; i < sizeof(needed) / sizeof(needed[0]); ++i)
        if (!GstHardwareProbeRunner::elementPresent(needed[i])) {
            std::printf("hardware probe tests: SKIPPED (missing %s)\n", needed[i]);
            return 0;
        }
    // The probe clip is written to flash only (PLT-09): a TMPDIR under /tmp is not used here.
    QString base = QString::fromLocal8Bit(qgetenv("TMPDIR"));
    if (base.isEmpty() || !isAllowedSpoolLocation(QDir(base).absolutePath().toStdString()))
        base = QDir::homePath() + QStringLiteral("/.cache");
    gDir = base + QStringLiteral("/sailtube-hw-probe-%1").arg(getpid());
    QDir().mkpath(gDir);
    if (!runPipeline(QStringLiteral("videotestsrc num-buffers=60 ! video/x-raw,width=320,height=240,framerate=30/1 ! "
                                    "vp9enc deadline=1 cpu-used=8 ! webmmux ! filesink location=") + gDir + "/v.webm")) {
        std::printf("FAIL fixture generation\n");
        return 1;
    }
    ply20_probePassesWithFrameWithin2s();
    ply20_probeFailsOnDecoderError();
    ply20_probeTimesOutWithoutFrame();
    ply20_bundledClipReachesTheDecoder();
    ply20_verdictPersistedPerBuild();
    ply20_hardwarePlanRendersThroughBridge();
    ply20_watchdogFallsBackToSoftware();
    QDir(gDir).removeRecursively();
    std::printf("hardware probe tests: %d checks, %d failure%s (fake decoders made: %d)\n", checks, failures,
                failures == 1 ? "" : "s", gDecodersMade.load());
    return failures ? 1 : 0;
}
