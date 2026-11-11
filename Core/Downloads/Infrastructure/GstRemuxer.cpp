// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "GstRemuxer.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QLibrary>
#include <QMutexLocker>
#include <QStringList>
#include <QThread>
#include <QTimer>
#include <cstdio>
#include <cstring>
#include <string>

namespace nordtile { namespace video { namespace downloads {

namespace gstremux {

// Minimal mirror of the public, stable GStreamer 1.x / GLib C ABI used here.
typedef int gboolean;
typedef unsigned long long GstClockTime;
struct GError { unsigned domain; int code; char *message; };
typedef void (*GCallback)();

const int stateNull = 1;
const int statePlaying = 4;
const int stateChangeFailure = 0;
const int messageEos = 1 << 0;
const int messageError = 1 << 1;
const int padLinkOk = 0;
const GstClockTime pollNs = 100ull * 1000 * 1000;

struct Api {
    gboolean (*initCheck)(int *, char ***, GError **);
    void (*version)(unsigned *, unsigned *, unsigned *, unsigned *);
    void *(*factoryFind)(const char *);
    void *(*featureLoad)(void *);
    void (*objectUnref)(void *);
    char *(*objectGetName)(void *);
    void *(*factoryMake)(const char *, const char *);
    void *(*pipelineNew)(const char *);
    gboolean (*binAdd)(void *, void *);
    gboolean (*elementLink)(void *, void *);
    void *(*getStaticPad)(void *, const char *);
    void *(*requestPad)(void *, const char *); // request_pad_simple (1.20+) or get_request_pad
    int (*padLink)(void *, void *);
    void *(*padGetCurrentCaps)(void *);
    unsigned (*capsGetSize)(const void *);
    void *(*capsGetStructure)(const void *, unsigned);
    const char *(*structureGetName)(const void *);
    void (*miniObjectUnref)(void *);
    int (*setState)(void *, int);
    void *(*getBus)(void *);
    void *(*busTimedPopFiltered)(void *, GstClockTime, int);
    const void *(*messageGetStructure)(void *);
    void (*messageParseError)(void *, GError **, char **);
    void (*errorFree)(GError *);
    void (*gFree)(void *);
    void (*objectSet)(void *, const char *, ...);
    unsigned long (*signalConnectData)(void *, const char *, GCallback, void *, void (*)(void *, void *), int);
    void *(*classFindProperty)(void *, const char *);

