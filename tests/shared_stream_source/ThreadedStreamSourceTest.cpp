// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Qt Core checks of ThreadedStreamSource (SharedStreamSourceThreaded.h): calls from the main
// thread run on the worker, completions come back on the main thread, never synchronous, exactly
// once (cancel and close included), none after destruction (also with a resolve in flight), the
// backend is created and deleted on the worker; warm-up and resolve timing hooks and markers; the
// queued hop the other way (worker -> main, the yt-dlp fallback's case); typed faults across both
// hops (player::SourceFaultSink: a stall is never a refusal); and the real SailTuneBackend against
// SailTune's FakeProvider. Build: tests/shared_stream_source/run.sh
#include <cstdio>
static int failures = 0, checks = 0;
#define CHECK(c) do { ++checks; if (!(c)) { ++failures; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

#include "Core/Player/Infrastructure/SharedStreamSourceSailTune.h"
#include "Shared/SailTune/tests/sharedplayback/FakeProvider.h" // read-only SailTune test helper (pinned)

#include <QAtomicInt>
#include <QCoreApplication>
#include <QHostAddress>

using namespace nordtile;
using playback::StreamError;
using playback::StreamErrorClass;
using playback::StreamRequest;
using playback::StreamSet;
using player::ThreadedStreamSource;

static const char *const vid = "AAAAAAAAAAA";

// What the backend saw, written on the worker thread and read on the main thread after waits.
struct Seen {
    QAtomicPointer<QThread> resolveThread, fetchThread, createdThread, deletedThread;
    QAtomicInt resolves, cancels, warmUps, deleted, finalsNamed;
    QAtomicInt invalidates, refusals;                    // a target that is no SourceFaultSink
    QAtomicPointer<QThread> faultThread;
};

class FakeResolver final : public QObject, public playback::StreamResolver {
public:
    explicit FakeResolver(Seen *seen, int delayMs = 40) : seen_(seen), delayMs_(delayMs) {}
    quint64 resolve(const StreamRequest &request, Completion completion) override {
        seen_->resolveThread.store(QThread::currentThread());
        seen_->resolves.ref();
        const quint64 op = next_++;
        pending_[op] = completion;
        StreamSet s;
        s.videoId = request.videoId;
        s.path = playback::ResolvePath::FastPath;
        s.resolveMs = 120;
        s.phases.visitorMs = 30;
        s.phases.requestMs = 80;
        s.phases.probeMs = 10;
        playback::StreamFormat a;
        a.formatId = QStringLiteral("251");
        s.audio << a;
        QTimer::singleShot(delayMs_, this, [this, op, s]() {
            auto it = pending_.find(op);
            if (it == pending_.end()) return;
            const Completion c = it->second;
            pending_.erase(it);
            ++completed_;
            c(s, StreamError());
        });
        return op;
    }
    // RES-09: the single-pass question; answered like a resolve here, counted apart.
    quint64 nameFinal(const StreamRequest &request, Completion completion) override {
        seen_->finalsNamed.ref();
        return resolve(request, std::move(completion));
    }
    void prefetch(const StreamRequest &) override {}
    void cancel(quint64 op) override {
        seen_->cancels.ref();
        auto it = pending_.find(op);
        if (it == pending_.end()) return;
        const Completion c = it->second;
        pending_.erase(it);
        QTimer::singleShot(0, this, [c]() { c(StreamSet(), playback::streamError(StreamErrorClass::Cancelled, false)); });
    }
    void invalidate(const QString &) override { seen_->faultThread.store(QThread::currentThread()); seen_->invalidates.ref(); }
    void reportRefused(const QString &, const playback::StreamFormat &) override {
        seen_->faultThread.store(QThread::currentThread());
        seen_->refusals.ref();
    }
    QVariantMap diagnostics() const override {
        QVariantMap d;
        d.insert(QStringLiteral("completed"), completed_);
        return d;
    }
private:
    Seen *seen_;
    int delayMs_;
    quint64 next_ = 1;
    qint64 completed_ = 0;
    std::map<quint64, Completion> pending_;
};

