// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Warm resolver host tests (Linux, Qt5Core), python3 + Core/Worker/Runtime/worker.py over a fake yt-dlp
// archive (tests/warm_worker/make_fake_archive.py). Named cases:
//   warm_reuse, cache_directory, lanes_independent, watchdog_busy_retry (NFR-ROB-03), timeout_restart,
//   cancel_once, output_overflow, crash_and_disable, archive_replaced_mid_session (NFR-ROB-04).
// With WARM_REAL_ARCHIVE set (the real yt-dlp.pyz), also prints the measured cold/warm cost.
#include "../../Core/Worker/Infrastructure/WarmResolver.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QProcess>
#include <QTemporaryDir>
#include <QTimer>
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <functional>
#include <vector>

using namespace nordtile::video;
using worker::Lane;
using worker::WarmResolver;
using worker::WorkerOptions;
static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++failures; } } while (0)
#define CASE(name) std::printf("warm_worker: %s\n", name)

static QString runtimeDir, python;

static bool waitFor(const std::function<bool()> &done, int ms) {
    QElapsedTimer t; t.start();
    while (!done() && t.elapsed() < ms) { QCoreApplication::processEvents(QEventLoop::AllEvents, 20); }
    return done();
}

static WorkerOptions options(const QString &archive, const QString &work, int stuckMs = 30000) {
    WorkerOptions o;
    o.workerProgram = python;
    o.programArguments << QStringLiteral("-I") << runtimeDir + QStringLiteral("/worker.py");
    o.archivePath = archive;
    o.workDirectory = work + QStringLiteral("/worker");
    o.cacheDirectory = work + QStringLiteral("/cache/yt-dlp");
    o.stuckMs = stuckMs;
    return o;
}

struct Call { bool done = false; resolver::RunResult r; qint64 at = -1; };

