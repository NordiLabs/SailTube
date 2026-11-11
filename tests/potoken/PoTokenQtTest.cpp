// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Process-level RES-14 checks: the PoTokenProvider with a fake Deno (fake_deno.c) that binds
// 127.0.0.1 or 0.0.0.0, the real /proc socket tables, the frozen install and the pin refusals;
// and the yt-dlp argument plumbing of YtDlpVideoService (only when ready AND requested).
// Built twice by tests/potoken/run.sh: FULL (this file) and STORE (-DSAILTUBE_STORE, the
// service part only). Linux only for the process part (/proc).
#include "Core/Resolver/Infrastructure/YtDlpRunner.h"
#include "Core/Resolver/Infrastructure/YtDlpVideoService.h"
#ifndef SAILTUBE_STORE
#include "Core/Resolver/Infrastructure/PoTokenProvider.h"
#endif

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QTimer>
#include <cstdio>
#include <functional>
#include <signal.h>

using namespace nordtile::video;
using namespace nordtile::video::resolver;

static int failures = 0;
static int checks = 0;
#define CHECK(c) do { ++checks; if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

static QString fakeDeno;     // argv[1]: the compiled fake_deno
static QString recorder;     // argv[2]: a script that writes its arguments to $SAILTUBE_TEST_RECORD

static void spin(int ms) {
    QEventLoop loop;
    QTimer::singleShot(ms, &loop, SLOT(quit()));
    loop.exec();
}

static bool waitFor(const std::function<bool()> &done, int timeoutMs) {
    QElapsedTimer t;
    t.start();
    while (!done()) {
        if (t.elapsed() > timeoutMs) return false;
        spin(10);
    }
    return true;
}

static QByteArray readAll(const QString &path) {
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}


static QStringList recordedArguments(const QString &file) {
    return QString::fromUtf8(readAll(file)).split(QLatin1Char('\n'), QString::SkipEmptyParts);
}

static void serviceArgs_withoutProvider(const QStringList &common) {
    const QStringList plain = YtDlpVideoService::detailsArguments(common, QStringLiteral("aBcDeFgHiJk"));
    CHECK(!plain.contains(QStringLiteral("--plugin-dirs")));
    CHECK(plain.contains(QStringLiteral("youtube:skip=dash")));
    for (const QString &a : plain) {
        CHECK(!a.contains(QLatin1String("fetch_pot")));
        CHECK(!a.contains(QLatin1String("youtubepot")));
    }
}

#ifndef SAILTUBE_STORE
static void writeFile(const QString &path, const QByteArray &data) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) f.write(data);
}

static QString sha(const QString &path) {
    return QString::fromLatin1(QCryptographicHash::hash(readAll(path), QCryptographicHash::Sha256).toHex());
}

