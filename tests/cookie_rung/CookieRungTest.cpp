// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// ACC-11 / RES-08 cookie rung on the yt-dlp runner: begin() once per run, end() exactly once per run on
// success, failure, timeout, cancel and runner shutdown; --cookies only with a jar; the jar path never comes
// back in output or diagnostics. Runs YtDlpRunner against a fake executable that echoes its arguments.
#include "Core/Resolver/Infrastructure/YtDlpRunner.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QTemporaryDir>
#include <cstdio>
#include <functional>

using namespace nordtile::video::resolver;
static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++failures; } } while (0)

static bool waitFor(const std::function<bool()> &done, int ms) {
    QElapsedTimer t; t.start();
    while (!done() && t.elapsed() < ms) QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    return done();
}

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    QTemporaryDir tmp;
    const QString fake = tmp.path() + QStringLiteral("/fake-yt-dlp");
    QFile f(fake);
    f.open(QIODevice::WriteOnly);
    f.write("#!/bin/sh\nfor a in \"$@\"; do case \"$a\" in --sleep) sleep 5;; --fail) echo \"failed with $*\" >&2; exit 1;; esac; done\n"
            "echo \"args: $*\"; echo \"diag: $*\" >&2\n");
    f.close();
    f.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
    RunnerOptions o;
    o.executablePath = fake;
    o.pythonChild = false;
    int begins = 0, ends = 0;
    QString jar = tmp.path() + QStringLiteral("/private/jar-0001.txt");
    QString handedOut;
    {
        YtDlpRunner runner(o);
        runner.setCookieHooks([&]() { ++begins; ExtractorCookies c; c.jarPath = jar; c.generation = 7; handedOut = jar; return c; },
                              [&](const ExtractorCookies &c) { ++ends; CHECK(c.jarPath == handedOut && c.generation == 7); });
        // success with a jar
        RunResult r; bool done = false;
        runner.run(QStringList() << QStringLiteral("--simulate") << QStringLiteral("--") << QStringLiteral("url"), 10000, 1 << 16,
                   [&](const RunResult &x) { r = x; done = true; });
        CHECK(waitFor([&]() { return done; }, 10000));
        CHECK(r.exitCode == 0 && r.output.contains("--cookies [cookies] -- url"));
        CHECK(!r.output.contains(jar.toUtf8()) && !r.diagnostics.contains(jar.toUtf8()));
        CHECK(begins == 1 && ends == 1);
        // failure
        done = false;
        runner.run(QStringList() << QStringLiteral("--fail"), 10000, 1 << 16, [&](const RunResult &x) { r = x; done = true; });
        CHECK(waitFor([&]() { return done; }, 10000));
        CHECK(r.exitCode == 1 && !r.diagnostics.contains(jar.toUtf8()) && r.diagnostics.contains("[cookies]"));
        CHECK(begins == 2 && ends == 2);
        // timeout
        done = false;
        runner.run(QStringList() << QStringLiteral("--sleep"), 1000, 1 << 16, [&](const RunResult &x) { r = x; done = true; });
        CHECK(waitFor([&]() { return done; }, 10000));
        CHECK(r.timedOut && begins == 3 && ends == 3);
        // cancel
        done = false;
        const quint64 id = runner.run(QStringList() << QStringLiteral("--sleep"), 10000, 1 << 16, [&](const RunResult &x) { r = x; done = true; });
        runner.cancel(id);
        runner.cancel(id);
        CHECK(waitFor([&]() { return done; }, 5000));
        CHECK(r.cancelled && begins == 4 && ends == 4);
        // signed-in features off: no --cookies at all
        jar.clear();
        done = false;
        runner.run(QStringList() << QStringLiteral("--") << QStringLiteral("url"), 10000, 1 << 16, [&](const RunResult &x) { r = x; done = true; });
        CHECK(waitFor([&]() { return done; }, 10000));
        CHECK(r.exitCode == 0 && !r.output.contains("--cookies") && begins == 5 && ends == 5);
        // shutdown with a call in flight: dropCookieHooks hands the jar back once, the destructor does not again
        jar = tmp.path() + QStringLiteral("/private/jar-0002.txt");
        runner.run(QStringList() << QStringLiteral("--sleep"), 10000, 1 << 16, [&](const RunResult &) {});
        CHECK(begins == 6);
        runner.dropCookieHooks();
        CHECK(ends == 6);
    }
    CHECK(ends == 6);   // no second end() from the destructor
    CHECK(YtDlpRunner::withCookies(QStringList() << QStringLiteral("-q"), QStringLiteral("/j")) ==
          (QStringList() << QStringLiteral("--cookies") << QStringLiteral("/j") << QStringLiteral("-q")));
    CHECK(YtDlpRunner::withCookies(QStringList() << QStringLiteral("-q"), QString()) == QStringList() << QStringLiteral("-q"));
    if (failures) { std::printf("cookie_rung: %d failure(s)\n", failures); return 1; }
    std::printf("cookie_rung CookieRungTest: all checks passed\n");
    return 0;
}
