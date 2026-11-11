// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

// The shared resolver off the UI thread (RES-01, RES-02, RES-21, NFR-PERF-02): ThreadedStreamSource
// owns a worker QThread on which a StreamSourceBackend (SailTune's FastStreamResolver +
// MediaProxy + SharedStreamSource, see SharedStreamSourceSailTune.h) is created, used and
// destroyed, and exposes the same playback::StreamResolver / StreamFetcher API on the caller's
// (GUI) thread. Qt Core only; no SailTube application types (namespace nordtile::player).
//
// Contract kept across the hop: calls return at once (ids allocated on the caller's thread),
// completions run later on the caller's thread, exactly once per operation (cancellation
// included, close-before-delivery included), never after the wrapper is destroyed. Calls are
// forwarded in order with posted events (no blocking in either direction, no Qt 5.10 API).
// diagnostics() returns the latest snapshot sent by the worker (refreshed after every completion
// and on each diagnostics() call), so it may lag by one event.
//
// Typed faults (player::SourceFaultSink, Core/Player/Domain/ResolveCache.h) cross the hop like
// invalidate(): reportFault(videoId, fault) reaches the target's own reportFault when the target
// is a sink; otherwise Rejected becomes reportRefused and every other fault invalidate.
//
// Timing (tools/experiments/perf_timing.md): StreamSourceTimingHooks run on the caller's thread
// with times from Options::monotonicMs; the *Marker() helpers format the marker lines.

#include "../../../Shared/Playback/StreamSource.h"
#include "../Domain/ResolveCache.h"
#include <QEvent>
#include <QAtomicInt>
#include <QObject>
#include <QString>
#include <QThread>
#include <QUrl>
#include <QVariantMap>
#include <functional>
#include <map>
#include <memory>

