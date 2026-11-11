// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "WarmResolver.h"

#include "Core/Process/Infrastructure/HelperProcess.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QTimer>
#include <algorithm>

namespace nordtile { namespace video { namespace worker {

struct WarmResolver::Job {
    quint64 id = 0;
    Lane lane = Lane::Latency;
    QStringList arguments;
    int timeoutMs = 0;
    qint64 outputLimit = 0;
    Completion completion;
    QElapsedTimer clock;
    bool sent = false;
    QTimer *timeout = nullptr;
};

struct WarmResolver::LaneState {
    Lane lane = Lane::Latency;
    process::HelperProcess *process = nullptr;
    std::unique_ptr<QTemporaryDir> temp;
    bool ready = false;
    QByteArray buffer;
    QList<quint64> queue;      // waiting, oldest first
    quint64 inflight = 0;
    QElapsedTimer inflightClock;
    QTimer *watchdog = nullptr;
    QTimer *startTimer = nullptr;
    QList<qint64> crashTimes;  // ms since epoch
    bool disabled = false;
    bool stuckReported = false;
    // diagnostics
    int starts = 0, restarts = 0, crashes = 0, stuck = 0, busyFailures = 0;
    qint64 importMs = -1, lastStartMs = -1;
    QString version;
    bool skew = false;
};

static resolver::RunResult failure(bool started, bool timedOut, bool cancelled, const char *diag) {
    resolver::RunResult r;
    r.started = started;
    r.timedOut = timedOut;
    r.cancelled = cancelled;
    r.diagnostics = QByteArray(diag);
    return r;
}

WarmResolver::WarmResolver(const WorkerOptions &options, QObject *parent) : QObject(parent), options_(options) {
    for (int i = 0; i < 2; ++i) {
        lanes_[i].reset(new LaneState);
        LaneState &s = *lanes_[i];
        s.lane = static_cast<Lane>(i);
        s.watchdog = new QTimer(this);
        s.watchdog->setInterval(1000);
        connect(s.watchdog, &QTimer::timeout, this, [this, i]() { checkStuck(*lanes_[i]); });
        s.startTimer = new QTimer(this);
        s.startTimer->setSingleShot(true);
        connect(s.startTimer, &QTimer::timeout, this, [this, i]() {
            LaneState &st = *lanes_[i];
            if (!st.ready && st.process) killWorker(st);   // never became ready: counts as a crash in onFinished
        });
    }
}

WarmResolver::~WarmResolver() {
    for (auto &l : lanes_) {
        if (l->process) {
            l->process->disconnect(this);
            l->process->stopGroup();
            delete l->process;
            l->process = nullptr;
        }
    }
    qDeleteAll(jobs_);   // no completions after destruction
}

bool WarmResolver::available(Lane l) const {
    return !lane(l).disabled && process::HelperProcess::isAbsoluteExecutable(options_.workerProgram);
}

bool WarmResolver::idle(Lane l) const {
    const LaneState &s = lane(l);
    return !s.disabled && s.process && s.ready && !s.inflight && s.queue.isEmpty();
}

QStringList WarmResolver::withCacheDirectory(const QStringList &arguments, const QString &cacheDirectory) {
    if (cacheDirectory.isEmpty()) return arguments;
    QStringList out;
    bool replaced = false;
    for (int i = 0; i < arguments.size(); ++i) {
        const QString &a = arguments.at(i);
        if (a == QLatin1String("--no-cache-dir") || a == QLatin1String("--rm-cache-dir")) {
            if (!replaced) { out << QStringLiteral("--cache-dir") << cacheDirectory; replaced = true; }
            continue;
        }
        if (a == QLatin1String("--cache-dir") && i + 1 < arguments.size()) { ++i; continue; }  // only ours
        out << a;
    }
    if (!replaced) {
        // Insert before the "--" separator or the trailing URL operands' separator, if any.
        const int sep = out.indexOf(QStringLiteral("--"));
        out.insert(sep >= 0 ? sep : 0, cacheDirectory);
        out.insert(sep >= 0 ? sep : 0, QStringLiteral("--cache-dir"));
    }
    return out;
}

void WarmResolver::warmUp(Lane l) {
    LaneState &s = lane(l);
    if (!s.process && !s.disabled) start(s);
}

quint64 WarmResolver::run(Lane l, const QStringList &arguments, int timeoutMs, qint64 outputLimit, Completion completion) {
    Job *job = new Job;
    job->id = nextId_++;
    job->lane = l;
    job->arguments = withCacheDirectory(arguments, options_.cacheDirectory);
    job->timeoutMs = timeoutMs;
    job->outputLimit = outputLimit;
    job->completion = std::move(completion);
    job->clock.start();
    jobs_.insert(job->id, job);
    LaneState &s = lane(l);
    if (s.disabled || arguments.isEmpty()) {
        const quint64 id = job->id;
        QTimer::singleShot(0, this, [this, id]() {
            if (Job *j = jobs_.value(id)) complete(j, failure(false, false, false, "worker-unavailable"));
        });
        return job->id;
    }
    job->timeout = new QTimer(this);
    job->timeout->setSingleShot(true);
    const quint64 id = job->id;
    connect(job->timeout, &QTimer::timeout, this, [this, id]() {
        Job *j = jobs_.value(id);
        if (!j) return;
        LaneState &st = lane(j->lane);
        if (st.inflight == id) {
            st.inflight = 0;
            complete(j, failure(true, true, false, "worker-timeout"));
            killWorker(st);   // its answer would arrive late; a fresh worker serves the next call
        } else {
            st.queue.removeAll(id);
            complete(j, failure(false, true, false, "worker-timeout"));
        }
    });
    job->timeout->start(qMax(1, timeoutMs));
    s.queue.append(job->id);
    if (!s.process) start(s);
    else dispatch(s);
    return job->id;
}

void WarmResolver::cancel(quint64 id) {
    Job *j = jobs_.value(id);
    if (!j) return;
    LaneState &s = lane(j->lane);
    if (s.inflight == id) {
        // The worker cannot abandon a running extraction: restart it, so the lane is free at once.
        s.inflight = 0;
        killWorker(s);
    } else {
        s.queue.removeAll(id);
    }
    QTimer::singleShot(0, this, [this, id]() {
        if (Job *job = jobs_.value(id)) complete(job, failure(false, false, true, "cancelled"));
    });
}

void WarmResolver::start(LaneState &s) {
    if (s.process || s.disabled) return;
    if (!process::HelperProcess::isAbsoluteExecutable(options_.workerProgram)) { s.disabled = true; return; }
    if (!QDir().mkpath(options_.workDirectory)) { s.disabled = true; return; }
    QFile::setPermissions(options_.workDirectory, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
    s.temp.reset(new QTemporaryDir(QDir(options_.workDirectory).filePath(QStringLiteral("tmp-XXXXXX"))));
    if (!s.temp->isValid()) { s.disabled = true; return; }
    QFile::setPermissions(s.temp->path(), QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
    if (!options_.cacheDirectory.isEmpty() && QDir().mkpath(options_.cacheDirectory))
        QFile::setPermissions(options_.cacheDirectory, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
    QProcessEnvironment env = options_.environment;
    for (const QString &k : env.keys())
        if (k.startsWith(QLatin1String("PYTHON")) || k.startsWith(QLatin1String("SAILTUNE_")) || k.startsWith(QLatin1String("NORDTILE_")))
            env.remove(k);
    env.insert(QStringLiteral("NORDTILE_RESOLVER_WORKER"), QStringLiteral("1"));
    env.insert(QStringLiteral("NORDTILE_WORKER_ARCHIVE"), options_.archivePath);
    env.insert(QStringLiteral("NORDTILE_WORKER_DIR"), QDir(options_.workDirectory).canonicalPath());
    env.insert(QStringLiteral("SAILTUNE_RESOLVER_TMPDIR"), QDir(s.temp->path()).canonicalPath());
    s.process = new process::HelperProcess(this);
    s.process->setProcessEnvironment(env);
    s.process->setProcessChannelMode(QProcess::SeparateChannels);
    s.process->setReadChannel(QProcess::StandardOutput);
    LaneState *ps = &s;
    connect(s.process, &QProcess::readyReadStandardOutput, this, [this, ps]() { onReadyRead(*ps); });
    connect(s.process, &QProcess::readyReadStandardError, s.process, [ps]() { ps->process->readAllStandardError(); });
    connect(s.process, static_cast<void (QProcess::*)(int, QProcess::ExitStatus)>(&QProcess::finished), this,
            [this, ps]() { onFinished(*ps); });
    s.ready = false;
    s.buffer.clear();
    ++s.starts;
    if (s.starts > 1) ++s.restarts;
    QElapsedTimer t; t.start();
    if (!s.process->startHelper(options_.workerProgram, options_.programArguments)) {
        delete s.process; s.process = nullptr; s.temp.reset();
        s.disabled = true;
        return;
    }
    s.lastStartMs = t.elapsed();
    s.startTimer->start(options_.startTimeoutMs);
    s.watchdog->start();
}

void WarmResolver::onReadyRead(LaneState &s) {
    if (!s.process) return;
    s.buffer += s.process->readAllStandardOutput();
    int nl;
    while ((nl = s.buffer.indexOf('\n')) >= 0) {
        const QByteArray line = s.buffer.left(nl);
        s.buffer.remove(0, nl + 1);
        handleLine(s, line);
        if (!s.process) return;
    }
    if (s.buffer.size() > 64 * 1024 * 1024) killWorker(s);   // a runaway answer
}

void WarmResolver::handleLine(LaneState &s, const QByteArray &line) {
    const QJsonObject o = QJsonDocument::fromJson(line).object();
    if (!s.ready) {
        if (o.value(QStringLiteral("ready")).toBool()) {
            s.ready = true;
            s.startTimer->stop();
            s.importMs = static_cast<qint64>(o.value(QStringLiteral("importMs")).toDouble(-1));
            s.version = o.value(QStringLiteral("version")).toString();
            dispatch(s);
        } else {
            killWorker(s);
        }
        return;
    }
    const quint64 id = static_cast<quint64>(o.value(QStringLiteral("id")).toDouble(0));
    if (o.value(QStringLiteral("skew")).toBool()) s.skew = true;
    if (id != s.inflight || id == 0) return;   // a late answer of a cancelled or timed-out call
    s.inflight = 0;
    s.stuckReported = false;
    Job *j = jobs_.value(id);
    if (j) {
        resolver::RunResult r;
        r.started = true;
        r.exitCode = o.value(QStringLiteral("code")).toInt(1);
        r.output = o.value(QStringLiteral("out")).toString().toUtf8();
        r.diagnostics = o.value(QStringLiteral("err")).toString().toUtf8();
        if (j->outputLimit > 0 && r.output.size() > j->outputLimit) { r.output.truncate(int(j->outputLimit)); r.overflow = true; }
        if (r.diagnostics.size() > 256 * 1024) r.diagnostics.truncate(256 * 1024);
        r.elapsedMs = j->clock.elapsed();
        complete(j, r);
    }
    dispatch(s);
}

void WarmResolver::dispatch(LaneState &s) {
    if (!s.process || !s.ready || s.inflight || s.queue.isEmpty()) return;
    const quint64 id = s.queue.takeFirst();
    Job *j = jobs_.value(id);
    if (!j) { dispatch(s); return; }
    QJsonObject req;
    req.insert(QStringLiteral("id"), static_cast<double>(id));
    req.insert(QStringLiteral("args"), QJsonArray::fromStringList(j->arguments));
    s.inflight = id;
    s.inflightClock.start();
    s.stuckReported = false;
    j->sent = true;
    s.process->write(QJsonDocument(req).toJson(QJsonDocument::Compact) + '\n');
}

void WarmResolver::checkStuck(LaneState &s) {
    if (!s.inflight || s.stuckReported || s.inflightClock.elapsed() < options_.stuckMs) return;
    s.stuckReported = true;
    ++s.stuck;
    qWarning("sailtube: resolver lane %d busy for %lld ms; %d waiting call(s) fail with busy, retry",
             static_cast<int>(s.lane), static_cast<long long>(s.inflightClock.elapsed()), s.queue.size());
    emit laneStuck(static_cast<int>(s.lane), s.inflightClock.elapsed());
    const QList<quint64> waiting = s.queue;
    s.queue.clear();
    for (quint64 id : waiting) {
        ++s.busyFailures;
        if (Job *j = jobs_.value(id)) complete(j, failure(false, true, false, "worker-busy"));
    }
}

void WarmResolver::killWorker(LaneState &s) {
    if (!s.process) return;
    s.process->stopGroup();   // onFinished runs from the finished signal
}

void WarmResolver::onFinished(LaneState &s) {
    process::HelperProcess *p = s.process;
    if (!p) return;
    s.process = nullptr;
    p->deleteLater();
    s.temp.reset();
    s.watchdog->stop();
    s.startTimer->stop();
    const bool wasReady = s.ready;
    s.ready = false;
    s.buffer.clear();
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (s.inflight || !wasReady) {
        ++s.crashes;
        s.crashTimes.append(now);
    }
    while (!s.crashTimes.isEmpty() && now - s.crashTimes.first() > 60000) s.crashTimes.removeFirst();
    if (s.crashTimes.size() >= options_.maxCrashesPerMinute) s.disabled = true;
    if (s.inflight) {
        const quint64 id = s.inflight;
        s.inflight = 0;
        if (Job *j = jobs_.value(id)) {
            resolver::RunResult r = failure(true, false, false, "worker-crashed");
            r.crashed = true;
            complete(j, r);
        }
    }
    if (s.disabled) {
        const QList<quint64> waiting = s.queue;
        s.queue.clear();
        for (quint64 id : waiting)
            if (Job *j = jobs_.value(id)) complete(j, failure(false, false, false, "worker-unavailable"));
    } else if (!s.queue.isEmpty()) {
        start(s);
    }
}

void WarmResolver::complete(Job *job, const resolver::RunResult &result) {
    jobs_.remove(job->id);
    if (job->timeout) { job->timeout->stop(); job->timeout->deleteLater(); }
    Completion c = std::move(job->completion);
    delete job;
    QTimer::singleShot(0, this, [c, result]() { if (c) c(result); });
}

QVariantMap WarmResolver::diagnostics() const {
    QVariantMap m;
    for (int i = 0; i < 2; ++i) {
        const LaneState &s = *lanes_[i];
        const QString p = i == 0 ? QStringLiteral("latency.") : QStringLiteral("background.");
        m.insert(p + QStringLiteral("starts"), s.starts);
        m.insert(p + QStringLiteral("restarts"), s.restarts);
        m.insert(p + QStringLiteral("crashes"), s.crashes);
        m.insert(p + QStringLiteral("stuck"), s.stuck);
        m.insert(p + QStringLiteral("busyFailures"), s.busyFailures);
        m.insert(p + QStringLiteral("importMs"), s.importMs);
        m.insert(p + QStringLiteral("disabled"), s.disabled);
        m.insert(p + QStringLiteral("versionSkew"), s.skew);
        m.insert(p + QStringLiteral("extractorVersion"), s.version);
    }
    return m;
}

}}}