    bool hasProperty(void *object, const char *name) const {
        // GTypeInstance (first member of every GObject) starts with its class pointer.
        return classFindProperty(*static_cast<void **>(object), name) != nullptr;
    }
};

template <typename T> bool resolve(QLibrary &library, const char *symbol, T &target) {
    target = reinterpret_cast<T>(library.resolve(symbol));
    return target != nullptr;
}

bool loadApi(const QString &gstLibrary, Api &g) {
    QLibrary gst(gstLibrary, 0);
    QLibrary glib(QStringLiteral("glib-2.0"), 0);
    QLibrary gobject(QStringLiteral("gobject-2.0"), 0);
    // Never unloaded: Qt Multimedia uses the same libraries for the process lifetime.
    if (!gst.load() || !glib.load() || !gobject.load()) return false;
    std::memset(&g, 0, sizeof g);
    if (!resolve(gst, "gst_element_request_pad_simple", g.requestPad))
        resolve(gst, "gst_element_get_request_pad", g.requestPad);
    return g.requestPad
        && resolve(gst, "gst_init_check", g.initCheck) && resolve(gst, "gst_version", g.version)
        && resolve(gst, "gst_element_factory_find", g.factoryFind)
        && resolve(gst, "gst_plugin_feature_load", g.featureLoad)
        && resolve(gst, "gst_object_unref", g.objectUnref) && resolve(gst, "gst_object_get_name", g.objectGetName)
        && resolve(gst, "gst_element_factory_make", g.factoryMake) && resolve(gst, "gst_pipeline_new", g.pipelineNew)
        && resolve(gst, "gst_bin_add", g.binAdd) && resolve(gst, "gst_element_link", g.elementLink)
        && resolve(gst, "gst_element_get_static_pad", g.getStaticPad) && resolve(gst, "gst_pad_link", g.padLink)
        && resolve(gst, "gst_pad_get_current_caps", g.padGetCurrentCaps)
        && resolve(gst, "gst_caps_get_size", g.capsGetSize) && resolve(gst, "gst_caps_get_structure", g.capsGetStructure)
        && resolve(gst, "gst_structure_get_name", g.structureGetName)
        && resolve(gst, "gst_mini_object_unref", g.miniObjectUnref)
        && resolve(gst, "gst_element_set_state", g.setState) && resolve(gst, "gst_element_get_bus", g.getBus)
        && resolve(gst, "gst_bus_timed_pop_filtered", g.busTimedPopFiltered)
        && resolve(gst, "gst_message_get_structure", g.messageGetStructure)
        && resolve(gst, "gst_message_parse_error", g.messageParseError)
        && resolve(glib, "g_error_free", g.errorFree) && resolve(glib, "g_free", g.gFree)
        && resolve(gobject, "g_object_set", g.objectSet)
        && resolve(gobject, "g_signal_connect_data", g.signalConnectData)
        && resolve(gobject, "g_object_class_find_property", g.classFindProperty);
}

bool elementLoads(const Api &g, const char *name) {
    void *factory = g.factoryFind(name);
    if (!factory) return false;
    void *loaded = g.featureLoad(factory); // the registry may list a plugin that no longer loads
    if (loaded) g.objectUnref(loaded);
    g.objectUnref(factory);
    return loaded != nullptr;
}

QString unavailableReason(const MuxerAvailability &a, MuxContainer container) {
    if (!a.loaded) return QStringLiteral("GStreamer is not available");
    const bool ok = container == MuxContainer::Mp4 ? a.mp4 : container == MuxContainer::WebM ? a.webm : a.matroska;
    if (ok) return QString();
    const char *name = container == MuxContainer::Mp4 ? "MP4" : container == MuxContainer::WebM ? "WebM" : "Matroska";
    return QStringLiteral("no GStreamer %1 muxer").arg(QLatin1String(name));
}

// Picks the demuxer from the file signature, so any input/output pairing works.
const char *demuxerFor(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return nullptr;
    const QByteArray head = file.read(12);
    if (head.size() >= 4 && head.startsWith("\x1A\x45\xDF\xA3")) return "matroskademux";
    if (head.size() >= 8) {
        const QByteArray box = head.mid(4, 4);
        if (box == "ftyp" || box == "styp" || box == "moov" || box == "sidx" || box == "mdat" || box == "free"
                || box == "skip" || box == "wide")
            return "qtdemux";
    }
    return nullptr;
}

// One demuxer branch. Callbacks run on GStreamer streaming threads.
struct Branch {
    const Api *api = nullptr;
    void *queue = nullptr;
    const char *kind = "";       // "video" or "audio"
    bool mp4 = false;            // output is MP4
    std::string refusedCodec;    // written before `refused` is set
    std::atomic<bool> linked;
    std::atomic<bool> linkFailed;
    std::atomic<bool> refused;
    std::atomic<bool> allPads;
    bool used = true;            // false: this input is absent (one-track remux)
    Branch() : linked(false), linkFailed(false), refused(false), allPads(false) {}
};

// The pad's media type ("video/x-h264"), empty when it has no caps yet.
std::string padMedia(const Api &g, void *pad) {
    std::string media;
    if (void *caps = g.padGetCurrentCaps(pad)) {
        if (g.capsGetSize(caps) > 0) {
            const char *name = g.structureGetName(g.capsGetStructure(caps, 0));
            if (name) media = name;
        }
        g.miniObjectUnref(caps);
    }
    return media;
}

// Both demuxers name their pads video_N / audio_N / subtitle_N; caps are the
// fallback for any other naming.
bool padIsKind(const Api &g, void *pad, const std::string &media, const char *kind) {
    const size_t n = std::strlen(kind);
    char *name = g.objectGetName(pad);
    const bool named = name && std::strncmp(name, kind, n) == 0 && name[n] == '_';
    g.gFree(name);
    return named || (media.compare(0, n, kind) == 0 && media.size() > n && media[n] == '/');
}

// VP8/VP9 never go into MP4 (DL-03 puts them in WebM/Matroska). mp4mux of
// GStreamer 1.26.11 also crashed the process (in gst_video_chroma_site_from_string)
// on 4:2:0 VP9 from qtdemux, so they must not reach it at all (E6).
bool refusedInMp4(const std::string &media) {
    return media == "video/x-vp8" || media == "video/x-vp9";
}

void onPadAdded(void *, void *pad, void *data) {
    Branch *b = static_cast<Branch *>(data);
    const Api &g = *b->api;
    if (b->linked.load() || b->refused.load()) return;
    const std::string media = padMedia(g, pad);
    // Extra streams (e.g. audio inside the video file) stay unlinked; demuxers
    // combine flow returns, so one linked stream keeps them running.
    if (!padIsKind(g, pad, media, b->kind)) return;
    if (b->mp4 && refusedInMp4(media)) {
        b->refusedCodec = media;
        b->refused = true;
        return;
    }
    void *sink = g.getStaticPad(b->queue, "sink");
    if (sink && g.padLink(pad, sink) == padLinkOk) b->linked = true;
    else b->linkFailed = true;
    if (sink) g.objectUnref(sink);
}

void onNoMorePads(void *, void *data) { static_cast<Branch *>(data)->allPads = true; }

class Worker : public QThread {
public:
    Worker(GstRemuxer *owner, quint64 id, const QString &video, const QString &audio, const QString &output,
           MuxContainer container, const Remuxer::Completion &completion)
        : id(id), completion(completion), cancelled(false), m_owner(owner), m_video(video), m_audio(audio),
          m_output(output), m_container(container) {}