// A target that takes typed faults (the adapter and the yt-dlp resolver do).
class TypedResolver final : public QObject, public playback::StreamResolver, public player::SourceFaultSink {
public:
    QAtomicInt stalled, connection, abandoned, rejected, unspecified, invalidates, refusals;
    QAtomicPointer<QThread> thread;
    quint64 resolve(const StreamRequest &, Completion) override { return 0; }
    void prefetch(const StreamRequest &) override {}
    void cancel(quint64) override {}
    void invalidate(const QString &) override { invalidates.ref(); }
    void reportRefused(const QString &, const playback::StreamFormat &) override { refusals.ref(); }
    QVariantMap diagnostics() const override { return QVariantMap(); }
    void reportFault(const std::string &videoId, player::SourceFault fault) override {
        if (videoId != vid) return;
        thread.store(QThread::currentThread());
        switch (fault) {
        case player::SourceFault::Stalled: stalled.ref(); break;
        case player::SourceFault::Connection: connection.ref(); break;
        case player::SourceFault::Abandoned: abandoned.ref(); break;
        case player::SourceFault::Rejected: rejected.ref(); break;
        case player::SourceFault::Unspecified: unspecified.ref(); break;
        }
    }
};

class FakeFetcher final : public QObject, public playback::StreamFetcher {
public:
    explicit FakeFetcher(Seen *seen) : seen_(seen) {}
    quint64 open(const playback::StreamFormat &, Opened opened) override {
        seen_->fetchThread.store(QThread::currentThread());
        const quint64 h = next_++;
        QTimer::singleShot(20, this, [opened]() { opened(QUrl(QStringLiteral("http://127.0.0.1:1/s/token")), StreamError()); });
        return h;
    }
    void close(quint64) override {}
    QVariantMap diagnostics() const override { return QVariantMap(); }
private:
    Seen *seen_;
    quint64 next_ = 1;
};

class FakeBackend final : public player::StreamSourceBackend {
public:
    explicit FakeBackend(Seen *seen) : seen_(seen), resolver_(seen), fetcher_(seen) {
        seen_->createdThread.store(QThread::currentThread());
    }
    ~FakeBackend() override {
        seen_->deletedThread.store(QThread::currentThread());
        seen_->deleted.ref();
    }
    playback::StreamResolver *resolver() override { return &resolver_; }
    playback::StreamFetcher *fetcher() override { return &fetcher_; }
    void warmUp(std::function<void(bool, qint64)> done) override {
        seen_->warmUps.ref();
        QTimer::singleShot(30, &resolver_, [done]() { done(true, 25); });
    }
private:
    Seen *seen_;
    FakeResolver resolver_;
    FakeFetcher fetcher_;
};

static StreamRequest request() {
    StreamRequest r;
    r.videoId = QString::fromLatin1(vid);
    return r;
}

struct Answer { StreamSet streams; StreamError error; QThread *thread; };
typedef std::vector<Answer> Answers;
static playback::StreamResolver::Completion into(Answers *list) {
    return [list](const StreamSet &s, const StreamError &e) { Answer a; a.streams = s; a.error = e; a.thread = QThread::currentThread(); list->push_back(a); };
}
static void settle(int ms) { waitFor([]() { return false; }, ms); }

