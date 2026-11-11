// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "GstEngine.h"
#include "EglSinkBridge.h"
#include "VideoFrameMailbox.h"
#include "Core/Player/Domain/TempDirPolicy.h"

#include <gst/app/gstappsink.h>
#include <gst/gst.h>
#include <gst/video/video.h>

#include <QAtomicInt>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEvent>
#include <QMutexLocker>
#include <QtGlobal>

#include <cstdio>
#include <cstring>
#include <deque>
#include <thread>

namespace nordtile { namespace player {

namespace {

const QEvent::Type kEngineEventType = static_cast<QEvent::Type>(QEvent::registerEventType());

enum EventKind {
    kBusMessage = 1, kNoMorePads, kFirstFrame, kVideoSize, kHardwareRenderFailed, kFirstByte,
    kDemuxReady, kFirstDecoded, kDecoderHeld, kBranchNotHeld, kStartSegment, kTeardownDone
};

// Cross-thread notification, delivered on the owner thread.
class EngineEvent : public QEvent {
public:
    EngineEvent(quint64 generation, int kind, int a, int b, GstMessage *message)
        : QEvent(kEngineEventType), generation(generation), kind(kind), a(a), b(b), message(message) {}
    ~EngineEvent() override {
        if (message) gst_message_unref(message);
    }
    quint64 generation;
    int kind;
    int a, b;
    GstMessage *message;
};

QMutex gInitMutex;
bool gInitialized = false;                // under gInitMutex
QAtomicInt gInitializedFlag(0);           // = gInitialized, readable without the mutex
qint64 gInitMs = 0;                       // duration of the attempt that set the state
QString gInitError;                       // under gInitMutex
std::set<std::string> gAvailable;

// ---- Start-up worker (startInitialize / preloadDecoders) ----------------------------

const QEvent::Type kInitEventType = static_cast<QEvent::Type>(QEvent::registerEventType());
enum InitEventKind { kInitReady = 1, kInitPreloaded };

class InitEvent : public QEvent {
public:
    InitEvent(int kind, bool ok, const QString &error, qint64 ms, int loaded)
        : QEvent(kInitEventType), kind(kind), ok(ok), error(error), ms(ms), loaded(loaded) {}
    int kind;
    bool ok;
    QString error;
    qint64 ms;
    int loaded;
};

// One long-lived thread running queued jobs in order. Heap-allocated and never
// destroyed (a static destructor must not race a running job at exit); the
// thread is joined by a Qt post routine when the QCoreApplication goes away.
struct InitWorker {
    QMutex mutex;
    QWaitCondition cond;             // queue changes and the end of an async init attempt
    std::deque<std::function<void()>> jobs;
    std::thread thread;
    bool running = false;
    bool stopping = false;
    bool initRequested = false;      // startInitialize() was called (once per process)
    bool initPending = false;        // its attempt is queued or running
    bool preloadRequested = false;

    void post(const std::function<void()> &job);
    void loop();
};

InitWorker &initWorker() {
    static InitWorker *w = new InitWorker;
    return *w;
}

void stopInitWorker() {
    InitWorker &w = initWorker();
    {
        QMutexLocker lock(&w.mutex);
        if (!w.running) return;
        w.stopping = true;
        w.cond.wakeAll();
    }
    if (w.thread.joinable()) w.thread.join();
    QMutexLocker lock(&w.mutex);
    w.running = false;
    w.jobs.clear();
    w.initPending = false;  // a dropped attempt no longer blocks waitInitialized()
    w.cond.wakeAll();
}

void InitWorker::post(const std::function<void()> &job) {
    QMutexLocker lock(&mutex);
    jobs.push_back(job);
    if (!running) {
        if (thread.joinable()) thread.join();  // a stopped worker of an earlier application
        stopping = false;
        running = true;
        thread = std::thread([this]() { loop(); });
        qAddPostRoutine(&stopInitWorker);
    }
    cond.wakeAll();
}

void InitWorker::loop() {
    for (;;) {
        std::function<void()> job;
        {
            QMutexLocker lock(&mutex);
            while (jobs.empty() && !stopping) cond.wait(&mutex);
            if (stopping) return;  // remaining jobs are dropped at shutdown
            job = jobs.front();
            jobs.pop_front();
        }
        job();
    }
}

void postInitEvent(int kind, bool ok, const QString &error, qint64 ms, int loaded) {
    QCoreApplication::postEvent(GstEngineInit::instance(), new InitEvent(kind, ok, error, ms, loaded));
}

// Applies one property; optional ones are skipped when the element lacks it.
bool applyProperty(GstElement *element, const PropertySetting &p, QString *error) {
    GParamSpec *spec = g_object_class_find_property(G_OBJECT_GET_CLASS(element), p.name.c_str());
    if (!spec) {
        if (p.optional) return true;
        if (error) *error = QStringLiteral("%1 has no property %2").arg(GST_ELEMENT_NAME(element), p.name.c_str());
        return false;
    }
    if (G_PARAM_SPEC_VALUE_TYPE(spec) == G_TYPE_STRING) {
        g_object_set(element, p.name.c_str(), p.value.c_str(), nullptr);
        return true;
    }
    GValue value = G_VALUE_INIT;
    g_value_init(&value, G_PARAM_SPEC_VALUE_TYPE(spec));
    const bool ok = gst_value_deserialize(&value, p.value.c_str());
    if (ok) g_object_set_property(G_OBJECT(element), p.name.c_str(), &value);
    g_value_unset(&value);
    if (!ok && error)
        *error = QStringLiteral("%1: bad value '%2' for %3").arg(GST_ELEMENT_NAME(element), p.value.c_str(), p.name.c_str());
    return ok || p.optional;
}

bool applyAll(GstElement *element, const std::vector<PropertySetting> &props, QString *error) {
    for (size_t i = 0; i < props.size(); ++i)
        if (!applyProperty(element, props[i], error)) return false;
    return true;
}

GstElement *makeElement(const ElementPlan &plan, QString *error) {
    GstElement *e = gst_element_factory_make(plan.factory.c_str(), plan.name.empty() ? nullptr : plan.name.c_str());
    if (!e) {
        if (error) *error = QStringLiteral("cannot create %1").arg(plan.factory.c_str());
        return nullptr;
    }
    if (!applyAll(e, plan.properties, error)) {
        gst_object_unref(e);
        return nullptr;
    }
    return e;
}

// PLT-09: uridecodebin's download buffer (PLY-04, a downloadbuffer / queue2 temp file) defaults to
// g_get_user_cache_dir()/<prgname>-XXXXXX, outside the app's own flash TMPDIR (emptied at start)
// and possibly not writable under Sailjail. Keep it in TMPDIR, removed when the queue closes.
// uridecodebin may set the template after adding the queue, so a notify handler re-applies it.
void keepSpoolInTempDir(GstElement *queue) {
    gchar *current = nullptr;
    g_object_get(queue, "temp-template", &current, nullptr);
    if (current) {
        const std::string dir = g_get_tmp_dir();
        const std::string spool = dir + "/sailtube-spool-XXXXXX";
        if (isAllowedSpoolLocation(dir) && spool != current)
            g_object_set(queue, "temp-template", spool.c_str(), "temp-remove", TRUE, nullptr);
        g_free(current);
    }
}

void spoolTemplateChanged(GObject *queue, GParamSpec *, gpointer) { keepSpoolInTempDir(GST_ELEMENT(queue)); }

// After teardown the old bus must not reach the owner any more.
GstBusSyncReply dropAll(GstBus *, GstMessage *, gpointer) { return GST_BUS_DROP; }

bool klassHas(GstElementFactory *f, const char *what) {
    const char *klass = gst_element_factory_get_metadata(f, GST_ELEMENT_METADATA_KLASS);
    return klass && std::strstr(klass, what);
}

bool startsWith(const char *s, const std::string &prefix) {
    return s && std::strncmp(s, prefix.c_str(), prefix.size()) == 0;
}

struct MappedSample {
    GstSample *sample;
    GstVideoFrame frame;
};

} // namespace

// ---- Callback contexts --------------------------------------------------------

struct GstEngine::RouteCtx {
    RouteKind kind = RouteKind::Audio;
    std::vector<std::string> prefixes;
    std::vector<GstElement *> chain;  // owned by the pipeline
    bool linked = false;
};

// PLY-06: one decoder sink pad of a branch (start-seek hold probe).
struct GstEngine::HoldPad {
    BranchCtx *branch = nullptr;
    GstPad *pad = nullptr;          // ref
    unsigned long probe = 0;        // 0 = removed
    bool waiting = false;           // a buffer is held in the probe right now
    bool flushing = false;          // FLUSH_START seen while waiting
    bool flushStopSeen = false;     // FLUSH_STOP seen after the release
    bool segmentSeen = false;       // the start seek's segment passed: probe done
};

struct GstEngine::BranchCtx {
    GstEngine *engine = nullptr;
    quint64 generation = 0;
    int index = 0;
    Branch branch = Branch::Muxed;
    bool network = false;
    GstElement *source = nullptr;
    std::vector<PropertySetting> sourceSetup;
    std::vector<RouteCtx> routes;
    ElementPlan unmatched;
    int unmatchedCount = 0;
    gint firstByte = 0;  // NFR-PERF timing: the source's first buffer was seen (atomic)
    gint demuxReady = 0; // NFR-PERF timing: the demuxer's first pad was seen (atomic)
    bool hideSize = false;    // PLY-04: BranchPlan::hideSizeUntilDemuxReady
    gint sizeProbeAdded = 0;  // the hide-size probe sits on this branch's demuxer (atomic)
    // PLY-06 start-seek hold, under the engine mutex.
    std::vector<std::unique_ptr<HoldPad>> holdPads;
    bool holdWanted = false;   // holdForStartSeek() was called
    bool flowed = false;       // a buffer passed a decoder before the hold was wanted
    bool heldPosted = false;   // decoderHeld / branchNotHeld posted
    bool released = false;     // start seek sent: drop until its segment
    bool passAll = false;      // released unchanged
    quint32 startSeqnum = 0;
    bool startSegmentKnown = false;
    GstElement *multiqueue = nullptr;  // decodebin's demuxed queue (ref), under the engine mutex

    ~BranchCtx() {
        for (size_t i = 0; i < holdPads.size(); ++i)
            if (holdPads[i]->pad) gst_object_unref(holdPads[i]->pad);
        if (multiqueue) gst_object_unref(multiqueue);
    }
};

struct GstEngine::Ctx {
    GstEngine *engine = nullptr;
    quint64 generation = 0;
    GstElement *pipeline = nullptr;        // valid until the teardown worker unrefs it
    GstElement *videoSource = nullptr;     // video (Dual) or muxed source bin of this build
    std::vector<BranchCtx *> branches;     // filled in build(), read-only afterwards
    gint firstDecoded = 0;                 // NFR-PERF timing (atomic)
    bool sampleSink = false;               // the video sink hands its frames to postSample() (set in build())
};

// Everything of one torn-down pipeline, owned by the teardown worker.
struct GstEngine::TeardownJob {
    GstElement *pipeline = nullptr;
    Ctx *ctx = nullptr;
    std::vector<std::unique_ptr<BranchCtx>> branches;
    std::shared_ptr<EglSinkBridge> bridge;  // detached after NULL (no streaming thread emits any more)
    gint64 startedUs = 0;                   // g_get_monotonic_time() at teardown()
    bool hardware = false;
};

struct GstEngine::Callbacks {