    const quint64 id;
    const Remuxer::Completion completion;
    std::atomic<bool> cancelled;
    bool ok = false; // written by run(), read after finished()
    QString error;

protected:
    void run() override {
        const QString part = m_output + QStringLiteral(".part");
        const QString faststart = m_output + QStringLiteral(".faststart.part");
        error = execute(part, faststart);
        if (error.isEmpty() && cancelled.load()) error = QStringLiteral("cancelled");
        if (error.isEmpty() && QFileInfo(part).size() <= 0) error = QStringLiteral("muxer produced no data");
        QFile::remove(faststart);
        if (error.isEmpty()) {
            // POSIX rename replaces an older file atomically.
            if (std::rename(QFile::encodeName(part).constData(), QFile::encodeName(m_output).constData()) != 0)
                error = QStringLiteral("cannot rename %1").arg(part);
        }
        if (!error.isEmpty()) QFile::remove(part);
        ok = error.isEmpty();
    }

private:
    QString execute(const QString &part, const QString &faststart) {
        if (cancelled.load()) return QStringLiteral("cancelled");
        const MuxerAvailability available = m_owner->availability(); // probes on first use, off the GUI thread
        const QString reason = unavailableReason(available, m_container);
        if (!reason.isEmpty()) return reason;
        std::shared_ptr<Api> api;
        QString muxer;
        {
            QMutexLocker lock(&m_owner->m_mutex);
            api = m_owner->m_api;
            muxer = m_container == MuxContainer::Mp4 ? m_owner->m_mp4Muxer
                  : m_container == MuxContainer::WebM ? QStringLiteral("webmmux") : QStringLiteral("matroskamux");
        }
        const bool hasVideo = !m_video.isEmpty(), hasAudio = !m_audio.isEmpty();
        const char *videoDemux = hasVideo ? demuxerFor(m_video) : "";
        const char *audioDemux = hasAudio ? demuxerFor(m_audio) : "";
        if (!videoDemux) return QStringLiteral("unrecognised video container: %1").arg(m_video);
        if (!audioDemux) return QStringLiteral("unrecognised audio container: %1").arg(m_audio);
        const Api &g = *api;

        void *pipeline = g.pipelineNew("sailtube-remux");
        if (!pipeline) return QStringLiteral("cannot create a GStreamer pipeline");
        QString failure;
        // Every element is added to the pipeline at once, so unreffing the
        // pipeline releases all of them on any exit path.
        const auto make = [&](const char *factory) -> void * {
            void *e = failure.isEmpty() ? g.factoryMake(factory, nullptr) : nullptr;
            if (e) g.binAdd(pipeline, e);
            else if (failure.isEmpty()) failure = QStringLiteral("GStreamer element %1 is missing").arg(QLatin1String(factory));
            return e;
        };
        void *videoSrc = nullptr, *videoDemuxer = nullptr, *videoQueue = nullptr;
        void *audioSrc = nullptr, *audioDemuxer = nullptr, *audioQueue = nullptr;
        if (hasVideo) { videoSrc = make("filesrc"); videoDemuxer = make(videoDemux); videoQueue = make("queue"); }
        if (hasAudio) { audioSrc = make("filesrc"); audioDemuxer = make(audioDemux); audioQueue = make("queue"); }
        void *mux = make(muxer.toLatin1().constData()), *sink = make("filesink");
        Branch video, audio;
        video.used = hasVideo;
        audio.used = hasAudio;
        if (failure.isEmpty()) failure = build(g, pipeline, videoSrc, videoDemuxer, videoQueue, audioSrc, audioDemuxer,
                                               audioQueue, mux, sink, part, faststart, video, audio);
        if (failure.isEmpty()) failure = awaitBus(g, pipeline, video, audio);
        g.setState(pipeline, stateNull); // stops the streaming threads that use `video`/`audio`
        g.objectUnref(pipeline);
        return failure;
    }

