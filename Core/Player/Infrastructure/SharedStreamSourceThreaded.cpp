// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// The shared resolver on a worker thread; see SharedStreamSourceThreaded.h.
#include "SharedStreamSourceThreaded.h"

#include <QCoreApplication>
#include <QElapsedTimer>

namespace nordtile { namespace player {

namespace threaded {

namespace {

int functionEventType() {
    static const int type = QEvent::registerEventType();
    return type;
}

class FunctionEvent final : public QEvent {
public:
    FunctionEvent(std::shared_ptr<QAtomicInt> open, std::function<void()> fn)
        : QEvent(static_cast<QEvent::Type>(functionEventType())), open_(std::move(open)), fn_(std::move(fn)) {}
    void run() { if (open_->load()) fn_(); }
private:
    std::shared_ptr<QAtomicInt> open_;
    std::function<void()> fn_;
};

playback::StreamSet emptyAnswer(const QString &videoId) {
    playback::StreamSet s;
    s.videoId = videoId;
    return s;
}

} // namespace

class Channel::Invoker final : public QObject {
public:
    bool event(QEvent *e) override {
        if (e->type() != functionEventType()) return QObject::event(e);
        static_cast<FunctionEvent *>(e)->run();
        return true;
    }
};

std::shared_ptr<Channel> Channel::here() { return on(QThread::currentThread()); }

std::shared_ptr<Channel> Channel::on(QThread *thread) {
    std::shared_ptr<Channel> c(new Channel);
    c->open_ = std::make_shared<QAtomicInt>(1);
    c->invoker_ = new Invoker;
    if (thread && thread != QThread::currentThread()) c->invoker_->moveToThread(thread);
    return c;
}

Channel::~Channel() {
    open_->store(0);
    QThread *t = invoker_->thread();
    // Pending events are deleted with the invoker. A finished thread runs no deferred deletes.
    if (t == QThread::currentThread() || !t || t->isFinished()) delete invoker_;
    else invoker_->deleteLater();
}

void Channel::post(std::function<void()> fn) {
    if (!open_->load()) return;
    QCoreApplication::postEvent(invoker_, new FunctionEvent(open_, std::move(fn)));
}

void Channel::close() { open_->store(0); }
bool Channel::isOpen() const { return open_->load() != 0; }
QThread *Channel::thread() const { return invoker_->thread(); }

// ---- QueuedResolver -----------------------------------------------------------------------------

struct QueuedResolver::Remote {
    std::map<quint64, quint64> ops; // caller op -> target op (target thread only)
};

QueuedResolver::QueuedResolver(std::shared_ptr<Channel> targetChannel, std::function<playback::StreamResolver *()> target)
    : target_(std::move(targetChannel)), self_(Channel::here()), remote_(std::make_shared<Remote>()),
      lookup_(std::move(target)), latest_(std::make_shared<QVariantMap>()) {}

QueuedResolver::~QueuedResolver() {
    self_->close(); // nothing more is delivered here
    pending_.clear();
    const std::shared_ptr<Remote> remote = remote_;
    const std::function<playback::StreamResolver *()> lookup = lookup_;
    target_->post([remote, lookup]() {
        playback::StreamResolver *t = lookup();
        const std::map<quint64, quint64> ops = remote->ops;
        remote->ops.clear();
        if (t) for (const auto &o : ops) t->cancel(o.second);
    });
}

quint64 QueuedResolver::resolve(const playback::StreamRequest &request, Completion completion) {
    return forward(request, std::move(completion), false);
}

quint64 QueuedResolver::nameFinal(const playback::StreamRequest &request, Completion completion) {
    return forward(request, std::move(completion), true);
}

quint64 QueuedResolver::forward(const playback::StreamRequest &request, Completion completion, bool nameFinal) {
    const quint64 op = next_++;
    pending_[op] = std::make_pair(request.videoId, std::move(completion));
    const std::shared_ptr<Remote> remote = remote_;
    const std::function<playback::StreamResolver *()> lookup = lookup_;
    const std::shared_ptr<Channel> self = self_;
    const std::shared_ptr<QVariantMap> latest = latest_;
    QueuedResolver *caller = this; // dereferenced only on the caller's thread while self is open
    const auto deliver = [caller, op](const playback::StreamSet &s, const playback::StreamError &e) {
        auto it = caller->pending_.find(op);
        if (it == caller->pending_.end()) return; // cancelled meanwhile: already answered
        const Completion c = it->second.second;
        caller->pending_.erase(it);
        if (c) c(s, e);
    };
    target_->post([remote, lookup, self, latest, request, op, deliver, nameFinal]() {
        playback::StreamResolver *t = lookup();
        if (!t) {
            self->post([deliver, request]() {
                deliver(emptyAnswer(request.videoId), playback::streamError(playback::StreamErrorClass::RuntimeUnavailable, false));
            });
            return;
        }
        const Completion done = [remote, lookup, self, latest, op, deliver](const playback::StreamSet &s,
                                                                            const playback::StreamError &e) {
            remote->ops.erase(op);
            playback::StreamResolver *target = lookup();
            const QVariantMap d = target ? target->diagnostics() : QVariantMap();
            self->post([latest, d, deliver, s, e]() {
                *latest = d;
                deliver(s, e);
            });
        };
        remote->ops[op] = nameFinal ? t->nameFinal(request, done) : t->resolve(request, done);
    });
    return op;
}

void QueuedResolver::prefetch(const playback::StreamRequest &request) {
    const std::function<playback::StreamResolver *()> lookup = lookup_;
    target_->post([lookup, request]() { if (playback::StreamResolver *t = lookup()) t->prefetch(request); });
}

void QueuedResolver::cancel(quint64 operation) {
    auto it = pending_.find(operation);
    if (it == pending_.end()) return;
    const QString videoId = it->second.first;
    const Completion c = it->second.second;
    pending_.erase(it);
    if (c) self_->post([c, videoId]() { c(emptyAnswer(videoId), playback::streamError(playback::StreamErrorClass::Cancelled, false)); });
    const std::shared_ptr<Remote> remote = remote_;
    const std::function<playback::StreamResolver *()> lookup = lookup_;
    target_->post([remote, lookup, operation]() {
        auto o = remote->ops.find(operation);
        if (o == remote->ops.end()) return;
        const quint64 id = o->second;
        remote->ops.erase(o);
        if (playback::StreamResolver *t = lookup()) t->cancel(id);
    });
}

void QueuedResolver::invalidate(const QString &videoId) {
    const std::function<playback::StreamResolver *()> lookup = lookup_;
    target_->post([lookup, videoId]() { if (playback::StreamResolver *t = lookup()) t->invalidate(videoId); });
}

void QueuedResolver::reportRefused(const QString &videoId, const playback::StreamFormat &format) {
    const std::function<playback::StreamResolver *()> lookup = lookup_;
    target_->post([lookup, videoId, format]() { if (playback::StreamResolver *t = lookup()) t->reportRefused(videoId, format); });
}

void QueuedResolver::reportFault(const std::string &videoId, SourceFault fault) {
    const std::function<playback::StreamResolver *()> lookup = lookup_;
    target_->post([lookup, videoId, fault]() {
        playback::StreamResolver *t = lookup();
        if (!t) return;
        if (SourceFaultSink *sink = dynamic_cast<SourceFaultSink *>(t)) sink->reportFault(videoId, fault);
        else if (fault == SourceFault::Rejected) t->reportRefused(QString::fromStdString(videoId), playback::StreamFormat());
        else t->invalidate(QString::fromStdString(videoId));
    });
}

QVariantMap QueuedResolver::diagnostics() const {
    const std::function<playback::StreamResolver *()> lookup = lookup_;
    const std::shared_ptr<Channel> self = self_;
    const std::shared_ptr<QVariantMap> latest = latest_;
    target_->post([lookup, self, latest]() {
        playback::StreamResolver *t = lookup();
        if (!t) return;
        const QVariantMap d = t->diagnostics();
        self->post([latest, d]() { *latest = d; });
    });
    return *latest_;
}

// ---- QueuedFetcher ------------------------------------------------------------------------------

struct QueuedFetcher::Remote {
    std::map<quint64, quint64> handles; // caller handle -> target handle (target thread only)
};

QueuedFetcher::QueuedFetcher(std::shared_ptr<Channel> targetChannel, std::function<playback::StreamFetcher *()> target)
    : target_(std::move(targetChannel)), self_(Channel::here()), remote_(std::make_shared<Remote>()),
      lookup_(std::move(target)), latest_(std::make_shared<QVariantMap>()) {}

QueuedFetcher::~QueuedFetcher() {
    self_->close();
    pending_.clear();
    const std::shared_ptr<Remote> remote = remote_;
    const std::function<playback::StreamFetcher *()> lookup = lookup_;
    target_->post([remote, lookup]() {
        playback::StreamFetcher *t = lookup();
        const std::map<quint64, quint64> handles = remote->handles;
        remote->handles.clear();
        if (t) for (const auto &h : handles) t->close(h.second);
    });
}

quint64 QueuedFetcher::open(const playback::StreamFormat &format, Opened opened) {
    const quint64 handle = next_++;
    pending_[handle] = std::move(opened);
    open_[handle] = true;
    const std::shared_ptr<Remote> remote = remote_;
    const std::function<playback::StreamFetcher *()> lookup = lookup_;
    const std::shared_ptr<Channel> self = self_;
    const std::shared_ptr<QVariantMap> latest = latest_;
    QueuedFetcher *caller = this; // dereferenced only on the caller's thread while self is open
    const auto deliver = [caller, handle](const QUrl &url, const playback::StreamError &e) {
        auto it = caller->pending_.find(handle);
        if (it == caller->pending_.end()) return;
        const Opened o = it->second;
        caller->pending_.erase(it);
        if (!e.ok()) caller->open_.erase(handle);
        if (o) o(url, e);
    };
    target_->post([remote, lookup, self, latest, format, handle, deliver]() {
        playback::StreamFetcher *t = lookup();
        if (!t) {
            self->post([deliver]() { deliver(QUrl(), playback::streamError(playback::StreamErrorClass::RuntimeUnavailable, false)); });
            return;
        }
        remote->handles[handle] = t->open(format, [lookup, self, latest, deliver](const QUrl &url, const playback::StreamError &e) {
            playback::StreamFetcher *target = lookup();
            const QVariantMap d = target ? target->diagnostics() : QVariantMap();
            self->post([latest, d, deliver, url, e]() {
                *latest = d;
                deliver(url, e);
            });
        });
    });
    return handle;
}

void QueuedFetcher::close(quint64 stream) {
    if (!open_.erase(stream)) return;
    auto it = pending_.find(stream);
    if (it != pending_.end()) {
        const Opened o = it->second;
        pending_.erase(it);
        if (o) self_->post([o]() { o(QUrl(), playback::streamError(playback::StreamErrorClass::Cancelled, false)); });
    }
    const std::shared_ptr<Remote> remote = remote_;
    const std::function<playback::StreamFetcher *()> lookup = lookup_;
    target_->post([remote, lookup, stream]() {
        auto h = remote->handles.find(stream);
        if (h == remote->handles.end()) return;
        const quint64 id = h->second;
        remote->handles.erase(h);
        if (playback::StreamFetcher *t = lookup()) t->close(id);
    });
}

QVariantMap QueuedFetcher::diagnostics() const {
    const std::function<playback::StreamFetcher *()> lookup = lookup_;
    const std::shared_ptr<Channel> self = self_;
    const std::shared_ptr<QVariantMap> latest = latest_;
    target_->post([lookup, self, latest]() {
        playback::StreamFetcher *t = lookup();
        if (!t) return;
        const QVariantMap d = t->diagnostics();
        self->post([latest, d]() { *latest = d; });
    });
    return *latest_;
}

} // namespace threaded

// ---- markers ------------------------------------------------------------------------------------

const char *timingPathLabel(playback::ResolvePath p) {
    switch (p) {
    case playback::ResolvePath::FastPath: return "fast";
    case playback::ResolvePath::Cache: return "cache";
    case playback::ResolvePath::Extractor: case playback::ResolvePath::ExtractorWithJavaScript: return "extractor";
    case playback::ResolvePath::Unknown: break;
    }
    return "extractor";
}

static QString marker(const char *event, qint64 atMs) {
    return QStringLiteral("sailtube-timing: %1 t=%2").arg(QLatin1String(event)).arg(atMs);
}

QString warmupStartMarker(qint64 atMs) { return marker("warmup-start", atMs); }
QString warmupDoneMarker(qint64 atMs, bool ok, qint64 visitorMs) {
    return marker("warmup-done", atMs) + QStringLiteral(" ok=%1 visitorMs=%2").arg(ok ? 1 : 0).arg(visitorMs);
}
QString resolveStartMarker(qint64 atMs, int run) { return marker("resolve-start", atMs) + QStringLiteral(" run=%1").arg(run); }
QString resolveDoneMarker(qint64 atMs, int run, playback::ResolvePath path) {
    return marker("resolve-done", atMs) + QStringLiteral(" run=%1 path=%2").arg(run).arg(QLatin1String(timingPathLabel(path)));
}
QString probeDoneMarker(qint64 atMs, int run) { return marker("probe-done", atMs) + QStringLiteral(" run=%1").arg(run); }

// ---- ThreadedStreamSource -----------------------------------------------------------------------

struct ThreadedStreamSource::Worker {
    StreamSourceBackend *backend = nullptr; // worker thread only
};

ThreadedStreamSource::ThreadedStreamSource(Factory factory, const Options &options, QObject *parent)
    : QObject(parent), options_(options), self_(threaded::Channel::here()), state_(std::make_shared<Worker>()) {
    if (!options_.monotonicMs) options_.monotonicMs = []() {
        static const QElapsedTimer origin = []() { QElapsedTimer t; t.start(); return t; }();
        return origin.elapsed() + 1; // monotonic, > 0
    };
    thread_ = new QThread;
    thread_->setObjectName(options_.threadName);
    worker_ = threaded::Channel::on(thread_);
    thread_->start();
    const std::shared_ptr<Worker> state = state_;
    worker_->post([state, factory]() { state->backend = factory ? factory() : nullptr; });
    resolver_.reset(new threaded::QueuedResolver(worker_, [state]() {
        return state->backend ? state->backend->resolver() : static_cast<playback::StreamResolver *>(nullptr);
    }));
    fetcher_.reset(new threaded::QueuedFetcher(worker_, [state]() {
        return state->backend ? state->backend->fetcher() : static_cast<playback::StreamFetcher *>(nullptr);
    }));
}

ThreadedStreamSource::~ThreadedStreamSource() {
    // Order: the forwarders post their cleanup (cancel / close) first, then the backend goes on
    // its own thread, then the thread stops; everything runs in posting order.
    resolver_.reset();
    fetcher_.reset();
    self_->close();
    const std::shared_ptr<Worker> state = state_;
    worker_->post([state]() {
        delete state->backend;
        state->backend = nullptr;
        QThread::currentThread()->quit();
    });
    thread_->wait();
    worker_.reset();
    delete thread_;
}

qint64 ThreadedStreamSource::now() const { return options_.monotonicMs(); }

void ThreadedStreamSource::warmUp() {
    const std::shared_ptr<Worker> state = state_;
    const std::shared_ptr<threaded::Channel> self = self_;
    const std::function<qint64()> clock = options_.monotonicMs;
    const StreamSourceTimingHooks hooks = options_.hooks;
    worker_->post([state, self, clock, hooks]() {
        if (!state->backend) return;
        const qint64 started = clock();
        if (hooks.warmupStart) self->post([hooks, started]() { hooks.warmupStart(started); });
        state->backend->warmUp([self, clock, hooks](bool ok, qint64 visitorMs) {
            const qint64 done = clock();
            if (hooks.warmupDone) self->post([hooks, done, ok, visitorMs]() { hooks.warmupDone(done, ok, visitorMs); });
        });
    });
}

quint64 ThreadedStreamSource::resolve(const playback::StreamRequest &request, Completion completion) {
    const qint64 started = now();
    const std::function<void(const ResolveTiming &)> hook = options_.hooks.resolveDone;
    const std::function<qint64()> clock = options_.monotonicMs;
    return resolver_->resolve(request, [hook, clock, started, completion](const playback::StreamSet &s, const playback::StreamError &e) {
        if (hook && e.kind != playback::StreamErrorClass::Cancelled) {
            ResolveTiming t;
            t.videoId = s.videoId;
            t.ok = e.ok();
            t.path = s.path;
            t.startedAtMs = started;
            t.doneAtMs = clock();
            t.totalMs = s.resolveMs;
            t.visitorMs = s.phases.visitorMs;
            t.requestMs = s.phases.requestMs;
            t.probeMs = s.phases.probeMs;
            hook(t);
        }
        if (completion) completion(s, e);
    });
}

void ThreadedStreamSource::prefetch(const playback::StreamRequest &request) { resolver_->prefetch(request); }
void ThreadedStreamSource::cancel(quint64 operation) { resolver_->cancel(operation); }
void ThreadedStreamSource::invalidate(const QString &videoId) { resolver_->invalidate(videoId); }
void ThreadedStreamSource::reportRefused(const QString &videoId, const playback::StreamFormat &format) {
    resolver_->reportRefused(videoId, format);
}
void ThreadedStreamSource::reportFault(const std::string &videoId, SourceFault fault) { resolver_->reportFault(videoId, fault); }
QVariantMap ThreadedStreamSource::diagnostics() const { return resolver_->diagnostics(); }

}}