static void makeArchive(const QString &out, const QString &version) {
    QProcess p;
    p.start(python, QStringList() << QStringLiteral("-I") << runtimeDir + QStringLiteral("/make_fake_archive.py") << out << version);
    p.waitForFinished(10000);
}

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    runtimeDir = QString::fromLocal8Bit(std::getenv("WARM_RUNTIME"));
    python = QString::fromLocal8Bit(std::getenv("WARM_PYTHON"));
    QTemporaryDir tmp;
    const QString work = tmp.path();
    const QString archive = work + QStringLiteral("/yt-dlp.pyz");
    makeArchive(archive, QStringLiteral("1.0"));
    CHECK(QFile::exists(archive));
    QElapsedTimer clock; clock.start();
    auto collect = [&](Call *c) { return [c, &clock](const resolver::RunResult &r) { CHECK(!c->done); c->done = true; c->r = r; c->at = clock.elapsed(); }; };

    {
        WarmResolver w(options(archive, work));
        CASE("warm_reuse");
        std::vector<Call> calls(10);
        for (auto &c : calls) w.run(Lane::Latency, QStringList() << QStringLiteral("--simulate") << QStringLiteral("x"), 20000, 1 << 20, collect(&c));
        CHECK(waitFor([&]() { for (auto &c : calls) if (!c.done) return false; return true; }, 30000));
        for (auto &c : calls) { CHECK(c.r.started && c.r.exitCode == 0 && c.r.output.contains("args=")); CHECK(c.r.diagnostics.contains("diag")); }
        CHECK(w.diagnostics().value(QStringLiteral("latency.starts")).toInt() == 1);
        CHECK(w.diagnostics().value(QStringLiteral("latency.extractorVersion")).toString() == QStringLiteral("1.0"));

        CASE("cache_directory");
        Call c;
        w.run(Lane::Latency, QStringList() << QStringLiteral("--no-cache-dir") << QStringLiteral("--") << QStringLiteral("x"), 20000, 1 << 20, collect(&c));
        CHECK(waitFor([&]() { return c.done; }, 10000));
        CHECK(c.r.output.contains("--cache-dir " + QFile::encodeName(work) + "/cache/yt-dlp"));
        CHECK(!c.r.output.contains("--no-cache-dir"));
        CHECK(QDir(work + QStringLiteral("/cache/yt-dlp")).exists());
        CHECK(WarmResolver::withCacheDirectory(QStringList() << QStringLiteral("-q"), QStringLiteral("/c")) ==
              (QStringList() << QStringLiteral("--cache-dir") << QStringLiteral("/c") << QStringLiteral("-q")));
        CHECK(WarmResolver::withCacheDirectory(QStringList() << QStringLiteral("-q"), QString()) == QStringList() << QStringLiteral("-q"));

        CASE("lanes_independent");
        Call slow, fast;
        const qint64 t0 = clock.elapsed();
        w.run(Lane::Background, QStringList() << QStringLiteral("--sleep") << QStringLiteral("3"), 20000, 1 << 20, collect(&slow));
        QTimer::singleShot(200, [&]() { w.run(Lane::Latency, QStringList() << QStringLiteral("x"), 20000, 1 << 20, collect(&fast)); });
        CHECK(waitFor([&]() { return fast.done; }, 10000));
        CHECK(fast.at - t0 < 2500);       // did not wait behind the 3 s background call
        CHECK(waitFor([&]() { return slow.done; }, 10000));
        CHECK(slow.r.exitCode == 0);
        std::printf("warm_worker: latency call answered after %lld ms while the background lane was busy\n", static_cast<long long>(fast.at - t0));

        CASE("output_overflow");
        Call big;
        w.run(Lane::Latency, QStringList() << QStringLiteral("--big") << QStringLiteral("5000"), 20000, 1000, collect(&big));
        CHECK(waitFor([&]() { return big.done; }, 10000));
        CHECK(big.r.overflow && big.r.output.size() == 1000);

        CASE("cancel_once");
        Call cancelled, after;
        const quint64 id = w.run(Lane::Latency, QStringList() << QStringLiteral("--sleep") << QStringLiteral("5"), 20000, 1 << 20, collect(&cancelled));
        QTimer::singleShot(300, [&]() { w.cancel(id); w.cancel(id); });
        CHECK(waitFor([&]() { return cancelled.done; }, 5000));
        CHECK(cancelled.r.cancelled);
        w.run(Lane::Latency, QStringList() << QStringLiteral("x"), 20000, 1 << 20, collect(&after));
        CHECK(waitFor([&]() { return after.done; }, 10000));
        CHECK(after.r.exitCode == 0);
        waitFor([]() { return false; }, 300);   // a second completion would show up here (CHECK in collect)
    }
    {
        CASE("watchdog_busy_retry");
        CHECK(WorkerOptions().stuckMs == 30000);   // the shipped threshold (NFR-ROB-03: logged within 35 s)
        WarmResolver w(options(archive, work, 1500));
        int stuckSignals = 0;
        QObject::connect(&w, &WarmResolver::laneStuck, [&](int lane, qint64) { CHECK(lane == 0); ++stuckSignals; });
        Call hung, q1, q2, bg;
        const qint64 t0 = clock.elapsed();
        w.run(Lane::Latency, QStringList() << QStringLiteral("--sleep") << QStringLiteral("8"), 20000, 1 << 20, collect(&hung));
        w.run(Lane::Latency, QStringList() << QStringLiteral("x"), 20000, 1 << 20, collect(&q1));
        w.run(Lane::Latency, QStringList() << QStringLiteral("x"), 20000, 1 << 20, collect(&q2));
        CHECK(waitFor([&]() { return q1.done && q2.done; }, 6000));
        CHECK(q1.r.timedOut && q1.r.diagnostics == "worker-busy" && q2.r.diagnostics == "worker-busy");
        CHECK(q1.at - t0 < 4000);
        CHECK(stuckSignals == 1);
        w.run(Lane::Background, QStringList() << QStringLiteral("x"), 20000, 1 << 20, collect(&bg));
        CHECK(waitFor([&]() { return bg.done; }, 10000));
        CHECK(bg.r.exitCode == 0);                                   // the other lane keeps serving
        CHECK(waitFor([&]() { return hung.done; }, 12000));
        CHECK(hung.r.exitCode == 0);                                 // the stuck call still finishes
        CHECK(w.diagnostics().value(QStringLiteral("latency.starts")).toInt() == 1);   // no double spawn
        CHECK(w.diagnostics().value(QStringLiteral("latency.busyFailures")).toInt() == 2);

        CASE("timeout_restart");
        Call late, next;
        w.run(Lane::Latency, QStringList() << QStringLiteral("--sleep") << QStringLiteral("5"), 800, 1 << 20, collect(&late));
        CHECK(waitFor([&]() { return late.done; }, 5000));
        CHECK(late.r.timedOut && late.r.started);
        w.run(Lane::Latency, QStringList() << QStringLiteral("x"), 20000, 1 << 20, collect(&next));
        CHECK(waitFor([&]() { return next.done; }, 15000));
        CHECK(next.r.exitCode == 0);
        CHECK(w.diagnostics().value(QStringLiteral("latency.restarts")).toInt() == 1);
        CHECK(w.diagnostics().value(QStringLiteral("latency.crashes")).toInt() == 0);   // our own kill is no crash
    }
    {
        CASE("warm_up_ahead");
        WarmResolver w(options(archive, work));
        w.warmUp(Lane::Latency);
        CHECK(waitFor([&]() { return w.diagnostics().value(QStringLiteral("latency.importMs")).toLongLong() >= 0; }, 15000));
        Call c;
        const qint64 t0 = clock.elapsed();
        w.run(Lane::Latency, QStringList() << QStringLiteral("x"), 20000, 1 << 20, collect(&c));
        CHECK(waitFor([&]() { return c.done; }, 10000));
        CHECK(c.r.exitCode == 0 && c.at - t0 < 1500);   // no start or import on the first call
        CHECK(w.diagnostics().value(QStringLiteral("latency.starts")).toInt() == 1);
        CHECK(w.diagnostics().value(QStringLiteral("background.starts")).toInt() == 0);   // lazily, on its first call
        CHECK(w.idle(Lane::Latency) && !w.idle(Lane::Background));
        CHECK(w.laneIdle(0) && !w.laneIdle(1) && w.laneAvailable(1));
        Call busy;
        w.run(Lane::Latency, QStringList() << QStringLiteral("--sleep") << QStringLiteral("1"), 20000, 1 << 20, collect(&busy));
        CHECK(waitFor([&]() { return !w.idle(Lane::Latency); }, 2000));   // serving: the runner would use a one-shot child
        CHECK(waitFor([&]() { return busy.done; }, 10000));
        CHECK(waitFor([&]() { return w.idle(Lane::Latency); }, 2000));
        w.wakeLane(1);
        CHECK(waitFor([&]() { return w.idle(Lane::Background); }, 15000));
        resolver::WarmBackend &backend = w;   // the runner's view
        Call viaBackend, cancelledViaBackend;
        backend.runOnLane(1, QStringList() << QStringLiteral("x"), 20000, 1 << 20, collect(&viaBackend));
        CHECK(waitFor([&]() { return viaBackend.done; }, 10000));
        CHECK(viaBackend.r.exitCode == 0);
        const quint64 bid = backend.runOnLane(1, QStringList() << QStringLiteral("--sleep") << QStringLiteral("5"), 20000, 1 << 20, collect(&cancelledViaBackend));
        backend.cancelRun(bid);
        CHECK(waitFor([&]() { return cancelledViaBackend.done; }, 5000));
        CHECK(cancelledViaBackend.r.cancelled);
    }
    {
        CASE("crash_and_disable");
        WarmResolver w(options(archive, work));
        for (int i = 0; i < 3; ++i) {
            Call c;
            w.run(Lane::Latency, QStringList() << QStringLiteral("--crash"), 20000, 1 << 20, collect(&c));
            CHECK(waitFor([&]() { return c.done; }, 15000));
            CHECK(c.r.crashed);
        }
        CHECK(!w.available(Lane::Latency));
        CHECK(w.available(Lane::Background));
        Call refused;
        w.run(Lane::Latency, QStringList() << QStringLiteral("x"), 20000, 1 << 20, collect(&refused));
        CHECK(waitFor([&]() { return refused.done; }, 2000));
        CHECK(!refused.r.started && refused.r.diagnostics == "worker-unavailable");
    }
    {
        CASE("archive_replaced_mid_session");
        WarmResolver w(options(archive, work));
        Call first;
        w.run(Lane::Latency, QStringList() << QStringLiteral("x"), 20000, 1 << 20, collect(&first));
        CHECK(waitFor([&]() { return first.done; }, 15000));
        makeArchive(archive + QStringLiteral(".new"), QStringLiteral("2.0"));
        QFile::remove(archive);
        CHECK(QFile::rename(archive + QStringLiteral(".new"), archive));   // the update lands mid-session
        for (int i = 0; i < 10; ++i) {
            Call c;
            w.run(Lane::Latency, QStringList() << QStringLiteral("--lazy"), 20000, 1 << 20, collect(&c));
            CHECK(waitFor([&]() { return c.done; }, 10000));
            CHECK(c.r.exitCode == 0 && c.r.output.contains("lazy=1.0"));   // the loaded copy, not the new file
        }
        CHECK(w.diagnostics().value(QStringLiteral("latency.versionSkew")).toBool());
        WarmResolver relaunched(options(archive, work));
        Call c;
        relaunched.run(Lane::Latency, QStringList() << QStringLiteral("--lazy"), 20000, 1 << 20, collect(&c));
        CHECK(waitFor([&]() { return c.done; }, 15000));
        CHECK(c.r.output.contains("lazy=2.0"));
        CHECK(relaunched.diagnostics().value(QStringLiteral("latency.extractorVersion")).toString() == QStringLiteral("2.0"));
        CHECK(!relaunched.diagnostics().value(QStringLiteral("latency.versionSkew")).toBool());
    }
    const QString real = QString::fromLocal8Bit(std::getenv("WARM_REAL_ARCHIVE"));
    if (!real.isEmpty() && QFile::exists(real)) {
        CASE("real_archive_measurement");
        QFile clip(work + QStringLiteral("/clip.mp4"));
        CHECK(clip.open(QIODevice::WriteOnly));
        clip.write(QByteArray("\x00\x00\x00\x18" "ftypmp42", 12) + QByteArray(4096, '\0'));
        clip.close();
        const QStringList args = QStringList() << QStringLiteral("--ignore-config") << QStringLiteral("--no-plugin-dirs")
            << QStringLiteral("--no-cache-dir") << QStringLiteral("--no-warnings") << QStringLiteral("--enable-file-urls")
            << QStringLiteral("--simulate") << QStringLiteral("--print") << QStringLiteral("%(id)s %(ext)s")
            << QStringLiteral("file://") + clip.fileName();
        WarmResolver w(options(real, work));
        QElapsedTimer t; t.start();
        Call firstCall;
        w.run(Lane::Latency, args, 60000, 1 << 20, collect(&firstCall));
        CHECK(waitFor([&]() { return firstCall.done; }, 60000));
        const qint64 firstMs = t.elapsed();
        CHECK(firstCall.r.exitCode == 0 && firstCall.r.output.contains("clip mp4"));
        std::vector<qint64> warm;
        for (int i = 0; i < 10; ++i) {
            Call c; t.restart();
            w.run(Lane::Latency, args, 60000, 1 << 20, collect(&c));
            CHECK(waitFor([&]() { return c.done; }, 60000));
            warm.push_back(t.elapsed());
            CHECK(c.r.exitCode == 0);
        }
        std::sort(warm.begin(), warm.end());
        std::printf("warm_worker: real yt-dlp %s: first call (start + import + run) %lld ms, import %lld ms, warm median %lld ms, max %lld ms\n",
                    qPrintable(w.diagnostics().value(QStringLiteral("latency.extractorVersion")).toString()),
                    static_cast<long long>(firstMs), w.diagnostics().value(QStringLiteral("latency.importMs")).toLongLong(),
                    static_cast<long long>(warm[warm.size() / 2]), static_cast<long long>(warm.back()));
    }
    if (failures) { std::printf("warm_worker: %d failure(s)\n", failures); return 1; }
    std::printf("warm_worker WarmResolverTest: all checks passed\n");
    return 0;
}