    QString build(const Api &g, void *pipeline, void *videoSrc, void *videoDemuxer, void *videoQueue, void *audioSrc,
                  void *audioDemuxer, void *audioQueue, void *mux, void *sink, const QString &part,
                  const QString &faststart, Branch &video, Branch &audio) {
        Q_UNUSED(pipeline);
        // Paths go to `location` properties, never through a pipeline parser.
        if (video.used) g.objectSet(videoSrc, "location", QFile::encodeName(m_video).constData(), static_cast<void *>(nullptr));
        if (audio.used) g.objectSet(audioSrc, "location", QFile::encodeName(m_audio).constData(), static_cast<void *>(nullptr));
        g.objectSet(sink, "location", QFile::encodeName(part).constData(), static_cast<void *>(nullptr));
        if (g.hasProperty(mux, "faststart")) {
            // moov first, so the gallery can stream it. The muxer spools mdat to
            // a temporary file, by default in g_get_tmp_dir(); keep it beside the
            // output instead (PLT-09: /tmp is RAM).
            if (g.hasProperty(mux, "faststart-file"))
                g.objectSet(mux, "faststart-file", QFile::encodeName(faststart).constData(), static_cast<void *>(nullptr));
            g.objectSet(mux, "faststart", gboolean(1), static_cast<void *>(nullptr));
        }
        if ((video.used && !g.elementLink(videoSrc, videoDemuxer)) || (audio.used && !g.elementLink(audioSrc, audioDemuxer))
            || !g.elementLink(mux, sink))
            return QStringLiteral("cannot link the remux pipeline");
        // Muxer pads are requested up front: an MP4 muxer refuses new pads once
        // it has started, and the demuxers expose theirs only while running.
        // No parsers: demuxed H.264 (avc), AAC, VP8/9 and Opus are what the muxers accept.
        const auto attach = [&](void *queue, const char *templ) -> bool {
            void *muxPad = g.requestPad(mux, templ);
            void *queuePad = g.getStaticPad(queue, "src");
            const bool linked = muxPad && queuePad && g.padLink(queuePad, muxPad) == padLinkOk;
            if (queuePad) g.objectUnref(queuePad);
            if (muxPad) g.objectUnref(muxPad); // the muxer keeps its own reference
            return linked;
        };
        if ((video.used && !attach(videoQueue, "video_%u")) || (audio.used && !attach(audioQueue, "audio_%u")))
            return QStringLiteral("the muxer refused a video or audio pad");
        video.api = &g; video.queue = videoQueue; video.kind = "video";
        audio.api = &g; audio.queue = audioQueue; audio.kind = "audio";
        video.mp4 = audio.mp4 = m_container == MuxContainer::Mp4;
        if (video.used) {
            g.signalConnectData(videoDemuxer, "pad-added", reinterpret_cast<GCallback>(&onPadAdded), &video, nullptr, 0);
            g.signalConnectData(videoDemuxer, "no-more-pads", reinterpret_cast<GCallback>(&onNoMorePads), &video, nullptr, 0);
        }
        if (audio.used) {
            g.signalConnectData(audioDemuxer, "pad-added", reinterpret_cast<GCallback>(&onPadAdded), &audio, nullptr, 0);
            g.signalConnectData(audioDemuxer, "no-more-pads", reinterpret_cast<GCallback>(&onNoMorePads), &audio, nullptr, 0);
        }
        return QString();
    }

