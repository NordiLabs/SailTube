// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

// Warm resolver (RES-01, CORE-03, NFR-ROB-03/04, PLT-14): long-lived workers that keep the packaged
// yt-dlp imported (Core/Worker/Runtime/worker.py) and run one yt-dlp command line per call.
//
// - Two lanes, each its own worker process: Latency (play, details) and Background (search, browse,
//   downloads), so a play tap never waits behind a browse call.
// - Same call shape as resolver::YtDlpRunner: run(arguments, timeoutMs, outputLimit, completion) with a
//   resolver::RunResult; completions are always asynchronous, exactly once (cancel included), never after
//   destruction.
// - Player-JS cache: "--no-cache-dir" in the arguments is replaced by "--cache-dir <cacheDirectory>"
//   (private, 0700), so challenge solutions survive between calls and app starts.
// - Watchdog: a lane whose call has run longer than stuckMs is logged, and its queued calls fail at once
//   with "busy, retry" (timedOut, diagnostics "worker-busy") instead of waiting; there is never more than one
//   process per lane, so nothing is double-spawned. A call past its own timeout kills the worker group and
//   the next call starts a fresh one.
// - A worker that dies three times within a minute disables its lane: available(lane) turns false and the
//   caller falls back to the one-shot child.
// - Children are HelperProcess (own process group, parent-death signal, absolute program path).

#include "Core/Resolver/Infrastructure/YtDlpRunner.h"

#include <QByteArray>
#include <QElapsedTimer>
#include <QHash>
#include <QList>
#include <QObject>
#include <QProcessEnvironment>
#include <QString>
#include <QStringList>
#include <QVariantMap>
#include <functional>
#include <memory>

class QTemporaryDir;
class QTimer;

namespace nordtile { namespace process { class HelperProcess; } }

namespace nordtile { namespace video { namespace worker {

enum class Lane { Latency = 0, Background = 1 };

struct WorkerOptions {
    QString workerProgram;           // absolute: the app binary in worker mode, or python3 in tests
    QStringList programArguments;    // e.g. {"-I", ".../worker.py"} for python3; empty for the app binary
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    QString archivePath;             // the installed yt-dlp.pyz (copied by the worker, NFR-ROB-04)
    QString workDirectory;           // private 0700 directory: archive copies and per-worker temp dirs
    QString cacheDirectory;          // yt-dlp --cache-dir; empty keeps the caller's cache arguments
    int startTimeoutMs = 20000;
    int stuckMs = 30000;
    int maxCrashesPerMinute = 3;
};

class WarmResolver final : public QObject, public resolver::WarmBackend {
    Q_OBJECT
public:
    using Completion = std::function<void(const resolver::RunResult &)>;
    explicit WarmResolver(const WorkerOptions &options, QObject *parent = nullptr);
    ~WarmResolver() override;

    bool available(Lane lane) const;
    // The worker is ready and serving nothing (a call would start at once).
    bool idle(Lane lane) const;
    // Starts the lane's worker ahead of the first call (e.g. once the first page is up).
    void warmUp(Lane lane);
    quint64 run(Lane lane, const QStringList &arguments, int timeoutMs, qint64 outputLimit, Completion completion);
    void cancel(quint64 id);
    // Fixed keys and numbers only: starts, restarts, crashes, stuck, busy failures, import ms, version, skew.
    QVariantMap diagnostics() const;
    static QStringList withCacheDirectory(const QStringList &arguments, const QString &cacheDirectory);

    // resolver::WarmBackend (YtDlpRunner's view; lane 0 latency, 1 background).
    bool laneAvailable(int l) const override { return l == 0 || l == 1 ? available(static_cast<Lane>(l)) : false; }
    quint64 runOnLane(int l, const QStringList &arguments, int timeoutMs, qint64 outputLimit, Completion c) override {
        return run(l == 1 ? Lane::Background : Lane::Latency, arguments, timeoutMs, outputLimit, std::move(c));
    }
    void cancelRun(quint64 id) override { cancel(id); }
    bool laneIdle(int l) const override { return (l == 0 || l == 1) && idle(static_cast<Lane>(l)); }
    void wakeLane(int l) override { if (l == 0 || l == 1) warmUp(static_cast<Lane>(l)); }

signals:
    void laneStuck(int lane, qint64 runningMs);

private:
    struct Job;
    struct LaneState;
    LaneState &lane(Lane l) { return *lanes_[static_cast<int>(l)]; }
    const LaneState &lane(Lane l) const { return *lanes_[static_cast<int>(l)]; }
    void start(LaneState &state);
    void onReadyRead(LaneState &state);
    void onFinished(LaneState &state);
    void dispatch(LaneState &state);
    void handleLine(LaneState &state, const QByteArray &line);
    void complete(Job *job, const resolver::RunResult &result);
    void killWorker(LaneState &state);
    void checkStuck(LaneState &state);

    WorkerOptions options_;
    std::unique_ptr<LaneState> lanes_[2];
    QHash<quint64, Job *> jobs_;
    quint64 nextId_ = 1;
};

}}}
