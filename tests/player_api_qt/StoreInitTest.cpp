// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// NFR-PERF-01 start-up API of the STORE engine stub (GstEngineStore.cpp, no GStreamer, E9):
// startInitialize() runs its afterInit job inline and delivers ready(false, ...) once on the GUI
// thread; initialized() / waitInitialized() are false at once; preloadDecoders() is a no-op.
// Build: tests/player_api_qt/run.sh (part "store", Qt5Core only).
#include "Core/Player/Infrastructure/GstEngine.h"
#include "Core/Player/Infrastructure/VideoFrameMailbox.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QThread>

#include <cstdio>
#include <functional>

using namespace nordtile::player;

static int checks = 0;
static int failures = 0;
#define CHECK(c) do { ++checks; if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

static void spin(int ms) {
    QElapsedTimer t;
    t.start();
    while (t.elapsed() < ms) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(1);
    }
}

// NFR-PERF-01 (STORE): the stub start-up API.
static void nfrPerf01_storeStubStartupApi() {
    int ready = 0, preloaded = 0;
    bool ok = true, onGui = false;
    QString error;
    GstEngineInit *n = GstEngineInit::instance();
    QObject::connect(n, &GstEngineInit::ready, [&](bool o, const QString &e, qint64) {
        ++ready;
        ok = o;
        error = e;
        onGui = QThread::currentThread() == QCoreApplication::instance()->thread();
    });
    QObject::connect(n, &GstEngineInit::decodersPreloaded, [&](int, qint64) { ++preloaded; });
    bool hook = false;
    QElapsedTimer t;
    t.start();
    GstEngine::startInitialize([&hook]() { hook = true; });
    CHECK(t.elapsed() < 5);
    CHECK(hook);        // inline in STORE
    CHECK(ready == 0);  // queued: never from inside the call
    CHECK(!GstEngine::initialized());
    t.restart();
    CHECK(!GstEngine::waitInitialized(5000));
    CHECK(t.elapsed() < 5);  // nothing to wait for
    GstEngine::preloadDecoders();
    CHECK(GstEngine::preloadFeatures().empty());
    spin(100);
    CHECK(ready == 1);
    CHECK(!ok);
    CHECK(error.contains(QStringLiteral("only available in the full")));
    CHECK(onGui);
    bool secondHook = false;
    GstEngine::startInitialize([&secondHook]() { secondHook = true; });
    spin(100);
    CHECK(ready == 1);
    CHECK(!secondHook);
    CHECK(preloaded == 0);
    // The stub engine still refuses to build.
    VideoFrameMailbox mailbox;
    GstEngine engine(&mailbox);
    QString buildError;
    CHECK(!engine.build(PlanInput(), &buildError));
    CHECK(!buildError.isEmpty());
    CHECK(GstEngine::availableElements().empty());
}

int main(int argc, char **argv) {
    std::setvbuf(stdout, nullptr, _IOLBF, 0);
    QCoreApplication app(argc, argv);
    std::printf("nfrPerf01_storeStubStartupApi\n");
    nfrPerf01_storeStubStartupApi();
    std::printf("engine store stub tests: %d checks, %d failure%s\n", checks, failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