    QString awaitBus(const Api &g, void *pipeline, const Branch &video, const Branch &audio) {
        void *bus = g.getBus(pipeline);
        if (!bus) return QStringLiteral("pipeline has no bus");
        QString failure;
        if (g.setState(pipeline, statePlaying) == stateChangeFailure) failure = QStringLiteral("cannot start the pipeline");
        const qint64 limit = GstRemuxer::timeoutMsFor((video.used ? QFileInfo(m_video).size() : 0)
                                                      + (audio.used ? QFileInfo(m_audio).size() : 0));
        QElapsedTimer clock;
        clock.start();
        // On a start failure the bus usually holds the reason; read it at once.
        GstClockTime timeout = failure.isEmpty() ? pollNs : 0;
        for (;;) {
            void *message = g.busTimedPopFiltered(bus, timeout, messageEos | messageError);
            if (message) {
                const void *s = g.messageGetStructure(message);
                const char *name = s ? g.structureGetName(s) : nullptr;
                if (name && std::strcmp(name, "GstMessageError") == 0) {
                    GError *err = nullptr;
                    char *debug = nullptr;
                    g.messageParseError(message, &err, &debug);
                    failure = QStringLiteral("GStreamer: %1").arg(err && err->message ? QString::fromUtf8(err->message)
                                                                                      : QStringLiteral("error"));
                    if (debug) failure += QStringLiteral(" (%1)").arg(QString::fromUtf8(debug).left(300));
                    if (err) g.errorFree(err);
                    g.gFree(debug);
                }
                g.miniObjectUnref(message);
                break; // EOS: the muxer has written its index and the sink is flushed
            }
            if (!failure.isEmpty()) break;
            if (cancelled.load()) { failure = QStringLiteral("cancelled"); break; }
            if (video.linkFailed.load() || audio.linkFailed.load()) { failure = QStringLiteral("cannot link a demuxed stream"); break; }
            if (video.refused.load() || audio.refused.load()) break;
            if (video.used && video.allPads.load() && !video.linked.load()) { failure = QStringLiteral("no video stream in %1").arg(m_video); break; }
            if (audio.used && audio.allPads.load() && !audio.linked.load()) { failure = QStringLiteral("no audio stream in %1").arg(m_audio); break; }
            if (clock.elapsed() > limit) { failure = QStringLiteral("remux timed out after %1 s").arg(limit / 1000); break; }
        }
        // Checked last: the unlinked stream may already have posted a generic error.
        for (const Branch *b : { &video, &audio })
            if (b->refused.load())
                failure = QStringLiteral("%1 is not muxed into MP4; use WebM or Matroska")
                              .arg(QString::fromStdString(b->refusedCodec));
        g.objectUnref(bus);
        return failure;
    }