namespace nordtile { namespace player {

namespace threaded {

// Runs functions on the thread of an invoker object (posted events, in order). Thread-safe
// post(); close() (on the owning thread) drops everything not yet run.
class Channel {
public:
    Channel(const Channel &) = delete;
    Channel &operator=(const Channel &) = delete;
    // A channel whose functions run on the calling thread.
    static std::shared_ptr<Channel> here();
    // A channel whose functions run on `thread` (its event loop must run).
    static std::shared_ptr<Channel> on(QThread *thread);
    ~Channel();
    void post(std::function<void()> fn);
    void close();
    bool isOpen() const;
    QThread *thread() const;

private:
    Channel() {}
    class Invoker;
    Invoker *invoker_ = nullptr;
    std::shared_ptr<QAtomicInt> open_;
};

// A StreamResolver on the caller's thread forwarding to `target`, which lives on the thread of
// `targetChannel` and is looked up there (it may be created after this object). Used both ways:
// GUI -> worker (ThreadedStreamSource) and worker -> GUI (the yt-dlp fallback, which lives with
// its QProcess runner on the GUI thread).
class QueuedResolver final : public playback::StreamResolver, public SourceFaultSink {
public:
    QueuedResolver(std::shared_ptr<Channel> targetChannel, std::function<playback::StreamResolver *()> target);
    ~QueuedResolver() override; // no completion afterwards; outstanding target work is cancelled
    quint64 resolve(const playback::StreamRequest &, Completion) override;
    void prefetch(const playback::StreamRequest &) override;
    void cancel(quint64 operation) override;
    void invalidate(const QString &videoId) override;
    void reportRefused(const QString &videoId, const playback::StreamFormat &) override;
    void reportFault(const std::string &videoId, SourceFault fault) override; // forwarded as it is (strike rules)
    quint64 nameFinal(const playback::StreamRequest &, Completion) override; // forwarded as it is (RES-09)
    QVariantMap diagnostics() const override;

private:
    struct Remote;
    quint64 forward(const playback::StreamRequest &, Completion, bool nameFinal);
    std::shared_ptr<Channel> target_, self_;
    std::shared_ptr<Remote> remote_;              // touched on the target thread only
    std::function<playback::StreamResolver *()> lookup_;
    std::map<quint64, std::pair<QString, Completion>> pending_; // video id, completion
    quint64 next_ = 1;
    std::shared_ptr<QVariantMap> latest_;         // written on the caller's thread only
};

// The same for a StreamFetcher.
class QueuedFetcher final : public playback::StreamFetcher {
public:
    QueuedFetcher(std::shared_ptr<Channel> targetChannel, std::function<playback::StreamFetcher *()> target);
    ~QueuedFetcher() override;
    quint64 open(const playback::StreamFormat &, Opened) override;
    void close(quint64 stream) override;
    QVariantMap diagnostics() const override;

private:
    struct Remote;
    std::shared_ptr<Channel> target_, self_;
    std::shared_ptr<Remote> remote_;
    std::function<playback::StreamFetcher *()> lookup_;
    std::map<quint64, Opened> pending_;           // not delivered yet
    std::map<quint64, bool> open_;                // every handle until close()
    quint64 next_ = 1;
    std::shared_ptr<QVariantMap> latest_;
};

} // namespace threaded

// What lives on the worker thread: created by the factory there, deleted there.
class StreamSourceBackend {
public:
    virtual ~StreamSourceBackend() {}
    virtual playback::StreamResolver *resolver() = 0;
    virtual playback::StreamFetcher *fetcher() = 0;
    // RES-05 / RES-21: mint the visitor id and open the connection; `done` once, on this thread.
    virtual void warmUp(std::function<void(bool ok, qint64 visitorMs)> done) = 0;
};

// One resolve as seen by the caller: times on Options::monotonicMs; phases from the component's
// StreamTimings (-1 not measured).
struct ResolveTiming {
    QString videoId;
    bool ok = false;
    playback::ResolvePath path = playback::ResolvePath::Unknown;
    qint64 startedAtMs = 0;   // resolve() called
    qint64 doneAtMs = 0;      // completion delivered
    qint64 totalMs = -1;      // the component's own total
    qint64 visitorMs = -1;
    qint64 requestMs = -1;    // /player round trip or extractor run
    qint64 probeMs = -1;      // RES-06 probe join; the probe is the last phase, so probe-done is at doneAtMs
};

struct StreamSourceTimingHooks {
    std::function<void(qint64 atMs)> warmupStart;                              // warmup-start
    std::function<void(qint64 atMs, bool ok, qint64 visitorMs)> warmupDone;    // warmup-done
    std::function<void(const ResolveTiming &)> resolveDone;                    // resolve-done (+ probe-done)
};

// Marker lines (perf_timing.md grammar). `run` is the app's per-tap counter.
const char *timingPathLabel(playback::ResolvePath);   // fast | extractor | cache
QString warmupStartMarker(qint64 atMs);
QString warmupDoneMarker(qint64 atMs, bool ok, qint64 visitorMs);
QString resolveStartMarker(qint64 atMs, int run);
QString resolveDoneMarker(qint64 atMs, int run, playback::ResolvePath);
QString probeDoneMarker(qint64 atMs, int run);

struct ThreadedStreamSourceOptions {
    std::function<qint64()> monotonicMs;  // thread-safe; default: a process-wide QElapsedTimer
    StreamSourceTimingHooks hooks;
    QString threadName = QStringLiteral("sailtube-resolver");
};

class ThreadedStreamSource final : public QObject, public playback::StreamResolver, public SourceFaultSink {
public:
    typedef std::function<StreamSourceBackend *()> Factory; // runs on the worker thread
    typedef ThreadedStreamSourceOptions Options;

    explicit ThreadedStreamSource(Factory factory, const Options &options = Options(), QObject *parent = nullptr);
    // Clean shutdown: no completion afterwards; the backend is deleted on the worker thread,
    // then the thread is stopped and joined.
    ~ThreadedStreamSource() override;

    // RES-05 / RES-21 off the UI thread: warmup-start / warmup-done hooks on the caller's thread.
    void warmUp();

    quint64 resolve(const playback::StreamRequest &, Completion) override;
    void prefetch(const playback::StreamRequest &) override;
    void cancel(quint64 operation) override;
    void invalidate(const QString &videoId) override;
    void reportRefused(const QString &videoId, const playback::StreamFormat &) override;
    // What the consumer observed when a delivered source stopped working (SharedStreamSource.h,
    // strike rules): Connection, Stalled and Abandoned are never a strike and keep the fast path;
    // Unspecified is invalidate(); Rejected is reportRefused without the format.
    void reportFault(const std::string &videoId, SourceFault fault) override;
    QVariantMap diagnostics() const override;

    playback::StreamFetcher *fetcher() { return fetcher_.get(); }
    QThread *workerThread() const { return thread_; }

private:
    struct Worker;
    qint64 now() const;
    Options options_;
    QThread *thread_ = nullptr;
    std::shared_ptr<threaded::Channel> worker_, self_;
    std::shared_ptr<Worker> state_;               // touched on the worker thread only
    std::unique_ptr<threaded::QueuedResolver> resolver_;
    std::unique_ptr<threaded::QueuedFetcher> fetcher_;
};

}}