    static void post(GstEngine *e, quint64 generation, int kind, int a, int b, GstMessage *m) {
        QCoreApplication::postEvent(e, new EngineEvent(generation, kind, a, b, m));
    }

    // Under e->mutex_: does `generation` still own the engine's state? After
    // teardown() a streaming thread of the old pipeline may still run until the
    // worker has set it to NULL; it must not touch the next build's state.
    static bool alive(GstEngine *e, quint64 generation) { return generation != 0 && generation == e->liveGeneration_; }

    static GstBusSyncReply busSync(GstBus *, GstMessage *message, gpointer data) {
        Ctx *c = static_cast<Ctx *>(data);
        post(c->engine, c->generation, kBusMessage, 0, 0, gst_message_ref(message));
        return GST_BUS_DROP;
    }

    static void sourceSetup(GstElement *, GstElement *source, gpointer data) {
        BranchCtx *b = static_cast<BranchCtx *>(data);
        QString ignored;
        applyAll(source, b->sourceSetup, &ignored);  // every setting is optional
        // NFR-PERF timing: the first media buffer this branch's source hands
        // downstream (through MediaProxy, the first byte from loopback).
        if (GstPad *pad = gst_element_get_static_pad(source, "src")) {
            gst_pad_add_probe(pad, GST_PAD_PROBE_TYPE_BUFFER, &Callbacks::firstByteProbe, b, nullptr);
            gst_object_unref(pad);
        }
    }

    static GstPadProbeReturn firstByteProbe(GstPad *, GstPadProbeInfo *, gpointer data) {
        BranchCtx *b = static_cast<BranchCtx *>(data);
        if (g_atomic_int_compare_and_exchange(&b->firstByte, 0, 1))
            post(b->engine, b->generation, kFirstByte, static_cast<int>(b->branch), 0, nullptr);
        return GST_PAD_PROBE_REMOVE;
    }

    static void padAdded(GstElement *, GstPad *pad, gpointer data) {
        BranchCtx *b = static_cast<BranchCtx *>(data);
        GstEngine *e = b->engine;
        GstCaps *caps = gst_pad_get_current_caps(pad);
        if (!caps) caps = gst_pad_query_caps(pad, nullptr);
        const std::string capsName =
            (caps && gst_caps_get_size(caps) > 0) ? gst_structure_get_name(gst_caps_get_structure(caps, 0)) : "";
        const char *name = capsName.c_str();
        GstElement *head = nullptr;
        {
            QMutexLocker lock(&e->mutex_);
            if (!alive(e, b->generation)) {
                if (caps) gst_caps_unref(caps);
                return;
            }
            // PLY-06: a stream exposed without any held decoder (no decoder in this branch, e.g.
            // raw audio) cannot take the start seek before preroll.
            if (b->holdWanted && b->holdPads.empty() && !b->heldPosted) {
                b->heldPosted = true;
                post(e, b->generation, kBranchNotHeld, static_cast<int>(b->branch), 0, nullptr);
            }
            for (size_t r = 0; r < b->routes.size() && !head; ++r) {
                RouteCtx &route = b->routes[r];
                if (route.linked || route.chain.empty()) continue;
                for (size_t p = 0; p < route.prefixes.size(); ++p) {
                    if (startsWith(name, route.prefixes[p])) {
                        route.linked = true;
                        head = route.chain.front();
                        break;
                    }
                }
            }
        }
        if (caps) gst_caps_unref(caps);
        if (head) {
            GstPad *sinkPad = gst_element_get_static_pad(head, "sink");
            if (sinkPad) {
                if (gst_pad_link(pad, sinkPad) != GST_PAD_LINK_OK)
                    GST_WARNING("route link failed for %s", GST_PAD_NAME(pad));
                gst_object_unref(sinkPad);
            }
            return;
        }
        // Not routed: give it a fakesink so the demuxer never sees not-linked.
        {
            // Diagnostics (run 5, a local file without a picture): which streams went nowhere.
            QMutexLocker lock(&e->mutex_);
            if (alive(e, b->generation)) {
                ++e->stats_.unroutedPads;
                if (!e->stats_.unroutedCaps.isEmpty()) e->stats_.unroutedCaps += QLatin1Char(',');
                e->stats_.unroutedCaps += QString::fromUtf8(*name ? name : "unknown");
            }
        }
        if (b->unmatched.factory.empty()) return;
        ElementPlan plan = b->unmatched;
        plan.name.clear();
        QString ignored;
        GstElement *sink = makeElement(plan, &ignored);
        if (!sink) return;
        GstElement *pipeline = GST_ELEMENT(gst_element_get_parent(b->source));
        if (!pipeline) { gst_object_unref(sink); return; }
        gst_bin_add(GST_BIN(pipeline), sink);
        gst_element_sync_state_with_parent(sink);
        GstPad *sinkPad = gst_element_get_static_pad(sink, "sink");
        gst_pad_link(pad, sinkPad);
        gst_object_unref(sinkPad);
        gst_object_unref(pipeline);
    }

    static void noMorePads(GstElement *, gpointer data) {
        BranchCtx *b = static_cast<BranchCtx *>(data);
        post(b->engine, b->generation, kNoMorePads, b->index, 0, nullptr);
    }

    // The branch whose source bin contains `element`, or null (route elements).
    static BranchCtx *branchOfElement(Ctx *c, GstElement *element) {
        for (size_t i = 0; i < c->branches.size(); ++i)
            if (gst_object_has_as_ancestor(GST_OBJECT(element), GST_OBJECT(c->branches[i]->source))) return c->branches[i];
        return nullptr;
    }

    static void deepElementAdded(GstBin *, GstBin *, GstElement *element, gpointer data) {
        Ctx *c = static_cast<Ctx *>(data);
        GstEngine *e = c->engine;
        GstElementFactory *f = gst_element_get_factory(element);
        if (!f) return;
        const char *factory = gst_plugin_feature_get_name(GST_PLUGIN_FEATURE(f));
        if (std::strcmp(factory, "downloadbuffer") == 0 || std::strcmp(factory, "queue2") == 0) {
            keepSpoolInTempDir(element);
            g_signal_connect(element, "notify::temp-template", G_CALLBACK(spoolTemplateChanged), nullptr);
            return;
        }
        BranchCtx *b = branchOfElement(c, element);
        if (std::strcmp(factory, "multiqueue") == 0) {
            // Seek bar (queryBuffered): the first multiqueue of the branch holds its demuxed data.
            if (!b) return;
            QMutexLocker lock(&e->mutex_);
            if (alive(e, c->generation) && !b->multiqueue) b->multiqueue = GST_ELEMENT(gst_object_ref(element));
            return;
        }
        if (klassHas(f, "Demux")) {
            // NFR-PERF timing: demux-ready = the demuxer's first pad (its headers are parsed).
            if (b) g_signal_connect(element, "pad-added", G_CALLBACK(&Callbacks::demuxPadAdded), b);
            // PLY-04 / NFR-PERF-02: no end-of-file probe through the download buffer before the first pad.
            if (b && b->hideSize && g_atomic_int_compare_and_exchange(&b->sizeProbeAdded, 0, 1)) {
                if (GstPad *pad = gst_element_get_static_pad(element, "sink")) {
                    gst_pad_add_probe(pad, GST_PAD_PROBE_TYPE_QUERY_UPSTREAM, &Callbacks::hideSizeProbe, b, nullptr);
                    gst_object_unref(pad);
                }
            }
            return;
        }
        // Codecs only: decodebin / uridecodebin themselves are "Generic/Bin/Decoder".
        if (!klassHas(f, "Codec") || !klassHas(f, "Decoder")) return;
        const bool video = klassHas(f, "Video");
        const bool audio = klassHas(f, "Audio");
        {
            QMutexLocker lock(&e->mutex_);
            if (!alive(e, c->generation)) return;
            // PLT-08 diagnostics: which decoder this build really autoplugged.
            if (video && e->stats_.videoDecoder.isEmpty()) e->stats_.videoDecoder = QString::fromUtf8(factory);
            if (audio && e->stats_.audioDecoder.isEmpty()) e->stats_.audioDecoder = QString::fromUtf8(factory);
        }
        if (b) addHoldProbe(e, b, element);
        if (video) watchFirstDecoded(c, element);
        if (!video || !c->videoSource || !gst_object_has_as_ancestor(GST_OBJECT(element), GST_OBJECT(c->videoSource)))
            return;
        GstPad *pad = gst_element_get_static_pad(element, "sink");
        if (!pad) return;
        QMutexLocker lock(&e->mutex_);
        if (e->decoderSinkPad_ || !alive(e, c->generation)) { gst_object_unref(pad); return; }
        e->decoderSinkPad_ = pad;
    }

    // PLY-04 / NFR-PERF-02 (BranchPlan::hideSizeUntilDemuxReady): until the demuxer added its first
    // pad, its BYTES duration (upstream size) queries stay unanswered. A pull-mode qtdemux then
    // skips the mfro read at the end of a fragmented file, which the download buffer would serve
    // with two extra range requests (to the end of the file and back) before the first pad. The
    // probe removes itself at the first query after demux-ready.
    static GstPadProbeReturn hideSizeProbe(GstPad *, GstPadProbeInfo *info, gpointer data) {
        BranchCtx *b = static_cast<BranchCtx *>(data);
        if (g_atomic_int_get(&b->demuxReady)) return GST_PAD_PROBE_REMOVE;  // headers parsed: answered normally
        GstQuery *q = GST_PAD_PROBE_INFO_QUERY(info);
        if (!q || GST_QUERY_TYPE(q) != GST_QUERY_DURATION) return GST_PAD_PROBE_OK;
        GstFormat format = GST_FORMAT_UNDEFINED;
        gst_query_parse_duration(q, &format, nullptr);
        if (format != GST_FORMAT_BYTES) return GST_PAD_PROBE_OK;
        GstEngine *e = b->engine;
        {
            QMutexLocker lock(&e->mutex_);
            if (alive(e, b->generation)) ++e->stats_.demuxerSizeQueriesHidden;
        }
        return GST_PAD_PROBE_DROP;  // not forwarded, not answered: the size is unknown to the demuxer
    }

    static void demuxPadAdded(GstElement *, GstPad *, gpointer data) {
        BranchCtx *b = static_cast<BranchCtx *>(data);
        if (g_atomic_int_compare_and_exchange(&b->demuxReady, 0, 1))
            post(b->engine, b->generation, kDemuxReady, static_cast<int>(b->branch), 0, nullptr);
    }

    // NFR-PERF timing: the first buffer out of the (first) video decoder.
    static void watchFirstDecoded(Ctx *c, GstElement *decoder) {
        GstPad *pad = gst_element_get_static_pad(decoder, "src");
        if (!pad) return;
        gst_pad_add_probe(pad, GST_PAD_PROBE_TYPE_BUFFER, &Callbacks::firstDecodedProbe, c, nullptr);
        gst_object_unref(pad);
    }