static void ply_completionOnCallerThread() {
    Seen seen;
    std::vector<player::ResolveTiming> timings;
    ThreadedStreamSource::Options options;
    options.monotonicMs = []() { return qint64(1000); };
    options.hooks.resolveDone = [&timings](const player::ResolveTiming &t) { timings.push_back(t); };
    QThread *main = QThread::currentThread();
    {
        ThreadedStreamSource source([&seen]() { return new FakeBackend(&seen); }, options);
        Answers got;
        const quint64 op = source.resolve(request(), into(&got));
        CHECK(op != 0 && got.empty()); // never synchronous
        CHECK(waitFor([&]() { return !got.empty(); }));
        settle(100);
        CHECK(got.size() == 1 && got[0].thread == main && got[0].error.ok());
        CHECK(!got.empty() && got[0].streams.videoId == QLatin1String(vid) && got[0].streams.audio.size() == 1);
        CHECK(seen.resolveThread.load() == source.workerThread() && seen.resolveThread.load() != main);
        CHECK(seen.createdThread.load() == source.workerThread());
        // Timing hook on the caller's thread with the component's phases.
        CHECK(timings.size() == 1);
        if (!timings.empty()) {
            const player::ResolveTiming &t = timings[0];
            CHECK(t.ok && t.path == playback::ResolvePath::FastPath && t.totalMs == 120 && t.visitorMs == 30
                  && t.requestMs == 80 && t.probeMs == 10 && t.startedAtMs == 1000 && t.doneAtMs == 1000);
        }
        // Diagnostics: the latest snapshot from the worker.
        CHECK(source.diagnostics().value(QStringLiteral("completed")).toLongLong() == 1);

        // Cancel: exactly one Cancelled completion on the main thread; the worker cancels too.
        Answers cancelled;
        const quint64 c = source.resolve(request(), into(&cancelled));
        source.cancel(c);
        source.cancel(c);
        CHECK(cancelled.empty());
        settle(200);
        CHECK(cancelled.size() == 1 && cancelled[0].error.kind == StreamErrorClass::Cancelled && cancelled[0].thread == main);
        CHECK(!cancelled.empty() && cancelled[0].streams.videoId == QLatin1String(vid));
        CHECK(seen.cancels.load() == 1);
        CHECK(timings.size() == 1); // no timing for a cancelled resolve

        // Fetcher: open on the worker, opened on the main thread; close before delivery -> Cancelled once.
        std::vector<std::pair<QUrl, QThread *>> opened;
        std::vector<StreamError> early;
        source.fetcher()->open(playback::StreamFormat(), [&opened](const QUrl &u, const StreamError &) {
            opened.push_back(std::make_pair(u, QThread::currentThread()));
        });
        const quint64 h = source.fetcher()->open(playback::StreamFormat(), [&early](const QUrl &, const StreamError &e) { early.push_back(e); });
        source.fetcher()->close(h);
        CHECK(waitFor([&]() { return !opened.empty(); }));
        settle(100);
        CHECK(opened.size() == 1 && opened[0].second == main && opened[0].first.host() == QLatin1String("127.0.0.1"));
        CHECK(seen.fetchThread.load() == source.workerThread());
        CHECK(early.size() == 1 && early[0].kind == StreamErrorClass::Cancelled);

        // Typed faults reach the worker's resolver. This one is no sink: a stall, a lost connection
        // and an unexplained failure arrive as invalidate, only Rejected as reportRefused.
        source.reportFault(vid, player::SourceFault::Stalled);
        source.reportFault(vid, player::SourceFault::Connection);
        source.reportFault(vid, player::SourceFault::Unspecified);
        CHECK(waitFor([&]() { return seen.invalidates.load() == 3; }));
        CHECK(seen.refusals.load() == 0 && seen.faultThread.load() == source.workerThread());
        source.reportFault(vid, player::SourceFault::Rejected);
        CHECK(waitFor([&]() { return seen.refusals.load() == 1; }));
        CHECK(seen.invalidates.load() == 3);
    }
    CHECK(seen.deleted.load() == 1 && seen.deletedThread.load() != main && seen.deletedThread.load() != nullptr);
}

// Destruction with a resolve in flight: no completion afterwards, clean shutdown.
static void ply_destructionWithResolveInFlight() {
    Seen seen;
    Answers got;
    {
        ThreadedStreamSource source([&seen]() { return new FakeBackend(&seen); });
        source.resolve(request(), into(&got));
        CHECK(waitFor([&]() { return seen.resolves.load() == 1; })); // running on the worker
    }
    settle(200);
    CHECK(got.empty());
    CHECK(seen.deleted.load() == 1);
    // Destroyed before the worker even created the backend.
    Seen early;
    Answers none;
    {
        ThreadedStreamSource source([&early]() { return new FakeBackend(&early); });
        source.resolve(request(), into(&none));
        source.warmUp();
    }
    settle(100);
    CHECK(none.empty() && early.deleted.load() == 1);
}