    GstRemuxer *const m_owner; // outlives the worker: ~GstRemuxer waits for it
    const QString m_video, m_audio, m_output;
    const MuxContainer m_container;
};

} // namespace gstremux

using gstremux::Worker;

GstRemuxer::GstRemuxer(QObject *parent) : GstRemuxer(QStringLiteral("gstreamer-1.0"), parent) {}

GstRemuxer::GstRemuxer(const QString &gstreamerLibrary, QObject *parent)
    : QObject(parent), m_library(gstreamerLibrary), m_nextId(1) {}

GstRemuxer::~GstRemuxer() {
    QList<Worker *> jobs;
    {
        QMutexLocker lock(&m_mutex);
        jobs = m_jobs.values();
        m_jobs.clear();
    }
    for (Worker *w : jobs) w->cancelled = true;
    for (Worker *w : jobs) {
        w->wait();
        delete w; // drops its queued completion
    }
}

qint64 GstRemuxer::timeoutMsFor(qint64 inputBytes) {
    const qint64 hundredMiB = 100ll * 1024 * 1024;
    return 60000 + (inputBytes > 0 ? inputBytes : 0) * 10000 / hundredMiB;
}

MuxerAvailability GstRemuxer::availability() {
    QMutexLocker lock(&m_mutex);
    if (!m_probed) probeLocked();
    return m_availability;
}

void GstRemuxer::probeLocked() {
    m_probed = true;
    std::shared_ptr<gstremux::Api> api(new gstremux::Api);
    if (!gstremux::loadApi(m_library, *api)) {
        m_availability.detail = QStringLiteral("GStreamer library %1 not found").arg(m_library);
        return;
    }
    if (!api->initCheck(nullptr, nullptr, nullptr)) {
        m_availability.detail = QStringLiteral("GStreamer failed to initialise");
        return;
    }
    unsigned major = 0, minor = 0, micro = 0, nano = 0;
    api->version(&major, &minor, &micro, &nano);
    QStringList found;
    const char *const names[] = { "mp4mux", "qtmux", "qtdemux", "webmmux", "matroskamux", "matroskademux" };
    for (const char *name : names)
        if (gstremux::elementLoads(*api, name)) found << QString::fromLatin1(name);
    const bool qtdemux = found.contains(QStringLiteral("qtdemux"));
    const bool mkvdemux = found.contains(QStringLiteral("matroskademux"));
    m_mp4Muxer = found.contains(QStringLiteral("mp4mux")) ? QStringLiteral("mp4mux")
               : found.contains(QStringLiteral("qtmux")) ? QStringLiteral("qtmux") : QString();
    m_availability.loaded = true;
    m_availability.mp4 = !m_mp4Muxer.isEmpty() && qtdemux;
    m_availability.webm = found.contains(QStringLiteral("webmmux")) && mkvdemux;
    m_availability.matroska = found.contains(QStringLiteral("matroskamux")) && mkvdemux;
    m_availability.detail = QStringLiteral("GStreamer %1.%2.%3: %4").arg(major).arg(minor).arg(micro)
        .arg(found.isEmpty() ? QStringLiteral("no muxers") : found.join(QLatin1Char(' ')));
    m_api = api;
}

void GstRemuxer::failLater(const Completion &completion, const QString &error) {
    // Created on the calling thread, so the completion is delivered there.
    QObject *context = new QObject(thread() == QThread::currentThread() ? this : nullptr);
    QTimer::singleShot(0, context, [context, completion, error]() {
        context->deleteLater();
        if (completion) completion(false, error);
    });
}

quint64 GstRemuxer::remux(const QString &videoPath, const QString &audioPath, const QString &outputPath,
                          MuxContainer container, Completion completion) {
    const quint64 id = m_nextId++;
    QString reason;
    if (videoPath.isEmpty() && audioPath.isEmpty()) reason = QStringLiteral("no input");
    else if (!videoPath.isEmpty() && !QFileInfo(videoPath).isFile()) reason = QStringLiteral("video input not found: %1").arg(videoPath);
    else if (!audioPath.isEmpty() && !QFileInfo(audioPath).isFile()) reason = QStringLiteral("audio input not found: %1").arg(audioPath);
    else if (!QFileInfo(outputPath).absoluteDir().exists()) reason = QStringLiteral("output folder missing: %1").arg(outputPath);
    if (reason.isEmpty()) {
        QMutexLocker lock(&m_mutex);
        // Not probed yet: the worker probes, so gst_init never runs on this thread.
        if (m_probed) reason = gstremux::unavailableReason(m_availability, container);
    }
    if (!reason.isEmpty()) {
        failLater(completion, reason);
        return id;
    }
    Worker *worker = new Worker(this, id, videoPath, audioPath, outputPath, container, completion);
    // `worker` lives on the calling thread, so this runs there once run() returns.
    connect(worker, &QThread::finished, worker, [this, worker]() {
        {
            QMutexLocker lock(&m_mutex);
            m_jobs.remove(worker->id);
        }
        worker->wait();
        const Completion done = worker->completion;
        const bool ok = worker->ok;
        const QString error = worker->error;
        worker->deleteLater();
        if (done) done(ok, error);
    }, Qt::QueuedConnection);
    {
        QMutexLocker lock(&m_mutex);
        m_jobs.insert(id, worker);
    }
    worker->start();
    return id;
}

void GstRemuxer::cancel(quint64 id) {
    QMutexLocker lock(&m_mutex);
    if (Worker *w = m_jobs.value(id, nullptr)) w->cancelled = true;
}

}}}
