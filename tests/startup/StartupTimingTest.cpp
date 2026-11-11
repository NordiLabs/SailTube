// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// NFR-PERF-01/07: the timing points log once per stage and per interval, in journal format.
#include "Clients/Sailfish/Presentation/StartupTiming.h"

#include <QCoreApplication>
#include <QRegularExpression>
#include <QThread>
#include <QTimer>
#include <cstdio>

using nordtile::video::sailfish::StartupTiming;

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    QElapsedTimer clock;
    clock.start();
    StartupTiming t(clock);
    QStringList signalled;
    QObject::connect(&t, &StartupTiming::marked, [&signalled](const QString &stage) { signalled << stage; });

    CHECK(!t.hasMark(QStringLiteral("app")));
    t.mark(QStringLiteral("app"));
    t.mark(QStringLiteral("app"));            // once only
    t.mark(QString());                         // ignored
    t.mark(QStringLiteral("first-frame"));
    CHECK(t.lines().size() == 2);
    const QRegularExpression startup(QStringLiteral("^sailtube-timing startup (app|first-frame) \\d+$"));
    for (const QString &l : t.lines()) CHECK(startup.match(l).hasMatch());
    // The shell waits for "first-frame" before loading non-critical components.
    CHECK(signalled == (QStringList() << QStringLiteral("app") << QStringLiteral("first-frame")));
    CHECK(t.hasMark(QStringLiteral("app")) && t.hasMark(QStringLiteral("first-frame")));
    CHECK(!t.hasMark(QStringLiteral("content")));

    t.end(QStringLiteral("video"), QStringLiteral("first-frame"));   // no begin: nothing
    CHECK(t.lines().size() == 2);
    CHECK(!t.running(QStringLiteral("video")));
    t.begin(QStringLiteral("video"));
    CHECK(t.running(QStringLiteral("video")));
    t.end(QStringLiteral("video"), QStringLiteral("ready"));
    t.end(QStringLiteral("video"), QStringLiteral("ready"));          // once per begin
    t.end(QStringLiteral("video"), QStringLiteral("first-frame"));
    CHECK(t.lines().size() == 4);
    CHECK(QRegularExpression(QStringLiteral("^sailtube-timing video ready \\d+$")).match(t.lines().at(2)).hasMatch());
    t.begin(QStringLiteral("video"));                                 // a new tap restarts it
    t.end(QStringLiteral("video"), QStringLiteral("ready"));
    CHECK(t.lines().size() == 5);
    CHECK(t.elapsed() >= 0);

    // Core's trace (docs/PERF_STARTUP.md): the time before main() and an exit after a stage.
    {
        QElapsedTimer c2; c2.start();
        StartupTiming trace(c2);
        // /proc counts in clock ticks (10 ms), so a fresh process can read 0: age it first.
        QThread::msleep(50);
        const qint64 age = StartupTiming::processAgeMs();
#ifdef Q_OS_LINUX
        CHECK(age >= 30);
#endif
        trace.setPreMainMs(age - c2.elapsed());
        CHECK(trace.lines().size() == 1);
        CHECK(QRegularExpression(QStringLiteral("^sailtube-timing startup pre-main \\d+$")).match(trace.lines().at(0)).hasMatch());
        CHECK(trace.sinceProcessStart() >= trace.preMainMs());
        trace.setPreMainMs(-5);
        CHECK(trace.preMainMs() == 0);
        // exitAfter: the event loop ends right after the stage is marked.
        trace.exitAfter(QStringLiteral("qml-loaded"));
        QTimer::singleShot(0, [&trace]() { trace.mark(QStringLiteral("app")); trace.mark(QStringLiteral("qml-loaded")); });
        QTimer guard; guard.setSingleShot(true);
        bool timedOut = false;
        QObject::connect(&guard, &QTimer::timeout, [&timedOut]() { timedOut = true; QCoreApplication::exit(1); });
        guard.start(5000);
        CHECK(app.exec() == 0);
        CHECK(!timedOut);
        // Already marked: exitAfter quits on the next turn of the loop.
        trace.exitAfter(QStringLiteral("app"));
        guard.start(5000);
        CHECK(app.exec() == 0);
        CHECK(!timedOut);
    }

    std::printf("startup timing tests: %s (%d failures)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