    static GstPadProbeReturn firstDecodedProbe(GstPad *, GstPadProbeInfo *, gpointer data) {
        Ctx *c = static_cast<Ctx *>(data);
        if (g_atomic_int_compare_and_exchange(&c->firstDecoded, 0, 1))
            post(c->engine, c->generation, kFirstDecoded, 0, 0, nullptr);
        return GST_PAD_PROBE_REMOVE;
    }

    // ---- PLY-06 start-seek hold ----------------------------------------------------------
    // One probe per decoder sink pad, installed when the decoder appears. Until
    // holdForStartSeek() it removes itself at the first buffer (the branch then
    // "flowed"). Held: the first buffer waits in the probe (engine mutex +
    // holdCond_) until the start seek is sent (then it and every pre-seek
    // buffer are dropped until the seek's segment) or the branch is released
    // unchanged. A FLUSH_START or the teardown also ends the wait.
    static void addHoldProbe(GstEngine *e, BranchCtx *b, GstElement *decoder) {
        GstPad *pad = gst_element_get_static_pad(decoder, "sink");
        if (!pad) return;
        QMutexLocker lock(&e->mutex_);
        if (!alive(e, b->generation)) { gst_object_unref(pad); return; }
        std::unique_ptr<HoldPad> hp(new HoldPad);
        hp->branch = b;
        hp->pad = pad;
        // Under the mutex, so the callback (which locks it too) sees hp->probe set.
        hp->probe = gst_pad_add_probe(pad, GstPadProbeType(GST_PAD_PROBE_TYPE_BUFFER | GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM |
                                                           GST_PAD_PROBE_TYPE_EVENT_FLUSH),
                                      &Callbacks::holdProbe, hp.get(), nullptr);
        b->holdPads.push_back(std::move(hp));
    }

    static GstPadProbeReturn holdProbe(GstPad *, GstPadProbeInfo *info, gpointer data) {
        HoldPad *hp = static_cast<HoldPad *>(data);
        BranchCtx *b = hp->branch;
        GstEngine *e = b->engine;
        QMutexLocker lock(&e->mutex_);
        const bool live = alive(e, b->generation);
        if (GST_PAD_PROBE_INFO_TYPE(info) & (GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM | GST_PAD_PROBE_TYPE_EVENT_FLUSH)) {
            GstEvent *ev = GST_PAD_PROBE_INFO_EVENT(info);
            if (!ev || !live) return GST_PAD_PROBE_OK;
            switch (GST_EVENT_TYPE(ev)) {
            case GST_EVENT_FLUSH_START:
                if (hp->waiting) {
                    hp->flushing = true;
                    e->holdCond_.wakeAll();
                }
                break;
            case GST_EVENT_FLUSH_STOP:
                hp->flushing = false;
                if (b->released) hp->flushStopSeen = true;
                break;
            case GST_EVENT_SEGMENT:
                if (b->released && !b->passAll && !hp->segmentSeen &&
                    (gst_event_get_seqnum(ev) == b->startSeqnum || hp->flushStopSeen)) {
                    hp->segmentSeen = true;
                    if (!b->startSegmentKnown) {
                        b->startSegmentKnown = true;
                        const GstSegment *seg = nullptr;
                        gst_event_parse_segment(ev, &seg);
                        const guint64 st = seg ? gst_segment_to_stream_time(seg, GST_FORMAT_TIME, seg->start) : GST_CLOCK_TIME_NONE;
                        post(e, b->generation, kStartSegment, static_cast<int>(b->branch),
                             st == GST_CLOCK_TIME_NONE ? -1 : static_cast<int>(st / 1000000), nullptr);
                    }
                }
                break;
            default:
                break;
            }
            return GST_PAD_PROBE_OK;
        }
        // A buffer.
        if (!live || b->passAll) { hp->probe = 0; return GST_PAD_PROBE_REMOVE; }
        if (!b->holdWanted && !b->released) {
            b->flowed = true;  // decoding started: too late for a start seek in this branch
            hp->probe = 0;
            return GST_PAD_PROBE_REMOVE;
        }
        if (!b->released) {
            if (!b->heldPosted) {
                b->heldPosted = true;
                post(e, b->generation, kDecoderHeld, static_cast<int>(b->branch), 0, nullptr);
            }
            hp->waiting = true;
            while (alive(e, b->generation) && !b->released && !b->passAll && !hp->flushing)
                e->holdCond_.wait(&e->mutex_);
            hp->waiting = false;
            if (!alive(e, b->generation)) return GST_PAD_PROBE_DROP;  // the old pipeline goes to NULL
            if (b->passAll) { hp->probe = 0; return GST_PAD_PROBE_REMOVE; }
        }
        if (hp->segmentSeen) { hp->probe = 0; return GST_PAD_PROBE_REMOVE; }
        ++e->stats_.startDropped;  // from before the start seek: never decoded
        return GST_PAD_PROBE_DROP;
    }

    static void postSample(GstEngine *e, quint64 generation, GstSample *sample, bool playing) {
        {
            QMutexLocker lock(&e->mutex_);
            if (!alive(e, generation)) {
                gst_sample_unref(sample);
                return;
            }
            if (!e->outputEnabled_) {
                if (e->heldPreroll_) gst_sample_unref(e->heldPreroll_);
                e->heldPreroll_ = playing ? nullptr : sample;
                if (playing) gst_sample_unref(sample);
                return;
            }
        }
        GstCaps *caps = gst_sample_get_caps(sample);
        GstBuffer *buffer = gst_sample_get_buffer(sample);
        GstVideoInfo info;
        if (!caps || !buffer || !gst_video_info_from_caps(&info, caps)) {
            gst_sample_unref(sample);
            return;
        }
        GstElement *audioSink = nullptr;
        if (playing) {
            QMutexLocker lock(&e->mutex_);
            if (alive(e, generation) && e->audioSink_) audioSink = GST_ELEMENT(gst_object_ref(e->audioSink_));
        }
        if (audioSink) {
            const GstSegment *segment = gst_sample_get_segment(sample);
            gint64 audioNs = 0;
            if (segment && GST_BUFFER_PTS_IS_VALID(buffer) &&
                gst_element_query_position(audioSink, GST_FORMAT_TIME, &audioNs)) {
                const guint64 st = gst_segment_to_stream_time(segment, GST_FORMAT_TIME, GST_BUFFER_PTS(buffer));
                if (st != GST_CLOCK_TIME_NONE) {
                    const qint64 offset = (static_cast<qint64>(st) - audioNs) / 1000000;
                    QMutexLocker lock(&e->mutex_);
                    e->stats_.lastAvOffsetMs = offset;
                    const qint64 a = offset < 0 ? -offset : offset;
                    if (a > e->stats_.maxAbsAvOffsetMs) e->stats_.maxAbsAvOffsetMs = a;
                    ++e->stats_.avSamples;
                }
            }
            gst_object_unref(audioSink);
        }
        MappedSample *m = new MappedSample;
        m->sample = sample;
        if (!gst_video_frame_map(&m->frame, &info, buffer, GST_MAP_READ)) {
            gst_sample_unref(sample);
            delete m;
            return;
        }
        VideoFrame frame;
        frame.keepAlive = std::shared_ptr<const void>(m, [](MappedSample *p) {
            gst_video_frame_unmap(&p->frame);
            gst_sample_unref(p->sample);
            delete p;
        });
        frame.data = static_cast<const unsigned char *>(GST_VIDEO_FRAME_PLANE_DATA(&m->frame, 0));
        frame.width = GST_VIDEO_INFO_WIDTH(&info);
        frame.height = GST_VIDEO_INFO_HEIGHT(&info);
        frame.stride = GST_VIDEO_FRAME_PLANE_STRIDE(&m->frame, 0);
        frame.pixelAspect = GST_VIDEO_INFO_PAR_D(&info) > 0
                                ? double(GST_VIDEO_INFO_PAR_N(&info)) / GST_VIDEO_INFO_PAR_D(&info)
                                : 1.0;
        bool sizeChanged = false, first = false;
        {
            // Checked and posted under the mutex: once teardown() returned, no
            // frame of the old pipeline reaches the mailbox.
            QMutexLocker lock(&e->mutex_);
            if (!alive(e, generation)) return;  // frame's keepAlive unmaps and unrefs
            if (frame.width != e->lastWidth_ || frame.height != e->lastHeight_) {
                e->lastWidth_ = frame.width;
                e->lastHeight_ = frame.height;
                sizeChanged = true;
            }
            if (e->mailbox_) e->mailbox_->post(frame);
            // The first frame is announced here, once it is in the mailbox, never when its
            // buffer merely arrived at the sink pad: the owner thread could otherwise handle
            // the notice before the frame is there (firstFrame with an empty mailbox), and a
            // frame whose buffer passed the pad while the output was still off would not be
            // announced before the next buffer.
            if (!e->firstFramePosted_ && e->outputEnabled_) {
                e->firstFramePosted_ = true;
                first = true;
            }
        }
        if (sizeChanged) post(e, generation, kVideoSize, frame.width, frame.height, nullptr);
        if (first) post(e, generation, kFirstFrame, 0, 0, nullptr);
    }

    static GstFlowReturn newSample(GstAppSink *sink, gpointer data) {
        Ctx *c = static_cast<Ctx *>(data);
        GstSample *s = gst_app_sink_pull_sample(sink);
        if (s) postSample(c->engine, c->generation, s, true);
        return GST_FLOW_OK;
    }

    static GstFlowReturn newPreroll(GstAppSink *sink, gpointer data) {
        Ctx *c = static_cast<Ctx *>(data);
        GstSample *s = gst_app_sink_pull_preroll(sink);
        if (s) postSample(c->engine, c->generation, s, false);
        return GST_FLOW_OK;
    }

    // Video sink input: frame counter, first-frame notice, segment/flush stats.
    static GstPadProbeReturn videoSinkProbe(GstPad *pad, GstPadProbeInfo *info, gpointer data) {
        Ctx *c = static_cast<Ctx *>(data);
        GstEngine *e = c->engine;
        if (GST_PAD_PROBE_INFO_TYPE(info) & GST_PAD_PROBE_TYPE_BUFFER) {
            bool first = false;
            {
                QMutexLocker lock(&e->mutex_);
                if (!alive(e, c->generation)) return GST_PAD_PROBE_OK;
                if (e->stats_.firstVideoSinkMs < 0) {
                    GstBuffer *buffer = GST_PAD_PROBE_INFO_BUFFER(info);
                    GstEvent *ev = gst_pad_get_sticky_event(pad, GST_EVENT_SEGMENT, 0);
                    if (ev && buffer && GST_BUFFER_PTS_IS_VALID(buffer)) {
                        const GstSegment *seg = nullptr;
                        gst_event_parse_segment(ev, &seg);
                        const guint64 st = seg ? gst_segment_to_stream_time(seg, GST_FORMAT_TIME, GST_BUFFER_PTS(buffer))
                                               : GST_CLOCK_TIME_NONE;
                        if (st != GST_CLOCK_TIME_NONE) e->stats_.firstVideoSinkMs = static_cast<qint64>(st / 1000000);
                    }
                    if (ev) gst_event_unref(ev);
                }
                ++e->stats_.framesRendered;
                if (e->freezeMode_ == 1) ++e->stats_.framesWhileFrozen;
                // Hardware: a buffer at the pad is not a shown frame; the
                // bridge reports the sink's own announcement (PLY-20). The
                // software appsink: postSample() announces it, after the
                // mailbox has the frame. Only a sink without either (a video
                // sink override) counts the buffer at its pad.
                if (!e->firstFramePosted_ && e->outputEnabled_ && !e->hwSink_ && !c->sampleSink) {
                    e->firstFramePosted_ = true;
                    first = true;
                }
            }
            if (first) post(e, c->generation, kFirstFrame, 0, 0, nullptr);
            return GST_PAD_PROBE_OK;
        }
        GstEvent *event = GST_PAD_PROBE_INFO_EVENT(info);
        if (event && GST_EVENT_TYPE(event) == GST_EVENT_CAPS) hardwareCaps(e, c->generation, event);
        sinkEvent(e, c->generation, event, true);
        return GST_PAD_PROBE_OK;
    }

