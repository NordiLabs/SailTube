// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <QElapsedTimer>
#include <QHash>
#include <QObject>
#include <QSet>
#include <QStringList>

namespace nordtile { namespace video { namespace sailfish {

// NFR-PERF-01/07: the start-up path and the tap-to-picture path, logged as
// "sailtube-timing startup <stage> <ms since main()>" and "sailtube-timing <name> <ms>"
// so they can be read from the journal on a device. Each stage logs once.
class StartupTiming final : public QObject {
    Q_OBJECT
public:
    // `clock` is the timer main() started first; it is copied.
    explicit StartupTiming(const QElapsedTimer &clock, QObject *parent = nullptr);

    // Logs a start-up stage once; later calls with the same stage are ignored.
    Q_INVOKABLE void mark(const QString &stage);
    // Starts (or restarts) an interval such as "video" when the user taps a video.
    Q_INVOKABLE void begin(const QString &name);
    // Logs "sailtube-timing <name> <stage> <ms>" once per begin(name); no-op without one.
    Q_INVOKABLE void end(const QString &name, const QString &stage);
    Q_INVOKABLE bool running(const QString &name) const { return intervals_.contains(name); }
    // True once mark(stage) has logged (QML defers non-critical work until "first-frame").
    Q_INVOKABLE bool hasMark(const QString &stage) const { return marked_.contains(stage); }
    Q_INVOKABLE qint64 elapsed() const { return clock_.elapsed(); }

    // Time before main() (dynamic loading, static initialisers), from the kernel's process start
    // time (/proc/self/stat field 22 against /proc/uptime). 0 where that is not available.
    static qint64 processAgeMs();
    // Logs "sailtube-timing startup pre-main <ms>" once. Stage lines keep "ms since main()", so
    // journals stay comparable; ms since process start = pre-main + stage ms.
    void setPreMainMs(qint64 ms);
    qint64 preMainMs() const { return preMainMs_; }
    // Milliseconds since the process started (pre-main + elapsed).
    Q_INVOKABLE qint64 sinceProcessStart() const { return preMainMs_ + clock_.elapsed(); }
    // Quits the application (queued QCoreApplication::exit(0)) right after `stage` is marked, or
    // at once when it was marked already. For trace runs (SAILTUBE_STARTUP_TRACE=exit).
    void exitAfter(const QString &stage);

    // Lines logged so far, oldest first (tests).
    QStringList lines() const { return lines_; }

signals:
    // Emitted once per stage, right after it is logged.
    void marked(const QString &stage);

private:
    void log(const QString &line);
    QElapsedTimer clock_;
    QSet<QString> marked_;
    QHash<QString, qint64> intervals_;
    QHash<QString, QSet<QString>> ended_;
    QStringList lines_;
    qint64 preMainMs_ = 0;
    QString exitStage_;
};

}}}
