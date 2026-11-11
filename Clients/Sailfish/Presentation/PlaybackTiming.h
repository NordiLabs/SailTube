// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

// NFR-PERF-02..06 / DIA-03 timing markers, exact grammar in
// tools/experiments/perf_timing.md:
//   sailtube-timing: <event> t=<ms> [key=value ...]
// t is milliseconds from one monotonic clock (QElapsedTimer) started by the
// first call (main() marks "launch" first). Printed through qInfo (stderr, and
// the journal on the device) only while the timing log is on: the DIA-03
// setting, or SAILTUBE_TIMING=1 in the environment. Values never carry URLs,
// tokens or cookies.

#include <QElapsedTimer>
#include <QObject>
#include <QSharedPointer>
#include <QString>
#include <QtGlobal>
#include <functional>

namespace nordtile { namespace video { namespace sailfish {

inline QElapsedTimer &timingClock() {
    static QElapsedTimer clock;
    if (!clock.isValid()) clock.start();
    return clock;
}

inline bool &timingEnabledFlag() {
    static bool on = qEnvironmentVariableIsSet("SAILTUBE_TIMING");
    return on;
}

inline void setTimingEnabled(bool on) { timingEnabledFlag() = on || qEnvironmentVariableIsSet("SAILTUBE_TIMING"); }
inline bool timingEnabled() { return timingEnabledFlag(); }

// keys: "run=3 net=wifi" (space separated, no spaces inside values).
inline void timingMark(const char *event, const QString &keys = QString()) {
    const qint64 t = timingClock().elapsed();
    if (!timingEnabled()) return;
    if (keys.isEmpty()) qInfo("sailtube-timing: %s t=%lld", event, static_cast<long long>(t));
    else qInfo("sailtube-timing: %s t=%lld %s", event, static_cast<long long>(t), qPrintable(keys));
}

// The same with a time taken earlier on this clock (e.g. a ThreadedStreamSource hook's atMs:
// warmup-start, warmup-done).
inline void timingMarkAt(const char *event, qint64 t, const QString &keys = QString()) {
    if (!timingEnabled()) return;
    if (keys.isEmpty()) qInfo("sailtube-timing: %s t=%lld", event, static_cast<long long>(t));
    else qInfo("sailtube-timing: %s t=%lld %s", event, static_cast<long long>(t), qPrintable(keys));
}

// The launch clock in milliseconds, for ThreadedStreamSource::Options::monotonicMs (thread-safe
// once main() has started it).
inline qint64 timingNowMs() { return timingClock().elapsed(); }

// RES-21 warm-up hook: runs `action` once, queued to `context`'s thread, after the first emission
// of `signal` (QQuickWindow::frameSwapped: the first screen is up). Never before it, never inside
// the emitting call; later emissions do nothing.
template <typename Sender, typename Signal>
inline void runOnceAfterFirst(Sender *sender, Signal signal, QObject *context, const std::function<void()> &action) {
    QSharedPointer<QMetaObject::Connection> connection(new QMetaObject::Connection);
    QSharedPointer<bool> done(new bool(false));
    *connection = QObject::connect(sender, signal, context, [connection, done, action]() {
        QObject::disconnect(*connection);
        if (*done) return;  // emissions already queued before the disconnect
        *done = true;
        action();
    }, Qt::QueuedConnection);
}

}}}