    // PLY-20: the zero-copy sink's picture size comes from its caps (its
    // frames never pass through postSample()).
    static void hardwareCaps(GstEngine *e, quint64 generation, GstEvent *event) {
        EglSinkBridge *bridge;
        {
            QMutexLocker lock(&e->mutex_);
            // Lives until the teardown worker detached it, after the streaming threads stopped.
            bridge = alive(e, generation) ? e->hwSink_ : nullptr;
        }
        if (!bridge) return;
        GstCaps *caps = nullptr;
        gst_event_parse_caps(event, &caps);
        if (!caps) return;
        int width = 0, height = 0, parN = 1, parD = 1;
        GstVideoInfo info;
        if (gst_video_info_from_caps(&info, caps)) {
            width = GST_VIDEO_INFO_WIDTH(&info);
            height = GST_VIDEO_INFO_HEIGHT(&info);
            parN = GST_VIDEO_INFO_PAR_N(&info);
            parD = GST_VIDEO_INFO_PAR_D(&info);
        } else if (gst_caps_get_size(caps) > 0) {
            const GstStructure *st = gst_caps_get_structure(caps, 0);
            gst_structure_get_int(st, "width", &width);
            gst_structure_get_int(st, "height", &height);
            gst_structure_get_fraction(st, "pixel-aspect-ratio", &parN, &parD);
        }
        if (width <= 0 || height <= 0) return;
        bridge->setCaps(width, height, parD > 0 ? double(parN) / parD : 1.0);
        bool sizeChanged = false;
        {
            QMutexLocker lock(&e->mutex_);
            if (!alive(e, generation)) return;
            if (width != e->lastWidth_ || height != e->lastHeight_) {
                e->lastWidth_ = width;
                e->lastHeight_ = height;
                sizeChanged = true;
            }
        }
        if (e->mailbox_) e->mailbox_->notifyListeners();
        if (sizeChanged) post(e, generation, kVideoSize, width, height, nullptr);
    }

    static GstPadProbeReturn audioSinkProbe(GstPad *, GstPadProbeInfo *info, gpointer data) {
        Ctx *c = static_cast<Ctx *>(data);
        sinkEvent(c->engine, c->generation, GST_PAD_PROBE_INFO_EVENT(info), false);
        return GST_PAD_PROBE_OK;
    }

    static void sinkEvent(GstEngine *e, quint64 generation, GstEvent *event, bool video) {
        if (!event) return;
        const quint32 seqnum = gst_event_get_seqnum(event);
        if (GST_EVENT_TYPE(event) == GST_EVENT_FLUSH_START) {
            // A push-mode demuxer forwards the upstream flush too; count one
            // flush per seek (seqnum).
            QMutexLocker lock(&e->mutex_);
            if (!alive(e, generation)) return;
            quint32 &last = video ? e->videoFlushSeqnum_ : e->audioFlushSeqnum_;
            if (seqnum != last) {
                last = seqnum;
                ++(video ? e->stats_.videoSinkFlushes : e->stats_.audioSinkFlushes);
                if (qEnvironmentVariableIsSet("SAILTUBE_PLAYER_DEBUG"))
                    std::fprintf(stderr, "player: flush seqnum %u at the %s sink\n", seqnum, video ? "video" : "audio");
            }
        } else if (GST_EVENT_TYPE(event) == GST_EVENT_SEGMENT) {
            const GstSegment *s = nullptr;
            gst_event_parse_segment(event, &s);
            if (!s) return;
            QMutexLocker lock(&e->mutex_);
            if (!alive(e, generation)) return;
            (video ? e->stats_.videoSegmentRate : e->stats_.audioSegmentRate) = s->rate * s->applied_rate;
            (video ? e->videoSegmentSeqnum_ : e->audioSegmentSeqnum_) = seqnum;
            const guint64 st = gst_segment_to_stream_time(s, GST_FORMAT_TIME, s->start);
            (video ? e->videoSegmentStartMs_ : e->audioSegmentStartMs_) =
                st == GST_CLOCK_TIME_NONE ? -1 : static_cast<qint64>(st / 1000000);
        }
    }

    // PLY-09 DropBeforeVideoDecoder.
    // Waits (without the engine mutex) until the pipeline clock reaches the
    // buffer's running time minus a small lead, so a dropping freeze consumes
    // the video stream at playback pace. Dropping as fast as possible would let
    // decodebin's multiqueue grow the audio queue without bound (an empty queue
    // lets the others overrun) and the demuxer would read, and download, the
    // whole stream ahead of the clock; the video would then be at EOS on thaw.
    static void paceToClock(GstEngine *e, GstElement *pipeline, GstPad *pad, GstBuffer *buffer) {
        const GstClockTime ts = GST_BUFFER_DTS_OR_PTS(buffer);
        if (!GST_CLOCK_TIME_IS_VALID(ts) || !pipeline) return;
        GstEvent *ev = gst_pad_get_sticky_event(pad, GST_EVENT_SEGMENT, 0);
        if (!ev) return;
        const GstSegment *seg = nullptr;
        gst_event_parse_segment(ev, &seg);
        const GstClockTime rt = seg ? gst_segment_to_running_time(seg, GST_FORMAT_TIME, ts) : GST_CLOCK_TIME_NONE;
        gst_event_unref(ev);
        if (!GST_CLOCK_TIME_IS_VALID(rt)) return;
        GstClock *clock = gst_element_get_clock(pipeline);
        if (!clock) return;
        const GstClockTime lead = 100 * GST_MSECOND;
        GstClockTime target = gst_element_get_base_time(pipeline) + rt;
        target = target > lead ? target - lead : 0;
        GstClockID id = gst_clock_new_single_shot_id(clock, target);
        bool wait;
        {
            QMutexLocker lock(&e->mutex_);
            wait = e->freezeMode_ != 0 && !e->freezeWait_;
            if (wait) e->freezeWait_ = id;
        }
        if (wait) {
            gst_clock_id_wait(id, nullptr);
            QMutexLocker lock(&e->mutex_);
            e->freezeWait_ = nullptr;
        }
        gst_clock_id_unref(id);
        gst_object_unref(clock);
    }

    static void unscheduleFreezeWait(GstEngine *e) {
        QMutexLocker lock(&e->mutex_);
        if (e->freezeWait_) gst_clock_id_unschedule(e->freezeWait_);
    }