// RES-05 / RES-21: warm-up on the worker, hooks on the main thread, markers.
static void res21_warmUpHooksAndMarkers() {
    Seen seen;
    std::vector<qint64> starts;
    std::vector<std::pair<bool, qint64>> dones;
    std::vector<QThread *> hookThreads;
    ThreadedStreamSource::Options options;
    options.hooks.warmupStart = [&](qint64 t) { starts.push_back(t); hookThreads.push_back(QThread::currentThread()); };
    options.hooks.warmupDone = [&](qint64, bool ok, qint64 ms) { dones.push_back(std::make_pair(ok, ms)); hookThreads.push_back(QThread::currentThread()); };
    ThreadedStreamSource source([&seen]() { return new FakeBackend(&seen); }, options);
    source.warmUp();
    CHECK(starts.empty());
    CHECK(waitFor([&]() { return !dones.empty(); }));
    CHECK(seen.warmUps.load() == 1 && starts.size() == 1 && dones.size() == 1 && dones[0].first && dones[0].second == 25);
    for (QThread *t : hookThreads) CHECK(t == QThread::currentThread());
    CHECK(!starts.empty() && starts[0] > 0); // default clock: monotonic

    CHECK(player::warmupStartMarker(12) == QLatin1String("sailtube-timing: warmup-start t=12"));
    CHECK(player::warmupDoneMarker(20, true, 363) == QLatin1String("sailtube-timing: warmup-done t=20 ok=1 visitorMs=363"));
    CHECK(player::warmupDoneMarker(20, false, 9000) == QLatin1String("sailtube-timing: warmup-done t=20 ok=0 visitorMs=9000"));
    CHECK(player::resolveStartMarker(30, 2) == QLatin1String("sailtube-timing: resolve-start t=30 run=2"));
    CHECK(player::resolveDoneMarker(40, 2, playback::ResolvePath::FastPath) == QLatin1String("sailtube-timing: resolve-done t=40 run=2 path=fast"));
    CHECK(player::resolveDoneMarker(40, 3, playback::ResolvePath::ExtractorWithJavaScript) == QLatin1String("sailtube-timing: resolve-done t=40 run=3 path=extractor"));
    CHECK(player::resolveDoneMarker(40, 4, playback::ResolvePath::Cache) == QLatin1String("sailtube-timing: resolve-done t=40 run=4 path=cache"));
    CHECK(player::probeDoneMarker(41, 2) == QLatin1String("sailtube-timing: probe-done t=41 run=2"));
}

// The other direction: a resolver on the worker thread forwarding to one on the main thread
// (the yt-dlp fallback lives with its QProcess runner on the GUI thread).
static void res08_queuedHopToMainThread() {
    Seen seen;
    FakeResolver onMain(&seen, 10);
    const std::shared_ptr<player::threaded::Channel> main = player::threaded::Channel::here();
    QThread worker;
    worker.start();
    const std::shared_ptr<player::threaded::Channel> onWorker = player::threaded::Channel::on(&worker);
    struct State { player::threaded::QueuedResolver *hop = nullptr; QAtomicInt done; QAtomicPointer<QThread> completionThread; QAtomicInt kind; };
    std::shared_ptr<State> state = std::make_shared<State>();
    FakeResolver *target = &onMain;
    onWorker->post([state, main, target]() {
        state->hop = new player::threaded::QueuedResolver(main, [target]() { return static_cast<playback::StreamResolver *>(target); });
        state->hop->resolve(request(), [state](const StreamSet &, const StreamError &e) {
            state->completionThread.store(QThread::currentThread());
            state->kind.store(static_cast<int>(e.kind));
            state->done.ref();
        });
    });
    CHECK(waitFor([&]() { return state->done.load() == 1; }));
    CHECK(seen.resolveThread.load() == QThread::currentThread());       // the target ran on main
    CHECK(state->completionThread.load() == &worker && state->kind.load() == 0); // back on the worker
    // RES-09: nameFinal crosses the hop as nameFinal (the adapter's single pass), not as resolve.
    CHECK(seen.finalsNamed.load() == 0);
    onWorker->post([state]() {
        state->hop->nameFinal(request(), [state](const StreamSet &, const StreamError &) { state->done.ref(); });
    });
    CHECK(waitFor([&]() { return state->done.load() == 2; }));
    CHECK(seen.finalsNamed.load() == 1);
    state->done.deref();
    // Destroyed on the worker with a resolve outstanding: no completion, the target cancels.
    onWorker->post([state]() {
        state->hop->resolve(request(), [state](const StreamSet &, const StreamError &) { state->done.ref(); });
        delete state->hop;
        state->hop = nullptr;
    });
    settle(300);
    CHECK(state->done.load() == 1 && seen.cancels.load() == 1);

    // Typed faults across the hop: a sink target gets the fault as it was reported, on its own
    // thread; a stall or a lost connection never arrives as a refusal.
    TypedResolver typed;
    TypedResolver *typedTarget = &typed;
    onWorker->post([state, main, typedTarget]() {
        player::threaded::QueuedResolver hop(main, [typedTarget]() { return static_cast<playback::StreamResolver *>(typedTarget); });
        player::SourceFaultSink *sink = &hop;
        sink->reportFault(vid, player::SourceFault::Stalled);
        sink->reportFault(vid, player::SourceFault::Connection);
        sink->reportFault(vid, player::SourceFault::Abandoned);
        sink->reportFault(vid, player::SourceFault::Unspecified);
        sink->reportFault(vid, player::SourceFault::Rejected);
    });
    CHECK(waitFor([&]() { return typed.rejected.load() == 1; }));
    CHECK(typed.stalled.load() == 1 && typed.connection.load() == 1 && typed.abandoned.load() == 1 && typed.unspecified.load() == 1);
    CHECK(typed.invalidates.load() == 0 && typed.refusals.load() == 0 && typed.thread.load() == QThread::currentThread());
    onWorker->post([]() { QThread::currentThread()->quit(); });
    worker.wait();
}

