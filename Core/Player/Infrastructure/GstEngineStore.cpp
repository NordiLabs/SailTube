// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// STORE (Harbour) build of GstEngine: no GStreamer.
//
// E9: the Harbour validator rejects a binary that
// links libgstreamer-1.0, libgstapp-1.0, libgstvideo-1.0,
// libgstnemointerfaces-1.0 or libaudioresource-qt. Until the reduced STORE
// player exists (Qt Multimedia, muxed or HLS only; user decision 2026-11-08),
// `CONFIG+=sailtube_store` compiles this file instead of GstEngine.cpp and
// EglSinkBridge.cpp: the engine never builds a pipeline, so VideoPlayer fails
// every load with kStoreUnavailable and the session shows it.
//
// Same header as the FULL engine (GstEngine.h), so VideoPlayer, the session
// and the QML types compile unchanged.

#include "GstEngine.h"

#include <QCoreApplication>
#include <QEvent>
#include <QMutex>
#include <QMutexLocker>

namespace nordtile { namespace player {

namespace {
const char kStoreUnavailable[] = "Video playback is only available in the full (Chum) build of SailTube.";

// NFR-PERF-01 start-up API without GStreamer: no worker thread; startInitialize() runs its
// afterInit job inline and ready(false, kStoreUnavailable, 0) is delivered once, on the
// application's thread (a queued event, as in the FULL build).
const QEvent::Type kStoreReadyEvent = static_cast<QEvent::Type>(QEvent::registerEventType());
QMutex gStoreMutex;
bool gStoreStarted = false;
}

// Complete types for the members the header declares (destroyed by ~GstEngine).
struct GstEngine::BranchCtx {};
struct GstEngine::RouteCtx {};
struct GstEngine::Ctx {};
struct GstEngine::Callbacks {};

bool GstEngine::initialize(QString *error) {
    if (error) *error = QString::fromLatin1(kStoreUnavailable);
    return false;
}

std::set<std::string> GstEngine::availableElements() { return std::set<std::string>(); }

void GstEngine::startInitialize(const std::function<void()> &afterInit) {
    GstEngineInit *notifier = GstEngineInit::instance();
    {
        QMutexLocker lock(&gStoreMutex);
        if (gStoreStarted) return;
        gStoreStarted = true;
    }
    if (afterInit) afterInit();  // e.g. the capability probe: still wanted (conservative fallback)
    QCoreApplication::postEvent(notifier, new QEvent(kStoreReadyEvent));
}

bool GstEngine::initialized() { return false; }
bool GstEngine::waitInitialized(int) { return false; }
void GstEngine::preloadDecoders() {}
std::vector<std::string> GstEngine::preloadFeatures() { return std::vector<std::string>(); }

GstEngineInit::GstEngineInit() : QObject(nullptr) {}

GstEngineInit *GstEngineInit::instance() {
    static GstEngineInit *notifier = nullptr;
    static QMutex mutex;
    QMutexLocker lock(&mutex);
    if (!notifier) {
        notifier = new GstEngineInit;
        if (QCoreApplication *app = QCoreApplication::instance())
            if (notifier->thread() != app->thread()) notifier->moveToThread(app->thread());
    }
    return notifier;
}

bool GstEngineInit::event(QEvent *e) {
    if (e->type() != kStoreReadyEvent) return QObject::event(e);
    emit ready(false, QString::fromLatin1(kStoreUnavailable), 0);
    return true;
}

GstEngine::GstEngine(VideoFrameMailbox *mailbox, QObject *parent) : QObject(parent), mailbox_(mailbox) {}

GstEngine::~GstEngine() {}

bool GstEngine::build(const PlanInput &, QString *error) {
    if (error) *error = QString::fromLatin1(kStoreUnavailable);
    return false;
}

void GstEngine::teardown() {}
bool GstEngine::setState(PipelineState) { return false; }
bool GstEngine::sendSeek(const SeekCommand &, quint32 *) { return false; }
bool GstEngine::sinkSawSegment(Branch, quint32) const { return false; }
bool GstEngine::sinkSegmentStart(Branch, quint32, qint64 *) const { return false; }
void GstEngine::holdForStartSeek() {}
bool GstEngine::sendStartSeek(const SeekCommand &, quint32 *) { return false; }
void GstEngine::releaseStartHolds() {}
bool GstEngine::queryPosition(Branch, qint64 *) const { return false; }
bool GstEngine::queryDuration(qint64 *) const { return false; }
bool GstEngine::queryBuffered(Branch, qint64, qint64, double, qint64 *) const { return false; }
bool GstEngine::querySeekable(bool *seekable) const {
    if (seekable) *seekable = false;
    return false;
}
void GstEngine::freeze(FreezeTarget) {}
void GstEngine::thaw(FreezeTarget) {}
void GstEngine::setVideoOutputEnabled(bool enabled) { outputEnabled_ = enabled; }
GstElement *GstEngine::hardwareVideoSink() const { return nullptr; }
void GstEngine::applyAudioValues(const AudioChainValues &) {}
EngineStats GstEngine::stats() const { return EngineStats(); }
void GstEngine::resetStats() {}
bool GstEngine::event(QEvent *e) { return QObject::event(e); }

} } // namespace nordtile::player