    // PLY-09 DropBeforeVideoDecoder.
    static GstPadProbeReturn freezeProbe(GstPad *pad, GstPadProbeInfo *info, gpointer data) {
        Ctx *c = static_cast<Ctx *>(data);
        GstEngine *e = c->engine;
        if (GST_PAD_PROBE_INFO_TYPE(info) & GST_PAD_PROBE_TYPE_EVENT_FLUSH) {
            GstEvent *ev = GST_PAD_PROBE_INFO_EVENT(info);
            if (ev && GST_EVENT_TYPE(ev) == GST_EVENT_FLUSH_START) unscheduleFreezeWait(e);
            return GST_PAD_PROBE_OK;
        }
        GstBuffer *buffer = GST_PAD_PROBE_INFO_BUFFER(info);
        if (!buffer) return GST_PAD_PROBE_OK;
        int mode;
        {
            QMutexLocker lock(&e->mutex_);
            if (!alive(e, c->generation)) return GST_PAD_PROBE_REMOVE;
            mode = e->freezeMode_;
            if (mode == 2 && !GST_BUFFER_FLAG_IS_SET(buffer, GST_BUFFER_FLAG_DELTA_UNIT)) {
                // Thaw: the first keyframe goes on, marked DISCONT; probe done.
                buffer = gst_buffer_make_writable(buffer);
                GST_BUFFER_FLAG_SET(buffer, GST_BUFFER_FLAG_DISCONT);
                GST_PAD_PROBE_INFO_DATA(info) = buffer;
                e->freezeMode_ = 0;
                e->freezeProbe_ = 0;
                return GST_PAD_PROBE_REMOVE;
            }
            if (mode == 0) {
                e->freezeProbe_ = 0;
                return GST_PAD_PROBE_REMOVE;
            }
        }
        paceToClock(e, c->pipeline, pad, buffer);
        QMutexLocker lock(&e->mutex_);
        if (alive(e, c->generation)) ++e->stats_.freezeDropped;
        return GST_PAD_PROBE_DROP;
    }
};

// ---- Static set-up --------------------------------------------------------------

bool GstEngine::initialize(QString *error) {
    QMutexLocker lock(&gInitMutex);
    if (gInitialized) return true;
    QElapsedTimer clock;
    clock.start();
    GError *err = nullptr;
    if (!gst_init_check(nullptr, nullptr, &err)) {
        gInitError = QString::fromUtf8(err ? err->message : "gst_init_check failed");
        if (error) *error = gInitError;
        if (err) g_error_free(err);
        gInitMs = clock.elapsed();
        return false;
    }
    GstRegistry *registry = gst_registry_get();
    // PLT-08: hardware decoders are never autoplugged.
    const std::vector<std::string> demote = elementsToDemote();
    for (size_t i = 0; i < demote.size(); ++i) {
        GstPluginFeature *f = gst_registry_lookup_feature(registry, demote[i].c_str());
        if (!f) continue;
        gst_plugin_feature_set_rank(f, GST_RANK_NONE);
        gst_object_unref(f);
    }
    const std::vector<std::string> probe = elementsToProbe();
    for (size_t i = 0; i < probe.size(); ++i) {
        GstElementFactory *f = gst_element_factory_find(probe[i].c_str());
        if (!f) continue;
        GstPluginFeature *loaded = gst_plugin_feature_load(GST_PLUGIN_FEATURE(f));
        if (loaded) {
            gAvailable.insert(probe[i]);
            gst_object_unref(loaded);
        }
        gst_object_unref(f);
    }
    gInitialized = true;
    gInitError.clear();
    gInitMs = clock.elapsed();
    gInitializedFlag.storeRelease(1);
    return true;
}

std::set<std::string> GstEngine::availableElements() {
    waitInitialized(kInitWaitMs);
    QMutexLocker lock(&gInitMutex);
    return gAvailable;
}

bool GstEngine::initialized() { return gInitializedFlag.loadAcquire() != 0; }

void GstEngine::startInitialize(const std::function<void()> &afterInit) {
    InitWorker &w = initWorker();
    GstEngineInit::instance();  // created here, on the caller's (GUI) thread
    {
        QMutexLocker lock(&w.mutex);
        if (w.initRequested) return;
        w.initRequested = true;
        w.initPending = true;
    }
    w.post([afterInit]() {
        QString error;
        const bool ok = GstEngine::initialize(&error);
        qint64 ms = 0;
        {
            QMutexLocker lock(&gInitMutex);
            ms = gInitMs;
        }
        InitWorker &worker = initWorker();
        {
            QMutexLocker lock(&worker.mutex);
            worker.initPending = false;
            worker.cond.wakeAll();
        }
        if (afterInit) afterInit();
        postInitEvent(kInitReady, ok, error, ms, 0);
    });
}

bool GstEngine::waitInitialized(int timeoutMs) {
    if (initialized()) return true;
    {
        InitWorker &w = initWorker();
        QMutexLocker lock(&w.mutex);
        QElapsedTimer clock;
        clock.start();
        while (w.initPending) {
            const qint64 left = qint64(timeoutMs) - clock.elapsed();
            if (left <= 0) return initialized();
            w.cond.wait(&w.mutex, static_cast<unsigned long>(left));
        }
    }
    if (initialized()) return true;
    return initialize();  // nothing pending: synchronous (first use, or a retry after a failure)
}

std::vector<std::string> GstEngine::preloadFeatures() {
    static const char *const names[] = {
        // Decoders and parsers decodebin plugs for the first video (SEL: VP9/H.264 + Opus/AAC).
        "avdec_h264", "avdec_aac", "avdec_vp9", "vp9dec", "opusdec", "vorbisdec",
        "h264parse", "vp9parse", "aacparse", "opusparse",
        // Demuxers, the decodebin family and type finding.
        "matroskademux", "qtdemux", "uridecodebin", "decodebin", "multiqueue", "queue2", "typefind",
        "video/x-matroska", "video/quicktime",
        // Source, video output and the audio chain (already loaded by initialize(): no-ops).
        "souphttpsrc", "videoconvert", "appsink", "audioconvert", "audioresample", "scaletempo",
        "equalizer-10bands", "volume", "rglimiter", "audiodynamic", "pulsesink"};
    return std::vector<std::string>(names, names + sizeof(names) / sizeof(names[0]));
}

void GstEngine::preloadDecoders() {
    InitWorker &w = initWorker();
    GstEngineInit::instance();
    {
        QMutexLocker lock(&w.mutex);
        if (w.preloadRequested) return;
        w.preloadRequested = true;
    }
    w.post([]() {
        QElapsedTimer clock;
        clock.start();
        int loaded = 0;
        if (GstEngine::initialize()) {  // queued after the init job: normally already done
            GstRegistry *registry = gst_registry_get();
            const std::vector<std::string> names = preloadFeatures();
            for (size_t i = 0; i < names.size(); ++i) {
                GstPluginFeature *f = gst_registry_lookup_feature(registry, names[i].c_str());
                if (!f) continue;
                // Loads the plugin (dlopen + plugin_init) and returns the loaded feature;
                // no element is instantiated.
                GstPluginFeature *l = gst_plugin_feature_load(f);
                if (l) {
                    ++loaded;
                    gst_object_unref(l);
                }
                gst_object_unref(f);
            }
        }
        postInitEvent(kInitPreloaded, true, QString(), clock.elapsed(), loaded);
    });
}

// ---- GstEngineInit ---------------------------------------------------------------------

GstEngineInit::GstEngineInit() : QObject(nullptr) {}

GstEngineInit *GstEngineInit::instance() {
    static GstEngineInit *notifier = nullptr;
    static QMutex mutex;
    QMutexLocker lock(&mutex);
    if (!notifier) {
        notifier = new GstEngineInit;
        // Signals are emitted on the application's (GUI) thread.
        if (QCoreApplication *app = QCoreApplication::instance())
            if (notifier->thread() != app->thread()) notifier->moveToThread(app->thread());
    }
    return notifier;
}

bool GstEngineInit::event(QEvent *e) {
    if (e->type() != kInitEventType) return QObject::event(e);
    const InitEvent *ie = static_cast<const InitEvent *>(e);
    if (ie->kind == kInitReady) emit ready(ie->ok, ie->error, ie->ms);
    else if (ie->kind == kInitPreloaded) emit decodersPreloaded(ie->loaded, ie->ms);
    return true;
}

// ---- Lifetime -----------------------------------------------------------------------

GstEngine::GstEngine(VideoFrameMailbox *mailbox, QObject *parent) : QObject(parent), mailbox_(mailbox) {
    // One worker: teardowns run one after the other, never on the owner thread.
    GError *err = nullptr;
    teardownPool_ = g_thread_pool_new(&GstEngine::runTeardown, this, 1, FALSE, &err);
    if (err) g_error_free(err);  // without a pool teardown() runs synchronously
}

GstEngine::~GstEngine() {
    teardown();
    // Waits for every queued teardown: no worker or streaming thread outlives the engine.
    if (teardownPool_) g_thread_pool_free(teardownPool_, FALSE, TRUE);
    teardownPool_ = nullptr;
}

bool GstEngine::build(const PlanInput &input, QString *error) {
    teardown();
    // NFR-PERF-01: never race a gst_init running on the start-up worker.
    if (!waitInitialized(kInitWaitMs)) {
        plan_ = PipelinePlan();
        if (error) *error = QStringLiteral("GStreamer is not initialised");
        return false;
    }
    plan_ = buildPipelinePlan(input);
    if (!plan_.ok) {
        QString message;
        for (size_t i = 0; i < plan_.errors.size(); ++i)
            message += (i ? QStringLiteral("; ") : QString()) + QString::fromStdString(plan_.errors[i]);
        if (error) *error = message;
        return false;
    }
    if (plan_.decode == DecodeMode::Hardware) {
        // droidvdec / droideglsink are single-instance device resources: a hardware build waits
        // for an older hardware pipeline still going to NULL on the worker.
        QMutexLocker lock(&teardownMutex_);
        while (pendingHardwareTeardowns_ > 0) teardownCond_.wait(&teardownMutex_);
    }
    ++generation_;
    {
        QMutexLocker lock(&mutex_);
        stats_ = EngineStats();
        firstFramePosted_ = false;
        outputEnabled_ = true;
        if (heldPreroll_) gst_sample_unref(heldPreroll_);
        heldPreroll_ = nullptr;
        lastWidth_ = lastHeight_ = 0;
        freezeMode_ = 0;
        freezeProbe_ = 0;
        videoSegmentSeqnum_ = audioSegmentSeqnum_ = 0;
        videoSegmentStartMs_ = audioSegmentStartMs_ = -1;
        videoFlushSeqnum_ = audioFlushSeqnum_ = 0;
        liveGeneration_ = generation_;
    }
    decodersLogged_ = false;
    Ctx *ctx = new Ctx;  // freed by the teardown worker, after the pipeline is gone
    ctx->engine = this;
    ctx->generation = generation_;
    ctx_ = ctx;

    pipeline_ = gst_pipeline_new("sailtube-player");
    ctx->pipeline = pipeline_;
    GstBus *bus = gst_pipeline_get_bus(GST_PIPELINE(pipeline_));
    gst_bus_set_sync_handler(bus, &Callbacks::busSync, ctx, nullptr);
    gst_object_unref(bus);
    g_signal_connect(pipeline_, "deep-element-added", G_CALLBACK(&Callbacks::deepElementAdded), ctx);

    QString err;
    bool ok = true;
    for (size_t bi = 0; ok && bi < plan_.branches.size(); ++bi) {
        const BranchPlan &bp = plan_.branches[bi];
        std::unique_ptr<BranchCtx> b(new BranchCtx);
        b->engine = this;
        b->generation = generation_;
        b->index = static_cast<int>(bi);
        b->branch = bp.branch;
        b->network = bp.network;
        b->hideSize = bp.hideSizeUntilDemuxReady;
        b->sourceSetup = bp.sourceSetup;
        b->unmatched = plan_.unmatchedPadSink;
        b->source = makeElement(bp.source, &err);
        if (!b->source) { ok = false; break; }
        gst_bin_add(GST_BIN(pipeline_), b->source);
        if (bp.branch == Branch::Video || bp.branch == Branch::Muxed) videoSource_ = b->source;
        ctx->branches.push_back(b.get());
        g_signal_connect(b->source, "source-setup", G_CALLBACK(&Callbacks::sourceSetup), b.get());
        g_signal_connect(b->source, "pad-added", G_CALLBACK(&Callbacks::padAdded), b.get());
        g_signal_connect(b->source, "no-more-pads", G_CALLBACK(&Callbacks::noMorePads), b.get());
        for (size_t ri = 0; ok && ri < bp.routes.size(); ++ri) {
            const PadRoute &rp = bp.routes[ri];
            RouteCtx route;
            route.kind = rp.kind;
            route.prefixes = rp.capsPrefixes;
            for (size_t ei = 0; ei < rp.chain.size(); ++ei) {
                GstElement *e = makeElement(rp.chain[ei], &err);
                if (!e) { ok = false; break; }
                gst_bin_add(GST_BIN(pipeline_), e);
                if (!route.chain.empty()) {
                    GstElement *prev = route.chain.back();
                    const std::string &capsText = rp.chain[ei - 1].linkCapsToNext;
                    bool linked;
                    if (!capsText.empty()) {
                        GstCaps *caps = gst_caps_from_string(capsText.c_str());
                        linked = gst_element_link_filtered(prev, e, caps);
                        if (caps) gst_caps_unref(caps);
                    } else {
                        linked = gst_element_link(prev, e);
                    }
                    if (!linked) {
                        err = QStringLiteral("cannot link %1 -> %2").arg(rp.chain[ei - 1].name.c_str(), rp.chain[ei].name.c_str());
                        ok = false;
                        break;
                    }
                }
                route.chain.push_back(e);
                if (rp.chain[ei].name == elementnames::kVideoSink) videoSink_ = e;
                if (rp.chain[ei].name == elementnames::kAudioSink) audioSink_ = e;
                if (rp.chain[ei].name == elementnames::kVideoDecoder) {
                    decoderSinkPad_ = gst_element_get_static_pad(e, "sink");  // hardware path
                    {
                        QMutexLocker lock(&mutex_);
                        stats_.videoDecoder = QString::fromStdString(rp.chain[ei].factory);
                    }
                    Callbacks::addHoldProbe(this, b.get(), e);   // PLY-06 start seek
                    Callbacks::watchFirstDecoded(ctx, e);         // NFR-PERF timing
                }
            }
            b->routes.push_back(route);
        }
        branches_.push_back(std::move(b));
    }
    ctx->videoSource = videoSource_;
    if (ok && videoSink_) {
        if (GST_IS_APP_SINK(videoSink_)) {
            GstAppSinkCallbacks cb;
            std::memset(&cb, 0, sizeof(cb));
            cb.new_preroll = &Callbacks::newPreroll;
            cb.new_sample = &Callbacks::newSample;
            gst_app_sink_set_callbacks(GST_APP_SINK(videoSink_), &cb, ctx, nullptr);
            ctx->sampleSink = true;
        }
        GstPad *pad = gst_element_get_static_pad(videoSink_, "sink");
        gst_pad_add_probe(pad, GstPadProbeType(GST_PAD_PROBE_TYPE_BUFFER | GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM |
                                               GST_PAD_PROBE_TYPE_EVENT_FLUSH),
                          &Callbacks::videoSinkProbe, ctx, nullptr);
        gst_object_unref(pad);
    }
    if (ok && videoSink_ && plan_.decode == DecodeMode::Hardware) attachHardwareSink();
    if (ok && audioSink_) {
        GstPad *pad = gst_element_get_static_pad(audioSink_, "sink");
        gst_pad_add_probe(pad, GstPadProbeType(GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM | GST_PAD_PROBE_TYPE_EVENT_FLUSH),
                          &Callbacks::audioSinkProbe, ctx, nullptr);
        gst_object_unref(pad);
    }
    if (!ok) {
        teardown();
        if (error) *error = err;
        return false;
    }
    return true;
}

void GstEngine::teardown() {
    if (!pipeline_) return;
    // PLY-09: a state-locked bin would not follow the pipeline to NULL.
    if (frozenTarget_ == FreezeTarget::LockAndPauseVideoSource && videoSource_)
        gst_element_set_locked_state(videoSource_, FALSE);
    {
        // Cut the old pipeline off: from here on no streaming-thread callback touches the
        // engine's state (alive() fails), held decoders and a pacing wait return at once.
        QMutexLocker lock(&mutex_);
        liveGeneration_ = 0;
        holdCond_.wakeAll();
        if (freezeProbe_ && decoderSinkPad_) gst_pad_remove_probe(decoderSinkPad_, freezeProbe_);
        freezeProbe_ = 0;
        freezeMode_ = 0;
        if (freezeWait_) gst_clock_id_unschedule(freezeWait_);
    }
    frozenTarget_ = FreezeTarget::None;
    stopHardwareSink();  // held frames go back before the sink stops
    TeardownJob *job = new TeardownJob;
    job->startedUs = g_get_monotonic_time();
    job->hardware = plan_.decode == DecodeMode::Hardware;
    if (hwBridge_) {
        // The surface stops drawing from it now; the worker detaches it after NULL.
        {
            QMutexLocker lock(&mutex_);
            hwSink_ = nullptr;
        }
        if (mailbox_) mailbox_->setHardwareSource(std::shared_ptr<HardwareFrameSource>());
        job->bridge = hwBridge_;
        hwBridge_.reset();
    }
    GstBus *bus = gst_pipeline_get_bus(GST_PIPELINE(pipeline_));
    gst_bus_set_sync_handler(bus, nullptr, nullptr, nullptr);  // a handler can only be replaced after clearing
    gst_bus_set_sync_handler(bus, &dropAll, nullptr, nullptr);
    gst_object_unref(bus);
    {
        QMutexLocker lock(&mutex_);
        if (decoderSinkPad_) gst_object_unref(decoderSinkPad_);
        decoderSinkPad_ = nullptr;
        if (heldPreroll_) gst_sample_unref(heldPreroll_);
        heldPreroll_ = nullptr;
        videoSink_ = audioSink_ = videoSource_ = nullptr;
    }
    job->pipeline = pipeline_;
    job->ctx = ctx_;
    job->branches.swap(branches_);
    pipeline_ = nullptr;
    ctx_ = nullptr;
    ++generation_;  // anything still queued from this pipeline is dropped
    if (job->hardware) {
        QMutexLocker lock(&teardownMutex_);
        ++pendingHardwareTeardowns_;
    }
    if (!teardownPool_ || !g_thread_pool_push(teardownPool_, job, nullptr)) runTeardown(job, this);
}

// Teardown worker: NULL (waits for every streaming thread of the old pipeline),
// detach the hardware sink, free the pipeline and its callback contexts.
void GstEngine::runTeardown(void *data, void *engineData) {
    TeardownJob *job = static_cast<TeardownJob *>(data);
    GstEngine *e = static_cast<GstEngine *>(engineData);
    gst_element_set_state(job->pipeline, GST_STATE_NULL);
    if (job->bridge) job->bridge->detachSink();  // no streaming thread can emit any more
    job->bridge.reset();  // the surface may hold it a little longer; it draws nothing
    gst_object_unref(job->pipeline);
    job->branches.clear();
    delete job->ctx;
    const qint64 ms = (g_get_monotonic_time() - job->startedUs) / 1000;
    const bool hardware = job->hardware;
    delete job;
    if (hardware) {
        QMutexLocker lock(&e->teardownMutex_);
        --e->pendingHardwareTeardowns_;
        e->teardownCond_.wakeAll();
    }
    // Generation 0: delivered whatever was built since (see event()).
    Callbacks::post(e, 0, kTeardownDone, static_cast<int>(ms), 0, nullptr);
}

// ---- PLY-06 start seek before preroll ----------------------------------------------------

GstEngine::BranchCtx *GstEngine::branchFor(Branch which) const {
    for (size_t i = 0; i < branches_.size(); ++i)
        if (branches_[i]->branch == which) return branches_[i].get();
    return nullptr;
}

void GstEngine::holdForStartSeek() {
    if (!pipeline_) return;
    QMutexLocker lock(&mutex_);
    for (size_t i = 0; i < branches_.size(); ++i) {
        BranchCtx &b = *branches_[i];
        if (b.holdWanted || b.released) continue;
        b.holdWanted = true;
        if (b.flowed && !b.heldPosted) {
            b.heldPosted = true;  // data already reached a decoder: too late for this branch
            Callbacks::post(this, generation_, kBranchNotHeld, static_cast<int>(b.branch), 0, nullptr);
        }
    }
}

bool GstEngine::sendStartSeek(const SeekCommand &c, quint32 *seqnum) {
    BranchCtx *b = pipeline_ ? branchFor(c.branch) : nullptr;
    if (!b) return false;
    GstPad *pad = nullptr;
    {
        QMutexLocker lock(&mutex_);
        if (!b->holdWanted || b->released || b->passAll) return false;
        for (size_t i = 0; i < b->holdPads.size() && !pad; ++i)
            if (b->holdPads[i]->waiting) pad = GST_PAD(gst_object_ref(b->holdPads[i]->pad));
        if (!pad) return false;  // not held (yet)
    }
    // Upstream unseekable (e.g. a source without Range support): run unchanged, nothing dropped.
    GstQuery *q = gst_query_new_seeking(GST_FORMAT_TIME);
    gboolean seekable = TRUE;
    const bool answered = gst_pad_peer_query(pad, q);
    if (answered) gst_query_parse_seeking(q, nullptr, &seekable, nullptr, nullptr);
    gst_query_unref(q);
    if (qEnvironmentVariableIsSet("SAILTUBE_PLAYER_DEBUG"))
        std::fprintf(stderr, "player: start seek at %s:%s: seeking query %s, seekable %d\n", GST_DEBUG_PAD_NAME(pad),
                     answered ? "answered" : "unanswered", int(seekable));
    if (!seekable) {
        gst_object_unref(pad);
        releaseStartHolds();
        return false;
    }
    int flags = GST_SEEK_FLAG_NONE;
    if (c.flags.flush) flags |= GST_SEEK_FLAG_FLUSH;
    if (c.flags.accurate) flags |= GST_SEEK_FLAG_ACCURATE;
    if (c.flags.keyUnit) flags |= GST_SEEK_FLAG_KEY_UNIT;
    GstEvent *seek = gst_event_new_seek(c.rate > 0 ? c.rate : 1.0, GST_FORMAT_TIME, GstSeekFlags(flags), GST_SEEK_TYPE_SET,
                                        c.positionNs < 0 ? 0 : c.positionNs, GST_SEEK_TYPE_NONE, GST_CLOCK_TIME_NONE);
    const quint32 seq = gst_event_get_seqnum(seek);
    if (seqnum) *seqnum = seq;
    {
        // Release first, then seek: the held thread drops what it holds and everything older
        // than the seek, so a seek that needs data to flow first (e.g. a push-mode WebM demuxer
        // fetching its Cues) can never block on the held decoder.
        QMutexLocker lock(&mutex_);
        b->startSeqnum = seq;
        b->released = true;
        holdCond_.wakeAll();
    }
    const bool ok = gst_pad_push_event(pad, seek);  // upstream: parser / multiqueue -> demuxer
    if (qEnvironmentVariableIsSet("SAILTUBE_PLAYER_DEBUG"))
        std::fprintf(stderr, "player: start seek at %s:%s (seqnum %u): %s\n", GST_DEBUG_PAD_NAME(pad), seq,
                     ok ? "accepted" : "refused");
    gst_object_unref(pad);
    if (!ok) {
        // Refused (rare: upstream said seekable): run unchanged; the decoder resyncs at the next
        // keyframe and the classic barrier seek at the first async-done applies the start.
        QMutexLocker lock(&mutex_);
        b->passAll = true;
        holdCond_.wakeAll();
    }
    return ok;
}

void GstEngine::releaseStartHolds() {
    QMutexLocker lock(&mutex_);
    for (size_t i = 0; i < branches_.size(); ++i) {
        BranchCtx &b = *branches_[i];
        if (!b.released) b.passAll = true;
    }
    holdCond_.wakeAll();
}

// ---- Operations ------------------------------------------------------------------

bool GstEngine::setState(PipelineState state) {
    if (!pipeline_) return state == PipelineState::Null;
    GstState s = GST_STATE_NULL;
    switch (state) {
    case PipelineState::Null: s = GST_STATE_NULL; break;
    case PipelineState::Ready: s = GST_STATE_READY; break;
    case PipelineState::Paused: s = GST_STATE_PAUSED; break;
    case PipelineState::Playing: s = GST_STATE_PLAYING; break;
    }
    return gst_element_set_state(pipeline_, s) != GST_STATE_CHANGE_FAILURE;
}

GstElement *GstEngine::sinkFor(Branch which) const {
    switch (which) {
    case Branch::Video: return videoSink_;
    case Branch::Audio: return audioSink_;
    case Branch::Muxed: return pipeline_;
    }
    return pipeline_;
}

bool GstEngine::sendSeek(const SeekCommand &c, quint32 *seqnum) {
    GstElement *target = sinkFor(c.branch);
    if (!target || !pipeline_) return false;
    int flags = GST_SEEK_FLAG_NONE;
    if (c.flags.flush) flags |= GST_SEEK_FLAG_FLUSH;
    if (c.flags.accurate) flags |= GST_SEEK_FLAG_ACCURATE;
    if (c.flags.keyUnit) flags |= GST_SEEK_FLAG_KEY_UNIT;
    GstEvent *seek = gst_event_new_seek(c.rate > 0 ? c.rate : 1.0, GST_FORMAT_TIME, GstSeekFlags(flags), GST_SEEK_TYPE_SET,
                                        c.positionNs < 0 ? 0 : c.positionNs, GST_SEEK_TYPE_NONE, GST_CLOCK_TIME_NONE);
    if (seqnum) *seqnum = gst_event_get_seqnum(seek);
    // Under the pipeline's state lock. After the preroll that follows a flush GstBin goes back to
    // PLAYING on a thread of its own (its state continuation, which holds that lock). A flush
    // that reaches a sink while the continuation runs, after it has set that sink to PLAYING,
    // makes the sink lose its state without the busy bin taking note: the bin commits PLAYING,
    // and the sink, which relies on its parent to bring it back, stays in PAUSED until the next
    // state change (audio or picture stops while the pipeline reports PLAYING). A seek sent to
    // the pipeline is serialised by gst_element_send_event() itself; a seek sent to one sink
    // (Dual, AudioOnly) only takes that sink's lock, so the engine takes the pipeline's here.
    // With it the flush lands before the continuation (the bin then waits for the sink's
    // preroll) or after it (an ordinary lost state), whoever sends the seek and whenever: the
    // audio re-align and a queued seek from the async-done handler, a user seek, a resend.
    GST_STATE_LOCK(pipeline_);
    const bool accepted = gst_element_send_event(target, seek);
    GST_STATE_UNLOCK(pipeline_);
    return accepted;
}

bool GstEngine::sinkSawSegment(Branch which, quint32 seqnum) const {
    QMutexLocker lock(&mutex_);
    return (which == Branch::Video ? videoSegmentSeqnum_ : audioSegmentSeqnum_) == seqnum;
}

bool GstEngine::sinkSegmentStart(Branch which, quint32 seqnum, qint64 *ms) const {
    QMutexLocker lock(&mutex_);
    const bool video = which == Branch::Video;
    if ((video ? videoSegmentSeqnum_ : audioSegmentSeqnum_) != seqnum) return false;
    const qint64 start = video ? videoSegmentStartMs_ : audioSegmentStartMs_;
    if (start < 0) return false;
    if (ms) *ms = start;
    return true;
}

bool GstEngine::queryPosition(Branch which, qint64 *ms) const {
    GstElement *target = sinkFor(which);
    gint64 ns = 0;
    if (!target || !gst_element_query_position(target, GST_FORMAT_TIME, &ns) || ns < 0) return false;
    if (ms) *ms = ns / 1000000;
    return true;
}

bool GstEngine::queryDuration(qint64 *ms) const {
    gint64 ns = 0;
    if (!pipeline_ || !gst_element_query_duration(pipeline_, GST_FORMAT_TIME, &ns) || ns <= 0) return false;
    if (ms) *ms = ns / 1000000;
    return true;
}

bool GstEngine::querySeekable(bool *seekable) const {
    if (!pipeline_) return false;
    GstQuery *q = gst_query_new_seeking(GST_FORMAT_TIME);
    const bool ok = gst_element_query(pipeline_, q);
    gboolean s = FALSE;
    if (ok) gst_query_parse_seeking(q, nullptr, &s, nullptr, nullptr);
    gst_query_unref(q);
    if (ok && seekable) *seekable = s;
    return ok;
}

bool GstEngine::queryBuffered(Branch which, qint64 positionMs, qint64 durationMs, double rate, qint64 *bufferedMs) const {
    if (!pipeline_) return false;
    const BranchCtx *b = branchFor(which);
    if (!b && branches_.size() == 1) b = branches_[0].get();
    if (!b) return false;
    if (positionMs < 0) positionMs = 0;
    qint64 best = -1;
    // The demuxed data ahead of the decoders: the shortest non-empty queue (an empty one is a
    // stream that has nothing more right now, e.g. an unrouted track).
    GstElement *mq = nullptr;
    {
        QMutexLocker lock(&mutex_);
        if (b->multiqueue) mq = GST_ELEMENT(gst_object_ref(b->multiqueue));
    }
    if (mq) {
        if (g_object_class_find_property(G_OBJECT_GET_CLASS(mq), "stats")) {  // GStreamer 1.18+
            GstStructure *stats = nullptr;
            g_object_get(mq, "stats", &stats, nullptr);
            if (stats) {
                const GValue *queues = gst_structure_get_value(stats, "queues");
                if (queues && GST_VALUE_HOLDS_ARRAY(queues)) {
                    guint64 shortest = G_MAXUINT64;
                    for (guint i = 0; i < gst_value_array_get_size(queues); ++i) {
                        const GValue *q = gst_value_array_get_value(queues, i);
                        if (!GST_VALUE_HOLDS_STRUCTURE(q)) continue;
                        const GstStructure *s = gst_value_get_structure(q);
                        guint buffers = 0;
                        guint64 time = 0;
                        if (!gst_structure_get_uint(s, "buffers", &buffers) || buffers == 0) continue;
                        if (!gst_structure_get_uint64(s, "time", &time)) continue;
                        if (time < shortest) shortest = time;
                    }
                    const double scale = rate > 0 ? rate : 1.0;
                    best = positionMs + (shortest == G_MAXUINT64 ? 0 : static_cast<qint64>(shortest / 1000000 * scale));
                }
                gst_structure_free(stats);
            }
        }
        gst_object_unref(mq);
    }
    // The bytes the branch's source fetched so far (an in-memory queue holds them before the
    // demuxer), as a share of the media: bytes and time are taken as proportional. Not for HLS:
    // its source fetches the playlist, not the media.
    if (durationMs > 0 && plan_.mode != PlayMode::Hls && b->source &&
        g_object_class_find_property(G_OBJECT_GET_CLASS(b->source), "source")) {
        GstElement *source = nullptr;
        g_object_get(b->source, "source", &source, nullptr);
        if (source) {
            gint64 read = 0, total = 0;
            if (gst_element_query_position(source, GST_FORMAT_BYTES, &read) &&
                gst_element_query_duration(source, GST_FORMAT_BYTES, &total) && read > 0 && total > 0) {
                const qint64 ms = static_cast<qint64>(static_cast<double>(read) / total * durationMs);
                if (ms > best) best = ms;
            }
            gst_object_unref(source);
        }
    }
    // A disk-backed branch (download buffer): the downloaded byte range holding the position.
    GstElement *target = sinkFor(which);
    if (!target) target = pipeline_;
    if (durationMs > 0) {
        GstQuery *q = gst_query_new_buffering(GST_FORMAT_PERCENT);
        if (gst_element_query(target, q)) {
            GstFormat format = GST_FORMAT_UNDEFINED;
            gint64 start = -1, stop = -1, total = -1;
            gst_query_parse_buffering_range(q, &format, &start, &stop, &total);
            if (format == GST_FORMAT_PERCENT) {
                // Bytes and time are taken as proportional: a 2 % slack for the container's overhead.
                const double at = static_cast<double>(positionMs) / durationMs * GST_FORMAT_PERCENT_MAX;
                const double slack = GST_FORMAT_PERCENT_MAX / 50.0;
                gint64 end = -1;
                const guint ranges = gst_query_get_n_buffering_ranges(q);
                for (guint i = 0; i < ranges; ++i) {
                    gint64 rs = 0, re = 0;
                    if (!gst_query_parse_nth_buffering_range(q, i, &rs, &re)) continue;
                    if (rs <= at + slack && re + slack >= at && re > end) end = re;
                }
                if (end > 0) {
                    const qint64 ms = static_cast<qint64>(static_cast<double>(end) / GST_FORMAT_PERCENT_MAX * durationMs);
                    if (ms > best) best = ms;
                }
            }
        }
        gst_query_unref(q);
    }
    if (best < 0) return false;
    if (best < positionMs) best = positionMs;
    if (durationMs > 0 && best > durationMs) best = durationMs;
    if (bufferedMs) *bufferedMs = best;
    return true;
}

void GstEngine::freeze(FreezeTarget target) {
    if (!pipeline_ || frozenTarget_ != FreezeTarget::None || target == FreezeTarget::None) return;
    {
        QMutexLocker lock(&mutex_);
        if (!decoderSinkPad_) {
            lock.unlock();
            emit warningOccurred(QStringLiteral("freeze: no video decoder found"));
            return;
        }
        freezeMode_ = 1;
        if (!freezeProbe_)
            freezeProbe_ = gst_pad_add_probe(decoderSinkPad_,
                                             GstPadProbeType(GST_PAD_PROBE_TYPE_BUFFER | GST_PAD_PROBE_TYPE_EVENT_FLUSH),
                                             &Callbacks::freezeProbe, ctx_, nullptr);
    }
    setSinkAsync(false);
    if (target == FreezeTarget::LockAndPauseVideoSource && videoSource_) {
        gst_element_set_locked_state(videoSource_, TRUE);
        gst_element_set_state(videoSource_, GST_STATE_PAUSED);
    }
    frozenTarget_ = target;
}

void GstEngine::thaw(FreezeTarget) {
    if (!pipeline_ || frozenTarget_ == FreezeTarget::None) return;
    if (frozenTarget_ == FreezeTarget::LockAndPauseVideoSource && videoSource_) {
        gst_element_set_locked_state(videoSource_, FALSE);
        gst_element_sync_state_with_parent(videoSource_);
    }
    {
        QMutexLocker lock(&mutex_);
        if (freezeProbe_) freezeMode_ = 2;  // the probe removes itself at the next keyframe
    }
    setSinkAsync(true);
    frozenTarget_ = FreezeTarget::None;
}

// ---- Hardware sink (PLY-20) ----------------------------------------------------------

void GstEngine::attachHardwareSink() {
    const quint64 generation = generation_;
    VideoFrameMailbox *mailbox = mailbox_;
    EglSinkBridge::Callbacks cb;
    cb.frameAnnounced = [this, mailbox, generation]() {
        if (mailbox) mailbox->notifyListeners();
        noteHardwareFrame(generation);
    };
    cb.geometryChanged = [mailbox]() {
        if (mailbox) mailbox->notifyListeners();
    };
    cb.renderFailed = [this, generation]() { Callbacks::post(this, generation, kHardwareRenderFailed, 0, 0, nullptr); };
    std::string why;
    hwBridge_ = EglSinkBridge::create(videoSink_, cb, &why);
    hwFailure_.clear();
    if (!hwBridge_) {
        // Frames could never be shown: report it once the owner loop runs.
        hwFailure_ = QString::fromStdString(why);
        Callbacks::post(this, generation, kHardwareRenderFailed, 0, 0, nullptr);
        return;
    }
    {
        QMutexLocker lock(&mutex_);
        hwSink_ = hwBridge_.get();
    }
    if (qEnvironmentVariableIsSet("SAILTUBE_PLAYER_DEBUG"))
        std::fprintf(stderr, "player: zero-copy sink in %s mode\n",
                     hwBridge_->mode() == EglSinkBridge::Mode::Texture ? "texture (NemoGstVideoTexture)" : "buffer (show-frame)");
    if (mailbox_) mailbox_->setHardwareSource(hwBridge_);
}

void GstEngine::stopHardwareSink() {
    if (hwBridge_) hwBridge_->stopRendering();
}

void GstEngine::detachHardwareSink() {
    if (!hwBridge_) return;
    hwBridge_->detachSink();
    {
        QMutexLocker lock(&mutex_);
        hwSink_ = nullptr;
    }
    if (mailbox_) mailbox_->setHardwareSource(std::shared_ptr<HardwareFrameSource>());
    hwBridge_.reset();  // the surface may hold it a little longer; it draws nothing
}

void GstEngine::noteHardwareFrame(quint64 generation) {
    bool first = false;
    {
        QMutexLocker lock(&mutex_);
        if (!Callbacks::alive(this, generation)) return;  // a frame of a torn-down pipeline
        if (!firstFramePosted_ && outputEnabled_) {
            firstFramePosted_ = true;
            first = true;
        }
    }
    if (first) Callbacks::post(this, generation, kFirstFrame, 0, 0, nullptr);
}

GstElement *GstEngine::hardwareVideoSink() const {
    return hwBridge_ ? videoSink_ : nullptr;
}

// While frozen the video sink gets no buffers; with async=false it never holds
// the pipeline in an async state change (pause, buffering, seek while hidden).
void GstEngine::setSinkAsync(bool async) {
    if (videoSink_ && g_object_class_find_property(G_OBJECT_GET_CLASS(videoSink_), "async"))
        g_object_set(videoSink_, "async", async ? TRUE : FALSE, nullptr);
}

void GstEngine::setVideoOutputEnabled(bool enabled) {
    GstSample *held = nullptr;
    bool first = false;
    {
        QMutexLocker lock(&mutex_);
        if (outputEnabled_ == enabled) return;
        outputEnabled_ = enabled;
        if (enabled) {
            held = heldPreroll_;
            heldPreroll_ = nullptr;
            if (held && !firstFramePosted_) { firstFramePosted_ = true; first = true; }
        }
    }
    if (held && ctx_) Callbacks::postSample(this, generation_, held, false);
    else if (held) gst_sample_unref(held);
    // PLY-06 on the hardware path: the bridge holds announced frames back too.
    if (hwBridge_ && hwBridge_->setOutputEnabled(enabled) && enabled) {
        QMutexLocker lock(&mutex_);
        if (!firstFramePosted_) { firstFramePosted_ = true; first = true; }
    }
    if (first) emit firstVideoFrame();
}

void GstEngine::applyAudioValues(const AudioChainValues &values) {
    if (!pipeline_) return;
    const std::vector<ElementProperties> blocks = audioChainProperties(values, plan_.limiterFactory);
    for (size_t i = 0; i < blocks.size(); ++i) {
        GstElement *e = gst_bin_get_by_name(GST_BIN(pipeline_), blocks[i].element.c_str());
        if (!e) continue;
        QString ignored;
        applyAll(e, blocks[i].properties, &ignored);
        gst_object_unref(e);
    }
}

EngineStats GstEngine::stats() const {
    QMutexLocker lock(&mutex_);
    return stats_;
}

void GstEngine::resetStats() {
    QMutexLocker lock(&mutex_);
    const EngineStats kept = stats_;
    stats_ = EngineStats();
    stats_.videoSegmentRate = kept.videoSegmentRate;
    stats_.audioSegmentRate = kept.audioSegmentRate;
    stats_.firstVideoSinkMs = kept.firstVideoSinkMs;
    stats_.videoDecoder = kept.videoDecoder;
    stats_.audioDecoder = kept.audioDecoder;
    stats_.demuxerSizeQueriesHidden = kept.demuxerSizeQueriesHidden;
    stats_.unroutedPads = kept.unroutedPads;
    stats_.unroutedCaps = kept.unroutedCaps;
}

// ---- Owner-thread delivery -----------------------------------------------------------

bool GstEngine::event(QEvent *ev) {
    if (ev->type() != kEngineEventType) return QObject::event(ev);
    EngineEvent *e = static_cast<EngineEvent *>(ev);
    if (e->kind == kTeardownDone) {
        emit teardownDone(e->a);
        return true;
    }
    if (e->generation != generation_ || !pipeline_) return true;  // stale
    switch (e->kind) {
    case kDemuxReady: emit demuxReady(e->a); break;
    case kFirstDecoded: emit firstDecodedVideo(); break;
    case kDecoderHeld: emit decoderHeld(e->a); break;
    case kBranchNotHeld: emit branchNotHeld(e->a); break;
    case kStartSegment: {
        const BranchCtx *b = branchFor(static_cast<Branch>(e->a));
        quint32 seq = 0;
        {
            QMutexLocker lock(&mutex_);
            if (b) seq = b->startSeqnum;
        }
        emit startSegment(e->a, seq, e->b);
        break;
    }
    case kBusMessage: handleMessage(e->message); break;
    case kNoMorePads: removeUnlinkedRoutes(e->a); break;
    case kFirstFrame: emit firstVideoFrame(); break;
    case kFirstByte: emit firstByte(e->a); break;
    case kVideoSize: emit videoSizeChanged(e->a, e->b); break;
    case kHardwareRenderFailed: {
        QString reason = hwBridge_ ? QString::fromStdString(hwBridge_->failureReason()) : hwFailure_;
        if (reason.isEmpty()) reason = QStringLiteral("hardware frames cannot be shown");
        emit hardwareRenderFailed(reason);
        break;
    }
    default: break;
    }
    return true;
}

int GstEngine::branchOf(GstElement *element) const {
    if (!element) return -1;
    for (size_t i = 0; i < branches_.size(); ++i) {
        const BranchCtx &b = *branches_[i];
        if (element == b.source || gst_object_has_as_ancestor(GST_OBJECT(element), GST_OBJECT(b.source)))
            return static_cast<int>(b.branch);
        for (size_t r = 0; r < b.routes.size(); ++r)
            for (size_t c = 0; c < b.routes[r].chain.size(); ++c)
                if (b.routes[r].chain[c] == element) return static_cast<int>(b.branch);
    }
    return -1;
}

void GstEngine::handleMessage(GstMessage *m) {
    GstObject *src = GST_MESSAGE_SRC(m);
    const bool fromPipeline = src == GST_OBJECT(pipeline_);
    switch (GST_MESSAGE_TYPE(m)) {
    case GST_MESSAGE_ASYNC_DONE:
        if (fromPipeline && !decodersLogged_) {
            // PLT-08 diagnostics: the decoders this build really uses (R2: an autoplugged droidadec
            // decoded no YouTube AAC frames; it must never be picked).
            decodersLogged_ = true;
            QString video, audio, unrouted;
            bool output;
            {
                QMutexLocker lock(&mutex_);
                video = stats_.videoDecoder;
                audio = stats_.audioDecoder;
                unrouted = stats_.unroutedCaps;
                output = outputEnabled_;
            }
            // Also: streams that reached no route (fakesink) and whether video frames reach the
            // mailbox at this first preroll (run 5: a local file played without a picture).
            qInfo("sailtube: decoders video=%s audio=%s unrouted=%s video-output=%s",
                  video.isEmpty() ? "none" : qPrintable(video), audio.isEmpty() ? "none" : qPrintable(audio),
                  unrouted.isEmpty() ? "none" : qPrintable(unrouted), output ? "on" : "off");
        }
        if (fromPipeline) emit asyncDone();
        break;
    case GST_MESSAGE_EOS:
        if (fromPipeline) emit endOfStream();
        break;
    case GST_MESSAGE_STATE_CHANGED:
        if (fromPipeline) {
            GstState oldState, newState, pending;
            gst_message_parse_state_changed(m, &oldState, &newState, &pending);
            int s = 0;
            switch (newState) {
            case GST_STATE_READY: s = int(PipelineState::Ready); break;
            case GST_STATE_PAUSED: s = int(PipelineState::Paused); break;
            case GST_STATE_PLAYING: s = int(PipelineState::Playing); break;
            default: s = int(PipelineState::Null); break;
            }
            emit pipelineStateChanged(s);
        }
        break;
    case GST_MESSAGE_BUFFERING: {
        gint percent = 100;
        gst_message_parse_buffering(m, &percent);
        const int branch = GST_IS_ELEMENT(src) ? branchOf(GST_ELEMENT(src)) : -1;
        emit bufferingPercent(branch < 0 ? int(Branch::Muxed) : branch, percent);
        break;
    }
    case GST_MESSAGE_QOS:
        if (src == GST_OBJECT(videoSink_)) {
            GstFormat format;
            guint64 processed = 0, dropped = 0;
            gst_message_parse_qos_stats(m, &format, &processed, &dropped);
            QMutexLocker lock(&mutex_);
            stats_.framesDropped = static_cast<qint64>(dropped);
        }
        break;
    case GST_MESSAGE_CLOCK_LOST:
        emit clockLost();
        break;
    case GST_MESSAGE_ERROR: {
        GError *err = nullptr;
        gchar *debug = nullptr;
        gst_message_parse_error(m, &err, &debug);
        const int branch = GST_IS_ELEMENT(src) ? branchOf(GST_ELEMENT(src)) : -1;
        bool network = false;
        for (size_t i = 0; i < branches_.size(); ++i)
            if (static_cast<int>(branches_[i]->branch) == branch) network = branches_[i]->network;
        network = network && err && err->domain == GST_RESOURCE_ERROR;
        const QString message = QString::fromUtf8(err ? err->message : "unknown error");
        const QString dbg = QString::fromUtf8(debug ? debug : "");
        if (err) g_error_free(err);
        g_free(debug);
        // RES-07: souphttpsrc puts the HTTP status of a refused request into the error's details
        // (GST_ELEMENT_ERROR_WITH_DETAILS, field "http-status-code", G_TYPE_UINT; the message
        // itself only carries the reason phrase).
        int httpStatus = 0;
        const GstStructure *details = nullptr;
        gst_message_parse_error_details(m, &details);
        if (details && gst_structure_has_field(details, "http-status-code")) {
            guint status = 0;
            gint signedStatus = 0;
            if (gst_structure_get_uint(details, "http-status-code", &status)) httpStatus = static_cast<int>(status);
            else if (gst_structure_get_int(details, "http-status-code", &signedStatus)) httpStatus = signedStatus;
            if (httpStatus < 100 || httpStatus > 599) httpStatus = 0;
        }
        emit errorOccurred(message, dbg, branch, network, httpStatus);
        break;
    }
    case GST_MESSAGE_WARNING: {
        GError *err = nullptr;
        gchar *debug = nullptr;
        gst_message_parse_warning(m, &err, &debug);
        const QString message = QString::fromUtf8(err ? err->message : "warning");
        if (err) g_error_free(err);
        g_free(debug);
        emit warningOccurred(message);
        break;
    }
    default:
        break;
    }
}

void GstEngine::removeUnlinkedRoutes(int index) {
    if (index < 0 || index >= static_cast<int>(branches_.size())) return;
    BranchCtx &b = *branches_[static_cast<size_t>(index)];
    for (size_t r = 0; r < b.routes.size(); ++r) {
        RouteCtx &route = b.routes[r];
        bool linked;
        {
            QMutexLocker lock(&mutex_);
            linked = route.linked;
            route.linked = true;  // never route a late pad to removed elements
        }
        if (linked || route.chain.empty()) continue;
        emit warningOccurred(QStringLiteral("no %1 stream in source; route removed")
                                 .arg(route.kind == RouteKind::Video ? "video" : "audio"));
        bool hadHardwareSink = false;
        for (size_t c = 0; c < route.chain.size(); ++c)
            if (route.chain[c] == videoSink_ && hwBridge_) hadHardwareSink = true;
        if (hadHardwareSink) stopHardwareSink();
        for (size_t c = 0; c < route.chain.size(); ++c) {
            GstElement *e = route.chain[c];
            {
                QMutexLocker lock(&mutex_);
                if (e == videoSink_) videoSink_ = nullptr;
                if (e == audioSink_) audioSink_ = nullptr;
            }
            gst_element_set_locked_state(e, TRUE);
            gst_element_set_state(e, GST_STATE_NULL);
            gst_bin_remove(GST_BIN(pipeline_), e);
        }
        if (hadHardwareSink) detachHardwareSink();
        route.chain.clear();
    }
}

} } // namespace nordtile::player
