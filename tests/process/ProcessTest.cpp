// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// PLT-14 host tests (Linux): (1) a helper started on behalf of a short-lived thread is alive 10 s
// after that thread exits; (2) after SIGKILL of the app process the helper exits within 2 s;
// (3) with PATH=/usr/bin:/bin a helper given by absolute path starts, a bare name is refused;
// (4) with QT_MESSAGE_PATTERN set, debug lines follow it.
#include "../../Core/Process/Infrastructure/HelperProcess.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QProcess>
#include <QTemporaryDir>
#include <QThread>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <csignal>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

using namespace nordtile::process;
static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++failures; } } while (0)

static QString sleepBinary() {
    for (const char *p : {"/bin/sleep", "/usr/bin/sleep"}) if (QFile::exists(QString::fromLatin1(p))) return QString::fromLatin1(p);
    return QString();
}

// Alive and not a zombie.
static bool alive(qint64 pid) {
    if (::kill(static_cast<pid_t>(pid), 0) != 0) return false;
    QFile stat(QStringLiteral("/proc/%1/stat").arg(pid));
    if (!stat.open(QIODevice::ReadOnly)) return false;
    const QByteArray s = stat.readAll();
    const int close = s.lastIndexOf(')');
    return close > 0 && close + 2 < s.size() && s.at(close + 2) != 'Z';
}

static int appMode(const char *pidFile) {
    // The "app": starts a helper from a short-lived worker thread, reports its pid, then idles.
    HelperHost host;
    qint64 pid = -1;
    std::thread t([&]() { HelperSpec spec; spec.helperProgram = sleepBinary(); spec.arguments << QStringLiteral("60"); pid = host.start(spec); });
    t.join();
    QFile f(QString::fromLocal8Bit(pidFile));
    if (pid <= 0 || !f.open(QIODevice::WriteOnly)) return 3;
    f.write(QByteArray::number(pid));
    f.close();
    for (;;) ::pause();
}

static int logMode() {
    qDebug("hello from the log check");
    return 0;
}

