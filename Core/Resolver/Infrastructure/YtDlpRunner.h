// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

// Bounded process boundary to the packaged upstream yt-dlp (shared SailTune
// runtime: PyOtherSide child + private QuickJS, see docs/SHARING.md). No shell,
// no PATH lookup, no config files, cookies, plugins or automatic downloads.

#include <QByteArray>
#include <QHash>
#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>
#include <functional>

class QTemporaryDir;
class QTimer;

namespace nordtile { namespace runtime { class ResolverProcess; } }

namespace nordtile { namespace video { namespace resolver {

class WarmBackend;

struct RunnerOptions {
    // The application binary in resolver-child mode (Python archive profile),
    // or an independently installed yt-dlp executable (development).
    QString executablePath;
    bool pythonChild = true;
    // JavaScript runtime for the full extraction profile ("quickjs:<path>").
    QString javascriptRuntime;
    int maximumConcurrent = 2;
    // Optional warm backend (core: Core/Worker/Infrastructure/WarmResolver implements it): while its lane
    // is idle, runs go to that long-lived worker instead of one child per call; when it is busy, starting or
    // unavailable, the one-shot child runs as before. Not owned; must outlive the runner.
    WarmBackend *warm = nullptr;
    int warmLane = 0;   // 0 latency (play, details), 1 background (search, downloads)
};

struct RunResult {
    int exitCode = -1;
    bool started = false;
    bool crashed = false;
    bool timedOut = false;
    bool cancelled = false;
    bool overflow = false;
    QByteArray output;      // stdout, bounded
    QByteArray diagnostics; // stderr, bounded; never shown or logged verbatim
    qint64 elapsedMs = 0;
};

// The warm path as the runner sees it (implemented by worker::WarmResolver); same contract as run().
class WarmBackend {
public:
    using Completion = std::function<void(const RunResult &)>;
    virtual ~WarmBackend() = default;
    virtual bool laneAvailable(int lane) const = 0;
    // Ready and not serving a call: a call sent now starts at once. Otherwise the runner uses a one-shot
    // child (the old parallelism), so a play tap never queues behind a search on the same lane.
    virtual bool laneIdle(int lane) const = 0;
    virtual void wakeLane(int lane) = 0;
    virtual quint64 runOnLane(int lane, const QStringList &arguments, int timeoutMs, qint64 outputLimit, Completion) = 0;
    virtual void cancelRun(quint64 id) = 0;
};

// ACC-11 / RES-08 cookie rung: a per-call cookie jar from the account (empty path = signed-in features off).
// begin() runs before each yt-dlp run, end() exactly once afterwards (success, failure, timeout, cancel,
// fallback or runner destruction), so the account folds rotations back and deletes the jar. The jar path is
// passed as --cookies only; it is removed from anything the run returns and never logged.
struct ExtractorCookies {
    QString jarPath;
    quint64 generation = 0;
};

class YtDlpRunner final : public QObject {
    Q_OBJECT
public:
    using Completion = std::function<void(const RunResult &)>;
    explicit YtDlpRunner(const RunnerOptions &, QObject *parent = nullptr);
    ~YtDlpRunner() override;

    bool available() const;
    // Arguments that every run shares; javascript selects the full profile.
    QStringList commonArguments(bool javascript) const;
    // Completion is always asynchronous and delivered exactly once, including
    // cancellation, except when the runner is destroyed first.
    quint64 run(const QStringList &arguments, int timeoutMs, qint64 outputLimit, Completion);
    void cancel(quint64 id);
    void setCookieHooks(std::function<ExtractorCookies()> begin, std::function<void(const ExtractorCookies &)> end);
    // Releases every jar still held (end() once each) and clears the hooks; call while the account
    // service is alive (at shutdown, before it is destroyed).
    void dropCookieHooks();
    // "--cookies <jar>" before the "--" operand separator (or first when there is none).
    static QStringList withCookies(const QStringList &arguments, const QString &jarPath);

private:
    struct Job;
    void startPending();
    void start(Job *);
    void finish(quint64 id, bool timedOut, bool cancelled);
    void read(Job *);
    void releaseCookies(Job *job, RunResult *result);
    RunnerOptions options_;
    std::function<ExtractorCookies()> cookiesBegin_;
    std::function<void(const ExtractorCookies &)> cookiesEnd_;
    quint64 nextId_ = 1;
    QHash<quint64, Job *> jobs_;
    QList<quint64> pending_;
    int running_ = 0;
};

}}}