struct Tree {
    QTemporaryDir dir;
    QString root;
    potoken::Layout layout;
    potoken::Pin pin;
    Tree() {
        root = QFileInfo(dir.path()).canonicalFilePath() + QStringLiteral("/potoken");
        layout.root = root.toStdString();
        QDir().mkpath(root + QStringLiteral("/deno"));
        QFile::copy(fakeDeno, QString::fromStdString(layout.denoBinary()));
        QFile::setPermissions(QString::fromStdString(layout.denoBinary()),
                              QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
        writeFile(QString::fromStdString(layout.lockfile()), "{ \"version\": \"5\" }\n");
        writeFile(QString::fromStdString(layout.mainModule()), "// provider entry (fake)\n");
        writeFile(QString::fromStdString(layout.pluginZip()), QByteArray("PK\x05\x06", 4) + QByteArray(18, '\0'));
        pin.providerTag = "1.2.3";
        pin.providerCommit = std::string(40, '7');
        pin.denoVersion = "2.4.5";
        pin.lockfileSha256 = sha(QString::fromStdString(layout.lockfile())).toStdString();
        pin.pluginSha256 = sha(QString::fromStdString(layout.pluginZip())).toStdString();
        pin.denoSha256 = sha(QString::fromStdString(layout.denoBinary())).toStdString();
        writeFile(QString::fromStdString(layout.stamp()), QByteArray::fromStdString(potoken::formatStamp(pin)));
    }
    void flag(const char *name) { writeFile(root + QLatin1Char('/') + QLatin1String(name), "1"); }
    QString file(const char *name) const { return root + QLatin1Char('/') + QLatin1String(name); }
    PoTokenProviderOptions options() const {
        PoTokenProviderOptions o;
        o.root = root;
        o.pin = pin;
        o.architecture = QStringLiteral("aarch64");
        o.pollMs = 50;
        o.timing.verifyTimeoutMs = 5000;
        o.hashSliceBytes = 4096;
        return o;
    }
};

static bool processGone(qint64 pid) {
    return waitFor([pid]() { return ::kill(static_cast<pid_t>(pid), 0) != 0; }, 3000);
}

// RES-14 accept: start, verify the 127.0.0.1-only bind against the real /proc tables, expose
// the base URL; the exact Deno command line and environment reach the process; off stops it.
static void res14_processLoopbackReady() {
    qputenv("SAILTUBE_TEST_SECRET", "must-not-reach-the-helper");
    Tree t;
    PoTokenProvider p(t.options());
    CHECK(p.state() == potoken::State::Off && p.extractorArguments().isEmpty());
    p.setEnabled(true);
    CHECK(waitFor([&p]() { return p.ready() || p.state() == potoken::State::Refused; }, 15000));
    CHECK(p.ready());
    std::printf("  loopback helper: %s\n", qPrintable(p.diagnostics()));
    // RES-14: aarch64 only (the fake runs where the test builds; the name is reported as detected).
    CHECK(!PoTokenProvider::runningArchitecture().isEmpty());
    // The heap ceiling setting applies from the next start; the running helper keeps its own.
    p.setHeapMb(256);
    CHECK(p.ready());
    CHECK(p.port() > 0 && p.baseUrl() == QStringLiteral("http://127.0.0.1:%1").arg(p.port()));
    CHECK(p.extractorArguments() == (QStringList() << QStringLiteral("--plugin-dirs")
        << QString::fromStdString(t.layout.pluginDir()) << QStringLiteral("--extractor-args")
        << QStringLiteral("youtubepot-bgutilhttp:base_url=http://127.0.0.1:%1").arg(p.port())));
    CHECK(recordedArguments(t.file("install-args.txt")) == (QStringList() << QStringLiteral("install") << QStringLiteral("--frozen")));
    QStringList expected;
    for (const std::string &a : potoken::serveCommand(t.layout, p.port(), potoken::defaultHeapMb).arguments)
        expected << QString::fromStdString(a);
    CHECK(recordedArguments(t.file("run-args.txt")) == expected);
    const QStringList env = recordedArguments(t.file("run-env.txt"));
    CHECK(env.contains(QStringLiteral("HOME=") + t.root));
    CHECK(env.contains(QStringLiteral("DENO_DIR=") + QString::fromStdString(t.layout.denoDir())));
    for (const QString &e : env) {
        CHECK(!e.startsWith(QLatin1String("SAILTUBE_")));
        CHECK(!e.startsWith(QLatin1String("PATH=")));
    }
    CHECK(QFileInfo(QString::fromStdString(t.layout.webGpuStub())).isFile());
    CHECK(potoken::installCurrent(readAll(QString::fromStdString(t.layout.installMarker())).toStdString(), t.pin));
    const qint64 pid = p.helperPid();
    CHECK(pid > 0);
    // Re-check while ready keeps it ready.
    spin(200);
    CHECK(p.ready());
    p.setEnabled(false);
    CHECK(p.state() == potoken::State::Off && p.extractorArguments().isEmpty() && p.baseUrl().isEmpty());
    CHECK(processGone(pid));
    // Enabled again: the frozen install is current, so it is not run a second time.
    QFile::remove(t.file("install-args.txt"));
    p.setEnabled(true);
    CHECK(waitFor([&p]() { return p.ready() || p.state() == potoken::State::Refused; }, 15000));
    CHECK(p.ready());
    CHECK(!QFileInfo(t.file("install-args.txt")).exists());
    const qint64 pid2 = p.helperPid();
    p.setEnabled(false);
    CHECK(processGone(pid2));
}

// RES-14 / NFR-SEC-05: a helper listening on 0.0.0.0 is stopped and refused.
static void res14_processAnyBindRefused() {
    Tree t;
    t.flag("fake-bind-any");
    PoTokenProvider p(t.options());
    qint64 seenPid = 0;
    QObject::connect(&p, &PoTokenProvider::stateChanged, [&p, &seenPid]() {
        if (p.helperPid() > 0) seenPid = p.helperPid();
    });
    p.setEnabled(true);
    CHECK(waitFor([&p]() { return p.state() == potoken::State::Refused || p.ready(); }, 15000));
    CHECK(p.state() == potoken::State::Refused && p.reasonText() == QLatin1String(potoken::reasonText(potoken::Reason::NonLoopbackBind)));
    CHECK(!p.ready() && p.extractorArguments().isEmpty() && p.helperPid() == 0);
    CHECK(seenPid > 0 && processGone(seenPid));
    std::printf("  all-interfaces helper: %s\n", qPrintable(p.diagnostics()));
    spin(300);
    CHECK(p.state() == potoken::State::Refused);  // no retry
}

// RES-14: pin refusals happen before anything runs.
static void res14_processPinRefusals() {
    {
        Tree t;
        writeFile(QString::fromStdString(t.layout.lockfile()), "{ \"version\": \"5\", \"edited\": true }\n");
        PoTokenProvider p(t.options());
        p.setEnabled(true);
        CHECK(waitFor([&p]() { return p.state() == potoken::State::Refused; }, 5000));
        CHECK(p.reasonText() == QLatin1String(potoken::reasonText(potoken::Reason::LockfileMismatch)));
        CHECK(!QFileInfo(t.file("install-args.txt")).exists() && !QFileInfo(t.file("run-args.txt")).exists());
    }
    {
        Tree t;
        PoTokenProviderOptions o = t.options();
        o.pin.providerCommit.clear();   // tag-only
        PoTokenProvider p(o);
        p.setEnabled(true);
        CHECK(waitFor([&p]() { return p.state() == potoken::State::Refused; }, 5000));
        CHECK(p.reasonText() == QLatin1String(potoken::reasonText(potoken::Reason::NotPinned)));
        CHECK(!QFileInfo(t.file("install-args.txt")).exists());
    }
    {
        Tree t;
        writeFile(QString::fromStdString(t.layout.stamp()), "tag=1.2.3\ncommit=" + QByteArray(40, '8') + "\ndeno=2.4.5\n");
        PoTokenProvider p(t.options());
        p.setEnabled(true);
        CHECK(waitFor([&p]() { return p.state() == potoken::State::Refused; }, 5000));
        CHECK(p.reasonText() == QLatin1String(potoken::reasonText(potoken::Reason::CommitMismatch)));
    }
    {
        Tree t;
        QFile::remove(QString::fromStdString(t.layout.pluginZip()));
        PoTokenProvider p(t.options());
        p.setEnabled(true);
        CHECK(waitFor([&p]() { return p.state() == potoken::State::Refused; }, 5000));
        CHECK(p.reasonText() == QLatin1String(potoken::reasonText(potoken::Reason::NotStaged)));
    }
    {
        Tree t;
        PoTokenProviderOptions o = t.options();
        o.architecture = QStringLiteral("armv7l");
        PoTokenProvider p(o);
        p.setEnabled(true);
        CHECK(p.state() == potoken::State::Refused);
        CHECK(p.reasonText() == QLatin1String(potoken::reasonText(potoken::Reason::UnsupportedArchitecture)));
    }
    {
        // The build's own pin is empty until reviewed: refused, nothing runs.
        Tree t;
        PoTokenProviderOptions o = t.options();
        o.pin = potoken::builtInPin();
        PoTokenProvider p(o);
        p.setEnabled(true);
        CHECK(waitFor([&p]() { return p.state() == potoken::State::Refused; }, 5000));
        CHECK(p.reasonText() == QLatin1String(potoken::reasonText(potoken::Reason::NotPinned)));
    }
}

// RES-14: an install that rewrites the lockfile is refused (never retried unlocked); a failed
// frozen install and a helper that exits back off.
static void res14_processInstallAndExitFailures() {
    {
        Tree t;
        t.flag("fake-touch-lock");
        PoTokenProvider p(t.options());
        p.setEnabled(true);
        CHECK(waitFor([&p]() { return p.state() == potoken::State::Refused; }, 5000));
        CHECK(p.reasonText() == QLatin1String(potoken::reasonText(potoken::Reason::LockfileChanged)));
        CHECK(!QFileInfo(t.file("run-args.txt")).exists());
        CHECK(!QFileInfo(QString::fromStdString(t.layout.installMarker())).exists());
    }
    {
        Tree t;
        t.flag("fake-install-fail");
        PoTokenProvider p(t.options());
        p.setEnabled(true);
        CHECK(waitFor([&p]() { return p.state() == potoken::State::Failed; }, 5000));
        CHECK(p.reasonText() == QLatin1String(potoken::reasonText(potoken::Reason::InstallFailed)));
        CHECK(recordedArguments(t.file("install-args.txt")) == (QStringList() << QStringLiteral("install") << QStringLiteral("--frozen")));
    }
    {
        Tree t;
        t.flag("fake-exit");
        PoTokenProvider p(t.options());
        p.setEnabled(true);
        CHECK(waitFor([&p]() { return p.state() == potoken::State::Failed; }, 5000));
        const QString reason = p.reasonText();
        CHECK(reason == QLatin1String(potoken::reasonText(potoken::Reason::Exited))
              || reason == QLatin1String(potoken::reasonText(potoken::Reason::NotListening)));
        CHECK(p.extractorArguments().isEmpty());
        p.setEnabled(false);
        CHECK(p.state() == potoken::State::Off);
    }
}

// RES-08 step 4 / RES-14: the service adds the provider arguments only when the provider is
// ready (non-empty source) and the request asked for it; never a cookie option.
static void res14_serviceArguments(const QStringList &common) {
    const QString id = QStringLiteral("aBcDeFgHiJk");
    const QStringList po = QStringList() << QStringLiteral("--plugin-dirs") << QStringLiteral("/data/potoken/yt-dlp-plugins")
        << QStringLiteral("--extractor-args") << QStringLiteral("youtubepot-bgutilhttp:base_url=http://127.0.0.1:40123");
    const QStringList with = YtDlpVideoService::detailsArguments(common, id, po, false);
    const int dash = with.indexOf(QStringLiteral("--"));
    CHECK(dash > 0 && with.indexOf(QStringLiteral("--plugin-dirs")) > 0 && with.indexOf(QStringLiteral("--plugin-dirs")) < dash);
    // --no-plugin-dirs (common) comes first, then only SailTube's directory (RES-08).
    CHECK(with.indexOf(QStringLiteral("--no-plugin-dirs")) >= 0
          && with.indexOf(QStringLiteral("--no-plugin-dirs")) < with.indexOf(QStringLiteral("--plugin-dirs")));
    CHECK(with.contains(QStringLiteral("youtube:skip=dash")));
    const QStringList forced = YtDlpVideoService::detailsArguments(common, id, po, true);
    CHECK(forced.contains(QStringLiteral("youtube:skip=dash;fetch_pot=always")));
    CHECK(!YtDlpVideoService::detailsArguments(common, id, QStringList(), true).join(QLatin1Char(' ')).contains(QLatin1String("fetch_pot")));
    // Anything but the plugin directory and youtubepot-* extractor-args drops them all.
    const QStringList cookie = QStringList(po) << QStringLiteral("--cookies") << QStringLiteral("/tmp/c.txt");
    const QStringList injected = YtDlpVideoService::detailsArguments(common, id, cookie, true);
    CHECK(!injected.contains(QStringLiteral("--cookies")) && !injected.contains(QStringLiteral("--plugin-dirs")));
    CHECK(!injected.join(QLatin1Char(' ')).contains(QLatin1String("fetch_pot")));
    const QStringList relative = QStringList() << QStringLiteral("--plugin-dirs") << QStringLiteral("plugins");
    CHECK(!YtDlpVideoService::detailsArguments(common, id, relative, false).contains(QStringLiteral("--plugin-dirs")));
    // The shared runtime accepts at most 63 arguments (Shared/Runtime/Python/resolver.py).
    CHECK(forced.size() <= 63);
    for (const QString &a : forced) CHECK(!a.contains(QLatin1String("cookie")));
}
#endif

// End to end through the runner: the recorded child arguments of details() runs.
static void res14_serviceRuns() {
    QTemporaryDir dir;
    const QString record = dir.path() + QStringLiteral("/args.txt");
    qputenv("SAILTUBE_TEST_RECORD", QFile::encodeName(record));
    RunnerOptions o;
    o.executablePath = recorder;
    o.pythonChild = false;
    YtDlpRunner runner(o);
    YtDlpVideoService service(&runner);
    int asked = 0;
#ifndef SAILTUBE_STORE
    bool providerReady = false;
    service.setPoTokenArguments([&asked, &providerReady]() {
        ++asked;
        return providerReady ? QStringList() << QStringLiteral("--plugin-dirs") << QStringLiteral("/data/potoken/yt-dlp-plugins")
                                             << QStringLiteral("--extractor-args")
                                             << QStringLiteral("youtubepot-bgutilhttp:base_url=http://127.0.0.1:40123")
                             : QStringList();
    });
#endif
    const auto run = [&](bool requested, bool force) {
        QFile::remove(record);
        bool done = false;
        YtDlpVideoService::DetailsOptions options;
        options.poProvider = requested;
        options.forcePoToken = force;
        service.details(QStringLiteral("aBcDeFgHiJk"), false, options,
                        [&done](ErrorCode, const catalog::VideoDetails &) { done = true; });
        waitFor([&done]() { return done; }, 10000);
        return recordedArguments(record);
    };
    QStringList a = run(true, true);
    CHECK(!a.isEmpty() && !a.contains(QStringLiteral("--plugin-dirs")));   // not ready (or STORE)
    CHECK(!a.join(QLatin1Char(' ')).contains(QLatin1String("fetch_pot")));
#ifndef SAILTUBE_STORE
    CHECK(asked == 1);
    providerReady = true;
    a = run(false, false);
    CHECK(!a.isEmpty() && !a.contains(QStringLiteral("--plugin-dirs")));   // ready, not requested
    CHECK(asked == 1);                                                     // not even asked
    // details(..., false, ...) is the token-free pass: it names its clients (RES-08 step 1).
    a = run(true, false);
    CHECK(a.contains(QStringLiteral("--plugin-dirs")) && a.contains(QStringLiteral("youtube:skip=dash;player_client=android_vr,visionos")));
    a = run(true, true);
    CHECK(a.contains(QStringLiteral("youtube:skip=dash;player_client=android_vr,visionos;fetch_pot=always")));
    // The older two-argument details() never asks.
    bool done = false;
    service.details(QStringLiteral("aBcDeFgHiJk"), false, [&done](ErrorCode, const catalog::VideoDetails &) { done = true; });
    waitFor([&done]() { return done; }, 10000);
    CHECK(!recordedArguments(record).contains(QStringLiteral("--plugin-dirs")));
#else
    CHECK(asked == 0);
#endif
    for (const QString &x : a) CHECK(!x.contains(QLatin1String("cookie")));
}

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    if (argc < 3) { std::printf("usage: %s <fake_deno> <recorder>\n", argv[0]); return 2; }
    fakeDeno = QString::fromLocal8Bit(argv[1]);
    recorder = QString::fromLocal8Bit(argv[2]);
    RunnerOptions o;
    o.executablePath = recorder;
    YtDlpRunner runner(o);
    const QStringList common = runner.commonArguments(true);
    serviceArgs_withoutProvider(common);
    res14_serviceRuns();
#ifndef SAILTUBE_STORE
    res14_serviceArguments(common);
    if (QFileInfo(QStringLiteral("/proc/net/tcp")).exists()) {
        res14_processLoopbackReady();
        res14_processAnyBindRefused();
        res14_processPinRefusals();
        res14_processInstallAndExitFailures();
    } else {
        std::printf("  process checks SKIPPED (no /proc socket tables on this host)\n");
    }
    std::printf("potoken qt (FULL): %d checks, %d failures\n", checks, failures);
#else
    std::printf("potoken qt (STORE): %d checks, %d failures\n", checks, failures);
#endif
    return failures ? 1 : 0;
}