int main(int argc, char **argv) {
    if (argc == 3 && std::strcmp(argv[1], "app") == 0) { QCoreApplication app(argc, argv); return appMode(argv[2]); }
    if (argc == 2 && std::strcmp(argv[1], "log") == 0) { QCoreApplication app(argc, argv); return logMode(); }
    QCoreApplication app(argc, argv);
    // Orphaned helpers are re-parented to this test, so it can reap them and see them exit.
    CHECK(::prctl(PR_SET_CHILD_SUBREAPER, 1) == 0);
    CHECK(!sleepBinary().isEmpty());

    {   // (1) short-lived calling thread
        HelperHost host;
        qint64 pid = -1;
        std::thread t([&]() { HelperSpec spec; spec.helperProgram = sleepBinary(); spec.arguments << QStringLiteral("30"); pid = host.start(spec); });
        t.join();
        CHECK(pid > 0);
        QThread::sleep(10);
        CHECK(alive(pid));
        CHECK(host.running() == 1);
        CHECK(host.stop(pid));
        CHECK(!alive(pid));
        std::printf("process: (1) helper alive 10 s after its calling thread exited\n");
    }
    {   // (2) SIGKILL of the app process
        QTemporaryDir tmp;
        const QByteArray pidFile = QFile::encodeName(tmp.path() + QStringLiteral("/helper.pid"));
        const pid_t appPid = ::fork();
        if (appPid == 0) { ::execl(argv[0], argv[0], "app", pidFile.constData(), static_cast<char *>(nullptr)); ::_exit(127); }
        qint64 helper = -1;
        for (int i = 0; i < 100 && helper <= 0; ++i) {
            QThread::msleep(50);
            QFile f(QString::fromLocal8Bit(pidFile));
            if (f.open(QIODevice::ReadOnly)) helper = f.readAll().toLongLong();
        }
        CHECK(helper > 0);
        CHECK(helper > 0 && alive(helper));
        ::kill(appPid, SIGKILL);
        int status = 0;
        ::waitpid(appPid, &status, 0);
        bool gone = false;
        for (int i = 0; i < 40 && !gone; ++i) {   // 2 s
            if (helper > 0 && ::waitpid(static_cast<pid_t>(helper), &status, WNOHANG) == helper) gone = true;
            else if (helper > 0 && !alive(helper)) gone = true;
            else QThread::msleep(50);
        }
        CHECK(gone);
        if (!gone && helper > 0) ::kill(static_cast<pid_t>(helper), SIGKILL);
        std::printf("process: (2) helper gone within 2 s of the app's SIGKILL: %s\n", gone ? "yes" : "NO");
    }
    {   // (3) trimmed PATH
        QTemporaryDir tmp;
        const QString script = tmp.path() + QStringLiteral("/sailtube-helper");
        QFile f(script);
        CHECK(f.open(QIODevice::WriteOnly));
        f.write("#!/bin/sh\nexit 0\n");
        f.close();
        f.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
        ::setenv("PATH", "/usr/bin:/bin", 1);
        HelperHost host;
        HelperSpec spec;
        spec.environment = QProcessEnvironment::systemEnvironment();
        spec.helperProgram = script;
        CHECK(host.start(spec) > 0);
        spec.helperProgram = QStringLiteral("sailtube-helper");   // bare name: never looked up
        CHECK(host.start(spec) == -1);
        spec.helperProgram = QStringLiteral("sleep");
        CHECK(host.start(spec) == -1);
        std::printf("process: (3) absolute path starts under PATH=/usr/bin:/bin, bare names refused\n");
    }
    {   // HelperProcess and HelperOwner directly (owner = this thread, which outlives them)
        CHECK(HelperProcess::isAbsoluteExecutable(sleepBinary()));
        CHECK(!HelperProcess::isAbsoluteExecutable(QStringLiteral("sleep")));
        CHECK(!HelperProcess::isAbsoluteExecutable(QStringLiteral("/nonexistent/sleep")));
        HelperProcess p;
        CHECK(!p.startHelper(QStringLiteral("sleep"), QStringList() << QStringLiteral("5")));
        CHECK(p.startHelper(sleepBinary(), QStringList() << QStringLiteral("30")));
        const qint64 pid = p.processId();
        CHECK(::getpgid(static_cast<pid_t>(pid)) == static_cast<pid_t>(pid));   // own process group
        p.stopGroup();
        CHECK(!alive(pid));
        HelperOwner owner;
        HelperSpec spec; spec.helperProgram = sleepBinary(); spec.arguments << QStringLiteral("30");
        const qint64 a = owner.startHelper(spec), b = owner.startHelper(spec);
        CHECK(a > 0 && b > 0 && owner.running() == 2);
        CHECK(owner.stopHelper(a));
        CHECK(!owner.stopHelper(a));
        owner.stopAll();
        CHECK(owner.running() == 0 && !alive(b));
    }
    {   // (4) QT_MESSAGE_PATTERN
        QProcess p;
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        env.insert(QStringLiteral("QT_MESSAGE_PATTERN"), QStringLiteral("SAILTUBE-PATTERN[%{type}] %{message}"));
        // Without a terminal, Qt on Sailfish sends messages to the journal; force the console for the check.
        env.insert(QStringLiteral("QT_LOGGING_TO_CONSOLE"), QStringLiteral("1"));
        p.setProcessEnvironment(env);
        p.start(QString::fromLocal8Bit(argv[0]), QStringList() << QStringLiteral("log"));
        CHECK(p.waitForFinished(10000));
        const QByteArray err = p.readAllStandardError();
        CHECK(err.contains("SAILTUBE-PATTERN[debug] hello from the log check"));
        std::printf("process: (4) debug lines follow QT_MESSAGE_PATTERN\n");
    }
    if (failures) { std::printf("process: %d failure(s)\n", failures); return 1; }
    std::printf("process ProcessTest: all checks passed\n");
    return 0;
}