// The real chain: SailTuneBackend (FastStreamResolver + MediaProxy + adapter) on the worker
// against SailTune's FakeProvider (player endpoint and media host for the component's probe).
static void realBackend_onWorker() {
    FakeProvider provider;
    CHECK(provider.listen(QHostAddress::LocalHost, 0));
    provider.playerBodies.push_back(fixture("player_ok.json"));
    const quint16 port = provider.serverPort();
    std::vector<std::pair<bool, qint64>> warm;
    ThreadedStreamSource::Options options;
    options.hooks.warmupDone = [&warm](qint64, bool ok, qint64 ms) { warm.push_back(std::make_pair(ok, ms)); };
    Answers got;
    {
        ThreadedStreamSource source([port]() {
            player::SailTuneBackend::Config c;
            c.fast.playerHost = QStringLiteral("127.0.0.1");
            c.fast.playerPort = port;
            c.fast.ignoreSslErrorsForTesting = true;
            c.fast.mediaEndpointOverride = [port](const QString &, QString *h, quint16 *p) {
                *h = QStringLiteral("127.0.0.1"); *p = port; return true;
            };
            return new player::SailTuneBackend(c);
        }, options);
        source.warmUp();
        CHECK(waitFor([&]() { return !warm.empty(); }));
        CHECK(!warm.empty() && warm[0].first && warm[0].second >= 0);
        CHECK(provider.count("/youtubei/v1/visitor_id") == 1);
        source.resolve(request(), into(&got));
        CHECK(waitFor([&]() { return !got.empty(); }));
        CHECK(!got.empty() && got[0].error.ok() && got[0].thread == QThread::currentThread());
        CHECK(!got.empty() && got[0].streams.path == playback::ResolvePath::FastPath && !got[0].streams.audio.isEmpty());
        CHECK(provider.count("/youtubei/v1/visitor_id") == 1 && provider.count("/youtubei/v1/player") == 1);
        CHECK(provider.count("/videoplayback") >= 1); // the component's probe, from the worker
        // A stall reported from the caller's thread reaches the adapter on the worker: the cached
        // answer goes, nothing is counted against the fast path, and the next resolve asks it again.
        source.reportFault(vid, player::SourceFault::Stalled);
        Answers again;
        source.resolve(request(), into(&again));
        CHECK(waitFor([&]() { return !again.empty(); }));
        CHECK(!again.empty() && again[0].error.ok() && again[0].streams.path == playback::ResolvePath::FastPath);
        CHECK(provider.count("/youtubei/v1/player") == 2);
        source.diagnostics();
        settle(100);
        const QVariantMap d = source.diagnostics();
        CHECK(d.value(QStringLiteral("faultsNotTheSource")).toLongLong() == 1 && d.value(QStringLiteral("faultStrikes")).toLongLong() == 0
              && d.value(QStringLiteral("breakerOpen")).toLongLong() == 0 && d.value(QStringLiteral("avoidedVideos")).toLongLong() == 0);
        // A second resolve in flight while the wrapper goes away.
        StreamRequest other = request();
        other.videoId = QStringLiteral("BBBBBBBBBBB");
        source.resolve(other, into(&got));
    }
    settle(300);
    CHECK(got.size() == 1);
}

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    ply_completionOnCallerThread();
    ply_destructionWithResolveInFlight();
    res21_warmUpHooksAndMarkers();
    res08_queuedHopToMainThread();
    realBackend_onWorker();
    std::printf("threaded stream source tests: %d checks, %d failure%s\n", checks, failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
