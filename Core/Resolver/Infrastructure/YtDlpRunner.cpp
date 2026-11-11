// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "YtDlpRunner.h"
#include "../../../Shared/Runtime/Process/ResolverProcess.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QPointer>
#include <QProcessEnvironment>
#include <QTemporaryDir>
#include <QTimer>

namespace nordtile { namespace video { namespace resolver {

namespace {
const qint64 diagnosticsLimit = 64 * 1024;
}

struct YtDlpRunner::Job {
    quint64 id = 0;
    QStringList arguments;
    int timeoutMs = 60000;
    qint64 outputLimit = 0;
    Completion completion;
    runtime::ResolverProcess *process = nullptr;
    QTemporaryDir *directory = nullptr;
    QTimer *deadline = nullptr;
    QElapsedTimer clock;
    RunResult result;
    bool done = false;
    quint64 warmId = 0;   // in flight on the warm backend
    ExtractorCookies cookies;
    bool cookiesHeld = false;
};

YtDlpRunner::YtDlpRunner(const RunnerOptions &options, QObject *parent)
    : QObject(parent), options_(options) {
    if (options_.maximumConcurrent < 1) options_.maximumConcurrent = 1;
}

YtDlpRunner::~YtDlpRunner() {
    for (Job *job : jobs_) {
        job->done = true;
        releaseCookies(job, nullptr);
        if (job->process) {
            job->process->disconnect(this);
            job->process->stopGroup();
            job->process->waitForFinished(500);
            delete job->process;
        }
        delete job->directory;
        delete job;
    }
}

bool YtDlpRunner::available() const {
    const QFileInfo exe(options_.executablePath);
    return exe.isAbsolute() && exe.isFile() && exe.isExecutable();
}

QStringList YtDlpRunner::commonArguments(bool javascript) const {
    QStringList a;
    a << QStringLiteral("--ignore-config") << QStringLiteral("--no-plugin-dirs")
      << QStringLiteral("--no-remote-components") << QStringLiteral("--no-cache-dir")
      << QStringLiteral("--no-progress") << QStringLiteral("--no-mark-watched")
      << QStringLiteral("--no-warnings")
      << QStringLiteral("--socket-timeout") << QStringLiteral("15")
      << QStringLiteral("--retries") << QStringLiteral("0")
      << QStringLiteral("--extractor-retries") << QStringLiteral("0")
      << QStringLiteral("--no-js-runtimes");
    if (javascript && !options_.javascriptRuntime.isEmpty())
        a << QStringLiteral("--js-runtimes") << options_.javascriptRuntime;
    return a;
}

quint64 YtDlpRunner::run(const QStringList &arguments, int timeoutMs, qint64 outputLimit, Completion completion) {
    Job *job = new Job;
    job->id = nextId_++;
    job->arguments = arguments;
    job->timeoutMs = qBound(1000, timeoutMs, 180000);
    job->outputLimit = qBound(qint64(1024), outputLimit, qint64(16 * 1024 * 1024));
    job->completion = std::move(completion);
    if (cookiesBegin_) {
        job->cookies = cookiesBegin_();
        job->cookiesHeld = true;
        if (!job->cookies.jarPath.isEmpty()) job->arguments = withCookies(job->arguments, job->cookies.jarPath);
    }
    jobs_.insert(job->id, job);
    const int lane = options_.warmLane;
    if (options_.warm && options_.warm->laneAvailable(lane) && !options_.warm->laneIdle(lane)) options_.warm->wakeLane(lane);
    if (options_.warm && options_.warm->laneAvailable(lane) && options_.warm->laneIdle(lane)) {
        QPointer<YtDlpRunner> guard(this);
        const quint64 id = job->id;
        job->warmId = options_.warm->runOnLane(lane, job->arguments, job->timeoutMs, job->outputLimit, [this, guard, id](const RunResult &r) {
            if (!guard) return;
            Job *current = jobs_.value(id, nullptr);
            if (!current || current->done) return;
            current->warmId = 0;
            if (!r.started && !r.cancelled && r.diagnostics == "worker-unavailable") {
                pending_.append(id);   // the lane went away: one-shot child instead
                startPending();
                return;
            }
            current->done = true;
            jobs_.remove(id);
            RunResult result = r;
            releaseCookies(current, &result);
            Completion completion = std::move(current->completion);
            delete current;
            completion(result);   // already asynchronous (WarmResolver delivers from the event loop)
        });
        return job->id;
    }
    pending_.append(job->id);
    QTimer::singleShot(0, this, [this]() { startPending(); });
    return job->id;
}

void YtDlpRunner::cancel(quint64 id) {
    Job *job = jobs_.value(id, nullptr);
    if (!job || job->done) return;
    if (job->warmId) {
        options_.warm->cancelRun(job->warmId);   // completes asynchronously with cancelled
        return;
    }
    if (pending_.removeOne(id)) {
        finish(id, false, true);
        return;
    }
    if (job->process) job->process->stopGroup();
    finish(id, false, true);
}

void YtDlpRunner::startPending() {
    while (running_ < options_.maximumConcurrent && !pending_.isEmpty()) {
        Job *job = jobs_.value(pending_.takeFirst(), nullptr);
        if (job && !job->done) start(job);
    }
}

void YtDlpRunner::start(Job *job) {
    ++running_;
    job->clock.start();
    if (!available()) {
        const quint64 id = job->id;
        QTimer::singleShot(0, this, [this, id]() { finish(id, false, false); });
        return;
    }
    job->directory = new QTemporaryDir(QDir::tempPath() + QStringLiteral("/sailtube-resolver-XXXXXX"));
    const QString temporary = job->directory->isValid()
        ? QFileInfo(job->directory->path()).canonicalFilePath() : QString();
    job->process = new runtime::ResolverProcess(this);
    job->deadline = new QTimer(job->process);
    job->deadline->setSingleShot(true);
    const quint64 id = job->id;
    connect(job->deadline, &QTimer::timeout, this, [this, id]() {
        Job *current = jobs_.value(id, nullptr);
        if (!current || current->done) return;
        current->process->stopGroup();
        finish(id, true, false);
    });
    connect(job->process, &QProcess::readyReadStandardOutput, this, [this, id]() {
        if (Job *current = jobs_.value(id, nullptr)) read(current);
    });
    connect(job->process, &QProcess::readyReadStandardError, this, [this, id]() {
        if (Job *current = jobs_.value(id, nullptr)) read(current);
    });
    connect(job->process, static_cast<void (QProcess::*)(int, QProcess::ExitStatus)>(&QProcess::finished),
            this, [this, id](int code, QProcess::ExitStatus status) {
        Job *current = jobs_.value(id, nullptr);
        if (!current || current->done) return;
        read(current);
        current->result.exitCode = code;
        current->result.crashed = status == QProcess::CrashExit;
        finish(id, false, false);
    });
    connect(job->process, static_cast<void (QProcess::*)(QProcess::ProcessError)>(&QProcess::error),
            this, [this, id](QProcess::ProcessError error) {
        Job *current = jobs_.value(id, nullptr);
        if (current && !current->done && error == QProcess::FailedToStart) finish(id, false, false);
    });
    connect(job->process, &QProcess::started, this, [this, id]() {
        if (Job *current = jobs_.value(id, nullptr)) current->result.started = true;
    });

    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    for (const QString &key : environment.keys())
        if (key.startsWith(QLatin1String("SAILTUNE_")) || key.startsWith(QLatin1String("PYTHON")))
            environment.remove(key);
    environment.remove(QStringLiteral("TMP"));
    environment.remove(QStringLiteral("TEMP"));
    if (!temporary.isEmpty()) {
        environment.insert(QStringLiteral("TMPDIR"), temporary);
        // Protocol name of the shared runtime (Shared/Runtime/Python/resolver.py).
        environment.insert(QStringLiteral("SAILTUNE_RESOLVER_TMPDIR"), temporary);
    }
    if (options_.pythonChild) environment.insert(QStringLiteral("SAILTUNE_RESOLVER_CHILD"), QStringLiteral("1"));
    job->process->setProcessEnvironment(environment);
    job->deadline->start(job->timeoutMs);
    job->process->start(options_.executablePath, job->arguments, QIODevice::ReadOnly);
}

void YtDlpRunner::read(Job *job) {
    if (!job->process || job->done) return;
    job->process->setReadChannel(QProcess::StandardOutput);
    job->result.output += job->process->read(job->outputLimit - job->result.output.size() + 1);
    job->process->setReadChannel(QProcess::StandardError);
    job->result.diagnostics += job->process->read(diagnosticsLimit - job->result.diagnostics.size() + 1);
    if (job->result.output.size() > job->outputLimit || job->result.diagnostics.size() > diagnosticsLimit) {
        job->result.overflow = true;
        job->process->stopGroup();
        finish(job->id, false, false);
    }
}

void YtDlpRunner::finish(quint64 id, bool timedOut, bool cancelled) {
    Job *job = jobs_.value(id, nullptr);
    if (!job || job->done) return;
    job->done = true;
    job->result.timedOut = timedOut;
    job->result.cancelled = cancelled;
    job->result.elapsedMs = job->clock.isValid() ? job->clock.elapsed() : 0;
    const bool wasRunning = job->clock.isValid();
    RunResult result = job->result;
    releaseCookies(job, &result);
    Completion completion = std::move(job->completion);
    QPointer<YtDlpRunner> guard(this);
    QTimer::singleShot(0, this, [this, guard, id, wasRunning, result, completion]() {
        if (!guard) return;
        Job *current = jobs_.take(id);
        if (current) {
            if (current->process) {
                current->process->disconnect(this);
                if (current->process->state() != QProcess::NotRunning) {
                    current->process->stopGroup();
                    current->process->waitForFinished(300);
                }
                current->process->deleteLater();
            }
            // The child group has been stopped; its private directory goes with it.
            delete current->directory;
            delete current;
        }
        if (wasRunning) --running_;
        if (completion) completion(result);
        startPending();
    });
}

void YtDlpRunner::setCookieHooks(std::function<ExtractorCookies()> begin, std::function<void(const ExtractorCookies &)> end) {
    cookiesBegin_ = std::move(begin);
    cookiesEnd_ = std::move(end);
}

QStringList YtDlpRunner::withCookies(const QStringList &arguments, const QString &jarPath) {
    if (jarPath.isEmpty()) return arguments;
    QStringList out = arguments;
    const int sep = out.indexOf(QStringLiteral("--"));
    const int at = sep >= 0 ? sep : 0;
    out.insert(at, jarPath);
    out.insert(at, QStringLiteral("--cookies"));
    return out;
}

void YtDlpRunner::dropCookieHooks() {
    for (Job *job : jobs_) releaseCookies(job, nullptr);
    cookiesBegin_ = nullptr;
    cookiesEnd_ = nullptr;
}

void YtDlpRunner::releaseCookies(Job *job, RunResult *result) {
    if (!job->cookiesHeld) return;
    job->cookiesHeld = false;
    const QByteArray jar = job->cookies.jarPath.toUtf8();
    if (result && !jar.isEmpty()) {
        result->output.replace(jar, QByteArrayLiteral("[cookies]"));
        result->diagnostics.replace(jar, QByteArrayLiteral("[cookies]"));
    }
    if (cookiesEnd_) cookiesEnd_(job->cookies);
}

}}}
